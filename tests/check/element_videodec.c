/* rdsvideodec: factory, templates and properties (cpu); rocDecode H.264/H.265 decode compared bit-exactly with
 * vah264dec/vah265dec, resolution changes, flush, drain and the DeepStream properties (gpu, skipped without a ROCm
 * device or the VA encoder/decoder a test needs).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"

#include <gst/check/gstharness.h>
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
      GstCaps *h265 = gst_caps_from_string ("video/x-h265, stream-format=byte-stream, alignment=au, parsed=true");
      GstCaps *hvc1 = gst_caps_from_string ("video/x-h265, stream-format=hvc1, alignment=au");

      seen_sink = TRUE;
      g_assert_true (gst_caps_can_intersect (caps, h264));
      g_assert_true (gst_caps_can_intersect (caps, h265));
      g_assert_false (gst_caps_can_intersect (caps, hvc1));
      gst_caps_unref (hvc1);
      gst_caps_unref (h265);
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
assert_uint_prop (GstElement * e, const gchar * name, guint min, guint max, guint def)
{
  GParamSpec *p = g_object_class_find_property (G_OBJECT_GET_CLASS (e), name);
  guint u;

  g_assert_nonnull (p);
  g_assert_true (G_IS_PARAM_SPEC_UINT (p));
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->minimum, ==, min);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->maximum, ==, max);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->default_value, ==, def);
  g_object_set (e, name, max, NULL);
  g_object_get (e, name, &u, NULL);
  g_assert_cmpuint (u, ==, max);
}

/* An enum property with DeepStream's integers and nicks (@nicks: one per value, from 0). */
static void
assert_enum_prop (GstElement * e, const gchar * name, gint def, const gchar * const *nicks, guint n)
{
  GParamSpec *p = g_object_class_find_property (G_OBJECT_GET_CLASS (e), name);
  GEnumClass *klass;
  gint v;

  g_assert_nonnull (p);
  g_assert_true (G_IS_PARAM_SPEC_ENUM (p));
  g_assert_cmpint (G_PARAM_SPEC_ENUM (p)->default_value, ==, def);
  klass = G_PARAM_SPEC_ENUM (p)->enum_class;
  g_assert_cmpuint (klass->n_values, ==, n);
  for (guint i = 0; i < n; i++) {
    GEnumValue *ev = g_enum_get_value (klass, (gint) i);

    g_assert_nonnull (ev);
    g_assert_cmpstr (ev->value_nick, ==, nicks[i]);
  }
  /* configs and launch lines set enums by integer or by nick */
  gst_util_set_object_arg (G_OBJECT (e), name, nicks[n - 1]);
  g_object_get (e, name, &v, NULL);
  g_assert_cmpint (v, ==, (gint) n - 1);
  gst_util_set_object_arg (G_OBJECT (e), name, "0");
  g_object_get (e, name, &v, NULL);
  g_assert_cmpint (v, ==, 0);
}

/* Names, ranges, defaults and enum integers as on NVIDIA's public nvv4l2decoder page; our cudadec-memtype default is
 * 0 (device), DeepStream's 2 (unified). */
static void
test_properties (void)
{
  static const gchar *const memtypes[] = { "memtype_device", "memtype_pinned", "memtype_unified" };
  static const gchar *const skips[] = { "decode_all", "decode_non_ref", "decode_key" };
  GstElement *e = gst_element_factory_make ("rdsvideodec", NULL);
  GParamSpec *p;
  gboolean b;

  g_assert_nonnull (e);
  assert_uint_prop (e, "gpu-id", 0, G_MAXUINT, 0);
  assert_uint_prop (e, "num-extra-surfaces", 0, 24, 0);
  assert_uint_prop (e, "drop-frame-interval", 0, 30, 0);
  assert_enum_prop (e, "cudadec-memtype", 0, memtypes, G_N_ELEMENTS (memtypes));
  assert_enum_prop (e, "skip-frames", 0, skips, G_N_ELEMENTS (skips));
  p = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "low-latency-mode");
  g_assert_nonnull (p);
  g_assert_true (G_IS_PARAM_SPEC_BOOLEAN (p));
  g_assert_false (G_PARAM_SPEC_BOOLEAN (p)->default_value);
  g_object_set (e, "low-latency-mode", TRUE, NULL);
  g_object_get (e, "low-latency-mode", &b, NULL);
  g_assert_true (b);
  /* frame selection may change while playing; memory, surfaces and the GPU only before streaming */
  g_assert_true (g_object_class_find_property (G_OBJECT_GET_CLASS (e), "drop-frame-interval")->flags
      & GST_PARAM_MUTABLE_PLAYING);
  g_assert_true (g_object_class_find_property (G_OBJECT_GET_CLASS (e), "skip-frames")->flags
      & GST_PARAM_MUTABLE_PLAYING);
  g_assert_true (g_object_class_find_property (G_OBJECT_GET_CLASS (e), "cudadec-memtype")->flags
      & GST_PARAM_MUTABLE_READY);
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

/* Moving patterns, so a stale or missing picture changes the checksum. Not observable with VA-encoded clips: odd
 * display sizes (the chroma ceil(w/2) / ceil(h/2) rounding) and a display rect with a non-zero left/top, since the
 * encoder only crops on the right and bottom of even-sized frames. */
static const Clip clips[] = {
  /* NV12 surfaces of at most 1 MiB: guard the first-export workaround (premap_surfaces) */
  { 320, 240, 30, 0, "ball" },
  { 640, 360, 300, 2, "ball" },         /* many frames through many small surfaces, with B-frames */
  { 1280, 720, 30, 2, "ball" },
  { 1366, 768, 20, 0, "ball" },         /* width not a multiple of 64: pitch padding */
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

/* With a sink that accepts anything, the decoder picks memory:RDS, with HIP device surfaces by default
 * (cudadec-memtype -1 = unset) and pinned / unified surfaces for cudadec-memtype 1 / 2. Pixels are unchanged. */
static void
test_rds_output (gconstpointer data)
{
  static const RdsBufSurfaceMemType mem_types[] = { RDS_BUF_MEM_HIP_DEVICE, RDS_BUF_MEM_HIP_PINNED,
    RDS_BUF_MEM_HIP_UNIFIED
  };
  gint memtype = GPOINTER_TO_INT (data);
  GstElement *pipe, *src, *sink;
  GError *err = NULL;
  GBytes *stream;
  GstSample *s;
  guint n = 0;
  gchar *dec, *desc;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  stream = rds_test_encode ("h264", 320, 240, 5, 0, "smpte");
  dec = memtype < 0 ? g_strdup ("rdsvideodec") : g_strdup_printf ("rdsvideodec cudadec-memtype=%d", memtype);
  desc = g_strdup_printf ("appsrc name=src caps=video/x-h264,stream-format=byte-stream format=bytes "
      "! h264parse ! %s ! appsink name=sink sync=false", dec);
  pipe = gst_parse_launch (desc, &err);
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
    g_assert_cmpint (surf->memType, ==, mem_types[MAX (memtype, 0)]);
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

  if (memtype > 0) {
    /* the copy into pinned / unified surfaces gives the same pixels */
    GBytes *clip = rds_test_encode ("h264", 640, 360, 10, 2, "ball");
    GPtrArray *ref = rds_test_decode_crc (clip, "h264", "vah264dec", FALSE);
    GPtrArray *got = rds_test_decode_crc (clip, "h264", dec, TRUE);

    assert_same_crcs (ref, got, dec);
    g_ptr_array_unref (got);
    g_ptr_array_unref (ref);
    g_bytes_unref (clip);
  }
  g_free (desc);
  g_free (dec);
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

#define REQUIRE_VA_H265() G_STMT_START {                                   \
    if (!rds_test_have_element ("vah265enc") || !rds_test_have_element ("vah265dec")) { \
      g_test_skip ("vah265enc or vah265dec not available");                 \
      return;                                                               \
    }                                                                       \
  } G_STMT_END

static const Clip h265_clips[] = {
  { 640, 480, 30, 2, "ball" },          /* with B-frames: reordering and the drain at EOS */
  { 1920, 1080, 20, 0, "ball" },        /* coded height 1088 (CTB-aligned): display cropping */
};

static void
test_h265_parity (gconstpointer data)
{
  const Clip *c = data;
  GBytes *stream;
  GPtrArray *ref, *rds, *sys;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H265 ();
  stream = rds_test_encode ("h265", c->width, c->height, c->frames, c->b_frames, c->pattern);
  g_assert_nonnull (stream);
  ref = rds_test_decode_crc (stream, "h265", "vah265dec", FALSE);
  rds = rds_test_decode_crc (stream, "h265", "rdsvideodec", TRUE);
  sys = rds_test_decode_crc (stream, "h265", "rdsvideodec", FALSE);
  g_assert_cmpuint (ref->len, ==, c->frames);
  assert_same_crcs (ref, rds, "memory:RDS path");
  assert_same_crcs (ref, sys, "system-memory fallback");
  g_ptr_array_unref (sys);
  g_ptr_array_unref (rds);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
}

typedef struct
{
  const gchar *codec;
  Clip a, b;
} SizeChange;

static const SizeChange size_changes[] = {
  /* larger coded size: the decoder is recreated */
  { "h264", { 320, 240, 15, 0, "ball" }, { 640, 480, 15, 2, "smpte" } },
  /* smaller: reconfigured in place, onto surfaces of at most 1 MiB (the first-export workaround must run again) */
  { "h264", { 1280, 720, 15, 2, "ball" }, { 640, 360, 15, 2, "ball" } },
  { "h265", { 1280, 720, 15, 2, "ball" }, { 640, 360, 15, 2, "ball" } },
};

/* Two streams back to back, each starting with its own SPS/PPS/IDR: every frame decodes as when each part is
 * decoded on its own, and frames after the change carry the new size in their caps (part of each checksum). */
static void
test_size_change (gconstpointer data)
{
  const SizeChange *sc = data;
  GBytes *parts[2];
  GBytes *both;
  GPtrArray *ref, *ref_b, *rds, *sys;
  gchar *enc = g_strdup_printf ("va%senc", sc->codec), *vadec = g_strdup_printf ("va%sdec", sc->codec);

  RDS_TEST_REQUIRE_GPU ();
  if (!rds_test_have_element (enc) || !rds_test_have_element (vadec)) {
    g_test_skip_printf ("%s or %s not available", enc, vadec);
    g_free (vadec);
    g_free (enc);
    return;
  }
  parts[0] = rds_test_encode (sc->codec, sc->a.width, sc->a.height, sc->a.frames, sc->a.b_frames, sc->a.pattern);
  parts[1] = rds_test_encode (sc->codec, sc->b.width, sc->b.height, sc->b.frames, sc->b.b_frames, sc->b.pattern);
  both = rds_test_concat (parts, 2);
  ref = rds_test_decode_crc (parts[0], sc->codec, vadec, FALSE);
  ref_b = rds_test_decode_crc (parts[1], sc->codec, vadec, FALSE);
  g_ptr_array_extend_and_steal (ref, ref_b);
  g_assert_cmpuint (ref->len, ==, sc->a.frames + sc->b.frames);

  rds = rds_test_decode_crc (both, sc->codec, "rdsvideodec", TRUE);
  sys = rds_test_decode_crc (both, sc->codec, "rdsvideodec", FALSE);
  assert_same_crcs (ref, rds, "memory:RDS path");
  assert_same_crcs (ref, sys, "system-memory fallback");

  g_ptr_array_unref (sys);
  g_ptr_array_unref (rds);
  g_ptr_array_unref (ref);
  g_bytes_unref (both);
  g_bytes_unref (parts[1]);
  g_bytes_unref (parts[0]);
  g_free (vadec);
  g_free (enc);
}

/* A framerate change without a size change still reaches the output caps. */
static void
test_framerate_change (void)
{
  GstElement *pipe, *src, *sink;
  GError *err = NULL;
  GPtrArray *aus[2];
  GstSample *s;
  gint first_n = 0, first_d = 0, last_n = 0, last_d = 0;
  guint n = 0;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  aus[0] = rds_test_encode_aus ("h264", "video/x-raw,format=NV12,width=320,height=240,framerate=30/1", 10, 0,
      "ball", NULL);
  aus[1] = rds_test_encode_aus ("h264", "video/x-raw,format=NV12,width=320,height=240,framerate=15/1", 10, 0,
      "ball", NULL);
  pipe = gst_parse_launch ("appsrc name=src format=time "
      "caps=video/x-h264,stream-format=byte-stream,alignment=au,framerate=30/1 "
      "! h264parse ! rdsvideodec ! video/x-raw,format=NV12 ! appsink name=sink sync=false", &err);
  g_assert_no_error (err);
  src = gst_bin_get_by_name (GST_BIN (pipe), "src");
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  for (guint part = 0; part < 2; part++) {
    if (part == 1) {
      GstCaps *caps = gst_caps_from_string ("video/x-h264,stream-format=byte-stream,alignment=au,framerate=15/1");

      gst_app_src_set_caps (GST_APP_SRC (src), caps);
      gst_caps_unref (caps);
    }
    for (guint i = 0; i < aus[part]->len; i++)
      g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src),
              gst_buffer_new_wrapped_bytes (aus[part]->pdata[i])), ==, GST_FLOW_OK);
  }
  g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src)), ==, GST_FLOW_OK);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstStructure *st = gst_caps_get_structure (gst_sample_get_caps (s), 0);

    g_assert_true (gst_structure_get_fraction (st, "framerate", &last_n, &last_d));
    if (n++ == 0) {
      first_n = last_n;
      first_d = last_d;
    }
    gst_sample_unref (s);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);
  g_assert_cmpuint (n, ==, 20);
  g_assert_cmpint (first_n, ==, 30 * first_d);
  g_assert_cmpint (last_n, ==, 15 * last_d);
  gst_object_unref (sink);
  gst_object_unref (src);
  gst_object_unref (pipe);
  g_ptr_array_unref (aus[1]);
  g_ptr_array_unref (aus[0]);
}

/* ---- harness helpers: push access units one by one, collect checksums of the system-memory output ---- */

static GstHarness *
new_dec_harness (const gchar * dec)
{
  gchar *desc = g_strdup_printf ("h264parse ! %s ! capsfilter caps=video/x-raw,format=NV12", dec);
  GstHarness *h = gst_harness_new_parse (desc);

  g_free (desc);
  gst_harness_set_src_caps_str (h, "video/x-h264,stream-format=byte-stream,alignment=au");
  return h;
}

static void
harness_push_aus (GstHarness * h, GPtrArray * aus, guint from, guint to)
{
  for (guint i = from; i < to; i++)
    g_assert_cmpint (gst_harness_push (h, gst_buffer_new_wrapped_bytes (aus->pdata[i])), ==,
        GST_FLOW_OK);
}

static void
harness_collect (GstHarness * h, GPtrArray * crcs)
{
  GstBuffer *buf;

  while ((buf = gst_harness_try_pull (h)) != NULL) {
    GstCaps *caps = gst_pad_get_current_caps (h->sinkpad);
    GstVideoInfo info;
    GstVideoFrame frame;

    g_assert_true (gst_video_info_from_caps (&info, caps));
    g_assert_true (gst_video_frame_map (&frame, &info, buf, GST_MAP_READ));
    if (crcs != NULL)
      g_ptr_array_add (crcs, rds_test_nv12_checksum (&frame));
    gst_video_frame_unmap (&frame);
    gst_caps_unref (caps);
    gst_buffer_unref (buf);
  }
}

/* At most @max frames pending in the base class (after EOS: none, frames the parser never displays are released). */
static void
assert_pending_frames (GstHarness * h, guint max)
{
  GstElement *dec = gst_harness_find_element (h, "rdsvideodec");
  GList *frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (dec));

  g_assert_cmpuint (g_list_length (frames), <=, max);
  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);
  gst_object_unref (dec);
}

/* Flush (seek): ten frames in, flush, then the whole stream again from its first frame: the output after the flush
 * matches the reference from frame 0 (no stale reference pictures, no leftovers from before the flush). */
static void
test_flush (void)
{
  GPtrArray *aus, *ref, *got;
  GBytes *stream;
  GstHarness *h;
  GstSegment seg;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  aus = rds_test_encode_aus ("h264", "video/x-raw,format=NV12,width=320,height=240,framerate=30/1", 30, 2,
      "ball", NULL);
  stream = rds_test_concat ((GBytes * const *) aus->pdata, aus->len);
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);

  h = new_dec_harness ("rdsvideodec");
  harness_push_aus (h, aus, 0, 10);
  harness_collect (h, NULL);
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (h, gst_event_new_segment (&seg)));
  harness_collect (h, NULL);
  harness_push_aus (h, aus, 0, aus->len);
  g_assert_true (gst_harness_push_event (h, gst_event_new_eos ()));
  got = g_ptr_array_new_with_free_func (g_free);
  harness_collect (h, got);
  assert_same_crcs (ref, got, "after flush");
  assert_pending_frames (h, 0);

  gst_harness_teardown (h);
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
  g_ptr_array_unref (aus);
}

/* The SPS and PPS NAL units (with their start codes) of an H.264 access unit. */
static GBytes *
h264_parameter_sets (GBytes * au)
{
  gsize size;
  const guint8 *d = g_bytes_get_data (au, &size);
  GByteArray *out = g_byte_array_new ();
  gsize i = 0;

  while (i + 3 < size) {
    gsize start, next;
    guint type;

    if (!(d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)) {
      i++;
      continue;
    }
    start = (i > 0 && d[i - 1] == 0) ? i - 1 : i;
    type = d[i + 3] & 0x1f;
    for (next = i + 3; next + 2 < size && !(d[next] == 0 && d[next + 1] == 0 && d[next + 2] == 1); next++);
    if (next + 2 >= size)
      next = size;
    else if (d[next - 1] == 0)
      next--;
    if (type == 7 || type == 8)
      g_byte_array_append (out, d + start, (guint) (next - start));
    i = next;
  }
  g_assert_cmpuint (out->len, >, 0);
  return g_byte_array_free_to_bytes (out);
}

/* A stream that starts in the middle of a GOP (parameter sets, then P-frames whose references are missing): those
 * frames fail to decode and never come out; they are dropped right away instead of piling up in the base class
 * until EOS. From the key frame on, the output matches the reference. */
static void
test_undisplayed_frames (void)
{
  GPtrArray *aus, *ref, *got;
  GBytes *head[2], *first, *stream;
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  aus = rds_test_encode_aus ("h264", "video/x-raw,format=NV12,width=320,height=240,framerate=30/1", 20, 0,
      "ball", "key-int-max=10");
  stream = rds_test_concat ((GBytes * const *) aus->pdata, aus->len);
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);
  head[0] = h264_parameter_sets (aus->pdata[0]);
  head[1] = aus->pdata[3];
  first = rds_test_concat (head, 2);
  h = new_dec_harness ("rdsvideodec");
  g_assert_cmpint (gst_harness_push (h, gst_buffer_new_wrapped_bytes (first)), ==, GST_FLOW_OK);
  g_bytes_unref (first);
  harness_push_aus (h, aus, 4, aus->len);
  /* no B-frames: at most the picture being displayed is pending */
  assert_pending_frames (h, 1);
  g_assert_true (gst_harness_push_event (h, gst_event_new_eos ()));
  got = g_ptr_array_new_with_free_func (g_free);
  harness_collect (h, got);
  assert_pending_frames (h, 0);
  g_ptr_array_remove_range (ref, 0, 10);
  assert_same_crcs (ref, got, "from the key frame on");

  gst_harness_teardown (h);
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (head[0]);
  g_bytes_unref (stream);
  g_ptr_array_unref (aus);
}

/* drop-frame-interval counts displayed frames from the last flush: after a flush mid-phase, the output starts again
 * at frame 0 of the new segment. */
static void
test_drop_frame_interval_flush (void)
{
  GPtrArray *aus, *ref, *got;
  GBytes *stream;
  GstHarness *h;
  GstSegment seg;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  aus = rds_test_encode_aus ("h264", "video/x-raw,format=NV12,width=320,height=240,framerate=30/1", 30, 0,
      "ball", NULL);
  stream = rds_test_concat ((GBytes * const *) aus->pdata, aus->len);
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);

  h = new_dec_harness ("rdsvideodec drop-frame-interval=5");
  harness_push_aus (h, aus, 0, 7);
  harness_collect (h, NULL);
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (h, gst_event_new_segment (&seg)));
  harness_collect (h, NULL);
  harness_push_aus (h, aus, 0, aus->len);
  g_assert_true (gst_harness_push_event (h, gst_event_new_eos ()));
  got = g_ptr_array_new_with_free_func (g_free);
  harness_collect (h, got);
  g_assert_cmpuint (got->len, ==, 6);
  for (guint i = 0; i < got->len; i++)
    g_assert_cmpstr (got->pdata[i], ==, ref->pdata[5 * i]);

  gst_harness_teardown (h);
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
  g_ptr_array_unref (aus);
}

/* Switching skip-frames from decode_key back to decode_all while playing, before frame @data: the delta frames after
 * the switch may reference skipped pictures, so output resumes at the next key frame, and every frame that comes
 * out is correct. Switching right after a key frame (11) waits too: conservative, since only IDR-only streams could
 * resume there. */
static void
test_skip_frames_switch (gconstpointer data)
{
  guint at = GPOINTER_TO_UINT (data);
  GPtrArray *aus, *ref, *got;
  GBytes *stream;
  GstHarness *h;
  GstElement *dec;
  static const guint expected[] = { 0, 10, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29 };

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  aus = rds_test_encode_aus ("h264", "video/x-raw,format=NV12,width=320,height=240,framerate=30/1", 30, 0,
      "ball", "key-int-max=10");
  stream = rds_test_concat ((GBytes * const *) aus->pdata, aus->len);
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);
  g_assert_cmpuint (ref->len, ==, 30);

  h = new_dec_harness ("rdsvideodec skip-frames=decode_key");
  dec = gst_harness_find_element (h, "rdsvideodec");
  harness_push_aus (h, aus, 0, at);
  g_object_set (dec, "skip-frames", 0, NULL);
  harness_push_aus (h, aus, at, aus->len);
  g_assert_true (gst_harness_push_event (h, gst_event_new_eos ()));
  got = g_ptr_array_new_with_free_func (g_free);
  harness_collect (h, got);
  g_assert_cmpuint (got->len, ==, G_N_ELEMENTS (expected));
  for (guint i = 0; i < got->len; i++)
    g_assert_cmpstr (got->pdata[i], ==, ref->pdata[expected[i]]);

  gst_object_unref (dec);
  gst_harness_teardown (h);
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
  g_ptr_array_unref (aus);
}

/* drop-frame-interval=5 keeps displayed frames 0, 5, 10, ... */
static void
test_drop_frame_interval (void)
{
  GBytes *stream;
  GPtrArray *ref, *got;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  stream = rds_test_encode ("h264", 320, 240, 30, 2, "ball");
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);
  got = rds_test_decode_crc (stream, "h264", "rdsvideodec drop-frame-interval=5", TRUE);
  g_assert_cmpuint (got->len, ==, 6);
  for (guint i = 0; i < got->len; i++)
    g_assert_cmpstr (got->pdata[i], ==, ref->pdata[5 * i]);
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
}

/* skip-frames=2 (decode_key) decodes the key frames only. */
static void
test_skip_frames_key (void)
{
  GBytes *stream;
  GPtrArray *ref, *got;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  stream = rds_test_encode_full ("h264", 320, 240, 30, 0, "ball", "key-int-max=10");
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);
  got = rds_test_decode_crc (stream, "h264", "rdsvideodec skip-frames=decode_key", FALSE);
  g_assert_cmpuint (ref->len, ==, 30);
  g_assert_cmpuint (got->len, ==, 3);
  for (guint i = 0; i < got->len; i++)
    g_assert_cmpstr (got->pdata[i], ==, ref->pdata[10 * i]);
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
}

/* skip-frames=1 (decode_non_ref, Jetson-only on DeepStream) decodes everything, as on DeepStream dGPU. */
static void
test_skip_frames_non_ref (void)
{
  GBytes *stream;
  GPtrArray *ref, *got;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  stream = rds_test_encode ("h264", 320, 240, 10, 2, "ball");
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);
  got = rds_test_decode_crc (stream, "h264", "rdsvideodec skip-frames=1 low-latency-mode=true", FALSE);
  assert_same_crcs (ref, got, "skip-frames=1");
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
}

static void
test_extra_surfaces (void)
{
  GBytes *stream;
  GPtrArray *ref, *got;

  RDS_TEST_REQUIRE_GPU ();
  REQUIRE_VA_H264 ();
  stream = rds_test_encode ("h264", 640, 360, 20, 2, "ball");
  ref = rds_test_decode_crc (stream, "h264", "vah264dec", FALSE);
  got = rds_test_decode_crc (stream, "h264", "rdsvideodec num-extra-surfaces=4", TRUE);
  assert_same_crcs (ref, got, "num-extra-surfaces=4");
  g_ptr_array_unref (got);
  g_ptr_array_unref (ref);
  g_bytes_unref (stream);
}

/* Waits for the first error or EOS on the bus; returns the error (NULL at EOS). */
static GError *
wait_for_error (GstElement * pipe)
{
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  GError *err = NULL;

  g_assert_nonnull (msg);
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR)
    gst_message_parse_error (msg, &err, NULL);
  gst_message_unref (msg);
  gst_object_unref (bus);
  return err;
}

/* A GPU that doesn't exist fails the start with an error message, not a crash. */
static void
test_invalid_gpu (void)
{
  GstElement *pipe;
  GError *err = NULL;

  RDS_TEST_REQUIRE_GPU ();
  pipe = gst_parse_launch ("appsrc caps=video/x-h264,stream-format=byte-stream,alignment=au "
      "! rdsvideodec gpu-id=99 ! fakesink", &err);
  g_assert_no_error (err);
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), ==, GST_STATE_CHANGE_FAILURE);
  err = wait_for_error (pipe);
  g_assert_error (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND);
  g_assert_nonnull (strstr (err->message, "99"));
  g_error_free (err);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
}

static gboolean
encoder_takes_format (const gchar * enc, const gchar * format)
{
  GstElementFactory *f = gst_element_factory_find (enc);
  gboolean ok = FALSE;
  gchar *str = g_strdup_printf ("video/x-raw, format=%s", format);
  GstCaps *want = gst_caps_from_string (str);

  if (f != NULL) {
    for (const GList * l = gst_element_factory_get_static_pad_templates (f); l != NULL; l = l->next) {
      GstStaticPadTemplate *t = l->data;
      GstCaps *caps;

      if (t->direction != GST_PAD_SINK)
        continue;
      caps = gst_static_caps_get (&t->static_caps);
      ok |= gst_caps_can_intersect (caps, want);
      gst_caps_unref (caps);
    }
    gst_object_unref (f);
  }
  gst_caps_unref (want);
  g_free (str);
  return ok;
}

/* 10-bit input: a stream error naming the bit depth. */
static void
test_10bit_error (void)
{
  GstElement *pipe, *src;
  GError *err = NULL;
  GPtrArray *aus;
  GBytes *stream;

  RDS_TEST_REQUIRE_GPU ();
  if (!rds_test_have_element ("vah265enc") || !encoder_takes_format ("vah265enc", "P010_10LE")) {
    g_test_skip ("vah265enc cannot encode P010_10LE");
    return;
  }
  aus = rds_test_encode_aus ("h265", "video/x-raw,format=P010_10LE,width=320,height=240,framerate=30/1", 5, 0,
      "ball", NULL);
  stream = rds_test_concat ((GBytes * const *) aus->pdata, aus->len);
  pipe = gst_parse_launch ("appsrc name=src caps=video/x-h265,stream-format=byte-stream format=bytes "
      "! h265parse ! rdsvideodec ! video/x-raw,format=NV12 ! fakesink", &err);
  g_assert_no_error (err);
  src = gst_bin_get_by_name (GST_BIN (pipe), "src");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  (void) gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new_wrapped_bytes (stream));
  (void) gst_app_src_end_of_stream (GST_APP_SRC (src));
  err = wait_for_error (pipe);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE);
  g_test_message ("%s", err->message);
  g_assert_nonnull (strstr (err->message, "bit depth 10"));
  g_error_free (err);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (src);
  gst_object_unref (pipe);
  g_bytes_unref (stream);
  g_ptr_array_unref (aus);
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
  g_test_add_data_func ("/gpu/videodec/rds-output", GINT_TO_POINTER (-1), test_rds_output);
  g_test_add_data_func ("/gpu/videodec/memtype-pinned", GINT_TO_POINTER (1), test_rds_output);
  g_test_add_data_func ("/gpu/videodec/memtype-unified", GINT_TO_POINTER (2), test_rds_output);
  g_test_add_func ("/gpu/videodec/pts-b-frames", test_pts_b_frames);
  for (guint i = 0; i < G_N_ELEMENTS (h265_clips); i++) {
    gchar *path = g_strdup_printf ("/gpu/videodec/h265-parity-%ux%u-b%u", h265_clips[i].width,
        h265_clips[i].height, h265_clips[i].b_frames);
    g_test_add_data_func (path, &h265_clips[i], test_h265_parity);
    g_free (path);
  }
  for (guint i = 0; i < G_N_ELEMENTS (size_changes); i++) {
    gchar *path = g_strdup_printf ("/gpu/videodec/size-change-%s-%ux%u-to-%ux%u", size_changes[i].codec,
        size_changes[i].a.width, size_changes[i].a.height, size_changes[i].b.width, size_changes[i].b.height);
    g_test_add_data_func (path, &size_changes[i], test_size_change);
    g_free (path);
  }
  g_test_add_func ("/gpu/videodec/framerate-change", test_framerate_change);
  g_test_add_func ("/gpu/videodec/flush", test_flush);
  g_test_add_func ("/gpu/videodec/undisplayed-frames", test_undisplayed_frames);
  g_test_add_func ("/gpu/videodec/drop-frame-interval", test_drop_frame_interval);
  g_test_add_func ("/gpu/videodec/drop-frame-interval-flush", test_drop_frame_interval_flush);
  g_test_add_data_func ("/gpu/videodec/skip-frames-switch-at-15", GUINT_TO_POINTER (15), test_skip_frames_switch);
  g_test_add_data_func ("/gpu/videodec/skip-frames-switch-at-11", GUINT_TO_POINTER (11), test_skip_frames_switch);
  g_test_add_func ("/gpu/videodec/skip-frames-key", test_skip_frames_key);
  g_test_add_func ("/gpu/videodec/skip-frames-non-ref", test_skip_frames_non_ref);
  g_test_add_func ("/gpu/videodec/num-extra-surfaces", test_extra_surfaces);
  g_test_add_func ("/gpu/videodec/invalid-gpu-id", test_invalid_gpu);
  g_test_add_func ("/gpu/videodec/10-bit-error", test_10bit_error);
  return g_test_run ();
}
