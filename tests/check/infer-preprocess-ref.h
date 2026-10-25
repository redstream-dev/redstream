/* CPU reference for rdsinfer's preprocessing (rds-infer-preprocess.h), built on transform-ref.h's sampling: the
 * source resized into the scaled rect as rds_buf_surf_transform resizes it, then normalised. Test-only.
 *
 * The reference keeps the sampled values unrounded (double), so a test can tell which output bytes the GPU may
 * legitimately round either way: the kernel computes in float (bilinear positions and weights included), so a
 * value within a small delta of a rounding boundary may land on the neighbouring byte.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_PREPROCESS_REF_H
#define RDS_INFER_PREPROCESS_REF_H

#include "transform-ref.h"

/* Unrounded R, G, B and gray (GRAY8 output: luma for YUV and GRAY8 sources, BT.601 intensity for RGB ones) of
 * pixel (x, y) of the rect (dl, dt, dw, dh), the whole source resized into it. */
static inline void
ref_pre_sample (const RefImage * src, guint x, guint y, guint dl, guint dt, guint dw, guint dh, gboolean bilinear,
    double rgb[3], double *gray)
{
  gint64 nx = (2 * ((gint64) x - dl) + 1) * (gint64) src->width;
  gint64 ny = (2 * ((gint64) y - dt) + 1) * (gint64) src->height;
  RefTap tx = ref_tap (nx, 2 * (gint64) dw, 0, (gint64) src->width - 1, bilinear);
  RefTap ty = ref_tap (ny, 2 * (gint64) dh, 0, (gint64) src->height - 1, bilinear);
  RefKind k = ref_kind (src->format);

  if (k == REF_KIND_RGB) {
    double rgba[4];

    ref_sample_rgba (src, tx, ty, rgba);
    rgb[0] = rgba[0], rgb[1] = rgba[1], rgb[2] = rgba[2];
    *gray = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2];
    return;
  }
  *gray = ref_sample (src, 0, 1, 0, tx, ty);
  if (k == REF_KIND_GRAY) {
    rgb[0] = rgb[1] = rgb[2] = *gray;
  } else {
    RefTap cx = ref_tap_chroma_of (nx, 2 * (gint64) dw, 0, src->width, bilinear);
    RefTap cy = ref_tap_chroma_of (ny, 2 * (gint64) dh, 0, src->height, bilinear);

    ref_yuv_to_rgb (*gray, ref_sample_uv (src, 0, cx, cy), ref_sample_uv (src, 1, cx, cy), rgb);
  }
}

/* Distance of the GPU's normalised value @got from the closest value the reference allows for an unrounded sample
 * @v: scale * (b - offset) for every byte b that v - delta .. v + delta rounds to. *alt is set when the closest
 * byte isn't the one v itself rounds to. */
static inline double
ref_pre_diff (float got, double v, double delta, float scale, float offset, gboolean * alt)
{
  guint8 lo = ref_store (v - delta), hi = ref_store (v + delta), mid = ref_store (v);
  double best = G_MAXDOUBLE;
  guint8 best_b = mid;

  for (guint b = lo; b <= hi; b++) {
    double d = fabs ((double) got - (double) scale * ((double) b - (double) offset));

    if (d < best || (d == best && b == mid))
      best = d, best_b = (guint8) b;
  }
  *alt = best_b != mid;
  return best;
}

#endif /* RDS_INFER_PREPROCESS_REF_H */
