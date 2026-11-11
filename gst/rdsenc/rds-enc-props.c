/* Redstream: the property mapping of rdsh264enc and rdsh265enc, from DeepStream 8.0's nvv4l2h264enc / nvv4l2h265enc
 * property values to the stock VA encoders' properties, and the QP string parsers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-props.h"

#include <string.h>

G_DEFINE_QUARK (rds-enc-error-quark, rds_enc_error)

guint
rds_enc_kbps (guint bitrate_bps)
{
  /* 64-bit: bps + 500 overflows a guint near the top of the range */
  guint64 kbps = ((guint64) bitrate_bps + 500) / 1000;

  return (guint) MAX (kbps, 1);
}

guint
rds_enc_target_percentage (guint bitrate, guint maxbitrate, gboolean vbr)
{
  guint64 pct;

  if (!vbr || maxbitrate <= bitrate)
    return 0;
  pct = (guint64) bitrate * 100 / maxbitrate;   /* rounded down; < 100 since bitrate < maxbitrate */
  return (guint) CLAMP (pct, 1, 100);
}

guint
rds_enc_key_int_max (guint idrinterval, gboolean * capped)
{
  gboolean cap = idrinterval > RDS_ENC_KEY_INT_MAX_CAP;

  if (capped)
    *capped = cap;
  if (idrinterval == 0)
    return 256;                 /* DeepStream 8.0 behaviour */
  return cap ? RDS_ENC_KEY_INT_MAX_CAP : idrinterval;
}

guint
rds_enc_i_frames (guint n, guint idr)
{
  guint64 frames;

  if (n == 0 || n >= idr)
    return 0;
  frames = ((guint64) idr + n - 1) / n - 1;     /* ceil (idr / n) - 1 */
  return (guint) MIN (frames, RDS_ENC_I_FRAMES_MAX);
}

typedef struct
{
  gint value;
  const gchar *name;            /* 8.0's enum nick */
  const gchar *va_profile;      /* NULL: not supported by the VA encoder */
} ProfileRow;

static const ProfileRow h264_profiles[] = {
  {0, "Baseline", "constrained-baseline"},
  {1, "Constrained-Baseline", "constrained-baseline"},
  {2, "Main", "main"},
  {4, "High", "high"},
  {17, "Constrained-High", "high"},
  {7, "High444", NULL},         /* VA has no High 4:4:4 */
};

static const ProfileRow h265_profiles[] = {
  {0, "Main", "main"},
  {1, "Main10", NULL},          /* needs P010 input */
  {3, "FREXT", NULL},           /* needs 4:4:4 input */
};

static gboolean
lookup_profile (const ProfileRow * rows, guint n_rows, const gchar * codec, gint v, const gchar ** va_profile,
    GError ** error)
{
  for (guint i = 0; i < n_rows; i++) {
    if (rows[i].value != v)
      continue;
    if (!rows[i].va_profile) {
      g_set_error (error, RDS_ENC_ERROR, RDS_ENC_ERROR_PROFILE, "profile %s (%d) is not supported by the VA encoder",
          rows[i].name, v);
      return FALSE;
    }
    *va_profile = rows[i].va_profile;
    return TRUE;
  }
  g_set_error (error, RDS_ENC_ERROR, RDS_ENC_ERROR_PROFILE, "unknown %s profile %d", codec, v);
  return FALSE;
}

gboolean
rds_enc_h264_profile (gint v, const gchar ** va_profile, GError ** error)
{
  return lookup_profile (h264_profiles, G_N_ELEMENTS (h264_profiles), "H.264", v, va_profile, error);
}

gboolean
rds_enc_h265_profile (gint v, const gchar ** va_profile, GError ** error)
{
  return lookup_profile (h265_profiles, G_N_ELEMENTS (h265_profiles), "H.265", v, va_profile, error);
}

gboolean
rds_enc_profile_is_baseline (const gchar * profile)
{
  return !g_strcmp0 (profile, "baseline") || !g_strcmp0 (profile, "constrained-baseline");
}

guint
rds_enc_b_frames (guint requested, gboolean baseline, gboolean * disabled)
{
  gboolean off = baseline && requested > 0;     /* ours */

  if (disabled)
    *disabled = off;
  return off ? 0 : requested;
}

/* @n decimal fields 0..RDS_ENC_QP_MAX, field i followed by seps[i] (the last by the end of the string), into @out
 * only when all of them parse. */
static gboolean
parse_qp_fields (const gchar * s, const gchar * seps, guint n, guint * out)
{
  guint vals[6];
  const gchar *p = s;

  g_assert (n <= G_N_ELEMENTS (vals));
  g_assert (strlen (seps) + 1 >= n);
  if (!s)
    return FALSE;
  for (guint i = 0; i < n; i++) {
    guint v = 0;

    if (!g_ascii_isdigit (*p))
      return FALSE;
    for (; g_ascii_isdigit (*p); p++) {
      v = v * 10 + (guint) (*p - '0');
      if (v > RDS_ENC_QP_MAX)   /* also keeps v from wrapping */
        return FALSE;
    }
    if (*p != (i + 1 < n ? seps[i] : '\0'))
      return FALSE;
    if (i + 1 < n)
      p++;
    vals[i] = v;
  }
  for (guint i = 0; i < n; i++)
    out[i] = vals[i];
  return TRUE;
}

gboolean
rds_enc_parse_qp_triplet (const gchar * s, guint out[3])
{
  return parse_qp_fields (s, "::", 3, out);
}

gboolean
rds_enc_parse_qp_range (const gchar * s, guint out[6])
{
  guint vals[6];

  if (!parse_qp_fields (s, ",:,:,", 6, vals))
    return FALSE;
  for (guint i = 0; i < 6; i += 2)
    if (vals[i] > vals[i + 1])
      return FALSE;
  for (guint i = 0; i < 6; i++)
    out[i] = vals[i];
  return TRUE;
}
