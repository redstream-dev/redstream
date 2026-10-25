/* Redstream: rdsinfer's preprocessing kernel, compiled by amdclang++ and linked into the plugin as an object.
 * C-compatible C++ only: no exceptions, no STL. The launcher and the output rules are in rds-infer-preprocess.h.
 *
 * One thread per output pixel writes all its channels. Inside the scaled rect it samples and converts with the
 * transform kernels' helpers (rds-kernel-common.h), exactly as rds_buf_surf_transform does for a whole-surface
 * source rect into that destination rect, rounds to bytes like the converter's RGBA / GRAY8 output, then
 * normalises.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <hip/hip_runtime.h>

#include <rds/rds-kernel-common.h>

#include "rds-infer-preprocess.h"

#define RDS_PREPROCESS_BLOCK 16

/* The pre-normalisation value (0..255) of channels 0..2 of pixel (x, y) inside the scaled rect. */
__device__ static inline void
sample_pixel (const RdsPreprocessJob * job, unsigned x, unsigned y, float v[3])
{
  const RdsKernelSurface *s = &job->src;
  int sk = kind_of (s->format);
  RdsTap tx = tap_full (x, job->dl, job->dw, 0, s->width, job->bilinear);
  RdsTap ty = tap_full (y, job->dt, job->dh, 0, s->height, job->bilinear);
  float rgba[4];

  if (sk == KIND_RGB) {
    fetch_rgba (s, tx, ty, rgba);
    if (job->color == RDS_PREPROCESS_GRAY) {
      v[0] = store_u8 (rgb_to_gray (rgba));
      return;
    }
  } else {
    float yv = fetch_y (s, tx, ty);

    if (job->color == RDS_PREPROCESS_GRAY) {
      v[0] = store_u8 (yv);
      return;
    }
    if (sk == KIND_GRAY) {
      rgba[0] = rgba[1] = rgba[2] = yv;
    } else {
      RdsTap cx = tap_chroma_of_luma (x, job->dl, job->dw, 0, s->width, job->bilinear);
      RdsTap cy = tap_chroma_of_luma (y, job->dt, job->dh, 0, s->height, job->bilinear);

      yuv_to_rgb (yv, fetch_uv (s, 0, cx, cy), fetch_uv (s, 1, cx, cy), rgba);
    }
  }
  if (job->color == RDS_PREPROCESS_BGR) {
    v[0] = store_u8 (rgba[2]);
    v[1] = store_u8 (rgba[1]);
    v[2] = store_u8 (rgba[0]);
  } else {
    v[0] = store_u8 (rgba[0]);
    v[1] = store_u8 (rgba[1]);
    v[2] = store_u8 (rgba[2]);
  }
}

__global__ static void
rds_infer_preprocess_kernel (RdsPreprocessJob job, float *dst)
{
  unsigned x = blockIdx.x * blockDim.x + threadIdx.x;
  unsigned y = blockIdx.y * blockDim.y + threadIdx.y;
  size_t plane = (size_t) job.out_w * job.out_h, px;
  float v[3] = { 0.0f, 0.0f, 0.0f };   /* padding */

  if (x >= job.out_w || y >= job.out_h)
    return;
  if (x >= job.dl && x - job.dl < job.dw && y >= job.dt && y - job.dt < job.dh)
    sample_pixel (&job, x, y, v);

  px = (size_t) y * job.out_w + x;
  for (unsigned c = 0; c < job.channels; c++) {
    float o = job.scale * (v[c] - job.offsets[c]);

    if (job.nhwc)
      dst[px * job.channels + c] = o;
    else
      dst[c * plane + px] = o;
  }
}

/* The planes the source format reads have a pitch that holds a row: one luma plane, or for 4:2:0 also the chroma
 * plane(s) of (width + 1) / 2 samples. */
static int
source_planes_ok (const RdsKernelSurface * s)
{
  unsigned long long w = s->width, cw = (w + 1) / 2;

  switch (s->format) {
    case RDS_KFMT_NV12:
      return s->pitch[0] >= w && s->pitch[1] >= 2 * cw;
    case RDS_KFMT_YUV420:
      return s->pitch[0] >= w && s->pitch[1] >= cw && s->pitch[2] >= cw;
    case RDS_KFMT_GRAY8:
      return s->pitch[0] >= w;
    case RDS_KFMT_RGB:
    case RDS_KFMT_BGR:
      return s->pitch[0] >= 3 * w;
    default:                   /* RGBA, BGRA, RGBx, BGRx */
      return s->pitch[0] >= 4 * w;
  }
}

extern "C" int
rds_infer_preprocess_launch (const RdsPreprocessJob * job, float *dst, unsigned index, void *stream)
{
  const RdsKernelSurface *s;

  if (job == NULL || dst == NULL)
    return -1;
  s = &job->src;
  if (s->data == NULL || s->width == 0 || s->height == 0 || s->format < RDS_KFMT_GRAY8 || s->format > RDS_KFMT_BGR
      || !source_planes_ok (s))
    return -1;
  if (job->color < RDS_PREPROCESS_RGB || job->color > RDS_PREPROCESS_GRAY
      || job->channels != (job->color == RDS_PREPROCESS_GRAY ? 1u : 3u))
    return -1;
  if (job->out_w == 0 || job->out_h == 0 || job->dw == 0 || job->dh == 0
      || (unsigned long long) job->dl + job->dw > job->out_w || (unsigned long long) job->dt + job->dh > job->out_h)
    return -1;

  (void) hipGetLastError ();    /* clear a stale error from an earlier call */
  hipLaunchKernelGGL (rds_infer_preprocess_kernel,
      dim3 ((job->out_w + RDS_PREPROCESS_BLOCK - 1) / RDS_PREPROCESS_BLOCK,
          (job->out_h + RDS_PREPROCESS_BLOCK - 1) / RDS_PREPROCESS_BLOCK),
      dim3 (RDS_PREPROCESS_BLOCK, RDS_PREPROCESS_BLOCK), 0, (hipStream_t) stream,
      *job, dst + (size_t) index * job->out_w * job->out_h * job->channels);
  return hipGetLastError () == hipSuccess ? 0 : -1;
}
