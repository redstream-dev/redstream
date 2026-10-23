/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Compiles like an app: includes the compat header and calls the *unprefixed* GStreamer functions. */
#include <rds/rds_compat.h>

static const gchar *
factory_name_of (GstElement * e)
{
  return gst_plugin_feature_get_name (GST_PLUGIN_FEATURE (gst_element_get_factory (e)));
}

static void
test_macro_remaps_factory_make (void)
{
  /* "nvarguscamerasrc" is not aliased: exercises the pass-through path of the redirected call. */
  g_assert_null (gst_element_factory_make ("nvarguscamerasrc", NULL));
  GstElement *q = gst_element_factory_make ("queue", NULL);
  g_assert_nonnull (q);
  gst_object_unref (q);
}

static void
test_macro_remaps_aliased_name (void)
{
  GstElement *e = gst_element_factory_make ("nvinfer", NULL);

  g_assert_nonnull (e);
  g_assert_cmpstr (factory_name_of (e), ==, "rdsinfer");
  gst_object_unref (e);
}

static void
test_macro_remaps_parse_launch (void)
{
  GError *err = NULL;
  /* The rdsinfer stand-in is a pad-less GstBin, so it is left unlinked here. */
  GstElement *pipeline = gst_parse_launch ("nvinfer name=i fakesink", &err);
  GstElement *infer;

  g_assert_no_error (err);
  g_assert_nonnull (pipeline);
  infer = gst_bin_get_by_name (GST_BIN (pipeline), "i");
  g_assert_nonnull (infer);
  g_assert_cmpstr (factory_name_of (infer), ==, "rdsinfer");
  gst_object_unref (infer);
  gst_object_unref (pipeline);
}

static void
test_function_pointer_still_works (void)
{
  GstElement *(*make) (const gchar *, const gchar *) = gst_element_factory_make;
  GstElement *(*launch) (const gchar *, GError **) = gst_parse_launch;

  g_assert_true (make == rds_compat_element_factory_make);
  g_assert_true (launch == rds_compat_parse_launch);
}

int
main (int argc, char **argv)
{
  g_unsetenv ("RDS_COMPAT_NO_REMAP");
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);

  /* Stand-in for the real rdsinfer element so the remap path can be exercised.
   * gst_element_register is not redirected by the compat header. */
  g_assert_true (gst_element_register (NULL, "rdsinfer", GST_RANK_NONE, GST_TYPE_BIN));

  g_test_add_func ("/compat-header/factory-make", test_macro_remaps_factory_make);
  g_test_add_func ("/compat-header/factory-make-aliased", test_macro_remaps_aliased_name);
  g_test_add_func ("/compat-header/parse-launch-aliased", test_macro_remaps_parse_launch);
  g_test_add_func ("/compat-header/function-pointer", test_function_pointer_still_works);
  return g_test_run ();
}
