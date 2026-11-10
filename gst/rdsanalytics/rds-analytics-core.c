/* Redstream: the rdsanalytics core. Geometry: rule scaling, the reference point of a box and the inside test.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-core.h"

#include <math.h>

/* A double to an int, clamped to the int range, NaN as 0 (ours: such inputs are undefined behaviour for a cast). */
static gint
to_int (gdouble v)
{
  if (isnan (v))
    return 0;
  return (gint) CLAMP (v, (gdouble) G_MININT, (gdouble) G_MAXINT);
}

/* DeepStream 8.0 behaviour: each vertex is scaled per axis and truncated, (int)(v * out / cfg). In 64-bit integers
 * the quotient truncates toward zero exactly as the cast does, without float rounding (ours: overflow-free). */
static gint
scale_coord (gint v, guint cfg, guint out)
{
  gint64 s = (gint64) v * (gint64) out / (gint64) cfg;

  return (gint) CLAMP (s, (gint64) G_MININT, (gint64) G_MAXINT);
}

void
rds_analytics_scale_points (const RdsAnalyticsPoint * in, guint n, guint cfg_w, guint cfg_h, guint out_w,
    guint out_h, RdsAnalyticsPoint * out)
{
  g_return_if_fail (cfg_w > 0 && cfg_h > 0);
  g_return_if_fail (n == 0 || (in != NULL && out != NULL));

  for (guint i = 0; i < n; i++) {
    RdsAnalyticsPoint p = in[i];

    out[i].x = scale_coord (p.x, cfg_w, out_w);
    out[i].y = scale_coord (p.y, cfg_h, out_h);
  }
}

/* DeepStream 8.0 behaviour: the bottom centre, floored to integers. The sums are in float, the box meta's type
 * (ours: 8.0's precision is unknown; the probe's boxes give the same points either way). */
RdsAnalyticsPoint
rds_analytics_ref_point (gfloat left, gfloat top, gfloat width, gfloat height)
{
  gfloat x = left + width / 2.0f;
  gfloat y = top + height;
  RdsAnalyticsPoint p = { to_int (floorf (x)), to_int (floorf (y)) };

  return p;
}

/* DeepStream 8.0 behaviour: the crossing number on integer coordinates. An edge (i, j) toggles when it straddles
 * the point's row, (yi > Py) != (yj > Py), and the point is left of the crossing x truncated to an int,
 * Px < (int) xc. The y test makes the bottom edge (and vertices) half-open and the truncated crossing makes the
 * right edge exclusive, so a triangle's apex row has no inside pixel. The crossing is computed in double from
 * integers: an exact integer crossing stays exact, so the vertex order can't change the result. */
gboolean
rds_analytics_point_in_polygon (RdsAnalyticsPoint p, const RdsAnalyticsPoint * poly, guint n)
{
  gboolean in = FALSE;

  if (n < 3)
    return FALSE;
  g_return_val_if_fail (poly != NULL, FALSE);

  for (guint i = 0, j = n - 1; i < n; j = i++) {
    gdouble xi = poly[i].x, yi = poly[i].y, xj = poly[j].x, yj = poly[j].y;

    if ((poly[i].y > p.y) != (poly[j].y > p.y)) {
      gint xc = to_int ((xj - xi) * ((gdouble) p.y - yi) / (yj - yi) + xi);

      if (p.x < xc)
        in = !in;
    }
  }
  return in;
}
