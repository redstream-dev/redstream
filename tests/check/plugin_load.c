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
static void
test_infer_plugin (void)
{
  GstPlugin *plugin = gst_registry_find_plugin (gst_registry_get (), "rdsinfer");

#if RDS_HAVE_INFER
  g_assert_nonnull (plugin);
  g_assert_cmpstr (gst_plugin_get_license (plugin), ==, "LGPL");
  gst_object_unref (plugin);
#else
  g_assert_null (plugin);
#endif
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/plugin/loads", test_plugin_loads);
  g_test_add_func ("/plugin/infer", test_infer_plugin);
  return g_test_run ();
}
