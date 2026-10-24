/* Multi-source integration (gpu): decoded sources of different sizes and codecs through rdsstreammux and
 * rdsstreamdemux, checked frame by frame against VA decode plus the CPU reference scaling; a stalled source with a
 * batch timeout; a source added and one removed while playing; and a long four-source run (the soak, its own test
 * entry) that neither leaks host nor GPU memory. Every test skips without a ROCm device or the stock elements it
 * needs (VA encoders/decoders, mp4mux, qtdemux).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"
#include "rds-test-soak.h"
#include "transform-ref.h"

#include <glib/gstdio.h>
#include <rds/rds.h>
#include <string.h>

#define MUX_W 1280
#define MUX_H 720
#define MUX_NV12_SIZE ((gsize) MUX_W * MUX_H * 3 / 2)

/* Demuxed branches end in a fakesink that keeps nothing: a sink holding on to its last buffer (basesink's
 * last-sample, appsink's sample) pins one batch per branch, and with as many branches as the mux's buffer-pool-size
 * the mux runs out of batches. */
#define BRANCH_SINK_PROPS "sync=false enable-last-sample=false"

static gboolean
require_elements (const gchar * const *names)
{
  for (; *names != NULL; names++) {
    if (!rds_test_have_element (*names)) {
      gchar *msg = g_strdup_printf ("%s not available", *names);

      g_test_skip (msg);
      g_free (msg);
      return FALSE;
    }
  }
  return TRUE;
}

/* ---- clips ---- */

typedef struct
{
  const gchar *codec;           /* "h264" or "h265" */
  guint width, height, frames, b_frames;
  const gchar *decoder;         /* what follows the parser in the mux pipeline */
} ClipSpec;

/* MP4 clips in a fresh temporary directory; clips_free deletes them. */
typedef struct
{
  gchar *dir;
  guint n;
  gchar *path[8];
} Clips;

static Clips *
clips_new (const ClipSpec * specs, guint n)
{
  Clips *c = g_new0 (Clips, 1);
  GError *err = NULL;

  g_assert_cmpuint (n, <=, G_N_ELEMENTS (c->path));
  c->dir = g_dir_make_tmp ("rds-pipeline-mux-XXXXXX", &err);
  g_assert_no_error (err);
  c->n = n;
  for (guint i = 0; i < n; i++) {
    gchar *name = g_strdup_printf ("clip%u.mp4", i);

    c->path[i] = g_build_filename (c->dir, name, NULL);
    rds_test_write_mp4 (specs[i].codec, specs[i].width, specs[i].height, specs[i].frames, specs[i].b_frames,
        c->path[i]);
    g_free (name);
  }
  return c;
}

static void
clips_free (Clips * c)
{
  for (guint i = 0; i < c->n; i++) {
    g_assert_cmpint (g_unlink (c->path[i]), ==, 0);
    g_free (c->path[i]);
  }
  g_assert_cmpint (g_rmdir (c->dir), ==, 0);
  g_free (c->dir);
  g_free (c);
}

/* `filesrc ! qtdemux ! parse ! <decoder>` for clip @path, as a gst-launch fragment (no trailing link). */
static gchar *
source_desc (const ClipSpec * spec, const gchar * path)
{
  return g_strdup_printf ("filesrc location=\"%s\" ! qtdemux ! %sparse ! %s", path, spec->codec, spec->decoder);
}

/* ---- collecting the demuxed streams ---- */

/* What one demux branch's sink saw. Written only by that branch's streaming thread (a pad probe); read
 * by the test once the pipeline is stopped or the branch is at EOS. */
typedef struct
{
  guint source_id;
  guint src_w, src_h;           /* expected source_frame_width/height */
  GPtrArray *ref;               /* optional: per frame, the packed MUX_W x MUX_H NV12 reference (freed when used) */
  GArray *ref_pts;              /* optional: per frame, the reference PTS */
  guint count;
  guint max_diff;
  GstClockTime last_pts;
  /* the probe's log of buffers and stream events, e.g. "A0 S0 G0 30 E30 D30 X30": the letter is the event
   * (A pad-added, D pad-deleted, S stream-start, G stream-segment, E stream-eos, R stream-reset, X EOS) and the
   * number the buffers seen before it */
  GString *log;
  guint probe_buffers;
} Collector;

static void
collector_init (Collector * c, guint source_id, guint src_w, guint src_h)
{
  memset (c, 0, sizeof (*c));
  c->source_id = source_id;
  c->src_w = src_w;
  c->src_h = src_h;
  c->last_pts = GST_CLOCK_TIME_NONE;
  c->log = g_string_new (NULL);
}

static void
collector_clear (Collector * c)
{
  if (c->ref != NULL)
    g_ptr_array_unref (c->ref);
  if (c->ref_pts != NULL)
    g_array_unref (c->ref_pts);
  g_string_free (c->log, TRUE);
}

/* Max |diff| between the view @buf (one MUX_W x MUX_H NV12 slot) and the packed NV12 frame @ref. */
static guint
view_max_diff (GstBuffer * buf, const guint8 * ref)
{
  RdsBufSurface *s = rds_buffer_get_surface (buf);
  RdsBufSurfaceParams *p = &s->surfaceList[0];
  guint max_diff = 0;

  g_assert_cmpuint (s->numFilled, ==, 1);
  g_assert_cmpint (p->colorFormat, ==, RDS_BUF_COLOR_FORMAT_NV12);
  g_assert_cmpuint (p->width, ==, MUX_W);
  g_assert_cmpuint (p->height, ==, MUX_H);
  g_assert_cmpint (rds_buf_surface_map (s, 0, -1, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 0, -1), ==, 0);
  for (guint pl = 0; pl < 2; pl++) {
    guint rows = pl == 0 ? MUX_H : MUX_H / 2;
    const guint8 *ref_plane = ref + (pl == 0 ? 0 : (gsize) MUX_W * MUX_H);

    for (guint y = 0; y < rows; y++) {
      const guint8 *got = (const guint8 *) p->mappedAddr.addr[pl] + (gsize) y * p->planeParams.pitch[pl];
      const guint8 *want = ref_plane + (gsize) y * MUX_W;

      for (guint x = 0; x < MUX_W; x++) {
        guint d = (guint) ABS ((gint) got[x] - (gint) want[x]);

        if (d > max_diff)
          max_diff = d;
      }
    }
  }
  g_assert_cmpint (rds_buf_surface_unmap (s, 0, -1), ==, 0);
  return max_diff;
}

/* A frame reaching the branch's sink (its queue thread): meta, PTS and, with a reference, pixels. Nothing keeps the
 * buffer afterwards, so the batch can go back to the mux's pool. (An appsink would keep its last sample, i.e. one
 * batch per branch: with as many branches as the mux's buffer-pool-size, the mux then runs out of batches.) */
static void
collector_frame (Collector * c, GstBuffer * buf)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsFrameMeta *fm;
  GstClockTime pts = GST_BUFFER_PTS (buf);

  g_assert_true (rds_memory_is_view (gst_buffer_peek_memory (buf, 0)));
  g_assert_nonnull (bm);
  g_assert_cmpuint (bm->num_frames_in_batch, ==, 1);
  fm = bm->frame_meta_list->data;
  if (fm->source_id != c->source_id || fm->pad_index != c->source_id)
    g_error ("src_%u: frame of source %u (pad %u)", c->source_id, fm->source_id, fm->pad_index);
  if (fm->source_frame_width != c->src_w || fm->source_frame_height != c->src_h)
    g_error ("src_%u: source frame %ux%u, want %ux%u", c->source_id, fm->source_frame_width,
        fm->source_frame_height, c->src_w, c->src_h);
  g_assert_cmpuint (fm->pipeline_width, ==, MUX_W);
  g_assert_cmpuint (fm->pipeline_height, ==, MUX_H);
  g_assert_cmpint (fm->frame_num, ==, (gint) c->count);
  g_assert_true (GST_CLOCK_TIME_IS_VALID (pts));
  g_assert_cmpuint (fm->buf_pts, ==, pts);
  if (GST_CLOCK_TIME_IS_VALID (c->last_pts) && pts <= c->last_pts)
    g_error ("src_%u frame %u: PTS %" GST_TIME_FORMAT " after %" GST_TIME_FORMAT, c->source_id, c->count,
        GST_TIME_ARGS (pts), GST_TIME_ARGS (c->last_pts));
  c->last_pts = pts;

  if (c->ref != NULL) {
    guint d;

    if (c->count >= c->ref->len)
      g_error ("src_%u: more frames than the reference (%u)", c->source_id, c->ref->len);
    if (pts != g_array_index (c->ref_pts, GstClockTime, c->count))
      g_error ("src_%u frame %u: PTS %" GST_TIME_FORMAT ", reference %" GST_TIME_FORMAT, c->source_id, c->count,
          GST_TIME_ARGS (pts), GST_TIME_ARGS (g_array_index (c->ref_pts, GstClockTime, c->count)));
    d = view_max_diff (buf, c->ref->pdata[c->count]);
    if (d > 1)
      g_error ("src_%u frame %u: max |diff| %u against the CPU reference", c->source_id, c->count, d);
    c->max_diff = MAX (c->max_diff, d);
    g_free (c->ref->pdata[c->count]);
    c->ref->pdata[c->count] = NULL;
  }
  g_atomic_int_inc ((gint *) & c->count);
}

/* Buffers and stream events reaching the sink, in order (see Collector.log). */
static GstPadProbeReturn
collector_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Collector *c = user_data;
  GstEvent *ev;
  guint id = G_MAXUINT;
  gchar code = 0;

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    collector_frame (c, GST_PAD_PROBE_INFO_BUFFER (info));
    c->probe_buffers++;
    return GST_PAD_PROBE_OK;
  }
  ev = GST_PAD_PROBE_INFO_EVENT (info);
  if (GST_EVENT_TYPE (ev) == GST_EVENT_EOS) {
    code = 'X';
    id = c->source_id;
  } else if (rds_event_parse_pad_added (ev, &id)) {
    code = 'A';
  } else if (rds_event_parse_pad_deleted (ev, &id)) {
    code = 'D';
  } else if (rds_event_parse_stream_eos (ev, &id)) {
    code = 'E';
  } else if (rds_event_parse_stream_reset (ev, &id)) {
    code = 'R';
  } else if (rds_event_parse_stream_start (ev, &id, NULL)) {
    code = 'S';
  } else if (rds_event_parse_stream_segment (ev, &id, NULL)) {
    code = 'G';
  }
  if (code != 0) {
    if (id != c->source_id)
      g_error ("src_%u got a stream event of source %u", c->source_id, id);
    g_string_append_printf (c->log, "%s%c%u", c->log->len ? " " : "", code, c->probe_buffers);
  }
  return GST_PAD_PROBE_OK;
}

static void
collector_attach (Collector * c, GstElement * sink)
{
  GstPad *pad = gst_element_get_static_pad (sink, "sink");

  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, collector_probe, c,
      NULL);
  gst_object_unref (pad);
}

/* Index of the first occurrence of event @code in @log (by position among the logged items), or -1. */
static gint
log_index (const gchar * log, gchar code)
{
  gchar **items = g_strsplit (log, " ", -1);
  gint ret = -1;

  for (guint i = 0; items[i] != NULL && ret < 0; i++)
    if (items[i][0] == code)
      ret = (gint) i;
  g_strfreev (items);
  return ret;
}

/* Buffers seen before event @code in @log; fails if absent. */
static guint
log_buffers_at (const gchar * log, gchar code)
{
  gchar **items = g_strsplit (log, " ", -1);
  gint i = log_index (log, code);
  guint ret;

  if (i < 0)
    g_error ("no '%c' in \"%s\"", code, log);
  ret = (guint) g_ascii_strtoull (items[i] + 1, NULL, 10);
  g_strfreev (items);
  return ret;
}

/* ---- batches leaving the mux ---- */

typedef struct
{
  gint64 time;                  /* monotonic, µs */
  guint num;                    /* frames in the batch */
  guint sources;                /* bit per source */
  GstClockTime pts;
  gint64 frame0_wait;           /* µs between source 0's frame arriving at the mux and the batch, or -1 */
} BatchRec;

typedef struct
{
  GMutex lock;
  GArray *batches;              /* BatchRec */
  GArray *arrival0;             /* gint64 per frame of source 0, at the mux sink pad */
} BatchLog;

static GstPadProbeReturn
arrival_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  BatchLog *bl = user_data;
  gint64 now = g_get_monotonic_time ();

  g_mutex_lock (&bl->lock);
  g_array_append_val (bl->arrival0, now);
  g_mutex_unlock (&bl->lock);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
batch_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  BatchLog *bl = user_data;
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  BatchRec r = { g_get_monotonic_time (), 0, 0, GST_BUFFER_PTS (buf), -1 };

  g_assert_nonnull (bm);
  r.num = bm->num_frames_in_batch;
  g_assert_cmpuint (rds_buffer_get_surface (buf)->numFilled, ==, r.num);
  g_mutex_lock (&bl->lock);
  for (GList * l = bm->frame_meta_list; l != NULL; l = l->next) {
    RdsFrameMeta *fm = l->data;

    r.sources |= 1u << fm->source_id;
    if (fm->source_id == 0 && bl->arrival0 != NULL && (guint) fm->frame_num < bl->arrival0->len)
      r.frame0_wait = r.time - g_array_index (bl->arrival0, gint64, fm->frame_num);
  }
  g_array_append_val (bl->batches, r);
  g_mutex_unlock (&bl->lock);
  return GST_PAD_PROBE_OK;
}

static void
batch_log_init (BatchLog * bl, GstElement * mux, gboolean arrivals)
{
  GstPad *pad = gst_element_get_static_pad (mux, "src");

  g_mutex_init (&bl->lock);
  bl->batches = g_array_new (FALSE, FALSE, sizeof (BatchRec));
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, batch_probe, bl, NULL);
  gst_object_unref (pad);
  if (arrivals) {
    bl->arrival0 = g_array_new (FALSE, FALSE, sizeof (gint64));
    pad = gst_element_get_static_pad (mux, "sink_0");
    gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, arrival_probe, bl, NULL);
    gst_object_unref (pad);
  }
}

static void
batch_log_clear (BatchLog * bl)
{
  g_array_unref (bl->batches);
  if (bl->arrival0 != NULL)
    g_array_unref (bl->arrival0);
  g_mutex_clear (&bl->lock);
}

/* ---- shared pipeline plumbing ---- */

static GstElement *
get_by_name (GstElement * pipe, const gchar * fmt, guint i)
{
  gchar *name = g_strdup_printf (fmt, i);
  GstElement *e = gst_bin_get_by_name (GST_BIN (pipe), name);

  g_assert_nonnull (e);
  g_free (name);
  return e;
}

/* Waits for EOS on the bus; fails on an error or after @timeout (first printing the @n_cols collectors' state). */
static void
wait_eos (GstElement * pipe, GstClockTime timeout, Collector * cols, guint n_cols)
{
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, timeout, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  if (msg == NULL)
    for (guint i = 0; i < n_cols; i++)
      g_printerr ("src_%u: %u frames, events \"%s\"\n", cols[i].source_id, cols[i].count, cols[i].log->str);
  if (msg == NULL)
    g_error ("no EOS within %" GST_TIME_FORMAT, GST_TIME_ARGS (timeout));
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;
    gchar *dbg = NULL;

    gst_message_parse_error (msg, &err, &dbg);
    g_error ("pipeline error from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message,
        dbg ? dbg : "");
  }
  gst_message_unref (msg);
  gst_object_unref (bus);
}

/* ---- four sources against the CPU reference ---- */

/* An H.264 320x240 clip with B-frames, H.264 1280x720, H.265 640x360 (all through rdsvideodec, memory:RDS), and
 * H.264 1920x1080 through vah264dec in system memory (the mux's upload path). Different lengths, so sources end
 * at different times. */
static const ClipSpec four_specs[] = {
  {"h264", 320, 240, 40, 2, "rdsvideodec"},
  {"h264", 1280, 720, 30, 0, "rdsvideodec"},
  {"h265", 640, 360, 36, 0, "rdsvideodec"},
  {"h264", 1920, 1080, 24, 0, "vah264dec ! video/x-raw,format=NV12"},
};

/* Decodes @path with va{codec}dec to system NV12 and scales every frame into a packed MUX_W x MUX_H NV12 with the
 * CPU reference (nearest: the mux's default interpolation). */
static void
make_reference (const ClipSpec * spec, const gchar * path, GPtrArray * ref, GArray * ref_pts)
{
  GstElement *pipe, *sink;
  GError *err = NULL;
  GstSample *s;
  gchar *desc;

  desc = g_strdup_printf ("filesrc location=\"%s\" ! qtdemux ! %sparse ! va%sdec ! video/x-raw,format=NV12 "
      "! appsink name=sink sync=false", path, spec->codec, spec->codec);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstVideoInfo info;
    GstVideoFrame frame;
    RefImage src = { 0 }, dst = { 0 };
    guint8 *out = g_malloc (MUX_NV12_SIZE);
    GstClockTime pts = GST_BUFFER_PTS (gst_sample_get_buffer (s));

    g_assert_true (gst_video_info_from_caps (&info, gst_sample_get_caps (s)));
    g_assert_cmpint (GST_VIDEO_INFO_WIDTH (&info), ==, spec->width);
    g_assert_cmpint (GST_VIDEO_INFO_HEIGHT (&info), ==, spec->height);
    g_assert_true (gst_video_frame_map (&frame, &info, gst_sample_get_buffer (s), GST_MAP_READ));
    src.format = RDS_BUF_COLOR_FORMAT_NV12;
    src.width = spec->width;
    src.height = spec->height;
    for (guint p = 0; p < 2; p++) {
      src.plane[p] = (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&frame, p);
      src.pitch[p] = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, p);
    }
    dst.format = RDS_BUF_COLOR_FORMAT_NV12;
    dst.width = MUX_W;
    dst.height = MUX_H;
    dst.plane[0] = out;
    dst.plane[1] = out + (gsize) MUX_W * MUX_H;
    dst.pitch[0] = dst.pitch[1] = MUX_W;
    ref_transform (&src, &dst, (RefRect) {
        0, 0, spec->width, spec->height}, (RefRect) {
        0, 0, MUX_W, MUX_H}, FALSE);
    gst_video_frame_unmap (&frame);
    g_ptr_array_add (ref, out);
    g_array_append_val (ref_pts, pts);
    gst_sample_unref (s);
  }
  (void) rds_test_bus_check (pipe);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (pipe);
  g_free (desc);
}

/* Four mixed sources ! rdsstreammux width=1280 height=720 batch-size=4 ! rdsstreamdemux ! 4 x queue ! fakesink:
 * every demuxed frame equals the CPU-reference scaling of the VA-decoded source frame (max |diff| <= 1), with the
 * source's PTS and frame count, the right source_id and source size in its meta, and every source's stream events.
 * Without a timeout the mux waits for every source still running, so the batch sizes are exact: 4 until the
 * shortest clip ends, then 3, 2, 1. */
static void
test_four_sources (void)
{
  const gchar *need[] = { "vah264enc", "vah265enc", "vah264dec", "vah265dec", "mp4mux", "qtdemux", "h264parse",
    "h265parse", NULL
  };
  const guint n = G_N_ELEMENTS (four_specs);
  Collector col[G_N_ELEMENTS (four_specs)];
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *mux;
  BatchLog bl = { 0 };
  GError *err = NULL;
  Clips *clips;
  guint max_frames = 0, total = 0;
  gint64 t0;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  clips = clips_new (four_specs, n);
  t0 = g_get_monotonic_time ();
  for (guint i = 0; i < n; i++) {
    collector_init (&col[i], i, four_specs[i].width, four_specs[i].height);
    col[i].ref = g_ptr_array_new_with_free_func (g_free);
    col[i].ref_pts = g_array_new (FALSE, FALSE, sizeof (GstClockTime));
    make_reference (&four_specs[i], clips->path[i], col[i].ref, col[i].ref_pts);
    g_assert_cmpuint (col[i].ref->len, ==, four_specs[i].frames);
    max_frames = MAX (max_frames, four_specs[i].frames);
  }
  g_test_message ("reference: %.1f s", (g_get_monotonic_time () - t0) / 1e6);

  for (guint i = 0; i < n; i++) {
    gchar *src = source_desc (&four_specs[i], clips->path[i]);

    g_string_append_printf (desc, "%s ! m.sink_%u d.src_%u ! queue ! fakesink name=o%u " BRANCH_SINK_PROPS " ",
        src, i, i, i);
    g_free (src);
  }
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u ! rdsstreamdemux name=d",
      MUX_W, MUX_H, n);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  mux = get_by_name (pipe, "m", 0);
  batch_log_init (&bl, mux, FALSE);
  for (guint i = 0; i < n; i++) {
    GstElement *sink = get_by_name (pipe, "o%u", i);

    collector_attach (&col[i], sink);
    gst_object_unref (sink);
  }

  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  wait_eos (pipe, 60 * GST_SECOND, col, n);
  g_test_message ("mux pipeline: %.1f s", (g_get_monotonic_time () - t0) / 1e6);
  gst_element_set_state (pipe, GST_STATE_NULL);

  for (guint i = 0; i < n; i++) {
    g_test_message ("src_%u (%s %ux%u): %u frames, max |diff| %u, events \"%s\"", i, four_specs[i].codec,
        four_specs[i].width, four_specs[i].height, col[i].count, col[i].max_diff, col[i].log->str);
    g_assert_cmpuint (col[i].count, ==, four_specs[i].frames);
    g_assert_cmpuint (col[i].probe_buffers, ==, four_specs[i].frames);
    /* pad-added, stream-start and stream-segment before the first frame; stream-eos after the last; then EOS */
    g_assert_cmpint (log_index (col[i].log->str, 'A'), ==, 0);
    g_assert_cmpuint (log_buffers_at (col[i].log->str, 'S'), ==, 0);
    g_assert_cmpuint (log_buffers_at (col[i].log->str, 'G'), ==, 0);
    g_assert_cmpuint (log_buffers_at (col[i].log->str, 'E'), ==, four_specs[i].frames);
    g_assert_cmpuint (log_buffers_at (col[i].log->str, 'X'), ==, four_specs[i].frames);
    g_assert_cmpint (log_index (col[i].log->str, 'D'), <, 0);
    total += col[i].count;
  }

  /* batch k holds frame k of every source that has one */
  g_assert_cmpuint (bl.batches->len, ==, max_frames);
  for (guint k = 0; k < bl.batches->len; k++) {
    BatchRec *r = &g_array_index (bl.batches, BatchRec, k);
    guint want = 0, want_mask = 0;

    for (guint i = 0; i < n; i++) {
      if (k < four_specs[i].frames) {
        want++;
        want_mask |= 1u << i;
      }
    }
    if (r->num != want || r->sources != want_mask)
      g_error ("batch %u: %u frames (sources 0x%x), want %u (0x%x)", k, r->num, r->sources, want, want_mask);
    if (k > 0)
      g_assert_cmpuint (r->pts, >=, g_array_index (bl.batches, BatchRec, k - 1).pts);
    total -= r->num;
  }
  g_assert_cmpuint (total, ==, 0);

  batch_log_clear (&bl);
  for (guint i = 0; i < n; i++)
    collector_clear (&col[i]);
  gst_object_unref (mux);
  gst_object_unref (pipe);
  clips_free (clips);
}

/* ---- stall ---- */

#define STALL_SOURCES 4
#define STALL_FRAMES 90
#define STALL_AT 20             /* source 3 stops before its frame STALL_AT */
#define STALL_US (1000 * 1000)

typedef struct
{
  GMutex lock;
  GCond cond;
  guint seen;
  guint block_at;               /* block before this buffer (G_MAXUINT: never) */
  gboolean blocked;
  gint64 t_block;
} Staller;

/* Passes buffers until the block_at-th, then blocks the pad there (until the probe is removed). */
static GstPadProbeReturn
stall_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  Staller *st = user_data;
  GstPadProbeReturn ret = GST_PAD_PROBE_PASS;

  g_mutex_lock (&st->lock);
  if (st->seen == st->block_at && !st->blocked) {
    st->blocked = TRUE;
    st->t_block = g_get_monotonic_time ();
    g_cond_broadcast (&st->cond);
    ret = GST_PAD_PROBE_OK;
  } else {
    st->seen++;
  }
  g_mutex_unlock (&st->lock);
  return ret;
}

static void
staller_wait_blocked (Staller * st)
{
  gint64 deadline = g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND;

  g_mutex_lock (&st->lock);
  while (!st->blocked)
    if (!g_cond_wait_until (&st->cond, &st->lock, deadline))
      g_error ("source never reached the stall point");
  g_mutex_unlock (&st->lock);
}

/* Four real-time sources (decoded clips paced by `identity sync=true`), batched-push-timeout=33000 (one frame
 * period at 30 fps): while source 3 is stalled for a second, the mux keeps pushing batches of the other three at
 * about the source rate, each about one timeout after its first frame arrived. Every frame of every source still
 * comes out. */
static void
test_stall (void)
{
  const gchar *need[] = { "vah264enc", "mp4mux", "qtdemux", "h264parse", "identity", NULL };
  const ClipSpec spec = { "h264", 320, 240, STALL_FRAMES, 0, "rdsvideodec ! identity sync=true" };
  Collector col[STALL_SOURCES];
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *mux, *id3;
  GstPad *pad;
  BatchLog bl = { 0 };
  Staller st = { 0 };
  GError *err = NULL;
  Clips *clips;
  gulong probe;
  gint64 t_unblock, win0, win1, wait_sum = 0, wait_max = 0, gap_max = 0, prev = 0;
  guint in_window = 0, waits = 0;
  gdouble rate;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  clips = clips_new (&spec, 1);
  for (guint i = 0; i < STALL_SOURCES; i++) {
    gchar *src = source_desc (&spec, clips->path[0]);

    g_string_append_printf (desc, "%s name=id%u ! m.sink_%u d.src_%u ! queue ! fakesink name=o%u "
        BRANCH_SINK_PROPS " ", src, i, i, i, i);
    g_free (src);
  }
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u batched-push-timeout=33000 "
      "! rdsstreamdemux name=d", MUX_W, MUX_H, STALL_SOURCES);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  mux = get_by_name (pipe, "m", 0);
  batch_log_init (&bl, mux, TRUE);
  for (guint i = 0; i < STALL_SOURCES; i++) {
    GstElement *sink = get_by_name (pipe, "o%u", i);

    collector_init (&col[i], i, spec.width, spec.height);
    collector_attach (&col[i], sink);
    gst_object_unref (sink);
  }
  g_mutex_init (&st.lock);
  g_cond_init (&st.cond);
  st.block_at = STALL_AT;
  id3 = get_by_name (pipe, "id%u", STALL_SOURCES - 1);
  pad = gst_element_get_static_pad (id3, "src");
  probe = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER, stall_probe, &st, NULL);

  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  staller_wait_blocked (&st);
  g_usleep (STALL_US);
  t_unblock = g_get_monotonic_time ();
  gst_pad_remove_probe (pad, probe);
  wait_eos (pipe, 60 * GST_SECOND, col, STALL_SOURCES);
  gst_element_set_state (pipe, GST_STATE_NULL);

  for (guint i = 0; i < STALL_SOURCES; i++) {
    g_assert_cmpuint (col[i].count, ==, STALL_FRAMES);
    g_assert_cmpuint (log_buffers_at (col[i].log->str, 'X'), ==, STALL_FRAMES);
  }

  /* the batches pushed while source 3 was stalled (leaving out the first 100 ms: frames already queued) */
  win0 = st.t_block + 100 * 1000;
  win1 = t_unblock;
  for (guint k = 0; k < bl.batches->len; k++) {
    BatchRec *r = &g_array_index (bl.batches, BatchRec, k);

    if (r->time < win0 || r->time >= win1)
      continue;
    if (r->num != STALL_SOURCES - 1 || r->sources != 0x7)
      g_error ("batch %u during the stall: %u frames (sources 0x%x)", k, r->num, r->sources);
    if (prev != 0)
      gap_max = MAX (gap_max, r->time - prev);
    prev = r->time;
    if (r->frame0_wait >= 0) {
      wait_sum += r->frame0_wait;
      wait_max = MAX (wait_max, r->frame0_wait);
      waits++;
    }
    in_window++;
  }
  rate = in_window / ((win1 - win0) / 1e6);
  g_test_message ("stall of %.0f ms: %u batches of 3 in %.0f ms (%.1f batches/s), max gap %.1f ms; source 0's frame "
      "waited %.1f ms on average (max %.1f ms) before its batch went out", (t_unblock - st.t_block) / 1e3, in_window,
      (win1 - win0) / 1e3, rate, gap_max / 1e3, waits ? wait_sum / 1e3 / waits : 0.0, wait_max / 1e3);
  /* about the 30 fps source rate */
  g_assert_cmpfloat (rate, >, 24.0);
  g_assert_cmpfloat (rate, <, 36.0);
  g_assert_cmpint (gap_max, <, 100 * 1000);
  g_assert_cmpuint (waits, >, 0);
  /* the batch waits for the missing source (at least about one timeout), but stays bounded. The timer of a batch
   * starts when the mux first sees a queued frame after the previous batch, not when that frame arrived, so a
   * frame that queued up behind the previous batch waits for that batch plus a full timeout: about 2-3 frame
   * periods here, and slightly fewer batches than the source rate. */
  g_assert_cmpint (wait_sum / waits, >, 25 * 1000);
  g_assert_cmpint (wait_max, <, 200 * 1000);

  g_cond_clear (&st.cond);
  g_mutex_clear (&st.lock);
  gst_object_unref (pad);
  gst_object_unref (id3);
  batch_log_clear (&bl);
  for (guint i = 0; i < STALL_SOURCES; i++)
    collector_clear (&col[i]);
  gst_object_unref (mux);
  gst_object_unref (pipe);
  clips_free (clips);
}

/* ---- dynamic sources ---- */

#define DYN_FRAMES 60

/* Adds `@desc` (ending in an element named "out") as a bin to @pipe and links it to a new mux pad sink_@i. */
static GstElement *
add_source_bin (GstElement * pipe, GstElement * mux, const gchar * desc, guint i)
{
  GError *err = NULL;
  /* no automatic ghost pads: they would also ghost the parser's sink pad, which waits for the demuxer's pad */
  GstElement *bin = gst_parse_bin_from_description (desc, FALSE, &err), *out;
  gchar *name = g_strdup_printf ("sink_%u", i);
  GstPad *mux_pad, *pad;

  g_assert_no_error (err);
  out = gst_bin_get_by_name (GST_BIN (bin), "out");
  g_assert_nonnull (out);
  pad = gst_element_get_static_pad (out, "src");
  g_assert_true (gst_element_add_pad (bin, gst_ghost_pad_new ("src", pad)));
  gst_object_unref (pad);
  gst_object_unref (out);
  g_assert_true (gst_bin_add (GST_BIN (pipe), bin));
  mux_pad = gst_element_request_pad_simple (mux, name);
  g_assert_nonnull (mux_pad);
  pad = gst_element_get_static_pad (bin, "src");
  g_assert_nonnull (pad);
  g_assert_cmpint (gst_pad_link (pad, mux_pad), ==, GST_PAD_LINK_OK);
  gst_object_unref (pad);
  gst_object_unref (mux_pad);
  g_free (name);
  return bin;
}

/* Waits until @c has seen at least @n frames. */
static void
wait_frames (Collector * c, guint n)
{
  gint64 deadline = g_get_monotonic_time () + 20 * G_TIME_SPAN_SECOND;

  while ((guint) g_atomic_int_get ((gint *) &c->count) < n) {
    if (g_get_monotonic_time () > deadline)
      g_error ("src_%u: fewer than %u frames", c->source_id, n);
    g_usleep (5000);
  }
}

/* Four real-time sources; while playing, a fifth source (with its demux branch) is added, and source 1 is
 * removed (blocked, EOS sent into its mux pad, unlinked, stopped, pad released). Every frame source 1 pushed comes
 * out of src_1 before its stream-eos, pad-deleted and EOS; source 4 starts with pad-added and delivers its whole
 * clip; the others are untouched. */
static void
test_dynamic (void)
{
  const gchar *need[] = { "vah264enc", "mp4mux", "qtdemux", "h264parse", "identity", NULL };
  const ClipSpec spec = { "h264", 320, 240, DYN_FRAMES, 0, "rdsvideodec ! identity sync=true name=out" };
  Collector col[5];
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *mux, *demux, *src1 = NULL, *bin4, *queue4, *sink4;
  GstPad *src1_pad, *mux_pad1, *demux_pad4, *pad, *demux_pad1;
  Staller st = { 0 };
  GError *err = NULL;
  Clips *clips;
  gchar *src;
  gint64 deadline;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  clips = clips_new (&spec, 1);
  src = source_desc (&spec, clips->path[0]);
  for (guint i = 0; i < 4; i++)
    g_string_append_printf (desc, "d.src_%u ! queue ! fakesink name=o%u " BRANCH_SINK_PROPS " ", i, i);
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=4 batched-push-timeout=33000 "
      "! rdsstreamdemux name=d", MUX_W, MUX_H);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  mux = get_by_name (pipe, "m", 0);
  demux = get_by_name (pipe, "d", 0);
  for (guint i = 0; i < 4; i++) {
    GstElement *bin = add_source_bin (pipe, mux, src, i);

    if (i == 1)
      src1 = gst_object_ref (bin);
  }
  for (guint i = 0; i < 5; i++)
    collector_init (&col[i], i, spec.width, spec.height);
  for (guint i = 0; i < 4; i++) {
    GstElement *sink = get_by_name (pipe, "o%u", i);

    collector_attach (&col[i], sink);
    gst_object_unref (sink);
  }
  /* counts what source 1 pushes into the mux; blocks once asked to */
  g_mutex_init (&st.lock);
  g_cond_init (&st.cond);
  st.block_at = G_MAXUINT;
  src1_pad = gst_element_get_static_pad (src1, "src");
  g_assert_nonnull (src1_pad);
  gst_pad_add_probe (src1_pad, GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_BUFFER, stall_probe, &st, NULL);

  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);

  /* add source 4: its demux branch first, so none of its frames is dropped */
  wait_frames (&col[0], 10);
  queue4 = gst_element_factory_make ("queue", NULL);
  sink4 = gst_element_factory_make ("fakesink", NULL);
  /* async=false: a sink added while playing would otherwise take the pipeline back to PAUSED until it prerolls,
   * and the paced source feeding it does not run in PAUSED */
  g_object_set (sink4, "sync", FALSE, "enable-last-sample", FALSE, "async", FALSE, NULL);
  collector_attach (&col[4], sink4);
  gst_bin_add_many (GST_BIN (pipe), queue4, sink4, NULL);
  g_assert_true (gst_element_link (queue4, sink4));
  g_assert_true (gst_element_sync_state_with_parent (sink4));
  g_assert_true (gst_element_sync_state_with_parent (queue4));
  demux_pad4 = gst_element_request_pad_simple (demux, "src_4");
  g_assert_nonnull (demux_pad4);
  pad = gst_element_get_static_pad (queue4, "sink");
  g_assert_cmpint (gst_pad_link (demux_pad4, pad), ==, GST_PAD_LINK_OK);
  gst_object_unref (pad);
  bin4 = add_source_bin (pipe, mux, src, 4);
  g_assert_true (gst_element_sync_state_with_parent (bin4));

  /* remove source 1: block it, end its stream in the mux, then take it out */
  wait_frames (&col[0], 30);
  g_mutex_lock (&st.lock);
  st.block_at = st.seen;
  g_mutex_unlock (&st.lock);
  staller_wait_blocked (&st);
  mux_pad1 = gst_pad_get_peer (src1_pad);
  g_assert_nonnull (mux_pad1);
  g_assert_true (gst_pad_send_event (mux_pad1, gst_event_new_eos ()));
  /* the mux sends rds-stream-eos once the EOS is through its queue (after the source's last frame) */
  deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  while (log_index (col[1].log->str, 'E') < 0) {
    if (g_get_monotonic_time () > deadline)
      g_error ("no stream-eos on src_1 (events \"%s\")", col[1].log->str);
    g_usleep (5000);
  }
  g_assert_true (gst_pad_unlink (src1_pad, mux_pad1));
  g_assert_cmpint (gst_element_set_state (src1, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_true (gst_bin_remove (GST_BIN (pipe), src1));
  gst_element_release_request_pad (mux, mux_pad1);
  gst_object_unref (mux_pad1);
  /* pad-deleted ends src_1; then its branch goes too */
  deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  while (log_index (col[1].log->str, 'X') < 0) {
    if (g_get_monotonic_time () > deadline)
      g_error ("src_1 never ended (events \"%s\")", col[1].log->str);
    g_usleep (5000);
  }
  demux_pad1 = gst_element_get_static_pad (demux, "src_1");
  g_assert_nonnull (demux_pad1);
  gst_element_release_request_pad (demux, demux_pad1);
  gst_object_unref (demux_pad1);

  wait_eos (pipe, 60 * GST_SECOND, col, 5);
  gst_element_set_state (pipe, GST_STATE_NULL);

  for (guint i = 0; i < 5; i++)
    g_test_message ("src_%u: %u frames, events \"%s\"", i, col[i].count, col[i].log->str);
  for (guint i = 0; i < 5; i++) {
    const gchar *log = col[i].log->str;

    g_assert_cmpint (log_index (log, 'A'), ==, 0);
    g_assert_cmpuint (log_buffers_at (log, 'S'), ==, 0);
    if (i == 1) {
      /* everything it pushed, then stream-eos, pad-deleted and EOS */
      g_assert_cmpuint (st.seen, >=, 30);
      g_assert_cmpuint (st.seen, <, DYN_FRAMES);
      g_assert_cmpuint (col[1].count, ==, st.seen);
      g_assert_cmpuint (log_buffers_at (log, 'E'), ==, st.seen);
      g_assert_cmpuint (log_buffers_at (log, 'D'), ==, st.seen);
      g_assert_cmpuint (log_buffers_at (log, 'X'), ==, st.seen);
      g_assert_cmpint (log_index (log, 'E'), <, log_index (log, 'D'));
      g_assert_cmpint (log_index (log, 'D'), <, log_index (log, 'X'));
    } else {
      g_assert_cmpuint (col[i].count, ==, DYN_FRAMES);
      g_assert_cmpuint (log_buffers_at (log, 'E'), ==, DYN_FRAMES);
      g_assert_cmpuint (log_buffers_at (log, 'X'), ==, DYN_FRAMES);
      g_assert_cmpint (log_index (log, 'D'), <, 0);
    }
  }

  g_cond_clear (&st.cond);
  g_mutex_clear (&st.lock);
  gst_object_unref (src1_pad);
  gst_object_unref (src1);
  gst_object_unref (demux_pad4);
  gst_object_unref (demux);
  gst_object_unref (mux);
  gst_object_unref (pipe);
  for (guint i = 0; i < 5; i++)
    collector_clear (&col[i]);
  g_free (src);
  clips_free (clips);
}

/* ---- soak ---- */

#define SOAK_SOURCES 4
#define SOAK_W 1280
#define SOAK_H 720
#define SOAK_FRAMES 300
#define SOAK_LOOPS 10

typedef struct
{
  guint batches, frames;
  gsize rss[SOAK_LOOPS], gpu_used[SOAK_LOOPS];  /* after each loop's last batch */
} MuxSoakStats;

/* Runs in the mux's source thread: samples memory use when a loop's last batch leaves the mux. */
static GstPadProbeReturn
mux_soak_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  MuxSoakStats *st = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  st->batches++;
  st->frames += bm->num_frames_in_batch;
  if (st->batches % SOAK_FRAMES == 0 && st->batches / SOAK_FRAMES <= SOAK_LOOPS) {
    guint loop = st->batches / SOAK_FRAMES - 1;

    st->rss[loop] = vm_rss_bytes ();
    st->gpu_used[loop] = gpu_used_bytes ();
  }
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
count_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  (*(guint *) user_data)++;
  return GST_PAD_PROBE_OK;
}

/* Four 1280x720 H.264 sources of 300 frames, each looped 10 times (3000 frames per source), through rdsvideodec,
 * rdsstreammux (batch-size 4) and rdsstreamdemux into fakesinks: VmRSS grows by less than 8 MiB and the process's
 * GPU memory moves by less than 16 MiB between the end of loop 2 and the end of loop 10. Reports the fps. */
static void
test_mux_soak (void)
{
  const gchar *need[] = { "vah264enc", "h264parse", NULL };
  MuxSoakStats st = { 0 };
  guint out[SOAK_SOURCES] = { 0 };
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *mux, *src[SOAK_SOURCES];
  GError *err = NULL;
  GstPad *pad;
  GBytes *stream;
  gint64 t0, t1;
  gssize rss_growth, gpu_delta;
  gdouble secs;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  stream = rds_test_encode ("h264", SOAK_W, SOAK_H, SOAK_FRAMES, 2, "ball");
  g_assert_nonnull (stream);

  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_string_append_printf (desc, "appsrc name=s%u caps=video/x-h264,stream-format=byte-stream format=bytes "
        "! h264parse ! rdsvideodec ! m.sink_%u d.src_%u ! queue ! fakesink name=o%u " BRANCH_SINK_PROPS " ",
        i, i, i, i);
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u ! rdsstreamdemux name=d",
      SOAK_W, SOAK_H, SOAK_SOURCES);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  mux = get_by_name (pipe, "m", 0);
  pad = gst_element_get_static_pad (mux, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, mux_soak_probe, &st, NULL);
  gst_object_unref (pad);
  for (guint i = 0; i < SOAK_SOURCES; i++) {
    GstElement *sink = get_by_name (pipe, "o%u", i);

    src[i] = get_by_name (pipe, "s%u", i);
    pad = gst_element_get_static_pad (sink, "sink");
    gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe, &out[i], NULL);
    gst_object_unref (pad);
    gst_object_unref (sink);
  }

  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* each loop is a complete stream (SPS/PPS + IDR first); the buffers share the encoded bytes */
  for (guint l = 0; l < SOAK_LOOPS; l++)
    for (guint i = 0; i < SOAK_SOURCES; i++)
      g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), gst_buffer_new_wrapped_bytes (stream)), ==,
          GST_FLOW_OK);
  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src[i])), ==, GST_FLOW_OK);
  wait_eos (pipe, 280 * GST_SECOND, NULL, 0);
  t1 = g_get_monotonic_time ();
  gst_element_set_state (pipe, GST_STATE_NULL);
  for (guint i = 0; i < SOAK_SOURCES; i++)
    gst_object_unref (src[i]);
  gst_object_unref (mux);
  gst_object_unref (pipe);
  g_bytes_unref (stream);

  secs = (t1 - t0) / 1e6;
  for (guint i = 0; i < SOAK_LOOPS; i++)
    g_test_message ("loop %2u: VmRSS %.1f MiB, GPU used %.1f MiB", i + 1, MIB (st.rss[i]), MIB (st.gpu_used[i]));
  rss_growth = (gssize) st.rss[SOAK_LOOPS - 1] - (gssize) st.rss[1];
  gpu_delta = (gssize) st.gpu_used[SOAK_LOOPS - 1] - (gssize) st.gpu_used[1];
  g_test_message ("%u frames in %u batches in %.1f s (%.0f fps, %.0f fps per source); VmRSS growth loop 2 -> %u: "
      "%+.2f MiB; GPU used delta: %+.2f MiB", st.frames, st.batches, secs, st.frames / secs,
      st.frames / secs / SOAK_SOURCES, SOAK_LOOPS, MIB (rss_growth), MIB (gpu_delta));
  /* without a timeout every batch has a frame of each source */
  g_assert_cmpuint (st.batches, ==, SOAK_FRAMES * SOAK_LOOPS);
  g_assert_cmpuint (st.frames, ==, SOAK_SOURCES * SOAK_FRAMES * SOAK_LOOPS);
  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_assert_cmpuint (out[i], ==, SOAK_FRAMES * SOAK_LOOPS);
  g_assert_cmpint (rss_growth, <, 8 * 1024 * 1024);
  g_assert_cmpint (ABS (gpu_delta), <, 16 * 1024 * 1024);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/gpu/mux-pipeline/four-sources", test_four_sources);
  g_test_add_func ("/gpu/mux-pipeline/stall", test_stall);
  g_test_add_func ("/gpu/mux-pipeline/dynamic", test_dynamic);
  g_test_add_func ("/gpu/mux-soak/four-sources", test_mux_soak);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_deinit ();
    return ret;
  }
}
