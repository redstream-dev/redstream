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

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/plugin/loads", test_plugin_loads);
  return g_test_run ();
}
