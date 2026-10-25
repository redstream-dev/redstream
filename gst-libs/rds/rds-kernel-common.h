/* Device helpers shared by the HIP kernels (rds-transform-kernels.hip and rdsinfer's preprocessing kernel): pixel
 * formats, sampling taps, plane fetches and the BT.601 colour conversions, so every kernel samples and converts
 * colour with the same code. The rules are documented at the top of rds-transform-kernels.hip. HIP only, all
 * static inline; private, not installed.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_KERNEL_COMMON_H
#define RDS_KERNEL_COMMON_H

#include <hip/hip_runtime.h>

#include "rds-transform-private.h"

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

/* Full-range intensity of an RGB value (BT.601 weights): GRAY8 from an RGB-family source. */
__device__ static inline float
rgb_to_gray (const float rgb[3])
{
  return 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
}

#endif /* RDS_KERNEL_COMMON_H */
