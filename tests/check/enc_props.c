/* rdsh264enc / rdsh265enc property mapping: kbps, target-percentage, the key-int-max cap, i-frames, the profile
 * tables and their errors, the B-frame rule, and the constqp / initqp / qp-range parsers (valid and malformed).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-props.h"

/* ------------------------------------------------------------------------------------------------------------ */
/* Rate */

static void
test_kbps (void)
{
  g_assert_cmpuint (rds_enc_kbps (4000000), ==, 4000);  /* 8.0's default */
  g_assert_cmpuint (rds_enc_kbps (1000), ==, 1);
  g_assert_cmpuint (rds_enc_kbps (1499), ==, 1);
  g_assert_cmpuint (rds_enc_kbps (1500), ==, 2);        /* half up */
  g_assert_cmpuint (rds_enc_kbps (2499), ==, 2);
  g_assert_cmpuint (rds_enc_kbps (2500), ==, 3);
  g_assert_cmpuint (rds_enc_kbps (123456), ==, 123);
  g_assert_cmpuint (rds_enc_kbps (123500), ==, 124);
  /* never 0 (VA's "auto") */
  g_assert_cmpuint (rds_enc_kbps (0), ==, 1);
  g_assert_cmpuint (rds_enc_kbps (1), ==, 1);
  g_assert_cmpuint (rds_enc_kbps (499), ==, 1);
  g_assert_cmpuint (rds_enc_kbps (500), ==, 1);
  /* no overflow at the top of the range */
  g_assert_cmpuint (rds_enc_kbps (G_MAXUINT), ==, 4294967);
  g_assert_cmpuint (rds_enc_kbps (G_MAXUINT - 295), ==, 4294967);
  g_assert_cmpuint (rds_enc_kbps (4294966500u), ==, 4294967);
  g_assert_cmpuint (rds_enc_kbps (4294966499u), ==, 4294966);
}

static void
test_target_percentage (void)
{
  /* VBR with maxbitrate above bitrate */
  g_assert_cmpuint (rds_enc_target_percentage (4000000, 8000000, TRUE), ==, 50);
  g_assert_cmpuint (rds_enc_target_percentage (2000000, 3000000, TRUE), ==, 66);   /* 66.7, rounded down */
  g_assert_cmpuint (rds_enc_target_percentage (999, 1000, TRUE), ==, 99);
  g_assert_cmpuint (rds_enc_target_percentage (1, 1000, TRUE), ==, 1);     /* 0.1, clamped to 1 */
  g_assert_cmpuint (rds_enc_target_percentage (0, 1000, TRUE), ==, 1);
  g_assert_cmpuint (rds_enc_target_percentage (1, G_MAXUINT, TRUE), ==, 1);
  /* 64-bit product: 100 * (G_MAXUINT - 1) overflows 32 bits */
  g_assert_cmpuint (rds_enc_target_percentage (G_MAXUINT - 1, G_MAXUINT, TRUE), ==, 99);
  g_assert_cmpuint (rds_enc_target_percentage (G_MAXUINT / 2, G_MAXUINT, TRUE), ==, 49);
  /* no effect: maxbitrate not above bitrate, or not VBR */
  g_assert_cmpuint (rds_enc_target_percentage (4000000, 0, TRUE), ==, 0);  /* 8.0's default maxbitrate */
  g_assert_cmpuint (rds_enc_target_percentage (4000000, 4000000, TRUE), ==, 0);
  g_assert_cmpuint (rds_enc_target_percentage (4000000, 3000000, TRUE), ==, 0);
  g_assert_cmpuint (rds_enc_target_percentage (0, 0, TRUE), ==, 0);
  g_assert_cmpuint (rds_enc_target_percentage (4000000, 8000000, FALSE), ==, 0);
  g_assert_cmpuint (rds_enc_target_percentage (1, G_MAXUINT, FALSE), ==, 0);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* GOP */

static void
test_key_int_max (void)
{
  gboolean capped = TRUE;

  g_assert_cmpuint (rds_enc_key_int_max (256, &capped), ==, 256);     /* 8.0's default */
  g_assert_false (capped);
  capped = TRUE;
  g_assert_cmpuint (rds_enc_key_int_max (0, &capped), ==, 256);       /* 0 means 256 */
  g_assert_false (capped);
  capped = TRUE;
  g_assert_cmpuint (rds_enc_key_int_max (1, &capped), ==, 1);
  g_assert_false (capped);
  capped = TRUE;
  g_assert_cmpuint (rds_enc_key_int_max (30, &capped), ==, 30);
  g_assert_false (capped);
  capped = TRUE;
  g_assert_cmpuint (rds_enc_key_int_max (1024, &capped), ==, 1024);
  g_assert_false (capped);
  g_assert_cmpuint (rds_enc_key_int_max (1025, &capped), ==, 1024);
  g_assert_true (capped);
  capped = FALSE;
  g_assert_cmpuint (rds_enc_key_int_max (G_MAXUINT, &capped), ==, 1024);
  g_assert_true (capped);
  /* @capped is optional */
  g_assert_cmpuint (rds_enc_key_int_max (5000, NULL), ==, 1024);
  g_assert_cmpuint (rds_enc_key_int_max (0, NULL), ==, 256);
}

static void
test_i_frames (void)
{
  /* N divides idr: the same frames as 8.0 */
  g_assert_cmpuint (rds_enc_i_frames (10, 30), ==, 2);
  g_assert_cmpuint (rds_enc_i_frames (15, 30), ==, 1);
  g_assert_cmpuint (rds_enc_i_frames (64, 256), ==, 3);
  g_assert_cmpuint (rds_enc_i_frames (1, 30), ==, 29);
  /* N doesn't divide idr: rounded up, VA spaces them evenly */
  g_assert_cmpuint (rds_enc_i_frames (30, 256), ==, 8);         /* ceil (8.53) - 1 */
  g_assert_cmpuint (rds_enc_i_frames (7, 30), ==, 4);           /* ceil (4.29) - 1 */
  g_assert_cmpuint (rds_enc_i_frames (29, 30), ==, 1);
  g_assert_cmpuint (rds_enc_i_frames (2, 3), ==, 1);
  /* N >= idr: none */
  g_assert_cmpuint (rds_enc_i_frames (30, 30), ==, 0);
  g_assert_cmpuint (rds_enc_i_frames (45, 30), ==, 0);
  g_assert_cmpuint (rds_enc_i_frames (1, 1), ==, 0);
  /* 0 and 4294967295 (8.0's default): none */
  g_assert_cmpuint (rds_enc_i_frames (0, 30), ==, 0);
  g_assert_cmpuint (rds_enc_i_frames (0, 256), ==, 0);
  g_assert_cmpuint (rds_enc_i_frames (G_MAXUINT, 256), ==, 0);
  g_assert_cmpuint (rds_enc_i_frames (G_MAXUINT, 1024), ==, 0);
  g_assert_cmpuint (rds_enc_i_frames (5, 0), ==, 0);
  /* at most VA's 1023 */
  g_assert_cmpuint (rds_enc_i_frames (1, 1024), ==, 1023);
  g_assert_cmpuint (rds_enc_i_frames (1, 1025), ==, 1023);
  g_assert_cmpuint (rds_enc_i_frames (1, G_MAXUINT), ==, 1023);
  g_assert_cmpuint (rds_enc_i_frames (2, 1024), ==, 511);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Profiles */

static void
check_profile_ok (gboolean (*fn) (gint, const gchar **, GError **), gint v, const gchar * expected)
{
  GError *err = NULL;
  const gchar *p = NULL;

  g_assert_true (fn (v, &p, &err));
  g_assert_no_error (err);
  g_assert_cmpstr (p, ==, expected);
}

static void
check_profile_error (gboolean (*fn) (gint, const gchar **, GError **), gint v, const gchar * expected_msg)
{
  GError *err = NULL;
  const gchar *p = "untouched";

  g_assert_false (fn (v, &p, &err));
  g_assert_error (err, RDS_ENC_ERROR, RDS_ENC_ERROR_PROFILE);
  g_assert_cmpstr (err->message, ==, expected_msg);
  g_assert_cmpstr (p, ==, "untouched");
  g_clear_error (&err);
  /* a NULL error is fine */
  g_assert_false (fn (v, &p, NULL));
  g_assert_cmpstr (p, ==, "untouched");
}

static void
test_h264_profile (void)
{
  check_profile_ok (rds_enc_h264_profile, 0, "constrained-baseline");  /* Baseline, 8.0's default */
  check_profile_ok (rds_enc_h264_profile, 1, "constrained-baseline");
  check_profile_ok (rds_enc_h264_profile, 2, "main");
  check_profile_ok (rds_enc_h264_profile, 4, "high");
  check_profile_ok (rds_enc_h264_profile, 17, "high");  /* Constrained-High */
  check_profile_error (rds_enc_h264_profile, 7, "profile High444 (7) is not supported by the VA encoder");
  check_profile_error (rds_enc_h264_profile, 3, "unknown H.264 profile 3");
  check_profile_error (rds_enc_h264_profile, 5, "unknown H.264 profile 5");
  check_profile_error (rds_enc_h264_profile, 18, "unknown H.264 profile 18");
  check_profile_error (rds_enc_h264_profile, -1, "unknown H.264 profile -1");
}

static void
test_h265_profile (void)
{
  check_profile_ok (rds_enc_h265_profile, 0, "main");
  check_profile_error (rds_enc_h265_profile, 1, "profile Main10 (1) is not supported by the VA encoder");
  check_profile_error (rds_enc_h265_profile, 3, "profile FREXT (3) is not supported by the VA encoder");
  check_profile_error (rds_enc_h265_profile, 2, "unknown H.265 profile 2");
  check_profile_error (rds_enc_h265_profile, 4, "unknown H.265 profile 4");
  check_profile_error (rds_enc_h265_profile, -1, "unknown H.265 profile -1");
}

static void
test_profile_is_baseline (void)
{
  const gchar *p = NULL;

  g_assert_true (rds_enc_profile_is_baseline ("baseline"));
  g_assert_true (rds_enc_profile_is_baseline ("constrained-baseline"));
  g_assert_false (rds_enc_profile_is_baseline ("main"));
  g_assert_false (rds_enc_profile_is_baseline ("high"));
  g_assert_false (rds_enc_profile_is_baseline ("extended"));
  g_assert_false (rds_enc_profile_is_baseline (""));
  g_assert_false (rds_enc_profile_is_baseline (NULL));
  /* the profile tables' results */
  g_assert_true (rds_enc_h264_profile (0, &p, NULL));
  g_assert_true (rds_enc_profile_is_baseline (p));
  g_assert_true (rds_enc_h264_profile (1, &p, NULL));
  g_assert_true (rds_enc_profile_is_baseline (p));
  g_assert_true (rds_enc_h264_profile (2, &p, NULL));
  g_assert_false (rds_enc_profile_is_baseline (p));
  g_assert_true (rds_enc_h264_profile (17, &p, NULL));
  g_assert_false (rds_enc_profile_is_baseline (p));
  g_assert_true (rds_enc_h265_profile (0, &p, NULL));
  g_assert_false (rds_enc_profile_is_baseline (p));
}

static void
test_b_frames (void)
{
  gboolean disabled = TRUE;

  g_assert_cmpuint (rds_enc_b_frames (0, FALSE, &disabled), ==, 0);
  g_assert_false (disabled);
  disabled = TRUE;
  g_assert_cmpuint (rds_enc_b_frames (1, FALSE, &disabled), ==, 1);
  g_assert_false (disabled);
  disabled = TRUE;
  g_assert_cmpuint (rds_enc_b_frames (2, FALSE, &disabled), ==, 2);
  g_assert_false (disabled);
  disabled = TRUE;
  g_assert_cmpuint (rds_enc_b_frames (0, TRUE, &disabled), ==, 0);   /* nothing to disable */
  g_assert_false (disabled);
  g_assert_cmpuint (rds_enc_b_frames (1, TRUE, &disabled), ==, 0);
  g_assert_true (disabled);
  disabled = FALSE;
  g_assert_cmpuint (rds_enc_b_frames (2, TRUE, &disabled), ==, 0);
  g_assert_true (disabled);
  /* @disabled is optional */
  g_assert_cmpuint (rds_enc_b_frames (2, TRUE, NULL), ==, 0);
  g_assert_cmpuint (rds_enc_b_frames (2, FALSE, NULL), ==, 2);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* QP strings */

#define SENTINEL 777u

static void
check_triplet_ok (const gchar * s, guint i, guint p, guint b)
{
  guint out[3] = { SENTINEL, SENTINEL, SENTINEL };

  g_assert_true (rds_enc_parse_qp_triplet (s, out));
  g_assert_cmpuint (out[0], ==, i);
  g_assert_cmpuint (out[1], ==, p);
  g_assert_cmpuint (out[2], ==, b);
}

static void
check_triplet_bad (const gchar * s)
{
  guint out[3] = { SENTINEL, SENTINEL, SENTINEL };

  if (rds_enc_parse_qp_triplet (s, out))
    g_error ("constqp \"%s\" accepted", s);
  for (guint k = 0; k < 3; k++)
    g_assert_cmpuint (out[k], ==, SENTINEL);    /* no partial writes */
}

static void
test_qp_triplet (void)
{
  check_triplet_ok ("20:25:30", 20, 25, 30);
  check_triplet_ok ("0:0:0", 0, 0, 0);
  check_triplet_ok ("51:51:51", 51, 51, 51);
  check_triplet_ok ("0:51:7", 0, 51, 7);
  check_triplet_ok ("30:20:10", 30, 20, 10);    /* no order between the frame types */
  check_triplet_ok ("05:007:0", 5, 7, 0);       /* leading zeros are decimal */

  check_triplet_bad (NULL);
  check_triplet_bad ("");
  /* missing fields */
  check_triplet_bad ("20");
  check_triplet_bad ("20:25");
  check_triplet_bad ("20:25:");
  check_triplet_bad (":25:30");
  check_triplet_bad ("20::30");
  check_triplet_bad ("::");
  /* extra fields and separators */
  check_triplet_bad ("20:25:30:35");
  check_triplet_bad ("20:25:30:");
  check_triplet_bad ("20,25,30");
  check_triplet_bad ("20:25,30");
  check_triplet_bad ("20;25;30");
  /* letters, signs, spaces */
  check_triplet_bad ("a:25:30");
  check_triplet_bad ("20:25:3x");
  check_triplet_bad ("0x10:25:30");
  check_triplet_bad ("-1:25:30");
  check_triplet_bad ("20:-5:30");
  check_triplet_bad ("+20:25:30");
  check_triplet_bad (" 20:25:30");
  check_triplet_bad ("20:25:30 ");
  check_triplet_bad ("20 :25:30");
  check_triplet_bad ("20: 25:30");
  check_triplet_bad ("20:25:30\n");
  check_triplet_bad ("2.5:25:30");
  /* above 51 */
  check_triplet_bad ("52:25:30");
  check_triplet_bad ("20:52:30");
  check_triplet_bad ("20:25:52");
  check_triplet_bad ("100:25:30");
  check_triplet_bad ("4294967296:25:30");       /* would wrap a guint */
  check_triplet_bad ("20:25:99999999999999999999");
}

static void
check_range_ok (const gchar * s, const guint expected[6])
{
  guint out[6] = { SENTINEL, SENTINEL, SENTINEL, SENTINEL, SENTINEL, SENTINEL };

  g_assert_true (rds_enc_parse_qp_range (s, out));
  for (guint k = 0; k < 6; k++)
    g_assert_cmpuint (out[k], ==, expected[k]);
}

static void
check_range_bad (const gchar * s)
{
  guint out[6] = { SENTINEL, SENTINEL, SENTINEL, SENTINEL, SENTINEL, SENTINEL };

  if (rds_enc_parse_qp_range (s, out))
    g_error ("qp-range \"%s\" accepted", s);
  for (guint k = 0; k < 6; k++)
    g_assert_cmpuint (out[k], ==, SENTINEL);    /* no partial writes */
}

static void
test_qp_range (void)
{
  check_range_ok ("10,40:5,35:15,45", (const guint[6]) { 10, 40, 5, 35, 15, 45 });
  check_range_ok ("0,51:0,51:0,51", (const guint[6]) { 0, 51, 0, 51, 0, 51 });
  check_range_ok ("0,0:0,0:0,0", (const guint[6]) { 0, 0, 0, 0, 0, 0 });
  check_range_ok ("51,51:51,51:51,51", (const guint[6]) { 51, 51, 51, 51, 51, 51 });
  check_range_ok ("20,20:10,30:01,02", (const guint[6]) { 20, 20, 10, 30, 1, 2 });     /* min == max */

  check_range_bad (NULL);
  check_range_bad ("");
  /* missing fields */
  check_range_bad ("10,40");
  check_range_bad ("10,40:5,35");
  check_range_bad ("10,40:5,35:15");
  check_range_bad ("10,40:5,35:15,");
  check_range_bad ("10,40:5,35:,45");
  check_range_bad ("10:5,35:15,45");
  check_range_bad (",40:5,35:15,45");
  check_range_bad ("10,40::15,45");
  check_range_bad (",:,:,");
  /* extra fields, wrong separators */
  check_range_bad ("10,40:5,35:15,45:1,2");
  check_range_bad ("10,40:5,35:15,45,50");
  check_range_bad ("10,40:5,35:15,45:");
  check_range_bad ("10:40:5:35:15:45");
  check_range_bad ("10,40,5,35,15,45");
  check_range_bad ("10,40;5,35;15,45");
  check_range_bad ("20:25:30");                 /* a triplet */
  /* letters, signs, spaces */
  check_range_bad ("a,40:5,35:15,45");
  check_range_bad ("10,40:5,35:15,4b");
  check_range_bad ("-1,40:5,35:15,45");
  check_range_bad ("10,40:-5,35:15,45");
  check_range_bad ("+10,40:5,35:15,45");
  check_range_bad (" 10,40:5,35:15,45");
  check_range_bad ("10,40:5,35:15,45 ");
  check_range_bad ("10, 40:5,35:15,45");
  check_range_bad ("10,40 :5,35:15,45");
  /* above 51 */
  check_range_bad ("10,52:5,35:15,45");
  check_range_bad ("10,40:5,35:15,52");
  check_range_bad ("10,40:5,99:15,45");
  check_range_bad ("10,40:5,35:15,4294967296");
  /* min above max */
  check_range_bad ("41,40:5,35:15,45");
  check_range_bad ("10,40:36,35:15,45");
  check_range_bad ("10,40:5,35:46,45");
  check_range_bad ("51,0:0,51:0,51");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/enc-props/rate/kbps", test_kbps);
  g_test_add_func ("/cpu/enc-props/rate/target-percentage", test_target_percentage);
  g_test_add_func ("/cpu/enc-props/gop/key-int-max", test_key_int_max);
  g_test_add_func ("/cpu/enc-props/gop/i-frames", test_i_frames);
  g_test_add_func ("/cpu/enc-props/profile/h264", test_h264_profile);
  g_test_add_func ("/cpu/enc-props/profile/h265", test_h265_profile);
  g_test_add_func ("/cpu/enc-props/profile/is-baseline", test_profile_is_baseline);
  g_test_add_func ("/cpu/enc-props/profile/b-frames", test_b_frames);
  g_test_add_func ("/cpu/enc-props/qp/triplet", test_qp_triplet);
  g_test_add_func ("/cpu/enc-props/qp/range", test_qp_range);
  return g_test_run ();
}
