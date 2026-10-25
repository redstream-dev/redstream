/* Redstream: rdstracker, a multi-object tracker. Property names follow DeepStream's public nvtracker documentation.
 *
 * An in-place GstBaseTransform in passthrough mode on rdsstreammux batches: buffers go downstream unchanged, with
 * the objects in their RdsBatchMeta tracked. Pixels are never written. The tracking itself is the CPU-only core in
 * rds-tracker-core (IOU or NvSORT, from ll-config-file); this file feeds it and writes its results back.
 * - start (READY -> PAUSED) loads ll-config-file (NULL: every default), posts its warnings, checks ll-lib-file (only
 *   the built-in tracker exists) and operate-on-class-ids, warns about the accepted no-op properties set to other
 *   than their default, and creates the core. Any error fails the state change. The properties are read here;
 *   display-tracking-id is read per batch.
 * - Per batch, under rds_acquire_meta_lock for the whole read-modify-write: each frame (stream = the frame meta's
 *   source_id, the id rdsstreammux also puts in its stream events) gives the core its objects (the detector's
 *   unclipped box, else rect_params; objects of classes outside operate-on-class-ids are left untouched) and
 *   whether inference ran (bInferDone). Then (DeepStream 8.0 behaviour): matched objects get the target's ID,
 *   class, box (tracker_bbox_info unclipped, rect_params clipped to the frame) and tracker confidence; detections
 *   without a reported target are removed; targets reported without a detection (frames without inference) become
 *   new objects, prepended newest ID first, labelled from that target's last matched detection; with
 *   display-tracking-id the ID is appended to display_text.
 * - The core is touched under core_lock only: by the streaming thread, by the stream events (PAD_DELETED,
 *   STREAM_RESET, STREAM_EOS) and FLUSH_STOP, and by start/stop. Every event still goes downstream.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdstracker.h"

#include "rds-object-style.h"
#include "rds-tracker-config.h"
#include "rds-tracker-core.h"

#include <rds/rds.h>
#include <string.h>

GST_DEBUG_CATEGORY (gst_rds_tracker_debug);
#define GST_CAT_DEFAULT gst_rds_tracker_debug

/* Property defaults, as nvtracker reports them. */
#define DEFAULT_TRACKER_WIDTH 960
#define DEFAULT_TRACKER_HEIGHT 544
#define DEFAULT_GPU_ID 0
#define DEFAULT_DISPLAY_TRACKING_ID TRUE
#define DEFAULT_TRACKING_ID_RESET_MODE 1
#define DEFAULT_TRACKING_SURFACE_TYPE 0
#define DEFAULT_COMPUTE_HW GST_RDS_TRACKER_COMPUTE_HW_DEFAULT
#define DEFAULT_INPUT_TENSOR_META FALSE
#define DEFAULT_TENSOR_META_GIE_ID 0
#define DEFAULT_SUB_BATCHES ""
#define DEFAULT_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT 0
#define DEFAULT_USER_META_POOL_SIZE 32

/* ll-lib-file names that select the built-in tracker (as a basename or a path ending in one): DeepStream's own
 * library name, so configs and apps run unchanged, and ours. */
#define BUILTIN_LIB "librdstracker-builtin.so"
static const gchar *const builtin_libs[] = { "libnvds_nvmultiobjecttracker.so", BUILTIN_LIB };

/* DeepStream 8.0 behaviour: an object the tracker creates on a frame without inference has this confidence. */
#define CREATED_CONFIDENCE (-0.1f)

enum
{
  PROP_0,
  PROP_LL_LIB_FILE,
  PROP_LL_CONFIG_FILE,
  PROP_TRACKER_WIDTH,
  PROP_TRACKER_HEIGHT,
  PROP_GPU_ID,
  PROP_DISPLAY_TRACKING_ID,
  PROP_TRACKING_ID_RESET_MODE,
  PROP_TRACKING_SURFACE_TYPE,
  PROP_COMPUTE_HW,
  PROP_INPUT_TENSOR_META,
  PROP_TENSOR_META_GIE_ID,
  PROP_SUB_BATCHES,
  PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT,
  PROP_USER_META_POOL_SIZE,
  PROP_OPERATE_ON_CLASS_IDS,
};

struct _GstRdsTracker
{
  GstBaseTransform parent;

  /* properties (object lock) */
  gchar *ll_lib_file;
  gchar *ll_config_file;
  guint tracker_width;
  guint tracker_height;
  guint gpu_id;
  gboolean display_tracking_id;
  guint tracking_id_reset_mode;
  guint tracking_surface_type;
  GstRdsTrackerComputeHw compute_hw;
  gboolean input_tensor_meta;
  guint tensor_meta_gie_id;
  gchar *sub_batches;
  gint sub_batch_err_recovery_trial_cnt;
  guint user_meta_pool_size;
  gchar *operate_on_class_ids;

  /* streaming state (core_lock) */
  GMutex core_lock;
  RdsTrackerCore *core;
  GArray *class_ids;            /* gint: operate-on-class-ids; NULL: every class */
  /* per-batch scratch */
  GPtrArray *frames;            /* RdsFrameMeta *, in batch order */
  GArray *frames_in;            /* RdsTrackerFrameIn */
  GArray *dets;                 /* RdsTrackerDet, every frame's in turn */
};

/* What a target keeps of its last matched detection, to label the objects created for it (RdsTrackerCoreOptions
 * user_copy): copied before the ID is appended to the detection's display_text. */
typedef struct
{
  gchar obj_label[RDS_MAX_LABEL_SIZE];
  gchar *display_text;          /* the text without the ID; may be NULL */
  gint unique_component_id;
} TargetLabel;

#define RDS_TRACKER_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), format = (string) { NV12, RGBA, BGRA, RGBx, BGRx }, " \
  "width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_TRACKER_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_TRACKER_CAPS));

GType
gst_rds_tracker_compute_hw_get_type (void)
{
  static gsize type;
  static const GEnumValue values[] = {
    {GST_RDS_TRACKER_COMPUTE_HW_DEFAULT, "Default", "default"},
    {GST_RDS_TRACKER_COMPUTE_HW_GPU, "GPU", "gpu"},
    {GST_RDS_TRACKER_COMPUTE_HW_VIC, "VIC", "vic"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsTrackerComputeHw", values);

    g_once_init_leave (&type, t);
  }
  return (GType) type;
}

G_DEFINE_TYPE (GstRdsTracker, gst_rds_tracker, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdstracker, "rdstracker", GST_RANK_NONE, GST_TYPE_RDS_TRACKER);

/* ---- start / stop ---- */

static gpointer
target_label_copy (gconstpointer src, gpointer data)
{
  const RdsObjectMeta *om = src;
  TargetLabel *l = g_new0 (TargetLabel, 1);

  (void) data;
  g_strlcpy (l->obj_label, om->obj_label, sizeof (l->obj_label));
  l->display_text = g_strdup (om->text_params.display_text);
  l->unique_component_id = om->unique_component_id;
  return l;
}

static void
target_label_free (gpointer data)
{
  TargetLabel *l = data;

  g_free (l->display_text);
  g_free (l);
}

static gboolean
lib_is_builtin (const gchar * lib)
{
  gchar *base;
  gboolean builtin = FALSE;

  if (!lib || !*lib)
    return TRUE;
  base = g_path_get_basename (lib);
  for (guint i = 0; i < G_N_ELEMENTS (builtin_libs); i++)
    builtin |= strcmp (base, builtin_libs[i]) == 0;
  g_free (base);
  return builtin;
}

/* operate-on-class-ids: class IDs separated by ';' (empty items are skipped). NULL with *@ok TRUE: every class. */
static GArray *
parse_class_ids (const gchar * s, gboolean * ok)
{
  gchar **items;
  GArray *ids;

  *ok = TRUE;
  if (!s)
    return NULL;
  items = g_strsplit (s, ";", -1);
  ids = g_array_new (FALSE, FALSE, sizeof (gint));
  for (guint i = 0; items[i] && *ok; i++) {
    gchar *item = g_strstrip (items[i]);
    gint64 v;

    if (!*item)
      continue;
    if (g_ascii_string_to_signed (item, 10, G_MININT, G_MAXINT, &v, NULL)) {
      gint id = (gint) v;

      g_array_append_val (ids, id);
    } else {
      *ok = FALSE;
    }
  }
  g_strfreev (items);
  if (!*ok || ids->len == 0) {
    g_array_unref (ids);
    return NULL;
  }
  return ids;
}

static void
post_noop_warning (GstRdsTracker * self, const gchar * prop, const gchar * value, const gchar * why)
{
  GST_ELEMENT_WARNING (self, RESOURCE, SETTINGS, ("element property %s: %s: ignored: %s", prop, value, why),
      (NULL));
}

/* Each accepted no-op property set to other than its default warns once per start. */
static void
warn_noop_properties (GstRdsTracker * self)
{
  GstRdsTrackerComputeHw hw;
  guint surface, pool, gie;
  gboolean tensor;
  gint trials;
  gchar *sub, *v;

  GST_OBJECT_LOCK (self);
  hw = self->compute_hw;
  surface = self->tracking_surface_type;
  pool = self->user_meta_pool_size;
  tensor = self->input_tensor_meta;
  gie = self->tensor_meta_gie_id;
  trials = self->sub_batch_err_recovery_trial_cnt;
  sub = g_strdup (self->sub_batches);
  GST_OBJECT_UNLOCK (self);

  if (hw != DEFAULT_COMPUTE_HW) {
    GEnumValue *ev = g_enum_get_value (g_type_class_peek (GST_TYPE_RDS_TRACKER_COMPUTE_HW), hw);

    post_noop_warning (self, "compute-hw", ev ? ev->value_nick : "?",
        "the tracker scales no frames: it works on the metadata, on the CPU");
  }
  if (surface != DEFAULT_TRACKING_SURFACE_TYPE) {
    v = g_strdup_printf ("%u", surface);
    post_noop_warning (self, "tracking-surface-type", v, "every surface of the batch is tracked");
    g_free (v);
  }
  if (pool != DEFAULT_USER_META_POOL_SIZE) {
    v = g_strdup_printf ("%u", pool);
    post_noop_warning (self, "user-meta-pool-size", v, "rdstracker emits no tracker user meta yet");
    g_free (v);
  }
  if (tensor != DEFAULT_INPUT_TENSOR_META)
    post_noop_warning (self, "input-tensor-meta", "true", "tensor meta input is not supported");
  if (gie != DEFAULT_TENSOR_META_GIE_ID) {
    v = g_strdup_printf ("%u", gie);
    post_noop_warning (self, "tensor-meta-gie-id", v, "tensor meta input is not supported");
    g_free (v);
  }
  if (sub && *sub)
    post_noop_warning (self, "sub-batches", sub, "the tracker runs as one instance for every stream");
  if (trials != DEFAULT_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT) {
    v = g_strdup_printf ("%d", trials);
    post_noop_warning (self, "sub-batch-err-recovery-trial-cnt", v,
        "the tracker runs as one instance for every stream, without sub-batches");
    g_free (v);
  }
  g_free (sub);
}

static gboolean
gst_rds_tracker_start (GstBaseTransform * trans)
{
  GstRdsTracker *self = GST_RDS_TRACKER (trans);
  RdsTrackerCoreOptions opts = {.user_copy = target_label_copy,.user_free = target_label_free };
  RdsTrackerConfig *cfg = NULL;
  gchar *lib, *path, *class_ids;
  guint width, height, gpu;
  GArray *ids;
  GError *err = NULL;
  gboolean ok;

  GST_OBJECT_LOCK (self);
  lib = g_strdup (self->ll_lib_file);
  path = g_strdup (self->ll_config_file);
  class_ids = g_strdup (self->operate_on_class_ids);
  opts.reset_mode = (RdsTrackerIdResetMode) self->tracking_id_reset_mode;
  width = self->tracker_width;
  height = self->tracker_height;
  gpu = self->gpu_id;
  GST_OBJECT_UNLOCK (self);

  if (!lib_is_builtin (lib)) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("element property ll-lib-file: %s: custom low-level tracker "
            "libraries are not supported yet; leave it unset, or set libnvds_nvmultiobjecttracker.so or "
            BUILTIN_LIB " (bare or as a path) for the built-in tracker", lib), (NULL));
    goto fail;
  }
  if (lib && *lib)
    GST_INFO_OBJECT (self, "ll-lib-file %s: using the built-in tracker", lib);
  ids = parse_class_ids (class_ids, &ok);
  if (!ok) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("element property operate-on-class-ids: %s: not a list of class "
            "IDs separated by ';'", class_ids), (NULL));
    goto fail;
  }
  if (!(cfg = rds_tracker_config_load (path, &err))) {
    if (err->domain == RDS_TRACKER_CONFIG_ERROR && err->code == RDS_TRACKER_CONFIG_ERROR_FILE)
      GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("%s", err->message), (NULL));
    else
      GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("%s", err->message), (NULL));
    g_error_free (err);
    if (ids)
      g_array_unref (ids);
    goto fail;
  }
  for (guint i = 0; i < cfg->warnings->len; i++)
    GST_ELEMENT_WARNING (self, RESOURCE, SETTINGS, ("%s", (const gchar *) g_ptr_array_index (cfg->warnings, i)),
        (NULL));
  warn_noop_properties (self);
  /* The core works in frame (mux) coordinates and on the CPU: neither the tracker size nor the GPU matter (DeepStream
   * 8.0 behaviour for the size). */
  GST_DEBUG_OBJECT (self, "tracker-width %u, tracker-height %u and gpu-id %u have no effect", width, height, gpu);

  g_mutex_lock (&self->core_lock);
  self->core = rds_tracker_core_new (cfg, &opts);
  self->class_ids = ids;
  g_mutex_unlock (&self->core_lock);
  GST_INFO_OBJECT (self, "%s tracker from %s, tracking-id-reset-mode %u", rds_tracker_type_name (cfg->tracker_type),
      path ? path : "the defaults", (guint) opts.reset_mode);
  rds_tracker_config_free (cfg);
  g_free (lib);
  g_free (path);
  g_free (class_ids);
  return TRUE;

fail:
  g_free (lib);
  g_free (path);
  g_free (class_ids);
  return FALSE;
}

static gboolean
gst_rds_tracker_stop (GstBaseTransform * trans)
{
  GstRdsTracker *self = GST_RDS_TRACKER (trans);

  g_mutex_lock (&self->core_lock);
  g_clear_pointer (&self->core, rds_tracker_core_free);
  g_clear_pointer (&self->class_ids, g_array_unref);
  g_mutex_unlock (&self->core_lock);
  return TRUE;
}

static void
with_core (GstRdsTracker * self, void (*func) (RdsTrackerCore *, guint64), guint source_id)
{
  g_mutex_lock (&self->core_lock);
  if (self->core)
    func (self->core, source_id);
  g_mutex_unlock (&self->core_lock);
}

/* Stream events change the core's per-stream state; PAD_ADDED needs nothing (streams are created on first sight).
 * Events are serialized with the buffers; core_lock keeps them off a batch in progress anyway. Only these four take
 * it: FLUSH_START and the rest never wait for a batch. */
static gboolean
gst_rds_tracker_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstRdsTracker *self = GST_RDS_TRACKER (trans);
  guint source_id;

  switch ((RdsEventType) GST_EVENT_TYPE (event)) {
    case RDS_EVENT_PAD_DELETED:
      if (rds_event_parse_pad_deleted (event, &source_id))
        with_core (self, rds_tracker_core_remove_stream, source_id);
      break;
    case RDS_EVENT_STREAM_RESET:
      if (rds_event_parse_stream_reset (event, &source_id))
        with_core (self, rds_tracker_core_stream_reset, source_id);
      break;
    case RDS_EVENT_STREAM_EOS:
      if (rds_event_parse_stream_eos (event, &source_id))
        with_core (self, rds_tracker_core_stream_eos, source_id);
      break;
    default:
      if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
        g_mutex_lock (&self->core_lock);
        if (self->core)
          rds_tracker_core_clear (self->core);
        g_mutex_unlock (&self->core_lock);
      }
      break;
  }
  return GST_BASE_TRANSFORM_CLASS (gst_rds_tracker_parent_class)->sink_event (trans, event);
}

/* ---- streaming ---- */

static gboolean
class_tracked (GstRdsTracker * self, gint class_id)
{
  if (!self->class_ids)
    return TRUE;
  for (guint i = 0; i < self->class_ids->len; i++)
    if (g_array_index (self->class_ids, gint, i) == class_id)
      return TRUE;
  return FALSE;
}

/* The frame size boxes are clipped to: the mux output (pipeline_width/height), as rdsinfer clips to its slot; else
 * the frame's slot in the batch; 0: not clipped on that axis. */
static void
frame_size (RdsFrameMeta * fm, RdsBufSurface * surf, gfloat * w, gfloat * h)
{
  *w = fm->pipeline_width;
  *h = fm->pipeline_height;
  if ((*w == 0 || *h == 0) && surf && fm->batch_id < surf->batchSize) {
    *w = surf->surfaceList[fm->batch_id].width;
    *h = surf->surfaceList[fm->batch_id].height;
  }
}

/* The unclipped box into tracker_bbox_info and, clipped to @fw x @fh (ours: an empty clip keeps a 0 width or height),
 * into rect_params' box. */
static void
set_boxes (RdsObjectMeta * om, const RdsTrackerTarget * t, gfloat fw, gfloat fh)
{
  gfloat l = t->x, top = t->y, r = t->x + t->w, b = t->y + t->h;

  om->tracker_bbox_info.org_bbox_coords.left = t->x;
  om->tracker_bbox_info.org_bbox_coords.top = t->y;
  om->tracker_bbox_info.org_bbox_coords.width = t->w;
  om->tracker_bbox_info.org_bbox_coords.height = t->h;
  if (fw > 0) {
    l = CLAMP (l, 0, fw);
    r = CLAMP (r, 0, fw);
  }
  if (fh > 0) {
    top = CLAMP (top, 0, fh);
    b = CLAMP (b, 0, fh);
  }
  om->rect_params.left = l;
  om->rect_params.top = top;
  om->rect_params.width = MAX (r - l, 0);
  om->rect_params.height = MAX (b - top, 0);
}

/* display-tracking-id: "<text> <id>" in a fresh string; NULL text gives "<id>" (ours: DeepStream 8.0 prints
 * "(null) <id>"). */
static void
append_id (RdsObjectMeta * om, guint64 id)
{
  gchar *old = om->text_params.display_text;

  om->text_params.display_text = old ? g_strdup_printf ("%s %" G_GUINT64_FORMAT, old, id) :
      g_strdup_printf ("%" G_GUINT64_FORMAT, id);
  g_free (old);
}

/* A target reported without a detection: a new object (DeepStream 8.0 behaviour), labelled from the target's last
 * matched detection, drawn with rdsinfer's defaults. */
static RdsObjectMeta *
create_object (RdsBatchMeta * bm, const RdsTrackerTarget * t, gfloat fw, gfloat fh, gboolean show_id)
{
  RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);
  const TargetLabel *label = t->last_user;

  om->object_id = t->id;
  om->class_id = t->class_id;
  om->confidence = CREATED_CONFIDENCE;
  om->tracker_confidence = t->tracker_confidence;
  /* the pool hands out zeroed metas today, but its API doesn't promise it */
  memset (&om->detector_bbox_info, 0, sizeof (om->detector_bbox_info));
  set_boxes (om, t, fw, fh);
  rds_object_meta_set_drawing_defaults (om);
  om->unique_component_id = label ? label->unique_component_id : 0;
  g_strlcpy (om->obj_label, label ? label->obj_label : "", sizeof (om->obj_label));
  om->text_params.display_text = g_strdup (label ? label->display_text : NULL);
  if (show_id)
    append_id (om, t->id);
  return om;
}

static void
write_back (RdsBatchMeta * bm, RdsFrameMeta * fm, const RdsTrackerFrameOut * fo, gfloat fw, gfloat fh,
    gboolean show_id)
{
  guint n_kept, n_created = 0;

  /* DeepStream 8.0 behaviour: detections without a reported target leave the frame (releasing their strings) */
  for (guint i = 0; i < fo->n_rejected; i++)
    rds_remove_obj_meta_from_frame (fm, fo->rejected[i]);
  n_kept = g_list_length (fm->obj_meta_list);

  for (guint i = 0; i < fo->n_targets; i++) {
    const RdsTrackerTarget *t = &fo->targets[i];
    RdsObjectMeta *om = t->associated_user;

    if (om) {
      /* confidence, detector_bbox_info, obj_label and unique_component_id stay the detector's; class_id is the
       * target's (it follows the detection with checkClassMatch 0) */
      om->object_id = t->id;
      om->class_id = t->class_id;
      om->tracker_confidence = t->tracker_confidence;
      set_boxes (om, t, fw, fh);
      if (show_id)
        append_id (om, t->id);
    } else {
      rds_add_obj_meta_to_frame (fm, create_object (bm, t, fw, fh, show_id), NULL);
      n_created++;
    }
  }

  /* DeepStream 8.0 behaviour: created objects go first, in the core's order (newest ID first). They were appended:
   * move that tail to the head of the frame's own list (under the meta lock; no meta changes lists). */
  if (n_created && n_kept) {
    GList *created = g_list_nth (fm->obj_meta_list, n_kept);

    created->prev->next = NULL;
    created->prev = NULL;
    fm->obj_meta_list = g_list_concat (created, fm->obj_meta_list);
  }
}

static gboolean
has_source (GPtrArray * frames, guint source_id)
{
  for (guint i = 0; i < frames->len; i++)
    if (((RdsFrameMeta *) g_ptr_array_index (frames, i))->source_id == source_id)
      return TRUE;
  return FALSE;
}

/* Passthrough with transform_ip, as in rdsinfer: the buffer is never made writable. The RdsBatchMeta is shared by
 * every holder of the batch on purpose (as DeepStream's batch meta is), so it is written in place under
 * rds_acquire_meta_lock, like rdsinfer adds its objects (see rdsinfer's header comment), and pixels are never
 * touched. */
static GstFlowReturn
gst_rds_tracker_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRdsTracker *self = GST_RDS_TRACKER (trans);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsBufSurface *surf = rds_buffer_get_surface (buf);
  RdsTrackerBatchOut out;
  gboolean show_id;
  guint duplicate = 0;
  gboolean has_duplicate = FALSE;

  if (!bm) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("rdstracker needs batches from rdsstreammux"),
        ("the buffer has no RdsBatchMeta"));
    return GST_FLOW_ERROR;
  }
  GST_OBJECT_LOCK (self);
  show_id = self->display_tracking_id;
  GST_OBJECT_UNLOCK (self);

  g_mutex_lock (&self->core_lock);
  if (!self->core) {
    g_mutex_unlock (&self->core_lock);
    return GST_FLOW_FLUSHING;
  }
  rds_acquire_meta_lock (bm);
  g_ptr_array_set_size (self->frames, 0);
  g_array_set_size (self->frames_in, 0);
  g_array_set_size (self->dets, 0);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    RdsTrackerFrameIn in = {.stream_id = fm->source_id,.frame_num = fm->frame_num,
      .detection_done = fm->bInferDone
    };

    if (has_source (self->frames, fm->source_id)) {
      has_duplicate = TRUE;
      duplicate = fm->source_id;
      break;
    }
    /* dets is filled for every frame first: its data may move, so the pointers are set afterwards */
    in.dets = GSIZE_TO_POINTER ((gsize) self->dets->len);
    for (GList * o = fm->obj_meta_list; o; o = o->next) {
      RdsObjectMeta *om = o->data;
      const RdsBbox_Coords *box = &om->detector_bbox_info.org_bbox_coords;
      RdsTrackerDet d = {.class_id = om->class_id,.confidence = om->confidence,.user = om };

      if (!class_tracked (self, om->class_id))
        continue;
      /* the detector's unclipped box; an object without one (added by an application, say) has its rect */
      if (box->width > 0 && box->height > 0) {
        d.x = box->left;
        d.y = box->top;
        d.w = box->width;
        d.h = box->height;
      } else {
        d.x = om->rect_params.left;
        d.y = om->rect_params.top;
        d.w = om->rect_params.width;
        d.h = om->rect_params.height;
      }
      g_array_append_val (self->dets, d);
    }
    in.n_dets = self->dets->len - GPOINTER_TO_SIZE (in.dets);
    g_ptr_array_add (self->frames, fm);
    g_array_append_val (self->frames_in, in);
  }
  if (has_duplicate) {
    rds_release_meta_lock (bm);
    g_mutex_unlock (&self->core_lock);
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("A batch has two frames of source %u", duplicate),
        ("rdstracker needs at most one frame per source in a batch, as rdsstreammux makes them"));
    return GST_FLOW_ERROR;
  }
  for (guint f = 0; f < self->frames_in->len; f++) {
    RdsTrackerFrameIn *in = &g_array_index (self->frames_in, RdsTrackerFrameIn, f);

    in->dets = in->n_dets ? &g_array_index (self->dets, RdsTrackerDet, GPOINTER_TO_SIZE (in->dets)) : NULL;
  }

  rds_tracker_core_process (self->core, (const RdsTrackerFrameIn *) self->frames_in->data, self->frames_in->len,
      &out);
  for (guint f = 0; f < out.n_frames; f++) {
    RdsFrameMeta *fm = g_ptr_array_index (self->frames, f);
    gfloat fw, fh;

    frame_size (fm, surf, &fw, &fh);
    write_back (bm, fm, &out.frames[f], fw, fh, show_id);
    GST_LOG_OBJECT (self, "source %u frame %d: %u reported, %u removed", fm->source_id, fm->frame_num,
        out.frames[f].n_targets, out.frames[f].n_rejected);
  }
  rds_release_meta_lock (bm);
  g_mutex_unlock (&self->core_lock);
  return GST_FLOW_OK;
}

/* ---- properties ---- */

/* The READY-only settings are read when streaming starts: refuse changes once streaming, including during the
 * READY -> PAUSED transition (object lock held, which also guards the state fields). */
static gboolean
check_mutable (GstRdsTracker * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_tracker_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsTracker *self = GST_RDS_TRACKER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LL_LIB_FILE:
      if (check_mutable (self, pspec)) {
        g_free (self->ll_lib_file);
        self->ll_lib_file = g_value_dup_string (value);
      }
      break;
    case PROP_LL_CONFIG_FILE:
      if (check_mutable (self, pspec)) {
        g_free (self->ll_config_file);
        self->ll_config_file = g_value_dup_string (value);
      }
      break;
    case PROP_TRACKER_WIDTH:
      if (check_mutable (self, pspec))
        self->tracker_width = g_value_get_uint (value);
      break;
    case PROP_TRACKER_HEIGHT:
      if (check_mutable (self, pspec))
        self->tracker_height = g_value_get_uint (value);
      break;
    case PROP_GPU_ID:
      if (check_mutable (self, pspec))
        self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_DISPLAY_TRACKING_ID:
      self->display_tracking_id = g_value_get_boolean (value);
      break;
    case PROP_TRACKING_ID_RESET_MODE:
      if (check_mutable (self, pspec))
        self->tracking_id_reset_mode = g_value_get_uint (value);
      break;
    case PROP_TRACKING_SURFACE_TYPE:
      if (check_mutable (self, pspec))
        self->tracking_surface_type = g_value_get_uint (value);
      break;
    case PROP_COMPUTE_HW:
      self->compute_hw = g_value_get_enum (value);
      break;
    case PROP_INPUT_TENSOR_META:
      self->input_tensor_meta = g_value_get_boolean (value);
      break;
    case PROP_TENSOR_META_GIE_ID:
      if (check_mutable (self, pspec))
        self->tensor_meta_gie_id = g_value_get_uint (value);
      break;
    case PROP_SUB_BATCHES:
      g_free (self->sub_batches);
      self->sub_batches = g_value_dup_string (value);
      break;
    case PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT:
      self->sub_batch_err_recovery_trial_cnt = g_value_get_int (value);
      break;
    case PROP_USER_META_POOL_SIZE:
      if (check_mutable (self, pspec))
        self->user_meta_pool_size = g_value_get_uint (value);
      break;
    case PROP_OPERATE_ON_CLASS_IDS:
      if (check_mutable (self, pspec)) {
        g_free (self->operate_on_class_ids);
        self->operate_on_class_ids = g_value_dup_string (value);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_tracker_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsTracker *self = GST_RDS_TRACKER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LL_LIB_FILE:
      g_value_set_string (value, self->ll_lib_file);
      break;
    case PROP_LL_CONFIG_FILE:
      g_value_set_string (value, self->ll_config_file);
      break;
    case PROP_TRACKER_WIDTH:
      g_value_set_uint (value, self->tracker_width);
      break;
    case PROP_TRACKER_HEIGHT:
      g_value_set_uint (value, self->tracker_height);
      break;
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_DISPLAY_TRACKING_ID:
      g_value_set_boolean (value, self->display_tracking_id);
      break;
    case PROP_TRACKING_ID_RESET_MODE:
      g_value_set_uint (value, self->tracking_id_reset_mode);
      break;
    case PROP_TRACKING_SURFACE_TYPE:
      g_value_set_uint (value, self->tracking_surface_type);
      break;
    case PROP_COMPUTE_HW:
      g_value_set_enum (value, self->compute_hw);
      break;
    case PROP_INPUT_TENSOR_META:
      g_value_set_boolean (value, self->input_tensor_meta);
      break;
    case PROP_TENSOR_META_GIE_ID:
      g_value_set_uint (value, self->tensor_meta_gie_id);
      break;
    case PROP_SUB_BATCHES:
      g_value_set_string (value, self->sub_batches);
      break;
    case PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT:
      g_value_set_int (value, self->sub_batch_err_recovery_trial_cnt);
      break;
    case PROP_USER_META_POOL_SIZE:
      g_value_set_uint (value, self->user_meta_pool_size);
      break;
    case PROP_OPERATE_ON_CLASS_IDS:
      g_value_set_string (value, self->operate_on_class_ids);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_tracker_finalize (GObject * object)
{
  GstRdsTracker *self = GST_RDS_TRACKER (object);

  g_free (self->ll_lib_file);
  g_free (self->ll_config_file);
  g_free (self->sub_batches);
  g_free (self->operate_on_class_ids);
  g_ptr_array_unref (self->frames);
  g_array_unref (self->frames_in);
  g_array_unref (self->dets);
  g_mutex_clear (&self->core_lock);
  G_OBJECT_CLASS (gst_rds_tracker_parent_class)->finalize (object);
}

#define PFLAGS (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
/* changeable only in NULL or READY */
#define PFLAGS_READY (PFLAGS | GST_PARAM_MUTABLE_READY)

static void
gst_rds_tracker_class_init (GstRdsTrackerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_rds_tracker_debug, "rdstracker", 0, "Redstream multi-object tracker");

  gobject_class->set_property = gst_rds_tracker_set_property;
  gobject_class->get_property = gst_rds_tracker_get_property;
  gobject_class->finalize = gst_rds_tracker_finalize;

  g_object_class_install_property (gobject_class, PROP_LL_LIB_FILE,
      g_param_spec_string ("ll-lib-file", "Low-level library file",
          "Low-level tracker library: only the built-in tracker exists (unset, libnvds_nvmultiobjecttracker.so "
          "or " BUILTIN_LIB ", bare or as a path)", NULL, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_LL_CONFIG_FILE,
      g_param_spec_string ("ll-config-file", "Low-level config file",
          "Tracker configuration file (YAML)", NULL, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_TRACKER_WIDTH,
      g_param_spec_uint ("tracker-width", "Tracker width",
          "Width of the frames the tracker works on", 0, G_MAXUINT, DEFAULT_TRACKER_WIDTH, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_TRACKER_HEIGHT,
      g_param_spec_uint ("tracker-height", "Tracker height",
          "Height of the frames the tracker works on", 0, G_MAXUINT, DEFAULT_TRACKER_HEIGHT, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU ID", "GPU the tracker runs on", 0, G_MAXUINT, DEFAULT_GPU_ID,
          PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_DISPLAY_TRACKING_ID,
      g_param_spec_boolean ("display-tracking-id", "Display tracking ID",
          "Show the tracking ID in each object's label", DEFAULT_DISPLAY_TRACKING_ID, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_TRACKING_ID_RESET_MODE,
      g_param_spec_uint ("tracking-id-reset-mode", "Tracking ID reset mode",
          "When a stream's tracking IDs restart: 0 never, 1 on stream reset, 2 at end of stream, 3 both", 0, 3,
          DEFAULT_TRACKING_ID_RESET_MODE, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_TRACKING_SURFACE_TYPE,
      g_param_spec_uint ("tracking-surface-type", "Tracking surface type",
          "Surface type of the streams to track (0: default)", 0, G_MAXUINT, DEFAULT_TRACKING_SURFACE_TYPE,
          PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_COMPUTE_HW,
      g_param_spec_enum ("compute-hw", "Compute hardware",
          "Hardware for the tracker's frame scaling", GST_TYPE_RDS_TRACKER_COMPUTE_HW, DEFAULT_COMPUTE_HW,
          PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INPUT_TENSOR_META,
      g_param_spec_boolean ("input-tensor-meta", "Input tensor meta",
          "Use tensor meta from an upstream preprocessing element", DEFAULT_INPUT_TENSOR_META, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_TENSOR_META_GIE_ID,
      g_param_spec_uint ("tensor-meta-gie-id", "Tensor meta GIE ID",
          "With input-tensor-meta: the unique ID of the tensor meta to use", 0, G_MAXUINT,
          DEFAULT_TENSOR_META_GIE_ID, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_SUB_BATCHES,
      g_param_spec_string ("sub-batches", "Sub-batches",
          "Split the batch into sub-batches of source IDs, tracked separately (e.g. 0,1;2,3)", DEFAULT_SUB_BATCHES,
          PFLAGS));
  g_object_class_install_property (gobject_class, PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT,
      g_param_spec_int ("sub-batch-err-recovery-trial-cnt", "Sub-batch error recovery trial count",
          "Times a failed sub-batch is restarted (-1: without limit)", -1, G_MAXINT,
          DEFAULT_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_USER_META_POOL_SIZE,
      g_param_spec_uint ("user-meta-pool-size", "User meta pool size",
          "Size of the pool of tracker user meta", 1, G_MAXUINT, DEFAULT_USER_META_POOL_SIZE, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_OPERATE_ON_CLASS_IDS,
      g_param_spec_string ("operate-on-class-ids", "Operate on class IDs",
          "Track only objects of these classes, separated by ';' (e.g. 0;2); unset or empty: all classes", NULL,
          PFLAGS_READY));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream multi-object tracker", "Filter/Video",
      "Gives the objects in memory:RDS batches persistent tracking IDs", "Redstream contributors");

  gst_type_mark_as_plugin_api (GST_TYPE_RDS_TRACKER_COMPUTE_HW, 0);

  trans_class->start = GST_DEBUG_FUNCPTR (gst_rds_tracker_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_rds_tracker_stop);
  trans_class->sink_event = GST_DEBUG_FUNCPTR (gst_rds_tracker_sink_event);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_rds_tracker_transform_ip);
  trans_class->passthrough_on_same_caps = TRUE;
  trans_class->transform_ip_on_passthrough = TRUE;
}

static void
gst_rds_tracker_init (GstRdsTracker * self)
{
  self->tracker_width = DEFAULT_TRACKER_WIDTH;
  self->tracker_height = DEFAULT_TRACKER_HEIGHT;
  self->gpu_id = DEFAULT_GPU_ID;
  self->display_tracking_id = DEFAULT_DISPLAY_TRACKING_ID;
  self->tracking_id_reset_mode = DEFAULT_TRACKING_ID_RESET_MODE;
  self->tracking_surface_type = DEFAULT_TRACKING_SURFACE_TYPE;
  self->compute_hw = DEFAULT_COMPUTE_HW;
  self->input_tensor_meta = DEFAULT_INPUT_TENSOR_META;
  self->tensor_meta_gie_id = DEFAULT_TENSOR_META_GIE_ID;
  self->sub_batches = g_strdup (DEFAULT_SUB_BATCHES);
  self->sub_batch_err_recovery_trial_cnt = DEFAULT_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT;
  self->user_meta_pool_size = DEFAULT_USER_META_POOL_SIZE;
  g_mutex_init (&self->core_lock);
  self->frames = g_ptr_array_new ();
  self->frames_in = g_array_new (FALSE, TRUE, sizeof (RdsTrackerFrameIn));
  self->dets = g_array_new (FALSE, TRUE, sizeof (RdsTrackerDet));
}
