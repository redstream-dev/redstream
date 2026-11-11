/* rdsh264enc / rdsh265enc caps decisions on plain caps: the preferred profile against downstream answers, the
 * Baseline + B frames rule, and the downstream level check.
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

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/enc-caps/preferred", test_preferred);
  g_test_add_func ("/cpu/enc-caps/b-frames", test_b_frames);
  g_test_add_func ("/cpu/enc-caps/level", test_level);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
