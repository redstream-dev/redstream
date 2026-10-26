/* Redstream: the rdstracker Kalman filter (plugin-internal, not installed), NvSORT's state estimator. Constant
 * velocity with a time step of one frame. The state is the box's top-left x, y, then w (or the aspect ratio a = w/h
 * with useAspectRatio), h, then the velocities: dx, dy for SIMPLE (6 states), dx, dy, dw|da, dh for REGULAR (8). The
 * measurement is x, y, w|a, h. Plain C, double precision, fixed-size matrices.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TRACKER_KF_H
#define RDS_TRACKER_KF_H

#include "rds-tracker-config.h"

G_BEGIN_DECLS

#define RDS_TRACKER_KF_MAX_STATES 8

/* The noise model, shared by every filter of a tracker. */
typedef struct
{
  guint n_states;               /* 6 (SIMPLE) or 8 (REGULAR) */
  gboolean use_aspect_ratio;
  /* noiseWeightVar4Loc and noiseWeightVar4Vel both > 0: noise proportional to the box height (DeepSORT style) */
  gboolean height_noise;
  gdouble weight_loc, weight_vel;
  /* otherwise the fixed variances */
  gdouble process_loc, process_size, process_vel, measurement;
} RdsTrackerKfParams;

typedef struct
{
  const RdsTrackerKfParams *params;
  gdouble x[RDS_TRACKER_KF_MAX_STATES];
  gdouble p[RDS_TRACKER_KF_MAX_STATES][RDS_TRACKER_KF_MAX_STATES];
} RdsTrackerKf;

/* FALSE when the config has no state estimator (stateEstimatorType 0). */
G_GNUC_INTERNAL gboolean rds_tracker_kf_params_from_config (RdsTrackerKfParams * params,
    const RdsTrackerConfig * cfg);

/* Starts a filter on a box with zero velocity. @params must outlive @kf. */
G_GNUC_INTERNAL void rds_tracker_kf_init (RdsTrackerKf * kf, const RdsTrackerKfParams * params, gfloat x, gfloat y,
    gfloat w, gfloat h);
/* One frame ahead. The height never goes below a small positive value. */
G_GNUC_INTERNAL void rds_tracker_kf_predict (RdsTrackerKf * kf);
/* Corrects the state with a measured box; does nothing when the innovation covariance is (nearly) singular. */
G_GNUC_INTERNAL void rds_tracker_kf_update (RdsTrackerKf * kf, gfloat x, gfloat y, gfloat w, gfloat h);
/* The current state as a box (after rds_tracker_kf_predict, the prediction). */
G_GNUC_INTERNAL void rds_tracker_kf_get_box (const RdsTrackerKf * kf, gfloat * x, gfloat * y, gfloat * w,
    gfloat * h);

G_END_DECLS

#endif /* RDS_TRACKER_KF_H */
