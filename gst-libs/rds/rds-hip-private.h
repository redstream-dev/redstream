/* Private HIP helpers for libgstrds. Never include from a public header (see the include-order note).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_HIP_PRIVATE_H
#define RDS_HIP_PRIVATE_H

/* Order matters: HIP's headers define __noinline__ as a macro, and GLib's gmacros.h then fails on
 * __has_attribute(__noinline__). Always include GStreamer/GLib first. */
#include <gst/gst.h>
#include <hip/hip_runtime_api.h>

/* Evaluates a HIP call, logs failures with GST_ERROR, and yields TRUE on success.
 * It logs to GST_CAT_DEFAULT, so the file's ensure_debug_category () must have run first. */
#define RDS_HIP_CHECK(call) G_GNUC_EXTENSION ({                          \
    hipError_t _rds_err = (call);                                        \
    if (G_UNLIKELY (_rds_err != hipSuccess))                             \
      GST_ERROR ("%s failed: %s (%d)", #call,                            \
          hipGetErrorString (_rds_err), (gint) _rds_err);                \
    _rds_err == hipSuccess; })

#endif /* RDS_HIP_PRIVATE_H */
