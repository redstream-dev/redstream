/* Private HIP helpers for libgstrds. Never include from a public header (see the include-order note).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_HIP_PRIVATE_H
#define RDS_HIP_PRIVATE_H

/* Order matters: HIP's headers define __noinline__ as a macro, and GLib's gmacros.h then fails on
 * __has_attribute(__noinline__). Always include GStreamer/GLib first. */
#include <gst/gst.h>
#include <hip/hip_runtime_api.h>

static inline gboolean
rds_hip_check (GstDebugCategory * cat, hipError_t err, const gchar * expr,
    const gchar * file, const gchar * func, gint line)
{
  if (G_LIKELY (err == hipSuccess))
    return TRUE;
  gst_debug_log (cat, GST_LEVEL_ERROR, file, func, line, NULL, "%s failed: %s (%d)",
      expr, hipGetErrorString (err), (gint) err);
  return FALSE;
}

/* Evaluates a HIP call, logs failures to GST_CAT_DEFAULT, and yields TRUE on success. */
#define RDS_HIP_CHECK(call) \
  rds_hip_check (GST_CAT_DEFAULT, (call), #call, __FILE__, G_STRFUNC, __LINE__)

#endif /* RDS_HIP_PRIVATE_H */
