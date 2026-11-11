/* rdsh264enc and rdsh265enc in pipelines (gpu): a round trip per codec, videotestsrc -> rdsvideoconvert (memory:RDS) ->
 * the encoder -> h26xparse -> rdsvideodec -> system NV12, with the frame count and every frame's PSNR against the
 * source frame of the same PTS; two sources through rdsstreammux and rdsstreamdemux into one encoder each; and (its own
 * test entry, in the soak suite) 3000 1080p frames per codec with host and GPU memory checked for growth
 * (rds-test-soak.h) and, with RDS_TEST_ENC_MIN_FPS set, the fps against that floor. Every test skips without a ROCm
 * device or the VA encoder, parser and decoder it needs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"
#include "rds-test-soak.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  const gchar *codec;           /* "h264" or "h265" */
  const gchar *element;
  const gchar *va_factory;
} EncCodec;

static const EncCodec codecs[] = {
  {"h264", "rdsh264enc", "vah264enc"},
  {"h265", "rdsh265enc", "vah265enc"},
};

#define SINK_PROPS "sync=false enable-last-sample=false"

/* Skips the test (TRUE: go on) without a ROCm device, @c's VA encoder and parser, and the extra elements in @more
 * (NULL-terminated, may be NULL). */
static gboolean
require (const EncCodec * c, const gchar * const *more)
{
  gchar *parse;
  gboolean ok;

  if (!rds_test_have_gpu ()) {
    g_test_skip ("no ROCm device visible");
    return FALSE;
  }
  parse = g_strdup_printf ("%sparse", c->codec);
  ok = rds_test_have_element (c->va_factory) && rds_test_have_element (parse);
  for (; ok && more != NULL && *more != NULL; more++)
    ok = rds_test_have_element (*more);
  if (!ok) {
    gchar *msg = g_strdup_printf ("%s, %s or another element not available", c->va_factory, parse);

    g_test_skip (msg);
    g_free (msg);
  }
  g_free (parse);
  return ok;
}

/* Waits for EOS on the bus; fails on an error or after @timeout. */
static void
wait_eos (GstElement * pipe, GstClockTime timeout)
{
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, timeout, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

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

static GstElement *
get_by_name (GstElement * pipe, const gchar * name)
{
  GstElement *e = gst_bin_get_by_name (GST_BIN (pipe), name);

  g_assert_nonnull (e);
  return e;
}

static void
add_probe (GstElement * pipe, const gchar * element, GstPadProbeType mask, GstPadProbeCallback cb, gpointer data)
{
  GstElement *e = get_by_name (pipe, element);
  GstPad *pad = gst_element_get_static_pad (e, "sink");

  gst_pad_add_probe (pad, mask, cb, data, NULL);
  gst_object_unref (pad);
  gst_object_unref (e);
}

/* ---- round trip ---- */

#define RT_W 1280
#define RT_H 720
#define RT_FRAMES 300
#define RT_BITRATE 8000000      /* bit/s, constant_bitrate (the bins' default rate control) */
#define RT_MIN_PSNR 35.0
/* PSNR of identical planes, which has no finite value */
#define PSNR_IDENTICAL 99.0

/* One NV12 frame, packed (no padding): Y then interleaved UV. */
typedef struct
{
  GstClockTime pts;
  guint8 *data;
} PackedFrame;

static void
packed_frame_free (PackedFrame * f)
{
  g_free (f->data);
  g_free (f);
}

typedef struct
{
  GMutex lock;
  /* frames waiting for their counterpart from the other appsink, by PTS */
  GHashTable *pending[2];       /* 0: source, 1: decoded */
  guint count[2];
  GArray *psnr_y, *psnr_uv;
} RoundTrip;

static gdouble
psnr (const guint8 * a, const guint8 * b, gsize n)
{
  guint64 sse = 0;

  for (gsize i = 0; i < n; i++) {
    gint d = (gint) a[i] - (gint) b[i];

    sse += (guint64) (d * d);
  }
  if (sse == 0)
    return PSNR_IDENTICAL;
  return 10.0 * log10 (255.0 * 255.0 * (gdouble) n / (gdouble) sse);
}

/* appsink new-sample for both sinks (@side 0: source, 1: decoded): packs the frame, then compares it with the other
 * side's frame of the same PTS if that one is already in, or keeps it until it comes. */
static GstFlowReturn
round_trip_sample (GstAppSink * sink, gpointer user_data, guint side)
{
  RoundTrip *rt = user_data;
  GstSample *s = gst_app_sink_pull_sample (sink);
  GstVideoInfo info;
  GstVideoFrame vf;
  PackedFrame *f = g_new0 (PackedFrame, 1), *other;
  const gsize y_size = RT_W * RT_H;

  g_assert_nonnull (s);
  g_assert_true (gst_video_info_from_caps (&info, gst_sample_get_caps (s)));
  g_assert_cmpint (GST_VIDEO_INFO_FORMAT (&info), ==, GST_VIDEO_FORMAT_NV12);
  g_assert_cmpint (GST_VIDEO_INFO_WIDTH (&info), ==, RT_W);
  g_assert_cmpint (GST_VIDEO_INFO_HEIGHT (&info), ==, RT_H);
  g_assert_true (gst_video_frame_map (&vf, &info, gst_sample_get_buffer (s), GST_MAP_READ));
  f->pts = GST_BUFFER_PTS (gst_sample_get_buffer (s));
  g_assert_true (GST_CLOCK_TIME_IS_VALID (f->pts));
  f->data = g_malloc (y_size * 3 / 2);
  for (guint r = 0; r < RT_H; r++)
    memcpy (f->data + (gsize) r * RT_W, (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&vf, 0)
        + (gsize) r * GST_VIDEO_FRAME_PLANE_STRIDE (&vf, 0), RT_W);
  for (guint r = 0; r < RT_H / 2; r++)
    memcpy (f->data + y_size + (gsize) r * RT_W, (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&vf, 1)
        + (gsize) r * GST_VIDEO_FRAME_PLANE_STRIDE (&vf, 1), RT_W);
  gst_video_frame_unmap (&vf);
  gst_sample_unref (s);

  g_mutex_lock (&rt->lock);
  rt->count[side]++;
  other = g_hash_table_lookup (rt->pending[!side], &f->pts);
  if (other != NULL) {
    gdouble py = psnr (f->data, other->data, y_size);
    gdouble puv = psnr (f->data + y_size, other->data + y_size, y_size / 2);

    g_array_append_val (rt->psnr_y, py);
    g_array_append_val (rt->psnr_uv, puv);
    g_hash_table_remove (rt->pending[!side], &f->pts);
    packed_frame_free (f);
  } else {
    if (g_hash_table_contains (rt->pending[side], &f->pts))
      g_error ("%s: two frames with PTS %" GST_TIME_FORMAT, side ? "decoded" : "source", GST_TIME_ARGS (f->pts));
    g_hash_table_insert (rt->pending[side], &f->pts, f);
  }
  g_mutex_unlock (&rt->lock);
  return GST_FLOW_OK;
}

static GstFlowReturn
source_sample (GstAppSink * sink, gpointer user_data)
{
  return round_trip_sample (sink, user_data, 0);
}

static GstFlowReturn
decoded_sample (GstAppSink * sink, gpointer user_data)
{
  return round_trip_sample (sink, user_data, 1);
}

static gint
cmp_double (gconstpointer a, gconstpointer b)
{
  gdouble x = *(const gdouble *) a, y = *(const gdouble *) b;

  return x < y ? -1 : x > y;
}

/* Sorts @v and logs its distribution; returns the minimum. */
static gdouble
log_psnr (const gchar * what, GArray * v)
{
  gdouble *d = (gdouble *) v->data;
  guint n = v->len, below = 0;

  g_array_sort (v, cmp_double);
  for (guint i = 0; i < n; i++)
    below += d[i] < RT_MIN_PSNR;
  g_test_message ("%s PSNR over %u frames: min %.2f dB, 5th percentile %.2f, median %.2f, max %.2f; %u below %.0f dB",
      what, n, d[0], d[n / 20], d[n / 2], d[n - 1], below, RT_MIN_PSNR);
  return d[0];
}

/* videotestsrc (smpte, moving horizontally) -> tee: one branch straight into an appsink (the source frames), the
 * other through rdsvideoconvert to memory:RDS, the encoder (8 Mbit/s CBR), h26xparse, rdsvideodec and rdsvideoconvert
 * back to system NV12. Every source frame comes back once, with its PTS, and every frame's Y and UV PSNR is at least
 * 35 dB. */
static void
test_round_trip (gconstpointer data)
{
  const EncCodec *c = data;
  const gchar *more[] = { "rdsvideodec", NULL };
  GstAppSinkCallbacks src_cb = { 0 }, dec_cb = { 0 };
  RoundTrip rt = { 0 };
  GstElement *pipe, *sink;
  GError *err = NULL;
  gchar *desc;
  gdouble min_y, min_uv;

  if (!require (c, more))
    return;
  g_mutex_init (&rt.lock);
  for (guint i = 0; i < 2; i++)
    rt.pending[i] = g_hash_table_new_full (g_int64_hash, g_int64_equal, NULL, (GDestroyNotify) packed_frame_free);
  rt.psnr_y = g_array_new (FALSE, FALSE, sizeof (gdouble));
  rt.psnr_uv = g_array_new (FALSE, FALSE, sizeof (gdouble));

  desc = g_strdup_printf ("videotestsrc pattern=smpte horizontal-speed=4 num-buffers=%u "
      "! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 ! tee name=t "
      "t. ! queue ! appsink name=src_frames " SINK_PROPS " "
      "t. ! queue ! rdsvideoconvert ! video/x-raw(memory:RDS),format=NV12 ! %s bitrate=%u ! %sparse ! rdsvideodec "
      "! rdsvideoconvert ! video/x-raw,format=NV12 ! appsink name=dec_frames " SINK_PROPS,
      RT_FRAMES, RT_W, RT_H, c->element, RT_BITRATE, c->codec);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  src_cb.new_sample = source_sample;
  dec_cb.new_sample = decoded_sample;
  sink = get_by_name (pipe, "src_frames");
  gst_app_sink_set_callbacks (GST_APP_SINK (sink), &src_cb, &rt, NULL);
  gst_object_unref (sink);
  sink = get_by_name (pipe, "dec_frames");
  gst_app_sink_set_callbacks (GST_APP_SINK (sink), &dec_cb, &rt, NULL);
  gst_object_unref (sink);

  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  wait_eos (pipe, 120 * GST_SECOND);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);

  g_test_message ("%s at %u bit/s CBR: %u source frames, %u decoded", c->element, RT_BITRATE, rt.count[0],
      rt.count[1]);
  g_assert_cmpuint (rt.count[0], ==, RT_FRAMES);
  g_assert_cmpuint (rt.count[1], ==, RT_FRAMES);
  /* every frame found its counterpart by PTS */
  g_assert_cmpuint (g_hash_table_size (rt.pending[0]), ==, 0);
  g_assert_cmpuint (g_hash_table_size (rt.pending[1]), ==, 0);
  g_assert_cmpuint (rt.psnr_y->len, ==, RT_FRAMES);
  min_y = log_psnr ("Y", rt.psnr_y);
  min_uv = log_psnr ("UV", rt.psnr_uv);
  g_assert_cmpfloat (min_y, >=, RT_MIN_PSNR);
  g_assert_cmpfloat (min_uv, >=, RT_MIN_PSNR);

  g_array_unref (rt.psnr_uv);
  g_array_unref (rt.psnr_y);
  for (guint i = 0; i < 2; i++)
    g_hash_table_unref (rt.pending[i]);
  g_mutex_clear (&rt.lock);
}

/* ---- two sources through the mux and demux ---- */

#define MS_W 1280
#define MS_H 720
#define MS_FRAMES 90

typedef struct
{
  guint frames, key_frames, eos;
  gboolean first_key;           /* whether the first buffer was a key frame */
  GstClockTime last_pts;
  gboolean pts_ordered;
} BranchStats;

static GstPadProbeReturn
branch_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  BranchStats *b = user_data;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
    gboolean key = !GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);

    if (b->frames == 0)
      b->first_key = key;
    b->frames++;
    if (key)
      b->key_frames++;
    /* no B frames: output order is presentation order */
    if (GST_CLOCK_TIME_IS_VALID (b->last_pts) && GST_BUFFER_PTS (buf) <= b->last_pts)
      b->pts_ordered = FALSE;
    b->last_pts = GST_BUFFER_PTS (buf);
  } else if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) == GST_EVENT_EOS) {
    b->eos++;
  }
  return GST_PAD_PROBE_OK;
}

/* Two videotestsrc sources (memory:RDS through rdsvideoconvert) -> rdsstreammux (batch-size 2) -> rdsstreamdemux ->
 * per source a queue, the encoder (rdsh264enc for source 0, rdsh265enc for source 1), h26xparse and a fakesink: each
 * branch gets all its frames in PTS order, a key frame first and no other (the default idrinterval, 256, is longer
 * than the stream), then one EOS, and its caps are the codec's at the mux's size. */
static void
test_multi_source (void)
{
  const gchar *more[] = { "vah265enc", "h265parse", "rdsstreammux", "rdsstreamdemux", NULL };
  BranchStats st[2];
  GString *desc = g_string_new (NULL);
  GstElement *pipe;
  GError *err = NULL;

  if (!require (&codecs[0], more))
    return;
  g_string_append_printf (desc, "rdsstreammux name=m batch-size=2 width=%u height=%u ! rdsstreamdemux name=d ",
      MS_W, MS_H);
  for (guint i = 0; i < 2; i++) {
    g_string_append_printf (desc, "videotestsrc pattern=%s num-buffers=%u "
        "! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 ! rdsvideoconvert "
        "! video/x-raw(memory:RDS),format=NV12 ! m.sink_%u "
        "d.src_%u ! queue ! %s ! %sparse ! fakesink name=o%u " SINK_PROPS " ",
        i == 0 ? "ball" : "smpte horizontal-speed=4", MS_FRAMES, i == 0 ? 640 : MS_W, i == 0 ? 360 : MS_H, i, i,
        codecs[i].element, codecs[i].codec, i);
    st[i] = (BranchStats) {.last_pts = GST_CLOCK_TIME_NONE, .pts_ordered = TRUE };
  }
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  for (guint i = 0; i < 2; i++) {
    gchar name[4];

    g_snprintf (name, sizeof (name), "o%u", i);
    add_probe (pipe, name, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, branch_probe, &st[i]);
  }

  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  wait_eos (pipe, 120 * GST_SECOND);
  for (guint i = 0; i < 2; i++) {
    gchar name[4];
    GstElement *sink;
    GstPad *pad;
    GstCaps *caps;
    GstStructure *s;
    gchar *media = g_strdup_printf ("video/x-%s", codecs[i].codec), *caps_str;
    gint w = 0, h = 0;

    g_snprintf (name, sizeof (name), "o%u", i);
    sink = get_by_name (pipe, name);
    pad = gst_element_get_static_pad (sink, "sink");
    caps = gst_pad_get_current_caps (pad);
    g_assert_nonnull (caps);
    caps_str = gst_caps_to_string (caps);
    g_test_message ("o%u: %u frames, %u key frames, %u EOS; caps %s", i, st[i].frames, st[i].key_frames, st[i].eos,
        caps_str);
    g_free (caps_str);
    g_assert_cmpuint (gst_caps_get_size (caps), ==, 1);
    s = gst_caps_get_structure (caps, 0);
    g_assert_true (gst_structure_has_name (s, media));
    g_assert_true (gst_structure_get_int (s, "width", &w));
    g_assert_true (gst_structure_get_int (s, "height", &h));
    g_assert_cmpint (w, ==, MS_W);
    g_assert_cmpint (h, ==, MS_H);
    g_free (media);
    gst_caps_unref (caps);
    gst_object_unref (pad);
    gst_object_unref (sink);
    g_assert_cmpuint (st[i].frames, ==, MS_FRAMES);
    g_assert_true (st[i].first_key);
    g_assert_cmpuint (st[i].key_frames, ==, 1);
    g_assert_cmpuint (st[i].eos, ==, 1);
    g_assert_true (st[i].pts_ordered);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
}

/* ---- soak ---- */

#define SOAK_W 1920
#define SOAK_H 1080
#define SOAK_FRAMES 3000
#define SOAK_SAMPLE_EVERY 300   /* frames between memory samples */
#define SOAK_SAMPLES (SOAK_FRAMES / SOAK_SAMPLE_EVERY)

typedef struct
{
  guint frames;
  gsize host[SOAK_SAMPLES], gpu_used[SOAK_SAMPLES];
} EncSoakStats;

/* On the fakesink: counts encoded frames and samples memory use every SOAK_SAMPLE_EVERY frames. */
static GstPadProbeReturn
soak_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  EncSoakStats *st = user_data;

  st->frames++;
  if (st->frames % SOAK_SAMPLE_EVERY == 0 && st->frames / SOAK_SAMPLE_EVERY <= SOAK_SAMPLES) {
    guint i = st->frames / SOAK_SAMPLE_EVERY - 1;

    st->host[i] = host_memory_bytes ();
    st->gpu_used[i] = gpu_used_bytes ();
  }
  return GST_PAD_PROBE_OK;
}

/* With RDS_TEST_ENC_MIN_FPS set (a number), @fps must reach it; unset, the fps is only reported. It is meant for
 * release builds (the bins run at ~200 fps at 1080p on the test iGPU); debug and sanitized builds are slower. */
static void
check_min_fps (const EncCodec * c, gdouble fps)
{
  const gchar *env = g_getenv ("RDS_TEST_ENC_MIN_FPS"), *plugins = g_getenv ("GST_PLUGIN_PATH_1_0");
  gchar *end = NULL;
  gdouble min;

  if (env == NULL || *env == '\0')
    return;
  min = g_ascii_strtod (env, &end);
  if (end == env || *end != '\0')
    g_error ("RDS_TEST_ENC_MIN_FPS=%s is not a number", env);
  if (fps < min)
    g_error ("%s: %.0f fps at %ux%u, below RDS_TEST_ENC_MIN_FPS=%s (build: the plugins in %s)", c->element, fps,
        SOAK_W, SOAK_H, env, plugins != NULL ? plugins : "?");
}

/* 3000 1920x1080 frames of videotestsrc (smpte, moving) -> rdsvideoconvert (memory:RDS) -> the encoder (its default
 * 4 Mbit/s CBR) -> h26xparse -> fakesink: every frame comes out, VmRSS (under ASan, the live heap) grows by less than
 * 8 MiB and the process's GPU memory moves by less than 16 MiB between frame 600 and frame 3000. Reports the fps, and
 * checks it against RDS_TEST_ENC_MIN_FPS when that is set (check_min_fps). */
static void
test_soak (gconstpointer data)
{
  const EncCodec *c = data;
  EncSoakStats st = { 0 };
  GstElement *pipe;
  GError *err = NULL;
  gchar *desc;
  gint64 t0, t1;
  gssize host_growth, gpu_delta;
  gdouble secs;

  if (!require (c, NULL))
    return;
  desc = g_strdup_printf ("videotestsrc pattern=smpte horizontal-speed=4 num-buffers=%u "
      "! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 ! rdsvideoconvert "
      "! video/x-raw(memory:RDS),format=NV12 ! %s ! %sparse ! fakesink name=sink " SINK_PROPS,
      SOAK_FRAMES, SOAK_W, SOAK_H, c->element, c->codec);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  add_probe (pipe, "sink", GST_PAD_PROBE_TYPE_BUFFER, soak_probe, &st);

  /* preroll (the VA encoder's open and the first frame) is not timed */
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_element_get_state (pipe, NULL, NULL, 30 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);
  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  wait_eos (pipe, 280 * GST_SECOND);
  t1 = g_get_monotonic_time ();
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);

  secs = (t1 - t0) / 1e6;
  for (guint i = 0; i < SOAK_SAMPLES; i++)
    g_test_message ("frame %4u: " RDS_TEST_HOST_MEMORY_NAME " %.1f MiB, GPU used %.1f MiB",
        (i + 1) * SOAK_SAMPLE_EVERY, MIB (st.host[i]), MIB (st.gpu_used[i]));
  host_growth = (gssize) st.host[SOAK_SAMPLES - 1] - (gssize) st.host[1];
  gpu_delta = (gssize) st.gpu_used[SOAK_SAMPLES - 1] - (gssize) st.gpu_used[1];
  g_test_message ("%s: %u %ux%u frames in %.1f s (%.0f fps); " RDS_TEST_HOST_MEMORY_NAME " growth frame %u -> %u: "
      "%+.2f MiB; GPU used delta: %+.2f MiB", c->element, st.frames, SOAK_W, SOAK_H, secs, st.frames / secs,
      2 * SOAK_SAMPLE_EVERY, SOAK_FRAMES, MIB (host_growth), MIB (gpu_delta));
  g_assert_cmpuint (st.frames, ==, SOAK_FRAMES);
  g_assert_cmpint (host_growth, <, 8 * 1024 * 1024);
  g_assert_cmpint (ABS (gpu_delta), <, 16 * 1024 * 1024);
  check_min_fps (c, st.frames / secs);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_data_func ("/gpu/enc-pipeline/round-trip-h264", &codecs[0], test_round_trip);
  g_test_add_data_func ("/gpu/enc-pipeline/round-trip-h265", &codecs[1], test_round_trip);
  g_test_add_func ("/gpu/enc-pipeline/multi-source", test_multi_source);
  g_test_add_data_func ("/gpu/enc-soak/h264", &codecs[0], test_soak);
  g_test_add_data_func ("/gpu/enc-soak/h265", &codecs[1], test_soak);
  ret = g_test_run ();
  /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at process
   * exit otherwise */
  gst_deinit ();
  return ret;
}
