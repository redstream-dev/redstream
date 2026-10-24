/* Decode + convert integration (gpu): decodebin autoplugging picks rdsvideodec, the system-memory fallback works
 * behind videoconvert, rdsvideodec ! rdsvideoconvert matches vah264dec plus a CPU reference conversion, and a long
 * decode/convert run (the soak, its own test entry) neither leaks host nor GPU memory. Every test skips without a ROCm
 * device or the stock elements it needs (VA encoder/decoder, mp4mux, qtdemux, uridecodebin3).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"
#include "rds-test-soak.h"
#include "transform-ref.h"

#include <glib/gstdio.h>
#include <rds/rds.h>
#include <stdlib.h>
#include <string.h>

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

/* An H.264 clip in an MP4 (avc stream format, as a demuxer hands it to decodebin) in a fresh temporary directory.
 * Free with clip_free, which deletes both. */
typedef struct
{
  gchar *dir, *path, *uri;
} Mp4Clip;

static Mp4Clip *
clip_new (guint width, guint height, guint frames, guint b_frames)
{
  Mp4Clip *c = g_new0 (Mp4Clip, 1);
  GError *err = NULL;
  GstElement *pipe;
  GstBus *bus;
  GstMessage *msg;
  gchar *desc;

  c->dir = g_dir_make_tmp ("rds-pipeline-XXXXXX", &err);
  g_assert_no_error (err);
  c->path = g_build_filename (c->dir, "clip.mp4", NULL);
  c->uri = g_filename_to_uri (c->path, NULL, &err);
  g_assert_no_error (err);
  desc = g_strdup_printf ("videotestsrc pattern=ball num-buffers=%u "
      "! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 ! vah264enc b-frames=%u ! h264parse "
      "! video/x-h264,stream-format=avc,alignment=au ! mp4mux ! filesink location=\"%s\"",
      frames, width, height, b_frames, c->path);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  bus = gst_element_get_bus (pipe);
  msg = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  gst_object_unref (bus);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
  g_free (desc);
  return c;
}

static void
clip_free (Mp4Clip * c)
{
  g_assert_cmpint (g_unlink (c->path), ==, 0);
  g_assert_cmpint (g_rmdir (c->dir), ==, 0);
  g_free (c->uri);
  g_free (c->path);
  g_free (c->dir);
  g_free (c);
}

/* Factory names of every element inside @bin, recursively, as one space-separated string. */
static gchar *
bin_factories (GstBin * bin)
{
  GstIterator *it = gst_bin_iterate_recurse (bin);
  GString *names = g_string_new (NULL);
  GValue item = G_VALUE_INIT;
  gboolean done = FALSE;

  while (!done) {
    switch (gst_iterator_next (it, &item)) {
      case GST_ITERATOR_OK:{
        GstElementFactory *f = gst_element_get_factory (g_value_get_object (&item));

        g_string_append_printf (names, " %s", f ? GST_OBJECT_NAME (f) : "?");
        g_value_reset (&item);
        break;
      }
      case GST_ITERATOR_RESYNC:
        g_string_truncate (names, 0);
        gst_iterator_resync (it);
        break;
      case GST_ITERATOR_ERROR:
        g_assert_not_reached ();
        break;
      case GST_ITERATOR_DONE:
        done = TRUE;
        break;
    }
  }
  g_value_unset (&item);
  gst_iterator_free (it);
  g_string_append_c (names, ' ');
  return g_string_free (names, FALSE);
}

static gboolean
bin_has_factory (GstBin * bin, const gchar * factory)
{
  gchar *names = bin_factories (bin), *needle = g_strdup_printf (" %s ", factory);
  gboolean ret = strstr (names, needle) != NULL;

  g_test_message ("elements:%s", names);
  g_free (needle);
  g_free (names);
  return ret;
}

/* Runs `@decodebin uri=… ! @tail ! appsink` to EOS; every sample's caps must intersect @expect (may be NULL).
 * Returns the number of frames and whether an rdsvideodec was plugged. */
static guint
run_decodebin (const gchar * decodebin, const gchar * uri, const gchar * tail, const gchar * expect,
    gboolean * plugged_rds)
{
  GstElement *pipe, *sink;
  GstCaps *want = expect ? gst_caps_from_string (expect) : NULL;
  GError *err = NULL;
  GstSample *s;
  guint n = 0;
  gchar *desc;

  desc = g_strdup_printf ("%s uri=%s ! %s ! appsink name=sink sync=false", decodebin, uri, tail);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    if (want != NULL && !gst_caps_is_subset (gst_sample_get_caps (s), want)) {
      gchar *got = gst_caps_to_string (gst_sample_get_caps (s));

      g_error ("%s: unexpected caps %s", decodebin, got);
    }
    gst_sample_unref (s);
    n++;
  }
  (void) rds_test_bus_check (pipe);
  /* decodebin keeps its children until it goes back to READY */
  *plugged_rds = bin_has_factory (GST_BIN (pipe), "rdsvideodec");
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (pipe);
  if (want != NULL)
    gst_caps_unref (want);
  g_free (desc);
  return n;
}

/* uridecodebin / uridecodebin3 autoplug rdsvideodec (rank primary + 10, above vah264dec) for an MP4 H.264 file,
 * and its frames reach a memory:RDS capsfilter. */
static void
test_autoplug (gconstpointer data)
{
  const gchar *decodebin = data;
  const gchar *need[] = { "vah264enc", "mp4mux", "qtdemux", "h264parse", decodebin, NULL };
  gboolean plugged = FALSE;
  Mp4Clip *clip;
  guint n;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  clip = clip_new (320, 240, 30, 2);
  n = run_decodebin (decodebin, clip->uri, "video/x-raw(memory:RDS),format=NV12",
      "video/x-raw(memory:RDS),format=NV12,width=320,height=240", &plugged);
  g_assert_cmpuint (n, ==, 30);
  g_assert_true (plugged);
  clip_free (clip);
}

/* Behind a system-memory-only element the autoplugged decoder still works: rdsvideodec falls back to system NV12
 * (or decodebin picks another decoder; either is fine, only success and the frame count are asserted). */
static void
test_fallback (gconstpointer data)
{
  const gchar *decodebin = data;
  const gchar *need[] = { "vah264enc", "mp4mux", "qtdemux", "h264parse", "videoconvert", decodebin, NULL };
  gboolean plugged = FALSE;
  Mp4Clip *clip;
  guint n;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  clip = clip_new (320, 240, 30, 2);
  n = run_decodebin (decodebin, clip->uri, "videoconvert ! video/x-raw,format=RGBA",
      "video/x-raw,format=RGBA,width=320,height=240", &plugged);
  g_assert_cmpuint (n, ==, 30);
  g_test_message ("%s picked %s", decodebin, plugged ? "rdsvideodec" : "another decoder");
  clip_free (clip);
}

/* A raw H.264 byte stream that changes size midway (two streams back to back), through uridecodebin into a
 * system-memory-only converter: every frame comes out, with the size of its part. The size change renegotiates
 * the decoder's output while the decodebin pad is already linked. */
static void
test_size_change_fallback (void)
{
  const gchar *need[] = { "vah264enc", "h264parse", "videoconvert", "uridecodebin", NULL };
  GBytes *parts[2], *both;
  GstElement *pipe, *sink;
  GError *err = NULL;
  GstSample *s;
  gchar *dir, *path, *uri, *desc;
  guint n = 0;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  parts[0] = rds_test_encode ("h264", 320, 240, 10, 2, "ball");
  parts[1] = rds_test_encode ("h264", 640, 360, 10, 2, "ball");
  both = rds_test_concat (parts, 2);
  dir = g_dir_make_tmp ("rds-pipeline-XXXXXX", &err);
  g_assert_no_error (err);
  path = g_build_filename (dir, "clip.h264", NULL);
  g_assert_true (g_file_set_contents (path, g_bytes_get_data (both, NULL), g_bytes_get_size (both), &err));
  g_assert_no_error (err);
  uri = g_filename_to_uri (path, NULL, &err);
  g_assert_no_error (err);

  desc = g_strdup_printf ("uridecodebin uri=%s ! videoconvert ! video/x-raw,format=RGBA ! appsink name=sink "
      "sync=false", uri);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstStructure *st = gst_caps_get_structure (gst_sample_get_caps (s), 0);
    gint w = 0, h = 0;

    g_assert_true (gst_structure_get_int (st, "width", &w) && gst_structure_get_int (st, "height", &h));
    g_assert_cmpint (w, ==, n < 10 ? 320 : 640);
    g_assert_cmpint (h, ==, n < 10 ? 240 : 360);
    gst_sample_unref (s);
    n++;
  }
  (void) rds_test_bus_check (pipe);
  g_assert_true (bin_has_factory (GST_BIN (pipe), "rdsvideodec"));
  g_assert_cmpuint (n, ==, 20);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (pipe);

  g_assert_cmpint (g_unlink (path), ==, 0);
  g_assert_cmpint (g_rmdir (dir), ==, 0);
  g_free (desc);
  g_free (uri);
  g_free (path);
  g_free (dir);
  g_bytes_unref (both);
  g_bytes_unref (parts[1]);
  g_bytes_unref (parts[0]);
}

/* ---- end to end ---- */

#define E2E_W 640
#define E2E_H 360

/* Every frame of `filesrc ! qtdemux ! h264parse ! @tail ! appsink`, mapped by @cb. */
typedef void (*FrameFunc) (const GstVideoFrame * frame, gpointer user_data);

static guint
run_file (const gchar * path, const gchar * tail, FrameFunc cb, gpointer user_data)
{
  GstElement *pipe, *sink;
  GError *err = NULL;
  GstSample *s;
  guint n = 0;
  gchar *desc;

  desc = g_strdup_printf ("filesrc location=\"%s\" ! qtdemux ! h264parse ! %s ! appsink name=sink sync=false",
      path, tail);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstVideoInfo info;
    GstVideoFrame frame;

    g_assert_true (gst_video_info_from_caps (&info, gst_sample_get_caps (s)));
    g_assert_true (gst_video_frame_map (&frame, &info, gst_sample_get_buffer (s), GST_MAP_READ));
    cb (&frame, user_data);
    gst_video_frame_unmap (&frame);
    gst_sample_unref (s);
    n++;
  }
  (void) rds_test_bus_check (pipe);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (pipe);
  g_free (desc);
  return n;
}

/* Keeps a tightly packed copy of each RGBA E2E_W x E2E_H frame. */
static void
keep_rgba (const GstVideoFrame * frame, gpointer user_data)
{
  GPtrArray *frames = user_data;
  guint8 *copy = g_malloc ((gsize) E2E_W * E2E_H * 4);

  g_assert_cmpint (GST_VIDEO_FRAME_FORMAT (frame), ==, GST_VIDEO_FORMAT_RGBA);
  g_assert_cmpint (GST_VIDEO_FRAME_WIDTH (frame), ==, E2E_W);
  g_assert_cmpint (GST_VIDEO_FRAME_HEIGHT (frame), ==, E2E_H);
  for (guint y = 0; y < E2E_H; y++)
    memcpy (copy + (gsize) y * E2E_W * 4, (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (frame, 0)
        + (gsize) y * GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), E2E_W * 4);
  g_ptr_array_add (frames, copy);
}

typedef struct
{
  GPtrArray *got;               /* rdsvideodec ! rdsvideoconvert frames, packed RGBA */
  guint index;
  guint max_diff;               /* over all frames */
  guint8 *ref;                  /* scratch: the reference conversion of the current frame */
} E2eCheck;

/* Converts a vah264dec NV12 frame on the CPU (nearest, whole frame) and compares it with the element's frame. */
static void
check_against_ref (const GstVideoFrame * frame, gpointer user_data)
{
  E2eCheck *c = user_data;
  RefImage src = { 0 }, dst = { 0 };
  const guint8 *got;
  guint max_diff = 0;

  g_assert_cmpint (GST_VIDEO_FRAME_FORMAT (frame), ==, GST_VIDEO_FORMAT_NV12);
  g_assert_cmpuint (c->index, <, c->got->len);
  src.format = RDS_BUF_COLOR_FORMAT_NV12;
  src.width = GST_VIDEO_FRAME_WIDTH (frame);
  src.height = GST_VIDEO_FRAME_HEIGHT (frame);
  for (guint p = 0; p < 2; p++) {
    src.plane[p] = (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (frame, p);
    src.pitch[p] = GST_VIDEO_FRAME_PLANE_STRIDE (frame, p);
  }
  dst.format = RDS_BUF_COLOR_FORMAT_RGBA;
  dst.width = E2E_W;
  dst.height = E2E_H;
  dst.plane[0] = c->ref;
  dst.pitch[0] = E2E_W * 4;
  ref_transform (&src, &dst, (RefRect) {
      0, 0, src.width, src.height}, (RefRect) {
      0, 0, E2E_W, E2E_H}, FALSE);

  got = c->got->pdata[c->index];
  for (gsize i = 0; i < (gsize) E2E_W * E2E_H * 4; i++) {
    guint d = (guint) ABS ((gint) got[i] - (gint) c->ref[i]);

    if (d > max_diff)
      max_diff = d;
  }
  if (max_diff > 1)
    g_error ("frame %u: max |diff| %u against the CPU reference", c->index, max_diff);
  c->max_diff = MAX (c->max_diff, max_diff);
  c->index++;
}

/* filesrc ! qtdemux ! h264parse ! rdsvideodec ! rdsvideoconvert ! RGBA 640x360 equals vah264dec (system NV12)
 * converted by the CPU reference (nearest, the converter's default), within 1 per channel, on every frame. */
static void
test_end_to_end (void)
{
  const gchar *need[] = { "vah264enc", "vah264dec", "mp4mux", "qtdemux", "h264parse", NULL };
  E2eCheck check = { 0 };
  Mp4Clip *clip;
  guint n;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  clip = clip_new (1280, 720, 30, 2);
  check.got = g_ptr_array_new_with_free_func (g_free);
  check.ref = g_malloc ((gsize) E2E_W * E2E_H * 4);
  n = run_file (clip->path, "rdsvideodec ! rdsvideoconvert ! video/x-raw,format=RGBA,width=640,height=360",
      keep_rgba, check.got);
  g_assert_cmpuint (n, ==, 30);
  n = run_file (clip->path, "vah264dec ! video/x-raw,format=NV12", check_against_ref, &check);
  g_assert_cmpuint (n, ==, 30);
  g_assert_cmpuint (check.index, ==, check.got->len);
  g_test_message ("%u frames, max |diff| %u", check.index, check.max_diff);
  g_free (check.ref);
  g_ptr_array_unref (check.got);
  clip_free (clip);
}

/* ---- soak ---- */

#define SOAK_W 1280
#define SOAK_H 720
#define SOAK_FRAMES 300
#define SOAK_LOOPS 10

typedef struct
{
  guint frames;
  gsize rss[SOAK_LOOPS], gpu_used[SOAK_LOOPS];  /* after each loop's last frame */
} SoakStats;

/* Runs in the streaming thread: samples memory use when a loop's last frame reaches the sink. */
static GstPadProbeReturn
soak_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  SoakStats *st = user_data;

  st->frames++;
  if (st->frames % SOAK_FRAMES == 0 && st->frames / SOAK_FRAMES <= SOAK_LOOPS) {
    guint loop = st->frames / SOAK_FRAMES - 1;

    st->rss[loop] = vm_rss_bytes ();
    st->gpu_used[loop] = gpu_used_bytes ();
  }
  return GST_PAD_PROBE_OK;
}

/* A 1280x720 clip of 300 frames decoded and converted 10 times in one pipeline (3000 frames): VmRSS grows by less
 * than 8 MiB and the process's GPU memory moves by less than 16 MiB between the end of loop 2 and the end of loop 10. */
static void
test_soak (void)
{
  const gchar *need[] = { "vah264enc", "h264parse", NULL };
  SoakStats st = { 0 };
  GstElement *pipe, *src, *sink;
  GError *err = NULL;
  GstBus *bus;
  GstMessage *msg;
  GstPad *pad;
  GBytes *stream;
  gint64 t0, t1;
  gssize rss_growth, gpu_delta;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements (need))
    return;
  stream = rds_test_encode ("h264", SOAK_W, SOAK_H, SOAK_FRAMES, 2, "ball");
  g_assert_nonnull (stream);

  pipe = gst_parse_launch ("appsrc name=src caps=video/x-h264,stream-format=byte-stream format=bytes "
      "! h264parse ! rdsvideodec ! rdsvideoconvert ! video/x-raw(memory:RDS),format=RGBA "
      "! fakesink name=sink sync=false", &err);
  g_assert_no_error (err);
  src = gst_bin_get_by_name (GST_BIN (pipe), "src");
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, soak_probe, &st, NULL);
  gst_object_unref (pad);

  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* each loop is a complete stream (SPS/PPS + IDR first); the buffers share the encoded bytes */
  for (guint i = 0; i < SOAK_LOOPS; i++)
    g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new_wrapped_bytes (stream)), ==,
        GST_FLOW_OK);
  g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src)), ==, GST_FLOW_OK);
  bus = gst_element_get_bus (pipe);
  msg = gst_bus_timed_pop_filtered (bus, 280 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (msg);
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    gchar *dbg = NULL;

    gst_message_parse_error (msg, &err, &dbg);
    g_error ("pipeline error from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message,
        dbg ? dbg : "");
  }
  gst_message_unref (msg);
  t1 = g_get_monotonic_time ();
  gst_object_unref (bus);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (src);
  gst_object_unref (pipe);
  g_bytes_unref (stream);

  g_assert_cmpuint (st.frames, ==, SOAK_FRAMES * SOAK_LOOPS);
  for (guint i = 0; i < SOAK_LOOPS; i++)
    g_test_message ("loop %2u: VmRSS %.1f MiB, GPU used %.1f MiB", i + 1, MIB (st.rss[i]), MIB (st.gpu_used[i]));
  rss_growth = (gssize) st.rss[SOAK_LOOPS - 1] - (gssize) st.rss[1];
  gpu_delta = (gssize) st.gpu_used[SOAK_LOOPS - 1] - (gssize) st.gpu_used[1];
  g_test_message ("%u frames in %.1f s (%.0f fps); VmRSS growth loop 2 -> %u: %+.2f MiB; GPU used delta: %+.2f MiB",
      st.frames, (t1 - t0) / 1e6, st.frames / ((t1 - t0) / 1e6), SOAK_LOOPS, rss_growth / (1024.0 * 1024.0),
      gpu_delta / (1024.0 * 1024.0));
  g_assert_cmpint (rss_growth, <, 8 * 1024 * 1024);
  g_assert_cmpint (ABS (gpu_delta), <, 16 * 1024 * 1024);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  g_test_add_data_func ("/gpu/pipeline/autoplug-uridecodebin", "uridecodebin", test_autoplug);
  g_test_add_data_func ("/gpu/pipeline/autoplug-uridecodebin3", "uridecodebin3", test_autoplug);
  g_test_add_data_func ("/gpu/pipeline/fallback-uridecodebin", "uridecodebin", test_fallback);
  g_test_add_data_func ("/gpu/pipeline/fallback-uridecodebin3", "uridecodebin3", test_fallback);
  g_test_add_func ("/gpu/pipeline/size-change-fallback", test_size_change_fallback);
  g_test_add_func ("/gpu/pipeline/end-to-end", test_end_to_end);
  g_test_add_func ("/gpu/soak/decode-convert", test_soak);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_deinit ();
    return ret;
  }
}
