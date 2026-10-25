/* rdsinfer element basics: registration, properties, pad templates, passthrough.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <gst/check/gstharness.h>

#define RDS_CAPS "video/x-raw(memory:RDS), format=NV12, width=64, height=48, framerate=30/1"

static void
test_factory (void)
{
  GstElementFactory *factory = gst_element_factory_find ("rdsinfer");

  g_assert_nonnull (factory);
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS), ==,
      "Filter/Analyzer/Video");
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), ==, GST_RANK_NONE);
  g_assert_cmpuint (gst_element_factory_get_num_pad_templates (factory), ==, 2);
  gst_object_unref (factory);
}

static void
check_template_caps (GstElement * e, const gchar * name)
{
  GstPadTemplate *templ = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (e), name);
  GstCaps *caps, *want, *sysmem;
  const gchar *formats[] = { "NV12", "RGBA", "BGRA", "RGBx", "BGRx" };

  g_assert_nonnull (templ);
  g_assert_cmpint (GST_PAD_TEMPLATE_PRESENCE (templ), ==, GST_PAD_ALWAYS);
  caps = gst_pad_template_get_caps (templ);
  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    want = gst_caps_from_string (RDS_CAPS);
    gst_caps_set_simple (want, "format", G_TYPE_STRING, formats[i], NULL);
    g_assert_true (gst_caps_can_intersect (caps, want));
    gst_caps_unref (want);
  }
  /* memory:RDS only: no system memory, no other formats */
  sysmem = gst_caps_from_string ("video/x-raw, format=NV12, width=64, height=48");
  g_assert_false (gst_caps_can_intersect (caps, sysmem));
  gst_caps_unref (sysmem);
  want = gst_caps_from_string ("video/x-raw(memory:RDS), format=I420, width=64, height=48");
  g_assert_false (gst_caps_can_intersect (caps, want));
  gst_caps_unref (want);
  gst_caps_unref (caps);
}

static void
test_pad_templates (void)
{
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);

  g_assert_nonnull (e);
  check_template_caps (e, "sink");
  check_template_caps (e, "src");
  gst_object_unref (e);
}

static void
test_config_file_path (void)
{
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);
  GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "config-file-path");
  gchar *path = NULL;

  g_assert_nonnull (pspec);
  g_assert_true (G_PARAM_SPEC_VALUE_TYPE (pspec) == G_TYPE_STRING);
  g_assert_true (pspec->flags & G_PARAM_READWRITE);
  g_object_get (e, "config-file-path", &path, NULL);
  g_assert_null (path);
  g_object_set (e, "config-file-path", "/tmp/pgie.yml", NULL);
  g_object_get (e, "config-file-path", &path, NULL);
  g_assert_cmpstr (path, ==, "/tmp/pgie.yml");
  g_free (path);
  g_object_set (e, "config-file-path", NULL, NULL);
  g_object_get (e, "config-file-path", &path, NULL);
  g_assert_null (path);
  gst_object_unref (e);
}

/* The skeleton forwards every buffer untouched (in place, no copy). */
static void
test_passthrough (void)
{
  GstHarness *h = gst_harness_new_parse ("rdsinfer");
  GstBuffer *in, *out;

  gst_harness_set_caps_str (h, RDS_CAPS, RDS_CAPS);
  in = gst_buffer_new_allocate (NULL, 64 * 48 * 3 / 2, NULL);
  GST_BUFFER_PTS (in) = 42 * GST_MSECOND;
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  g_assert_true (out == in);
  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, 42 * GST_MSECOND);
  gst_buffer_unref (out);
  gst_buffer_unref (in);
  gst_harness_teardown (h);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/infer/factory", test_factory);
  g_test_add_func ("/cpu/infer/pad-templates", test_pad_templates);
  g_test_add_func ("/cpu/infer/config-file-path", test_config_file_path);
  g_test_add_func ("/cpu/infer/passthrough", test_passthrough);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
