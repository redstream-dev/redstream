/* Redstream: the rdsanalytics rule types shared by the config parser and the analytics core (plugin-internal, not
 * installed). Coordinates are in the config's config-width x config-height space; the core scales them to the mux
 * output.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ANALYTICS_TYPES_H
#define RDS_ANALYTICS_TYPES_H

#include <glib.h>

G_BEGIN_DECLS

/* The four features, one config group kind each: [<name>-stream-<n>]. */
typedef enum
{
  RDS_ANALYTICS_ROI_FILTERING,
  RDS_ANALYTICS_OVERCROWDING,
  RDS_ANALYTICS_LINE_CROSSING,
  RDS_ANALYTICS_DIRECTION,
  RDS_ANALYTICS_N_FEATURES,
} RdsAnalyticsFeature;

/* Line-crossing mode: how closely the motion must follow the direction vector. Unset or unknown values are loose
 * (DeepStream 8.0 behaviour). */
typedef enum
{
  RDS_ANALYTICS_MODE_LOOSE,
  RDS_ANALYTICS_MODE_BALANCED,
  RDS_ANALYTICS_MODE_STRICT,
} RdsAnalyticsMode;

typedef struct
{
  gint x, y;
} RdsAnalyticsPoint;

/* One labelled rule, from a roi-<label>, line-crossing-<label> or direction-<label> key. */
typedef struct
{
  gchar *label;
  /* ROI: the polygon's vertices in order (>= 3). Direction: 2, the vector's start and end. Line crossing: 4, the
   * direction vector's start and end, then the line's two ends. */
  RdsAnalyticsPoint *points;
  guint n_points;
} RdsAnalyticsRule;

/* One [<feature>-stream-<n>] group. Fields that don't apply to the feature keep their defaults. */
typedef struct
{
  RdsAnalyticsFeature feature;
  guint pad_index;              /* <n>: the frame meta's pad_index (DeepStream 8.0 behaviour) */
  gboolean enable;
  GArray *class_ids;            /* gint; NULL: every class (unset, empty, or a list holding -1) */
  gboolean inverse_roi;         /* ROI filtering */
  guint object_threshold;       /* overcrowding */
  guint time_threshold;         /* overcrowding: accepted, no effect (DeepStream 8.0 behaviour) */
  RdsAnalyticsMode mode;        /* line crossing */
  gboolean extended;            /* line crossing */
  GPtrArray *rules;             /* RdsAnalyticsRule *, in config order; never empty */
} RdsAnalyticsGroup;

/* The groups of one stream; NULL for a feature it has no group for. */
typedef struct
{
  guint pad_index;
  RdsAnalyticsGroup *groups[RDS_ANALYTICS_N_FEATURES];
} RdsAnalyticsStream;

G_END_DECLS

#endif /* RDS_ANALYTICS_TYPES_H */
