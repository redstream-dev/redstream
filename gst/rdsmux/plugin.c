/* Redstream: the rdsmux plugin (batching sources into memory:RDS batches).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

#include "gstrdsstreammux.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  return GST_ELEMENT_REGISTER (rdsstreammux, plugin);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, rdsmux,
    "Redstream stream batching", plugin_init,
    PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
