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

#include "rds-analytics-config.h"

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

/* ------------------------------------------------------------------------------------------------------------ */
/* The analytics core: per-stream state keyed by the frame meta's pad_index, fed one frame at a time.
 *
 * ROI filtering (DeepStream 8.0 behaviour): an object passing the group's class-id filter gets the label of every
 * ROI its point is inside, or outside with inverse-roi, and is counted for that label. Overcrowding (DeepStream 8.0
 * behaviour): the same count without inverse, and the status is count >= object-threshold && count > 0, on the
 * frame itself. A group with enable=0 contributes nothing, not even its zero entries (DeepStream 8.0 behaviour).
 *
 * History (DeepStream 8.0 behaviour): a stream with a direction or line-crossing group keeps a history per object
 * id, one point per frame the object is in (the frames it misses don't count), its x clamped at 0. The window is
 * its last 50 points, and n = observations - 1. The reference is the integer-truncated mean of the window's oldest
 * 2 points, or its oldest 4 once the window is full.
 *
 * Direction (DeepStream 8.0 behaviour):
 * - From n = 50 on, an object whose window has population standard deviations sx <= 20 and sy <= 0.1 * sqrt (h),
 *   h the current box height (ours: 8.0's h is undetermined), is STOPPED: " STOPPED " in objStatus, no direction.
 * - Otherwise, from n = 10 on, with d = newest point - reference and |d| >= 55 px: every direction rule whose vector
 *   is within 60 degrees of d (cos >= 0.5), in config order.
 * - Untracked objects get neither (ours: 8.0 turns direction and line crossing off for the stream for good).
 *
 * Line crossing (DeepStream 8.0 behaviour), per line rule (the direction vector d1 -> d2, then the line l1 - l2) and
 * per object passing the group's class-id filter, until the object has been counted on that line:
 * - "past" is the side of the line the vector points to, "before" the other one; the sides are exact on the integer
 *   points, so a point on a slanted line is on it (ours; see line_crossed);
 * - from n = 3 (loose), 10 (balanced) or 20 (strict) on, with ref the history's reference and d = newest - ref:
 *   ref strictly before the line, the newest point on or past it (loose) or strictly past it (balanced, strict);
 * - the angle between d and the vector under 90 degrees (loose), under 60 (balanced) or under 36.87, cos > 0.8
 *   (strict; ours within 8.0's measured range), and |d| >= 32, 55 or 78 px;
 * - with extended=0, the crossing hits the segment l1 - l2 lengthened by half the current box width at each end,
 *   strictly; with extended=1, anywhere on the infinite line.
 * The object is then counted once on that line: lcStatus and the " LC:<l> " piece on that frame only, and the line's
 * counts. There is no STOPPED gate. Untracked objects are skipped (ours). */

/* The object id of an object no tracker has given an id: DeepStream's UNTRACKED_OBJECT_ID. */
#define RDS_ANALYTICS_UNTRACKED G_MAXUINT64
/* A frame without a PTS (GStreamer's GST_CLOCK_TIME_NONE). */
#define RDS_ANALYTICS_PTS_NONE G_MAXUINT64

typedef struct
{
  guint64 id;                   /* the tracking id, or RDS_ANALYTICS_UNTRACKED */
  gint class_id;
  gfloat left, top, width, height;      /* frame pixels, the mux output's */
} RdsAnalyticsObject;

/* One objInROIcnt entry. */
typedef struct
{
  const gchar *label;
  RdsAnalyticsFeature feature;  /* RDS_ANALYTICS_ROI_FILTERING or RDS_ANALYTICS_OVERCROWDING */
  guint count;
} RdsAnalyticsRoiCount;

/* One ocStatus entry. */
typedef struct
{
  const gchar *label;
  guint count;
  gboolean status;
} RdsAnalyticsOcStatus;

/* One line's objLCCurrCnt and objLCCumCnt entries. */
typedef struct
{
  const gchar *label;
  guint64 current;              /* the objects counted on this frame */
  guint64 cumulative;           /* the objects counted on this stream so far (kept by a stream reset: ours) */
} RdsAnalyticsLcCount;

/* One objCnt entry. */
typedef struct
{
  gint class_id;
  guint count;
} RdsAnalyticsClassCount;

typedef struct
{
  /* roiStatus: the ROI-filtering labels, in config order */
  const gchar *const *roi_labels;
  guint n_roi_labels;
  /* ocStatus: the overcrowding labels the object is in, whatever the status, in config order */
  const gchar *const *oc_labels;
  guint n_oc_labels;
  /* lcStatus: the lines the object is counted on, on this frame only, in config order */
  const gchar *const *lc_labels;
  guint n_lc_labels;
  /* dirStatus: " DIR:<l> " per matching direction rule, in config order; "" when none */
  const gchar *dir_status;
  /* the object is STOPPED (dir_status is then "") */
  gboolean stopped;
  /* objStatus: " ROI:<l>" per ROI label, then " DIR:<l> " per direction, then " LC:<l> " per line counted on this
   * frame, then " STOPPED " (DeepStream 8.0 behaviour); "" when empty */
  const gchar *obj_status;
  /* the object has something to report (a label, a direction, a line crossing or STOPPED): it gets an object meta (DeepStream 8.0
   * behaviour) */
  gboolean has_info;
} RdsAnalyticsObjectResult;

/* Owned by the core: valid only until the next rds_analytics_core_process_frame (of any stream) or
 * rds_analytics_core_free. So a caller writes everything it needs from a frame's result, its metas and its display
 * meta included, before processing the next frame. Labels are the core's copies. Every array is in config order:
 * groups in the order of RdsAnalyticsFeature, rules in the order of their keys. */
typedef struct
{
  /* objInROIcnt: every enabled ROI-filtering label, then every enabled overcrowding label, zeros included */
  const RdsAnalyticsRoiCount *roi_counts;
  guint n_roi_counts;
  /* ocStatus: every enabled overcrowding label */
  const RdsAnalyticsOcStatus *oc;
  guint n_oc;
  /* objLCCurrCnt and objLCCumCnt: every enabled line, zeros included */
  const RdsAnalyticsLcCount *lc;
  guint n_lc;
  /* objCnt: every class seen on the stream so far, sorted by the class id as unsigned, so -1 comes last (DeepStream
   * 8.0 behaviour: the order of its display-obj-cnt text); see rds_analytics_core_new */
  const RdsAnalyticsClassCount *obj_cnt;
  guint n_obj_cnt;
  /* one per input object, in input order */
  const RdsAnalyticsObjectResult *objects;
  guint n_objects;
  /* TRUE on the first frame of the stream where direction or line crossing skipped an untracked object, FALSE
   * after: the element logs it once per stream (ours) */
  gboolean untracked_warning;
} RdsAnalyticsFrameResult;

typedef struct _RdsAnalyticsCore RdsAnalyticsCore;

/* Copies what it needs from @cfg, the rules scaled to @out_w x @out_h, the mux output.
 *
 * objCnt is, per class, the mode of the class's per-frame object counts over the frames of the stream whose PTS is
 * within cfg->obj_cnt_win_in_ms of the current one, both ends included (DeepStream 8.0 behaviour), counting every
 * object whatever the groups and filters. A frame where a seen class is absent counts 0 for it; frames before a
 * class was first seen count 0 too (ours). Ties go to the value seen most recently (ours). The window holds at most
 * 9000 frames, and a PTS going backwards or missing restarts it (ours). */
G_GNUC_INTERNAL RdsAnalyticsCore *rds_analytics_core_new (const RdsAnalyticsConfig * cfg, guint out_w,
    guint out_h);
G_GNUC_INTERNAL void rds_analytics_core_free (RdsAnalyticsCore * core);

/* Runs the analytics on one frame of stream @pad_index, with PTS @pts in nanoseconds (or RDS_ANALYTICS_PTS_NONE),
 * and fills @out. A new pad_index gets fresh state. A stream without groups still gets objCnt. */
G_GNUC_INTERNAL void rds_analytics_core_process_frame (RdsAnalyticsCore * core, guint pad_index, guint64 pts,
    const RdsAnalyticsObject * objs, guint n, RdsAnalyticsFrameResult * out);

G_END_DECLS

#endif /* RDS_ANALYTICS_CORE_H */
