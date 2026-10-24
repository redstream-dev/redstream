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

/* An enum property: type name, default, contiguous integer range lo..hi with a nick for each value. */
static void
check_enum (GstElement * e, const gchar * name, const gchar * type, gint def, gint lo, gint hi)
{
  GParamSpec *p = find_prop (e, name, G_TYPE_PARAM_ENUM);
  GEnumClass *ec = G_PARAM_SPEC_ENUM (p)->enum_class;

  g_assert_cmpstr (g_type_name (p->value_type), ==, type);
  g_assert_cmpint (G_PARAM_SPEC_ENUM (p)->default_value, ==, def);
  g_assert_cmpuint (ec->n_values, ==, (guint) (hi - lo + 1));
  for (gint v = lo; v <= hi; v++) {
    GEnumValue *ev = g_enum_get_value (ec, v);
    g_assert_nonnull (ev);
    g_assert_nonnull (ev->value_nick);
  }
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

  p = find_prop (e, "src-crop", G_TYPE_PARAM_STRING);
  g_assert_cmpstr (G_PARAM_SPEC_STRING (p)->default_value, ==, "");
  p = find_prop (e, "dest-crop", G_TYPE_PARAM_STRING);
  g_assert_cmpstr (G_PARAM_SPEC_STRING (p)->default_value, ==, "");

  check_enum (e, "interpolation-method", "GstRdsInterpolationMethod", 6, 0, 6);
  check_enum (e, "compute-hw", "GstRdsComputeHW", 0, 0, 2);
  check_enum (e, "flip-method", "GstRdsVideoFlipMethod", 0, 0, 7);
  check_enum (e, "copy-hw", "GstRdsCopyHWType", 1, 1, 2);
  gst_util_set_object_arg (G_OBJECT (e), "interpolation-method", "1");
  g_object_get (e, "interpolation-method", &en, NULL);
  g_assert_cmpint (en, ==, 1);

  p = find_prop (e, "allow-odd-crop", G_TYPE_PARAM_BOOLEAN);
  g_assert_true (G_PARAM_SPEC_BOOLEAN (p)->default_value);
  p = find_prop (e, "contiguous-buffers", G_TYPE_PARAM_BOOLEAN);
  g_assert_false (G_PARAM_SPEC_BOOLEAN (p)->default_value);
  p = find_prop (e, "bl-output", G_TYPE_PARAM_BOOLEAN);
  g_assert_false (G_PARAM_SPEC_BOOLEAN (p)->default_value);

  gst_object_unref (e);
}

/* crop strings are validated when set: a bad one is refused with a warning and the previous value stays */
static void
test_crop_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsvideoconvert", NULL);
  gchar *str = NULL;

  g_object_set (e, "src-crop", "10:20:100:50", "dest-crop", "0:0:160:120", NULL);
  g_object_get (e, "src-crop", &str, NULL);
  g_assert_cmpstr (str, ==, "10:20:100:50");
  g_free (str);

  g_test_expect_message ("rdsvideo", G_LOG_LEVEL_WARNING, "*src-crop*1:2:3*");
  g_object_set (e, "src-crop", "1:2:3", NULL);
  g_test_expect_message ("rdsvideo", G_LOG_LEVEL_WARNING, "*dest-crop*-1:0:5:5*");
  g_object_set (e, "dest-crop", "-1:0:5:5", NULL);
  g_test_assert_expected_messages ();
  g_object_get (e, "src-crop", &str, NULL);
  g_assert_cmpstr (str, ==, "10:20:100:50");
  g_free (str);
  g_object_get (e, "dest-crop", &str, NULL);
  g_assert_cmpstr (str, ==, "0:0:160:120");
  g_free (str);

  /* empty (or NULL) clears it */
  g_object_set (e, "src-crop", "", "dest-crop", NULL, NULL);
  g_object_get (e, "src-crop", &str, NULL);
  g_assert_cmpstr (str, ==, "");
  g_free (str);
  g_object_get (e, "dest-crop", &str, NULL);
  g_assert_cmpstr (str, ==, "");
  g_free (str);
  gst_object_unref (e);
}

/* Runs @desc until it posts an error; returns it. With @at_start, going to PAUSED itself must fail. */
static GError *
pipeline_error (const gchar * desc, gboolean at_start)
{
  GError *err = NULL;
  GstElement *pipe = gst_parse_launch (desc, &err);
  GstBus *bus;
  GstMessage *msg;

  g_assert_no_error (err);
  bus = gst_element_get_bus (pipe);
  if (at_start)
    g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_FAILURE);
  else
    gst_element_set_state (pipe, GST_STATE_PLAYING);
  msg = gst_bus_timed_pop_filtered (bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_ERROR);
  gst_message_parse_error (msg, &err, NULL);
  gst_message_unref (msg);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (pipe);
  return err;
}

/* Settings we cannot honour fail at start, before any GPU work (so this runs without a GPU too). */
static void
test_unsupported_settings (void)
{
  static const gchar *props[] = { "flip-method=1", "flip-method=7", "compute-hw=2" };

  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    gchar *desc = g_strdup_printf ("videotestsrc num-buffers=1 ! video/x-raw,format=NV12,width=64,height=48 "
        "! rdsvideoconvert %s ! video/x-raw,format=RGBA ! fakesink", props[i]);
    GError *err = pipeline_error (desc, TRUE);

    g_assert_error (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_SETTINGS);
    g_clear_error (&err);
    g_free (desc);
  }
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

/* ---- gpu: system memory, crops, filters ---- */

#define SYS_NV12_320 "video/x-raw,format=NV12,width=320,height=240,framerate=30/1"
#define SYS_RGBA_320 "video/x-raw,format=RGBA,width=320,height=240,framerate=30/1"
#define SYS_RGBA_160 "video/x-raw,format=RGBA,width=160,height=120,framerate=30/1"

/* A system-memory frame in the default layout for @caps_str, every byte pseudo-random. */
static GstBuffer *
sys_frame (const gchar * caps_str, guint32 seed)
{
  GstCaps *caps = gst_caps_from_string (caps_str);
  GstVideoInfo info;
  GstBuffer *buf;
  GstMapInfo map;

  g_assert_true (gst_video_info_from_caps (&info, caps));
  buf = gst_buffer_new_allocate (NULL, GST_VIDEO_INFO_SIZE (&info), NULL);
  g_assert_true (gst_buffer_map (buf, &map, GST_MAP_WRITE));
  rng_state = seed * 2654435761u + 1;
  for (gsize i = 0; i < map.size; i++)
    map.data[i] = rng_next ();
  gst_buffer_unmap (buf, &map);
  gst_caps_unref (caps);
  return buf;
}

/* CPU copy of a frame: a memory:RDS surface, or a system-memory frame described by @caps (honours GstVideoMeta). */
static void
host_of (GstBuffer * buf, GstCaps * caps, HostImage * h)
{
  GstVideoInfo info;
  GstVideoFrame f;

  if (rds_buffer_get_surface (buf) != NULL) {
    snapshot_buffer (buf, h);
    return;
  }
  memset (h, 0, sizeof (*h));
  g_assert_true (gst_video_info_from_caps (&info, caps));
  g_assert_true (gst_video_frame_map (&f, &info, buf, GST_MAP_READ));
  h->im.format = rds_buf_color_format_from_video_format (GST_VIDEO_FRAME_FORMAT (&f));
  h->im.width = GST_VIDEO_FRAME_WIDTH (&f);
  h->im.height = GST_VIDEO_FRAME_HEIGHT (&f);
  h->num_planes = GST_VIDEO_FRAME_N_PLANES (&f);
  for (guint p = 0; p < h->num_planes; p++) {
    gint comp[GST_VIDEO_MAX_COMPONENTS];

    gst_video_format_info_component (f.info.finfo, p, comp);
    h->im.pitch[p] = GST_VIDEO_FRAME_PLANE_STRIDE (&f, p);
    h->rows[p] = GST_VIDEO_FRAME_COMP_HEIGHT (&f, comp[0]);
    h->row_bytes[p] = GST_VIDEO_FRAME_COMP_WIDTH (&f, comp[0]) * GST_VIDEO_FRAME_COMP_PSTRIDE (&f, comp[0]);
    h->im.plane[p] = g_memdup2 (GST_VIDEO_FRAME_PLANE_DATA (&f, p), (gsize) h->im.pitch[p] * h->rows[p]);
  }
  gst_video_frame_unmap (&f);
}

static void
host_of_str (GstBuffer * buf, const gchar * caps_str, HostImage * h)
{
  GstCaps *caps = gst_caps_from_string (caps_str);

  host_of (buf, caps, h);
  gst_caps_unref (caps);
}

/* The fill rdsvideoconvert uses around dest-crop: Y 16, U/V 128 for YUV; 0 for GRAY8 and RGB (alpha too). */
static guint8
black_byte (RdsBufSurfaceColorFormat f, guint plane)
{
  if (ref_kind (f) == REF_KIND_YUV420)
    return plane == 0 ? 16 : 128;
  return 0;
}

/* An image shaped like @like, every plane filled with black. */
static void
host_black (const HostImage * like, HostImage * out)
{
  *out = *like;
  for (guint p = 0; p < like->num_planes; p++) {
    gsize size = (gsize) like->im.pitch[p] * like->rows[p];

    out->im.plane[p] = g_malloc (size);
    memset (out->im.plane[p], black_byte (like->im.format, p), size);
  }
}

/* Every pixel byte outside @d (even-aligned for 4:2:0) is exactly black. */
static void
assert_black_outside (const HostImage * h, RefRect d)
{
  for (guint p = 0; p < h->num_planes; p++) {
    guint sub = (p > 0 && ref_kind (h->im.format) == REF_KIND_YUV420) ? 2 : 1;
    guint pw = (h->im.width + sub - 1) / sub, bpp = h->row_bytes[p] / pw;

    for (guint y = 0; y < h->rows[p]; y++) {
      for (guint x = 0; x < h->row_bytes[p]; x++) {
        guint px = x / bpp * sub, py = y * sub;
        guint8 v = h->im.plane[p][(gsize) y * h->im.pitch[p] + x];

        if (px >= d.left && px < d.left + d.width && py >= d.top && py < d.top + d.height)
          continue;
        if (v != black_byte (h->im.format, p))
          g_error ("plane %u row %u byte %u outside the rect: %u", p, y, x, v);
      }
    }
  }
}

/* The output must match the reference transform of rect @s of the input into rect @d of a black frame. */
static void
assert_matches_ref (const HostImage * hin, const HostImage * hout, RefRect s, RefRect d, gboolean bilinear)
{
  HostImage href;

  host_black (hout, &href);
  ref_transform (&hin->im, &href.im, s, d, bilinear);
  g_assert_cmpuint (max_diff (hout, &href, TRUE), <=, 1);
  host_image_clear (&href);
}

/* A harness around "rdsvideoconvert @props" with fixed caps on both sides; @bus (optional) gets its messages. */
static GstHarness *
make_harness_props (const gchar * props, const gchar * in_caps, const gchar * out_caps, GstBus * bus)
{
  gchar *desc = g_strdup_printf ("rdsvideoconvert %s", props);
  GError *err = NULL;
  GstElement *e = gst_parse_launch (desc, &err);
  GstHarness *h;

  g_assert_no_error (err);
  gst_object_ref_sink (e);
  if (bus != NULL)
    gst_element_set_bus (e, bus);
  h = gst_harness_new_with_element (e, "sink", "src");
  gst_harness_set_src_caps_str (h, in_caps);
  gst_harness_set_sink_caps_str (h, out_caps);
  gst_object_unref (e);
  g_free (desc);
  return h;
}

/* Pushes a random system frame of @in_caps through "rdsvideoconvert @props" to @out_caps and compares the
 * output with the reference for rects @s -> @d. */
static void
check_sys_conversion (const gchar * props, const gchar * in_caps, const gchar * out_caps, RefRect s, RefRect d,
    gboolean bilinear, guint32 seed)
{
  GstHarness *h = make_harness_props (props, in_caps, out_caps, NULL);
  GstBuffer *in = sys_frame (in_caps, seed), *out;
  HostImage hin, hout;
  GstCaps *caps;

  host_of_str (in, in_caps, &hin);
  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  caps = gst_pad_get_current_caps (h->sinkpad);
  host_of (out, caps, &hout);
  gst_caps_unref (caps);
  assert_matches_ref (&hin, &hout, s, d, bilinear);
  if (d.width != hout.im.width || d.height != hout.im.height)
    assert_black_outside (&hout, d);
  host_image_clear (&hout);
  host_image_clear (&hin);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
}

/* videotestsrc in system memory, converted, against the reference applied to the same frames (via tee). */
static void
test_sys_pipeline (void)
{
  GError *err = NULL;
  GstElement *pipe, *ref, *out;
  RefRect r = { 0, 0, 320, 240 };

  RDS_TEST_REQUIRE_GPU ();
  pipe = gst_parse_launch ("videotestsrc pattern=smpte num-buffers=3 "
      "! video/x-raw,format=NV12,width=320,height=240 ! tee name=t "
      "t. ! queue ! appsink name=ref sync=false "
      "t. ! queue ! rdsvideoconvert ! video/x-raw,format=RGBA,width=320,height=240 ! appsink name=out sync=false",
      &err);
  g_assert_no_error (err);
  ref = gst_bin_get_by_name (GST_BIN (pipe), "ref");
  out = gst_bin_get_by_name (GST_BIN (pipe), "out");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  for (guint i = 0; i < 3; i++) {
    GstSample *sin = NULL, *sout = NULL;
    HostImage hin, hout;

    g_signal_emit_by_name (ref, "try-pull-sample", (GstClockTime) (10 * GST_SECOND), &sin);
    g_signal_emit_by_name (out, "try-pull-sample", (GstClockTime) (10 * GST_SECOND), &sout);
    g_assert_nonnull (sin);
    g_assert_nonnull (sout);
    g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (gst_sample_get_caps (sout), 0),
            GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
    host_of (gst_sample_get_buffer (sin), gst_sample_get_caps (sin), &hin);
    host_of (gst_sample_get_buffer (sout), gst_sample_get_caps (sout), &hout);
    g_assert_cmpint (hout.im.format, ==, RDS_BUF_COLOR_FORMAT_RGBA);
    assert_matches_ref (&hin, &hout, r, r, FALSE);
    host_image_clear (&hout);
    host_image_clear (&hin);
    gst_sample_unref (sout);
    gst_sample_unref (sin);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (out);
  gst_object_unref (ref);
  gst_object_unref (pipe);
}

/* system -> memory:RDS -> system through two converters (NV12 -> RGBA -> NV12): luma survives within +-2 */
static void
test_sys_round_trip (void)
{
  GError *err = NULL;
  GstElement *pipe, *ref, *out;

  RDS_TEST_REQUIRE_GPU ();
  pipe = gst_parse_launch ("videotestsrc pattern=smpte num-buffers=2 "
      "! video/x-raw,format=NV12,width=320,height=240 ! tee name=t "
      "t. ! queue ! appsink name=ref sync=false "
      "t. ! queue ! rdsvideoconvert ! video/x-raw(memory:RDS),format=RGBA ! rdsvideoconvert "
      "! video/x-raw,format=NV12 ! appsink name=out sync=false", &err);
  g_assert_no_error (err);
  ref = gst_bin_get_by_name (GST_BIN (pipe), "ref");
  out = gst_bin_get_by_name (GST_BIN (pipe), "out");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  for (guint i = 0; i < 2; i++) {
    GstSample *sin = NULL, *sout = NULL;
    HostImage hin, hout;

    g_signal_emit_by_name (ref, "try-pull-sample", (GstClockTime) (10 * GST_SECOND), &sin);
    g_signal_emit_by_name (out, "try-pull-sample", (GstClockTime) (10 * GST_SECOND), &sout);
    g_assert_nonnull (sin);
    g_assert_nonnull (sout);
    host_of (gst_sample_get_buffer (sin), gst_sample_get_caps (sin), &hin);
    host_of (gst_sample_get_buffer (sout), gst_sample_get_caps (sout), &hout);
    g_assert_cmpint (hout.im.format, ==, RDS_BUF_COLOR_FORMAT_NV12);
    g_assert_cmpuint (hout.im.width, ==, 320);
    /* Luma within +-2 wherever chroma is flat around the pixel. At a colour edge, nearest chroma sampling pairs
     * a luma value with its neighbour's chroma; the RGB result can leave the gamut and clip, which no round trip
     * survives, so colours outside the RGB gamut (parts of the smpte pattern's bottom rows) are skipped too. */
    {
      guint checked = 0;

      for (guint y = 0; y < 240; y++) {
        for (guint x = 0; x < 320; x++) {
          gint cx = x / 2, cy = y / 2, a = hin.im.plane[0][y * hin.im.pitch[0] + x];
          gint b = hout.im.plane[0][y * hout.im.pitch[0] + x];
          const guint8 *uv = hin.im.plane[1];
          gboolean flat = TRUE;

          for (gint dy = -1; dy <= 1; dy++)
            for (gint dx = -1; dx <= 1; dx++) {
              gint nx = CLAMP (cx + dx, 0, 159), ny = CLAMP (cy + dy, 0, 119);
              flat &= memcmp (uv + cy * hin.im.pitch[1] + 2 * cx, uv + ny * hin.im.pitch[1] + 2 * nx, 2) == 0;
            }
          if (!flat)
            continue;
          {
            double rgb[3];

            ref_yuv_to_rgb (a, uv[cy * hin.im.pitch[1] + 2 * cx], uv[cy * hin.im.pitch[1] + 2 * cx + 1], rgb);
            if (MIN (rgb[0], MIN (rgb[1], rgb[2])) < -2.0 || MAX (rgb[0], MAX (rgb[1], rgb[2])) > 257.0)
              continue;
          }
          if (ABS (a - b) > 2)
            g_error ("luma at %u,%u: %d in, %d out", x, y, a, b);
          checked++;
        }
      }
      g_assert_cmpuint (checked, >, 320 * 240 / 2);
    }
    host_image_clear (&hout);
    host_image_clear (&hin);
    gst_sample_unref (sout);
    gst_sample_unref (sin);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (out);
  gst_object_unref (ref);
  gst_object_unref (pipe);
}

/* Each single mixed path: system -> memory:RDS and memory:RDS -> system, against the reference. */
static void
test_mixed_memory (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL, *out;
  HostImage hin, hout;
  RefRect s = { 0, 0, 320, 240 }, d = { 0, 0, 160, 120 };

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness_props ("", SYS_NV12_320, RGBA_160, NULL);
  in = sys_frame (SYS_NV12_320, 81);
  host_of_str (in, SYS_NV12_320, &hin);
  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  g_assert_nonnull (rds_buffer_get_surface (out));
  host_of (out, NULL, &hout);
  assert_matches_ref (&hin, &hout, s, d, FALSE);
  host_image_clear (&hout);
  host_image_clear (&hin);
  gst_buffer_unref (out);
  gst_harness_teardown (h);

  h = make_harness_props ("", NV12_320, "video/x-raw,format=I420,width=160,height=120,framerate=30/1", NULL);
  pool = make_pool (NV12_320, 1);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
  fill_buffer (in, 82);
  snapshot_buffer (in, &hin);
  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  g_assert_null (rds_buffer_get_surface (out));
  host_of_str (out, "video/x-raw,format=I420,width=160,height=120", &hout);
  assert_matches_ref (&hin, &hout, s, d, FALSE);
  host_image_clear (&hout);
  host_image_clear (&hin);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

/* A system frame whose planes are not in ascending order (UV before Y, via GstVideoMeta) still converts. */
static void
test_sys_foreign_layout (void)
{
  GstHarness *h;
  GstBuffer *in, *out;
  GstVideoInfo info;
  gsize offset[GST_VIDEO_MAX_PLANES] = { 0 };
  gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
  GstCaps *caps = gst_caps_from_string (SYS_NV12_320);
  HostImage hin, hout;
  RefRect s = { 0, 0, 320, 240 }, d = { 0, 0, 160, 120 };

  RDS_TEST_REQUIRE_GPU ();
  g_assert_true (gst_video_info_from_caps (&info, caps));
  stride[0] = 384;
  stride[1] = 352;
  offset[1] = 0;
  offset[0] = (gsize) stride[1] * 120 + 64;
  in = sys_frame ("video/x-raw,format=GRAY8,width=384,height=400", 83);  /* enough random bytes */
  g_assert_cmpuint (gst_buffer_get_size (in), >=, offset[0] + (gsize) stride[0] * 240);
  gst_buffer_add_video_meta_full (in, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_NV12, 320, 240, 2, offset,
      stride);
  host_of (in, caps, &hin);

  h = make_harness_props ("", SYS_NV12_320, SYS_RGBA_160, NULL);
  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  host_of_str (out, SYS_RGBA_160, &hout);
  assert_matches_ref (&hin, &hout, s, d, FALSE);
  host_image_clear (&hout);
  host_image_clear (&hin);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  gst_caps_unref (caps);
}

/* New input geometry mid-stream: the staging surfaces follow. */
static void
test_sys_caps_change (void)
{
  GstHarness *h;
  static const gchar *ins[] = { SYS_NV12_320, "video/x-raw,format=I420,width=176,height=144,framerate=30/1" };

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness_props ("", SYS_NV12_320, SYS_RGBA_160, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (ins); i++) {
    GstBuffer *in, *out;
    HostImage hin, hout;
    RefRect d = { 0, 0, 160, 120 };
    RefRect s = { 0, 0, i == 0 ? 320 : 176, i == 0 ? 240 : 144 };

    gst_harness_set_src_caps_str (h, ins[i]);
    in = sys_frame (ins[i], 90 + i);
    host_of_str (in, ins[i], &hin);
    out = gst_harness_push_and_pull (h, in);
    g_assert_nonnull (out);
    host_of_str (out, SYS_RGBA_160, &hout);
    assert_matches_ref (&hin, &hout, s, d, FALSE);
    host_image_clear (&hout);
    host_image_clear (&hin);
    gst_buffer_unref (out);
  }
  gst_harness_teardown (h);
}

static void
test_src_crop (void)
{
  RefRect full = { 0, 0, 160, 120 };

  RDS_TEST_REQUIRE_GPU ();
  check_sys_conversion ("src-crop=10:20:100:50", SYS_NV12_320, SYS_RGBA_160, (RefRect) {
      10, 20, 100, 50}, full, FALSE, 101);
  /* clamped to the frame: 300:200:100:100 on 320x240 is 300:200:20:40 */
  check_sys_conversion ("src-crop=300:200:100:100", SYS_NV12_320, SYS_RGBA_160, (RefRect) {
      300, 200, 20, 40}, full, FALSE, 102);
  /* odd source rects are fine on 4:2:0 */
  check_sys_conversion ("src-crop=11:21:101:51 allow-odd-crop=false", SYS_NV12_320,
      "video/x-raw,format=NV12,width=160,height=120,framerate=30/1", (RefRect) {
      11, 21, 101, 51}, full, FALSE, 103);
}

static void
test_dest_crop (void)
{
  RefRect s = { 0, 0, 320, 240 };

  RDS_TEST_REQUIRE_GPU ();
  /* RGBA: black outside, alpha 0 */
  check_sys_conversion ("dest-crop=0:0:160:120", SYS_NV12_320, SYS_RGBA_320, s, (RefRect) {
      0, 0, 160, 120}, FALSE, 111);
  /* same caps both sides: the crop disables passthrough; Y 16 / UV 128 outside */
  check_sys_conversion ("dest-crop=40:30:160:120", SYS_NV12_320, SYS_NV12_320, s, (RefRect) {
      40, 30, 160, 120}, FALSE, 112);
  /* clamped: 200:100:500:500 on 320x240 is 200:100:120:140 */
  check_sys_conversion ("dest-crop=200:100:500:500", SYS_NV12_320, "video/x-raw,format=I420,width=320,"
      "height=240,framerate=30/1", s, (RefRect) {
      200, 100, 120, 140}, FALSE, 113);
  /* both crops at once */
  check_sys_conversion ("src-crop=10:20:100:50 dest-crop=8:6:64:48", SYS_NV12_320, SYS_RGBA_160, (RefRect) {
      10, 20, 100, 50}, (RefRect) {
      8, 6, 64, 48}, FALSE, 114);
}

/* memory:RDS output with dest-crop: the fill happens on the device surface */
static void
test_dest_crop_rds (void)
{
  GstHarness *h;
  GstBufferPool *pool;
  GstBuffer *in = NULL, *out;
  HostImage hin, hout;
  RefRect s = { 0, 0, 320, 240 }, d = { 10, 10, 100, 100 };

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness_props ("dest-crop=10:10:100:100", NV12_320,
      "video/x-raw(memory:RDS),format=RGBA,width=320,height=240,framerate=30/1", NULL);
  pool = make_pool (NV12_320, 1);
  for (guint i = 0; i < 2; i++) {       /* the second output buffer is a recycled one */
    g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &in, NULL), ==, GST_FLOW_OK);
    fill_buffer (in, 121 + i);
    snapshot_buffer (in, &hin);
    out = gst_harness_push_and_pull (h, in);
    g_assert_nonnull (out);
    snapshot_buffer (out, &hout);
    assert_matches_ref (&hin, &hout, s, d, FALSE);
    assert_black_outside (&hout, d);
    host_image_clear (&hout);
    host_image_clear (&hin);
    gst_buffer_unref (out);
  }
  gst_harness_teardown (h);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

/* src-crop is relative to the input's crop meta (offset by it, clamped to its size) */
static void
test_src_crop_with_meta (void)
{
  static const struct
  {
    const gchar *props;
    RefRect expect;
  } cases[] = {
    { "src-crop=5:10:300:30", { 15, 30, 195, 30 } },    /* width clamped to the meta's 200 */
    { "src-crop=190:90:50:50", { 200, 110, 10, 10 } },  /* both clamped */
    { "", { 10, 20, 200, 100 } },                        /* the meta alone */
  };

  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    GstHarness *h = make_harness_props (cases[i].props, SYS_NV12_320, SYS_RGBA_160, NULL);
    GstBuffer *in = sys_frame (SYS_NV12_320, 130 + i), *out;
    GstVideoCropMeta *crop = gst_buffer_add_video_crop_meta (in);
    HostImage hin, hout;

    crop->x = 10;
    crop->y = 20;
    crop->width = 200;
    crop->height = 100;
    host_of_str (in, SYS_NV12_320, &hin);
    out = gst_harness_push_and_pull (h, in);
    g_assert_nonnull (out);
    host_of_str (out, SYS_RGBA_160, &hout);
    assert_matches_ref (&hin, &hout, cases[i].expect, (RefRect) {
        0, 0, 160, 120}, FALSE);
    host_image_clear (&hout);
    host_image_clear (&hin);
    gst_buffer_unref (out);
    gst_harness_teardown (h);
  }
}

/* Pushes one frame through a converter expected to fail; returns the error. */
static GError *
push_error (const gchar * props, const gchar * in_caps, const gchar * out_caps)
{
  GstBus *bus = gst_bus_new ();
  GstHarness *h = make_harness_props (props, in_caps, out_caps, bus);
  GstMessage *msg;
  GError *err = NULL;

  g_assert_cmpint (gst_harness_push (h, sys_frame (in_caps, 140)), ==, GST_FLOW_ERROR);
  msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  gst_message_parse_error (msg, &err, NULL);
  gst_message_unref (msg);
  gst_element_set_bus (h->element, NULL);
  gst_harness_teardown (h);
  gst_object_unref (bus);
  return err;
}

static void
test_crop_empty_after_clamp (void)
{
  GError *err;

  RDS_TEST_REQUIRE_GPU ();
  /* reported by the element as an empty crop, before the transform would reject it */
  err = push_error ("src-crop=400:0:10:10", SYS_NV12_320, SYS_RGBA_160);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED);
  g_assert_nonnull (strstr (err->message, "empty"));
  g_clear_error (&err);
  err = push_error ("dest-crop=0:240:10:10", SYS_NV12_320, SYS_RGBA_320);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED);
  g_assert_nonnull (strstr (err->message, "empty"));
  g_clear_error (&err);
}

static void
test_odd_dest_crop (void)
{
  GError *err;
  RefRect s = { 0, 0, 320, 240 };

  RDS_TEST_REQUIRE_GPU ();
  err = push_error ("dest-crop=1:1:101:51 allow-odd-crop=false", SYS_NV12_320, SYS_NV12_320);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_FAILED);
  g_clear_error (&err);
  /* allowed: aligned outward to 0:0:102:52 */
  check_sys_conversion ("dest-crop=1:1:101:51", SYS_NV12_320, SYS_NV12_320, s, (RefRect) {
      0, 0, 102, 52}, FALSE, 150);
  /* RGB output needs no alignment */
  check_sys_conversion ("dest-crop=1:1:101:51 allow-odd-crop=false", SYS_NV12_320, SYS_RGBA_320, s, (RefRect) {
      1, 1, 101, 51}, FALSE, 151);
}

static void
test_interpolation (void)
{
  static const struct
  {
    const gchar *props;
    gboolean bilinear;
  } cases[] = {
    { "interpolation-method=0", FALSE }, { "interpolation-method=1", TRUE }, { "interpolation-method=2", TRUE },
    { "interpolation-method=5", FALSE }, { "", FALSE },
  };
  RefRect s = { 0, 0, 320, 240 }, d = { 0, 0, 200, 150 };

  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    check_sys_conversion (cases[i].props, SYS_NV12_320, "video/x-raw,format=RGBA,width=200,height=150,"
        "framerate=30/1", s, d, cases[i].bilinear, 160 + i);
}

/* Crops turn passthrough off and on again at run time. */
static void
test_crop_passthrough (void)
{
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  h = make_harness_props ("", SYS_NV12_320, SYS_NV12_320, NULL);
  for (guint step = 0; step < 3; step++) {
    GstBuffer *in = sys_frame (SYS_NV12_320, 170 + step), *out;
    gboolean cropped = step == 1;

    if (step > 0)
      g_object_set (h->element, "src-crop", cropped ? "0:0:160:120" : "", NULL);
    out = gst_harness_push_and_pull (h, gst_buffer_ref (in));
    g_assert_nonnull (out);
    g_assert_cmpint (gst_base_transform_is_passthrough (GST_BASE_TRANSFORM (h->element)), ==, !cropped);
    g_assert_cmpint (out == in, ==, !cropped);
    gst_buffer_unref (out);
    gst_buffer_unref (in);
  }
  gst_harness_teardown (h);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/videoconvert/factory", test_factory);
  g_test_add_func ("/cpu/videoconvert/properties", test_properties);
  g_test_add_func ("/cpu/videoconvert/crop-properties", test_crop_properties);
  g_test_add_func ("/cpu/videoconvert/unsupported-settings", test_unsupported_settings);
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
  g_test_add_func ("/gpu/videoconvert/sys-pipeline", test_sys_pipeline);
  g_test_add_func ("/gpu/videoconvert/sys-round-trip", test_sys_round_trip);
  g_test_add_func ("/gpu/videoconvert/mixed-memory", test_mixed_memory);
  g_test_add_func ("/gpu/videoconvert/sys-foreign-layout", test_sys_foreign_layout);
  g_test_add_func ("/gpu/videoconvert/sys-caps-change", test_sys_caps_change);
  g_test_add_func ("/gpu/videoconvert/src-crop", test_src_crop);
  g_test_add_func ("/gpu/videoconvert/dest-crop", test_dest_crop);
  g_test_add_func ("/gpu/videoconvert/dest-crop-rds", test_dest_crop_rds);
  g_test_add_func ("/gpu/videoconvert/src-crop-with-meta", test_src_crop_with_meta);
  g_test_add_func ("/gpu/videoconvert/crop-empty-after-clamp", test_crop_empty_after_clamp);
  g_test_add_func ("/gpu/videoconvert/odd-dest-crop", test_odd_dest_crop);
  g_test_add_func ("/gpu/videoconvert/interpolation", test_interpolation);
  g_test_add_func ("/gpu/videoconvert/crop-passthrough", test_crop_passthrough);
  return g_test_run ();
}
