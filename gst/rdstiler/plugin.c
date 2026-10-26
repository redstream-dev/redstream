/* Redstream: the rdstiler plugin (rdsmultistreamtiler: memory:RDS batches into one tiled frame).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

#include "gstrdsmultistreamtiler.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (rdsmultistreamtiler, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, rdstiler,
    "Redstream multi-stream tiler", plugin_init,
    PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
