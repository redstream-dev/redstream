/* Redstream: the rdstracker Kalman filter. Indices 0-3 are x, y, w|a, h; 4 and up are their velocities (dx, dy for
 * SIMPLE; dx, dy, dw|da, dh for REGULAR). The DeepStream docs describe the state and the noise options, not the
 * matrices: the noise model below follows DeepSORT's published formulation, and its constants are ours.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tracker-kf.h"

#include <string.h>

#define N_MEAS 4
#define IDX_SIZE 2              /* w, or a with useAspectRatio */
#define IDX_H 3

/* Ours, following DeepSORT: an aspect ratio doesn't scale with the box height, so its terms are small constant
 * standard deviations, in both noise modes. */
#define STD_ASPECT_PROCESS 1e-2
#define STD_ASPECT_VEL_PROCESS 1e-5
#define STD_ASPECT_MEASUREMENT 1e-1
#define STD_ASPECT_INIT 1e-2
#define STD_ASPECT_VEL_INIT 1e-5

/* Ours: the initial standard deviations in height-proportional mode, as multiples of weight_loc·h (positions and
 * size) and weight_vel·h (velocities). DeepSORT uses 2 and 10; with those, the predictions of a constant-velocity
 * object between detections are up to 2 px off observed DeepStream 8.0 output, and 10 and 13 keep them within 1 px
 * on the first frames and within 1.4 px after. */
#define INIT_SCALE_LOC 10.0
#define INIT_SCALE_VEL 13.0

/* Ours: the initial velocity variance in fixed-noise mode, as a multiple of processNoiseVar4Vel (the positions and
 * size start at the measurement noise). */
#define INIT_SCALE_FIXED_VEL 10.0

gboolean
rds_tracker_kf_params_from_config (RdsTrackerKfParams * params, const RdsTrackerConfig * cfg)
{
  *params = (RdsTrackerKfParams) {
    0
  };
  if (cfg->state_estimator_type == 0)
    return FALSE;
  /* the config loader reads 3 (SIMPLE_LOC) as 2 */
  params->n_states = cfg->state_estimator_type == 1 ? 6 : 8;
  params->use_aspect_ratio = cfg->use_aspect_ratio;
  params->height_noise = cfg->noise_weight_var4loc > 0 && cfg->noise_weight_var4vel > 0;
  params->weight_loc = cfg->noise_weight_var4loc;
  params->weight_vel = cfg->noise_weight_var4vel;
  params->process_loc = cfg->process_noise_var4loc;
  params->process_size = cfg->process_noise_var4size;
  params->process_vel = cfg->process_noise_var4vel;
  params->measurement = cfg->measurement_noise_var4detector;
  return TRUE;
}

static gdouble
sq (gdouble v)
{
  return v * v;
}

/* The diagonal of the process noise for a box of height @h. */
static void
process_noise (const RdsTrackerKfParams * prm, gdouble h, gdouble * q)
{
  for (guint i = 0; i < prm->n_states; i++) {
    gboolean vel = i >= N_MEAS;
    guint base = vel ? i - N_MEAS : i;

    if (prm->use_aspect_ratio && base == IDX_SIZE)
      q[i] = sq (vel ? STD_ASPECT_VEL_PROCESS : STD_ASPECT_PROCESS);
    else if (prm->height_noise)
      /* DeepSORT: w is treated like h when there is no aspect ratio */
      q[i] = sq ((vel ? prm->weight_vel : prm->weight_loc) * h);
    else
      q[i] = vel ? prm->process_vel : base < IDX_SIZE ? prm->process_loc : prm->process_size;
  }
}

/* The diagonal of the measurement noise for a box of height @h. */
static void
measurement_noise (const RdsTrackerKfParams * prm, gdouble h, gdouble * r)
{
  for (guint i = 0; i < N_MEAS; i++) {
    if (prm->use_aspect_ratio && i == IDX_SIZE)
      r[i] = sq (STD_ASPECT_MEASUREMENT);
    else if (prm->height_noise)
      r[i] = sq (prm->weight_loc * h);
    else
      r[i] = prm->measurement;
  }
}

static void
measure (const RdsTrackerKfParams * prm, gfloat x, gfloat y, gfloat w, gfloat h, gdouble * z)
{
  z[0] = x;
  z[1] = y;
  z[IDX_SIZE] = prm->use_aspect_ratio ? (gdouble) w / h : w;
  z[IDX_H] = h;
}

void
rds_tracker_kf_init (RdsTrackerKf * kf, const RdsTrackerKfParams * params, gfloat x, gfloat y, gfloat w, gfloat h)
{
  gdouble r[N_MEAS];

  g_return_if_fail (params->n_states == 6 || params->n_states == 8);
  memset (kf, 0, sizeof *kf);
  kf->params = params;
  measure (params, x, y, w, h, kf->x);
  measurement_noise (params, h, r);
  for (guint i = 0; i < params->n_states; i++) {
    gboolean vel = i >= N_MEAS;
    guint base = vel ? i - N_MEAS : i;
    gdouble v;

    if (params->use_aspect_ratio && base == IDX_SIZE)
      v = sq (vel ? STD_ASPECT_VEL_INIT : STD_ASPECT_INIT);
    else if (params->height_noise)
      v = sq ((vel ? INIT_SCALE_VEL * params->weight_vel : INIT_SCALE_LOC * params->weight_loc) * h);
    else
      v = vel ? INIT_SCALE_FIXED_VEL * params->process_vel : r[i];
    kf->p[i][i] = v;
  }
}

void
rds_tracker_kf_predict (RdsTrackerKf * kf)
{
  const RdsTrackerKfParams *prm = kf->params;
  guint n = prm->n_states, n_vel = n - N_MEAS;
  gdouble q[RDS_TRACKER_KF_MAX_STATES];
  gdouble fp[RDS_TRACKER_KF_MAX_STATES][RDS_TRACKER_KF_MAX_STATES];

  process_noise (prm, kf->x[IDX_H], q);
  /* F = I plus 1 at (i, i + 4) for every velocity: x' = F x, P' = F P F^T + Q */
  for (guint i = 0; i < n_vel; i++)
    kf->x[i] += kf->x[i + N_MEAS];
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      fp[i][j] = kf->p[i][j] + (i < n_vel ? kf->p[i + N_MEAS][j] : 0);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      kf->p[i][j] = fp[i][j] + (j < n_vel ? fp[i][j + N_MEAS] : 0);
  for (guint i = 0; i < n; i++)
    kf->p[i][i] += q[i];
}

/* Inverts the symmetric positive definite 4x4 @s in place by Gauss-Jordan elimination (no pivoting needed). */
static void
invert4 (gdouble s[N_MEAS][N_MEAS])
{
  gdouble a[N_MEAS][2 * N_MEAS] = { {0} };

  for (guint i = 0; i < N_MEAS; i++) {
    for (guint j = 0; j < N_MEAS; j++)
      a[i][j] = s[i][j];
    a[i][N_MEAS + i] = 1;
  }
  for (guint c = 0; c < N_MEAS; c++) {
    gdouble piv = a[c][c];

    for (guint j = 0; j < 2 * N_MEAS; j++)
      a[c][j] /= piv;
    for (guint i = 0; i < N_MEAS; i++) {
      gdouble f = a[i][c];

      if (i == c || f == 0)
        continue;
      for (guint j = 0; j < 2 * N_MEAS; j++)
        a[i][j] -= f * a[c][j];
    }
  }
  for (guint i = 0; i < N_MEAS; i++)
    for (guint j = 0; j < N_MEAS; j++)
      s[i][j] = a[i][N_MEAS + j];
}

void
rds_tracker_kf_update (RdsTrackerKf * kf, gfloat x, gfloat y, gfloat w, gfloat h)
{
  const RdsTrackerKfParams *prm = kf->params;
  guint n = prm->n_states;
  gdouble z[N_MEAS], r[N_MEAS], s[N_MEAS][N_MEAS], k[RDS_TRACKER_KF_MAX_STATES][N_MEAS];
  gdouble hp[N_MEAS][RDS_TRACKER_KF_MAX_STATES];

  measure (prm, x, y, w, h, z);
  measurement_noise (prm, kf->x[IDX_H], r);
  /* H selects the first four states: S = H P H^T + R, K = P H^T S^-1 */
  for (guint i = 0; i < N_MEAS; i++)
    for (guint j = 0; j < N_MEAS; j++)
      s[i][j] = kf->p[i][j] + (i == j ? r[i] : 0);
  invert4 (s);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < N_MEAS; j++) {
      gdouble v = 0;

      for (guint m = 0; m < N_MEAS; m++)
        v += kf->p[i][m] * s[m][j];
      k[i][j] = v;
    }
  /* x += K (z - H x), P -= K H P */
  for (guint j = 0; j < N_MEAS; j++)
    z[j] -= kf->x[j];
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < N_MEAS; j++)
      kf->x[i] += k[i][j] * z[j];
  for (guint i = 0; i < N_MEAS; i++)
    for (guint j = 0; j < n; j++)
      hp[i][j] = kf->p[i][j];
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      for (guint m = 0; m < N_MEAS; m++)
        kf->p[i][j] -= k[i][m] * hp[m][j];
  /* keep P symmetric against rounding */
  for (guint i = 0; i < n; i++)
    for (guint j = i + 1; j < n; j++)
      kf->p[i][j] = kf->p[j][i] = (kf->p[i][j] + kf->p[j][i]) / 2;
}

void
rds_tracker_kf_get_box (const RdsTrackerKf * kf, gfloat * x, gfloat * y, gfloat * w, gfloat * h)
{
  *x = kf->x[0];
  *y = kf->x[1];
  *w = kf->params->use_aspect_ratio ? kf->x[IDX_SIZE] * kf->x[IDX_H] : kf->x[IDX_SIZE];
  *h = kf->x[IDX_H];
}
