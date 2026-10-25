/* Redstream: the rdsinfer plugin (MIGraphX inference on memory:RDS batches).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

#include "gstrdsinfer.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (rdsinfer, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, rdsinfer,
    "Redstream inference", plugin_init,
    PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
