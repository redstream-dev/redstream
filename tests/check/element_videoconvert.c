/* rdsvideoconvert: factory, caps, properties, helpers (cpu) and memory:RDS conversion (gpu, skipped without a
 * ROCm device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include "../../gst/rdsvideo/rds-video-util.h"

#include <gst/base/gstbasetransform.h>
#include <gst/check/gstharness.h>
#include <rds/rds.h>
#include <string.h>

static const gchar *formats[] = { "NV12", "I420", "RGBA", "BGRA", "RGBx", "BGRx", "RGB", "BGR", "GRAY8" };

/* ---- cpu ---- */

static void
check_template_caps (GstCaps * caps)
{
  gboolean seen_rds = FALSE, seen_sys = FALSE;

  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *s = gst_caps_get_structure (caps, i);
    GstCapsFeatures *f = gst_caps_get_features (caps, i);
    const GValue *list = gst_structure_get_value (s, "format");

    g_assert_true (gst_structure_has_name (s, "video/x-raw"));
    if (gst_caps_features_contains (f, RDS_CAPS_FEATURE_MEMORY_RDS))
      seen_rds = TRUE;
    else if (gst_caps_features_is_equal (f, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
      seen_sys = TRUE;
    g_assert_nonnull (list);
    g_assert_true (GST_VALUE_HOLDS_LIST (list));
    g_assert_cmpuint (gst_value_list_get_size (list), ==, G_N_ELEMENTS (formats));
    for (guint k = 0; k < G_N_ELEMENTS (formats); k++) {
      gboolean found = FALSE;
      for (guint j = 0; j < gst_value_list_get_size (list); j++)
        found |= g_strcmp0 (g_value_get_string (gst_value_list_get_value (list, j)), formats[k]) == 0;
      g_assert_true (found);
    }
  }
  g_assert_true (seen_rds);
  g_assert_true (seen_sys);
}

static void
test_factory (void)
{
  GstElementFactory *factory = gst_element_factory_find ("rdsvideoconvert");
  const GList *l;
  guint n = 0;

  g_assert_nonnull (factory);
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), ==, GST_RANK_NONE);
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS), ==,
      "Filter/Converter/Video/Scaler");
  for (l = gst_element_factory_get_static_pad_templates (factory); l != NULL; l = l->next, n++) {
    GstCaps *caps = gst_static_caps_get (&((GstStaticPadTemplate *) l->data)->static_caps);
    check_template_caps (caps);
    gst_caps_unref (caps);
  }
  g_assert_cmpuint (n, ==, 2);
  gst_object_unref (factory);
}

static GParamSpec *
find_prop (GstElement * e, const gchar * name, GType type)
{
  GParamSpec *p = g_object_class_find_property (G_OBJECT_GET_CLASS (e), name);

  g_assert_nonnull (p);
  g_assert_true (G_TYPE_CHECK_INSTANCE_TYPE (p, type));
  g_assert_true (p->flags & G_PARAM_READWRITE);
  return p;
}

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideoconvert", NULL);
  static const struct
  {
    gint value;
    const gchar *nick;
  } mem_types[] = {
    { 0, "nvbuf-mem-default" }, { 1, "nvbuf-mem-cuda-pinned" }, { 2, "nvbuf-mem-cuda-device" },
    { 3, "nvbuf-mem-cuda-unified" },
  };
  GParamSpec *p;
  GEnumClass *ec;
  guint u;
  gint en;
  gboolean b;

  g_assert_nonnull (e);

  p = find_prop (e, "gpu-id", G_TYPE_PARAM_UINT);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->default_value, ==, 0);
  g_object_get (e, "gpu-id", &u, NULL);
  g_assert_cmpuint (u, ==, 0);

  p = find_prop (e, "nvbuf-memory-type", G_TYPE_PARAM_ENUM);
  g_assert_cmpstr (g_type_name (p->value_type), ==, "GstRdsVidConvBufMemoryType");
  g_assert_cmpint (G_PARAM_SPEC_ENUM (p)->default_value, ==, 0);
  ec = G_PARAM_SPEC_ENUM (p)->enum_class;
  g_assert_cmpuint (ec->n_values, ==, G_N_ELEMENTS (mem_types));
  for (guint i = 0; i < G_N_ELEMENTS (mem_types); i++) {
    GEnumValue *v = g_enum_get_value (ec, mem_types[i].value);
    g_assert_nonnull (v);
    g_assert_cmpstr (v->value_nick, ==, mem_types[i].nick);
  }
  /* configs set it by integer or nick */
  gst_util_set_object_arg (G_OBJECT (e), "nvbuf-memory-type", "3");
  g_object_get (e, "nvbuf-memory-type", &en, NULL);
  g_assert_cmpint (en, ==, 3);
  gst_util_set_object_arg (G_OBJECT (e), "nvbuf-memory-type", "nvbuf-mem-cuda-device");
  g_object_get (e, "nvbuf-memory-type", &en, NULL);
  g_assert_cmpint (en, ==, 2);

  p = find_prop (e, "output-buffers", G_TYPE_PARAM_UINT);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->default_value, ==, 4);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->minimum, ==, 1);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->maximum, ==, G_MAXUINT);

  p = find_prop (e, "disable-passthrough", G_TYPE_PARAM_BOOLEAN);
  g_assert_false (G_PARAM_SPEC_BOOLEAN (p)->default_value);
  g_object_set (e, "disable-passthrough", TRUE, NULL);
  g_object_get (e, "disable-passthrough", &b, NULL);
  g_assert_true (b);

  p = find_prop (e, "silent", G_TYPE_PARAM_BOOLEAN);
  g_assert_false (G_PARAM_SPEC_BOOLEAN (p)->default_value);

  gst_object_unref (e);
}

static void
test_parse_rect (void)
{
  const RdsBufSurfTransformRect sentinel = {.top = 7,.left = 7,.width = 7,.height = 7 };
  static const gchar *bad[] = {
    "1:2:3", "1:2:3:4:5", "-1:2:3:4", "1:2:-3:4", "a:b:c:d", "1:2:3:4x", " 1:2:3:4", "1::3:4", "1:2:3:",
    "+1:2:3:4", "4294967296:0:1:1", "0:0:99999999999999999999:1", ":::", "1;2;3;4",
  };
  RdsBufSurfTransformRect r;

  g_assert_true (rds_video_parse_rect ("10:20:100:50", &r));
  g_assert_cmpuint (r.left, ==, 10);
  g_assert_cmpuint (r.top, ==, 20);
  g_assert_cmpuint (r.width, ==, 100);
  g_assert_cmpuint (r.height, ==, 50);

  g_assert_true (rds_video_parse_rect ("0:0:4294967295:1", &r));
  g_assert_cmpuint (r.width, ==, G_MAXUINT32);

  r = sentinel;
  g_assert_true (rds_video_parse_rect ("", &r));
  g_assert_cmpuint (r.left | r.top | r.width | r.height, ==, 0);
  r = sentinel;
  g_assert_true (rds_video_parse_rect (NULL, &r));
  g_assert_cmpuint (r.left | r.top | r.width | r.height, ==, 0);

  for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
    r = sentinel;
    if (rds_video_parse_rect (bad[i], &r))
      g_error ("\"%s\" was accepted", bad[i]);
    g_assert_cmpmem (&r, sizeof (r), &sentinel, sizeof (sentinel));
  }
}

static void
test_wrap_surface (void)
{
  static const struct
  {
    GstVideoFormat fmt;
    RdsBufSurfaceColorFormat cfmt;
    guint planes;
    guint bpp[3];
  } cases[] = {
    { GST_VIDEO_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_NV12, 2, { 1, 2 } },
    { GST_VIDEO_FORMAT_I420, RDS_BUF_COLOR_FORMAT_YUV420, 3, { 1, 1, 1 } },
    { GST_VIDEO_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA, 1, { 4 } },
    { GST_VIDEO_FORMAT_BGR, RDS_BUF_COLOR_FORMAT_BGR, 1, { 3 } },
    { GST_VIDEO_FORMAT_GRAY8, RDS_BUF_COLOR_FORMAT_GRAY8, 1, { 1 } },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    GstVideoInfo info;
    GstVideoFrame frame;
    GstBuffer *buf;
    RdsBufSurface surf;
    RdsBufSurfaceParams params;
    guint8 *base;

    g_assert_true (gst_video_info_set_format (&info, cases[i].fmt, 37, 23));
    buf = gst_buffer_new_allocate (NULL, GST_VIDEO_INFO_SIZE (&info), NULL);
    g_assert_true (gst_video_frame_map (&frame, &info, buf, GST_MAP_READ));
    g_assert_true (rds_video_frame_wrap_surface (&frame, &surf, &params));

    base = GST_VIDEO_FRAME_PLANE_DATA (&frame, 0);
    g_assert_cmpuint (surf.batchSize, ==, 1);
    g_assert_cmpuint (surf.numFilled, ==, 1);
    g_assert_cmpint (surf.memType, ==, RDS_BUF_MEM_SYSTEM);
    g_assert_null (surf._priv);
    g_assert_true (surf.surfaceList == &params);
    g_assert_true (params.dataPtr == base);
    g_assert_cmpint (params.colorFormat, ==, cases[i].cfmt);
    g_assert_cmpuint (params.width, ==, 37);
    g_assert_cmpuint (params.height, ==, 23);
    g_assert_cmpuint (params.pitch, ==, GST_VIDEO_FRAME_PLANE_STRIDE (&frame, 0));
    g_assert_cmpuint (params.planeParams.num_planes, ==, cases[i].planes);
    for (guint p = 0; p < cases[i].planes; p++) {
      guint div = p == 0 ? 1 : 2;
      g_assert_cmpuint (params.planeParams.offset[p], ==,
          (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&frame, p) - base);
      g_assert_cmpuint (params.planeParams.pitch[p], ==, GST_VIDEO_FRAME_PLANE_STRIDE (&frame, p));
      g_assert_cmpuint (params.planeParams.width[p], ==, (37 + div - 1) / div);
      g_assert_cmpuint (params.planeParams.height[p], ==, (23 + div - 1) / div);
      g_assert_cmpuint (params.planeParams.bytesPerPix[p], ==, cases[i].bpp[p]);
      g_assert_cmpuint (params.planeParams.psize[p], ==,
          params.planeParams.pitch[p] * params.planeParams.height[p]);
    }
    gst_video_frame_unmap (&frame);
    gst_buffer_unref (buf);
  }
}


/* ---- cpu: caps transform and fixation, through the class vfuncs ---- */

static GstCaps *
transform_caps (GstElement * e, const gchar * in)
{
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (e);
  GstCaps *caps = gst_caps_from_string (in);
  GstCaps *ret = klass->transform_caps (GST_BASE_TRANSFORM (e), GST_PAD_SINK, caps, NULL);

  gst_caps_unref (caps);
  return ret;
}

/* What the base class does: transform, keep what the src template and downstream allow, fixate. */
static GstCaps *
negotiate (GstElement * e, const gchar * in, const gchar * downstream)
{
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (e);
  GstCaps *caps = gst_caps_from_string (in), *peer = gst_caps_from_string (downstream);
  GstCaps *templ = gst_pad_get_pad_template_caps (GST_BASE_TRANSFORM_SRC_PAD (e));
  GstCaps *other = transform_caps (e, in), *tmp;

  tmp = gst_caps_intersect_full (peer, templ, GST_CAPS_INTERSECT_FIRST);
  gst_caps_replace (&peer, tmp);
  gst_caps_unref (tmp);
  tmp = gst_caps_intersect_full (peer, other, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (other);
  other = klass->fixate_caps (GST_BASE_TRANSFORM (e), GST_PAD_SINK, caps, tmp);
  g_assert_true (gst_caps_is_fixed (other));
  gst_caps_unref (templ);
  gst_caps_unref (peer);
  gst_caps_unref (caps);
  return other;
}

static void
assert_caps_str (GstCaps * caps, const gchar * expected)
{
  GstCaps *exp = gst_caps_from_string (expected);

  if (!gst_caps_is_equal (caps, exp)) {
    gchar *got = gst_caps_to_string (caps);
    g_error ("got %s, expected %s", got, expected);
  }
  gst_caps_unref (exp);
}

#define NV12_320 "video/x-raw(memory:RDS),format=NV12,width=320,height=240,framerate=30/1"
#define RGBA_160 "video/x-raw(memory:RDS),format=RGBA,width=160,height=120,framerate=30/1"

#define SYS_IN "video/x-raw,format=NV12,width=320,height=240,framerate=30/1,pixel-aspect-ratio=1/1," \
  "colorimetry=bt601,chroma-site=jpeg,interlace-mode=progressive"
#define RDS_IN "video/x-raw(memory:RDS),format=NV12,width=320,height=240,framerate=30/1," \
  "pixel-aspect-ratio=1/1,colorimetry=bt601,chroma-site=jpeg"

static void
test_transform_caps (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideoconvert", NULL);
  GstCaps *caps;

  /* each input's own memory first, then the other; format, size and colour fields are free */
  caps = transform_caps (e, SYS_IN);
  g_assert_cmpuint (gst_caps_get_size (caps), ==, 2);
  g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 0),
          GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
  g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, 1), RDS_CAPS_FEATURE_MEMORY_RDS));
  for (guint i = 0; i < 2; i++) {
    GstStructure *st = gst_caps_get_structure (caps, i);
    g_assert_false (gst_structure_has_field (st, "format"));
    g_assert_false (gst_structure_has_field (st, "width"));
    g_assert_false (gst_structure_has_field (st, "pixel-aspect-ratio"));
    g_assert_false (gst_structure_has_field (st, "colorimetry"));
    g_assert_true (gst_structure_has_field (st, "framerate"));
  }
  gst_caps_unref (caps);

  caps = transform_caps (e, RDS_IN);
  g_assert_cmpuint (gst_caps_get_size (caps), ==, 2);
  g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, 0), RDS_CAPS_FEATURE_MEMORY_RDS));
  g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 1),
          GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
  gst_caps_unref (caps);
  gst_object_unref (e);
}

static void
test_fixate (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideoconvert", NULL);
  GstCaps *caps;

  /* downstream allows the input caps: exactly those (passthrough), in the input's memory */
  caps = negotiate (e, SYS_IN, "video/x-raw,format=NV12");
  assert_caps_str (caps, SYS_IN);
  gst_caps_unref (caps);
  caps = negotiate (e, SYS_IN, "video/x-raw(ANY)");
  assert_caps_str (caps, SYS_IN);
  gst_caps_unref (caps);
  caps = negotiate (e, RDS_IN, "video/x-raw(memory:RDS)");
  assert_caps_str (caps, RDS_IN);
  gst_caps_unref (caps);

  /* format only: the input size and PAR; YUV colorimetry does not carry over to RGB */
  caps = negotiate (e, SYS_IN, "video/x-raw,format=RGBA");
  assert_caps_str (caps, "video/x-raw,format=RGBA,width=320,height=240,framerate=30/1,pixel-aspect-ratio=1/1,"
      "interlace-mode=progressive");
  gst_caps_unref (caps);

  /* another 4:2:0 format keeps colorimetry and siting */
  caps = negotiate (e, RDS_IN, "video/x-raw(memory:RDS),format=I420");
  assert_caps_str (caps, "video/x-raw(memory:RDS),format=I420,width=320,height=240,framerate=30/1,"
      "pixel-aspect-ratio=1/1,colorimetry=bt601,chroma-site=jpeg");
  gst_caps_unref (caps);

  /* size only: the input format, with its colour fields */
  caps = negotiate (e, SYS_IN, "video/x-raw,width=160,height=120");
  assert_caps_str (caps, "video/x-raw,format=NV12,width=160,height=120,framerate=30/1,pixel-aspect-ratio=1/1,"
      "colorimetry=bt601,chroma-site=jpeg,interlace-mode=progressive");
  gst_caps_unref (caps);

  /* downstream asks for another PAR: nearest allowed */
  caps = negotiate (e, SYS_IN, "video/x-raw,width=160,height=120,pixel-aspect-ratio={4/3,10/11}");
  g_assert_true (gst_structure_has_field (gst_caps_get_structure (caps, 0), "pixel-aspect-ratio"));
  {
    gint n, d;
    g_assert_true (gst_structure_get_fraction (gst_caps_get_structure (caps, 0), "pixel-aspect-ratio", &n, &d));
    g_assert_cmpint (n, ==, 10);
    g_assert_cmpint (d, ==, 11);
  }
  gst_caps_unref (caps);

  /* downstream adds a field the input lacks, as a list: still fixed (first value) */
  caps = negotiate (e, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1",
      "video/x-raw,format=I420,colorimetry={bt601,bt709}");
  assert_caps_str (caps, "video/x-raw,format=I420,width=320,height=240,framerate=30/1,colorimetry=bt601");
  gst_caps_unref (caps);
  caps = negotiate (e, SYS_IN, "video/x-raw,format=I420,colorimetry={bt709,bt2020}");
  g_assert_cmpstr (gst_structure_get_string (gst_caps_get_structure (caps, 0), "colorimetry"), ==, "bt709");
  gst_caps_unref (caps);

  /* no PAR on the input: none added */
  caps = negotiate (e, "video/x-raw,format=NV12,width=320,height=240,framerate=30/1", "video/x-raw,format=RGBA");
  assert_caps_str (caps, "video/x-raw,format=RGBA,width=320,height=240,framerate=30/1");
  gst_caps_unref (caps);

  gst_object_unref (e);
}

/* gpu-id reaches the pool proposed upstream. Only allocation would fail on a missing GPU, so any id works here. */
static void
test_propose_allocation_gpu_id (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideoconvert", NULL);
  GstBaseTransformClass *klass = GST_BASE_TRANSFORM_GET_CLASS (e);
  GstCaps *caps = gst_caps_from_string (NV12_320);
  GstQuery *decide = gst_query_new_allocation (caps, TRUE), *query = gst_query_new_allocation (caps, TRUE);
  GstBufferPool *proposed = NULL;
  GstStructure *config;
  guint batch = 0, gpu = 0;
  RdsBufSurfaceMemType mt = RDS_BUF_MEM_SYSTEM;

  g_assert_cmpint (gst_element_set_state (e, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  g_object_set (e, "gpu-id", 3, NULL);
  g_assert_true (klass->propose_allocation (GST_BASE_TRANSFORM (e), decide, query));
  g_assert_cmpuint (gst_query_get_n_allocation_pools (query), ==, 1);
  gst_query_parse_nth_allocation_pool (query, 0, &proposed, NULL, NULL, NULL);
  g_assert_true (RDS_IS_BUFFER_POOL (proposed));
  config = gst_buffer_pool_get_config (proposed);
  g_assert_true (rds_buffer_pool_config_get_surface_params (config, &batch, &mt, &gpu));
  g_assert_cmpuint (gpu, ==, 3);
  g_assert_cmpuint (batch, ==, 1);
  g_assert_cmpint (mt, ==, RDS_BUF_MEM_DEFAULT);
  gst_structure_free (config);

  gst_object_unref (proposed);
  gst_query_unref (query);
  gst_query_unref (decide);
  gst_caps_unref (caps);
  gst_element_set_state (e, GST_STATE_NULL);
  gst_object_unref (e);
}

/* Pool settings are refused during READY -> PAUSED too (state READY, next PAUSED). */
static void
test_mutable_in_transition (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideoconvert", NULL);
  guint v = 0;

  g_assert_cmpint (gst_element_set_state (e, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  GST_OBJECT_LOCK (e);
  GST_STATE_NEXT (e) = GST_STATE_PAUSED;
  GST_STATE_PENDING (e) = GST_STATE_PAUSED;
  GST_OBJECT_UNLOCK (e);
  g_test_expect_message ("rdsvideo", G_LOG_LEVEL_WARNING, "*output-buffers*");
  g_object_set (e, "output-buffers", 9, NULL);
  g_test_assert_expected_messages ();
  g_object_get (e, "output-buffers", &v, NULL);
  g_assert_cmpuint (v, ==, 4);

  GST_OBJECT_LOCK (e);
  GST_STATE_NEXT (e) = GST_STATE_VOID_PENDING;
  GST_STATE_PENDING (e) = GST_STATE_VOID_PENDING;
  GST_OBJECT_UNLOCK (e);
  g_object_set (e, "output-buffers", 9, NULL);
  g_object_get (e, "output-buffers", &v, NULL);
  g_assert_cmpuint (v, ==, 9);
  gst_element_set_state (e, GST_STATE_NULL);
  gst_object_unref (e);
}

/* ---- gpu helpers ---- */

static GstBufferPool *
make_pool (const gchar * caps_str, guint batch_size)
{
  GstBufferPool *pool = rds_buffer_pool_new ();
  GstCaps *caps = gst_caps_from_string (caps_str);
  GstStructure *config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, caps, 0, 1, 0);
  rds_buffer_pool_config_set_surface_params (config, batch_size, RDS_BUF_MEM_DEFAULT, 0);
  g_assert_true (gst_buffer_pool_set_config (pool, config));
  g_assert_true (gst_buffer_pool_set_active (pool, TRUE));
  gst_caps_unref (caps);
  return pool;
}

/* Fills every surface of @buf's batch with a deterministic pattern and marks them filled. */
static void
fill_buffer (GstBuffer * buf, guint32 seed)
{
  RdsBufSurface *s = rds_buffer_get_surface (buf);

  g_assert_nonnull (s);
  for (guint i = 0; i < s->batchSize; i++) {
    g_assert_nonnull (s->surfaceList[i].dataPtr);
    fill_random (s, i, seed + i);
  }
  s->numFilled = s->batchSize;
}

static void
snapshot_buffer (GstBuffer * buf, HostImage * h)
{
  RdsBufSurface *s = rds_buffer_get_surface (buf);

  g_assert_nonnull (s);
  snapshot (s, 0, h);
}

static GstHarness *
make_harness (const gchar * in_caps, const gchar * out_caps)
{
  GstHarness *h = gst_harness_new ("rdsvideoconvert");

  gst_harness_set_src_caps_str (h, in_caps);
  gst_harness_set_sink_caps_str (h, out_caps);
  return h;
}


/* ---- gpu ---- */

static void
test_convert_scale (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL, *out;
  GstCaps *caps, *expected;
  HostImage hin, hout, href;
  RefRect s = { 0, 0, 320, 240 }, d = { 0, 0, 160, 120 };

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness (NV12_320, RGBA_160);
  pool = make_pool (NV12_320, 1);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 3);
  GST_BUFFER_PTS (in) = 42 * GST_MSECOND;
  snapshot_buffer (in, &hin);

  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, 42 * GST_MSECOND);
  caps = gst_pad_get_current_caps (h->sinkpad);
  expected = gst_caps_from_string (RGBA_160);
  g_assert_true (gst_caps_is_equal (caps, expected));
  gst_caps_unref (expected);
  gst_caps_unref (caps);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 1);
  snapshot_buffer (out, &hout);

  /* the whole destination is written: start the reference from a copy of the output */
  href = hout;
  for (guint pl = 0; pl < hout.num_planes; pl++)
    href.im.plane[pl] = g_memdup2 (hout.im.plane[pl], (gsize) hout.im.pitch[pl] * hout.rows[pl]);
  memset (href.im.plane[0], 0, (gsize) href.im.pitch[0] * href.rows[0]);
  ref_transform (&hin.im, &href.im, s, d, FALSE);
  g_assert_cmpuint (max_diff (&hout, &href, TRUE), <=, 1);

  host_image_clear (&href);
  host_image_clear (&hout);
  host_image_clear (&hin);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

/* The element sets its own session (GPU, stream) before every transform: the streaming thread (here the test
 * thread, since the harness pushes synchronously) is shared with whatever else ran there. */
static void
test_session_per_call (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  hipStream_t other = NULL;
  gpointer element_stream = NULL;

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness (NV12_320, RGBA_160);
  pool = make_pool (NV12_320, 1);
  g_assert_cmpint (hipStreamCreate (&other), ==, hipSuccess);

  for (guint i = 0; i < 2; i++) {
    RdsBufSurfTransformConfigParams cfg = {.compute_mode = RdsBufSurfTransformCompute_GPU,.gpu_id = 0,
      .cuda_stream = other
    };
    GstBuffer *in = NULL, *out;

    g_assert_cmpint (rds_buf_surf_transform_set_session_params (&cfg), ==, RdsBufSurfTransformError_Success);
    g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
    fill_buffer (in, 20 + i);
    out = gst_harness_push_and_pull (h, in);
    g_assert_nonnull (out);
    g_assert_cmpint (rds_buf_surf_transform_get_session_params (&cfg), ==, RdsBufSurfTransformError_Success);
    g_assert_nonnull (cfg.cuda_stream);
    g_assert_true (cfg.cuda_stream != (gpointer) other);
    if (i == 0)
      element_stream = cfg.cuda_stream;
    else
      g_assert_true (cfg.cuda_stream == element_stream);
    gst_buffer_unref (out);
  }

  g_assert_cmpint (rds_buf_surf_transform_set_default_session (), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (hipStreamDestroy (other), ==, hipSuccess);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

static void
test_passthrough (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL, *out;
  HostImage hin, hout;

  RDS_TEST_REQUIRE_GPU ();
  pool = make_pool (NV12_320, 1);

  /* same caps on both sides: the input buffer itself comes out */
  h = make_harness (NV12_320, NV12_320);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 5);
  out = gst_harness_push_and_pull (h, gst_buffer_ref (in));
  g_assert_true (out == in);
  g_assert_true (gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (h->element)));
  gst_buffer_unref (out);
  gst_harness_teardown (h);

  /* disable-passthrough: a new buffer with the same pixels */
  h = gst_harness_new ("rdsvideoconvert");
  g_object_set (h->element, "disable-passthrough", TRUE, NULL);
  gst_harness_set_src_caps_str (h, NV12_320);
  gst_harness_set_sink_caps_str (h, NV12_320);
  snapshot_buffer (in, &hin);
  out = gst_harness_push_and_pull (h, gst_buffer_ref (in));
  g_assert_nonnull (out);
  g_assert_true (out != in);
  g_assert_true (rds_buffer_get_surface (out) != rds_buffer_get_surface (in));
  snapshot_buffer (out, &hout);
  g_assert_cmpuint (max_diff (&hin, &hout, TRUE), ==, 0);
  host_image_clear (&hout);
  host_image_clear (&hin);
  gst_buffer_unref (out);
  gst_harness_teardown (h);

  gst_buffer_unref (in);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

static void
test_batch_rejected (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL;
  GstBus *bus = gst_bus_new ();
  GstMessage *msg;
  GError *err = NULL;

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness (NV12_320, RGBA_160);
  gst_element_set_bus (h->element, bus);
  pool = make_pool (NV12_320, 2);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 9);
  g_assert_cmpuint (rds_buffer_get_surface (in)->batchSize, ==, 2);

  g_assert_cmpint (gst_harness_push (h, in), ==, GST_FLOW_ERROR);
  msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  gst_message_parse_error (msg, &err, NULL);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_NOT_IMPLEMENTED);
  g_clear_error (&err);
  gst_message_unref (msg);

  gst_element_set_bus (h->element, NULL);
  gst_object_unref (bus);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

static void
test_batch_meta_kept (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL, *out;
  RdsBatchMeta *batch, *got;
  RdsFrameMeta *frame;

  RDS_TEST_REQUIRE_GPU ();
  h = gst_harness_new ("rdsvideoconvert");
  /* downstream supports GstVideoMeta, so the output pool adds one describing the output */
  gst_harness_add_propose_allocation_meta (h, GST_VIDEO_META_API_TYPE, NULL);
  gst_harness_set_src_caps_str (h, NV12_320);
  gst_harness_set_sink_caps_str (h, RGBA_160);
  pool = make_pool (NV12_320, 1);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 11);
  g_assert_nonnull (gst_buffer_add_video_meta (in, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, 320, 240));
  batch = rds_create_batch_meta (1);
  frame = rds_acquire_frame_meta_from_pool (batch);
  frame->source_id = 7;
  rds_add_frame_meta_to_batch (batch, frame);
  g_assert_nonnull (rds_buffer_add_batch_meta (in, batch));

  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  got = gst_buffer_get_rds_batch_meta (out);
  g_assert_nonnull (got);
  g_assert_cmpuint (got->num_frames_in_batch, ==, 1);
  g_assert_cmpuint (((RdsFrameMeta *) got->frame_meta_list->data)->source_id, ==, 7);
  /* the output keeps its own video meta (from the output pool), not a copy of the input's */
  g_assert_nonnull (gst_buffer_get_video_meta (out));
  g_assert_cmpuint (gst_buffer_get_video_meta (out)->width, ==, 160);
  {
    gpointer state = NULL;
    guint n = 0;
    while (gst_buffer_iterate_meta_filtered (out, &state, GST_VIDEO_META_API_TYPE) != NULL)
      n++;
    g_assert_cmpuint (n, ==, 1);
  }

  gst_buffer_unref (out);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

static void
test_passthrough_full_caps (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL, *out;

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness (RDS_IN, "video/x-raw(memory:RDS),format=NV12");
  pool = make_pool (RDS_IN, 1);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 31);
  out = gst_harness_push_and_pull (h, gst_buffer_ref (in));
  g_assert_true (out == in);
  g_assert_true (gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (h->element)));
  gst_buffer_unref (out);
  gst_buffer_unref (in);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

/* Runs @desc (with an element named conv) to EOS; returns the converter (a new reference) and its src caps. */
static GstElement *
run_pipeline (const gchar * desc, GstCaps ** src_caps)
{
  GError *err = NULL;
  GstElement *pipe = gst_parse_launch (desc, &err), *conv;
  GstBus *bus;
  GstMessage *msg;

  g_assert_no_error (err);
  conv = gst_bin_get_by_name (GST_BIN (pipe), "conv");
  g_assert_nonnull (conv);
  bus = gst_element_get_bus (pipe);
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  msg = gst_bus_timed_pop_filtered (bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  *src_caps = gst_pad_get_current_caps (GST_BASE_TRANSFORM_SRC_PAD (conv));
  g_assert_nonnull (*src_caps);
  /* read while the pipeline is still running */
  g_object_set_data (G_OBJECT (conv), "was-passthrough",
      GINT_TO_POINTER (gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (conv))));
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (pipe);
  return conv;
}

static void
test_pipeline_passthrough (void)
{
  GstElement *conv;
  GstCaps *caps;

  RDS_TEST_REQUIRE_GPU ();
  /* system NV12 in, NV12 asked downstream: passthrough with the full upstream caps */
  conv = run_pipeline ("videotestsrc num-buffers=3 ! video/x-raw,format=NV12,width=320,height=240 "
      "! rdsvideoconvert name=conv ! video/x-raw,format=NV12 ! fakesink", &caps);
  g_assert_true (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (conv), "was-passthrough")));
  g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 0),
          GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
  gst_caps_unref (caps);
  gst_object_unref (conv);

  /* downstream accepts anything: the input's own (system) memory wins, again passthrough */
  conv = run_pipeline ("videotestsrc num-buffers=3 ! video/x-raw,format=NV12,width=320,height=240 "
      "! rdsvideoconvert name=conv ! fakesink", &caps);
  g_assert_true (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (conv), "was-passthrough")));
  g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 0),
          GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
  gst_caps_unref (caps);
  gst_object_unref (conv);
}

static void
test_passthrough_toggle (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstEvent *ev;

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness (NV12_320, NV12_320);
  pool = make_pool (NV12_320, 1);
  for (guint step = 0; step < 3; step++) {
    GstBuffer *in = NULL, *out;
    gboolean disabled = step == 1;

    if (step > 0) {
      while ((ev = gst_harness_try_pull_upstream_event (h)) != NULL)
        gst_event_unref (ev);
      g_object_set (h->element, "disable-passthrough", disabled, NULL);
      /* upstream is asked to renegotiate allocation */
      while ((ev = gst_harness_try_pull_upstream_event (h)) != NULL
          && GST_EVENT_TYPE (ev) != GST_EVENT_RECONFIGURE)
        gst_event_unref (ev);
      g_assert_nonnull (ev);
      gst_event_unref (ev);
    }
    g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
    fill_buffer (in, 40 + step);
    out = gst_harness_push_and_pull (h, gst_buffer_ref (in));
    g_assert_nonnull (out);
    g_assert_cmpint (gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (h->element)), ==, !disabled);
    if (disabled) {
      HostImage hin, hout;

      g_assert_true (out != in);
      snapshot_buffer (in, &hin);
      snapshot_buffer (out, &hout);
      g_assert_cmpuint (max_diff (&hin, &hout, TRUE), ==, 0);
      host_image_clear (&hout);
      host_image_clear (&hin);
    } else {
      g_assert_true (out == in);
    }
    gst_buffer_unref (out);
    gst_buffer_unref (in);
  }
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

/* stop forgets the negotiated caps: a later passthrough toggle does not act on stale ones */
static void
test_passthrough_reset_on_stop (void)
{
  GstElement *conv;
  GstCaps *caps;

  RDS_TEST_REQUIRE_GPU ();
  conv = run_pipeline ("videotestsrc num-buffers=1 ! video/x-raw,format=NV12,width=64,height=48 "
      "! rdsvideoconvert name=conv ! fakesink", &caps);
  gst_caps_unref (caps);
  g_assert_true (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (conv), "was-passthrough")));
  g_object_set (conv, "disable-passthrough", TRUE, NULL);
  g_object_set (conv, "disable-passthrough", FALSE, NULL);
  g_assert_false (gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (conv)));
  gst_object_unref (conv);
}

static void
test_crop_meta (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL, *out;
  GstVideoCropMeta *crop;
  HostImage hin, hout, href;
  RefRect s = { 10, 20, 100, 50 }, d = { 0, 0, 160, 120 };

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness (NV12_320, RGBA_160);
  pool = make_pool (NV12_320, 1);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 51);
  crop = gst_buffer_add_video_crop_meta (in);
  crop->x = 10;
  crop->y = 20;
  crop->width = 100;
  crop->height = 50;
  snapshot_buffer (in, &hin);

  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  g_assert_null (gst_buffer_get_video_crop_meta (out));
  snapshot_buffer (out, &hout);
  href = hout;
  href.im.plane[0] = g_malloc0 ((gsize) hout.im.pitch[0] * hout.rows[0]);
  ref_transform (&hin.im, &href.im, s, d, FALSE);
  g_assert_cmpuint (max_diff (&hout, &href, TRUE), <=, 1);

  host_image_clear (&href);
  host_image_clear (&hout);
  host_image_clear (&hin);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

static void
test_caps_query (void)
{
  GstHarness *h;
  GstCaps *caps;
  gboolean rds = FALSE, sys = FALSE;

  RDS_TEST_REQUIRE_GPU ();
  h = gst_harness_new ("rdsvideoconvert");
  gst_harness_set_src_caps_str (h, SYS_IN);
  caps = gst_pad_query_caps (GST_BASE_TRANSFORM_SRC_PAD (h->element), NULL);
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstCapsFeatures *f = gst_caps_get_features (caps, i);
    rds |= gst_caps_features_contains (f, RDS_CAPS_FEATURE_MEMORY_RDS);
    sys |= gst_caps_features_is_equal (f, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY);
  }
  g_assert_true (rds);
  g_assert_true (sys);
  g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 0),
          GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
  gst_caps_unref (caps);
  gst_harness_teardown (h);
}

static void
test_propose_allocation (void)
{
  GstHarness *h;
  GstBufferPool *pool, *proposed = NULL;
  GstBuffer *in = NULL, *out;
  GstCaps *caps = gst_caps_from_string (NV12_320);
  GstQuery *query;
  GstStructure *config;
  guint min = 0, batch = 0, gpu = 99;
  RdsBufSurfaceMemType mt = RDS_BUF_MEM_SYSTEM;

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness (NV12_320, RGBA_160);
  pool = make_pool (NV12_320, 1);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 61);
  out = gst_harness_push_and_pull (h, in);      /* negotiated, not passthrough */
  gst_buffer_unref (out);

  query = gst_query_new_allocation (caps, TRUE);
  g_assert_true (gst_pad_peer_query (h->srcpad, query));
  g_assert_cmpuint (gst_query_get_n_allocation_pools (query), >=, 1);
  gst_query_parse_nth_allocation_pool (query, 0, &proposed, NULL, &min, NULL);
  g_assert_true (RDS_IS_BUFFER_POOL (proposed));
  g_assert_cmpuint (min, ==, 2);
  config = gst_buffer_pool_get_config (proposed);
  g_assert_true (rds_buffer_pool_config_get_surface_params (config, &batch, &mt, &gpu));
  g_assert_cmpuint (batch, ==, 1);
  g_assert_cmpint (mt, ==, RDS_BUF_MEM_DEFAULT);
  g_assert_cmpuint (gpu, ==, 0);
  gst_structure_free (config);
  g_assert_true (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL));

  gst_object_unref (proposed);
  gst_query_unref (query);
  gst_caps_unref (caps);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

static void
test_decide_allocation (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideoconvert", NULL);
  GstHarness *h;
  GstBufferPool *pool, *outpool;
  GstBuffer *in = NULL, *out;
  GstStructure *config;
  guint min = 0, batch = 0, gpu = 99, v = 0;
  RdsBufSurfaceMemType mt = RDS_BUF_MEM_SYSTEM;

  RDS_TEST_REQUIRE_GPU ();
  g_object_set (e, "nvbuf-memory-type", 3, "output-buffers", 6, NULL);
  h = gst_harness_new_with_element (e, "sink", "src");
  gst_harness_set_src_caps_str (h, NV12_320);
  gst_harness_set_sink_caps_str (h, RGBA_160);
  pool = make_pool (NV12_320, 1);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 71);
  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  g_assert_cmpint (rds_buffer_get_surface (out)->memType, ==, RDS_BUF_MEM_HIP_UNIFIED);
  gst_buffer_unref (out);

  outpool = gst_base_transform_get_buffer_pool (GST_BASE_TRANSFORM (e));
  g_assert_true (RDS_IS_BUFFER_POOL (outpool));
  config = gst_buffer_pool_get_config (outpool);
  g_assert_true (gst_buffer_pool_config_get_params (config, NULL, NULL, &min, NULL));
  g_assert_cmpuint (min, ==, 6);
  g_assert_true (rds_buffer_pool_config_get_surface_params (config, &batch, &mt, &gpu));
  g_assert_cmpuint (batch, ==, 1);
  g_assert_cmpint (mt, ==, RDS_BUF_MEM_HIP_UNIFIED);
  g_assert_cmpuint (gpu, ==, 0);
  gst_structure_free (config);
  gst_object_unref (outpool);

  /* allocation settings are fixed while streaming */
  g_test_expect_message ("rdsvideo", G_LOG_LEVEL_WARNING, "*output-buffers*");
  g_object_set (e, "output-buffers", 9, NULL);
  g_test_expect_message ("rdsvideo", G_LOG_LEVEL_WARNING, "*nvbuf-memory-type*");
  g_object_set (e, "nvbuf-memory-type", 2, NULL);
  g_test_expect_message ("rdsvideo", G_LOG_LEVEL_WARNING, "*gpu-id*");
  g_object_set (e, "gpu-id", 1, NULL);
  g_test_assert_expected_messages ();
  g_object_get (e, "output-buffers", &v, NULL);
  g_assert_cmpuint (v, ==, 6);
  g_object_get (e, "nvbuf-memory-type", &v, NULL);
  g_assert_cmpuint (v, ==, 3);
  g_object_get (e, "gpu-id", &v, NULL);
  g_assert_cmpuint (v, ==, 0);

  gst_harness_teardown (h);
  gst_object_unref (e);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/videoconvert/factory", test_factory);
  g_test_add_func ("/cpu/videoconvert/properties", test_properties);
  g_test_add_func ("/cpu/videoconvert/parse-rect", test_parse_rect);
  g_test_add_func ("/cpu/videoconvert/wrap-surface", test_wrap_surface);
  g_test_add_func ("/cpu/videoconvert/transform-caps", test_transform_caps);
  g_test_add_func ("/cpu/videoconvert/fixate", test_fixate);
  g_test_add_func ("/cpu/videoconvert/propose-allocation-gpu-id", test_propose_allocation_gpu_id);
  g_test_add_func ("/cpu/videoconvert/mutable-in-transition", test_mutable_in_transition);
  g_test_add_func ("/gpu/videoconvert/convert-scale", test_convert_scale);
  g_test_add_func ("/gpu/videoconvert/session-per-call", test_session_per_call);
  g_test_add_func ("/gpu/videoconvert/passthrough", test_passthrough);
  g_test_add_func ("/gpu/videoconvert/batch-rejected", test_batch_rejected);
  g_test_add_func ("/gpu/videoconvert/batch-meta-kept", test_batch_meta_kept);
  g_test_add_func ("/gpu/videoconvert/passthrough-full-caps", test_passthrough_full_caps);
  g_test_add_func ("/gpu/videoconvert/pipeline-passthrough", test_pipeline_passthrough);
  g_test_add_func ("/gpu/videoconvert/passthrough-toggle", test_passthrough_toggle);
  g_test_add_func ("/gpu/videoconvert/passthrough-reset-on-stop", test_passthrough_reset_on_stop);
  g_test_add_func ("/gpu/videoconvert/crop-meta", test_crop_meta);
  g_test_add_func ("/gpu/videoconvert/caps-query", test_caps_query);
  g_test_add_func ("/gpu/videoconvert/propose-allocation", test_propose_allocation);
  g_test_add_func ("/gpu/videoconvert/decide-allocation", test_decide_allocation);
  return g_test_run ();
}
