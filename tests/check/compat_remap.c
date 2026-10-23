#include <rds/rds-compat-remap.h>

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
test_rewrite_launch (void)
{
  gchar *out = rds_compat_rewrite_launch ("uridecodebin uri=file:///a.mp4 ! m.sink_0 "
      "nvstreammux name=m batch-size=1 ! nvinfer config-file-path=nvinfer.txt ! fakesink");
  g_assert_cmpstr (out, ==, "uridecodebin uri=file:///a.mp4 ! m.sink_0 "
      "rdsstreammux name=m batch-size=1 ! rdsinfer config-file-path=nvinfer.txt ! fakesink");
  g_free (out);
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

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/compat/map-known", test_map_known);
  g_test_add_func ("/compat/map-passthrough", test_map_passthrough);
  g_test_add_func ("/compat/rewrite-launch", test_rewrite_launch);
  g_test_add_func ("/compat/remap-disabled", test_remap_disabled);
  g_test_add_func ("/compat/factory-make-passthrough", test_factory_make_passthrough);
  return g_test_run ();
}
