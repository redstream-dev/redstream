/* Redstream: rdsstreamdemux, splits batches (memory:RDS with an RdsBatchMeta, as rdsstreammux makes them) into one
 * stream per source. Property and pad names follow DeepStream's public nvstreamdemux documentation.
 *
 * Buffers: a frame of source N goes out on src_N as a read-only surface view of its batch slot (no copy); the
 * batch returns to its pool once every view is released. The view carries the batch's flags, duration and
 * timestamps with PTS = the frame's buf_pts (the source's original PTS) and no DTS, plus a one-frame RdsBatchMeta
 * (a deep copy of the frame, batch_id 0). The first buffer of every stream on a pad (and the first after a flush)
 * is DISCONT. Frames of sources without a requested pad, or whose pad has ended, are dropped.
 *
 * Sticky events, per src pad (also for pads requested while data flows):
 * - STREAM_START: the upstream stream-id plus "/N", and "/N/<k>" for the k-th later stream on the pad (the group id
 *   is kept); a new one for every rds-stream-start (N) and every restart;
 * - CAPS: the sink caps, again whenever they change;
 * - SEGMENT, before the first buffer or EOS: the source's own segment (from rds-stream-segment N) once one came,
 *   else the sink segment. buf_pts is in the source's segment, so running times downstream are those of the
 *   source. A source's segment is forgotten on a flush and on its rds-stream-start, -stream-reset and -pad-deleted:
 *   the source sends a new one before its next frame.
 * Events without a source (GAP and other timed events) are in the mux's output time base, not the sources'.
 *
 * Other downstream events:
 * - source-scoped stream events (rds-pad-added, -pad-deleted, -stream-start, -stream-segment, -stream-eos,
 *   -stream-reset for source N) go to src_N only, and are dropped when it does not exist;
 * - rds-stream-eos (N) ends src_N when per-stream-eos is set; rds-pad-deleted (N) always ends it;
 * - the global EOS ends every pad not ended yet (a pad gets EOS at most once per stream);
 * - after a pad has ended, rds-stream-start / -pad-added / -stream-reset for its source start a new stream on it;
 * - flushes and every other event go to every pad.
 * Upstream events from the src pads: QoS is dropped (it would throttle every source for one branch's lateness), a
 * seek reaches upstream once however many branches send it (same seqnum), everything else goes upstream.
 *
 * Flow: the return of a batch combines the pads' returns (GstFlowCombiner): NOT_LINKED only when every pad is.
 * As with tee, FLUSHING or an error from one branch stops the streaming thread for all: block or unlink a branch
 * before stopping it. Pads may be requested and released while PLAYING, from any thread (also from pad-added
 * handlers).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsstreamdemux.h"

#include <gst/base/gstflowcombiner.h>
#include <rds/rds.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_stream_demux_debug);
#define GST_CAT_DEFAULT gst_rds_stream_demux_debug

#define DEFAULT_PER_STREAM_EOS FALSE

enum
{
  PROP_0,
  PROP_PER_STREAM_EOS,
};

/* One requested src pad. Refcounted: the chain function keeps it alive while it pushes, even if the pad is released
 * meanwhile. Fields other than ref, pad and id are protected by the element's lock. */
typedef struct
{
  gint ref;
  GstPad *pad;
  guint id;
  gboolean registered;          /* FALSE once released: its flow returns no longer count */
  guint generation;             /* streams started on this pad: suffix of the next stream-id */
  gboolean need_discont;
  gboolean need_stream_start;
  gboolean need_caps;
  gboolean need_segment;
  gboolean eos;                 /* EOS went out (or is going out) on this pad */
} SrcPad;

struct _GstRdsStreamDemux
{
  GstElement parent;

  GstPad *sinkpad;

  GMutex lock;                  /* everything below */
  GHashTable *pads;             /* source id -> SrcPad */
  GHashTable *reserved;         /* source ids of pads being requested (set) */
  GHashTable *segments;         /* source id -> GstSegment, from rds-stream-segment */
  GstFlowCombiner *flow;
  gchar *stream_id;             /* upstream */
  gboolean have_group_id;
  guint group_id;
  GstCaps *caps;
  GstSegment segment;
  gboolean have_segment;
  gboolean per_stream_eos;
  gboolean have_seek_seqnum;
  guint32 seek_seqnum;          /* of the last seek sent upstream */
  guint64 dropped;              /* frames without a pad (debug output) */
};

#define gst_rds_stream_demux_parent_class parent_class
G_DEFINE_TYPE (GstRdsStreamDemux, gst_rds_stream_demux, GST_TYPE_ELEMENT);
GST_ELEMENT_REGISTER_DEFINE (rdsstreamdemux, "rdsstreamdemux", GST_RANK_NONE, GST_TYPE_RDS_STREAM_DEMUX);

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS ")"));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src_%u", GST_PAD_SRC, GST_PAD_REQUEST,
    GST_STATIC_CAPS ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS ")"));

static SrcPad *
src_pad_ref (SrcPad * sp)
{
  g_atomic_int_inc (&sp->ref);
  return sp;
}

static void
src_pad_unref (gpointer data)
{
  SrcPad *sp = data;

  if (g_atomic_int_dec_and_test (&sp->ref)) {
    gst_object_unref (sp->pad);
    g_free (sp);
  }
}

/* Lock held. The sticky events @sp still needs, in order, as far as they are known; clears their flags. The
 * segment only with @segment: data and EOS need one, other events don't. */
static GList *
take_pending (GstRdsStreamDemux * self, SrcPad * sp, gboolean segment)
{
  GList *events = NULL;

  if (sp->need_stream_start) {
    const gchar *up = self->stream_id != NULL ? self->stream_id : "rdsstreamdemux";
    gchar *sid = sp->generation == 0 ? g_strdup_printf ("%s/%u", up, sp->id) :
        g_strdup_printf ("%s/%u/%u", up, sp->id, sp->generation);
    GstEvent *ev = gst_event_new_stream_start (sid);

    if (self->have_group_id)
      gst_event_set_group_id (ev, self->group_id);
    events = g_list_append (events, ev);
    g_free (sid);
    sp->need_stream_start = FALSE;
    sp->generation++;
  }
  if (sp->need_caps && self->caps != NULL) {
    events = g_list_append (events, gst_event_new_caps (self->caps));
    sp->need_caps = FALSE;
  }
  if (segment && sp->need_segment) {
    const GstSegment *seg = g_hash_table_lookup (self->segments, GUINT_TO_POINTER (sp->id));

    if (seg == NULL && self->have_segment)
      seg = &self->segment;
    if (seg != NULL) {
      events = g_list_append (events, gst_event_new_segment (seg));
      sp->need_segment = FALSE;
    }
  }
  return events;
}

static void
push_events (SrcPad * sp, GList * events)
{
  for (GList * l = events; l != NULL; l = l->next)
    gst_pad_push_event (sp->pad, l->data);
  g_list_free (events);
}

/* Lock held. A new stream on @sp: new stream-start, caps, segment, DISCONT. */
static void
new_stream (SrcPad * sp)
{
  sp->eos = FALSE;
  sp->need_stream_start = sp->need_caps = sp->need_segment = sp->need_discont = TRUE;
}

/* Lock held. The pad of source @id, with a new reference, or NULL. */
static SrcPad *
lookup (GstRdsStreamDemux * self, guint id)
{
  SrcPad *sp = g_hash_table_lookup (self->pads, GUINT_TO_POINTER (id));

  return sp != NULL ? src_pad_ref (sp) : NULL;
}

/* Sends EOS on @sp once per stream, after its pending sticky events. */
static void
send_eos (GstRdsStreamDemux * self, SrcPad * sp)
{
  GList *events;

  g_mutex_lock (&self->lock);
  if (sp->eos) {
    g_mutex_unlock (&self->lock);
    return;
  }
  sp->eos = TRUE;
  events = take_pending (self, sp, TRUE);
  g_mutex_unlock (&self->lock);
  push_events (sp, events);
  GST_DEBUG_OBJECT (sp->pad, "EOS");
  gst_pad_push_event (sp->pad, gst_event_new_eos ());
}

/* Lock held. A new reference to every pad. */
static GList *
all_pads (GstRdsStreamDemux * self)
{
  GList *pads = NULL;
  GHashTableIter it;
  gpointer v;

  g_hash_table_iter_init (&it, self->pads);
  while (g_hash_table_iter_next (&it, NULL, &v))
    pads = g_list_prepend (pads, src_pad_ref (v));
  return pads;
}

/* Pushes @event (transfer full) on @sp, after its pending stream-start and caps when @sticky. */
static void
forward (GstRdsStreamDemux * self, SrcPad * sp, GstEvent * event, gboolean sticky)
{
  GList *events = NULL;

  if (sticky) {
    g_mutex_lock (&self->lock);
    events = take_pending (self, sp, FALSE);
    g_mutex_unlock (&self->lock);
  }
  push_events (sp, events);
  gst_pad_push_event (sp->pad, event);
}

static void
forward_all (GstRdsStreamDemux * self, GstEvent * event, gboolean sticky)
{
  GList *pads;

  g_mutex_lock (&self->lock);
  pads = all_pads (self);
  g_mutex_unlock (&self->lock);
  for (GList * l = pads; l != NULL; l = l->next)
    forward (self, l->data, gst_event_ref (event), sticky);
  g_list_free_full (pads, src_pad_unref);
  gst_event_unref (event);
}

/* Lock held. */
static void
mark_all (GstRdsStreamDemux * self, gsize offset)
{
  GHashTableIter it;
  gpointer v;

  g_hash_table_iter_init (&it, self->pads);
  while (g_hash_table_iter_next (&it, NULL, &v))
    G_STRUCT_MEMBER (gboolean, v, offset) = TRUE;
}

/* Source-scoped stream events: to src_N only. Returns FALSE if @event is not one. */
static gboolean
handle_stream_event (GstRdsStreamDemux * self, GstEvent * event)
{
  GstSegment *seg = NULL;
  guint id = 0;
  gboolean ok, end = FALSE, restart = FALSE, start = FALSE, forget = FALSE;
  SrcPad *sp;

  switch ((RdsEventType) GST_EVENT_TYPE (event)) {
    case RDS_EVENT_PAD_ADDED:
      ok = rds_event_parse_pad_added (event, &id);
      restart = TRUE;
      break;
    case RDS_EVENT_STREAM_START:
      ok = rds_event_parse_stream_start (event, &id, NULL);
      start = forget = TRUE;
      break;
    case RDS_EVENT_STREAM_RESET:
      ok = rds_event_parse_stream_reset (event, &id);
      restart = forget = TRUE;
      break;
    case RDS_EVENT_STREAM_SEGMENT:
      ok = rds_event_parse_stream_segment (event, &id, &seg);
      break;
    case RDS_EVENT_STREAM_EOS:
      ok = rds_event_parse_stream_eos (event, &id);
      g_mutex_lock (&self->lock);
      end = self->per_stream_eos;
      g_mutex_unlock (&self->lock);
      break;
    case RDS_EVENT_PAD_DELETED:
      ok = rds_event_parse_pad_deleted (event, &id);
      end = forget = TRUE;
      break;
    default:
      return FALSE;
  }
  if (!ok) {
    GST_WARNING_OBJECT (self, "malformed %s event, dropped", GST_EVENT_TYPE_NAME (event));
    gst_event_unref (event);
    return TRUE;
  }

  g_mutex_lock (&self->lock);
  if (seg != NULL)
    g_hash_table_insert (self->segments, GUINT_TO_POINTER (id), gst_segment_copy (seg));
  else if (forget)
    g_hash_table_remove (self->segments, GUINT_TO_POINTER (id));
  sp = lookup (self, id);
  if (sp != NULL) {
    if (start || (restart && sp->eos))
      new_stream (sp);
    if (seg != NULL)
      sp->need_segment = TRUE;
  }
  g_mutex_unlock (&self->lock);

  if (sp == NULL) {
    GST_DEBUG_OBJECT (self, "no src_%u for %s, dropped", id, GST_EVENT_TYPE_NAME (event));
    gst_event_unref (event);
    return TRUE;
  }
  forward (self, sp, event, TRUE);
  if (end)
    send_eos (self, sp);
  src_pad_unref (sp);
  return TRUE;
}

static gboolean
gst_rds_stream_demux_sink_event (G_GNUC_UNUSED GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (parent);
  GList *pads;

  if (handle_stream_event (self, event))
    return TRUE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:{
      const gchar *sid = NULL;
      guint group = 0;

      gst_event_parse_stream_start (event, &sid);
      g_mutex_lock (&self->lock);
      g_free (self->stream_id);
      self->stream_id = g_strdup (sid);
      self->have_group_id = gst_event_parse_group_id (event, &group);
      self->group_id = group;
      mark_all (self, G_STRUCT_OFFSET (SrcPad, need_stream_start));
      mark_all (self, G_STRUCT_OFFSET (SrcPad, need_discont));
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_CAPS:{
      GstCaps *caps;

      gst_event_parse_caps (event, &caps);
      g_mutex_lock (&self->lock);
      gst_caps_replace (&self->caps, caps);
      mark_all (self, G_STRUCT_OFFSET (SrcPad, need_caps));
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_SEGMENT:{
      const GstSegment *seg;

      gst_event_parse_segment (event, &seg);
      g_mutex_lock (&self->lock);
      gst_segment_copy_into (seg, &self->segment);
      self->have_segment = TRUE;
      mark_all (self, G_STRUCT_OFFSET (SrcPad, need_segment));
      g_mutex_unlock (&self->lock);
      gst_event_unref (event);
      return TRUE;
    }
    case GST_EVENT_EOS:
      g_mutex_lock (&self->lock);
      pads = all_pads (self);
      g_mutex_unlock (&self->lock);
      for (GList * l = pads; l != NULL; l = l->next)
        send_eos (self, l->data);
      g_list_free_full (pads, src_pad_unref);
      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_FLUSH_STOP:
      g_mutex_lock (&self->lock);
      {
        GHashTableIter it;
        gpointer v;

        /* the pads drop their EOS and segment; the sources' segments are stale: they send new ones */
        g_hash_table_iter_init (&it, self->pads);
        while (g_hash_table_iter_next (&it, NULL, &v)) {
          ((SrcPad *) v)->eos = FALSE;
          ((SrcPad *) v)->need_segment = ((SrcPad *) v)->need_discont = TRUE;
        }
      }
      g_hash_table_remove_all (self->segments);
      gst_flow_combiner_reset (self->flow);
      g_mutex_unlock (&self->lock);
      forward_all (self, event, FALSE);
      return TRUE;
    default:
      /* flush-start and other out-of-band events as they are; serialized ones after the sticky events */
      forward_all (self, event, GST_EVENT_IS_SERIALIZED (event) && GST_EVENT_TYPE (event) != GST_EVENT_FLUSH_START);
      return TRUE;
  }
}

static gboolean
gst_rds_stream_demux_src_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (parent);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_QOS:
      /* one branch being late must not throttle every source upstream */
      gst_event_unref (event);
      return TRUE;
    case GST_EVENT_SEEK:{
      guint32 seqnum = gst_event_get_seqnum (event);
      gboolean repeat;

      g_mutex_lock (&self->lock);
      repeat = self->have_seek_seqnum && self->seek_seqnum == seqnum;
      self->have_seek_seqnum = TRUE;
      self->seek_seqnum = seqnum;
      g_mutex_unlock (&self->lock);
      if (repeat) {
        /* the same seek from another branch: already sent */
        GST_DEBUG_OBJECT (pad, "seek %u already sent upstream", seqnum);
        gst_event_unref (event);
        return TRUE;
      }
      return gst_pad_push_event (self->sinkpad, event);
    }
    default:
      return gst_pad_event_default (pad, parent, event);
  }
}

static gboolean
gst_rds_stream_demux_sink_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  if (GST_QUERY_TYPE (query) == GST_QUERY_ALLOCATION)
    return FALSE;               /* the outputs are views of the batch: nothing to propose */
  return gst_pad_query_default (pad, parent, query);
}

static gboolean
gst_rds_stream_demux_src_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (parent);

  if (GST_QUERY_TYPE (query) == GST_QUERY_CAPS) {
    GstCaps *filter, *caps;

    gst_query_parse_caps (query, &filter);
    g_mutex_lock (&self->lock);
    caps = self->caps != NULL ? gst_caps_ref (self->caps) : gst_pad_get_pad_template_caps (pad);
    g_mutex_unlock (&self->lock);
    if (filter != NULL) {
      GstCaps *tmp = gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);

      gst_caps_unref (caps);
      caps = tmp;
    }
    gst_query_set_caps_result (query, caps);
    gst_caps_unref (caps);
    return TRUE;
  }
  return gst_pad_query_default (pad, parent, query);
}

/* What one output needs from a frame of the batch, taken under the batch's meta lock so that the push (which runs
 * downstream code, and may block) happens without it. */
typedef struct
{
  guint source_id;
  guint batch_id;
  GstClockTime pts;
  RdsBatchMeta *meta;           /* one-frame batch meta with a deep copy of the frame (batch_id 0), owned */
} FrameSnapshot;

/* Snapshots every frame of @bm under its meta lock. Returns an array of FrameSnapshot. */
static GArray *
snapshot_frames (RdsBatchMeta * bm)
{
  GArray *frames;

  rds_acquire_meta_lock (bm);
  frames = g_array_sized_new (FALSE, FALSE, sizeof (FrameSnapshot), g_list_length (bm->frame_meta_list));
  for (GList * l = bm->frame_meta_list; l != NULL; l = l->next) {
    RdsFrameMeta *fm = l->data;
    FrameSnapshot f = { fm->source_id, fm->batch_id, fm->buf_pts, rds_create_batch_meta (1) };
    /* locks bm again (recursive) then f.meta: the order rds_copy_frame_meta_to_batch documents */
    RdsFrameMeta *copy = rds_copy_frame_meta_to_batch (fm, f.meta);

    if (copy != NULL)
      copy->batch_id = 0;
    g_array_append_val (frames, f);
  }
  rds_release_meta_lock (bm);
  return frames;
}

/* The output for frame @f of @batch: a view of its slot carrying f's batch meta (taken), or NULL. */
static GstBuffer *
make_output (GstRdsStreamDemux * self, GstBuffer * batch, FrameSnapshot * f)
{
  GstBuffer *out = rds_buffer_new_surface_view (batch, f->batch_id);

  if (out == NULL) {
    GST_WARNING_OBJECT (self, "frame of source %u names slot %u, which the batch does not have", f->source_id,
        f->batch_id);
    return NULL;
  }
  gst_buffer_copy_into (out, batch, GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS, 0, -1);
  GST_BUFFER_PTS (out) = f->pts;
  GST_BUFFER_DTS (out) = GST_CLOCK_TIME_NONE;
  rds_buffer_add_batch_meta (out, f->meta);
  f->meta = NULL;
  return out;
}

static GstFlowReturn
gst_rds_stream_demux_chain (G_GNUC_UNUSED GstPad * pad, GstObject * parent, GstBuffer * batch)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (parent);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (batch);
  GstFlowReturn ret = GST_FLOW_OK;
  GArray *frames;

  if (bm == NULL || rds_buffer_get_surface (batch) == NULL) {
    gst_buffer_unref (batch);
    GST_ELEMENT_ERROR (self, STREAM, DEMUX, (NULL),
        ("input buffer without %s", bm == NULL ? "a batch meta" : "a memory:RDS surface"));
    return GST_FLOW_ERROR;
  }

  /* An upstream probe or another holder of the batch may edit its meta: never walk it unlocked, and never push
   * with the lock held. */
  frames = snapshot_frames (bm);
  for (guint i = 0; i < frames->len; i++) {
    FrameSnapshot *f = &g_array_index (frames, FrameSnapshot, i);
    GList *events;
    GstBuffer *out;
    GstFlowReturn r;
    gboolean discont;
    SrcPad *sp;

    g_mutex_lock (&self->lock);
    sp = lookup (self, f->source_id);
    if (sp == NULL || sp->eos) {
      self->dropped++;
      g_mutex_unlock (&self->lock);
      GST_DEBUG_OBJECT (self, "frame of source %u dropped: %s (%" G_GUINT64_FORMAT " so far)", f->source_id,
          sp == NULL ? "no pad" : "pad ended", self->dropped);
      if (sp != NULL)
        src_pad_unref (sp);
      continue;
    }
    events = take_pending (self, sp, TRUE);
    discont = sp->need_discont;
    sp->need_discont = FALSE;
    g_mutex_unlock (&self->lock);

    push_events (sp, events);
    out = make_output (self, batch, f);
    if (out != NULL) {
      if (discont)
        GST_BUFFER_FLAG_SET (out, GST_BUFFER_FLAG_DISCONT);
      r = gst_pad_push (sp->pad, out);
      g_mutex_lock (&self->lock);
      /* a pad released meanwhile returns FLUSHING: that must not stop the others */
      if (sp->registered)
        ret = gst_flow_combiner_update_pad_flow (self->flow, sp->pad, r);
      g_mutex_unlock (&self->lock);
    }
    src_pad_unref (sp);
  }
  for (guint i = 0; i < frames->len; i++)
    rds_destroy_batch_meta (g_array_index (frames, FrameSnapshot, i).meta);     /* dropped frames' metas */
  g_array_unref (frames);
  gst_buffer_unref (batch);
  return ret;
}

/* Lock held. TRUE if source @id has a pad or one is being requested. */
static gboolean
id_taken (GstRdsStreamDemux * self, guint id)
{
  return g_hash_table_contains (self->pads, GUINT_TO_POINTER (id))
      || g_hash_table_contains (self->reserved, GUINT_TO_POINTER (id));
}

/* The id is reserved under the lock, but the pad is added (which emits pad-added, and activates the pad when PAUSED
 * or PLAYING) without it, so a handler may request or release pads. The chain function only sees the pad once it is
 * added and active. */
static GstPad *
gst_rds_stream_demux_request_new_pad (GstElement * element, GstPadTemplate * templ, const gchar * name,
    G_GNUC_UNUSED const GstCaps * caps)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (element);
  guint id = 0;
  gboolean taken;
  gchar *pad_name;
  SrcPad *sp;
  GstPad *pad;

  if (name != NULL && g_strcmp0 (name, "src_%u") != 0) {
    guint64 v;

    if (!g_str_has_prefix (name, "src_")
        || !g_ascii_string_to_unsigned (name + strlen ("src_"), 10, 0, G_MAXUINT, &v, NULL)) {
      GST_WARNING_OBJECT (self, "bad pad name %s", name);
      return NULL;
    }
    id = (guint) v;
    g_mutex_lock (&self->lock);
    taken = id_taken (self, id);
  } else {
    g_mutex_lock (&self->lock);
    while (id_taken (self, id))
      id++;
    taken = FALSE;
  }
  if (!taken)
    g_hash_table_add (self->reserved, GUINT_TO_POINTER (id));
  g_mutex_unlock (&self->lock);
  if (taken) {
    GST_WARNING_OBJECT (self, "pad src_%u already exists", id);
    return NULL;
  }

  pad_name = g_strdup_printf ("src_%u", id);
  pad = gst_pad_new_from_template (templ, pad_name);
  g_free (pad_name);
  gst_pad_set_query_function (pad, gst_rds_stream_demux_src_query);
  gst_pad_set_event_function (pad, gst_rds_stream_demux_src_event);
  gst_pad_use_fixed_caps (pad);

  sp = g_new0 (SrcPad, 1);
  sp->ref = 1;
  sp->pad = gst_object_ref (pad);
  sp->id = id;
  sp->registered = TRUE;
  new_stream (sp);

  if (!gst_element_add_pad (element, pad)) {
    g_mutex_lock (&self->lock);
    g_hash_table_remove (self->reserved, GUINT_TO_POINTER (id));
    g_mutex_unlock (&self->lock);
    src_pad_unref (sp);
    return NULL;
  }
  g_mutex_lock (&self->lock);
  g_hash_table_remove (self->reserved, GUINT_TO_POINTER (id));
  g_hash_table_insert (self->pads, GUINT_TO_POINTER (id), sp);
  gst_flow_combiner_add_pad (self->flow, pad);
  g_mutex_unlock (&self->lock);
  GST_DEBUG_OBJECT (self, "requested %" GST_PTR_FORMAT, pad);
  return pad;
}

static void
gst_rds_stream_demux_release_pad (GstElement * element, GstPad * pad)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (element);
  GHashTableIter it;
  gpointer v;
  gboolean found = FALSE;

  g_mutex_lock (&self->lock);
  g_hash_table_iter_init (&it, self->pads);
  while (g_hash_table_iter_next (&it, NULL, &v)) {
    SrcPad *sp = v;

    if (sp->pad == pad) {
      sp->registered = FALSE;
      gst_flow_combiner_remove_pad (self->flow, pad);
      g_hash_table_iter_remove (&it);   /* drops the table's reference */
      found = TRUE;
      break;
    }
  }
  g_mutex_unlock (&self->lock);
  if (found) {
    /* outside the lock: pad-removed handlers may request or release pads */
    GST_DEBUG_OBJECT (self, "releasing %" GST_PTR_FORMAT, pad);
    gst_pad_set_active (pad, FALSE);
    gst_element_remove_pad (element, pad);
  }
}

static GstStateChangeReturn
gst_rds_stream_demux_change_state (GstElement * element, GstStateChange transition)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (element);
  GstStateChangeReturn ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    GHashTableIter it;
    gpointer v;

    g_mutex_lock (&self->lock);
    g_clear_pointer (&self->stream_id, g_free);
    self->have_group_id = FALSE;
    gst_caps_replace (&self->caps, NULL);
    self->have_segment = FALSE;
    g_hash_table_remove_all (self->segments);
    self->have_seek_seqnum = FALSE;
    gst_flow_combiner_reset (self->flow);
    g_hash_table_iter_init (&it, self->pads);
    while (g_hash_table_iter_next (&it, NULL, &v)) {
      SrcPad *sp = v;

      sp->generation = 0;
      new_stream (sp);
    }
    g_mutex_unlock (&self->lock);
  }
  return ret;
}

static void
gst_rds_stream_demux_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (object);

  switch (prop_id) {
    case PROP_PER_STREAM_EOS:
      g_mutex_lock (&self->lock);
      self->per_stream_eos = g_value_get_boolean (value);
      g_mutex_unlock (&self->lock);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rds_stream_demux_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (object);

  switch (prop_id) {
    case PROP_PER_STREAM_EOS:
      g_mutex_lock (&self->lock);
      g_value_set_boolean (value, self->per_stream_eos);
      g_mutex_unlock (&self->lock);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rds_stream_demux_finalize (GObject * object)
{
  GstRdsStreamDemux *self = GST_RDS_STREAM_DEMUX (object);

  g_hash_table_unref (self->pads);
  g_hash_table_unref (self->segments);
  g_hash_table_unref (self->reserved);
  gst_flow_combiner_free (self->flow);
  g_free (self->stream_id);
  gst_caps_replace (&self->caps, NULL);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_rds_stream_demux_class_init (GstRdsStreamDemuxClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_rds_stream_demux_debug, "rdsstreamdemux", 0, "Redstream stream demuxer");

  gobject_class->set_property = gst_rds_stream_demux_set_property;
  gobject_class->get_property = gst_rds_stream_demux_get_property;
  gobject_class->finalize = gst_rds_stream_demux_finalize;

  g_object_class_install_property (gobject_class, PROP_PER_STREAM_EOS,
      g_param_spec_boolean ("per-stream-eos", "Per-stream EOS",
          "Send EOS on a source's pad when its stream ends (rds-stream-eos), instead of only on the global EOS",
          DEFAULT_PER_STREAM_EOS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream stream demuxer", "Codec/Demuxer/Video",
      "Splits batched memory:RDS buffers back into one stream per source, without copying",
      "Redstream contributors");

  element_class->request_new_pad = gst_rds_stream_demux_request_new_pad;
  element_class->release_pad = gst_rds_stream_demux_release_pad;
  element_class->change_state = gst_rds_stream_demux_change_state;
}

static void
gst_rds_stream_demux_init (GstRdsStreamDemux * self)
{
  g_mutex_init (&self->lock);
  self->pads = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, src_pad_unref);
  self->reserved = g_hash_table_new (g_direct_hash, g_direct_equal);
  self->segments = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) gst_segment_free);
  self->flow = gst_flow_combiner_new ();
  self->per_stream_eos = DEFAULT_PER_STREAM_EOS;
  gst_segment_init (&self->segment, GST_FORMAT_TIME);

  self->sinkpad = gst_pad_new_from_static_template (&sink_template, "sink");
  gst_pad_set_chain_function (self->sinkpad, gst_rds_stream_demux_chain);
  gst_pad_set_event_function (self->sinkpad, gst_rds_stream_demux_sink_event);
  gst_pad_set_query_function (self->sinkpad, gst_rds_stream_demux_sink_query);
  gst_element_add_pad (GST_ELEMENT (self), self->sinkpad);
}
