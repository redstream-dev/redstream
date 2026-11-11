/* Redstream: the rdsenc plugin (rdsh264enc and rdsh265enc: H.264 and H.265 encoding with the VA encoders, with
 * DeepStream's nvv4l2h264enc / nvv4l2h265enc properties).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

#include "gstrdsh26xenc.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ok = GST_ELEMENT_REGISTER (rdsh264enc, plugin);

  ok &= GST_ELEMENT_REGISTER (rdsh265enc, plugin);
  return ok;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, rdsenc,
    "Redstream H.264 and H.265 encoders", plugin_init,
    PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
