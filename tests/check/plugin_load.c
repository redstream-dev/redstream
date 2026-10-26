/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

static void
test_plugin_loads (void)
{
  GstPlugin *plugin = gst_registry_find_plugin (gst_registry_get (), "rdsvideo");

  g_assert_nonnull (plugin);
  g_assert_cmpstr (gst_plugin_get_license (plugin), ==, "LGPL");
  gst_object_unref (plugin);
}

/* RDS_HAVE_INFER comes from the build: rdsinfer is optional (MIGraphX, libyaml, json-glib). */
#if RDS_HAVE_INFER
static void
test_infer_plugin (void)
{
  GstPlugin *plugin = gst_registry_find_plugin (gst_registry_get (), "rdsinfer");

  g_assert_nonnull (plugin);
  g_assert_cmpstr (gst_plugin_get_license (plugin), ==, "LGPL");
  gst_object_unref (plugin);
}
#endif

/* RDS_HAVE_TRACKER comes from the build: rdstracker is optional (libyaml). */
#if RDS_HAVE_TRACKER
static void
test_tracker_plugin (void)
{
  GstPlugin *plugin = gst_registry_find_plugin (gst_registry_get (), "rdstracker");

  g_assert_nonnull (plugin);
  g_assert_cmpstr (gst_plugin_get_license (plugin), ==, "LGPL");
  gst_object_unref (plugin);
}
#endif

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/plugin/loads", test_plugin_loads);
#if RDS_HAVE_INFER
  g_test_add_func ("/plugin/infer", test_infer_plugin);
#endif
#if RDS_HAVE_TRACKER
  g_test_add_func ("/plugin/tracker", test_tracker_plugin);
#endif
  return g_test_run ();
}
