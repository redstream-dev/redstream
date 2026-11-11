/* Redstream: the property mapping of rdsh264enc and rdsh265enc (plugin-internal, not installed). Pure functions from
 * DeepStream 8.0's nvv4l2h264enc / nvv4l2h265enc property values to the stock VA encoders' (vah264enc, vah265enc)
 * property values, and the parsers of the QP strings. GLib only, so the cpu tests link it alone.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ENC_PROPS_H
#define RDS_ENC_PROPS_H

#include <glib.h>

G_BEGIN_DECLS

#define RDS_ENC_ERROR (rds_enc_error_quark ())
G_GNUC_INTERNAL GQuark rds_enc_error_quark (void);

typedef enum
{
  RDS_ENC_ERROR_PROFILE,        /* a profile the VA encoder can't produce, or an unknown value */
} RdsEncError;

/* The largest key-int-max we pass (ours: VA takes more, 8.0 takes any guint) and VA's largest i-frames. */
#define RDS_ENC_KEY_INT_MAX_CAP 1024
#define RDS_ENC_I_FRAMES_MAX 1023
/* The QP strings' range (H.264 and H.265, 8 bit). */
#define RDS_ENC_QP_MAX 51

/* 8.0's `bitrate` is in bit/s, VA's `bitrate` in kbit/s: @bitrate_bps / 1000 rounded half up, at least 1 (VA's
 * 0 means "auto"). */
G_GNUC_INTERNAL guint rds_enc_kbps (guint bitrate_bps);

/* VA's `target-percentage` for 8.0's `maxbitrate`: with VBR and @maxbitrate > @bitrate,
 * clamp (100 * bitrate / maxbitrate, 1, 100), rounded down (both in bit/s); otherwise 0, meaning "leave VA's
 * default" (maxbitrate then has no effect: DeepStream 8.0 behaviour). */
G_GNUC_INTERNAL guint rds_enc_target_percentage (guint bitrate, guint maxbitrate, gboolean vbr);

/* VA's `key-int-max` for 8.0's `idrinterval`: 0 means 256 (DeepStream 8.0 behaviour); above
 * RDS_ENC_KEY_INT_MAX_CAP it is capped and *@capped set (ours; the caller warns). @capped may be NULL. */
G_GNUC_INTERNAL guint rds_enc_key_int_max (guint idrinterval, gboolean * capped);

/* VA's `i-frames` (I frames between two IDRs, spaced evenly by VA) for 8.0's `iframeinterval` @n (an I frame every
 * @n frames) and the effective key-int-max @idr (rds_enc_key_int_max's result): ceil (idr / n) - 1 when
 * 0 < n < idr, else 0 (so 0 and 4294967295, "none", give 0), at most RDS_ENC_I_FRAMES_MAX. The same frames as 8.0
 * when n divides idr; otherwise VA spaces them evenly where 8.0 counts every n from the last IDR (ours). */
G_GNUC_INTERNAL guint rds_enc_i_frames (guint n, guint idr);

/* VA's `profile` caps string for 8.0's H.264 `profile` enum value @v: 0 Baseline and 1 Constrained-Baseline →
 * "constrained-baseline"; 2 Main → "main"; 4 High and 17 Constrained-High → "high". 7 High444 (VA has no
 * High 4:4:4) and unknown values: FALSE with a RDS_ENC_ERROR_PROFILE error naming the profile, *@va_profile
 * untouched. */
G_GNUC_INTERNAL gboolean rds_enc_h264_profile (gint v, const gchar ** va_profile, GError ** error);

/* The same for H.265: 0 Main → "main". 1 Main10 and 3 FREXT (they would need P010 / 4:4:4 input) and unknown
 * values: FALSE with a RDS_ENC_ERROR_PROFILE error. */
G_GNUC_INTERNAL gboolean rds_enc_h265_profile (gint v, const gchar ** va_profile, GError ** error);

/* Whether the caps profile string @profile (from the profile helpers or from downstream caps) is a Baseline one
 * ("baseline", "constrained-baseline"), which has no B frames. NULL: FALSE. */
G_GNUC_INTERNAL gboolean rds_enc_profile_is_baseline (const gchar * profile);

/* VA's `b-frames` for 8.0's `num-B-Frames` @requested (0..2, enforced by the property spec): 0 with *@disabled
 * set when @baseline and @requested > 0 (ours: VA would write B slices into a Baseline stream; the caller warns),
 * else @requested with *@disabled cleared. @disabled may be NULL. */
G_GNUC_INTERNAL guint rds_enc_b_frames (guint requested, gboolean baseline, gboolean * disabled);

/* `constqp` and `initqp`: "I:P:B", three decimal integers 0..RDS_ENC_QP_MAX, no signs, spaces or other fields.
 * TRUE fills @out with I, P, B; FALSE (NULL, empty or malformed) leaves @out untouched. */
G_GNUC_INTERNAL gboolean rds_enc_parse_qp_triplet (const gchar * s, guint out[3]);

/* `qp-range`: "minP,maxP:minI,maxI:minB,maxB", six decimal integers 0..RDS_ENC_QP_MAX with min <= max in each
 * pair, no signs, spaces or other fields. TRUE fills @out in string order (min P, max P, min I, max I, min B,
 * max B); FALSE leaves @out untouched. */
G_GNUC_INTERNAL gboolean rds_enc_parse_qp_range (const gchar * s, guint out[6]);

G_END_DECLS

#endif /* RDS_ENC_PROPS_H */
