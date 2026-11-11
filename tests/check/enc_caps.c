/* rdsh264enc / rdsh265enc caps decisions on plain caps: the preferred profile against downstream answers, the
 * Baseline + B frames rule, the downstream level check and verdict (written into the stream or refused), the size
 * range from the VA encoder's template and caps limited to it.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-caps.h"

#define H264_ALL "video/x-h264, profile=(string){ constrained-baseline, main, high }, " \
  "stream-format=(string)byte-stream, alignment=(string)au"

/* rds_enc_caps_narrow on @answer, compared with @want (caps strings) and @want_only_baseline. */
static void
check_narrow (const gchar * answer, const gchar * preferred, guint b, const gchar * want, gboolean want_only_baseline)
{
  GstCaps *a = gst_caps_from_string (answer), *w = gst_caps_from_string (want), *got;
  gboolean only_baseline = !want_only_baseline;
  gchar *s;

  got = rds_enc_caps_narrow (a, "video/x-h264", preferred, b, &only_baseline);
  s = gst_caps_to_string (got);
  g_test_message ("%s, %s, b %u -> %s", answer, preferred ? preferred : "(none)", b, s);
  g_assert_true (gst_caps_is_equal (got, w));
  g_assert_cmpint (only_baseline, ==, want_only_baseline);
  g_free (s);
  gst_caps_unref (got);
  gst_caps_unref (w);
  gst_caps_unref (a);
}

#define TAIL ", stream-format=(string)byte-stream, alignment=(string)au"

static void
test_preferred (void)
{
  /* downstream allows everything: the preference */
  check_narrow (H264_ALL, "main", 0, "video/x-h264, profile=(string)main" TAIL, FALSE);
  check_narrow (H264_ALL, "constrained-baseline", 0, "video/x-h264, profile=(string)constrained-baseline" TAIL,
      TRUE);
  /* downstream fixes another profile: it wins */
  check_narrow ("video/x-h264, profile=(string)high" TAIL, "main", 0, "video/x-h264, profile=(string)high" TAIL,
      FALSE);
  /* the preference isn't allowed: the answer stays as it is */
  check_narrow ("video/x-h264, profile=(string){ main, constrained-baseline }" TAIL, "high", 0,
      "video/x-h264, profile=(string){ main, constrained-baseline }" TAIL, FALSE);
  /* no preference */
  check_narrow (H264_ALL, NULL, 0, H264_ALL, FALSE);
  /* an answer without a profile field gets the preferred one */
  check_narrow ("video/x-h264" TAIL, "main", 0, "video/x-h264, profile=(string)main" TAIL, FALSE);
}

static void
test_b_frames (void)
{
  /* the preferred Baseline wins over the B frames: Baseline only, the caller turns B frames off */
  check_narrow (H264_ALL, "constrained-baseline", 2, "video/x-h264, profile=(string)constrained-baseline" TAIL,
      TRUE);
  check_narrow ("video/x-h264, profile=(string){ constrained-baseline, main }" TAIL, "constrained-baseline", 2,
      "video/x-h264, profile=(string)constrained-baseline" TAIL, TRUE);
  /* downstream fixes Baseline */
  check_narrow ("video/x-h264, profile=(string)constrained-baseline" TAIL, "high", 1,
      "video/x-h264, profile=(string)constrained-baseline" TAIL, TRUE);
  /* the preference isn't allowed and there is a choice: B frames leave Baseline out */
  check_narrow ("video/x-h264, profile=(string){ constrained-baseline, high }" TAIL, "main", 2,
      "video/x-h264, profile=(string)high" TAIL, FALSE);
  check_narrow ("video/x-h264, profile=(string){ baseline, constrained-baseline, main, high }" TAIL, NULL, 1,
      "video/x-h264, profile=(string){ main, high }" TAIL, FALSE);
  /* without B frames the choice stays */
  check_narrow ("video/x-h264, profile=(string){ constrained-baseline, high }" TAIL, "main", 0,
      "video/x-h264, profile=(string){ constrained-baseline, high }" TAIL, FALSE);
  /* B frames with a non-Baseline result: nothing to do */
  check_narrow (H264_ALL, "main", 2, "video/x-h264, profile=(string)main" TAIL, FALSE);
  /* in several structures */
  check_narrow ("video/x-h264, profile=(string)constrained-baseline, width=(int)640; "
      "video/x-h264, profile=(string)main, width=(int)640", NULL, 2,
      "video/x-h264, profile=(string)main, width=(int)640", FALSE);
}

static void
check_level (const gchar * caps, const gchar * allowed, const gchar * want)
{
  GstCaps *c = gst_caps_from_string (caps), *a = gst_caps_from_string (allowed);
  gchar *wanted = NULL;

  g_assert_cmpint (rds_enc_caps_level_refused (c, a, &wanted), ==, want != NULL);
  g_assert_cmpstr (wanted, ==, want);
  g_free (wanted);
  gst_caps_unref (a);
  gst_caps_unref (c);
}

#define OUT "video/x-h264, profile=(string)high, level=(string)3.1, width=(int)1280, height=(int)720"

static void
test_level (void)
{
  /* another level downstream: refused because of it */
  check_level (OUT, "video/x-h264, level=(string)4.1", "4.1");
  check_level (OUT, "video/x-h264, level=(string){ 4, 4.1 }", "{ (string)4, (string)4.1 }");
  /* the same level, or none: accepted */
  check_level (OUT, "video/x-h264, level=(string)3.1", NULL);
  check_level (OUT, "video/x-h264", NULL);
  /* refused for another reason as well (the profile): no level warning */
  check_level (OUT, "video/x-h264, profile=(string)main, level=(string)4.1", NULL);
  check_level (OUT, "video/x-h265", NULL);
  /* no level in the caps */
  check_level ("video/x-h264, profile=(string)high", "video/x-h264, profile=(string)main", NULL);
}

/* rds_enc_caps_level_check of @caps against @allowed: @verdict, and the level to write (RAISE) or downstream's
 * serialized (REFUSED). */
static void
check_level_verdict (const gchar * caps, const gchar * allowed, RdsEncCodec codec, RdsEncLevelVerdict verdict,
    const gchar * want)
{
  GstCaps *c = gst_caps_from_string (caps), *a = gst_caps_from_string (allowed);
  const gchar *raise_to = "untouched";
  gchar *wanted = NULL;

  g_test_message ("%s against %s", caps, allowed);
  g_assert_cmpint (rds_enc_caps_level_check (c, a, codec, &raise_to, &wanted), ==, verdict);
  if (verdict == RDS_ENC_LEVEL_RAISE) {
    g_assert_cmpstr (raise_to, ==, want);
    g_assert_null (wanted);
  } else {
    g_assert_cmpstr (raise_to, ==, "untouched");
    g_assert_cmpstr (wanted, ==, verdict == RDS_ENC_LEVEL_REFUSED ? want : NULL);
  }
  g_free (wanted);
  gst_caps_unref (a);
  gst_caps_unref (c);
}

#define OUT265 "video/x-h265, profile=(string)main, tier=(string)main, level=(string)3.1, width=(int)1280"

static void
test_level_verdict (void)
{
  /* a fixed downstream level above VA's: written (DeepStream 8.0 behaviour) */
  check_level_verdict (OUT, "video/x-h264, level=(string)4.1", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_RAISE, "4.1");
  check_level_verdict (OUT, "video/x-h264, level=(string)5.1", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_RAISE, "5.1");
  check_level_verdict (OUT, "video/x-h264, level=(string)4", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_RAISE, "4");
  check_level_verdict (OUT, "video/x-h264, profile=(string)high, level=(string)4.1; video/x-h264",
      RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_ACCEPTED, NULL);  /* the second structure takes VA's caps as they are */
  check_level_verdict (OUT265, "video/x-h265, level=(string)5.1", RDS_ENC_CODEC_H265, RDS_ENC_LEVEL_RAISE, "5.1");
  check_level_verdict ("video/x-h264, level=(string)1", "video/x-h264, level=(string)1b", RDS_ENC_CODEC_H264,
      RDS_ENC_LEVEL_RAISE, "1b");
  /* the same level or none: nothing to do */
  check_level_verdict (OUT, "video/x-h264, level=(string)3.1", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_ACCEPTED, NULL);
  check_level_verdict (OUT, "video/x-h264", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_ACCEPTED, NULL);
  check_level_verdict ("video/x-h264, profile=(string)high", "video/x-h264, level=(string)4.1", RDS_ENC_CODEC_H264,
      RDS_ENC_LEVEL_ACCEPTED, NULL);
  /* refused for another reason too: not the level's business */
  check_level_verdict (OUT, "video/x-h264, profile=(string)main, level=(string)4.1", RDS_ENC_CODEC_H264,
      RDS_ENC_LEVEL_ACCEPTED, NULL);
  /* below VA's, a list, an unknown level, an H.264 level for H.265: refused (ours) */
  check_level_verdict (OUT, "video/x-h264, level=(string)2", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_REFUSED, "2");
  check_level_verdict (OUT, "video/x-h264, level=(string)1b", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_REFUSED, "1b");
  check_level_verdict (OUT, "video/x-h264, level=(string){ 4, 4.1 }", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_REFUSED,
      "{ (string)4, (string)4.1 }");
  check_level_verdict (OUT, "video/x-h264, level=(string)9.9", RDS_ENC_CODEC_H264, RDS_ENC_LEVEL_REFUSED, "9.9");
  check_level_verdict (OUT265, "video/x-h265, level=(string)3", RDS_ENC_CODEC_H265, RDS_ENC_LEVEL_REFUSED, "3");
  check_level_verdict (OUT265, "video/x-h265, level=(string)3.2", RDS_ENC_CODEC_H265, RDS_ENC_LEVEL_REFUSED, "3.2");
  /* VA's own level unknown: refused */
  check_level_verdict ("video/x-h264, level=(string)7", "video/x-h264, level=(string)4.1", RDS_ENC_CODEC_H264,
      RDS_ENC_LEVEL_REFUSED, "4.1");
}

/* rds_enc_caps_size_range on @templ: FALSE when @want is NULL, else the range "min_w max_w min_h max_h". */
static void
check_range (const gchar * templ, const gchar * want)
{
  GstCaps *t = gst_caps_from_string (templ);
  gint r[4] = { -1, -1, -1, -1 };
  gchar *got;

  g_assert_cmpint (rds_enc_caps_size_range (t, &r[0], &r[1], &r[2], &r[3]), ==, want != NULL);
  if (want != NULL) {
    got = g_strdup_printf ("%d %d %d %d", r[0], r[1], r[2], r[3]);
    g_assert_cmpstr (got, ==, want);
    g_free (got);
  } else {
    for (guint i = 0; i < 4; i++)
      g_assert_cmpint (r[i], ==, -1);
  }
  gst_caps_unref (t);
}

static void
test_size_range (void)
{
  /* VA's templates: the system-memory structure's range */
  check_range ("video/x-raw(memory:VAMemory), width=(int)[ 1, 2 ], height=(int)[ 1, 2 ], format=(string)NV12; "
      "video/x-raw, width=(int)[ 128, 4096 ], height=(int)[ 128, 4096 ], format=(string)NV12", "128 4096 128 4096");
  check_range ("video/x-raw(memory:SystemMemory), width=(int)[ 130, 8192 ], height=(int)[ 128, 4352 ]",
      "130 8192 128 4352");
  /* a fixed size is a range of one */
  check_range ("video/x-raw, width=(int)640, height=(int)[ 16, 4096 ]", "640 640 16 4096");
  /* no limits known */
  check_range ("ANY", NULL);
  check_range ("video/x-raw, format=(string)NV12", NULL);
  check_range ("video/x-raw(memory:VAMemory), width=(int)[ 1, 2 ], height=(int)[ 1, 2 ]", NULL);
}

/* rds_enc_caps_limit_size on @in to 130..8192 x 128..4352, compared with @want. */
static void
check_limit (const gchar * in, const gchar * want)
{
  GstCaps *i = gst_caps_from_string (in), *w = gst_caps_from_string (want), *got;
  gchar *s;

  got = rds_enc_caps_limit_size (i, 130, 8192, 128, 4352);
  s = gst_caps_to_string (got);
  g_test_message ("%s -> %s", in, s);
  g_assert_true (gst_caps_is_equal (got, w));
  g_free (s);
  gst_caps_unref (got);
  gst_caps_unref (w);
  gst_caps_unref (i);
}

static void
test_limit_size (void)
{
  /* ranges narrowed, features kept, every structure */
  check_limit ("video/x-raw(memory:RDS), format=(string)NV12, width=(int)[ 1, 16384 ], height=(int)[ 1, 16384 ]; "
      "video/x-raw, format=(string)RGBA, width=(int)[ 1, 16384 ], height=(int)[ 1, 16384 ]",
      "video/x-raw(memory:RDS), format=(string)NV12, width=(int)[ 130, 8192 ], height=(int)[ 128, 4352 ]; "
      "video/x-raw, format=(string)RGBA, width=(int)[ 130, 8192 ], height=(int)[ 128, 4352 ]");
  /* fixed sizes: kept inside, dropped outside */
  check_limit ("video/x-raw, width=(int)1281, height=(int)721; video/x-raw, width=(int)129, height=(int)256; "
      "video/x-raw, width=(int)256, height=(int)4353", "video/x-raw, width=(int)1281, height=(int)721");
  check_limit ("video/x-raw, width=(int)8193, height=(int)256", "EMPTY");
  /* an edge: a range of one becomes an int */
  check_limit ("video/x-raw, width=(int)[ 8192, 9000 ], height=(int)[ 100, 128 ]",
      "video/x-raw, width=(int)8192, height=(int)128");
  /* no size fields: the ranges are added */
  check_limit ("video/x-raw, format=(string)NV12",
      "video/x-raw, format=(string)NV12, width=(int)[ 130, 8192 ], height=(int)[ 128, 4352 ]");
  check_limit ("ANY", "ANY");
  check_limit ("EMPTY", "EMPTY");
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/enc-caps/preferred", test_preferred);
  g_test_add_func ("/cpu/enc-caps/b-frames", test_b_frames);
  g_test_add_func ("/cpu/enc-caps/level", test_level);
  g_test_add_func ("/cpu/enc-caps/level-verdict", test_level_verdict);
  g_test_add_func ("/cpu/enc-caps/size-range", test_size_range);
  g_test_add_func ("/cpu/enc-caps/limit-size", test_limit_size);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
