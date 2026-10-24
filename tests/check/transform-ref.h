/* CPU reference for rds_buf_surf_transform, written from the documented rules (sampling at pixel centres, BT.601
 * limited range, plane-wise 4:2:0 scaling, centre-sited chroma). Test-only and deliberately independent of the HIP
 * kernels: it walks the destination plane by plane instead of in 2x2 blocks and uses its own arithmetic.
 *
 * Rules, as implemented here:
 * - Positions are continuous (pixel i covers [i, i + 1)). Destination pixel xd of a rect (dl, dw) maps to
 *   xs = sl + (xd - dl + 0.5) * sw / dw in a source rect (sl, sw). Nearest takes floor (xs), computed exactly in
 *   integers; bilinear interpolates at xs - 0.5 between floor and floor + 1. Indices are clamped to the rect.
 * - A 4:2:0 chroma plane is scaled as an image of its own: its rects are the luma rects halved (left / 2,
 *   width / 2, in real numbers), and indices are clamped to [floor (left / 2), ceil ((left + width) / 2) - 1].
 * - 4:2:0 -> RGB/GRAY: chroma sits at half the luma position (the chroma position of luma position xs is xs / 2,
 *   i.e. centre siting), with the same nearest / bilinear rule and the chroma clamp above.
 * - Sampled values stay in floating point until the output is written: rounding is floor (v + 0.5), clamped to
 *   0..255, and happens once per output byte.
 * - RGB -> 4:2:0: every destination pixel's RGB is converted; Y is written per pixel, U and V are the mean of the
 *   (unrounded) values of the block's pixels that lie inside the destination rect.
 * - GRAY8 is the luma plane in YUV paths (U = V = 128) and full-range intensity in RGB paths.
 * - Alpha is copied when both formats have it, else 255; x bytes are 255.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TRANSFORM_REF_H
#define RDS_TRANSFORM_REF_H

#include <glib.h>
#include <math.h>
#include <rds/rds-buf-surface.h>

typedef struct
{
  RdsBufSurfaceColorFormat format;
  guint width, height;
  guint8 *plane[3];
  guint pitch[3];
} RefImage;

typedef struct
{
  guint left, top, width, height;
} RefRect;

typedef enum
{
  REF_KIND_YUV420,
  REF_KIND_GRAY,
  REF_KIND_RGB,
} RefKind;

static inline RefKind
ref_kind (RdsBufSurfaceColorFormat f)
{
  if (f == RDS_BUF_COLOR_FORMAT_NV12 || f == RDS_BUF_COLOR_FORMAT_YUV420)
    return REF_KIND_YUV420;
  if (f == RDS_BUF_COLOR_FORMAT_GRAY8)
    return REF_KIND_GRAY;
  return REF_KIND_RGB;
}

/* Byte positions of R, G, B and alpha (-1: none) inside one pixel, and the pixel size. */
static inline void
ref_rgb_layout (RdsBufSurfaceColorFormat f, int *r, int *g, int *b, int *a, int *size)
{
  *a = -1;
  switch (f) {
    case RDS_BUF_COLOR_FORMAT_RGBA:
      *r = 0, *g = 1, *b = 2, *a = 3, *size = 4;
      break;
    case RDS_BUF_COLOR_FORMAT_BGRA:
      *b = 0, *g = 1, *r = 2, *a = 3, *size = 4;
      break;
    case RDS_BUF_COLOR_FORMAT_RGBx:
      *r = 0, *g = 1, *b = 2, *size = 4;
      break;
    case RDS_BUF_COLOR_FORMAT_BGRx:
      *b = 0, *g = 1, *r = 2, *size = 4;
      break;
    case RDS_BUF_COLOR_FORMAT_RGB:
      *r = 0, *g = 1, *b = 2, *size = 3;
      break;
    case RDS_BUF_COLOR_FORMAT_BGR:
      *b = 0, *g = 1, *r = 2, *size = 3;
      break;
    default:                   /* GRAY8 */
      *r = *g = *b = 0, *size = 1;
      break;
  }
}

static inline guint8
ref_store (double v)
{
  float f = floorf ((float) v + 0.5f);

  return (guint8) (f < 0.0f ? 0.0f : (f > 255.0f ? 255.0f : f));
}

/* One axis of a sampling position: the two source indices and the weight of the second. */
typedef struct
{
  gint64 a, b;
  double w;
} RefTap;

static inline gint64
ref_clamp (gint64 v, gint64 lo, gint64 hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

/* Source position num / den (continuous coordinates, num >= 0) sampled inside [lo, hi]. */
static inline RefTap
ref_tap (gint64 num, gint64 den, gint64 lo, gint64 hi, gboolean bilinear)
{
  RefTap t;

  if (!bilinear) {
    t.a = t.b = ref_clamp (num / den, lo, hi);
    t.w = 0.0;
  } else {
    double p = (double) num / (double) den - 0.5;
    double fl = floor (p);

    t.a = ref_clamp ((gint64) fl, lo, hi);
    t.b = ref_clamp ((gint64) fl + 1, lo, hi);
    t.w = p - fl;
  }
  return t;
}

/* Luma (full-resolution) tap for destination index d of the rect (dl, dw), source rect (sl, sw):
 * xs = (2 * sl * dw + (2 * (d - dl) + 1) * sw) / (2 * dw). */
static inline RefTap
ref_tap_full (guint d, guint dl, guint dw, guint sl, guint sw, gboolean bilinear)
{
  gint64 num = 2 * (gint64) sl * dw + (2 * ((gint64) d - dl) + 1) * (gint64) sw;

  return ref_tap (num, 2 * (gint64) dw, sl, (gint64) sl + sw - 1, bilinear);
}

/* Chroma tap of a source rect (sl, sw) for a source luma position given as num / den: position / 2. */
static inline RefTap
ref_tap_chroma_of (gint64 num, gint64 den, guint sl, guint sw, gboolean bilinear)
{
  return ref_tap (num, 2 * den, sl / 2, ((gint64) sl + sw + 1) / 2 - 1, bilinear);
}

static inline double
ref_lerp2 (double v00, double v10, double v01, double v11, double wx, double wy)
{
  return (1.0 - wy) * ((1.0 - wx) * v00 + wx * v10) + wy * ((1.0 - wx) * v01 + wx * v11);
}

static inline double
ref_px (const RefImage * im, int plane, int bpp, int byte, gint64 x, gint64 y)
{
  return im->plane[plane][y * im->pitch[plane] + x * bpp + byte];
}

static inline double
ref_sample (const RefImage * im, int plane, int bpp, int byte, RefTap tx, RefTap ty)
{
  return ref_lerp2 (ref_px (im, plane, bpp, byte, tx.a, ty.a), ref_px (im, plane, bpp, byte, tx.b, ty.a),
      ref_px (im, plane, bpp, byte, tx.a, ty.b), ref_px (im, plane, bpp, byte, tx.b, ty.b), tx.w, ty.w);
}

/* Chroma of a 4:2:0 image: component 0 = U, 1 = V. */
static inline double
ref_sample_uv (const RefImage * im, int comp, RefTap tx, RefTap ty)
{
  if (im->format == RDS_BUF_COLOR_FORMAT_NV12)
    return ref_sample (im, 1, 2, comp, tx, ty);
  return ref_sample (im, 1 + comp, 1, 0, tx, ty);
}

static inline void
ref_sample_rgba (const RefImage * im, RefTap tx, RefTap ty, double rgba[4])
{
  int r, g, b, a, size;

  ref_rgb_layout (im->format, &r, &g, &b, &a, &size);
  rgba[0] = ref_sample (im, 0, size, r, tx, ty);
  rgba[1] = ref_sample (im, 0, size, g, tx, ty);
  rgba[2] = ref_sample (im, 0, size, b, tx, ty);
  rgba[3] = a >= 0 ? ref_sample (im, 0, size, a, tx, ty) : 255.0;
}

static inline void
ref_rgb_to_yuv (const double rgb[3], double yuv[3])
{
  yuv[0] = 16.0 + 0.257 * rgb[0] + 0.504 * rgb[1] + 0.098 * rgb[2];
  yuv[1] = 128.0 - 0.148 * rgb[0] - 0.291 * rgb[1] + 0.439 * rgb[2];
  yuv[2] = 128.0 + 0.439 * rgb[0] - 0.368 * rgb[1] - 0.071 * rgb[2];
}

static inline void
ref_yuv_to_rgb (double y, double u, double v, double rgb[3])
{
  double c = 1.164 * (y - 16.0);

  rgb[0] = c + 1.596 * (v - 128.0);
  rgb[1] = c - 0.392 * (u - 128.0) - 0.813 * (v - 128.0);
  rgb[2] = c + 2.017 * (u - 128.0);
}

static inline guint8 *
ref_dst_byte (RefImage * im, int plane, int bpp, int byte, guint x, guint y)
{
  return &im->plane[plane][(gsize) y * im->pitch[plane] + (gsize) x * bpp + byte];
}

/* Writes one RGB-family or GRAY8 destination pixel from RGBA values; rgba[3] is used only when src_alpha. */
static inline void
ref_put_rgb (RefImage * dst, guint x, guint y, const double rgba[4], gboolean src_alpha)
{
  int r, g, b, a, size;
  guint8 *p;

  if (dst->format == RDS_BUF_COLOR_FORMAT_GRAY8) {
    *ref_dst_byte (dst, 0, 1, 0, x, y) = ref_store (0.299 * rgba[0] + 0.587 * rgba[1] + 0.114 * rgba[2]);
    return;
  }
  ref_rgb_layout (dst->format, &r, &g, &b, &a, &size);
  p = ref_dst_byte (dst, 0, size, 0, x, y);
  p[r] = ref_store (rgba[0]);
  p[g] = ref_store (rgba[1]);
  p[b] = ref_store (rgba[2]);
  if (size == 4)
    p[a >= 0 ? a : 3] = (a >= 0 && src_alpha) ? ref_store (rgba[3]) : 255;
}

/* Transforms rect s of src into rect d of dst. For a 4:2:0 dst, d must already be even-aligned (or end on the
 * surface edge). Destination bytes outside d are not touched. */
static inline void
ref_transform (const RefImage * src, RefImage * dst, RefRect s, RefRect d, gboolean bilinear)
{
  RefKind sk = ref_kind (src->format), dk = ref_kind (dst->format);
  int sr, sg, sb, sa, ssize;

  ref_rgb_layout (src->format, &sr, &sg, &sb, &sa, &ssize);

  if (dk == REF_KIND_YUV420) {
    /* luma plane */
    for (guint y = d.top; y < d.top + d.height; y++) {
      RefTap ty = ref_tap_full (y, d.top, d.height, s.top, s.height, bilinear);
      for (guint x = d.left; x < d.left + d.width; x++) {
        RefTap tx = ref_tap_full (x, d.left, d.width, s.left, s.width, bilinear);
        double yv;

        if (sk == REF_KIND_RGB) {
          double rgba[4], yuv[3];
          ref_sample_rgba (src, tx, ty, rgba);
          ref_rgb_to_yuv (rgba, yuv);
          yv = yuv[0];
        } else {
          yv = ref_sample (src, 0, 1, 0, tx, ty);
        }
        *ref_dst_byte (dst, 0, 1, 0, x, y) = ref_store (yv);
      }
    }
    /* chroma plane(s): one sample per 2x2 block, blocks clipped to the rect */
    for (guint cy = d.top / 2; cy < (d.top + d.height + 1) / 2; cy++) {
      for (guint cx = d.left / 2; cx < (d.left + d.width + 1) / 2; cx++) {
        double u, v;

        if (sk == REF_KIND_GRAY) {
          u = v = 128.0;
        } else if (sk == REF_KIND_YUV420) {
          /* chroma rects are the luma rects halved: xs_c = sl / 2 + (cx - dl / 2 + 0.5) * sw / dw */
          gint64 nx = (gint64) s.left * d.width + (2 * (gint64) cx - d.left + 1) * s.width;
          gint64 ny = (gint64) s.top * d.height + (2 * (gint64) cy - d.top + 1) * s.height;
          RefTap tx = ref_tap (nx, 2 * (gint64) d.width, s.left / 2, ((gint64) s.left + s.width + 1) / 2 - 1,
              bilinear);
          RefTap ty = ref_tap (ny, 2 * (gint64) d.height, s.top / 2, ((gint64) s.top + s.height + 1) / 2 - 1,
              bilinear);
          u = ref_sample_uv (src, 0, tx, ty);
          v = ref_sample_uv (src, 1, tx, ty);
        } else {
          double su = 0.0, sv = 0.0;
          int n = 0;

          for (guint y = 2 * cy; y < 2 * cy + 2; y++) {
            for (guint x = 2 * cx; x < 2 * cx + 2; x++) {
              double rgba[4], yuv[3];
              if (x < d.left || x >= d.left + d.width || y < d.top || y >= d.top + d.height)
                continue;
              ref_sample_rgba (src, ref_tap_full (x, d.left, d.width, s.left, s.width, bilinear),
                  ref_tap_full (y, d.top, d.height, s.top, s.height, bilinear), rgba);
              ref_rgb_to_yuv (rgba, yuv);
              su += yuv[1];
              sv += yuv[2];
              n++;
            }
          }
          u = su / n;
          v = sv / n;
        }
        if (dst->format == RDS_BUF_COLOR_FORMAT_NV12) {
          *ref_dst_byte (dst, 1, 2, 0, cx, cy) = ref_store (u);
          *ref_dst_byte (dst, 1, 2, 1, cx, cy) = ref_store (v);
        } else {
          *ref_dst_byte (dst, 1, 1, 0, cx, cy) = ref_store (u);
          *ref_dst_byte (dst, 2, 1, 0, cx, cy) = ref_store (v);
        }
      }
    }
    return;
  }

  for (guint y = d.top; y < d.top + d.height; y++) {
    for (guint x = d.left; x < d.left + d.width; x++) {
      gint64 nx = 2 * (gint64) s.left * d.width + (2 * ((gint64) x - d.left) + 1) * (gint64) s.width;
      gint64 ny = 2 * (gint64) s.top * d.height + (2 * ((gint64) y - d.top) + 1) * (gint64) s.height;
      RefTap tx = ref_tap (nx, 2 * (gint64) d.width, s.left, (gint64) s.left + s.width - 1, bilinear);
      RefTap ty = ref_tap (ny, 2 * (gint64) d.height, s.top, (gint64) s.top + s.height - 1, bilinear);
      double rgba[4];
      gboolean src_alpha = FALSE;

      if (sk == REF_KIND_YUV420 || sk == REF_KIND_GRAY) {
        double yv = ref_sample (src, 0, 1, 0, tx, ty);

        if (dk == REF_KIND_GRAY) {
          *ref_dst_byte (dst, 0, 1, 0, x, y) = ref_store (yv);
          continue;
        }
        if (sk == REF_KIND_GRAY) {
          rgba[0] = rgba[1] = rgba[2] = yv;
        } else {
          RefTap cx = ref_tap_chroma_of (nx, 2 * (gint64) d.width, s.left, s.width, bilinear);
          RefTap cy = ref_tap_chroma_of (ny, 2 * (gint64) d.height, s.top, s.height, bilinear);
          ref_yuv_to_rgb (yv, ref_sample_uv (src, 0, cx, cy), ref_sample_uv (src, 1, cx, cy), rgba);
        }
        rgba[3] = 255.0;
      } else {
        ref_sample_rgba (src, tx, ty, rgba);
        src_alpha = sa >= 0;
      }
      ref_put_rgb (dst, x, y, rgba, src_alpha);
    }
  }
}

#endif /* RDS_TRANSFORM_REF_H */
