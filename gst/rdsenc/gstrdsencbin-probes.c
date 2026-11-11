/* Redstream: GstRdsEncBin's pad probes: the preferred profile and the level warning on the VA encoder's caps, the
 * input size check on the bin's sink pad; filler stripping, the H.265 conformance window, the input's timestamps
 * (on buffers, segments and force-key-unit events), DELTA_UNIT, the framerate in the caps, batch refusal and
 * copy-meta on its stream. Locking: gstrdsencbin-private.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsencbin-private.h"

#include <rds/rds.h>
#include <gst/video/video.h>

#include "rds-enc-caps.h"
#include "rds-enc-props.h"
#include "rds-enc-nal.h"

#define GST_CAT_DEFAULT gst_rds_enc_bin_debug

/* ---- the preferred profile ---- */

/* The answer to the VA encoder's downstream CAPS queries, decided by rds_enc_caps_narrow: narrowed to the `profile`
 * property's profile when downstream allows it (downstream caps that fix another profile win: DeepStream 8.0
 * behaviour), and with B frames requested, Baseline left out when there is a choice. When only Baseline is left and
 * B frames were requested (H.264), the VA encoder's b-frames is set to 0 with one warning per run (ours: VA would
 * write B slices into a Baseline stream).
 *
 * Why setting b-frames here works, and what to re-check on a GStreamer update (written against gst-plugins-bad 1.28's
 * va plugin): VA's reconfiguration first decides the profile, asking downstream for its allowed caps (this query),
 * then resets its state, copying the b-frames *property* into the GOP settings, then builds the GOP structure. VA has
 * a Baseline check of its own that drops B frames, but it runs during the profile decision, before the reset, so the
 * reset undoes it. Our property change, made inside the same query, is what the reset copies. If VA reorders these
 * steps, /gpu/enc/b-frames-h264 (Baseline from the property and from downstream caps: no B slices) fails. */
static GstPadProbeReturn
caps_query_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstRdsEncBinPrivate *priv = PRIV (self);
  const GstRdsEncCodecInfo *codec = GST_RDS_ENC_BIN_GET_CLASS (self)->info;
  GstQuery *q = GST_PAD_PROBE_INFO_QUERY (info);
  GstCaps *res, *narrowed;
  const gchar *pref;
  guint requested, b_frames, b_set;
  gboolean only_baseline = FALSE, warn = FALSE;

  if (GST_QUERY_TYPE (q) != GST_QUERY_CAPS || !(GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_PULL))
    return GST_PAD_PROBE_OK;
  gst_query_parse_caps_result (q, &res);
  if (res == NULL || gst_caps_is_empty (res) || gst_caps_is_any (res))
    return GST_PAD_PROBE_OK;

  GST_OBJECT_LOCK (self);
  pref = priv->va_profile;      /* a static string */
  requested = priv->b_frames;
  GST_OBJECT_UNLOCK (self);
  if (pref == NULL)             /* not started */
    return GST_PAD_PROBE_OK;

  narrowed = rds_enc_caps_narrow (res, codec->caps_name, pref, requested, &only_baseline);
  only_baseline = only_baseline && requested > 0;
  b_frames = rds_enc_b_frames (requested, only_baseline, NULL);

  GST_DEBUG_OBJECT (self, "downstream caps %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT ", b-frames %u", res, narrowed,
      b_frames);
  gst_query_set_caps_result (q, narrowed);
  gst_caps_unref (narrowed);

  GST_OBJECT_LOCK (self);
  b_set = priv->b_frames_set;
  priv->b_frames_set = b_frames;
  if (only_baseline && !priv->b_frames_warned) {
    priv->b_frames_warned = TRUE;
    warn = TRUE;
  }
  GST_OBJECT_UNLOCK (self);

  if (warn)
    g_warning ("%s: num-B-Frames=%u with a Baseline profile: Baseline has no B frames, so none are encoded",
        GST_OBJECT_NAME (self), requested);
  if (b_frames != b_set) {
    GstElement *encoder = gst_pad_get_parent_element (pad);

    if (encoder != NULL) {
      gst_rds_enc_bin_set_va_uint (G_OBJECT (encoder), "b-frames", b_frames);
      gst_object_unref (encoder);
    }
  }
  return GST_PAD_PROBE_OK;
}

/* The VA encoder's output caps. VA computes the level from the size, frame rate and bitrate and writes it in its caps
 * and the stream; a `level` that downstream caps fix to another value makes the caps refused (not-negotiated).
 * DeepStream 8.0 writes the downstream level instead; here it is a warning, once per run, naming both (ours). */
static GstPadProbeReturn
caps_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstRdsEncBinPrivate *priv = PRIV (self);
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  GstCaps *caps, *allowed;
  const gchar *level;
  gchar *want = NULL;
  gboolean warn, refused;

  if (GST_EVENT_TYPE (ev) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  gst_event_parse_caps (ev, &caps);
  level = gst_structure_get_string (gst_caps_get_structure (caps, 0), "level");
  if (level == NULL)
    return GST_PAD_PROBE_OK;
  /* What downstream allows (the src capsfilter's accept-caps would only check its own caps). This query comes back
   * through caps_query_probe on this pad, which recomputes the same b-frames as during VA's profile decision, so it
   * sets nothing; were it ever to set b-frames here, that would only apply at VA's next reconfiguration. */
  allowed = gst_pad_peer_query_caps (pad, NULL);
  refused = rds_enc_caps_level_refused (caps, allowed, &want);
  gst_caps_unref (allowed);
  if (!refused)                 /* accepted, or refused for more than the level */
    return GST_PAD_PROBE_OK;

  GST_OBJECT_LOCK (self);
  warn = !priv->level_warned;
  priv->level_warned = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (warn)
    g_warning ("%s: downstream caps ask for level %s, but the VA encoder sets the level itself (%s for this size, "
        "frame rate and bitrate): drop the level from the caps after the encoder", GST_OBJECT_NAME (self), want,
        level);
  g_free (want);
  return GST_PAD_PROBE_OK;
}

/* ---- the input size ---- */

/* The sizes the VA encoder takes on system memory (its sink template's range: on this driver H.264 128..4096 on each
 * side, H.265 width 130..8192 and height 128..4352), as the bin's sink pad (a ghost of rdsvideoconvert's) answers
 * for them. Without this rdsvideoconvert, which takes 1..16384, would scale the frames to a size VA takes.
 * - CAPS queries: the answer is limited to that range, so upstream picks a size VA takes (the pad template stays
 *   rdsvideoconvert's).
 * - Accept-caps: a size outside is refused, silently. GStreamer checks every CAPS event with an accept-caps query on
 *   the pad it reaches, so caps outside, at the start or mid-stream, are not-negotiated, as with VA alone.
 * - The warning naming the range, once per run (ours: VA posts nothing), comes when negotiation really fails: at a
 *   CAPS event outside the range (event probes run before that accept-caps check), or at a CAPS query whose answer
 *   the limit leaves empty (upstream caps that fix a size outside, as a capsfilter does: no CAPS event follows).
 * Sizes inside, odd ones included, are accepted; VA codes an odd size as the next even one (DeepStream 8.0
 * behaviour). Accept-caps and CAPS events are judged on fixed caps only, by the first structure's width and height;
 * caps features are ignored on purpose, since rdsvideoconvert turns every memory it takes into VA's system memory. */

typedef struct
{
  gint min_w, max_w, min_h, max_h;
} SizeRange;

/* The range, or FALSE when none is known (any size is left to VA). */
static gboolean
get_size_range (GstRdsEncBin * self, SizeRange * r)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  gboolean known;

  GST_OBJECT_LOCK (self);
  known = priv->have_size_range;
  *r = (SizeRange) { priv->min_width, priv->max_width, priv->min_height, priv->max_height };
  GST_OBJECT_UNLOCK (self);
  return known;
}

/* Fixed @caps whose (first structure's) size is outside the range; *@w x *@h is that size. */
static gboolean
size_outside (GstRdsEncBin * self, const GstCaps * caps, gint * w, gint * h, SizeRange * r)
{
  const GstStructure *st;

  if (caps == NULL || !gst_caps_is_fixed (caps) || !get_size_range (self, r))
    return FALSE;
  st = gst_caps_get_structure (caps, 0);
  if (!gst_structure_get_int (st, "width", w) || !gst_structure_get_int (st, "height", h))
    return FALSE;
  return *w < r->min_w || *w > r->max_w || *h < r->min_h || *h > r->max_h;
}

/* The warning, the first time in a run: @what is the size refused. */
static void
warn_size (GstRdsEncBin * self, const gchar * what, const SizeRange * r)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  gboolean warn;

  GST_OBJECT_LOCK (self);
  warn = !priv->size_warned;
  priv->size_warned = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (warn)
    g_warning ("%s: %s is outside the sizes the VA encoder takes for %s (width %d to %d, height %d to %d): not "
        "negotiated", GST_OBJECT_NAME (self), what, GST_RDS_ENC_BIN_GET_CLASS (self)->info->codec_name, r->min_w,
        r->max_w, r->min_h, r->max_h);
}

/* CAPS events into the bin: the warning (sink_probe). */
static void
check_caps_event (GstRdsEncBin * self, GstEvent * ev)
{
  GstCaps *caps;
  SizeRange r;
  gint w, h;
  gchar *what;

  gst_event_parse_caps (ev, &caps);
  if (!size_outside (self, caps, &w, &h, &r))
    return;
  what = g_strdup_printf ("%dx%d", w, h);
  warn_size (self, what, &r);
  g_free (what);
}

/* Queries on the bin's sink pad: accept-caps refused outside the range (before rdsvideoconvert answers), CAPS
 * answers limited to it (after). */
static GstPadProbeReturn
sink_query_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstQuery *q = GST_PAD_PROBE_INFO_QUERY (info);
  gboolean pull = (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_PULL) != 0;
  GstCaps *caps, *limited, *filter;
  SizeRange r;
  gint w, h;
  gchar *what;

  if (GST_QUERY_TYPE (q) == GST_QUERY_ACCEPT_CAPS && !pull) {
    gst_query_parse_accept_caps (q, &caps);
    if (!size_outside (self, caps, &w, &h, &r))
      return GST_PAD_PROBE_OK;
    GST_DEBUG_OBJECT (self, "refusing %dx%d: outside %d..%d x %d..%d", w, h, r.min_w, r.max_w, r.min_h, r.max_h);
    gst_query_set_accept_caps_result (q, FALSE);
    return GST_PAD_PROBE_HANDLED;
  }
  if (GST_QUERY_TYPE (q) == GST_QUERY_CAPS && pull) {
    gst_query_parse_caps_result (q, &caps);
    if (caps == NULL || !get_size_range (self, &r))
      return GST_PAD_PROBE_OK;
    limited = rds_enc_caps_limit_size (caps, r.min_w, r.max_w, r.min_h, r.max_h);
    GST_LOG_OBJECT (self, "caps %" GST_PTR_FORMAT " limited to %" GST_PTR_FORMAT, caps, limited);
    if (gst_caps_is_empty (limited) && !gst_caps_is_empty (caps)) {
      /* only sizes outside were asked for: name them from the filter when it fixes one */
      gst_query_parse_caps (q, &filter);
      if (filter != NULL && gst_caps_get_size (filter) > 0
          && gst_structure_get_int (gst_caps_get_structure (filter, 0), "width", &w)
          && gst_structure_get_int (gst_caps_get_structure (filter, 0), "height", &h))
        what = g_strdup_printf ("%dx%d", w, h);
      else
        what = g_strdup ("the size asked for");
      warn_size (self, what, &r);
      g_free (what);
    }
    gst_query_set_caps_result (q, limited);
    gst_caps_unref (limited);
  }
  return GST_PAD_PROBE_OK;
}

/* ---- the stream probes ---- */

static void
meta_record_free (gpointer p)
{
  MetaRecord *r = p;

  gst_buffer_unref (r->metas);
  g_free (r);
}

/* probe_lock held: takes every record out, to be freed with free_records once the lock is dropped (freeing a record
 * frees its metas). */
static GList *
steal_records (GstRdsEncBinPrivate * priv)
{
  GList *l = priv->records.head;

  g_queue_init (&priv->records);
  return l;
}

static void
free_records (GList * l)
{
  g_list_free_full (l, meta_record_free);
}

/* Drops every record (a flush, EOS). */
static void
clear_records (GstRdsEncBinPrivate * priv)
{
  GList *l;

  g_mutex_lock (&priv->probe_lock);
  l = steal_records (priv);
  g_mutex_unlock (&priv->probe_lock);
  free_records (l);
}

/* probe_lock held */
static void
clear_segments (GstRdsEncBinPrivate * priv)
{
  g_queue_clear_full (&priv->in_segments, (GDestroyNotify) gst_segment_free);
  gst_segment_init (&priv->last_in_segment, GST_FORMAT_UNDEFINED);
  priv->out_seqnum = GST_SEQNUM_INVALID;
}

/* Clears the probes' state, for a new run (READY to PAUSED, PAUSED to READY). */
void
gst_rds_enc_bin_reset_probes (GstRdsEncBin * self, gboolean copy_meta)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  GList *old;

  g_mutex_lock (&priv->probe_lock);
  priv->copy_meta = copy_meta;
  old = steal_records (priv);
  clear_segments (priv);
  priv->va_shift = 0;
  priv->shift = 0;
  priv->in_size = (VideoSize) { 0 };
  priv->in_since = GST_CLOCK_TIME_NONE;
  priv->out_size = (VideoSize) { 0 };
  gst_clear_caps (&priv->out_caps);
  priv->hevc_extra_bits = 0;
  g_mutex_unlock (&priv->probe_lock);
  free_records (old);
}

static VideoSize
video_size (const GstCaps * caps)
{
  const GstStructure *st = gst_caps_get_structure (caps, 0);
  VideoSize v = { 0 };

  (void) gst_structure_get_int (st, "width", &v.width);
  (void) gst_structure_get_int (st, "height", &v.height);
  if (!gst_structure_get_fraction (st, "framerate", &v.fps_n, &v.fps_d))
    v.fps_n = v.fps_d = 0;
  return v;
}

/* The frames in @buf by its batch meta (1 without one). */
static guint
batch_frames (GstBuffer * buf)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  guint n;

  if (bm == NULL)
    return 1;
  rds_acquire_meta_lock (bm);
  n = bm->num_frames_in_batch;
  rds_release_meta_lock (bm);
  return n;
}

/* copy-meta: a record of @buf's metas, copied as gst_buffer_copy_into copies them (each meta's transform with the copy
 * quark, so RdsBatchMeta is deep-copied under its own lock; metas tied to the memory are left out); NULL without a
 * PTS. Called without probe_lock. */
static MetaRecord *
new_record (GstBuffer * buf)
{
  MetaRecord *r;

  if (!GST_BUFFER_PTS_IS_VALID (buf))
    return NULL;
  r = g_new (MetaRecord, 1);
  r->pts = GST_BUFFER_PTS (buf);
  r->metas = gst_buffer_new ();
  gst_buffer_copy_into (r->metas, buf, GST_BUFFER_COPY_META, 0, (gsize) - 1);
  return r;
}

/* Keeps @r, dropping records more than MAX_RECORD_AGE frames older than it (freed after the lock is dropped). */
static void
add_record (GstRdsEncBinPrivate * priv, MetaRecord * r)
{
  GList *evicted = NULL;

  g_mutex_lock (&priv->probe_lock);
  g_queue_push_tail (&priv->records, r);
  while (g_queue_get_length (&priv->records) > MAX_RECORD_AGE + 1)
    evicted = g_list_prepend (evicted, g_queue_pop_head (&priv->records));
  g_mutex_unlock (&priv->probe_lock);
  free_records (evicted);
}

/* The bin's input, before the convert: a batch of more than one frame is an element error (ours: DeepStream 8.0
 * encodes the first frame only, silently). With copy-meta, the input metas are recorded here, on the original
 * buffer: the VA encoder sees the converted one. A flush drops the records. A CAPS event of a size VA doesn't take
 * gives the size warning (see the input size). */
static GstPadProbeReturn
sink_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstRdsEncBinPrivate *priv = PRIV (self);
  GstBufferList *list = NULL;
  GstBuffer *buf = NULL;
  guint n = 0, len;
  gboolean copy_meta;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
    if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_FLUSH_STOP)
      clear_records (priv);
    else if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_CAPS)
      check_caps_event (self, GST_PAD_PROBE_INFO_EVENT (info));
    return GST_PAD_PROBE_OK;
  }
  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
    list = GST_PAD_PROBE_INFO_BUFFER_LIST (info);
    len = gst_buffer_list_length (list);
  } else {
    buf = GST_PAD_PROBE_INFO_BUFFER (info);
    len = 1;
  }
  for (guint i = 0; i < len; i++)
    n = MAX (n, batch_frames (list != NULL ? gst_buffer_list_get (list, i) : buf));
  if (n > 1) {
    GST_ELEMENT_ERROR (self, STREAM, WRONG_TYPE, ("%s takes one frame per buffer: add a tiler or a demuxer "
            "before it", GST_RDS_ENC_BIN_GET_CLASS (self)->info->element), ("%u frames in the buffer", n));
    gst_mini_object_unref (GST_PAD_PROBE_INFO_DATA (info));
    GST_PAD_PROBE_INFO_DATA (info) = NULL;
    GST_PAD_PROBE_INFO_FLOW_RETURN (info) = GST_FLOW_ERROR;
    return GST_PAD_PROBE_HANDLED;
  }

  g_mutex_lock (&priv->probe_lock);
  copy_meta = priv->copy_meta;
  g_mutex_unlock (&priv->probe_lock);
  if (!copy_meta)
    return GST_PAD_PROBE_OK;
  for (guint i = 0; i < len; i++) {
    MetaRecord *r = new_record (list != NULL ? gst_buffer_list_get (list, i) : buf);

    if (r != NULL)
      add_record (priv, r);
  }
  return GST_PAD_PROBE_OK;
}

/* gst_buffer_foreach_meta: removes the unlocked metas of API @api (a GType as a pointer; NULL: all). */
static gboolean
remove_meta (GstBuffer * buf, GstMeta ** meta, gpointer api)
{
  if (!GST_META_FLAG_IS_SET (*meta, GST_META_FLAG_LOCKED)
      && (api == NULL || (*meta)->info->api == GPOINTER_TO_SIZE (api))) {
    GST_TRACE ("removing %s from %" GST_PTR_FORMAT, g_type_name ((*meta)->info->api), buf);
    *meta = NULL;
  }
  return TRUE;
}

static gboolean
same_size (const VideoSize * a, const VideoSize * b)
{
  return a->width == b->width && a->height == b->height && a->fps_n == b->fps_n && a->fps_d == b->fps_d;
}

/* The VA encoder's input buffers (converted, so the bin's own input is never touched):
 * - the batch meta is removed: VA's base class copies a frame's untagged metas onto its output, which would
 *   deep-copy RdsBatchMeta for every frame, and without copy-meta the output has none (DeepStream 8.0 behaviour);
 *   with copy-meta the output gets the bin's input metas (set_metas);
 * - the PTS of the first frame of new caps, from which output frames have them. */
static GstPadProbeReturn
va_sink_buffer_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBinPrivate *priv = PRIV (GST_RDS_ENC_BIN (user_data));
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);

  if (gst_buffer_get_meta (buf, RDS_META_API_TYPE) != NULL) {
    buf = gst_buffer_make_writable (buf);
    gst_buffer_foreach_meta (buf, remove_meta, GSIZE_TO_POINTER (RDS_META_API_TYPE));
    GST_PAD_PROBE_INFO_DATA (info) = buf;
  }

  g_mutex_lock (&priv->probe_lock);
  if (!GST_CLOCK_TIME_IS_VALID (priv->in_since) && !same_size (&priv->in_size, &priv->out_size))
    priv->in_since = GST_BUFFER_PTS_IS_VALID (buf) ? GST_BUFFER_PTS (buf) : 0;
  g_mutex_unlock (&priv->probe_lock);
  return GST_PAD_PROBE_OK;
}

/* The VA encoder's input events: its segments (for the shift) and caps (the size and frame rate the output gets). */
static GstPadProbeReturn
va_sink_event_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBinPrivate *priv = PRIV (GST_RDS_ENC_BIN (user_data));
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  const GstSegment *seg;
  GstCaps *caps;

  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_SEGMENT:
      gst_event_parse_segment (ev, &seg);
      if (seg->format != GST_FORMAT_TIME)
        break;
      g_mutex_lock (&priv->probe_lock);
      g_queue_push_tail (&priv->in_segments, gst_segment_copy (seg));
      while (g_queue_get_length (&priv->in_segments) > MAX_PENDING_SEGMENTS)
        gst_segment_free (g_queue_pop_head (&priv->in_segments));
      g_mutex_unlock (&priv->probe_lock);
      break;
    case GST_EVENT_CAPS:
      gst_event_parse_caps (ev, &caps);
      g_mutex_lock (&priv->probe_lock);
      priv->in_size = video_size (caps);
      priv->in_since = GST_CLOCK_TIME_NONE;
      g_mutex_unlock (&priv->probe_lock);
      break;
    case GST_EVENT_FLUSH_STOP:
      /* the VA encoder drops the segments it holds, and measures its shift again */
      g_mutex_lock (&priv->probe_lock);
      clear_segments (priv);
      g_mutex_unlock (&priv->probe_lock);
      break;
    default:
      break;
  }
  return GST_PAD_PROBE_OK;
}

/* The VA encoder's output events.
 * - SEGMENT: VA shifts its timestamps and segment so that the first PTS is at least 1000 h (by 1000 h minus the
 *   first PTS after a start or a flush); the shift is measured against the input segment this one comes from.
 *   Without B frames (b-frames as applied for this run is 0) it is removed from the segment and, in the buffer
 *   probe, from every PTS and DTS, so the output has the input's timestamps and segment (DeepStream 8.0 behaviour).
 *   With B frames VA's segment and timestamps stay: removing the shift would put the first DTS before 0 (ours:
 *   DeepStream 8.0 writes DTS = PTS there). A segment with the seqnum of the previous one is that one sent again
 *   (sticky events re-sent): it takes no input segment from the queue.
 * - CAPS: from here on the frames are those of the latest input caps (VA drains the older frames first); a framerate
 *   other than the input's is replaced by the input's (ours: VA keeps the old one after a frame-rate change, and
 *   when its caps don't change otherwise it sends none: then va_src_buffer_probe sends them).
 * - force-key-unit (VA's, before the IDR frame it forced): its timestamp is the frame's PTS, shifted like the
 *   buffers, and the shift is removed from it as from them; its stream time and running time VA computes against its
 *   own (shifted) segment, so they are the input's already.
 * - EOS: every frame is out; the copy-meta records go. */
static GstPadProbeReturn
va_src_event_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstRdsEncBinPrivate *priv = PRIV (self);
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info), *out = NULL;
  const GstSegment *seg;
  GstSegment in, fixed;
  GstSegment *queued;
  GstCaps *caps;
  VideoSize v;
  guint b_frames;
  guint32 seqnum;
  GstClockTime va_shift, shift, ts, stream_time, running_time;
  gboolean again = FALSE, unshift, all_headers;
  guint count;

  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_SEGMENT:
      gst_event_parse_segment (ev, &seg);
      if (seg->format != GST_FORMAT_TIME)
        break;
      GST_OBJECT_LOCK (self);
      b_frames = priv->b_frames_set;
      GST_OBJECT_UNLOCK (self);
      seqnum = gst_event_get_seqnum (ev);
      g_mutex_lock (&priv->probe_lock);
      if (seqnum != GST_SEQNUM_INVALID && seqnum == priv->out_seqnum) {
        /* the same segment again (the pad re-sending its sticky events): no input segment of its own, same shift;
         * rewritten only if it is VA's and not already ours */
        again = TRUE;
        unshift = priv->shift > 0 && seg->start == priv->out_start;
      } else {
        queued = g_queue_pop_head (&priv->in_segments);
        if (queued != NULL) {
          priv->last_in_segment = *queued;
          gst_segment_free (queued);
        }
        in = priv->last_in_segment;
        priv->va_shift = in.format == GST_FORMAT_TIME ? rds_enc_segment_shift (&in, seg) : 0;
        priv->shift = b_frames == 0 ? priv->va_shift : 0;
        priv->out_seqnum = seqnum;
        priv->out_start = seg->start;
        unshift = priv->shift > 0;
      }
      va_shift = priv->va_shift;
      shift = priv->shift;
      g_mutex_unlock (&priv->probe_lock);
      if (unshift) {
        fixed = *seg;
        rds_enc_segment_unshift (&fixed, shift);
        out = gst_event_new_segment (&fixed);
      }
      GST_DEBUG_OBJECT (self, "output segment %" GST_SEGMENT_FORMAT "%s: VA's shift %" GST_TIME_FORMAT ", removed %"
          GST_TIME_FORMAT, seg, again ? " (again)" : "", GST_TIME_ARGS (va_shift), GST_TIME_ARGS (unshift ? shift : 0));
      break;
    case GST_EVENT_CAPS:
      gst_event_parse_caps (ev, &caps);
      g_mutex_lock (&priv->probe_lock);
      priv->out_size = priv->in_size;
      priv->in_since = GST_CLOCK_TIME_NONE;
      v = priv->out_size;
      g_mutex_unlock (&priv->probe_lock);
      if (v.fps_d > 0) {
        const GstStructure *st = gst_caps_get_structure (caps, 0);
        gint n, d;

        if (!gst_structure_get_fraction (st, "framerate", &n, &d) || n != v.fps_n || d != v.fps_d) {
          GstCaps *c = gst_caps_copy (caps);

          gst_caps_set_simple (c, "framerate", GST_TYPE_FRACTION, v.fps_n, v.fps_d, NULL);
          GST_DEBUG_OBJECT (self, "VA's caps %" GST_PTR_FORMAT " get the input's framerate %d/%d", caps, v.fps_n,
              v.fps_d);
          out = gst_event_new_caps (c);
          gst_caps_unref (c);
        }
      }
      if (out != NULL)
        gst_event_parse_caps (out, &caps);
      g_mutex_lock (&priv->probe_lock);
      gst_caps_replace (&priv->out_caps, caps);
      g_mutex_unlock (&priv->probe_lock);
      break;
    case GST_EVENT_CUSTOM_DOWNSTREAM:
      if (!gst_video_event_is_force_key_unit (ev) || !gst_video_event_parse_downstream_force_key_unit (ev, &ts,
              &stream_time, &running_time, &all_headers, &count))
        break;
      g_mutex_lock (&priv->probe_lock);
      shift = priv->shift;
      g_mutex_unlock (&priv->probe_lock);
      if (shift > 0 && GST_CLOCK_TIME_IS_VALID (ts))
        out = gst_video_event_new_downstream_force_key_unit (rds_enc_ts_unshift (ts, shift), stream_time,
            running_time, all_headers, count);
      break;
    case GST_EVENT_EOS:
      clear_records (priv);
      break;
    default:
      break;
  }
  if (out != NULL) {
    gst_event_set_seqnum (out, gst_event_get_seqnum (ev));
    gst_event_unref (ev);
    GST_PAD_PROBE_INFO_DATA (info) = out;
  }
  return GST_PAD_PROBE_OK;
}

/* copy-meta: the output buffer @buf (writable) gets the metas the bin's input buffer of the same frame had, and no
 * other. The record is found by the frame's input PTS @in_pts (the output PTS without VA's shift, so B-frame
 * reordering doesn't matter); with two inputs of the same PTS, the first record matching is used. With no record
 * (an input without PTS) the buffer keeps what VA gave it. Without copy-meta nothing is done: the batch meta was
 * removed before VA (va_sink_buffer_probe). */
static void
set_metas (GstRdsEncBinPrivate * priv, GstBuffer * buf, GstClockTime in_pts)
{
  MetaRecord *rec = NULL;

  if (!GST_CLOCK_TIME_IS_VALID (in_pts))
    return;
  g_mutex_lock (&priv->probe_lock);
  if (priv->copy_meta) {
    for (GList * l = priv->records.head; l != NULL; l = l->next) {
      MetaRecord *r = l->data;

      if (r->pts == in_pts) {
        rec = r;
        g_queue_delete_link (&priv->records, l);
        break;
      }
    }
  }
  g_mutex_unlock (&priv->probe_lock);
  if (rec == NULL)
    return;
  gst_buffer_foreach_meta (buf, remove_meta, NULL);
  gst_buffer_copy_into (buf, rec->metas, GST_BUFFER_COPY_META, 0, (gsize) - 1);
  meta_record_free (rec);
}

/* The VA encoder's output buffers, one access unit each:
 * - filler NAL units stripped (DeepStream 8.0 behaviour: its output has none, so CBR is a ceiling); a buffer of
 *   filler only is dropped;
 * - H.265: the SPS conformance window set from the input size (ours: VA on this Mesa decodes too wide when the width
 *   is not a multiple of 64), on the buffers VA marks as key units (it writes parameter sets only before IDRs);
 * - DELTA_UNIT cleared on intra pictures (IDR and I) and set on the others (DeepStream 8.0 behaviour: VA marks only
 *   its IDRs);
 * - the timestamp shift removed (va_src_event_probe), duration kept;
 * - caps with the new framerate pushed before the first frame of a frame-rate change VA sent no caps for;
 * - with copy-meta, the input's metas (set_metas). Buffer flags stay the encoder's with copy-meta too (ours:
 *   DeepStream 8.0 copies the input's flags, clearing DELTA_UNIT everywhere). */
static GstPadProbeReturn
va_src_buffer_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstRdsEncBinPrivate *priv = PRIV (self);
  RdsEncCodec codec = GST_RDS_ENC_BIN_GET_CLASS (self)->info->codec;
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info), *res;
  GstClockTime shift, va_shift;
  GstCaps *new_caps = NULL;
  VideoSize v;
  guint extra;
  gboolean intra = FALSE, mapped;
  GstMapInfo map;

  res = rds_enc_strip_filler (buf, codec);
  gst_buffer_unref (buf);
  buf = res;
  GST_PAD_PROBE_INFO_DATA (info) = buf;
  if (gst_buffer_get_size (buf) == 0)
    return GST_PAD_PROBE_DROP;

  g_mutex_lock (&priv->probe_lock);
  shift = priv->shift;
  va_shift = priv->va_shift;
  extra = priv->hevc_extra_bits;
  /* the first frame of new input caps that VA sent no caps event for (a frame-rate change only) */
  if (GST_CLOCK_TIME_IS_VALID (priv->in_since) && priv->out_caps != NULL && GST_BUFFER_PTS_IS_VALID (buf)
      && rds_enc_ts_unshift (GST_BUFFER_PTS (buf), va_shift) >= priv->in_since) {
    priv->out_size = priv->in_size;
    priv->in_since = GST_CLOCK_TIME_NONE;
    if (priv->out_size.fps_d > 0) {
      new_caps = gst_caps_copy (priv->out_caps);
      gst_caps_set_simple (new_caps, "framerate", GST_TYPE_FRACTION, priv->out_size.fps_n, priv->out_size.fps_d,
          NULL);
      if (gst_caps_is_equal (new_caps, priv->out_caps))
        gst_clear_caps (&new_caps);
    }
  }
  v = priv->out_size;
  g_mutex_unlock (&priv->probe_lock);
  if (new_caps != NULL) {
    /* Pushed on VA's src pad from inside its own buffer probe: the push runs on this streaming thread, which holds
     * the pad's stream lock (recursive), and the pad's object lock is not held during probes. The event goes through
     * va_src_event_probe (re-entered here, no lock of ours held), which finds the framerate already right. */
    GST_DEBUG_OBJECT (self, "new input framerate %d/%d without new caps from VA: sending %" GST_PTR_FORMAT, v.fps_n,
        v.fps_d, new_caps);
    if (!gst_pad_push_event (pad, gst_event_new_caps (new_caps)))
      GST_DEBUG_OBJECT (self, "downstream refused the caps with the new framerate");
    gst_caps_unref (new_caps);
  }

  if (codec == RDS_ENC_CODEC_H265 && !GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT) && v.width > 0
      && v.height > 0) {
    res = rds_enc_h265_fix_sps_buffer (buf, (guint) v.width, (guint) v.height);
    gst_buffer_unref (buf);
    buf = res;
  }

  mapped = gst_buffer_map (buf, &map, GST_MAP_READ);
  if (mapped) {
    intra = rds_enc_au_is_intra (map.data, map.size, codec, &extra);
    gst_buffer_unmap (buf, &map);
    g_mutex_lock (&priv->probe_lock);
    priv->hevc_extra_bits = extra;
    g_mutex_unlock (&priv->probe_lock);
  }

  buf = gst_buffer_make_writable (buf);
  if (mapped) {
    if (intra)
      GST_BUFFER_FLAG_UNSET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
    else
      GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  }
  set_metas (priv, buf, rds_enc_ts_unshift (GST_BUFFER_PTS (buf), va_shift));
  if (shift > 0) {
    GST_BUFFER_PTS (buf) = rds_enc_ts_unshift (GST_BUFFER_PTS (buf), shift);
    GST_BUFFER_DTS (buf) = rds_enc_ts_unshift (GST_BUFFER_DTS (buf), shift);
  }
  GST_PAD_PROBE_INFO_DATA (info) = buf;
  return GST_PAD_PROBE_OK;
}

/* ---- adding them ---- */

void
gst_rds_enc_bin_add_probes (GstRdsEncBin * self, GstElement * encoder)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  GstPad *pad;

  pad = gst_element_get_static_pad (encoder, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM | GST_PAD_PROBE_TYPE_PULL, caps_query_probe, self,
      NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, caps_event_probe, self, NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, va_src_event_probe, self, NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, va_src_buffer_probe, self, NULL);
  gst_object_unref (pad);
  pad = gst_element_get_static_pad (encoder, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH, va_sink_event_probe,
      self, NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, va_sink_buffer_probe, self, NULL);
  gst_object_unref (pad);
  gst_pad_add_probe (priv->sink_ghost, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST
      | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH, sink_probe, self, NULL);
  gst_pad_add_probe (priv->sink_ghost, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM | GST_PAD_PROBE_TYPE_PUSH
      | GST_PAD_PROBE_TYPE_PULL, sink_query_probe, self, NULL);
}
