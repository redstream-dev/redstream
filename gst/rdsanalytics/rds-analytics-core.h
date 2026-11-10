/* Redstream: the rdsanalytics core (plugin-internal, not installed). Plain C with GLib only: no GStreamer, no GPU.
 *
 * Geometry (DeepStream 8.0 behaviour): rules are written in the config's config-width x config-height space and
 * scaled to the mux output size, x and y separately, each scaled vertex truncated to an integer. An object is
 * represented by one integer point, its bottom centre floored; the rest of its box is ignored. A point is inside a
 * polygon by the crossing-number test on those integers, which is even-odd for self-intersecting polygons, includes
 * the left and top edges and excludes the right and bottom ones, and doesn't depend on the vertex order.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ANALYTICS_CORE_H
#define RDS_ANALYTICS_CORE_H

#include "rds-analytics-types.h"

G_BEGIN_DECLS

/* Scales @n points from the config space (@cfg_w x @cfg_h, both > 0) to the mux output (@out_w x @out_h): each
 * coordinate becomes (int)(v * out / cfg), per axis. In practice v >= 0: the config parser rejects negative
 * coordinates. @out may be @in. */
G_GNUC_INTERNAL void rds_analytics_scale_points (const RdsAnalyticsPoint * in, guint n, guint cfg_w, guint cfg_h,
    guint out_w, guint out_h, RdsAnalyticsPoint * out);

/* The point that stands for a box (left, top, width, height in frame pixels): its bottom centre, floored,
 * (floor (left + width / 2), floor (top + height)). */
G_GNUC_INTERNAL RdsAnalyticsPoint rds_analytics_ref_point (gfloat left, gfloat top, gfloat width, gfloat height);

/* Whether @p is inside the polygon of @n vertices @poly (closed: the last vertex joins the first). Fewer than 3
 * vertices contain nothing. */
G_GNUC_INTERNAL gboolean rds_analytics_point_in_polygon (RdsAnalyticsPoint p, const RdsAnalyticsPoint * poly,
    guint n);

G_END_DECLS

#endif /* RDS_ANALYTICS_CORE_H */
