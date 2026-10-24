/* rdsstreamdemux: factory, pads, splitting system-memory batches into zero-copy views, sticky and stream events,
 * EOS, flow combining and dynamic pads (cpu); rdsstreammux ! rdsstreamdemux on device memory (gpu, skipped without
 * a ROCm device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/check/gstharness.h>
#include <rds/rds.h>
#include <string.h>

#define IN_CAPS "video/x-raw(memory:RDS),format=NV12,width=64,height=48,framerate=30/1"
#define IN_CAPS_2 "video/x-raw(memory:RDS),format=NV12,width=64,height=48,framerate=25/1"
#define UPSTREAM_SID "upstream-batch"

static const RdsBufSurfaceCreateParams nv12_64 = {
  .width = 64, .height = 48, .colorFormat = RDS_BUF_COLOR_FORMAT_NV12, .layout = RDS_BUF_LAYOUT_PITCH,
};

/* The pixel value slot @slot of a test batch is filled with. */
#define SLOT_VALUE(slot) ((guint8) (10 * ((slot) + 1)))
/* The input PTS recorded for a frame of @source in batch @n. */
#define SRC_PTS(n, source) ((GstClockTime) ((n) * 40 + (source) * 3 + 1) * GST_MSECOND)

static GstAllocator *sys_alloc;

/* A system-memory batch of @n frames: slot i holds source @sources[i] (frame meta source_id/pad_index), filled with
 * SLOT_VALUE (i), buf_pts SRC_PTS (@seq, source). The batch itself gets PTS 1000 s + @seq, the MARKER flag (no
 * DISCONT) and a second, unrelated meta, so tests can tell what the outputs take from it. */
static GstBuffer *
make_batch (guint seq, const guint * sources, guint n)
{
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (n);
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (sys_alloc, n, &nv12_64));
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

    g_assert_cmpint (rds_buf_surface_memset (s, i, -1, SLOT_VALUE (i)), ==, 0);
    fm->source_id = sources[i];
    fm->pad_index = sources[i];
    fm->batch_id = i;
    fm->frame_num = (gint) seq;
    fm->buf_pts = SRC_PTS (seq, sources[i]);
    fm->source_frame_width = 64;
    fm->source_frame_height = 48;
    rds_add_frame_meta_to_batch (bm, fm);
  }
  s->numFilled = n;
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  gst_buffer_add_video_meta (buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, 64, 48);
  GST_BUFFER_PTS (buf) = 1000 * GST_SECOND + seq;
  GST_BUFFER_DTS (buf) = 1000 * GST_SECOND + seq;
  GST_BUFFER_DURATION (buf) = 7 * GST_MSECOND;
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_MARKER);
  return buf;
}

/* TRUE if every pixel byte of surface 0 of @s (system memory) equals @v. */
static gboolean
slot_is (const RdsBufSurface * s, guint8 v)
{
  const RdsBufSurfaceParams *sp = &s->surfaceList[0];
  const RdsBufSurfacePlaneParams *pp = &sp->planeParams;

  for (guint p = 0; p < pp->num_planes; p++) {
    const guint8 *ptr = (const guint8 *) sp->dataPtr + pp->offset[p];

    for (guint32 y = 0; y < pp->height[p]; y++)
      for (guint32 x = 0; x < pp->width[p] * pp->bytesPerPix[p]; x++)
        if (ptr[(gsize) y * pp->pitch[p] + x] != v)
          return FALSE;
  }
  return TRUE;
}

/* ---- harness ---- */

typedef struct
{
  GstElement *demux;
  GstHarness *in;
  GstHarness *out[8];           /* by source id, NULL when not requested through a harness */
} Demux;

static void
demux_init (Demux * d, const gchar * props)
{
  gchar *desc = g_strdup_printf ("rdsstreamdemux %s", props != NULL ? props : "");
  GError *err = NULL;

  memset (d, 0, sizeof (*d));
  d->demux = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  gst_object_ref_sink (d->demux);
  g_free (desc);
  d->in = gst_harness_new_with_element (d->demux, "sink", NULL);
}

/* Starts the input: stream-start (UPSTREAM_SID), IN_CAPS and a TIME segment. */
static void
demux_start (Demux * d)
{
  gst_harness_play (d->in);
  g_assert_true (gst_harness_push_event (d->in, gst_event_new_stream_start (UPSTREAM_SID)));
  gst_harness_set_src_caps_str (d->in, IN_CAPS);
}

static GstHarness *
demux_add_out (Demux * d, guint id)
{
  gchar *name = g_strdup_printf ("src_%u", id);

  g_assert_null (d->out[id]);
  d->out[id] = gst_harness_new_with_element (d->demux, NULL, name);
  g_free (name);
  return d->out[id];
}

static void
demux_clear (Demux * d)
{
  for (guint i = 0; i < G_N_ELEMENTS (d->out); i++)
    if (d->out[i] != NULL)
      gst_harness_teardown (d->out[i]);
  gst_harness_teardown (d->in);
  gst_object_unref (d->demux);
}

static GstFlowReturn
push_batch (Demux * d, guint seq, const guint * sources, guint n)
{
  return gst_harness_push (d->in, make_batch (seq, sources, n));
}

/* Events received by @h so far, in order, as short strings: "stream-start:<id>", "caps", "segment", "eos",
 * "rds-<name>:<source>", "flush-start", "flush-stop", or the event type name. Consumes them. */
static gchar *
drain_events (GstHarness * h)
{
  GString *g = g_string_new (NULL);
  GstEvent *ev;

  while ((ev = gst_harness_try_pull_event (h)) != NULL) {
    guint id = 0;

    if (g->len > 0)
      g_string_append_c (g, ' ');
    switch ((RdsEventType) GST_EVENT_TYPE (ev)) {
      case RDS_EVENT_PAD_ADDED:
        g_assert_true (rds_event_parse_pad_added (ev, &id));
        g_string_append_printf (g, "rds-added:%u", id);
        break;
      case RDS_EVENT_PAD_DELETED:
        g_assert_true (rds_event_parse_pad_deleted (ev, &id));
        g_string_append_printf (g, "rds-deleted:%u", id);
        break;
      case RDS_EVENT_STREAM_START:
        g_assert_true (rds_event_parse_stream_start (ev, &id, NULL));
        g_string_append_printf (g, "rds-start:%u", id);
        break;
      case RDS_EVENT_STREAM_SEGMENT:
        g_assert_true (rds_event_parse_stream_segment (ev, &id, NULL));
        g_string_append_printf (g, "rds-segment:%u", id);
        break;
      case RDS_EVENT_STREAM_EOS:
        g_assert_true (rds_event_parse_stream_eos (ev, &id));
        g_string_append_printf (g, "rds-eos:%u", id);
        break;
      case RDS_EVENT_STREAM_RESET:
        g_assert_true (rds_event_parse_stream_reset (ev, &id));
        g_string_append_printf (g, "rds-reset:%u", id);
        break;
      default:
        if (GST_EVENT_TYPE (ev) == GST_EVENT_STREAM_START) {
          const gchar *sid;

          gst_event_parse_stream_start (ev, &sid);
          g_string_append_printf (g, "stream-start:%s", sid);
        } else {
          g_string_append (g, GST_EVENT_TYPE_NAME (ev));
        }
        break;
    }
    gst_event_unref (ev);
  }
  return g_string_free (g, FALSE);
}

static void
assert_events (GstHarness * h, const gchar * expected)
{
  gchar *got = drain_events (h);

  g_assert_cmpstr (got, ==, expected);
  g_free (got);
}

/* ---- cpu ---- */

static void
test_factory (void)
{
  GstElementFactory *f = gst_element_factory_find ("rdsstreamdemux");
  GstElement *e;
  GParamSpec *ps;
  gboolean v = TRUE;
  const GList *templates;
  guint n_src = 0, n_sink = 0;

  g_assert_nonnull (f);
  g_assert_cmpstr (gst_element_factory_get_metadata (f, GST_ELEMENT_METADATA_KLASS), ==, "Codec/Demuxer/Video");
  for (templates = gst_element_factory_get_static_pad_templates (f); templates != NULL; templates = templates->next) {
    GstStaticPadTemplate *t = templates->data;
    GstCaps *caps = gst_static_caps_get (&t->static_caps);

    g_assert_true (rds_caps_has_memory_rds (caps));
    if (t->direction == GST_PAD_SRC) {
      g_assert_cmpstr (t->name_template, ==, "src_%u");
      g_assert_cmpint (t->presence, ==, GST_PAD_REQUEST);
      n_src++;
    } else {
      g_assert_cmpstr (t->name_template, ==, "sink");
      g_assert_cmpint (t->presence, ==, GST_PAD_ALWAYS);
      n_sink++;
    }
    gst_caps_unref (caps);
  }
  g_assert_cmpuint (n_src, ==, 1);
  g_assert_cmpuint (n_sink, ==, 1);

  e = gst_element_factory_create (f, NULL);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "per-stream-eos");
  g_assert_nonnull (ps);
  g_assert_true (ps->value_type == G_TYPE_BOOLEAN);
  g_object_get (e, "per-stream-eos", &v, NULL);
  g_assert_false (v);
  gst_object_unref (e);
  gst_object_unref (f);
}

/* src_N by name, or the lowest free N for a template name; a released name can be requested again. */
static void
test_request_pads (void)
{
  GstElement *e = gst_element_factory_make ("rdsstreamdemux", NULL);
  GstPad *p0, *p5, *p1, *again;

  p0 = gst_element_request_pad_simple (e, "src_0");
  p5 = gst_element_request_pad_simple (e, "src_5");
  g_assert_cmpstr (GST_PAD_NAME (p0), ==, "src_0");
  g_assert_cmpstr (GST_PAD_NAME (p5), ==, "src_5");
  p1 = gst_element_request_pad_simple (e, "src_%u");
  g_assert_nonnull (p1);
  g_assert_cmpstr (GST_PAD_NAME (p1), ==, "src_1");
  g_assert_null (gst_element_request_pad_simple (e, "src_-1"));
  g_assert_null (gst_element_request_pad_simple (e, "src_4294967296"));

  gst_element_release_request_pad (e, p0);
  gst_object_unref (p0);
  again = gst_element_request_pad_simple (e, "src_0");
  g_assert_nonnull (again);
  gst_element_release_request_pad (e, again);
  gst_object_unref (again);
  gst_element_release_request_pad (e, p5);
  gst_object_unref (p5);
  gst_element_release_request_pad (e, p1);
  gst_object_unref (p1);
  gst_object_unref (e);
}

/* A batch of three sources gives one buffer per requested pad: a read-only view of that slot, with PTS = the
 * frame's buf_pts, the batch's flags, and a one-frame batch meta (batch_id 0, the frame's other fields kept). */
static void
test_split (void)
{
  static const guint sources[] = { 2, 0, 1 };
  Demux d;

  demux_init (&d, NULL);
  for (guint i = 0; i < 3; i++)
    demux_add_out (&d, i);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 4, sources, 3), ==, GST_FLOW_OK);

  for (guint slot = 0; slot < 3; slot++) {
    guint src = sources[slot];
    GstBuffer *b = gst_harness_pull (d.out[src]);
    GstMemory *mem;
    RdsBatchMeta *bm;
    RdsFrameMeta *fm;

    g_assert_nonnull (b);
    g_assert_cmpuint (gst_buffer_n_memory (b), ==, 1);
    mem = gst_buffer_peek_memory (b, 0);
    g_assert_true (rds_memory_is_view (mem));
    g_assert_true (GST_MEMORY_IS_READONLY (mem));
    g_assert_true (slot_is (rds_buffer_get_surface (b), SLOT_VALUE (slot)));

    g_assert_cmpuint (GST_BUFFER_PTS (b), ==, SRC_PTS (4, src));
    g_assert_false (GST_BUFFER_DTS_IS_VALID (b));
    g_assert_cmpuint (GST_BUFFER_DURATION (b), ==, 7 * GST_MSECOND);
    g_assert_true (GST_BUFFER_FLAG_IS_SET (b, GST_BUFFER_FLAG_MARKER));
    /* the first buffer on the pad */
    g_assert_true (GST_BUFFER_FLAG_IS_SET (b, GST_BUFFER_FLAG_DISCONT));
    /* only the batch meta, rebuilt: no other meta of the batch */
    g_assert_null (gst_buffer_get_video_meta (b));

    bm = gst_buffer_get_rds_batch_meta (b);
    g_assert_nonnull (bm);
    g_assert_cmpuint (bm->num_frames_in_batch, ==, 1);
    g_assert_cmpuint (bm->max_frames_in_batch, ==, 1);
    fm = bm->frame_meta_list->data;
    g_assert_cmpuint (fm->batch_id, ==, 0);
    g_assert_cmpuint (fm->source_id, ==, src);
    g_assert_cmpuint (fm->pad_index, ==, src);
    g_assert_cmpint (fm->frame_num, ==, 4);
    g_assert_cmpuint (fm->buf_pts, ==, SRC_PTS (4, src));
    g_assert_cmpuint (fm->source_frame_width, ==, 64);
    gst_buffer_unref (b);
  }
  demux_clear (&d);
}

static void
batch_freed (gpointer data, G_GNUC_UNUSED GstMiniObject * where)
{
  *(gboolean *) data = TRUE;
}

/* The batch lives until the last view is released. */
static void
test_batch_lifetime (void)
{
  static const guint sources[] = { 0, 1 };
  gboolean freed = FALSE;
  GstBuffer *batch, *b0, *b1;
  Demux d;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  batch = make_batch (0, sources, 2);
  gst_mini_object_weak_ref (GST_MINI_OBJECT (batch), batch_freed, &freed);
  g_assert_cmpint (gst_harness_push (d.in, batch), ==, GST_FLOW_OK);
  b0 = gst_harness_pull (d.out[0]);
  b1 = gst_harness_pull (d.out[1]);
  g_assert_false (freed);
  gst_buffer_unref (b0);
  g_assert_false (freed);
  gst_buffer_unref (b1);
  g_assert_true (freed);
  demux_clear (&d);
}

/* A frame whose source has no pad is dropped; the others keep flowing. Without any pad the batch is dropped too. */
static void
test_unrequested_dropped (void)
{
  static const guint sources[] = { 0, 1, 2 };
  Demux d;

  demux_init (&d, NULL);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 0, sources, 3), ==, GST_FLOW_OK);

  demux_add_out (&d, 0);
  demux_add_out (&d, 2);
  for (guint seq = 1; seq < 3; seq++) {
    g_assert_cmpint (push_batch (&d, seq, sources, 3), ==, GST_FLOW_OK);
    for (guint i = 0; i < 3; i += 2) {
      GstBuffer *b = gst_harness_pull (d.out[i]);

      g_assert_cmpuint (GST_BUFFER_PTS (b), ==, SRC_PTS (seq, i));
      /* DISCONT on the pad's first buffer only */
      g_assert_cmpint (GST_BUFFER_FLAG_IS_SET (b, GST_BUFFER_FLAG_DISCONT), ==, seq == 1);
      gst_buffer_unref (b);
    }
  }
  g_assert_cmpuint (gst_harness_buffers_received (d.out[0]), ==, 2);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[2]), ==, 2);
  demux_clear (&d);
}

/* stream-start @sid, the sink caps and a TIME segment equal to @seg went out on @h, in that order, and nothing
 * else. */
static void
assert_sticky (GstHarness * h, const gchar * sid, const GstCaps * caps, const GstSegment * seg)
{
  GstPad *pad = GST_PAD_PEER (h->sinkpad);
  GstEvent *ev;
  GstCaps *cur;
  const GstSegment *s;
  gchar *expected = g_strdup_printf ("stream-start:%s caps segment", sid);

  assert_events (h, expected);
  g_free (expected);
  cur = gst_pad_get_current_caps (pad);
  g_assert_true (gst_caps_is_equal (cur, caps));
  gst_caps_unref (cur);
  ev = gst_pad_get_sticky_event (pad, GST_EVENT_SEGMENT, 0);
  g_assert_nonnull (ev);
  gst_event_parse_segment (ev, &s);
  g_assert_cmpint (s->format, ==, seg->format);
  g_assert_cmpuint (s->start, ==, seg->start);
  g_assert_cmpuint (s->base, ==, seg->base);
  g_assert_cmpfloat (s->rate, ==, seg->rate);
  gst_event_unref (ev);
}

/* Every src pad gets its own stream-start (the upstream stream-id plus the source id), the sink caps and the
 * segment before its first buffer, also when requested while data flows; new upstream caps reach every pad before
 * its next buffer. The segment is the source's own (rds-stream-segment) when one came, else the sink segment. */
static void
test_sticky_events (void)
{
  static const guint sources[] = { 0, 1, 2 };
  GstCaps *caps = gst_caps_from_string (IN_CAPS), *caps2 = gst_caps_from_string (IN_CAPS_2);
  GstSegment sink_seg, src_seg;
  GstBuffer *b;
  Demux d;

  gst_segment_init (&sink_seg, GST_FORMAT_TIME);
  gst_segment_init (&src_seg, GST_FORMAT_TIME);
  src_seg.start = 10 * GST_SECOND;
  src_seg.base = 2 * GST_SECOND;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_start (&d);
  /* nothing goes out before the pad's first data */
  assert_events (d.out[0], "");
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_segment (1, &src_seg)));
  g_assert_cmpint (push_batch (&d, 0, sources, 3), ==, GST_FLOW_OK);
  assert_sticky (d.out[0], UPSTREAM_SID "/0", caps, &sink_seg);
  gst_buffer_unref (gst_harness_pull (d.out[0]));

  /* requested while PLAYING, after data has flowed */
  demux_add_out (&d, 1);
  demux_add_out (&d, 2);
  g_assert_cmpint (push_batch (&d, 1, sources, 3), ==, GST_FLOW_OK);
  assert_events (d.out[0], "");
  assert_sticky (d.out[1], UPSTREAM_SID "/1", caps, &src_seg);
  assert_sticky (d.out[2], UPSTREAM_SID "/2", caps, &sink_seg);
  for (guint i = 0; i < 3; i++)
    gst_buffer_unref (gst_harness_pull (d.out[i]));

  /* a caps change goes to every pad before its next buffer (the harness sends a new segment with it) */
  gst_harness_set_src_caps_str (d.in, IN_CAPS_2);
  g_assert_cmpint (push_batch (&d, 2, sources, 3), ==, GST_FLOW_OK);
  for (guint i = 0; i < 3; i++) {
    GstCaps *cur = gst_pad_get_current_caps (GST_PAD_PEER (d.out[i]->sinkpad));

    assert_events (d.out[i], "caps segment");
    g_assert_true (gst_caps_is_equal (cur, caps2));
    gst_caps_unref (cur);
    b = gst_harness_pull (d.out[i]);
    gst_buffer_unref (b);
  }
  gst_caps_unref (caps);
  gst_caps_unref (caps2);
  demux_clear (&d);
}

/* Source-scoped stream events go to their source's pad only (after its sticky events); other serialized custom
 * events and flushes go to every pad. */
static void
test_event_routing (void)
{
  static const guint sources[] = { 0, 1 };
  GstStructure *st = gst_structure_new_empty ("x-test-custom");
  Demux d;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 0, sources, 2), ==, GST_FLOW_OK);
  for (guint i = 0; i < 2; i++) {
    gst_buffer_unref (gst_harness_pull (d.out[i]));
    g_free (drain_events (d.out[i]));
  }

  g_assert_true (gst_harness_push_event (d.in, rds_event_new_pad_added (1)));
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_start (1, "x")));
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_reset (0)));
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_eos (1)));
  /* a source without a pad: dropped */
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_eos (7)));
  g_assert_true (gst_harness_push_event (d.in, gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM, st)));
  assert_events (d.out[0], "rds-reset:0 custom-downstream");
  /* rds-stream-start starts a new stream on the pad: new stream-id */
  assert_events (d.out[1],
      "rds-added:1 stream-start:" UPSTREAM_SID "/1/1 caps rds-start:1 rds-eos:1 custom-downstream");

  g_assert_true (gst_harness_push_event (d.in, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (d.in, gst_event_new_flush_stop (TRUE)));
  for (guint i = 0; i < 2; i++)
    assert_events (d.out[i], "flush-start flush-stop");
  /* after a flush the segment goes out again before the next buffer */
  {
    GstSegment seg;

    gst_segment_init (&seg, GST_FORMAT_TIME);
    g_assert_true (gst_harness_push_event (d.in, gst_event_new_segment (&seg)));
  }
  g_assert_cmpint (push_batch (&d, 1, sources, 2), ==, GST_FLOW_OK);
  for (guint i = 0; i < 2; i++) {
    assert_events (d.out[i], "segment");
    gst_buffer_unref (gst_harness_pull (d.out[i]));
  }
  demux_clear (&d);
}

/* Number of EOS events @h received; consumes the queued events. */
static guint
count_eos (GstHarness * h)
{
  gchar *ev = drain_events (h);
  gchar **parts = g_strsplit (ev, " ", -1);
  guint n = 0;

  for (gchar ** p = parts; *p != NULL; p++)
    n += g_strcmp0 (*p, "eos") == 0;
  g_strfreev (parts);
  g_free (ev);
  return n;
}

/* per-stream-eos: rds-stream-eos (N) ends src_N only; the global EOS then ends the others, never src_N twice.
 * Off (the default): rds-stream-eos only goes through as an event, the global EOS ends every pad. */
static void
check_per_stream_eos (gboolean per_stream)
{
  static const guint sources[] = { 0, 1 }, only0[] = { 0 };
  Demux d;

  demux_init (&d, per_stream ? "per-stream-eos=true" : NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 0, sources, 2), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_eos (1)));
  g_assert_cmpuint (count_eos (d.out[0]), ==, 0);
  g_assert_cmpuint (count_eos (d.out[1]), ==, per_stream ? 1 : 0);

  /* source 0 keeps flowing */
  g_assert_cmpint (push_batch (&d, 1, only0, 1), ==, GST_FLOW_OK);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[0]), ==, 2);

  g_assert_true (gst_harness_push_event (d.in, gst_event_new_eos ()));
  g_assert_cmpuint (count_eos (d.out[0]), ==, 1);
  g_assert_cmpuint (count_eos (d.out[1]), ==, per_stream ? 0 : 1);
  demux_clear (&d);

  /* rds-pad-deleted ends the pad either way */
  demux_init (&d, per_stream ? "per-stream-eos=true" : NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 0, sources, 2), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_pad_deleted (1)));
  g_assert_cmpuint (count_eos (d.out[0]), ==, 0);
  g_assert_cmpuint (count_eos (d.out[1]), ==, 1);
  demux_clear (&d);
}

static void
test_per_stream_eos_on (void)
{
  check_per_stream_eos (TRUE);
}

static void
test_per_stream_eos_off (void)
{
  check_per_stream_eos (FALSE);
}

/* rds-pad-deleted (N) ends src_N, once, also after a per-stream EOS; the other pads are untouched. A pad that
 * never saw data still gets its stream-start and segment before the EOS. */
static void
test_pad_deleted (void)
{
  static const guint sources[] = { 0, 1 };
  Demux d;

  demux_init (&d, "per-stream-eos=true");
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_add_out (&d, 2);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 0, sources, 2), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_pad_deleted (0)));
  g_assert_cmpuint (count_eos (d.out[0]), ==, 1);
  g_assert_cmpuint (count_eos (d.out[1]), ==, 0);

  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_eos (1)));
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_pad_deleted (1)));
  g_assert_cmpuint (count_eos (d.out[1]), ==, 1);

  g_assert_true (gst_harness_push_event (d.in, rds_event_new_pad_deleted (2)));
  /* the segment comes only right before the EOS */
  assert_events (d.out[2], "stream-start:" UPSTREAM_SID "/2 caps rds-deleted:2 segment eos");

  g_assert_true (gst_harness_push_event (d.in, gst_event_new_eos ()));
  for (guint i = 0; i < 3; i++)
    g_assert_cmpuint (count_eos (d.out[i]), ==, 0);
  demux_clear (&d);
}

/* After a per-stream EOS, frames of that source are dropped without an error; once it starts again
 * (rds-stream-start) its pad gets a new stream-start and flows. */
static void
test_restart_after_eos (void)
{
  static const guint only0[] = { 0 };
  Demux d;

  demux_init (&d, "per-stream-eos=true");
  demux_add_out (&d, 0);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 0, only0, 1), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_eos (0)));
  g_assert_cmpuint (count_eos (d.out[0]), ==, 1);
  /* a late frame of the ended source is dropped, and the batch is not refused (more sources may come) */
  g_assert_cmpint (push_batch (&d, 9, only0, 1), ==, GST_FLOW_OK);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[0]), ==, 1);

  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_start (0, "again")));
  g_assert_cmpint (push_batch (&d, 1, only0, 1), ==, GST_FLOW_OK);
  /* a new stream-id for the new stream */
  assert_events (d.out[0], "stream-start:" UPSTREAM_SID "/0/1 caps rds-start:0 segment");
  g_assert_cmpuint (gst_harness_buffers_received (d.out[0]), ==, 2);
  {
    GstBuffer *b = gst_harness_pull (d.out[0]);

    gst_buffer_unref (b);
    b = gst_harness_pull (d.out[0]);
    g_assert_true (GST_BUFFER_FLAG_IS_SET (b, GST_BUFFER_FLAG_DISCONT));
    gst_buffer_unref (b);
  }
  demux_clear (&d);
}

/* NOT_LINKED from one pad does not stop the others; with every pad unlinked the push returns NOT_LINKED. */
static void
test_flow_combiner (void)
{
  static const guint sources[] = { 0, 1 };
  GstPad *p1;
  Demux d;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  p1 = gst_element_request_pad_simple (d.demux, "src_1");
  demux_start (&d);
  for (guint seq = 0; seq < 3; seq++)
    g_assert_cmpint (push_batch (&d, seq, sources, 2), ==, GST_FLOW_OK);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[0]), ==, 3);

  gst_harness_teardown (d.out[0]);
  d.out[0] = NULL;
  g_assert_cmpint (push_batch (&d, 3, sources, 2), ==, GST_FLOW_NOT_LINKED);
  gst_element_release_request_pad (d.demux, p1);
  gst_object_unref (p1);
  demux_clear (&d);
}

typedef struct
{
  Demux *d;
  gint stop;
  guint pushed;
  GstFlowReturn bad;
} PushCtx;

static gpointer
push_thread (gpointer data)
{
  static const guint sources[] = { 0, 1, 2, 3 };
  PushCtx *c = data;

  while (!g_atomic_int_get (&c->stop)) {
    GstFlowReturn r = push_batch (c->d, c->pushed, sources, 4);

    if (r != GST_FLOW_OK)
      c->bad = r;
    c->pushed++;
  }
  return NULL;
}

/* Pads requested and released (from another thread) while batches flow: no error reaches upstream, the stable pad
 * gets every frame. */
static void
test_dynamic_pads (void)
{
  PushCtx c = { 0 };
  GThread *t;
  Demux d;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  gst_harness_set_drop_buffers (d.out[0], TRUE);
  demux_start (&d);
  c.d = &d;
  t = g_thread_new ("push", push_thread, &c);
  for (guint round = 0; round < 200; round++) {
    guint id = 1 + round % 3;
    GstHarness *h = demux_add_out (&d, id);

    g_usleep (200);
    gst_harness_teardown (h);
    d.out[id] = NULL;
  }
  g_atomic_int_set (&c.stop, 1);
  g_thread_join (t);
  g_assert_cmpint (c.bad, ==, GST_FLOW_OK);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[0]), ==, c.pushed);
  demux_clear (&d);
}

/* A downstream pixel writer (make_writable + WRITE map) gets its own copy: the batch slot and the other views
 * stay as they were. */
static void
test_writer_isolated (void)
{
  static const guint sources[] = { 0, 1 };
  GstBuffer *batch, *b0, *b1, *w;
  GstMapInfo info;
  RdsBufSurface *ws;
  Demux d;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  batch = make_batch (0, sources, 2);
  g_assert_cmpint (gst_harness_push (d.in, gst_buffer_ref (batch)), ==, GST_FLOW_OK);
  b0 = gst_harness_pull (d.out[0]);
  b1 = gst_harness_pull (d.out[1]);

  w = gst_buffer_make_writable (gst_buffer_ref (b0));
  g_assert_true (gst_buffer_map (w, &info, GST_MAP_WRITE));
  ws = (RdsBufSurface *) info.data;
  g_assert_false (rds_memory_is_view (gst_buffer_peek_memory (w, 0)));
  g_assert_cmpint (rds_buf_surface_memset (ws, 0, -1, 0xee), ==, 0);
  gst_buffer_unmap (w, &info);
  g_assert_true (slot_is (rds_buffer_get_surface (w), 0xee));

  g_assert_true (slot_is (rds_buffer_get_surface (b0), SLOT_VALUE (0)));
  g_assert_true (slot_is (rds_buffer_get_surface (b1), SLOT_VALUE (1)));
  {
    GstBuffer *v = rds_buffer_new_surface_view (batch, 0);

    g_assert_true (slot_is (rds_buffer_get_surface (v), SLOT_VALUE (0)));
    gst_buffer_unref (v);
  }
  gst_buffer_unref (w);
  gst_buffer_unref (b0);
  gst_buffer_unref (b1);
  gst_buffer_unref (batch);
  demux_clear (&d);
}

/* The src pads answer caps queries with the sink caps once known; upstream gets no allocation proposal. */
static void
test_queries (void)
{
  GstCaps *caps = gst_caps_from_string (IN_CAPS), *got;
  GstQuery *q;
  Demux d;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_start (&d);
  got = gst_pad_query_caps (GST_PAD_PEER (d.out[0]->sinkpad), NULL);
  g_assert_true (gst_caps_is_equal (got, caps));
  gst_caps_unref (got);

  q = gst_query_new_allocation (caps, TRUE);
  g_assert_false (gst_pad_peer_query (d.in->srcpad, q));
  gst_query_unref (q);
  gst_caps_unref (caps);
  demux_clear (&d);
}

/* QoS from a branch does not go upstream (it would throttle every source); the same seek (seqnum) from several
 * branches goes upstream once; a new seek and other upstream events go through. */
static void
test_upstream_events (void)
{
  GstEvent *seek, *ev;
  Demux d;

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  while ((ev = gst_harness_try_pull_upstream_event (d.in)) != NULL)
    gst_event_unref (ev);

  g_assert_true (gst_harness_push_upstream_event (d.out[0], gst_event_new_qos (GST_QOS_TYPE_UNDERFLOW, 0.5,
              GST_MSECOND, GST_SECOND)));
  g_assert_null (gst_harness_try_pull_upstream_event (d.in));

  seek = gst_event_new_seek (1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, 0);
  for (guint i = 0; i < 2; i++)
    g_assert_true (gst_harness_push_upstream_event (d.out[i], gst_event_ref (seek)));
  ev = gst_harness_try_pull_upstream_event (d.in);
  g_assert_nonnull (ev);
  g_assert_cmpint (GST_EVENT_TYPE (ev), ==, GST_EVENT_SEEK);
  g_assert_cmpuint (gst_event_get_seqnum (ev), ==, gst_event_get_seqnum (seek));
  gst_event_unref (ev);
  g_assert_null (gst_harness_try_pull_upstream_event (d.in));
  gst_event_unref (seek);

  seek = gst_event_new_seek (1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, 0);
  g_assert_true (gst_harness_push_upstream_event (d.out[1], seek));
  ev = gst_harness_try_pull_upstream_event (d.in);
  g_assert_nonnull (ev);
  g_assert_cmpint (GST_EVENT_TYPE (ev), ==, GST_EVENT_SEEK);
  gst_event_unref (ev);

  g_assert_true (gst_harness_push_upstream_event (d.out[0], gst_event_new_reconfigure ()));
  ev = gst_harness_try_pull_upstream_event (d.in);
  g_assert_nonnull (ev);
  g_assert_cmpint (GST_EVENT_TYPE (ev), ==, GST_EVENT_RECONFIGURE);
  gst_event_unref (ev);
  demux_clear (&d);
}

/* A source's segment does not outlive a flush, its reset or its pad-deleted: after FLUSH_STOP, rds-stream-reset (N)
 * goes out without the old segment, and the next frame gets the source's new segment, or the sink segment while
 * the source has sent none. */
static void
test_stale_segment (void)
{
  static const guint sources[] = { 0, 1 };
  GstSegment old_seg, new_seg, seg;
  GstEvent *ev;
  const GstSegment *s;
  Demux d;

  gst_segment_init (&old_seg, GST_FORMAT_TIME);
  old_seg.start = 10 * GST_SECOND;
  gst_segment_init (&new_seg, GST_FORMAT_TIME);
  new_seg.start = 20 * GST_SECOND;
  gst_segment_init (&seg, GST_FORMAT_TIME);

  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  for (guint i = 0; i < 2; i++)
    g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_segment (i, &old_seg)));
  g_assert_cmpint (push_batch (&d, 0, sources, 2), ==, GST_FLOW_OK);
  for (guint i = 0; i < 2; i++) {
    gst_buffer_unref (gst_harness_pull (d.out[i]));
    g_free (drain_events (d.out[i]));
  }

  g_assert_true (gst_harness_push_event (d.in, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (d.in, gst_event_new_flush_stop (TRUE)));
  g_assert_true (gst_harness_push_event (d.in, gst_event_new_segment (&seg)));
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_reset (1)));
  assert_events (d.out[1], "flush-start flush-stop rds-reset:1");
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_segment (1, &new_seg)));
  g_assert_cmpint (push_batch (&d, 1, sources, 2), ==, GST_FLOW_OK);
  assert_events (d.out[1], "rds-segment:1 segment");
  ev = gst_pad_get_sticky_event (GST_PAD_PEER (d.out[1]->sinkpad), GST_EVENT_SEGMENT, 0);
  gst_event_parse_segment (ev, &s);
  g_assert_cmpuint (s->start, ==, 20 * GST_SECOND);
  gst_event_unref (ev);
  /* source 0 sent no new segment since the flush: the sink segment */
  assert_events (d.out[0], "flush-start flush-stop segment");
  ev = gst_pad_get_sticky_event (GST_PAD_PEER (d.out[0]->sinkpad), GST_EVENT_SEGMENT, 0);
  gst_event_parse_segment (ev, &s);
  g_assert_cmpuint (s->start, ==, 0);
  gst_event_unref (ev);

  /* a reset alone (no flush) also forgets it: the next frame falls back to the sink segment */
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_reset (1)));
  g_assert_true (gst_harness_push_event (d.in, gst_event_new_segment (&seg)));
  g_assert_cmpint (push_batch (&d, 2, sources, 2), ==, GST_FLOW_OK);
  assert_events (d.out[1], "rds-reset:1 segment");
  ev = gst_pad_get_sticky_event (GST_PAD_PEER (d.out[1]->sinkpad), GST_EVENT_SEGMENT, 0);
  gst_event_parse_segment (ev, &s);
  g_assert_cmpuint (s->start, ==, 0);
  gst_event_unref (ev);

  /* nor does pad-deleted: a source coming back on the same index starts from the sink segment */
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_segment (1, &new_seg)));
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_pad_deleted (1)));
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_pad_added (1)));
  g_assert_cmpint (push_batch (&d, 3, sources, 2), ==, GST_FLOW_OK);
  ev = gst_pad_get_sticky_event (GST_PAD_PEER (d.out[1]->sinkpad), GST_EVENT_SEGMENT, 0);
  gst_event_parse_segment (ev, &s);
  g_assert_cmpuint (s->start, ==, 0);
  gst_event_unref (ev);
  demux_clear (&d);
}

/* After a per-stream EOS, a flush (a flushing seek) revives the pad: the source's frames flow again. */
static void
test_eos_then_flush (void)
{
  static const guint sources[] = { 0, 1 };
  GstSegment seg;
  Demux d;

  gst_segment_init (&seg, GST_FORMAT_TIME);
  demux_init (&d, "per-stream-eos=true");
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  g_assert_cmpint (push_batch (&d, 0, sources, 2), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_eos (1)));
  g_assert_cmpuint (count_eos (d.out[1]), ==, 1);
  g_assert_cmpint (push_batch (&d, 1, sources, 2), ==, GST_FLOW_OK);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[1]), ==, 1);

  g_assert_true (gst_harness_push_event (d.in, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (d.in, gst_event_new_flush_stop (TRUE)));
  g_assert_true (gst_harness_push_event (d.in, gst_event_new_segment (&seg)));
  g_assert_cmpint (push_batch (&d, 2, sources, 2), ==, GST_FLOW_OK);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[1]), ==, 2);
  g_assert_cmpuint (gst_harness_buffers_received (d.out[0]), ==, 3);
  demux_clear (&d);
}

/* PAUSED -> READY -> PLAYING: the pads are reused and get their sticky events again; a pad ended by a per-stream
 * EOS before flows again. */
static void
test_state_cycle (void)
{
  static const guint sources[] = { 0, 1 };
  GstCaps *caps = gst_caps_from_string (IN_CAPS);
  GstSegment seg;
  Demux d;

  gst_segment_init (&seg, GST_FORMAT_TIME);
  demux_init (&d, "per-stream-eos=true");
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  for (guint round = 0; round < 2; round++) {
    demux_start (&d);
    g_assert_cmpint (push_batch (&d, round, sources, 2), ==, GST_FLOW_OK);
    for (guint i = 0; i < 2; i++) {
      gchar *sid = g_strdup_printf (UPSTREAM_SID "/%u", i);

      assert_sticky (d.out[i], sid, caps, &seg);
      gst_buffer_unref (gst_harness_pull (d.out[i]));
      g_free (sid);
    }
    g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_eos (1)));
    g_assert_cmpuint (count_eos (d.out[1]), ==, 1);
    g_assert_cmpint (gst_element_set_state (d.demux, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  }
  gst_caps_unref (caps);
  demux_clear (&d);
}

/* Each branch's running time is its source's: with source 1 in a segment starting at 10 s (base 2 s), a frame at
 * PTS 10.5 s runs at 2.5 s on src_1, while source 0 (sink segment) runs at its PTS. */
static void
test_running_time (void)
{
  static const guint sources[] = { 0, 1 };
  GstSegment seg;
  GstBuffer *batch, *b;
  RdsBatchMeta *bm;
  GstEvent *ev;
  const GstSegment *s;
  Demux d;

  gst_segment_init (&seg, GST_FORMAT_TIME);
  seg.start = seg.time = 10 * GST_SECOND;
  seg.base = 2 * GST_SECOND;
  demux_init (&d, NULL);
  demux_add_out (&d, 0);
  demux_add_out (&d, 1);
  demux_start (&d);
  g_assert_true (gst_harness_push_event (d.in, rds_event_new_stream_segment (1, &seg)));
  batch = make_batch (0, sources, 2);
  bm = gst_buffer_get_rds_batch_meta (batch);
  ((RdsFrameMeta *) g_list_nth_data (bm->frame_meta_list, 1))->buf_pts = 10500 * GST_MSECOND;
  g_assert_cmpint (gst_harness_push (d.in, batch), ==, GST_FLOW_OK);

  for (guint i = 0; i < 2; i++) {
    b = gst_harness_pull (d.out[i]);
    ev = gst_pad_get_sticky_event (GST_PAD_PEER (d.out[i]->sinkpad), GST_EVENT_SEGMENT, 0);
    gst_event_parse_segment (ev, &s);
    g_assert_cmpuint (gst_segment_to_running_time (s, GST_FORMAT_TIME, GST_BUFFER_PTS (b)), ==,
        i == 0 ? SRC_PTS (0, 0) : 2500 * GST_MSECOND);
    gst_event_unref (ev);
    gst_buffer_unref (b);
  }
  demux_clear (&d);
}

static void
request_more (GstElement * e, GstPad * pad, gpointer data)
{
  GstPad **extra = data;

  if (g_strcmp0 (GST_PAD_NAME (pad), "src_0") == 0)
    *extra = gst_element_request_pad_simple (e, "src_7");
}

/* A pad-added handler may request another pad (no lock is held while pads are added). */
static void
test_request_from_handler (void)
{
  GstElement *e = gst_element_factory_make ("rdsstreamdemux", NULL);
  GstPad *p0, *extra = NULL;

  g_signal_connect (e, "pad-added", G_CALLBACK (request_more), &extra);
  p0 = gst_element_request_pad_simple (e, "src_0");
  g_assert_nonnull (p0);
  g_assert_nonnull (extra);
  g_assert_cmpstr (GST_PAD_NAME (extra), ==, "src_7");
  gst_element_release_request_pad (e, extra);
  gst_object_unref (extra);
  gst_element_release_request_pad (e, p0);
  gst_object_unref (p0);
  gst_object_unref (e);
}

/* ---- gpu ---- */

#define APP_CAPS "video/x-raw,format=NV12,width=64,height=48,framerate=30/1"

/* A system-memory NV12 64x48 frame, every byte pseudo-random. */
static GstBuffer *
sys_frame (guint32 seed, GstClockTime pts)
{
  GstVideoInfo info;
  GstBuffer *buf;
  GstMapInfo map;

  gst_video_info_set_format (&info, GST_VIDEO_FORMAT_NV12, 64, 48);
  buf = gst_buffer_new_allocate (NULL, GST_VIDEO_INFO_SIZE (&info), NULL);
  g_assert_true (gst_buffer_map (buf, &map, GST_MAP_WRITE));
  rng_state = seed * 2654435761u + 1;
  for (gsize i = 0; i < map.size; i++)
    map.data[i] = rng_next ();
  gst_buffer_unmap (buf, &map);
  GST_BUFFER_PTS (buf) = pts;
  return buf;
}

/* TRUE if the device view @b holds exactly the system NV12 frame @ref. */
static gboolean
view_equals (GstBuffer * b, GstBuffer * ref)
{
  RdsBufSurface *s = rds_buffer_get_surface (b);
  RdsBufSurfaceParams *p = &s->surfaceList[0];
  GstVideoInfo info;
  GstMapInfo map;
  gboolean same = TRUE;

  gst_video_info_set_format (&info, GST_VIDEO_FORMAT_NV12, 64, 48);
  g_assert_true (gst_buffer_map (ref, &map, GST_MAP_READ));
  g_assert_cmpint (rds_buf_surface_map (s, 0, -1, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 0, -1), ==, 0);
  for (guint pl = 0; pl < 2; pl++) {
    guint rows = pl == 0 ? 48 : 24;

    for (guint y = 0; y < rows; y++)
      same &= memcmp ((guint8 *) p->mappedAddr.addr[pl] + (gsize) y * p->planeParams.pitch[pl],
          map.data + GST_VIDEO_INFO_PLANE_OFFSET (&info, pl) + (gsize) y * GST_VIDEO_INFO_PLANE_STRIDE (&info, pl),
          64) == 0;
  }
  g_assert_cmpint (rds_buf_surface_unmap (s, 0, -1), ==, 0);
  gst_buffer_unmap (ref, &map);
  return same;
}

/* rdsstreammux ! rdsstreamdemux with three sources: every source's frames come out of its own pad, as device-memory
 * views equal to the input (same size: an exact copy), with the input PTS and per-pad events; a downstream writer
 * does not touch the other views of the batch. (A queue on each branch, as usual after a demuxer: the sinks
 * preroll one after the other on the single streaming thread.) */
static void
test_mux_demux (void)
{
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *src[3], *sink[3];
  GError *err = NULL;

  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < 3; i++)
    g_string_append_printf (desc, "appsrc name=s%u format=time caps=\"" APP_CAPS "\" ! m.sink_%u "
        "d.src_%u ! queue ! appsink name=o%u sync=false max-buffers=8 ", i, i, i, i);
  g_string_append (desc, "rdsstreammux name=m width=64 height=48 batch-size=3 ! rdsstreamdemux name=d");
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  for (guint i = 0; i < 3; i++) {
    gchar *name = g_strdup_printf ("s%u", i);

    src[i] = gst_bin_get_by_name (GST_BIN (pipe), name);
    name[0] = 'o';
    sink[i] = gst_bin_get_by_name (GST_BIN (pipe), name);
    g_free (name);
  }
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);

  for (guint n = 0; n < 3; n++) {
    GstBuffer *in[3], *out[3], *orig1;
    HostImage before, after;

    for (guint i = 0; i < 3; i++) {
      in[i] = sys_frame (10 * n + i, SRC_PTS (n, i));
      g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), gst_buffer_ref (in[i])), ==, GST_FLOW_OK);
    }
    for (guint i = 0; i < 3; i++) {
      GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink[i]), 5 * GST_SECOND);
      RdsBatchMeta *bm;
      RdsFrameMeta *fm;

      g_assert_nonnull (sample);
      out[i] = gst_buffer_ref (gst_sample_get_buffer (sample));
      gst_sample_unref (sample);
      g_assert_true (rds_memory_is_view (gst_buffer_peek_memory (out[i], 0)));
      g_assert_cmpint (rds_buffer_get_surface (out[i])->memType, !=, RDS_BUF_MEM_SYSTEM);
      g_assert_cmpuint (GST_BUFFER_PTS (out[i]), ==, SRC_PTS (n, i));
      bm = gst_buffer_get_rds_batch_meta (out[i]);
      g_assert_cmpuint (bm->num_frames_in_batch, ==, 1);
      fm = bm->frame_meta_list->data;
      g_assert_cmpuint (fm->source_id, ==, i);
      g_assert_cmpuint (fm->batch_id, ==, 0);
      g_assert_cmpint (fm->frame_num, ==, (gint) n);
      g_assert_true (view_equals (out[i], in[i]));
    }

    /* a writer on source 0's frame leaves source 1's view (same batch) and its own original alone */
    snapshot (rds_buffer_get_surface (out[1]), 0, &before);
    orig1 = gst_buffer_ref (out[0]);
    out[0] = gst_buffer_make_writable (out[0]);
    {
      GstMapInfo info;

      g_assert_true (gst_buffer_map (out[0], &info, GST_MAP_WRITE));
      g_assert_cmpint (rds_buf_surface_memset ((RdsBufSurface *) info.data, 0, -1, 0x55), ==, 0);
      gst_buffer_unmap (out[0], &info);
    }
    g_assert_true (view_equals (orig1, in[0]));
    snapshot (rds_buffer_get_surface (out[1]), 0, &after);
    g_assert_cmpuint (max_diff (&before, &after, TRUE), ==, 0);
    host_image_clear (&before);
    host_image_clear (&after);
    gst_buffer_unref (orig1);
    for (guint i = 0; i < 3; i++) {
      gst_buffer_unref (out[i]);
      gst_buffer_unref (in[i]);
    }
  }

  for (guint i = 0; i < 3; i++)
    g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src[i])), ==, GST_FLOW_OK);
  for (guint i = 0; i < 3; i++) {
    GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink[i]), 5 * GST_SECOND);

    g_assert_null (sample);
    g_assert_true (gst_app_sink_is_eos (GST_APP_SINK (sink[i])));
  }

  gst_element_set_state (pipe, GST_STATE_NULL);
  for (guint i = 0; i < 3; i++) {
    gst_object_unref (src[i]);
    gst_object_unref (sink[i]);
  }
  gst_object_unref (pipe);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  sys_alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);

  g_test_add_func ("/cpu/demux/factory", test_factory);
  g_test_add_func ("/cpu/demux/request-pads", test_request_pads);
  g_test_add_func ("/cpu/demux/split", test_split);
  g_test_add_func ("/cpu/demux/batch-lifetime", test_batch_lifetime);
  g_test_add_func ("/cpu/demux/unrequested-dropped", test_unrequested_dropped);
  g_test_add_func ("/cpu/demux/sticky-events", test_sticky_events);
  g_test_add_func ("/cpu/demux/event-routing", test_event_routing);
  g_test_add_func ("/cpu/demux/per-stream-eos-on", test_per_stream_eos_on);
  g_test_add_func ("/cpu/demux/per-stream-eos-off", test_per_stream_eos_off);
  g_test_add_func ("/cpu/demux/pad-deleted", test_pad_deleted);
  g_test_add_func ("/cpu/demux/restart-after-eos", test_restart_after_eos);
  g_test_add_func ("/cpu/demux/flow-combiner", test_flow_combiner);
  g_test_add_func ("/cpu/demux/dynamic-pads", test_dynamic_pads);
  g_test_add_func ("/cpu/demux/writer-isolated", test_writer_isolated);
  g_test_add_func ("/cpu/demux/queries", test_queries);
  g_test_add_func ("/cpu/demux/upstream-events", test_upstream_events);
  g_test_add_func ("/cpu/demux/stale-segment", test_stale_segment);
  g_test_add_func ("/cpu/demux/eos-then-flush", test_eos_then_flush);
  g_test_add_func ("/cpu/demux/state-cycle", test_state_cycle);
  g_test_add_func ("/cpu/demux/running-time", test_running_time);
  g_test_add_func ("/cpu/demux/request-from-handler", test_request_from_handler);

  g_test_add_func ("/gpu/demux/mux-demux", test_mux_demux);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_object_unref (sys_alloc);
    gst_deinit ();
    return ret;
  }
}
