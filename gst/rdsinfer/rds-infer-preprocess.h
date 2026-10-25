/* Redstream: rdsinfer's preprocessing kernel (rds-infer-preprocess.hip), plugin-internal, not installed: one batch
 * slot into one image of the network's float input tensor, letterboxed and normalised. The shared part is plain C
 * with no GLib and no HIP types (streams are void *), so both compilers include it; the C-only helpers at the end
 * are hidden from the HIP compiler.
 *
 * Output pixel (x, y) of image `index`, channel c:
 * - inside the scaled rect (dl, dt, dw, dh): the source surface resized to dw x dh exactly as rds_buf_surf_transform
 *   resizes the whole surface into that rect (same sampling positions, filters, chroma siting, BT.601 conversion
 *   and rounding to 0..255, through the helpers in rds-kernel-common.h): the RGBA value for RGB and BGR, the GRAY8
 *   value for GRAY (the luma for YUV and GRAY8 sources, the BT.601 intensity of RGB for RGB-family sources);
 * - outside it (the letterbox padding): 0;
 * then y = scale * (x - offsets[c]). RGB writes R, G, B as channels 0, 1, 2 and BGR writes B, G, R; GRAY has one
 * channel (offsets[0]). NCHW puts channel c of pixel (x, y) at c * out_h * out_w + y * out_w + x, NHWC at
 * (y * out_w + x) * channels + c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_PREPROCESS_H
#define RDS_INFER_PREPROCESS_H

#include <rds/rds-transform-private.h>

#ifdef __cplusplus
extern "C" {
#endif

enum
{
  RDS_PREPROCESS_RGB = 0,       /* model-color-format values */
  RDS_PREPROCESS_BGR = 1,
  RDS_PREPROCESS_GRAY = 2,
};

typedef struct
{
  RdsKernelSurface src;         /* one slot, the whole surface is the source */
  unsigned out_w, out_h, channels;      /* network input; channels is 3 for RGB and BGR, 1 for GRAY */
  int color;                    /* RDS_PREPROCESS_* */
  int nhwc, bilinear;           /* bilinear: after the filter mapping (rds_transform_filter_is_bilinear) */
  float scale, offsets[3];
  unsigned dl, dt, dw, dh;      /* where the scaled source goes: RdsInferLetterbox's pad and scaled size */
} RdsPreprocessJob;

/* Writes image @index of the float tensor at @dst (out_w * out_h * channels floats per image) and nothing else,
 * asynchronously on @stream (a hipStream_t, NULL for the null stream) on the current device. Returns 0, or -1 if
 * the job is invalid (no source or destination, an empty or out-of-range rect, channels not matching color) or the
 * launch failed. */
__attribute__ ((visibility ("hidden"))) int rds_infer_preprocess_launch (const RdsPreprocessJob * job, float *dst,
    unsigned index, void *stream);

#ifdef __cplusplus
}
#endif

#ifndef __HIP__
#include "rds-infer-post.h"

/* Sets the source (the whole slot @slot) and the geometry (network size, scaled rect) of @job from @lb, which must
 * have been computed for this slot's size: returns FALSE, leaving @job untouched, when lb's frame size isn't the
 * slot's. The other fields are left for the caller. */
static inline gboolean
rds_infer_preprocess_job_set_source (RdsPreprocessJob * job, const RdsBufSurfaceParams * slot,
    const RdsInferLetterbox * lb)
{
  if (lb->frame_w != slot->width || lb->frame_h != slot->height)
    return FALSE;
  rds_kernel_surface_init (&job->src, slot);
  job->out_w = lb->net_w;
  job->out_h = lb->net_h;
  job->dl = lb->pad_left;
  job->dt = lb->pad_top;
  job->dw = lb->scaled_w;
  job->dh = lb->scaled_h;
  return TRUE;
}
#endif

#endif /* RDS_INFER_PREPROCESS_H */
