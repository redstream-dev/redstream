/* rdsvideodec: factory and templates (cpu), rocDecode H.264 decode compared bit-exactly with vah264dec (gpu, skipped
 * without a ROCm device or the VA H.264 encoder/decoder).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"

#include <rds/rds.h>

/* ---- cpu ---- */

static void
test_factory (void)
{
  GstElementFactory *factory = gst_element_factory_find ("rdsvideodec");
  const GList *l;
  gboolean seen_sink = FALSE, seen_src = FALSE;

  g_assert_nonnull (factory);
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), ==, GST_RANK_PRIMARY + 10);
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS), ==,
      "Codec/Decoder/Video/Hardware");
  for (l = gst_element_factory_get_static_pad_templates (factory); l != NULL; l = l->next) {
    GstStaticPadTemplate *t = l->data;
    GstCaps *caps = gst_static_caps_get (&t->static_caps);

    if (t->direction == GST_PAD_SINK) {
      GstCaps *h264 = gst_caps_from_string ("video/x-h264, stream-format=byte-stream, alignment=au, parsed=true");
      GstCaps *avc = gst_caps_from_string ("video/x-h264, stream-format=avc, alignment=au");
      GstCaps *nal = gst_caps_from_string ("video/x-h264, stream-format=byte-stream, alignment=nal");

      seen_sink = TRUE;
      g_assert_true (gst_caps_can_intersect (caps, h264));
      g_assert_false (gst_caps_can_intersect (caps, avc));
      g_assert_false (gst_caps_can_intersect (caps, nal));
      gst_caps_unref (nal);
      gst_caps_unref (avc);
      gst_caps_unref (h264);
    } else {
      seen_src = TRUE;
      g_assert_cmpuint (gst_caps_get_size (caps), ==, 2);
      /* memory:RDS first, then the system-memory fallback; NV12 only */
      g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, 0), RDS_CAPS_FEATURE_MEMORY_RDS));
      g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 1),
              GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
      for (guint i = 0; i < 2; i++) {
        GstStructure *s = gst_caps_get_structure (caps, i);
        g_assert_true (gst_structure_has_name (s, "video/x-raw"));
        g_assert_cmpstr (gst_structure_get_string (s, "format"), ==, "NV12");
        g_assert_true (gst_structure_has_field (s, "width"));
        g_assert_true (gst_structure_has_field (s, "height"));
      }
    }
    gst_caps_unref (caps);
  }
  g_assert_true (seen_sink);
  g_assert_true (seen_src);
  gst_object_unref (factory);
}

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideodec", NULL);
  GParamSpec *p;
  guint u;

  g_assert_nonnull (e);
  p = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "gpu-id");
  g_assert_nonnull (p);
  g_assert_true (G_IS_PARAM_SPEC_UINT (p));
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->default_value, ==, 0);
  g_object_set (e, "gpu-id", 1, NULL);
  g_object_get (e, "gpu-id", &u, NULL);
  g_assert_cmpuint (u, ==, 1);
  gst_object_unref (e);
}

/* ---- gpu ---- */

#define REQUIRE_VA_H264() G_STMT_START {                                   \
    if (!rds_test_have_element ("vah264enc") || !rds_test_have_element ("vah264dec")) { \
      g_test_skip ("vah264enc or vah264dec not available");                 \
      return;                                                               \
    }                                                                       \
  } G_STMT_END

typedef struct
{
  guint width, height, frames, b_frames;
  const gchar *pattern;
} Clip;

static const Clip clips[] = {
  { 320, 240, 30, 0, "smpte" },
  { 1280, 720, 30, 2, "ball" },
  { 1366, 768, 20, 0, "smpte" },        /* width not a multiple of 64: pitch padding */
  { 1920, 1080, 20, 0, "ball" },        /* coded height 1088: display cropping */
};

static void
assert_same_crcs (GPtrArray * ref, GPtrArray * got, const gchar * what)
{
  g_assert_cmpuint (got->len, ==, ref->len);
  for (guint i = 0; i < ref->len; i++) {
    if (g_strcmp0 (ref->pdata[i], got->pdata[i]) != 0)
      g_error ("%s: frame %u differs: reference %s, got %s", what, i, (gchar *) ref->pdata[i],
          (gchar *) got->pdata[i]);
  }
}

static void
test_crc_parity (gconstpointer data)
{
  const Clip *c = data;
  GBytes *stream;
  GPtrArray *ref, *rds, *sys;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  stream = rds_test_encode ("h264", c->width, c->height, c->frames, c->b_frames, c->pattern);
  g_assert_nonnull (stream);

  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);
  rds = rds_test_decode_crc (stream, "h264", "rdsvideodec", TRUE);
  sys = rds_test_decode_crc (stream, "h264", "rdsvideodec", FALSE);
  g_assert_cmpuint (ref->len, ==, c->frames);
  assert_same_crcs (ref, rds, "memory:RDS path");
  assert_same_crcs (ref, sys, "system-memory fallback");
  g_test_message ("%ux%u: %u frames, first %s", c->width, c->height, ref->len, (gchar *) ref->pdata[0]);

  g_ptr_array_unref (sys);
  g_ptr_array_unref (rds);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
}

/* With a sink that accepts anything, the decoder picks memory:RDS, with HIP device surfaces by default. */
static void
test_rds_output (void)
{
  GstElement *pipe, *src, *sink;
  GError *err = NULL;
  GBytes *stream;
  GstSample *s;
  guint n = 0;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  stream = rds_test_encode ("h264", 320, 240, 5, 0, "smpte");
  pipe = gst_parse_launch ("appsrc name=src caps=video/x-h264,stream-format=byte-stream format=bytes "
      "! h264parse ! rdsvideodec ! appsink name=sink sync=false", &err);
  g_assert_no_error (err);
  src = gst_bin_get_by_name (GST_BIN (pipe), "src");
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new_wrapped_bytes (stream)), ==,
      GST_FLOW_OK);
  g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src)), ==, GST_FLOW_OK);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstCaps *caps = gst_sample_get_caps (s);
    GstStructure *st = gst_caps_get_structure (caps, 0);
    RdsBufSurface *surf = rds_buffer_get_surface (gst_sample_get_buffer (s));
    gint w = 0, h = 0;

    g_assert_true (rds_caps_has_memory_rds (caps));
    g_assert_cmpstr (gst_structure_get_string (st, "format"), ==, "NV12");
    g_assert_true (gst_structure_get_int (st, "width", &w) && gst_structure_get_int (st, "height", &h));
    g_assert_cmpint (w, ==, 320);
    g_assert_cmpint (h, ==, 240);
    g_assert_nonnull (surf);
    g_assert_cmpint (surf->memType, ==, RDS_BUF_MEM_HIP_DEVICE);
    g_assert_cmpuint (surf->numFilled, ==, 1);
    g_assert_cmpuint (surf->surfaceList[0].width, ==, 320);
    g_assert_cmpuint (surf->surfaceList[0].height, ==, 240);
    g_assert_cmpint (surf->surfaceList[0].colorFormat, ==, RDS_BUF_COLOR_FORMAT_NV12);
    gst_sample_unref (s);
    n++;
  }
  g_assert_cmpuint (n, ==, 5);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (src);
  gst_object_unref (pipe);
  g_bytes_unref (stream);
}

static GstPadProbeReturn
record_pts (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GArray *pts = user_data;
  GstClockTime t = GST_BUFFER_PTS (GST_PAD_PROBE_INFO_BUFFER (info));

  g_array_append_val (pts, t);
  return GST_PAD_PROBE_OK;
}

static gint
cmp_time (gconstpointer a, gconstpointer b)
{
  GstClockTime x = *(const GstClockTime *) a, y = *(const GstClockTime *) b;

  return x < y ? -1 : x > y;
}

/* Output PTS are the input PTS, in display order, although B-frames arrive in decode order; the last frames come
 * out at EOS. */
static void
test_pts_b_frames (void)
{
  GstElement *pipe, *dec, *sink;
  GError *err = NULL;
  GArray *in_pts = g_array_new (FALSE, FALSE, sizeof (GstClockTime));
  GArray *out_pts = g_array_new (FALSE, FALSE, sizeof (GstClockTime));
  GstPad *pad;
  GstSample *s;
  gboolean reordered = FALSE;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  pipe = gst_parse_launch ("videotestsrc pattern=ball num-buffers=30 "
      "! video/x-raw,format=NV12,width=320,height=240,framerate=30/1 ! vah264enc b-frames=2 ! h264parse "
      "! rdsvideodec name=dec ! appsink name=sink sync=false", &err);
  g_assert_no_error (err);
  dec = gst_bin_get_by_name (GST_BIN (pipe), "dec");
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  pad = gst_element_get_static_pad (dec, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, record_pts, in_pts, NULL);
  gst_object_unref (pad);
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstClockTime t = GST_BUFFER_PTS (gst_sample_get_buffer (s));

    g_array_append_val (out_pts, t);
    gst_sample_unref (s);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);

  g_assert_cmpuint (in_pts->len, ==, 30);
  for (guint i = 1; i < in_pts->len; i++)
    reordered |= g_array_index (in_pts, GstClockTime, i) < g_array_index (in_pts, GstClockTime, i - 1);
  g_assert_true (reordered);    /* the encoder really produced B-frames */
  g_array_sort (in_pts, cmp_time);
  g_assert_cmpuint (out_pts->len, ==, in_pts->len);
  for (guint i = 0; i < out_pts->len; i++)
    g_assert_cmpuint (g_array_index (out_pts, GstClockTime, i), ==, g_array_index (in_pts, GstClockTime, i));

  g_array_unref (out_pts);
  g_array_unref (in_pts);
  gst_object_unref (sink);
  gst_object_unref (dec);
  gst_object_unref (pipe);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cpu/videodec/factory", test_factory);
  g_test_add_func ("/cpu/videodec/properties", test_properties);
  for (guint i = 0; i < G_N_ELEMENTS (clips); i++) {
    gchar *path = g_strdup_printf ("/gpu/videodec/crc-parity-%ux%u-b%u", clips[i].width, clips[i].height,
        clips[i].b_frames);
    g_test_add_data_func (path, &clips[i], test_crc_parity);
    g_free (path);
  }
  g_test_add_func ("/gpu/videodec/rds-output", test_rds_output);
  g_test_add_func ("/gpu/videodec/pts-b-frames", test_pts_b_frames);
  return g_test_run ();
}
