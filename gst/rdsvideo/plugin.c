/* Redstream: the rdsvideo plugin (rocDecode video decode and memory:RDS conversion).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

static gboolean
plugin_init (G_GNUC_UNUSED GstPlugin * plugin)
{
  return TRUE;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, rdsvideo,
    "Redstream video decode and conversion", plugin_init,
    PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
