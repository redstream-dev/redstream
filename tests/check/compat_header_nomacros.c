/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* RDS_COMPAT_NO_MACROS keeps the GStreamer functions unredirected; the DeepStream aliases stay available. */
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

static void
test_aliases_still_available (void)
{
  NvDsBatchMeta *(*create) (guint) = nvds_create_batch_meta;

  g_assert_true (create == rds_create_batch_meta);
  g_assert_cmpuint (sizeof (NvBufSurface), ==, sizeof (RdsBufSurface));
  g_assert_cmpint (GST_NVEVENT_STREAM_EOS, ==, RDS_EVENT_STREAM_EOS);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/compat-header-nomacros/no-redirection", test_no_redirection);
  g_test_add_func ("/compat-header-nomacros/aliases", test_aliases_still_available);
  return g_test_run ();
}
