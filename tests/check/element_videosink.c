/* rdsvideosink: factory, properties and their forwarding to the inner sink, accepted no-ops, the video-sink property,
 * and the errors when the inner sink cannot start (cpu); RDS RGBA frames reaching the inner sink as BGRx, batched
 * input refused, video-sink refused while streaming (gpu, skipped without a ROCm device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"

#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <rds/rds.h>
#include <string.h>

#define W 64
#define H 48

static GstAllocator *dev_alloc;

/* The inner sink: the bin's only direct child flagged as a sink. */
static GstElement *
inner_sink (GstElement * bin)
{
  GstIterator *it = gst_bin_iterate_sinks (GST_BIN (bin));
  GValue v = G_VALUE_INIT;
  GstElement *e = NULL;

  if (gst_iterator_next (it, &v) == GST_ITERATOR_OK) {
    e = g_value_dup_object (&v);
    g_value_unset (&v);
  }
  g_assert_cmpint (gst_iterator_next (it, &v), ==, GST_ITERATOR_DONE);
  gst_iterator_free (it);
  g_assert_nonnull (e);
  return e;
}

static gchar *
factory_of (GstElement * e)
{
  return g_strdup (GST_OBJECT_NAME (gst_element_get_factory (e)));
}

/* ---- a sink that fails to start without posting an error, as some display sinks do ---- */

typedef struct
{
  GstElement parent;
} RdsTestSilentSink;
typedef struct
{
  GstElementClass parent_class;
} RdsTestSilentSinkClass;

static GType rds_test_silent_sink_get_type (void);
G_DEFINE_TYPE (RdsTestSilentSink, rds_test_silent_sink, GST_TYPE_ELEMENT);

static GstStaticPadTemplate silent_sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStateChangeReturn
silent_sink_change_state (GstElement * e, GstStateChange t)
{
  if (t == GST_STATE_CHANGE_NULL_TO_READY)
    return GST_STATE_CHANGE_FAILURE;
  return GST_ELEMENT_CLASS (rds_test_silent_sink_parent_class)->change_state (e, t);
}

static void
rds_test_silent_sink_init (RdsTestSilentSink * self)
{
  gst_element_add_pad (GST_ELEMENT (self), gst_pad_new_from_static_template (&silent_sink_template, "sink"));
  GST_OBJECT_FLAG_SET (self, GST_ELEMENT_FLAG_SINK);
}

static void
rds_test_silent_sink_class_init (RdsTestSilentSinkClass * klass)
{
  GstElementClass *ec = GST_ELEMENT_CLASS (klass);

  ec->change_state = silent_sink_change_state;
  gst_element_class_add_static_pad_template (ec, &silent_sink_template);
  gst_element_class_set_static_metadata (ec, "Silent failing sink", "Sink/Video", "Fails to start, says nothing",
      "Redstream contributors");
}

/* ---- cpu ---- */

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideosink", NULL);
  gboolean sync, qos, async, far, create, dt;
  gint64 lateness;
  guint64 deadline;
  guint x, y, w, h, gpu, rows, cols, profile;
  gpointer display;
  gchar *vs, *winsys;
  GstPad *pad;
  GParamSpec *ps;

  g_assert_nonnull (e);
  g_assert_true (GST_IS_BIN (e));
  g_assert_true (GST_OBJECT_FLAG_IS_SET (e, GST_ELEMENT_FLAG_SINK));
  pad = gst_element_get_static_pad (e, "sink");
  g_assert_nonnull (pad);
  g_assert_true (GST_IS_GHOST_PAD (pad));
  gst_object_unref (pad);

  /* defaults as gst-inspect shows them for DeepStream 8.0's nveglglessink; video-sink and winsys are ours */
  g_object_get (e, "sync", &sync, "qos", &qos, "max-lateness", &lateness, "processing-deadline", &deadline,
      "async", &async, "window-x", &x, "window-y", &y, "window-width", &w, "window-height", &h,
      "force-aspect-ratio", &far, "create-window", &create, "gpu-id", &gpu, "rows", &rows, "columns", &cols,
      "profile", &profile, "display", &display, "do-transform", &dt, "winsys", &winsys, "video-sink", &vs, NULL);
  g_assert_true (sync);
  g_assert_true (qos);
  g_assert_cmpint (lateness, ==, 5 * GST_MSECOND);
  g_assert_cmpuint (deadline, ==, 15 * GST_MSECOND);
  g_assert_true (async);
  g_assert_cmpuint (x, ==, 10);
  g_assert_cmpuint (y, ==, 10);
  g_assert_cmpuint (w, ==, 0);
  g_assert_cmpuint (h, ==, 0);
  g_assert_true (far);
  g_assert_true (create);
  g_assert_cmpuint (gpu, ==, 0);
  g_assert_cmpuint (rows, ==, 1);
  g_assert_cmpuint (cols, ==, 1);
  g_assert_cmpuint (profile, ==, 0);
  g_assert_null (display);
  g_assert_true (dt);
  g_assert_cmpstr (winsys, ==, "x11");
  g_assert_cmpstr (vs, ==, "autovideosink");
  g_free (winsys);
  g_free (vs);

  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "gpu-id");
  g_assert_true (ps->flags & GST_PARAM_MUTABLE_READY);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "video-sink");
  g_assert_true (ps->flags & GST_PARAM_MUTABLE_READY);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "rows");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->minimum, ==, 1);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->maximum, ==, G_MAXINT);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "max-lateness");
  g_assert_cmpint (G_PARAM_SPEC_INT64 (ps)->minimum, ==, -1);
  gst_object_unref (e);
}

static void
test_forwarding (void)
{
  GstElement *e = gst_parse_launch ("rdsvideosink video-sink=fakesink", NULL);
  GstElement *s, *conv;
  gboolean sync, qos, async;
  gint64 lateness;
  guint64 deadline;
  guint gpu;
  gchar *f;

  g_assert_nonnull (e);
  s = inner_sink (e);
  f = factory_of (s);
  g_assert_cmpstr (f, ==, "fakesink");
  g_free (f);
  /* our values reach the inner sink, defaults included (fakesink's own are qos FALSE, max-lateness -1) */
  g_object_get (s, "sync", &sync, "qos", &qos, "max-lateness", &lateness, "processing-deadline", &deadline,
      "async", &async, NULL);
  g_assert_true (sync);
  g_assert_true (qos);
  g_assert_cmpint (lateness, ==, 5 * GST_MSECOND);
  g_assert_cmpuint (deadline, ==, 15 * GST_MSECOND);
  g_assert_true (async);
  /* and so do later changes */
  g_object_set (e, "sync", FALSE, "qos", FALSE, "max-lateness", (gint64) - 1, "processing-deadline", (guint64) 7,
      "async", FALSE, NULL);
  g_object_get (s, "sync", &sync, "qos", &qos, "max-lateness", &lateness, "processing-deadline", &deadline,
      "async", &async, NULL);
  g_assert_false (sync);
  g_assert_false (qos);
  g_assert_cmpint (lateness, ==, -1);
  g_assert_cmpuint (deadline, ==, 7);
  g_assert_false (async);
  /* properties the inner sink lacks are stored, silently */
  g_object_set (e, "window-x", 300u, "window-width", 512u, "create-window", FALSE, "force-aspect-ratio", FALSE,
      NULL);
  g_object_get (e, "window-x", &gpu, NULL);
  g_assert_cmpuint (gpu, ==, 300);
  /* gpu-id also picks the converter's GPU */
  g_object_set (e, "gpu-id", 1u, NULL);
  conv = gst_bin_get_by_name (GST_BIN (e), "convert");
  g_assert_nonnull (conv);
  g_object_get (conv, "gpu-id", &gpu, NULL);
  g_assert_cmpuint (gpu, ==, 1);
  gst_object_unref (conv);
  gst_object_unref (s);

  /* a new inner sink gets the current values */
  g_object_set (e, "video-sink", "fakesink", NULL);
  s = inner_sink (e);
  g_object_get (s, "sync", &sync, "max-lateness", &lateness, NULL);
  g_assert_false (sync);
  g_assert_cmpint (lateness, ==, -1);
  gst_object_unref (s);
  gst_object_unref (e);
}

static void
test_forwarding_nested (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideosink", NULL);
  GstElement *s, *child;
  gint64 lateness;
  gchar *f;

  /* the default autovideosink is a bin: the sinks inside it get the values too (before READY, its placeholder) */
  s = inner_sink (e);
  f = factory_of (s);
  g_assert_cmpstr (f, ==, "autovideosink");
  g_free (f);
  child = inner_sink (s);
  g_object_get (child, "max-lateness", &lateness, NULL);
  g_assert_cmpint (lateness, ==, 5 * GST_MSECOND);
  g_object_set (e, "max-lateness", (gint64) 9, NULL);
  g_object_get (child, "max-lateness", &lateness, NULL);
  g_assert_cmpint (lateness, ==, 9);
  gst_object_unref (child);
  gst_object_unref (s);
  gst_object_unref (e);
}

static void
test_noops_warn_once (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideosink", NULL);
  guint rows;
  gchar *winsys;

  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*rows*no effect*");
  g_object_set (e, "rows", 2u, NULL);
  g_object_set (e, "rows", 3u, NULL);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*columns*no effect*");
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*profile*no effect*");
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*display*no effect*");
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*do-transform*no effect*");
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*winsys*no effect*");
  g_object_set (e, "columns", 2u, "profile", 1u, "display", (gpointer) e, "do-transform", FALSE, "winsys",
      "wayland", NULL);
  g_test_assert_expected_messages ();
  /* their default values: no warning */
  g_object_set (e, "columns", 1u, "display", NULL, NULL);
  g_object_get (e, "rows", &rows, "winsys", &winsys, NULL);
  g_assert_cmpuint (rows, ==, 3);
  g_assert_cmpstr (winsys, ==, "wayland");
  g_free (winsys);
  gst_object_unref (e);
}

static void
test_video_sink_unknown (void)
{
  GstElement *e = gst_parse_launch ("rdsvideosink video-sink=fakesink", NULL);
  GstElement *s;
  gchar *vs;

  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*no element*rds-no-such-sink*");
  g_object_set (e, "video-sink", "rds-no-such-sink", NULL);
  g_test_assert_expected_messages ();
  g_object_get (e, "video-sink", &vs, NULL);
  g_assert_cmpstr (vs, ==, "fakesink");
  g_free (vs);
  s = inner_sink (e);
  vs = factory_of (s);
  g_assert_cmpstr (vs, ==, "fakesink");
  g_free (vs);
  gst_object_unref (s);
  /* an element that does not link (no sink pad): the old sink is put back */
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*fakesrc*does not link*keeping*fakesink*");
  g_object_set (e, "video-sink", "fakesrc", NULL);
  g_test_assert_expected_messages ();
  s = inner_sink (e);
  vs = factory_of (s);
  g_assert_cmpstr (vs, ==, "fakesink");
  g_free (vs);
  g_assert_true (gst_pad_is_linked (GST_ELEMENT_CAST (s)->sinkpads->data));
  gst_object_unref (s);
  /* NULL or empty: back to the default */
  g_object_set (e, "video-sink", NULL, NULL);
  g_object_get (e, "video-sink", &vs, NULL);
  g_assert_cmpstr (vs, ==, "autovideosink");
  g_free (vs);
  gst_object_unref (e);
}

static GstMessage *
pop_error (GstBus * bus)
{
  return gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
}

static void
test_silent_failure (void)
{
  GstElement *p = gst_pipeline_new (NULL);
  GstElement *e = gst_element_factory_make ("rdsvideosink", NULL);
  GstBus *bus = gst_element_get_bus (p);
  GstMessage *m;
  GError *err = NULL;
  gchar *dbg = NULL;

  /* an inner sink that fails to start without a word still leaves a clear error on the bus (ours: DeepStream 8.0's
   * nveglglessink fails the state change silently) */
  g_assert_true (gst_element_register (NULL, "rdstestsilentsink", GST_RANK_NONE,
          rds_test_silent_sink_get_type ()));
  g_object_set (e, "video-sink", "rdstestsilentsink", NULL);
  gst_bin_add (GST_BIN (p), e);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_FAILURE);
  m = pop_error (bus);
  g_assert_nonnull (m);
  g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (e));
  gst_message_parse_error (m, &err, &dbg);
  g_assert_nonnull (strstr (err->message, "rdstestsilentsink"));
  g_assert_nonnull (strstr (err->message, "video-sink"));
  g_clear_error (&err);
  g_free (dbg);
  gst_message_unref (m);
  g_assert_null (pop_error (bus));
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (p);
}

static void
test_no_display (void)
{
  GstElement *p, *e;
  GstBus *bus;
  GstMessage *m;
  GstStateChangeReturn r;
  GError *err = NULL;
  gchar *display;
  guint n = 0;

  if (!gst_registry_check_feature_version (gst_registry_get (), "ximagesink", 1, 0, 0)) {
    g_test_skip ("no ximagesink");
    return;
  }
  /* no X display: ximagesink's own error reaches the bus, and only that one */
  display = g_strdup (g_getenv ("DISPLAY"));
  g_unsetenv ("DISPLAY");
  p = gst_pipeline_new (NULL);
  e = gst_element_factory_make ("rdsvideosink", NULL);
  bus = gst_element_get_bus (p);
  g_object_set (e, "video-sink", "ximagesink", NULL);
  gst_bin_add (GST_BIN (p), e);
  r = gst_element_set_state (p, GST_STATE_READY);
  if (r != GST_STATE_CHANGE_FAILURE)
    r = gst_element_set_state (p, GST_STATE_PAUSED);
  g_assert_cmpint (r, ==, GST_STATE_CHANGE_FAILURE);
  while ((m = pop_error (bus)) != NULL) {
    gst_message_parse_error (m, &err, NULL);
    g_test_message ("error from %s: %s", GST_MESSAGE_SRC_NAME (m), err->message);
    g_assert_cmpstr (G_OBJECT_TYPE_NAME (GST_MESSAGE_SRC (m)), ==, "GstXImageSink");
    g_clear_error (&err);
    gst_message_unref (m);
    n++;
  }
  g_assert_cmpuint (n, ==, 1);
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (p);
  if (display != NULL)
    g_setenv ("DISPLAY", display, TRUE);
  g_free (display);
}

/* ---- gpu ---- */

/* A device RGBA batch of @n random slots (seed 7 + i), each with a frame meta. */
static GstBuffer *
make_frames (guint n)
{
  RdsBufSurfaceCreateParams cp = {.width = W,.height = H,.layout = RDS_BUF_LAYOUT_PITCH,
    .colorFormat = RDS_BUF_COLOR_FORMAT_RGBA
  };
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (n);
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (dev_alloc, n, &cp));
  s = rds_buffer_get_surface (buf);
  s->numFilled = n;
  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

    fill_random (s, i, 7 + i);
    fm->batch_id = i;
    fm->pad_index = i;
    fm->source_id = i;
    rds_add_frame_meta_to_batch (bm, fm);
  }
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  GST_BUFFER_PTS (buf) = 0;
  GST_BUFFER_DURATION (buf) = 40 * GST_MSECOND;
  return buf;
}

typedef struct
{
  GstHarness *h;
  GstBus *bus;
} Sink;

static void
sink_init (Sink * k, const gchar * props)
{
  gchar *desc = g_strdup_printf ("rdsvideosink video-sink=fakesink sync=false %s", props != NULL ? props : "");
  GstElement *e = gst_parse_launch (desc, NULL);

  g_assert_nonnull (e);
  k->bus = gst_bus_new ();
  gst_element_set_bus (e, k->bus);
  k->h = gst_harness_new_with_element (e, "sink", NULL);
  gst_object_unref (e);
  gst_harness_set_src_caps_str (k->h, "video/x-raw(memory:RDS),format=RGBA,width=64,height=48,framerate=25/1");
  g_free (desc);
}

static void
sink_clear (Sink * k)
{
  gst_element_set_bus (k->h->element, NULL);
  gst_bus_set_flushing (k->bus, TRUE);
  gst_harness_teardown (k->h);
  gst_object_unref (k->bus);
}

static void
test_pixels (void)
{
  Sink k;
  GstBuffer *in;
  GstElement *s;
  GstSample *sample = NULL;
  GstVideoInfo info;
  GstVideoFrame frame;
  HostImage img;

  RDS_TEST_REQUIRE_GPU ();
  sink_init (&k, NULL);
  in = make_frames (1);
  snapshot (rds_buffer_get_surface (in), 0, &img);
  g_assert_cmpint (gst_harness_push (k.h, in), ==, GST_FLOW_OK);

  /* system-memory BGRx with the same pixels reaches the inner sink */
  s = inner_sink (k.h->element);
  g_object_get (s, "last-sample", &sample, NULL);
  g_assert_nonnull (sample);
  g_assert_true (gst_video_info_from_caps (&info, gst_sample_get_caps (sample)));
  g_assert_cmpint (GST_VIDEO_INFO_FORMAT (&info), ==, GST_VIDEO_FORMAT_BGRx);
  g_assert_cmpint (GST_VIDEO_INFO_WIDTH (&info), ==, W);
  g_assert_cmpint (GST_VIDEO_INFO_HEIGHT (&info), ==, H);
  g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (gst_sample_get_caps (sample), 0),
          GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
  g_assert_true (gst_video_frame_map (&frame, &info, gst_sample_get_buffer (sample), GST_MAP_READ));
  for (guint y = 0; y < H; y++) {
    const guint8 *src = img.im.plane[0] + (gsize) y * img.im.pitch[0];
    const guint8 *dst = (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&frame, 0) +
        (gsize) y * GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0);

    for (guint x = 0; x < W; x++) {
      g_assert_cmpuint (dst[4 * x + 0], ==, src[4 * x + 2]);
      g_assert_cmpuint (dst[4 * x + 1], ==, src[4 * x + 1]);
      g_assert_cmpuint (dst[4 * x + 2], ==, src[4 * x + 0]);
    }
  }
  gst_video_frame_unmap (&frame);
  gst_sample_unref (sample);
  gst_object_unref (s);
  host_image_clear (&img);
  g_assert_null (pop_error (k.bus));
  sink_clear (&k);
}

static void
test_batched (void)
{
  Sink k;
  GstMessage *m;
  GError *err = NULL;
  gchar *dbg = NULL;
  GstBufferList *list;

  RDS_TEST_REQUIRE_GPU ();
  /* more than one frame per buffer: an element error (ours: DeepStream 8.0 fails the first buffer silently) */
  sink_init (&k, NULL);
  g_assert_cmpint (gst_harness_push (k.h, make_frames (2)), ==, GST_FLOW_ERROR);
  m = pop_error (k.bus);
  g_assert_nonnull (m);
  g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (k.h->element));
  gst_message_parse_error (m, &err, &dbg);
  g_assert_cmpstr (err->message, ==, "rdsvideosink takes one frame per buffer: add a tiler or a demuxer");
  g_assert_nonnull (strstr (dbg, "2 frames"));
  g_clear_error (&err);
  g_free (dbg);
  gst_message_unref (m);
  sink_clear (&k);

  /* in a buffer list too */
  sink_init (&k, NULL);
  list = gst_buffer_list_new ();
  gst_buffer_list_add (list, make_frames (1));
  gst_buffer_list_add (list, make_frames (2));
  g_assert_cmpint (gst_pad_push_list (k.h->srcpad, list), ==, GST_FLOW_ERROR);
  m = pop_error (k.bus);
  g_assert_nonnull (m);
  gst_message_parse_error (m, &err, NULL);
  g_assert_cmpstr (err->message, ==, "rdsvideosink takes one frame per buffer: add a tiler or a demuxer");
  g_clear_error (&err);
  gst_message_unref (m);
  sink_clear (&k);
}

static void
test_video_sink_while_playing (void)
{
  Sink k;
  gchar *vs;

  RDS_TEST_REQUIRE_GPU ();
  sink_init (&k, NULL);
  g_assert_cmpint (gst_harness_push (k.h, make_frames (1)), ==, GST_FLOW_OK);
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*video-sink*NULL or READY*");
  g_object_set (k.h->element, "video-sink", "autovideosink", NULL);
  g_test_assert_expected_messages ();
  g_object_get (k.h->element, "video-sink", &vs, NULL);
  g_assert_cmpstr (vs, ==, "fakesink");
  g_free (vs);
  /* still streaming */
  g_assert_cmpint (gst_harness_push (k.h, make_frames (1)), ==, GST_FLOW_OK);
  g_assert_null (pop_error (k.bus));
  sink_clear (&k);
}

int
main (int argc, char **argv)
{
  gint ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  dev_alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);

  g_test_add_func ("/cpu/videosink/properties", test_properties);
  g_test_add_func ("/cpu/videosink/forwarding", test_forwarding);
  g_test_add_func ("/cpu/videosink/forwarding-nested", test_forwarding_nested);
  g_test_add_func ("/cpu/videosink/noops-warn-once", test_noops_warn_once);
  g_test_add_func ("/cpu/videosink/video-sink-unknown", test_video_sink_unknown);
  g_test_add_func ("/cpu/videosink/silent-failure", test_silent_failure);
  g_test_add_func ("/cpu/videosink/no-display", test_no_display);
  g_test_add_func ("/gpu/videosink/pixels", test_pixels);
  g_test_add_func ("/gpu/videosink/batched", test_batched);
  g_test_add_func ("/gpu/videosink/video-sink-while-playing", test_video_sink_while_playing);
  ret = g_test_run ();

  gst_object_unref (dev_alloc);
  gst_deinit ();
  return ret;
}
