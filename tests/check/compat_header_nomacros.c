/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* RDS_COMPAT_NO_MACROS keeps the GStreamer functions unredirected. */
#define RDS_COMPAT_NO_MACROS
#include <rds/rds_compat.h>

static void
test_no_redirection (void)
{
  GstElement *(*make) (const gchar *, const gchar *) = gst_element_factory_make;
  GstElement *(*launch) (const gchar *, GError **) = gst_parse_launch;

  g_assert_true (make != rds_compat_element_factory_make);
  g_assert_true (launch != rds_compat_parse_launch);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/compat-header-nomacros/no-redirection", test_no_redirection);
  return g_test_run ();
}
