/* Redstream: the rdsmux plugin (batching sources into memory:RDS batches, and splitting them again).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

#include "gstrdsstreamdemux.h"
#include "gstrdsstreammux.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ok = GST_ELEMENT_REGISTER (rdsstreammux, plugin);

  ok &= GST_ELEMENT_REGISTER (rdsstreamdemux, plugin);
  return ok;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, rdsmux,
    "Redstream stream batching", plugin_init,
    PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
