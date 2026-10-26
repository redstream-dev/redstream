/* Redstream: the rdssink plugin (rdslogger: per-stream FPS on stdout; rdsvideosink: display sink for memory:RDS
 * frames).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>

#include "gstrdslogger.h"
#include "gstrdsvideosink.h"

static gboolean
plugin_init (GstPlugin * plugin)
{
  gboolean ok = GST_ELEMENT_REGISTER (rdslogger, plugin);

  ok &= GST_ELEMENT_REGISTER (rdsvideosink, plugin);
  return ok;
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR, GST_VERSION_MINOR, rdssink,
    "Redstream sinks and loggers", plugin_init,
    PACKAGE_VERSION, "LGPL", GST_PACKAGE_NAME, GST_PACKAGE_ORIGIN)
