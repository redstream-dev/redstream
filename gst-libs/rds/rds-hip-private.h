/* Private HIP helpers for libgstrds and the in-tree plugins. Never include from a public header (see the include-order note).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_HIP_PRIVATE_H
#define RDS_HIP_PRIVATE_H

/* Order matters: HIP's headers define __noinline__ as a macro, and GLib's gmacros.h then fails on
 * __has_attribute(__noinline__). Always include GStreamer/GLib first. */
#include <gst/gst.h>
#include <string.h>
#include <hip/hip_runtime_api.h>

/* Evaluates a HIP call, logs failures with GST_ERROR, and yields TRUE on success.
 * It logs to GST_CAT_DEFAULT, so the file's ensure_debug_category () must have run first. */
#define RDS_HIP_CHECK(call) G_GNUC_EXTENSION ({                          \
    hipError_t _rds_err = (call);                                        \
    if (G_UNLIKELY (_rds_err != hipSuccess))                             \
      GST_ERROR ("%s failed: %s (%d)", #call,                            \
          hipGetErrorString (_rds_err), (gint) _rds_err);                \
    _rds_err == hipSuccess; })

/* Copies @height rows of @width bytes from GPU-accessible @src into host memory @dst and waits for it on @stream.
 *
 * HIP's 2D copy (hipMemcpy2D, hipMemcpy2DAsync) into host memory, pageable or pinned, can report completion
 * before its last rows have landed when the host is under memory load: ROCm 10.1 on the gfx1036 iGPU, with the
 * default settings, with HSA_ENABLE_SDMA=0, and for pageable memory even with HSA_DISABLE_CACHE=1. One 1D copy
 * of the whole pitched span doesn't show it, so this does that instead: straight into @dst when the pitches
 * match (the bytes between rows are overwritten too), otherwise into @bounce, then a row repack on the CPU.
 * @bounce grows as needed; the caller owns it and frees it with g_free (). */
static inline hipError_t
rds_hip_copy_2d_to_host (void *dst, gsize dpitch, const void *src, gsize spitch, gsize width, gsize height,
    hipStream_t stream, guint8 ** bounce, gsize * bounce_size)
{
  gsize span;
  hipError_t err;

  if (width == 0 || height == 0)
    return hipSuccess;
  span = spitch * (height - 1) + width;
  if (dpitch == spitch || height == 1) {
    err = hipMemcpyAsync (dst, src, span, hipMemcpyDefault, stream);
    return err == hipSuccess ? hipStreamSynchronize (stream) : err;
  }
  if (*bounce_size < span) {
    g_free (*bounce);
    *bounce = g_malloc (span);
    *bounce_size = span;
  }
  err = hipMemcpyAsync (*bounce, src, span, hipMemcpyDefault, stream);
  if (err == hipSuccess)
    err = hipStreamSynchronize (stream);
  if (err != hipSuccess)
    return err;
  for (gsize y = 0; y < height; y++)
    memcpy ((guint8 *) dst + y * dpitch, *bounce + y * spitch, width);
  return hipSuccess;
}

#endif /* RDS_HIP_PRIVATE_H */
