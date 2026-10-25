/* rdstracker Kalman filter: against a plain double-precision reference written here with full matrices, convergence on
 * a constant-velocity object, the aspect-ratio state, the two noise modes and covariance growth without updates.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tracker-kf.h"

#include <math.h>
#include <string.h>

#define N 8

static RdsTrackerKfParams
params_height (guint n_states, gboolean aspect)
{
  RdsTrackerKfParams p = {.n_states = n_states,.use_aspect_ratio = aspect,.height_noise = TRUE,
    .weight_loc = 0.05,.weight_vel = 0.00625
  };

  return p;
}

static RdsTrackerKfParams
params_fixed (guint n_states, gboolean aspect)
{
  RdsTrackerKfParams p = {.n_states = n_states,.use_aspect_ratio = aspect,.process_loc = 2,.process_size = 1,
    .process_vel = 1,.measurement = 4
  };

  return p;
}

/* --- the reference: x' = F x, P' = F P F^T + Q; K = P H^T (H P H^T + R)^-1, x += K (z - H x), P = (I - K H) P --- */

typedef struct
{
  const RdsTrackerKfParams *prm;
  guint n;
  gdouble x[N], p[N][N];
} Ref;

static void
matmul (guint n, guint m, guint k, const gdouble * a, const gdouble * b, gdouble * c)
{
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < k; j++) {
      gdouble v = 0;

      for (guint l = 0; l < m; l++)
        v += a[i * m + l] * b[l * k + j];
      c[i * k + j] = v;
    }
}

static void
transpose (guint n, guint m, const gdouble * a, gdouble * t)
{
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < m; j++)
      t[j * n + i] = a[i * m + j];
}

/* Gauss-Jordan with partial pivoting */
static void
inverse (guint n, const gdouble * a, gdouble * inv)
{
  gdouble w[4][8];

  g_assert_cmpuint (n, ==, 4);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < 2 * n; j++)
      w[i][j] = j < n ? a[i * n + j] : (j - n == i);
  for (guint c = 0; c < n; c++) {
    guint best = c;

    for (guint i = c + 1; i < n; i++)
      if (fabs (w[i][c]) > fabs (w[best][c]))
        best = i;
    for (guint j = 0; j < 2 * n; j++) {
      gdouble t = w[c][j];

      w[c][j] = w[best][j];
      w[best][j] = t;
    }
    for (guint j = 2 * n; j-- > 0;)
      w[c][j] /= w[c][c];
    for (guint i = 0; i < n; i++)
      if (i != c)
        for (guint j = 2 * n; j-- > 0;)
          w[i][j] -= w[i][c] * w[c][j];
  }
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      inv[i * n + j] = w[i][n + j];
}

/* The standard deviations of the height-proportional model (rds-tracker-kf.c, with its initial scales 10 and 13),
 * per state: positions and size, then velocities. */
static gdouble
ref_std (const RdsTrackerKfParams * prm, guint i, gdouble h, gdouble loc_scale, gdouble vel_scale,
    gdouble aspect, gdouble aspect_vel)
{
  gboolean vel = i >= 4;

  if (prm->use_aspect_ratio && i % 4 == 2)
    return vel ? aspect_vel : aspect;
  return vel ? vel_scale * prm->weight_vel * h : loc_scale * prm->weight_loc * h;
}

static void
ref_q (const Ref * r, gdouble * q)
{
  memset (q, 0, sizeof (gdouble) * r->n * r->n);
  for (guint i = 0; i < r->n; i++) {
    gdouble v;

    if (r->prm->height_noise)
      v = pow (ref_std (r->prm, i, r->x[3], 1, 1, 1e-2, 1e-5), 2);
    else if (r->prm->use_aspect_ratio && i % 4 == 2)
      v = pow (i >= 4 ? 1e-5 : 1e-2, 2);
    else
      v = i >= 4 ? r->prm->process_vel : i < 2 ? r->prm->process_loc : r->prm->process_size;
    q[i * r->n + i] = v;
  }
}

static void
ref_r (const Ref * r, gdouble * rr)
{
  memset (rr, 0, sizeof (gdouble) * 16);
  for (guint i = 0; i < 4; i++) {
    gdouble v;

    if (r->prm->use_aspect_ratio && i == 2)
      v = 1e-2;
    else if (r->prm->height_noise)
      v = pow (r->prm->weight_loc * r->x[3], 2);
    else
      v = r->prm->measurement;
    rr[i * 4 + i] = v;
  }
}

static void
ref_z (const Ref * r, const gfloat * box, gdouble * z)
{
  z[0] = box[0];
  z[1] = box[1];
  z[2] = r->prm->use_aspect_ratio ? (gdouble) box[2] / box[3] : box[2];
  z[3] = box[3];
}

static void
ref_init (Ref * r, const RdsTrackerKfParams * prm, const gfloat * box)
{
  memset (r, 0, sizeof *r);
  r->prm = prm;
  r->n = prm->n_states;
  ref_z (r, box, r->x);
  for (guint i = 0; i < r->n; i++) {
    gdouble v;

    if (prm->height_noise)
      v = pow (ref_std (prm, i, box[3], 10, 13, 1e-2, 1e-5), 2);
    else if (prm->use_aspect_ratio && i % 4 == 2)
      v = pow (i >= 4 ? 1e-5 : 1e-2, 2);
    else
      v = i >= 4 ? 10 * prm->process_vel : prm->measurement;
    r->p[i][i] = v;
  }
}

static void
ref_f (const Ref * r, gdouble * f)
{
  memset (f, 0, sizeof (gdouble) * r->n * r->n);
  for (guint i = 0; i < r->n; i++) {
    f[i * r->n + i] = 1;
    if (i + 4 < r->n)
      f[i * r->n + i + 4] = 1;
  }
}

static void
ref_predict (Ref * r)
{
  guint n = r->n;
  gdouble f[N * N], ft[N * N], q[N * N], x[N], fp[N * N], p[N * N];

  ref_q (r, q);
  ref_f (r, f);
  transpose (n, n, f, ft);
  matmul (n, n, 1, f, r->x, x);
  memcpy (r->x, x, sizeof (gdouble) * n);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      p[i * n + j] = r->p[i][j];
  matmul (n, n, n, f, p, fp);
  matmul (n, n, n, fp, ft, p);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      r->p[i][j] = p[i * n + j] + q[i * n + j];
}

static void
ref_update (Ref * r, const gfloat * box)
{
  guint n = r->n;
  gdouble h[4 * N] = { 0 }, ht[N * 4], p[N * N], ph[N * 4], s[16], si[16], rr[16], k[N * 4], z[4], hx[4];
  gdouble kh[N * N], ikh[N * N], np[N * N];

  for (guint i = 0; i < 4; i++)
    h[i * n + i] = 1;
  transpose (4, n, h, ht);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      p[i * n + j] = r->p[i][j];
  ref_r (r, rr);
  matmul (n, n, 4, p, ht, ph);
  matmul (4, n, 4, h, ph, s);
  for (guint i = 0; i < 16; i++)
    s[i] += rr[i];
  inverse (4, s, si);
  matmul (n, 4, 4, ph, si, k);
  ref_z (r, box, z);
  matmul (4, n, 1, h, r->x, hx);
  for (guint i = 0; i < 4; i++)
    z[i] -= hx[i];
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < 4; j++)
      r->x[i] += k[i * 4 + j] * z[j];
  matmul (n, 4, n, k, h, kh);
  for (guint i = 0; i < n * n; i++)
    ikh[i] = (i % (n + 1) == 0) - kh[i];
  matmul (n, n, n, ikh, p, np);
  for (guint i = 0; i < n; i++)
    for (guint j = 0; j < n; j++)
      r->p[i][j] = np[i * n + j];
}

static void
assert_close (gdouble a, gdouble b, gdouble rel)
{
  if (fabs (a - b) > rel * MAX (1.0, fabs (b)))
    g_error ("%.12g differs from the reference %.12g", a, b);
}

static void
compare (const RdsTrackerKf * kf, const Ref * r)
{
  for (guint i = 0; i < r->n; i++) {
    assert_close (kf->x[i], r->x[i], 1e-9);
    for (guint j = 0; j < r->n; j++)
      assert_close (kf->p[i][j], r->p[i][j], 1e-9);
  }
}

/* Every variant (SIMPLE and REGULAR, w or a, both noise modes) on a jittery accelerating box with gaps. */
static void
test_reference (void)
{
  for (guint v = 0; v < 8; v++) {
    RdsTrackerKfParams prm = (v & 4) ? params_fixed (v & 1 ? 8 : 6, v & 2) : params_height (v & 1 ? 8 : 6, v & 2);
    GRand *rand = g_rand_new_with_seed (v);
    RdsTrackerKf kf;
    Ref ref;
    gfloat box[4] = { 50, 80, 60, 120 };

    rds_tracker_kf_init (&kf, &prm, box[0], box[1], box[2], box[3]);
    ref_init (&ref, &prm, box);
    compare (&kf, &ref);
    for (guint f = 1; f < 60; f++) {
      rds_tracker_kf_predict (&kf);
      ref_predict (&ref);
      compare (&kf, &ref);
      if (f % 7 == 3)
        continue;
      box[0] = 50 + 3 * f + 0.02f * f * f + (gfloat) g_rand_double_range (rand, -2, 2);
      box[1] = 80 - 1.5f * f + (gfloat) g_rand_double_range (rand, -2, 2);
      box[2] = 60 + 0.4f * f + (gfloat) g_rand_double_range (rand, -1, 1);
      box[3] = 120 + 0.6f * f + (gfloat) g_rand_double_range (rand, -1, 1);
      rds_tracker_kf_update (&kf, box[0], box[1], box[2], box[3]);
      ref_update (&ref, box);
      compare (&kf, &ref);
    }
    g_rand_free (rand);
  }
}

/* A constant-velocity box: after 10 updates the state is within 0.5 px, in every variant, and so is the next
 * prediction within 1 px. */
static void
test_converges (void)
{
  for (guint v = 0; v < 8; v++) {
    RdsTrackerKfParams prm = (v & 4) ? params_fixed (v & 1 ? 8 : 6, v & 2) : params_height (v & 1 ? 8 : 6, v & 2);
    RdsTrackerKf kf;
    gfloat x, y, w, h;
    /* SIMPLE has no size velocity: the size stays constant for it. The aspect ratio stays 0.5: its velocity is held
     * near 0 by its tiny noise. */
    gfloat dw = prm.n_states == 8 ? 0.25f : 0, dh = prm.n_states == 8 ? 0.5f : 0;

    rds_tracker_kf_init (&kf, &prm, 100, 200, 80, 160);
    for (guint f = 1; f <= 10; f++) {
      rds_tracker_kf_predict (&kf);
      rds_tracker_kf_update (&kf, 100 + 8 * f, 200 - 3 * f, 80 + dw * f, 160 + dh * f);
    }
    rds_tracker_kf_get_box (&kf, &x, &y, &w, &h);
    g_test_message ("variant %u: error %.3f %.3f %.3f %.3f", v, x - 180, y - 170, w - (80 + dw * 10),
        h - (160 + dh * 10));
    g_assert_cmpfloat (fabs (x - 180), <, 0.5);
    g_assert_cmpfloat (fabs (y - 170), <, 0.5);
    g_assert_cmpfloat (fabs (w - (80 + dw * 10)), <, 0.5);
    g_assert_cmpfloat (fabs (h - (160 + dh * 10)), <, 0.5);
    rds_tracker_kf_predict (&kf);
    rds_tracker_kf_get_box (&kf, &x, &y, &w, &h);
    g_assert_cmpfloat (fabs (x - 188), <, 1);
    g_assert_cmpfloat (fabs (y - 167), <, 1);
    g_assert_cmpfloat (fabs (w - (80 + dw * 11)), <, 1);
    g_assert_cmpfloat (fabs (h - (160 + dh * 11)), <, 1);
  }
}

/* With useAspectRatio the state holds w/h; the box gives w back. */
static void
test_aspect_ratio (void)
{
  RdsTrackerKfParams prm = params_height (8, TRUE);
  RdsTrackerKf kf;
  gfloat x, y, w, h;

  rds_tracker_kf_init (&kf, &prm, 10, 20, 37, 91);
  g_assert_cmpfloat (fabs (kf.x[2] - 37.0 / 91), <, 1e-12);
  rds_tracker_kf_get_box (&kf, &x, &y, &w, &h);
  g_assert_cmpfloat (x, ==, 10);
  g_assert_cmpfloat (y, ==, 20);
  g_assert_cmpfloat (fabs (w - 37), <, 1e-4);
  g_assert_cmpfloat (h, ==, 91);
  /* a steady box stays where it is */
  for (guint f = 0; f < 5; f++) {
    rds_tracker_kf_predict (&kf);
    rds_tracker_kf_update (&kf, 10, 20, 37, 91);
  }
  rds_tracker_kf_get_box (&kf, &x, &y, &w, &h);
  g_assert_cmpfloat (fabs (w - 37), <, 1e-3);
  g_assert_cmpfloat (fabs (h - 91), <, 1e-3);
}

/* Height-proportional and fixed noise give different filters; the height-proportional one scales with the box. */
static void
test_noise_modes (void)
{
  RdsTrackerKfParams ph = params_height (8, FALSE), pf = params_fixed (8, FALSE);
  RdsTrackerKf a, b, big;
  gfloat xa, xb, y, w, h;

  rds_tracker_kf_init (&a, &ph, 0, 0, 100, 100);
  rds_tracker_kf_init (&b, &pf, 0, 0, 100, 100);
  rds_tracker_kf_init (&big, &ph, 0, 0, 400, 400);
  g_assert_cmpfloat (fabs (a.p[0][0] - b.p[0][0]), >, 1);
  g_assert_cmpfloat (fabs (big.p[0][0] - 16 * a.p[0][0]), <, 1e-9 * big.p[0][0]);
  for (guint f = 1; f <= 3; f++) {
    rds_tracker_kf_predict (&a);
    rds_tracker_kf_predict (&b);
    rds_tracker_kf_update (&a, 10 * f, 0, 100, 100);
    rds_tracker_kf_update (&b, 10 * f, 0, 100, 100);
  }
  rds_tracker_kf_predict (&a);
  rds_tracker_kf_predict (&b);
  rds_tracker_kf_get_box (&a, &xa, &y, &w, &h);
  rds_tracker_kf_get_box (&b, &xb, &y, &w, &h);
  g_test_message ("prediction after 3 updates: height noise %.3f, fixed noise %.3f", xa, xb);
  g_assert_cmpfloat (fabs (xa - xb), >, 0.1);
}

/* Predictions without updates grow the covariance on every diagonal entry; an update shrinks the measured ones. */
static void
test_covariance_grows (void)
{
  for (guint v = 0; v < 2; v++) {
    RdsTrackerKfParams prm = v ? params_fixed (6, FALSE) : params_height (8, TRUE);
    RdsTrackerKf kf;
    gdouble prev[N];

    rds_tracker_kf_init (&kf, &prm, 0, 0, 50, 100);
    for (guint i = 0; i < prm.n_states; i++)
      prev[i] = kf.p[i][i];
    for (guint f = 0; f < 5; f++) {
      rds_tracker_kf_predict (&kf);
      for (guint i = 0; i < prm.n_states; i++) {
        g_assert_cmpfloat (kf.p[i][i], >, prev[i]);
        prev[i] = kf.p[i][i];
      }
    }
    rds_tracker_kf_update (&kf, 0, 0, 50, 100);
    for (guint i = 0; i < 4; i++)
      g_assert_cmpfloat (kf.p[i][i], <, prev[i]);
  }
}

static void
test_params_from_config (void)
{
  GError *err = NULL;
  RdsTrackerConfig *cfg = rds_tracker_config_load (NULL, &err);
  RdsTrackerKfParams prm;

  g_assert_no_error (err);
  g_assert_false (rds_tracker_kf_params_from_config (&prm, cfg));
  cfg->state_estimator_type = 1;
  g_assert_true (rds_tracker_kf_params_from_config (&prm, cfg));
  g_assert_cmpuint (prm.n_states, ==, 6);
  g_assert_false (prm.height_noise);
  cfg->state_estimator_type = 2;
  cfg->noise_weight_var4loc = 0.05f;
  g_assert_true (rds_tracker_kf_params_from_config (&prm, cfg));
  g_assert_cmpuint (prm.n_states, ==, 8);
  /* both weights must be set */
  g_assert_false (prm.height_noise);
  cfg->noise_weight_var4vel = 0.00625f;
  cfg->use_aspect_ratio = TRUE;
  g_assert_true (rds_tracker_kf_params_from_config (&prm, cfg));
  g_assert_true (prm.height_noise);
  g_assert_true (prm.use_aspect_ratio);
  rds_tracker_config_free (cfg);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/tracker-kf/reference", test_reference);
  g_test_add_func ("/cpu/tracker-kf/converges", test_converges);
  g_test_add_func ("/cpu/tracker-kf/aspect-ratio", test_aspect_ratio);
  g_test_add_func ("/cpu/tracker-kf/noise-modes", test_noise_modes);
  g_test_add_func ("/cpu/tracker-kf/covariance-grows", test_covariance_grows);
  g_test_add_func ("/cpu/tracker-kf/params-from-config", test_params_from_config);
  return g_test_run ();
}
