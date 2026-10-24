/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds-compat-remap.h>

#include <string.h>

static void
test_map_known (void)
{
  g_assert_cmpstr (rds_compat_map_factory_name ("nvinfer"), ==, "rdsinfer");
  g_assert_cmpstr (rds_compat_map_factory_name ("nvstreammux"), ==, "rdsstreammux");
  g_assert_cmpstr (rds_compat_map_factory_name ("nveglglessink"), ==, "rdsvideosink");
  g_assert_cmpstr (rds_compat_map_factory_name ("nv3dsink"), ==, "rdsvideosink");
}

static void
test_map_passthrough (void)
{
  g_assert_cmpstr (rds_compat_map_factory_name ("queue"), ==, "queue");
  g_assert_cmpstr (rds_compat_map_factory_name ("nvarguscamerasrc"), ==, "nvarguscamerasrc");
  g_assert_null (rds_compat_map_factory_name (NULL));
}

static void
assert_rewrite (const gchar * in, const gchar * expected)
{
  gchar *out = rds_compat_rewrite_launch (in);

  g_assert_cmpstr (out, ==, expected);
  g_free (out);
}

static void
test_rewrite_launch (void)
{
  assert_rewrite ("uridecodebin uri=file:///a.mp4 ! m.sink_0 "
      "nvstreammux name=m batch-size=1 ! nvinfer config-file-path=nvinfer.txt ! fakesink",
      "uridecodebin uri=file:///a.mp4 ! m.sink_0 "
      "rdsstreammux name=m batch-size=1 ! rdsinfer config-file-path=nvinfer.txt ! fakesink");
}

static void
test_rewrite_launch_edges (void)
{
  assert_rewrite ("nvinfer ! fakesink", "rdsinfer ! fakesink");
  assert_rewrite ("fakesrc ! nvdsosd", "fakesrc ! rdsosd");
  assert_rewrite ("nvinfer", "rdsinfer");
  assert_rewrite ("nvinfer!fakesink", "rdsinfer!fakesink");
  assert_rewrite ("fakesink name=nvinfer0", "fakesink name=nvinfer0");
  assert_rewrite ("nvmsgconv ! fakesink", "nvmsgconv ! fakesink");
  g_assert_null (rds_compat_rewrite_launch (NULL));
}

static void
test_remap_disabled (void)
{
  g_setenv ("RDS_COMPAT_NO_REMAP", "1", TRUE);
  g_assert_cmpstr (rds_compat_map_factory_name ("nvinfer"), ==, "nvinfer");
  gchar *out = rds_compat_rewrite_launch ("nvstreammux ! fakesink");
  g_assert_cmpstr (out, ==, "nvstreammux ! fakesink");
  g_free (out);
  g_unsetenv ("RDS_COMPAT_NO_REMAP");
}

static void
test_factory_make_passthrough (void)
{
  GstElement *e = rds_compat_element_factory_make ("queue", "q");
  g_assert_nonnull (e);
  gst_object_unref (e);
}

static void
test_factory_make_remaps (void)
{
  GstElement *e = rds_compat_element_factory_make ("nvinfer", NULL);

  g_assert_nonnull (e);
  g_assert_cmpstr (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (gst_element_get_factory (e))),
      ==, "rdsinfer");
  gst_object_unref (e);
}

static void
test_parse_launch_remaps (void)
{
  GError *err = NULL;
  /* The rdsinfer stand-in is a pad-less GstBin, so it is left unlinked here. */
  GstElement *pipeline = rds_compat_parse_launch ("nvinfer name=i fakesink", &err);
  GstElement *infer;

  g_assert_no_error (err);
  g_assert_nonnull (pipeline);
  infer = gst_bin_get_by_name (GST_BIN (pipeline), "i");
  g_assert_nonnull (infer);
  g_assert_cmpstr (gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (gst_element_get_factory (infer))),
      ==, "rdsinfer");
  gst_object_unref (infer);
  gst_object_unref (pipeline);
}

static void
test_parse_launch_error_names_rds (void)
{
  GError *err = NULL;
  GstElement *pipeline = rds_compat_parse_launch ("nvtracker ! fakesink", &err);

  /* gst_parse_launch may return a partial pipeline even when it sets an error. */
  if (pipeline != NULL)
    gst_object_unref (pipeline);
  g_assert_true (g_error_matches (err, GST_PARSE_ERROR, GST_PARSE_ERROR_NO_SUCH_ELEMENT));
  g_assert_nonnull (strstr (err->message, "rdstracker"));
  g_error_free (err);
}

int
main (int argc, char **argv)
{
  g_unsetenv ("RDS_COMPAT_NO_REMAP");
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  /* Stand-in for the real rdsinfer element so the remap path can be exercised. */
  g_assert_true (gst_element_register (NULL, "rdsinfer", GST_RANK_NONE, GST_TYPE_BIN));

  g_test_add_func ("/compat/map-known", test_map_known);
  g_test_add_func ("/compat/map-passthrough", test_map_passthrough);
  g_test_add_func ("/compat/rewrite-launch", test_rewrite_launch);
  g_test_add_func ("/compat/rewrite-launch-edges", test_rewrite_launch_edges);
  g_test_add_func ("/compat/remap-disabled", test_remap_disabled);
  g_test_add_func ("/compat/factory-make-passthrough", test_factory_make_passthrough);
  g_test_add_func ("/compat/factory-make-remaps", test_factory_make_remaps);
  g_test_add_func ("/compat/parse-launch-remaps", test_parse_launch_remaps);
  g_test_add_func ("/compat/parse-launch-error-names-rds", test_parse_launch_error_names_rds);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_deinit ();
    return ret;
  }
}
