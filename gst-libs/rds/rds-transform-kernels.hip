/* Redstream: HIP kernels for libgstrds, compiled by amdclang++ and linked into the C library as an object.
 * C-compatible C++ only: no exceptions, no STL. Entry points are declared in rds-transform-private.h.
 *
 * The transform kernel samples and converts in one pass, with a runtime switch on the formats (uniform per
 * launch, so it doesn't diverge). The rules (docs/compat-matrix.md):
 * - Positions are continuous: pixel i covers [i, i + 1). Destination pixel xd of the rect (dl, dw) maps to
 *   xs = sl + (xd - dl + 0.5) * sw / dw in the source rect (sl, sw). Nearest takes floor (xs), computed exactly in
 *   integers so it never depends on float rounding; bilinear interpolates at xs - 0.5 (float). Source indices are
 *   clamped to the source rect.
 * - 4:2:0 destination: one thread per 2x2 block. Each in-bounds pixel gets its own luma; the block's chroma comes
 *   from the source chroma plane scaled plane-wise (the rects halved), or for an RGB source from the mean U/V of
 *   the block's in-bounds pixels. Blocks on an odd right or bottom surface edge are partial.
 * - 4:2:0 source into RGB/GRAY: chroma sits at half the luma position (centre siting: luma index u is chroma
 *   index (u + 0.5) / 2 - 0.5). Chroma indices are clamped to the halved source rect.
 * - BT.601 limited range both ways; GRAY8 is the luma plane in YUV paths and full-range intensity in RGB paths.
 *   Values stay in float until written, then floor (v + 0.5) clamped to 0..255.
 * - Alpha is copied when both formats have it, else 255; x bytes are 255.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <hip/hip_runtime.h>

#include "rds-kernel-common.h"

#define RDS_SMOKE_BLOCK 256
#define RDS_TRANSFORM_BLOCK 16

__global__ static void
rds_smoke_fill_kernel (unsigned char *dst, unsigned n, unsigned char value)
{
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;

  if (i < n)
    dst[i] = value;
}

extern "C" int
rds_transform_kernel_smoke (void *dev, unsigned n, unsigned char value, void *stream)
{
  unsigned blocks = (n + RDS_SMOKE_BLOCK - 1) / RDS_SMOKE_BLOCK;

  if (dev == NULL)
    return -1;
  if (n == 0)
    return 0;

  (void) hipGetLastError ();    /* clear a stale error from an earlier call */
  hipLaunchKernelGGL (rds_smoke_fill_kernel, dim3 (blocks), dim3 (RDS_SMOKE_BLOCK), 0, (hipStream_t) stream,
      (unsigned char *) dev, n, value);
  return hipGetLastError () == hipSuccess ? 0 : -1;
}

/* ---- transform ---- */

__device__ static inline unsigned char *
dst_at (const RdsKernelSurface * s, int plane, int bpp, unsigned x, unsigned y)
{
  return s->data + s->offset[plane] + (size_t) y * s->pitch[plane] + (size_t) x * bpp;
}

/* One 2x2 block (bx, by) of a 4:2:0 destination rect. */
__device__ static void
transform_block_420 (const RdsKernelJob * job, unsigned bx, unsigned by)
{
  const RdsKernelSurface *s = &job->src, *d = &job->dst;
  int sk = kind_of (s->format);
  unsigned x0 = job->dl + 2 * bx, y0 = job->dt + 2 * by;
  unsigned xend = job->dl + job->dw, yend = job->dt + job->dh;
  unsigned cx = x0 / 2, cy = y0 / 2;
  float su = 0.0f, sv = 0.0f, u, v;
  int n = 0;

  for (unsigned y = y0; y < y0 + 2 && y < yend; y++) {
    RdsTap ty = tap_full (y, job->dt, job->dh, job->st, job->sh, job->bilinear);

    for (unsigned x = x0; x < x0 + 2 && x < xend; x++) {
      RdsTap tx = tap_full (x, job->dl, job->dw, job->sl, job->sw, job->bilinear);
      float yv;

      if (sk == KIND_RGB) {
        float rgba[4], pu, pv;

        fetch_rgba (s, tx, ty, rgba);
        rgb_to_yuv (rgba, &yv, &pu, &pv);
        su += pu;
        sv += pv;
        n++;
      } else {
        yv = fetch_y (s, tx, ty);
      }
      *dst_at (d, 0, 1, x, y) = store_u8 (yv);
    }
  }

  if (sk == KIND_RGB) {
    u = su / (float) n;
    v = sv / (float) n;
  } else if (sk == KIND_GRAY) {
    u = v = 128.0f;
  } else {
    RdsTap tx = tap_chroma_planewise (cx, job->dl, job->dw, job->sl, job->sw, job->bilinear);
    RdsTap ty = tap_chroma_planewise (cy, job->dt, job->dh, job->st, job->sh, job->bilinear);

    u = fetch_uv (s, 0, tx, ty);
    v = fetch_uv (s, 1, tx, ty);
  }
  if (d->format == RDS_KFMT_NV12) {
    unsigned char *p = dst_at (d, 1, 2, cx, cy);
    p[0] = store_u8 (u);
    p[1] = store_u8 (v);
  } else {
    *dst_at (d, 1, 1, cx, cy) = store_u8 (u);
    *dst_at (d, 2, 1, cx, cy) = store_u8 (v);
  }
}

/* One pixel (x, y) of an RGB-family or GRAY8 destination rect. */
__device__ static void
transform_pixel (const RdsKernelJob * job, unsigned x, unsigned y)
{
  const RdsKernelSurface *s = &job->src, *d = &job->dst;
  int sk = kind_of (s->format);
  RdsTap tx = tap_full (x, job->dl, job->dw, job->sl, job->sw, job->bilinear);
  RdsTap ty = tap_full (y, job->dt, job->dh, job->st, job->sh, job->bilinear);
  float rgba[4];
  int src_alpha = 0;
  RdsRgbLayout l;
  unsigned char *p;

  if (sk == KIND_RGB) {
    fetch_rgba (s, tx, ty, rgba);
    src_alpha = rgb_layout (s->format).a >= 0;
  } else {
    float yv = fetch_y (s, tx, ty);

    if (d->format == RDS_KFMT_GRAY8) {
      *dst_at (d, 0, 1, x, y) = store_u8 (yv);
      return;
    }
    if (sk == KIND_GRAY) {
      rgba[0] = rgba[1] = rgba[2] = yv;
    } else {
      RdsTap cx = tap_chroma_of_luma (x, job->dl, job->dw, job->sl, job->sw, job->bilinear);
      RdsTap cy = tap_chroma_of_luma (y, job->dt, job->dh, job->st, job->sh, job->bilinear);

      yuv_to_rgb (yv, fetch_uv (s, 0, cx, cy), fetch_uv (s, 1, cx, cy), rgba);
    }
    rgba[3] = 255.0f;
  }

  if (d->format == RDS_KFMT_GRAY8) {
    *dst_at (d, 0, 1, x, y) = store_u8 (rgb_to_gray (rgba));
    return;
  }
  l = rgb_layout (d->format);
  p = dst_at (d, 0, l.size, x, y);
  p[l.r] = store_u8 (rgba[0]);
  p[l.g] = store_u8 (rgba[1]);
  p[l.b] = store_u8 (rgba[2]);
  if (l.size == 4)
    p[3] = (l.a >= 0 && src_alpha) ? store_u8 (rgba[3]) : 255;
}

__global__ static void
rds_transform_kernel (RdsKernelJob job)
{
  unsigned gx = blockIdx.x * blockDim.x + threadIdx.x;
  unsigned gy = blockIdx.y * blockDim.y + threadIdx.y;

  if (kind_of (job.dst.format) == KIND_YUV420) {
    if (gx < (job.dw + 1) / 2 && gy < (job.dh + 1) / 2)
      transform_block_420 (&job, gx, gy);
  } else if (gx < job.dw && gy < job.dh) {
    transform_pixel (&job, job.dl + gx, job.dt + gy);
  }
}

extern "C" int
rds_transform_launch (const RdsKernelJob * job, void *stream)
{
  unsigned w = job->dw, h = job->dh;
  dim3 block (RDS_TRANSFORM_BLOCK, RDS_TRANSFORM_BLOCK);

  if (job->dst.format == RDS_KFMT_NV12 || job->dst.format == RDS_KFMT_YUV420) {
    w = (w + 1) / 2;
    h = (h + 1) / 2;
  }
  (void) hipGetLastError ();    /* clear a stale error from an earlier call */
  hipLaunchKernelGGL (rds_transform_kernel,
      dim3 ((w + RDS_TRANSFORM_BLOCK - 1) / RDS_TRANSFORM_BLOCK, (h + RDS_TRANSFORM_BLOCK - 1) / RDS_TRANSFORM_BLOCK),
      block, 0, (hipStream_t) stream, *job);
  return hipGetLastError () == hipSuccess ? 0 : -1;
}
