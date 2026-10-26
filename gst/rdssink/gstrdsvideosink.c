/* Redstream: rdsvideosink, a display sink for memory:RDS frames, standing in for DeepStream's nveglglessink and
 * nv3dsink. A bin:
 *
 *   ghost sink pad -> rdsvideoconvert -> video/x-raw,format=BGRx -> videoconvert -> <video-sink>
 *
 * The frames are copied to system memory and handed to a stock GStreamer sink, the factory named by video-sink
 * (ours, default autovideosink). The nveglglessink/nv3dsink properties gst-inspect shows for DeepStream 8.0 keep
 * their names and defaults; those the inner sink also has (sync, qos, max-lateness, window-x, ...) are forwarded to
 * it, and to the sinks inside it when it is a bin (autovideosink picks its sink at READY); the others are stored
 * and do nothing. Behaviour the docs leave open matches DeepStream 8.0 ("DeepStream 8.0 behaviour") except where
 * marked "ours"; every such choice has a row in docs/compat-matrix.md.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsvideosink.h"

#include <rds/rds.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_video_sink_debug);
#define GST_CAT_DEFAULT gst_rds_video_sink_debug

#define DEFAULT_VIDEO_SINK "autovideosink"

enum
{
  PROP_0,
  PROP_VIDEO_SINK,
  PROP_FWD_FIRST,
};

/* Forwarded to the inner sink (and the sinks inside it) when it has a writable property of the same name; otherwise
 * stored, silently (ours: whether a sink has them is known only once autovideosink has picked one). */
enum
{
  FWD_SYNC,
  FWD_QOS,
  FWD_MAX_LATENESS,
  FWD_PROCESSING_DEADLINE,
  FWD_ASYNC,
  FWD_WINDOW_X,
  FWD_WINDOW_Y,
  FWD_WINDOW_WIDTH,
  FWD_WINDOW_HEIGHT,
  FWD_FORCE_ASPECT_RATIO,
  FWD_CREATE_WINDOW,
  FWD_GPU_ID,
  /* GstBaseSink's other writable properties, which both DeepStream sinks show */
  FWD_BLOCKSIZE,
  FWD_ENABLE_LAST_SAMPLE,
  FWD_MAX_BITRATE,
  FWD_RENDER_DELAY,
  FWD_SHOW_PREROLL_FRAME,
  FWD_THROTTLE_TIME,
  FWD_TS_OFFSET,
  N_FWDS,
};

/* nveglglessink's that have no counterpart here: stored; a value that would do something logs one warning per
 * property (ours) */
enum
{
  NOOP_ROWS,
  NOOP_COLUMNS,
  NOOP_PROFILE,
  NOOP_DISPLAY,
  NOOP_DO_TRANSFORM,
  NOOP_WINSYS,
  N_NOOPS,
};

#define PROP_NOOP_FIRST (PROP_FWD_FIRST + N_FWDS)

static GParamSpec *fwd_specs[N_FWDS];
static GParamSpec *noop_specs[N_NOOPS];

struct _GstRdsVideoSink
{
  GstBin parent;

  GstElement *convert;          /* rdsvideoconvert */
  GstElement *filter;           /* BGRx system memory */
  GstElement *videoconvert;
  GstElement *sink;             /* the inner sink (owned by the bin); NULL if video-sink could not be made */

  /* properties (object lock) */
  gchar *video_sink;
  GValue fwds[N_FWDS];
  GValue noops[N_NOOPS];
  guint noops_warned;           /* bit per NOOP_* */

  gint child_error;             /* atomic: a child posted an error during the current state change */
};

/* the formats rdsvideoconvert takes, memory:RDS or system memory */
#define RDS_VIDEO_SINK_FIELDS \
  "format = (string) { NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR, GRAY8 }, width = (int) [ 1, 16384 ], " \
  "height = (int) [ 1, 16384 ], framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), " RDS_VIDEO_SINK_FIELDS "; video/x-raw, "
        RDS_VIDEO_SINK_FIELDS));

G_DEFINE_TYPE (GstRdsVideoSink, gst_rds_video_sink, GST_TYPE_BIN);
GST_ELEMENT_REGISTER_DEFINE (rdsvideosink, "rdsvideosink", GST_RANK_NONE, GST_TYPE_RDS_VIDEO_SINK);

/* ---- forwarding ---- */

/* Sets @e's property named like @ours to @v, converted to its type, when @e has it writable. */
static void
forward_one (GstElement * e, GParamSpec * ours, const GValue * v)
{
  GParamSpec *theirs = g_object_class_find_property (G_OBJECT_GET_CLASS (e), ours->name);
  GValue t = G_VALUE_INIT;

  if (theirs == NULL || !(theirs->flags & G_PARAM_WRITABLE) || (theirs->flags & G_PARAM_CONSTRUCT_ONLY))
    return;
  g_value_init (&t, G_PARAM_SPEC_VALUE_TYPE (theirs));
  /* a value out of their range is not forwarded */
  if (g_value_type_transformable (G_VALUE_TYPE (v), G_VALUE_TYPE (&t)) && g_value_transform (v, &t)
      && !g_param_value_validate (theirs, &t)) {
    GST_DEBUG_OBJECT (e, "forwarding %s", ours->name);
    g_object_set_property (G_OBJECT (e), ours->name, &t);
  }
  g_value_unset (&t);
}

/* Forwards property @i (-1: all of them) to @e and, when @e is a bin, to the sinks inside it. */
static void
forward_tree (GstRdsVideoSink * self, GstElement * e, gint i)
{
  GValue vals[N_FWDS] = { G_VALUE_INIT };
  GstIterator *it;
  GValue item = G_VALUE_INIT;
  gboolean done = FALSE;

  GST_OBJECT_LOCK (self);
  for (gint k = 0; k < N_FWDS; k++) {
    if (i < 0 || k == i) {
      g_value_init (&vals[k], G_VALUE_TYPE (&self->fwds[k]));
      g_value_copy (&self->fwds[k], &vals[k]);
    }
  }
  GST_OBJECT_UNLOCK (self);
  for (gint k = 0; k < N_FWDS; k++) {
    if (G_IS_VALUE (&vals[k])) {
      forward_one (e, fwd_specs[k], &vals[k]);
      g_value_unset (&vals[k]);
    }
  }

  if (!GST_IS_BIN (e))
    return;
  it = gst_bin_iterate_sinks (GST_BIN (e));
  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_OK:
        forward_tree (self, g_value_get_object (&item), i);
        g_value_reset (&item);
        break;
      case GST_ITERATOR_RESYNC:
        gst_iterator_resync (it);
        break;
      default:
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);
}

/* Property @i (-1: all) to the inner sink; gpu-id to the converter as well. */
static void
forward (GstRdsVideoSink * self, gint i)
{
  GstElement *sink;

  GST_OBJECT_LOCK (self);
  sink = self->sink != NULL ? gst_object_ref (self->sink) : NULL;
  GST_OBJECT_UNLOCK (self);
  if (sink != NULL) {
    forward_tree (self, sink, i);
    gst_object_unref (sink);
  }
  if ((i < 0 || i == FWD_GPU_ID) && self->convert != NULL) {
    GValue v = G_VALUE_INIT;

    g_value_init (&v, G_TYPE_UINT);
    GST_OBJECT_LOCK (self);
    g_value_copy (&self->fwds[FWD_GPU_ID], &v);
    GST_OBJECT_UNLOCK (self);
    forward_one (self->convert, fwd_specs[FWD_GPU_ID], &v);
    g_value_unset (&v);
  }
}

/* A sink added inside the inner sink (autovideosink picks its sink at READY) gets the current values. */
static void
gst_rds_video_sink_deep_element_added (GstBin * bin, GstBin * sub_bin, GstElement * child)
{
  GstRdsVideoSink *self = GST_RDS_VIDEO_SINK (bin);

  if (GST_OBJECT_FLAG_IS_SET (child, GST_ELEMENT_FLAG_SINK))
    forward_tree (self, child, -1);
  if (GST_BIN_CLASS (gst_rds_video_sink_parent_class)->deep_element_added != NULL)
    GST_BIN_CLASS (gst_rds_video_sink_parent_class)->deep_element_added (bin, sub_bin, child);
}

/* ---- the inner sink ---- */

static void
set_sink_pointer (GstRdsVideoSink * self, GstElement * sink)
{
  GST_OBJECT_LOCK (self);
  self->sink = sink;
  GST_OBJECT_UNLOCK (self);
}

/* Puts @s in the bin as the inner sink, linked after videoconvert, with the current values, in the bin's state. */
static gboolean
attach_sink (GstRdsVideoSink * self, GstElement * s)
{
  gst_bin_add (GST_BIN (self), s);
  set_sink_pointer (self, s);
  if (!gst_element_link (self->videoconvert, s))
    return FALSE;
  forward (self, -1);
  return gst_element_sync_state_with_parent (s);
}

static void
detach_sink (GstRdsVideoSink * self, GstElement * s)
{
  set_sink_pointer (self, NULL);
  gst_element_set_locked_state (s, TRUE);
  gst_element_set_state (s, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self), s);
}

/* Replaces the inner sink by a new element of factory @name. FALSE: no such element, or it does not link or start;
 * the old sink is back in place. Called with the state lock held (or from init). */
static gboolean
set_inner_sink (GstRdsVideoSink * self, const gchar * name)
{
  GstElement *s, *old = self->sink;

  if (self->videoconvert == NULL || (s = gst_element_factory_make (name, NULL)) == NULL)
    return FALSE;
  if (old != NULL) {
    gst_object_ref (old);
    detach_sink (self, old);
  }
  gst_object_ref (s);
  if (!attach_sink (self, s)) {
    detach_sink (self, s);
    if (old != NULL) {
      gst_element_set_locked_state (old, FALSE);
      if (!attach_sink (self, old))
        GST_WARNING_OBJECT (self, "cannot restore the previous video sink");
      gst_object_unref (old);
    }
    gst_object_unref (s);
    return FALSE;
  }
  gst_object_unref (s);
  if (old != NULL)
    gst_object_unref (old);
  return TRUE;
}

/* ---- streaming ---- */

static guint
frames_in (GstBuffer * buf)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsBufSurface *surf = rds_buffer_get_surface (buf);
  guint n = surf != NULL ? surf->numFilled : 1;

  if (bm != NULL) {
    rds_acquire_meta_lock (bm);
    n = MAX (n, g_list_length (bm->frame_meta_list));
    rds_release_meta_lock (bm);
  }
  return n;
}

/* Batched input is an element error (ours: DeepStream 8.0 accepts the caps, then fails the first buffer without a
 * message and the pipeline hangs). */
static GstPadProbeReturn
on_buffer (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsVideoSink *self = GST_RDS_VIDEO_SINK (user_data);
  guint n = 0;

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    GstBufferList *list = GST_PAD_PROBE_INFO_BUFFER_LIST (info);

    for (guint i = 0; i < gst_buffer_list_length (list); i++)
      n = MAX (n, frames_in (gst_buffer_list_get (list, i)));
  } else {
    n = frames_in (GST_PAD_PROBE_INFO_BUFFER (info));
  }
  if (n <= 1)
    return GST_PAD_PROBE_OK;
  GST_ELEMENT_ERROR (self, STREAM, WRONG_TYPE,
      ("rdsvideosink takes one frame per buffer: add a tiler or a demuxer"), ("%u frames in the buffer", n));
  gst_mini_object_unref (GST_PAD_PROBE_INFO_DATA (info));
  GST_PAD_PROBE_INFO_DATA (info) = NULL;
  GST_PAD_PROBE_INFO_FLOW_RETURN (info) = GST_FLOW_ERROR;
  return GST_PAD_PROBE_HANDLED;
}

static void
gst_rds_video_sink_handle_message (GstBin * bin, GstMessage * message)
{
  GstRdsVideoSink *self = GST_RDS_VIDEO_SINK (bin);

  if (GST_MESSAGE_TYPE (message) == GST_MESSAGE_ERROR)
    g_atomic_int_set (&self->child_error, 1);
  GST_BIN_CLASS (gst_rds_video_sink_parent_class)->handle_message (bin, message);
}

/* The child that did not reach @target: the inner sink when it is one of them (the bin changes the sinks first),
 * else the first one found. NULL: none. */
static GstElement *
child_left_behind (GstRdsVideoSink * self, GstState target)
{
  GstElement *found = NULL;

  GST_OBJECT_LOCK (self);
  if (self->sink != NULL && GST_STATE (self->sink) < target)
    found = gst_object_ref (self->sink);
  for (GList * l = GST_BIN_CHILDREN (self); l != NULL && found == NULL; l = l->next) {
    if (GST_STATE (l->data) < target)
      found = gst_object_ref (l->data);
  }
  GST_OBJECT_UNLOCK (self);
  return found;
}

static GstStateChangeReturn
gst_rds_video_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstRdsVideoSink *self = GST_RDS_VIDEO_SINK (element);
  GstStateChangeReturn ret;
  gchar *name;

  if (transition == GST_STATE_CHANGE_NULL_TO_READY && (self->sink == NULL || self->convert == NULL
          || self->filter == NULL || self->videoconvert == NULL)) {
    GST_OBJECT_LOCK (self);
    name = g_strdup (self->video_sink);
    GST_OBJECT_UNLOCK (self);
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("rdsvideosink is missing an element"),
        ("needs rdsvideoconvert, capsfilter, videoconvert and the video sink '%s'", name));
    g_free (name);
    return GST_STATE_CHANGE_FAILURE;
  }

  g_atomic_int_set (&self->child_error, 0);
  ret = GST_ELEMENT_CLASS (gst_rds_video_sink_parent_class)->change_state (element, transition);
  /* a sink that fails to start without a word still leaves an error on the bus (ours: DeepStream 8.0's
   * nveglglessink fails without a bus message when there is no display) */
  if (ret == GST_STATE_CHANGE_FAILURE && GST_STATE_TRANSITION_NEXT (transition) >
      GST_STATE_TRANSITION_CURRENT (transition) && !g_atomic_int_get (&self->child_error)) {
    GstElement *behind = child_left_behind (self, GST_STATE_TRANSITION_NEXT (transition));

    GST_OBJECT_LOCK (self);
    name = g_strdup (self->video_sink);
    GST_OBJECT_UNLOCK (self);
    if (behind == NULL || behind == self->sink)
      GST_ELEMENT_ERROR (self, RESOURCE, OPEN_WRITE,
          ("The video sink '%s' could not start: check the display (DISPLAY), or choose another sink with the "
              "video-sink property", name), ("%s failed without an error message",
              gst_state_change_get_name (transition)));
    else
      GST_ELEMENT_ERROR (self, CORE, STATE_CHANGE, ("An element inside rdsvideosink failed %s",
              gst_state_change_get_name (transition)), ("%s failed without an error message",
              GST_OBJECT_NAME (behind)));
    g_free (name);
    if (behind != NULL)
      gst_object_unref (behind);
  }
  return ret;
}

/* ---- properties ---- */

/* Properties marked "changeable only in NULL or READY": refuse changes once streaming (object lock held). */
static gboolean
check_mutable (GstRdsVideoSink * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_video_sink_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsVideoSink *self = GST_RDS_VIDEO_SINK (object);
  gboolean ok, warn = FALSE;
  const gchar *name;
  gchar *old;

  if (prop_id == PROP_VIDEO_SINK) {
    /* the state lock keeps the state from changing between the check and the swap */
    GST_STATE_LOCK (self);
    GST_OBJECT_LOCK (self);
    ok = check_mutable (self, pspec);
    GST_OBJECT_UNLOCK (self);
    name = g_value_get_string (value);
    if (name == NULL || *name == '\0')
      name = DEFAULT_VIDEO_SINK;
    if (ok && !set_inner_sink (self, name)) {
      GST_OBJECT_LOCK (self);
      old = g_strdup (self->video_sink);
      GST_OBJECT_UNLOCK (self);
      g_warning ("%s: no element '%s', or it does not link or start; keeping the video sink '%s'",
          GST_OBJECT_NAME (self), name, old);
      g_free (old);
      ok = FALSE;
    }
    if (ok) {
      GST_OBJECT_LOCK (self);
      g_free (self->video_sink);
      self->video_sink = g_strdup (name);
      GST_OBJECT_UNLOCK (self);
    }
    GST_STATE_UNLOCK (self);
    return;
  }

  if (prop_id >= PROP_FWD_FIRST && prop_id < PROP_FWD_FIRST + N_FWDS) {
    guint i = prop_id - PROP_FWD_FIRST;

    GST_OBJECT_LOCK (self);
    ok = !(pspec->flags & GST_PARAM_MUTABLE_READY) || check_mutable (self, pspec);
    if (ok)
      g_value_copy (value, &self->fwds[i]);
    GST_OBJECT_UNLOCK (self);
    if (ok)
      forward (self, (gint) i);
    return;
  }

  if (prop_id >= PROP_NOOP_FIRST && prop_id < PROP_NOOP_FIRST + N_NOOPS) {
    guint i = prop_id - PROP_NOOP_FIRST;

    GST_OBJECT_LOCK (self);
    g_value_copy (value, &self->noops[i]);
    if (!g_param_value_defaults (pspec, value) && !(self->noops_warned & (1u << i))) {
      self->noops_warned |= 1u << i;
      warn = TRUE;
    }
    GST_OBJECT_UNLOCK (self);
    if (warn)
      g_warning ("%s: %s is not implemented; the property is accepted and has no effect", GST_OBJECT_NAME (self),
          pspec->name);
    return;
  }

  G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
gst_rds_video_sink_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsVideoSink *self = GST_RDS_VIDEO_SINK (object);

  GST_OBJECT_LOCK (self);
  if (prop_id == PROP_VIDEO_SINK)
    g_value_set_string (value, self->video_sink);
  else if (prop_id >= PROP_FWD_FIRST && prop_id < PROP_FWD_FIRST + N_FWDS)
    g_value_copy (&self->fwds[prop_id - PROP_FWD_FIRST], value);
  else if (prop_id >= PROP_NOOP_FIRST && prop_id < PROP_NOOP_FIRST + N_NOOPS)
    g_value_copy (&self->noops[prop_id - PROP_NOOP_FIRST], value);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_video_sink_finalize (GObject * object)
{
  GstRdsVideoSink *self = GST_RDS_VIDEO_SINK (object);

  g_free (self->video_sink);
  for (guint i = 0; i < N_FWDS; i++)
    g_value_unset (&self->fwds[i]);
  for (guint i = 0; i < N_NOOPS; i++)
    g_value_unset (&self->noops[i]);
  G_OBJECT_CLASS (gst_rds_video_sink_parent_class)->finalize (object);
}

/* Releases a new element that was never added to the bin. */
static void
drop_floating (GstElement ** e)
{
  if (*e != NULL)
    gst_object_unref (gst_object_ref_sink (*e));
  *e = NULL;
}

static void
gst_rds_video_sink_init (GstRdsVideoSink * self)
{
  GstBin *bin = GST_BIN (self);
  GstPad *target, *ghost;
  GstCaps *caps;

  for (guint i = 0; i < N_FWDS; i++) {
    g_value_init (&self->fwds[i], G_PARAM_SPEC_VALUE_TYPE (fwd_specs[i]));
    g_param_value_set_default (fwd_specs[i], &self->fwds[i]);
  }
  for (guint i = 0; i < N_NOOPS; i++) {
    g_value_init (&self->noops[i], G_PARAM_SPEC_VALUE_TYPE (noop_specs[i]));
    g_param_value_set_default (noop_specs[i], &self->noops[i]);
  }
  self->video_sink = g_strdup (DEFAULT_VIDEO_SINK);

  /* a missing element fails the NULL to READY change with an error */
  self->convert = gst_element_factory_make ("rdsvideoconvert", "convert");
  self->filter = gst_element_factory_make ("capsfilter", "filter");
  self->videoconvert = gst_element_factory_make ("videoconvert", "videoconvert");
  if (self->convert != NULL && self->filter != NULL && self->videoconvert != NULL) {
    caps = gst_caps_from_string ("video/x-raw, format = (string) BGRx");
    g_object_set (self->filter, "caps", caps, NULL);
    gst_caps_unref (caps);
    gst_bin_add_many (bin, self->convert, self->filter, self->videoconvert, NULL);
    if (!gst_element_link_many (self->convert, self->filter, self->videoconvert, NULL))
      g_warning ("%s: cannot link rdsvideoconvert, capsfilter and videoconvert", GST_OBJECT_NAME (self));
    target = gst_element_get_static_pad (self->convert, "sink");
  } else {
    drop_floating (&self->convert);
    drop_floating (&self->filter);
    drop_floating (&self->videoconvert);
    target = NULL;
  }
  ghost = gst_ghost_pad_new_no_target_from_template ("sink",
      gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (self), "sink"));
  if (target != NULL) {
    gst_ghost_pad_set_target (GST_GHOST_PAD (ghost), target);
    gst_object_unref (target);
  }
  gst_pad_add_probe (ghost, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST, on_buffer, self, NULL);
  gst_element_add_pad (GST_ELEMENT (self), ghost);

  if (self->videoconvert != NULL && !set_inner_sink (self, DEFAULT_VIDEO_SINK))
    GST_WARNING_OBJECT (self, "no element '%s': set video-sink", DEFAULT_VIDEO_SINK);
  GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_SINK);
}

static void
gst_rds_video_sink_class_init (GstRdsVideoSinkClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBinClass *bin_class = GST_BIN_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  GST_DEBUG_CATEGORY_INIT (gst_rds_video_sink_debug, "rdsvideosink", 0, "Redstream video sink");

  gobject_class->set_property = gst_rds_video_sink_set_property;
  gobject_class->get_property = gst_rds_video_sink_get_property;
  gobject_class->finalize = gst_rds_video_sink_finalize;

  g_object_class_install_property (gobject_class, PROP_VIDEO_SINK,
      g_param_spec_string ("video-sink", "Video sink",
          "Factory name of the GStreamer sink that shows the frames (NULL or empty: autovideosink; ours: a factory "
          "name, not an element instance)",
          DEFAULT_VIDEO_SINK, flags | GST_PARAM_MUTABLE_READY));

  /* names, ranges and defaults as gst-inspect shows them for DeepStream 8.0's nveglglessink */
#define FWD_DESC(what) what " (forwarded to the video sink when it has this property)"
  fwd_specs[FWD_SYNC] = g_param_spec_boolean ("sync", "Sync", FWD_DESC ("Sync on the clock"), TRUE, flags);
  fwd_specs[FWD_QOS] = g_param_spec_boolean ("qos", "QoS",
      FWD_DESC ("Generate Quality-of-Service events upstream"), TRUE, flags);
  fwd_specs[FWD_MAX_LATENESS] = g_param_spec_int64 ("max-lateness", "Max lateness",
      FWD_DESC ("Maximum number of nanoseconds that a buffer can be late before it is dropped (-1 unlimited)"), -1,
      G_MAXINT64, 5 * GST_MSECOND, flags);
  fwd_specs[FWD_PROCESSING_DEADLINE] = g_param_spec_uint64 ("processing-deadline", "Processing deadline",
      FWD_DESC ("Maximum processing time for a buffer in nanoseconds"), 0, G_MAXUINT64, 15 * GST_MSECOND, flags);
  fwd_specs[FWD_ASYNC] = g_param_spec_boolean ("async", "Async", FWD_DESC ("Go asynchronously to PAUSED"), TRUE,
      flags);
  fwd_specs[FWD_WINDOW_X] = g_param_spec_uint ("window-x", "Window x", FWD_DESC ("X coordinate of the window"), 0,
      G_MAXINT, 10, flags);
  fwd_specs[FWD_WINDOW_Y] = g_param_spec_uint ("window-y", "Window y", FWD_DESC ("Y coordinate of the window"), 0,
      G_MAXINT, 10, flags);
  fwd_specs[FWD_WINDOW_WIDTH] = g_param_spec_uint ("window-width", "Window width",
      FWD_DESC ("Width of the window (0: the video's)"), 0, G_MAXINT, 0, flags);
  fwd_specs[FWD_WINDOW_HEIGHT] = g_param_spec_uint ("window-height", "Window height",
      FWD_DESC ("Height of the window (0: the video's)"), 0, G_MAXINT, 0, flags);
  fwd_specs[FWD_FORCE_ASPECT_RATIO] = g_param_spec_boolean ("force-aspect-ratio", "Force aspect ratio",
      FWD_DESC ("Keep the frame's aspect ratio when scaling"), TRUE, flags);
  fwd_specs[FWD_CREATE_WINDOW] = g_param_spec_boolean ("create-window", "Create window",
      FWD_DESC ("Create a window when the application provides none"), TRUE, flags);
  fwd_specs[FWD_GPU_ID] = g_param_spec_uint ("gpu-id", "GPU id",
      FWD_DESC ("GPU that copies the frames to system memory"), 0, G_MAXUINT, 0, flags | GST_PARAM_MUTABLE_READY);
  fwd_specs[FWD_BLOCKSIZE] = g_param_spec_uint ("blocksize", "Block size",
      FWD_DESC ("Size in bytes to pull per buffer (0 = default)"), 0, G_MAXUINT, 4096, flags);
  fwd_specs[FWD_ENABLE_LAST_SAMPLE] = g_param_spec_boolean ("enable-last-sample", "Enable last sample",
      FWD_DESC ("Keep the last sample in the video sink"), TRUE, flags);
  fwd_specs[FWD_MAX_BITRATE] = g_param_spec_uint64 ("max-bitrate", "Max bitrate",
      FWD_DESC ("The maximum bits per second to render (0 = disabled)"), 0, G_MAXUINT64, 0, flags);
  fwd_specs[FWD_RENDER_DELAY] = g_param_spec_uint64 ("render-delay", "Render delay",
      FWD_DESC ("Additional render delay of the sink in nanoseconds"), 0, G_MAXUINT64, 0, flags);
  fwd_specs[FWD_SHOW_PREROLL_FRAME] = g_param_spec_boolean ("show-preroll-frame", "Show preroll frame",
      FWD_DESC ("Whether to render video frames during preroll"), TRUE, flags);
  fwd_specs[FWD_THROTTLE_TIME] = g_param_spec_uint64 ("throttle-time", "Throttle time",
      FWD_DESC ("The time to keep between rendered buffers (0 = disabled)"), 0, G_MAXUINT64, 0, flags);
  fwd_specs[FWD_TS_OFFSET] = g_param_spec_int64 ("ts-offset", "Timestamp offset",
      FWD_DESC ("Timestamp offset in nanoseconds"), G_MININT64, G_MAXINT64, 0, flags);
#undef FWD_DESC
  for (guint i = 0; i < N_FWDS; i++)
    g_object_class_install_property (gobject_class, PROP_FWD_FIRST + i, fwd_specs[i]);

#define NOOP_DESC "Accepted for compatibility; has no effect"
  noop_specs[NOOP_ROWS] = g_param_spec_uint ("rows", "Rows", NOOP_DESC, 1, G_MAXINT, 1, flags);
  noop_specs[NOOP_COLUMNS] = g_param_spec_uint ("columns", "Columns", NOOP_DESC, 1, G_MAXINT, 1, flags);
  noop_specs[NOOP_PROFILE] = g_param_spec_uint ("profile", "Profile", NOOP_DESC, 0, G_MAXUINT, 0, flags);
  noop_specs[NOOP_DISPLAY] = g_param_spec_pointer ("display", "Display", NOOP_DESC, flags);
  noop_specs[NOOP_DO_TRANSFORM] = g_param_spec_boolean ("do-transform", "Do transform", NOOP_DESC, TRUE, flags);
  /* not in DeepStream 8.0's dGPU gst-inspect (Jetson's nveglglessink has it): the string type and default are ours */
  noop_specs[NOOP_WINSYS] = g_param_spec_string ("winsys", "Window system", NOOP_DESC " (x11 or wayland)", "x11",
      flags);
#undef NOOP_DESC
  for (guint i = 0; i < N_NOOPS; i++)
    g_object_class_install_property (gobject_class, PROP_NOOP_FIRST + i, noop_specs[i]);

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_set_static_metadata (element_class, "Redstream video sink", "Sink/Video",
      "Shows memory:RDS frames with a stock GStreamer video sink", "Redstream contributors");

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_rds_video_sink_change_state);
  bin_class->handle_message = GST_DEBUG_FUNCPTR (gst_rds_video_sink_handle_message);
  bin_class->deep_element_added = GST_DEBUG_FUNCPTR (gst_rds_video_sink_deep_element_added);
}
