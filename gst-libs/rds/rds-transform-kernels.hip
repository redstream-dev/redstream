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

#include "rds-transform-private.h"

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

enum
{
  KIND_YUV420,
  KIND_GRAY,
  KIND_RGB,
};

/* Two source indices along one axis and the weight of the second (0 for nearest). */
typedef struct
{
  int i0, i1;
  float f;
} RdsTap;

/* Byte offsets of R, G, B and alpha (-1: none) in a pixel, and the pixel size; GRAY8 reads R = G = B. */
typedef struct
{
  int r, g, b, a, size;
} RdsRgbLayout;

__device__ static inline int
kind_of (int fmt)
{
  switch (fmt) {
    case RDS_KFMT_YUV420:
    case RDS_KFMT_NV12:
      return KIND_YUV420;
    case RDS_KFMT_GRAY8:
      return KIND_GRAY;
    default:
      return KIND_RGB;
  }
}

__device__ static inline RdsRgbLayout
rgb_layout (int fmt)
{
  RdsRgbLayout l;

  switch (fmt) {
    case RDS_KFMT_RGBA:
      l.r = 0, l.g = 1, l.b = 2, l.a = 3, l.size = 4;
      break;
    case RDS_KFMT_BGRA:
      l.r = 2, l.g = 1, l.b = 0, l.a = 3, l.size = 4;
      break;
    case RDS_KFMT_RGBx:
      l.r = 0, l.g = 1, l.b = 2, l.a = -1, l.size = 4;
      break;
    case RDS_KFMT_BGRx:
      l.r = 2, l.g = 1, l.b = 0, l.a = -1, l.size = 4;
      break;
    case RDS_KFMT_RGB:
      l.r = 0, l.g = 1, l.b = 2, l.a = -1, l.size = 3;
      break;
    case RDS_KFMT_BGR:
      l.r = 2, l.g = 1, l.b = 0, l.a = -1, l.size = 3;
      break;
    default:                   /* GRAY8 */
      l.r = 0, l.g = 0, l.b = 0, l.a = -1, l.size = 1;
      break;
  }
  return l;
}

__device__ static inline int
clamp_int (int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

__device__ static inline unsigned char
store_u8 (float v)
{
  float r = floorf (v + 0.5f);

  return (unsigned char) (r < 0.0f ? 0.0f : (r > 255.0f ? 255.0f : r));
}

/* Bilinear tap at index-space position p (integers are sample centres), clamped to [lo, hi]. */
__device__ static inline RdsTap
tap_at (float p, int lo, int hi)
{
  RdsTap t;
  float fl = floorf (p);
  int i = (int) fl;

  t.i0 = clamp_int (i, lo, hi);
  t.i1 = clamp_int (i + 1, lo, hi);
  t.f = p - fl;
  return t;
}

__device__ static inline RdsTap
tap_nearest (unsigned i, int lo, int hi)
{
  RdsTap t;

  t.i0 = t.i1 = clamp_int ((int) i, lo, hi);
  t.f = 0.0f;
  return t;
}

/* Full-resolution tap for destination index d of the rect (dl, dlen), source rect (lo, len). */
__device__ static inline RdsTap
tap_full (unsigned d, unsigned dl, unsigned dlen, unsigned lo, unsigned len, int bilinear)
{
  unsigned k = d - dl;
  int hi = (int) (lo + len) - 1;

  if (!bilinear)
    return tap_nearest (lo + ((2 * k + 1) * len) / (2 * dlen), (int) lo, hi);
  return tap_at ((float) lo + ((float) k + 0.5f) * ((float) len / (float) dlen) - 0.5f, (int) lo, hi);
}

/* Chroma tap for destination luma index d (4:2:0 source into RGB/GRAY): half the luma position. */
__device__ static inline RdsTap
tap_chroma_of_luma (unsigned d, unsigned dl, unsigned dlen, unsigned lo, unsigned len, int bilinear)
{
  unsigned k = d - dl;
  int clo = (int) (lo / 2), chi = (int) ((lo + len + 1) / 2) - 1;

  if (!bilinear)
    return tap_nearest ((2 * lo * dlen + (2 * k + 1) * len) / (4 * dlen), clo, chi);
  return tap_at (((float) lo + ((float) k + 0.5f) * ((float) len / (float) dlen)) * 0.5f - 0.5f, clo, chi);
}

/* Chroma tap for destination chroma index c (4:2:0 into 4:2:0): the chroma plane scaled as an image, with rects
 * (dl / 2, dlen / 2) and (lo / 2, len / 2); dl is even. */
__device__ static inline RdsTap
tap_chroma_planewise (unsigned c, unsigned dl, unsigned dlen, unsigned lo, unsigned len, int bilinear)
{
  int clo = (int) (lo / 2), chi = (int) ((lo + len + 1) / 2) - 1;

  if (!bilinear)
    return tap_nearest ((lo * dlen + (2 * c - dl + 1) * len) / (2 * dlen), clo, chi);
  return tap_at ((float) lo * 0.5f + ((float) (c - dl / 2) + 0.5f) * ((float) len / (float) dlen) - 0.5f, clo,
      chi);
}

__device__ static inline float
fetch (const unsigned char *plane, unsigned pitch, int bpp, int byte, int x, int y)
{
  return (float) plane[(size_t) y * pitch + (size_t) x * bpp + byte];
}

__device__ static inline float
sample_plane (const unsigned char *plane, unsigned pitch, int bpp, int byte, RdsTap tx, RdsTap ty)
{
  float v00 = fetch (plane, pitch, bpp, byte, tx.i0, ty.i0);
  float v10, v01, v11, top, bot;

  if (tx.f == 0.0f && ty.f == 0.0f)
    return v00;
  v10 = fetch (plane, pitch, bpp, byte, tx.i1, ty.i0);
  v01 = fetch (plane, pitch, bpp, byte, tx.i0, ty.i1);
  v11 = fetch (plane, pitch, bpp, byte, tx.i1, ty.i1);
  top = v00 + tx.f * (v10 - v00);
  bot = v01 + tx.f * (v11 - v01);
  return top + ty.f * (bot - top);
}

__device__ static inline float
fetch_y (const RdsKernelSurface * s, RdsTap tx, RdsTap ty)
{
  return sample_plane (s->data + s->offset[0], s->pitch[0], 1, 0, tx, ty);
}

/* U (comp 0) or V (comp 1) of a 4:2:0 surface: NV12 interleaved, I420 separate planes. */
__device__ static inline float
fetch_uv (const RdsKernelSurface * s, int comp, RdsTap tx, RdsTap ty)
{
  if (s->format == RDS_KFMT_NV12)
    return sample_plane (s->data + s->offset[1], s->pitch[1], 2, comp, tx, ty);
  return sample_plane (s->data + s->offset[1 + comp], s->pitch[1 + comp], 1, 0, tx, ty);
}

/* R, G, B and alpha (255 when the format has none). */
__device__ static inline void
fetch_rgba (const RdsKernelSurface * s, RdsTap tx, RdsTap ty, float rgba[4])
{
  RdsRgbLayout l = rgb_layout (s->format);
  const unsigned char *p = s->data + s->offset[0];

  rgba[0] = sample_plane (p, s->pitch[0], l.size, l.r, tx, ty);
  rgba[1] = sample_plane (p, s->pitch[0], l.size, l.g, tx, ty);
  rgba[2] = sample_plane (p, s->pitch[0], l.size, l.b, tx, ty);
  rgba[3] = l.a >= 0 ? sample_plane (p, s->pitch[0], l.size, l.a, tx, ty) : 255.0f;
}

/* BT.601 limited range */
__device__ static inline void
rgb_to_yuv (const float rgb[3], float *y, float *u, float *v)
{
  *y = 16.0f + 0.257f * rgb[0] + 0.504f * rgb[1] + 0.098f * rgb[2];
  *u = 128.0f - 0.148f * rgb[0] - 0.291f * rgb[1] + 0.439f * rgb[2];
  *v = 128.0f + 0.439f * rgb[0] - 0.368f * rgb[1] - 0.071f * rgb[2];
}

__device__ static inline void
yuv_to_rgb (float y, float u, float v, float rgb[3])
{
  rgb[0] = 1.164f * (y - 16.0f) + 1.596f * (v - 128.0f);
  rgb[1] = 1.164f * (y - 16.0f) - 0.392f * (u - 128.0f) - 0.813f * (v - 128.0f);
  rgb[2] = 1.164f * (y - 16.0f) + 2.017f * (u - 128.0f);
}

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
    *dst_at (d, 0, 1, x, y) = store_u8 (0.299f * rgba[0] + 0.587f * rgba[1] + 0.114f * rgba[2]);
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
