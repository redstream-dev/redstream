/* Redstream: rdsanalytics, video analytics on the objects of rdsstreammux batches (ROI filtering, overcrowding,
 * direction and line crossing). Property names and config files follow DeepStream's public nvdsanalytics
 * documentation.
 *
 * An in-place GstBaseTransform in passthrough mode: buffers go downstream unchanged, with the results added to their
 * RdsBatchMeta as user metas (rds-analytics-meta.h) and display metas. Pixels are never touched: an OSD downstream
 * draws the overlays. The analytics themselves are the
 * GLib-only core in rds-analytics-core; this file feeds it and writes its results.
 * - start (READY -> PAUSED) loads config-file, which is required even with enable=FALSE (DeepStream 8.0 behaviour;
 *   8.0 fails the state change without posting an ERROR, we post one: ours). The core is made on the first batch,
 *   for the batch surface's size (the mux output), and made again whenever that size changes: every stream's state
 *   then resets (ours; it is logged).
 * - config-file set while started (DeepStream 8.0 behaviour: settable in any state) is loaded and validated at once,
 *   on the caller's thread, and applies from the next buffer with all state reset (DeepStream 8.0 behaviour). A file
 *   that fails posts an ERROR and the old config stays, with its state and the property's old value (ours).
 * - enable=FALSE (NULL/READY only) and [property] enable=0: pass-through, no meta at all (DeepStream 8.0 behaviour).
 *   unique-id is read per batch.
 * - Per batch, under the core lock and the batch meta lock: each frame meta in turn (partial batches and repeated
 *   streams included) goes through the core with its pad_index and buf_pts; then the frame gets its analytics frame
 *   meta, and each object with something to report its object info (DeepStream 8.0 behaviour); then the osd-mode
 *   overlays as display meta, and in osd-mode 2 the objects' status text and border (rds-analytics-draw).
 * - Stream events (ours: 8.0 ignores them): rds-stream-reset resets the stream's state, rds-pad-deleted frees it
 *   (keeping its cumulative line-crossing counts). Every event still goes downstream.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsanalytics.h"

#include "rds-analytics-config.h"
#include "rds-analytics-core.h"
#include "rds-analytics-draw.h"

#include <rds/rds.h>
#include <rds/rds-analytics-meta.h>

GST_DEBUG_CATEGORY (gst_rds_analytics_debug);
#define GST_CAT_DEFAULT gst_rds_analytics_debug

/* The core's sentinels are GStreamer's and the metas': it is GLib-only, so it can't use their names. */
G_STATIC_ASSERT (RDS_ANALYTICS_PTS_NONE == GST_CLOCK_TIME_NONE);
G_STATIC_ASSERT (RDS_ANALYTICS_UNTRACKED == RDS_UNTRACKED_OBJECT_ID);

/* Property defaults, as DeepStream 8.0's nvdsanalytics reports them. */
#define DEFAULT_ENABLE TRUE
#define DEFAULT_UNIQUE_ID 17

enum
{
  PROP_0,
  PROP_CONFIG_FILE,
  PROP_ENABLE,
  PROP_UNIQUE_ID,
};

struct _GstRdsAnalytics
{
  GstBaseTransform parent;

  /* properties (object lock) */
  gboolean enable;
  guint unique_id;

  /* core_lock: config-file, the started flag and the configs together, so a config-file set can't race start or
   * stop (see gst_rds_analytics_start); and the streaming state */
  GMutex core_lock;
  gchar *config_file;
  gboolean started;             /* between start and stop: config-file loads at once */
  RdsAnalyticsConfig *cfg;      /* the config in use */
  RdsAnalyticsConfig *pending;  /* loaded while started, swapped in on the next buffer */
  RdsAnalyticsCore *core;       /* NULL until the first batch, and after a swap */
  guint out_w, out_h;           /* the size the core's rules are scaled to */
  /* per-frame scratch */
  GArray *objs;                 /* RdsAnalyticsObject */
  GPtrArray *obj_metas;         /* RdsObjectMeta *, the same order */
};

/* Any format and size: the element reads the metadata only (DeepStream 8.0 takes NV12 and RGBA). */
#define RDS_ANALYTICS_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), width = (int) [ 1, 2147483647 ], " \
  "height = (int) [ 1, 2147483647 ], framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_ANALYTICS_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_ANALYTICS_CAPS));

G_DEFINE_TYPE (GstRdsAnalytics, gst_rds_analytics, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdsanalytics, "rdsanalytics", GST_RANK_NONE, GST_TYPE_RDS_ANALYTICS);

/* ---- config ---- */

/* Loads @path, posting an ERROR naming the file (and the key) on failure. */
static RdsAnalyticsConfig *
load_config (GstRdsAnalytics * self, const gchar * path)
{
  RdsAnalyticsConfig *cfg;
  GError *err = NULL;

  if (!path || !*path) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("element property config-file: not set: rdsanalytics needs a "
            "config file"), (NULL));
    return NULL;
  }
  if (!(cfg = rds_analytics_config_load (path, &err))) {
    if (err->domain == RDS_ANALYTICS_CONFIG_ERROR && err->code == RDS_ANALYTICS_CONFIG_ERROR_FILE)
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("%s", err->message), (NULL));
    else
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("%s", err->message), (NULL));
    g_error_free (err);
    return NULL;
  }
  GST_INFO_OBJECT (self, "config %s: enable %d, %ux%u, %u streams with groups", path, cfg->enable,
      cfg->config_width, cfg->config_height, cfg->streams->len);
  return cfg;
}

/* config-file set: stored when stopped (start loads it); while started, loaded and validated now and swapped in on
 * the next buffer. The load runs without the lock (posting an ERROR takes the object lock, and a batch may hold
 * core_lock); the path and the pending config are then published together under core_lock. If the element stopped
 * meanwhile, only the path is kept: the next start loads it, and nothing stale is left pending. */
static void
set_config_file (GstRdsAnalytics * self, const gchar * path)
{
  RdsAnalyticsConfig *cfg;
  gboolean started;

  g_mutex_lock (&self->core_lock);
  started = self->started;
  if (!started) {
    g_free (self->config_file);
    self->config_file = g_strdup (path);
  }
  g_mutex_unlock (&self->core_lock);
  if (!started)
    return;

  if (!(cfg = load_config (self, path)))
    return;                     /* the old config and property value stay (ours) */
  g_mutex_lock (&self->core_lock);
  g_free (self->config_file);
  self->config_file = g_strdup (path);
  if (self->started) {
    g_clear_pointer (&self->pending, rds_analytics_config_free);
    self->pending = g_steal_pointer (&cfg);
  }
  g_mutex_unlock (&self->core_lock);
  g_clear_pointer (&cfg, rds_analytics_config_free);
}

/* ---- start / stop ---- */

static gboolean
gst_rds_analytics_start (GstBaseTransform * trans)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (trans);
  /* The file is loaded without core_lock, so a config-file set may land meanwhile; not yet started, it only stores
   * its path. So the config is installed, and started set, only if the path is still the one loaded, in the same
   * critical section; otherwise the newer path is loaded instead. Once started is set, sets load and go to pending,
   * which stop clears with started. */
  for (;;) {
    RdsAnalyticsConfig *cfg;
    gchar *path;
    gboolean current;

    g_mutex_lock (&self->core_lock);
    path = g_strdup (self->config_file);
    g_mutex_unlock (&self->core_lock);
    /* required whatever enable says (DeepStream 8.0 behaviour) */
    cfg = load_config (self, path);
    if (!cfg) {
      g_free (path);
      return FALSE;
    }
    g_mutex_lock (&self->core_lock);
    current = g_strcmp0 (path, self->config_file) == 0;
    if (current) {
      g_clear_pointer (&self->pending, rds_analytics_config_free);
      self->cfg = g_steal_pointer (&cfg);
      self->started = TRUE;
    }
    g_mutex_unlock (&self->core_lock);
    g_free (path);
    if (current)
      return TRUE;
    rds_analytics_config_free (cfg);
  }
}

static gboolean
gst_rds_analytics_stop (GstBaseTransform * trans)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (trans);

  g_mutex_lock (&self->core_lock);
  self->started = FALSE;
  g_clear_pointer (&self->core, rds_analytics_core_free);
  g_clear_pointer (&self->cfg, rds_analytics_config_free);
  g_clear_pointer (&self->pending, rds_analytics_config_free);
  self->out_w = self->out_h = 0;
  g_mutex_unlock (&self->core_lock);
  return TRUE;
}

/* ---- events ---- */

/* The stream events name a source by the id rdsstreammux gives it: the N of its sink_N pad, which the mux also
 * writes to both source_id and pad_index of that source's frame metas (fill_frame_meta, and every rds_event_new_*
 * call, take the pad's index). So an event's id is the pad_index the core keys its streams by, with no mapping.
 *
 * A source's removal is its rds-pad-deleted, which rdsstreammux sends once the pad can give no more frames
 * (release_pad): it frees the stream's state (ours). rds-stream-eos alone changes nothing: a stream that ends and
 * stays on its pad (a looping or reconnecting source, a flush) is not removed, and its restart comes with an
 * rds-stream-reset (the mux sends one on the pad's first data after a flush). FLUSH_STOP needs nothing for that
 * reason. The core lock keeps events off a batch in progress, and it is needed: rds-pad-deleted is sent from
 * release_pad, so it can arrive on the application's thread, not the streaming one. */
static gboolean
gst_rds_analytics_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (trans);
  guint pad_index;

  switch ((RdsEventType) GST_EVENT_TYPE (event)) {
    case RDS_EVENT_STREAM_RESET:
      /* ours: 8.0 ignores it */
      if (rds_event_parse_stream_reset (event, &pad_index)) {
        g_mutex_lock (&self->core_lock);
        if (self->core)
          rds_analytics_core_reset_stream (self->core, pad_index);
        g_mutex_unlock (&self->core_lock);
        GST_DEBUG_OBJECT (self, "stream %u reset", pad_index);
      }
      break;
    case RDS_EVENT_PAD_DELETED:
      /* ours: 8.0 ignores it */
      if (rds_event_parse_pad_deleted (event, &pad_index)) {
        g_mutex_lock (&self->core_lock);
        if (self->core)
          rds_analytics_core_remove_stream (self->core, pad_index);
        g_mutex_unlock (&self->core_lock);
        GST_DEBUG_OBJECT (self, "stream %u removed", pad_index);
      }
      break;
    default:
      break;
  }
  return GST_BASE_TRANSFORM_CLASS (gst_rds_analytics_parent_class)->sink_event (trans, event);
}

/* ---- streaming ---- */

/* The frame's analytics frame meta, on every frame (DeepStream 8.0 behaviour), from the result's arrays. */
static void
write_frame_meta (RdsFrameMeta * fm, const RdsAnalyticsFrameResult * res, guint uid)
{
  RdsAnalyticsFrameMeta *m = rds_analytics_frame_meta_attach (fm, uid);

  if (!m)
    return;                     /* the library logged a critical */
  for (guint i = 0; i < res->n_roi_counts; i++)
    rds_analytics_frame_meta_set_roi_count (m, res->roi_counts[i].label, res->roi_counts[i].count);
  for (guint i = 0; i < res->n_oc; i++)
    rds_analytics_frame_meta_set_oc_status (m, res->oc[i].label, res->oc[i].status);
  for (guint i = 0; i < res->n_lc; i++)
    rds_analytics_frame_meta_set_lc_counts (m, res->lc[i].label, res->lc[i].current, res->lc[i].cumulative);
  for (guint i = 0; i < res->n_obj_cnt; i++)
    rds_analytics_frame_meta_set_obj_count (m, res->obj_cnt[i].class_id, res->obj_cnt[i].count);
}

/* An object info on each object with something to report, and on no other (DeepStream 8.0 behaviour). */
static void
write_obj_infos (GPtrArray * obj_metas, const RdsAnalyticsFrameResult * res, guint uid)
{
  for (guint i = 0; i < res->n_objects; i++) {
    const RdsAnalyticsObjectResult *r = &res->objects[i];
    RdsAnalyticsObjInfo *info;

    if (!r->has_info)
      continue;
    if (!(info = rds_analytics_obj_info_attach (g_ptr_array_index (obj_metas, i), uid)))
      continue;
    for (guint k = 0; k < r->n_roi_labels; k++)
      rds_analytics_obj_info_add_roi_status (info, r->roi_labels[k]);
    for (guint k = 0; k < r->n_oc_labels; k++)
      rds_analytics_obj_info_add_oc_status (info, r->oc_labels[k]);
    for (guint k = 0; k < r->n_lc_labels; k++)
      rds_analytics_obj_info_add_lc_status (info, r->lc_labels[k]);
    rds_analytics_obj_info_set_dir_status (info, r->dir_status);
    rds_analytics_obj_info_set_obj_status (info, r->obj_status);
  }
}

/* One frame through the core, then its metas. Called with the core lock and the batch meta lock held. */
static void
process_frame (GstRdsAnalytics * self, RdsFrameMeta * fm, guint uid)
{
  RdsAnalyticsFrameResult res;

  g_array_set_size (self->objs, 0);
  g_ptr_array_set_size (self->obj_metas, 0);
  for (GList * l = fm->obj_meta_list; l; l = l->next) {
    RdsObjectMeta *om = l->data;
    RdsAnalyticsObject o = {
      .id = om->object_id,.class_id = om->class_id,
      .left = om->rect_params.left,.top = om->rect_params.top,
      .width = om->rect_params.width,.height = om->rect_params.height,
    };

    g_array_append_val (self->objs, o);
    g_ptr_array_add (self->obj_metas, om);
  }
  /* streams are keyed by pad_index, as the config's -stream-<n> groups are (DeepStream 8.0 behaviour) */
  rds_analytics_core_process_frame (self->core, fm->pad_index, fm->buf_pts,
      (const RdsAnalyticsObject *) self->objs->data, self->objs->len, &res);
  if (res.untracked_warning)
    GST_WARNING_OBJECT (self, "stream %u: untracked objects (without a tracker id): direction and line crossing "
        "skip them; they still count for the ROIs and overcrowding", fm->pad_index);

  write_frame_meta (fm, &res, uid);
  write_obj_infos (self->obj_metas, &res, uid);
  /* the overlays (osd-mode, display-obj-cnt) from the same res, whose arrays are the core's and are valid only until
   * the next rds_analytics_core_process_frame */
  rds_analytics_draw_frame (fm, self->cfg, self->core, &res, (RdsObjectMeta * const *) self->obj_metas->pdata);
}

/* Passthrough with transform_ip, as in rdstracker: the buffer is never made writable; the shared RdsBatchMeta is
 * written in place under rds_acquire_meta_lock, and pixels are never touched. */
static GstFlowReturn
gst_rds_analytics_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (trans);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsBufSurface *surf;
  gboolean enable;
  guint uid, w, h;

  GST_OBJECT_LOCK (self);
  enable = self->enable;
  uid = self->unique_id;
  GST_OBJECT_UNLOCK (self);
  /* DeepStream 8.0 behaviour: no meta at all */
  if (!enable)
    return GST_FLOW_OK;

  if (!bm) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("rdsanalytics needs batches from rdsstreammux"),
        ("the buffer has no RdsBatchMeta"));
    return GST_FLOW_ERROR;
  }
  surf = rds_buffer_get_surface (buf);
  if (!surf || surf->batchSize == 0) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("rdsanalytics needs batches from rdsstreammux"),
        ("the buffer has no RdsBufSurface"));
    return GST_FLOW_ERROR;
  }
  /* the rules are scaled to the mux output: the batch's surface size (DeepStream 8.0 behaviour) */
  w = surf->surfaceList[0].width;
  h = surf->surfaceList[0].height;

  g_mutex_lock (&self->core_lock);
  if (self->pending) {
    /* a config set while playing, from this buffer on, with all state reset (DeepStream 8.0 behaviour) */
    g_clear_pointer (&self->core, rds_analytics_core_free);
    g_clear_pointer (&self->cfg, rds_analytics_config_free);
    self->cfg = g_steal_pointer (&self->pending);
    GST_INFO_OBJECT (self, "new config applied: every stream's state reset");
  }
  if (!self->cfg) {
    /* stopped (or stopping) while this buffer was on its way: start always installs a config */
    g_mutex_unlock (&self->core_lock);
    return GST_FLOW_FLUSHING;
  }
  /* [property] enable=0: no meta at all (DeepStream 8.0 behaviour) */
  if (!self->cfg->enable) {
    g_mutex_unlock (&self->core_lock);
    return GST_FLOW_OK;
  }
  if (self->core && (w != self->out_w || h != self->out_h)) {
    /* ours: the rules are scaled again, and every stream's state starts afresh */
    GST_WARNING_OBJECT (self, "the mux output size changed from %ux%u to %ux%u: rules rescaled, every stream's "
        "state reset", self->out_w, self->out_h, w, h);
    g_clear_pointer (&self->core, rds_analytics_core_free);
  }
  if (!self->core) {
    self->core = rds_analytics_core_new (self->cfg, w, h);
    self->out_w = w;
    self->out_h = h;
    GST_DEBUG_OBJECT (self, "rules scaled from %ux%u to %ux%u", self->cfg->config_width, self->cfg->config_height,
        w, h);
  }

  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next)
    process_frame (self, l->data, uid);
  rds_release_meta_lock (bm);
  g_mutex_unlock (&self->core_lock);
  return GST_FLOW_OK;
}

/* ---- properties ---- */

/* enable is read per batch but may change only in NULL or READY (DeepStream 8.0 behaviour): refuse changes once
 * streaming, including during the READY -> PAUSED transition (object lock held). */
static gboolean
check_mutable (GstRdsAnalytics * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_analytics_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (object);

  switch (prop_id) {
    case PROP_CONFIG_FILE:
      set_config_file (self, g_value_get_string (value));
      break;
    case PROP_ENABLE:
      GST_OBJECT_LOCK (self);
      if (check_mutable (self, pspec))
        self->enable = g_value_get_boolean (value);
      GST_OBJECT_UNLOCK (self);
      break;
    case PROP_UNIQUE_ID:
      GST_OBJECT_LOCK (self);
      self->unique_id = g_value_get_uint (value);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rds_analytics_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (object);

  if (prop_id == PROP_CONFIG_FILE) {
    g_mutex_lock (&self->core_lock);
    g_value_set_string (value, self->config_file);
    g_mutex_unlock (&self->core_lock);
    return;
  }
  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_ENABLE:
      g_value_set_boolean (value, self->enable);
      break;
    case PROP_UNIQUE_ID:
      g_value_set_uint (value, self->unique_id);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_analytics_finalize (GObject * object)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (object);

  g_free (self->config_file);
  g_clear_pointer (&self->core, rds_analytics_core_free);
  g_clear_pointer (&self->cfg, rds_analytics_config_free);
  g_clear_pointer (&self->pending, rds_analytics_config_free);
  g_array_unref (self->objs);
  g_ptr_array_unref (self->obj_metas);
  g_mutex_clear (&self->core_lock);
  G_OBJECT_CLASS (gst_rds_analytics_parent_class)->finalize (object);
}

#define PFLAGS (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)

static void
gst_rds_analytics_class_init (GstRdsAnalyticsClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_rds_analytics_debug, "rdsanalytics", 0, "Redstream video analytics");

  gobject_class->set_property = gst_rds_analytics_set_property;
  gobject_class->get_property = gst_rds_analytics_get_property;
  gobject_class->finalize = gst_rds_analytics_finalize;

  g_object_class_install_property (gobject_class, PROP_CONFIG_FILE,
      g_param_spec_string ("config-file", "Config file",
          "Analytics config file (key file); set while playing, it applies from the next buffer with every "
          "stream's state reset", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_ENABLE,
      g_param_spec_boolean ("enable", "Enable",
          "Run the analytics, or pass the batches through with no meta", DEFAULT_ENABLE,
          PFLAGS | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_UNIQUE_ID,
      g_param_spec_uint ("unique-id", "Unique ID",
          "ID written to the element's metas, to tell its output from another instance's", 0, G_MAXUINT,
          DEFAULT_UNIQUE_ID, PFLAGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream video analytics", "Filter/Video",
      "ROI filtering, overcrowding, direction detection and line crossing on the objects of memory:RDS batches",
      "Redstream contributors");

  trans_class->start = GST_DEBUG_FUNCPTR (gst_rds_analytics_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_rds_analytics_stop);
  trans_class->sink_event = GST_DEBUG_FUNCPTR (gst_rds_analytics_sink_event);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_rds_analytics_transform_ip);
  trans_class->passthrough_on_same_caps = TRUE;
  trans_class->transform_ip_on_passthrough = TRUE;
}

/* ---- test hooks ---- */

guint
rds_analytics_test_get_history_count (GstElement * element)
{
  GstRdsAnalytics *self = GST_RDS_ANALYTICS (element);
  guint n;

  g_mutex_lock (&self->core_lock);
  n = self->core ? rds_analytics_core_count_histories (self->core) : 0;
  g_mutex_unlock (&self->core_lock);
  return n;
}

static void
gst_rds_analytics_init (GstRdsAnalytics * self)
{
  self->enable = DEFAULT_ENABLE;
  self->unique_id = DEFAULT_UNIQUE_ID;
  g_mutex_init (&self->core_lock);
  self->objs = g_array_new (FALSE, TRUE, sizeof (RdsAnalyticsObject));
  self->obj_metas = g_ptr_array_new ();
}
