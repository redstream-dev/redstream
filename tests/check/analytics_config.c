/* rdsanalytics config parser: defaults, every key, the rules, class-id, the errors, skipped groups and unknown keys,
 * the key table and its rows in docs/compat-matrix.md.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-config.h"

#include <glib/gstdio.h>
#include <string.h>

#define PROPERTY "[property]\nenable=1\nconfig-width=1280\nconfig-height=720\n"

/* A one-off config: @text written to a fresh temp dir. Returns the path; tmp_cleanup removes the file and dir. */
static gchar *
tmp_config (const gchar * text)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("rds-analytics-config-XXXXXX", &err);
  gchar *path = g_build_filename (dir, "analytics.txt", NULL);

  g_assert_no_error (err);
  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  g_free (dir);
  return path;
}

static void
tmp_cleanup (gchar * path)
{
  gchar *dir = g_path_get_dirname (path);

  g_unlink (path);
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

static RdsAnalyticsConfig *
text_ok (const gchar * text)
{
  gchar *path = tmp_config (text);
  GError *err = NULL;
  RdsAnalyticsConfig *cfg = rds_analytics_config_load (path, &err);

  g_assert_no_error (err);
  g_assert_nonnull (cfg);
  tmp_cleanup (path);
  return cfg;
}

/* Loads @text, which must fail with @code; the message must contain "<path>:" followed by each of the
 * NULL-terminated substrings, in order. */
static void
text_fails (const gchar * text, gint code, ...)
{
  gchar *path = tmp_config (text);
  gchar *prefix = g_strconcat (path, ":", NULL);
  GError *err = NULL;
  RdsAnalyticsConfig *cfg = rds_analytics_config_load (path, &err);
  const gchar *pos, *want;
  va_list ap;

  g_assert_null (cfg);
  g_assert_error (err, RDS_ANALYTICS_CONFIG_ERROR, code);
  g_test_message ("%s", err->message);
  pos = strstr (err->message, prefix);
  g_assert_nonnull (pos);
  pos += strlen (prefix);
  va_start (ap, code);
  while ((want = va_arg (ap, const gchar *))) {
    const gchar *found = strstr (pos, want);

    if (!found)
      g_error ("\"%s\" not found after the file name in \"%s\"", want, err->message);
    pos = found + strlen (want);
  }
  va_end (ap);
  g_error_free (err);
  g_free (prefix);
  tmp_cleanup (path);
}

/* The messages the parser logs (domain "rdsanalytics"), captured between capture_start and capture_stop. */
static GPtrArray *captured;

static void
capture_handler (const gchar * domain, GLogLevelFlags level, const gchar * message, gpointer user_data)
{
  (void) domain;
  (void) level;
  (void) user_data;
  g_test_message ("logged: %s", message);
  g_ptr_array_add (captured, g_strdup (message));
}

static guint capture_id;

static void
capture_start (void)
{
  captured = g_ptr_array_new_with_free_func (g_free);
  capture_id = g_log_set_handler ("rdsanalytics", G_LOG_LEVEL_MESSAGE, capture_handler, NULL);
}

/* Stops capturing and checks that exactly the NULL-terminated glob patterns were logged, in order. */
static void
capture_stop (const gchar * pattern, ...)
{
  va_list ap;
  guint n = 0;

  g_log_remove_handler ("rdsanalytics", capture_id);
  va_start (ap, pattern);
  for (const gchar * p = pattern; p; p = va_arg (ap, const gchar *), n++) {
    g_assert_cmpuint (n, <, captured->len);
    if (!g_pattern_match_simple (p, g_ptr_array_index (captured, n)))
      g_error ("logged \"%s\", expected \"%s\"", (const gchar *) g_ptr_array_index (captured, n), p);
  }
  va_end (ap);
  g_assert_cmpuint (captured->len, ==, n);
  g_clear_pointer (&captured, g_ptr_array_unref);
}

static const RdsAnalyticsGroup *
group (const RdsAnalyticsConfig * cfg, guint pad, RdsAnalyticsFeature feature)
{
  const RdsAnalyticsStream *s = rds_analytics_config_get_stream (cfg, pad);

  g_assert_nonnull (s);
  g_assert_cmpuint (s->pad_index, ==, pad);
  return s->groups[feature];
}

static const RdsAnalyticsRule *
rule (const RdsAnalyticsGroup * g, guint i)
{
  g_assert_nonnull (g);
  g_assert_cmpuint (i, <, g->rules->len);
  return g_ptr_array_index (g->rules, i);
}

/* @rule's label and points, as "x,y x,y ..." */
static void
assert_rule (const RdsAnalyticsRule * r, const gchar * label, const gchar * points)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < r->n_points; i++)
    g_string_append_printf (s, "%s%d,%d", i ? " " : "", r->points[i].x, r->points[i].y);
  g_assert_cmpstr (r->label, ==, label);
  g_assert_cmpstr (s->str, ==, points);
  g_string_free (s, TRUE);
}

/* The required keys only: every other default (DeepStream 8.0 behaviour, obj-cnt-win-in-ms ours), no streams. */
static void
test_defaults (void)
{
  RdsAnalyticsConfig *cfg = text_ok (PROPERTY);

  g_assert_true (cfg->enable);
  g_assert_cmpuint (cfg->config_width, ==, 1280);
  g_assert_cmpuint (cfg->config_height, ==, 720);
  g_assert_cmpuint (cfg->osd_mode, ==, 2);
  g_assert_cmpuint (cfg->display_font_size, ==, 12);
  g_assert_cmpuint (cfg->obj_cnt_win_in_ms, ==, 50);
  g_assert_false (cfg->display_obj_cnt);
  g_assert_cmpuint (cfg->streams->len, ==, 0);
  g_assert_null (rds_analytics_config_get_stream (cfg, 0));
  rds_analytics_config_free (cfg);
}

/* Each [property] key read. */
static void
test_property (void)
{
  RdsAnalyticsConfig *cfg = text_ok ("[property]\nenable=0\nconfig-width=1920\nconfig-height=1080\nosd-mode=1\n"
      "display-font-size=20\nobj-cnt-win-in-ms=1000\ndisplay-obj-cnt=1\n");

  g_assert_false (cfg->enable);
  g_assert_cmpuint (cfg->config_width, ==, 1920);
  g_assert_cmpuint (cfg->config_height, ==, 1080);
  g_assert_cmpuint (cfg->osd_mode, ==, 1);
  g_assert_cmpuint (cfg->display_font_size, ==, 20);
  g_assert_cmpuint (cfg->obj_cnt_win_in_ms, ==, 1000);
  g_assert_true (cfg->display_obj_cnt);
  rds_analytics_config_free (cfg);

  /* booleans as true/false; font size 0 reads as 12; osd-mode 0 */
  cfg = text_ok ("[property]\nenable=true\nconfig-width=1\nconfig-height=1\nosd-mode=0\ndisplay-font-size=0\n"
      "display-obj-cnt=false\nobj-cnt-win-in-ms=1\n");
  g_assert_true (cfg->enable);
  g_assert_cmpuint (cfg->osd_mode, ==, 0);
  g_assert_cmpuint (cfg->display_font_size, ==, 12);
  g_assert_false (cfg->display_obj_cnt);
  g_assert_cmpuint (cfg->obj_cnt_win_in_ms, ==, 1);
  rds_analytics_config_free (cfg);
}

/* Every feature group and its keys, two streams, the rules in config order. */
static void
test_groups (void)
{
  RdsAnalyticsConfig *cfg = text_ok (PROPERTY
      "[roi-filtering-stream-0]\nenable=1\nroi-RF=295;643;579;634;642;913;56;828\nroi-B=0;0;10;0;10;10;\n"
      "inverse-roi=1\nclass-id=0;2\n"
      "[overcrowding-stream-0]\nenable=0\nroi-OC=1;1;5;1;5;5\nobject-threshold=3\ntime-threshold=500\n"
      "[line-crossing-stream-2]\nenable=1\nline-crossing-Exit=789;672;1084;900;851;773;1203;732\n"
      "line-crossing-In= 1 ; 2 ;3;4;5;6;7;8\nmode=strict\nextended=1\nclass-id=-1\n"
      "[direction-detection-stream-2]\nenable=1\ndirection-South=284;840;360;662\ndirection-North=0;10;0;0\n");
  const RdsAnalyticsGroup *g;

  g_assert_cmpuint (cfg->streams->len, ==, 2);
  g_assert_null (rds_analytics_config_get_stream (cfg, 1));

  g = group (cfg, 0, RDS_ANALYTICS_ROI_FILTERING);
  g_assert_nonnull (g);
  g_assert_cmpint (g->feature, ==, RDS_ANALYTICS_ROI_FILTERING);
  g_assert_cmpuint (g->pad_index, ==, 0);
  g_assert_true (g->enable);
  g_assert_true (g->inverse_roi);
  g_assert_nonnull (g->class_ids);
  g_assert_cmpuint (g->class_ids->len, ==, 2);
  g_assert_cmpint (g_array_index (g->class_ids, gint, 0), ==, 0);
  g_assert_cmpint (g_array_index (g->class_ids, gint, 1), ==, 2);
  g_assert_cmpuint (g->rules->len, ==, 2);
  assert_rule (rule (g, 0), "RF", "295,643 579,634 642,913 56,828");
  /* a trailing ';' is accepted */
  assert_rule (rule (g, 1), "B", "0,0 10,0 10,10");

  g = group (cfg, 0, RDS_ANALYTICS_OVERCROWDING);
  g_assert_nonnull (g);
  g_assert_false (g->enable);
  g_assert_cmpuint (g->object_threshold, ==, 3);
  g_assert_cmpuint (g->time_threshold, ==, 500);
  g_assert_null (g->class_ids);
  assert_rule (rule (g, 0), "OC", "1,1 5,1 5,5");
  g_assert_null (group (cfg, 0, RDS_ANALYTICS_LINE_CROSSING));
  g_assert_null (group (cfg, 0, RDS_ANALYTICS_DIRECTION));

  g = group (cfg, 2, RDS_ANALYTICS_LINE_CROSSING);
  g_assert_nonnull (g);
  g_assert_cmpuint (g->pad_index, ==, 2);
  g_assert_cmpint (g->mode, ==, RDS_ANALYTICS_MODE_STRICT);
  g_assert_true (g->extended);
  g_assert_null (g->class_ids);
  assert_rule (rule (g, 0), "Exit", "789,672 1084,900 851,773 1203,732");
  assert_rule (rule (g, 1), "In", "1,2 3,4 5,6 7,8");

  g = group (cfg, 2, RDS_ANALYTICS_DIRECTION);
  g_assert_nonnull (g);
  g_assert_cmpuint (g->rules->len, ==, 2);
  assert_rule (rule (g, 0), "South", "284,840 360,662");
  assert_rule (rule (g, 1), "North", "0,10 0,0");
  g_assert_null (group (cfg, 2, RDS_ANALYTICS_ROI_FILTERING));
  rds_analytics_config_free (cfg);
}

/* Group defaults: enable 1 (ours), every class, no inverse, threshold 0, loose, not extended. */
static void
test_group_defaults (void)
{
  RdsAnalyticsConfig *cfg = text_ok (PROPERTY
      "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\n"
      "[overcrowding-stream-0]\nroi-O=0;0;10;0;10;10\n"
      "[line-crossing-stream-0]\nline-crossing-L=0;0;0;10;0;5;10;5\n");
  const RdsAnalyticsGroup *g;

  for (guint f = 0; f < RDS_ANALYTICS_DIRECTION; f++) {
    g = group (cfg, 0, f);
    g_assert_nonnull (g);
    g_assert_true (g->enable);
    g_assert_null (g->class_ids);
  }
  g_assert_false (group (cfg, 0, RDS_ANALYTICS_ROI_FILTERING)->inverse_roi);
  g_assert_cmpuint (group (cfg, 0, RDS_ANALYTICS_OVERCROWDING)->object_threshold, ==, 0);
  g_assert_cmpuint (group (cfg, 0, RDS_ANALYTICS_OVERCROWDING)->time_threshold, ==, 0);
  g = group (cfg, 0, RDS_ANALYTICS_LINE_CROSSING);
  g_assert_cmpint (g->mode, ==, RDS_ANALYTICS_MODE_LOOSE);
  g_assert_false (g->extended);
  rds_analytics_config_free (cfg);
}

/* mode: the three values; unknown reads as loose; accepted and ignored in a direction group (no log line). */
static void
test_mode (void)
{
  static const struct
  {
    const gchar *value;
    RdsAnalyticsMode mode;
  } cases[] = {
    {"loose", RDS_ANALYTICS_MODE_LOOSE}, {"balanced", RDS_ANALYTICS_MODE_BALANCED},
    {"strict", RDS_ANALYTICS_MODE_STRICT}, {"superstrict", RDS_ANALYTICS_MODE_LOOSE},
  };
  RdsAnalyticsConfig *cfg;

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    gchar *text = g_strdup_printf (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=0;0;0;10;0;5;10;5\n"
        "mode=%s\n", cases[i].value);

    cfg = text_ok (text);
    g_assert_cmpint (group (cfg, 0, RDS_ANALYTICS_LINE_CROSSING)->mode, ==, cases[i].mode);
    rds_analytics_config_free (cfg);
    g_free (text);
  }

  capture_start ();
  cfg = text_ok (PROPERTY "[direction-detection-stream-0]\ndirection-D=0;0;0;10\nmode=strict\n");
  capture_stop (NULL);
  g_assert_nonnull (group (cfg, 0, RDS_ANALYTICS_DIRECTION));
  rds_analytics_config_free (cfg);
  /* empty too: it is a no-op key */
  capture_start ();
  cfg = text_ok (PROPERTY "[direction-detection-stream-0]\ndirection-D=0;0;0;10\nmode=\n");
  capture_stop (NULL);
  rds_analytics_config_free (cfg);
  /* matching is case-sensitive (ours) */
  cfg = text_ok (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=0;0;0;10;0;5;10;5\nmode=Strict\n");
  g_assert_cmpint (group (cfg, 0, RDS_ANALYTICS_LINE_CROSSING)->mode, ==, RDS_ANALYTICS_MODE_LOOSE);
  rds_analytics_config_free (cfg);
}

/* class-id: unset, empty, -1 and a list holding -1 all mean every class; a list keeps its classes. */
static void
test_class_id (void)
{
  static const struct
  {
    const gchar *line;
    const gchar *ids;           /* NULL: every class */
  } cases[] = {
    {"", NULL}, {"class-id=\n", NULL}, {"class-id=-1\n", NULL}, {"class-id=-1;1\n", NULL}, {"class-id=1;-1\n", NULL},
    {"class-id=0;2\n", "0 2"}, {"class-id=7\n", "7"}, {"class-id=3;\n", "3"}, {"class-id=;\n", NULL},
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    gchar *text = g_strdup_printf (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\n%s", cases[i].line);
    RdsAnalyticsConfig *cfg = text_ok (text);
    const RdsAnalyticsGroup *g = group (cfg, 0, RDS_ANALYTICS_ROI_FILTERING);

    if (!cases[i].ids) {
      g_assert_null (g->class_ids);
    } else {
      GString *s = g_string_new (NULL);

      g_assert_nonnull (g->class_ids);
      for (guint j = 0; j < g->class_ids->len; j++)
        g_string_append_printf (s, "%s%d", j ? " " : "", g_array_index (g->class_ids, gint, j));
      g_assert_cmpstr (s->str, ==, cases[i].ids);
      g_string_free (s, TRUE);
    }
    rds_analytics_config_free (cfg);
    g_free (text);
  }
}

/* Each error names the group and the key. */
static void
test_errors (void)
{
  GError *err = NULL;
  const gint INVALID = RDS_ANALYTICS_CONFIG_ERROR_INVALID;

  g_assert_null (rds_analytics_config_load ("/nonexistent/analytics.txt", &err));
  g_assert_error (err, RDS_ANALYTICS_CONFIG_ERROR, RDS_ANALYTICS_CONFIG_ERROR_FILE);
  g_assert_nonnull (strstr (err->message, "/nonexistent/analytics.txt"));
  g_clear_error (&err);

  text_fails ("[property\nenable=1\n", RDS_ANALYTICS_CONFIG_ERROR_PARSE, NULL);
  text_fails ("", INVALID, "[property]", "missing group", NULL);
  text_fails ("[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\n", INVALID, "[property]", "missing group", NULL);
  /* the required keys */
  text_fails ("[property]\nconfig-width=1280\nconfig-height=720\n", INVALID, "[property] enable", "missing", NULL);
  text_fails ("[property]\nenable=1\nconfig-height=720\n", INVALID, "[property] config-width", "missing", NULL);
  text_fails ("[property]\nenable=1\nconfig-width=1280\n", INVALID, "[property] config-height", "missing", NULL);
  text_fails ("[property]\nenable=1\nconfig-width=0\nconfig-height=720\n", INVALID, "[property] config-width: 0: ",
      "at least 1", NULL);
  text_fails ("[property]\nenable=1\nconfig-width=1280\nconfig-height=0\n", INVALID,
      "[property] config-height: 0: ", "at least 1", NULL);
  /* types and ranges */
  text_fails ("[property]\nenable=maybe\nconfig-width=1280\nconfig-height=720\n", INVALID,
      "[property] enable: maybe: ", "not a boolean", NULL);
  text_fails (PROPERTY "osd-mode=3\n", INVALID, "[property] osd-mode: 3: ", "at most 2", NULL);
  text_fails (PROPERTY "osd-mode=1.5\n", INVALID, "[property] osd-mode: 1.5: ", "not an integer", NULL);
  text_fails (PROPERTY "obj-cnt-win-in-ms=0\n", INVALID, "[property] obj-cnt-win-in-ms: 0: ", "at least 1", NULL);
  text_fails (PROPERTY "obj-cnt-win-in-ms=1000000001\n", INVALID, "[property] obj-cnt-win-in-ms: 1000000001: ",
      "at most 1000000000", NULL);
  text_fails (PROPERTY "display-font-size=-1\n", INVALID, "[property] display-font-size: -1: ", "at least 0", NULL);
  text_fails (PROPERTY "[overcrowding-stream-0]\nroi-O=0;0;10;0;10;10\nobject-threshold=-1\n", INVALID,
      "[overcrowding-stream-0] object-threshold: -1: ", "at least 0", NULL);
  text_fails (PROPERTY "[overcrowding-stream-0]\nroi-O=0;0;10;0;10;10\ntime-threshold=2.5\n", INVALID,
      "[overcrowding-stream-0] time-threshold: 2.5: ", "not an integer", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\nclass-id=a\n", INVALID,
      "[roi-filtering-stream-0] class-id: a: ", "not an integer", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\nclass-id=-2\n", INVALID,
      "[roi-filtering-stream-0] class-id: -2: ", "at least -1", NULL);
  /* empty values */
  text_fails (PROPERTY "osd-mode=\n", INVALID, "[property] osd-mode: : ", "empty value", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=\n", INVALID, "[roi-filtering-stream-0] roi-A: : ",
      "empty value", NULL);
  text_fails (PROPERTY "[overcrowding-stream-0]\nroi-A=\n", INVALID, "[overcrowding-stream-0] roi-A: : ",
      "empty value", NULL);
  text_fails (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=\n", INVALID,
      "[line-crossing-stream-0] line-crossing-L: : ", "empty value", NULL);
  text_fails (PROPERTY "[direction-detection-stream-0]\ndirection-D=\n", INVALID,
      "[direction-detection-stream-0] direction-D: : ", "empty value", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\nenable=\n", INVALID,
      "[roi-filtering-stream-0] enable: : ", "empty value", NULL);
  /* ROI polygons */
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10\n", INVALID,
      "[roi-filtering-stream-0] roi-A: 0;0;10;0;10: ", "odd number of values", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;10\n", INVALID,
      "[roi-filtering-stream-0] roi-A: 0;0;10;10: ", "at least 3 points", NULL);
  text_fails (PROPERTY "[overcrowding-stream-0]\nroi-A=0;0\n", INVALID, "[overcrowding-stream-0] roi-A: 0;0: ",
      "at least 3 points", NULL);
  /* lines and directions */
  text_fails (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=1;2;3;4;5;6\n", INVALID,
      "[line-crossing-stream-0] line-crossing-L: 1;2;3;4;5;6: ", "8 values", NULL);
  text_fails (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=1;2;3;4;5;6;7;8;9\n", INVALID,
      "[line-crossing-stream-0] line-crossing-L: 1;2;3;4;5;6;7;8;9: ", "8 values", NULL);
  text_fails (PROPERTY "[direction-detection-stream-0]\ndirection-D=1;2;3\n", INVALID,
      "[direction-detection-stream-0] direction-D: 1;2;3: ", "4 values", NULL);
  text_fails (PROPERTY "[direction-detection-stream-0]\ndirection-D=1;2;3;4;5\n", INVALID,
      "[direction-detection-stream-0] direction-D: 1;2;3;4;5: ", "4 values", NULL);
  /* coordinates: integers >= 0 */
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;-1;10;0;10;10\n", INVALID,
      "[roi-filtering-stream-0] roi-A: 0;-1;10;0;10;10: ", "-1: must be at least 0", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;100.5;10;0;10;10\n", INVALID,
      "[roi-filtering-stream-0] roi-A: 0;100.5;10;0;10;10: ", "100.5: not an integer", NULL);
  text_fails (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=1;2;3;4;5;6;7;x\n", INVALID,
      "[line-crossing-stream-0] line-crossing-L: 1;2;3;4;5;6;7;x: ", "not an integer", NULL);
  text_fails (PROPERTY "[direction-detection-stream-0]\ndirection-D=1;2;3;-4\n", INVALID,
      "[direction-detection-stream-0] direction-D: 1;2;3;-4: ", "at least 0", NULL);
  /* duplicate labels in one feature of one stream, also across repeated group headers */
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\nroi-A=0;0;20;0;20;20\n", INVALID,
      "[roi-filtering-stream-0] roi-A", "duplicate label", NULL);
  text_fails (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=0;0;0;10;0;5;10;5\n"
      "[line-crossing-stream-0]\nline-crossing-L=0;0;0;10;0;5;10;5\n", INVALID,
      "[line-crossing-stream-0] line-crossing-L", "duplicate label", NULL);
  /* ours: a label in both the roi-filtering and the overcrowding group of a stream, whichever comes first */
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\n"
      "[overcrowding-stream-0]\nroi-A=0;0;20;0;20;20\n", INVALID,
      "[overcrowding-stream-0] roi-A", "label already used by [roi-filtering-stream-0]", NULL);
  text_fails (PROPERTY "[overcrowding-stream-3]\nroi-B=0;0;20;0;20;20\n"
      "[roi-filtering-stream-3]\nroi-A=0;0;10;0;10;10\nroi-B=0;0;10;0;10;10\n", INVALID,
      "[roi-filtering-stream-3] roi-B", "label already used by [overcrowding-stream-3]", NULL);
  /* a feature group with no rule (DeepStream 8.0 behaviour), a bad stream number, an empty label (ours) */
  text_fails (PROPERTY "[roi-filtering-stream-0]\nenable=1\n", INVALID, "[roi-filtering-stream-0]", "no roi-<label>",
      NULL);
  text_fails (PROPERTY "[line-crossing-stream-0]\nenable=0\n", INVALID, "[line-crossing-stream-0]",
      "no line-crossing-<label>", NULL);
  text_fails (PROPERTY "[direction-detection-stream-1]\n", INVALID, "[direction-detection-stream-1]",
      "no direction-<label>", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-x]\nroi-A=0;0;10;0;10;10\n", INVALID, "[roi-filtering-stream-x]",
      "stream number", NULL);
  text_fails (PROPERTY "[roi-filtering-stream--1]\nroi-A=0;0;10;0;10;10\n", INVALID, "[roi-filtering-stream--1]",
      "stream number", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-=0;0;10;0;10;10\n", INVALID, "[roi-filtering-stream-0] roi-",
      "empty label", NULL);
  /* ours: stream numbers are written without leading zeros, so two spellings never name one stream */
  text_fails (PROPERTY "[roi-filtering-stream-00]\nroi-A=0;0;10;0;10;10\n", INVALID, "[roi-filtering-stream-00]",
      "00: not a stream number", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\n[roi-filtering-stream-00]\n"
      "roi-B=0;0;10;0;10;10\n", INVALID, "[roi-filtering-stream-00]", "not a stream number", NULL);
  text_fails (PROPERTY "[line-crossing-stream-7]\nline-crossing-L=0;0;0;10;0;5;10;5\n[line-crossing-stream-007]\n"
      "line-crossing-M=0;0;0;10;0;5;10;5\n", INVALID, "[line-crossing-stream-007]", "not a stream number", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-99999999999]\nroi-A=0;0;10;0;10;10\n", INVALID,
      "[roi-filtering-stream-99999999999]", "not a stream number", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-]\nroi-A=0;0;10;0;10;10\n", INVALID, "[roi-filtering-stream-]",
      "not a stream number", NULL);
  /* list items, upper bounds */
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;;0;10;10\n", INVALID,
      "[roi-filtering-stream-0] roi-A: 0;0;;0;10;10: ", "item 3 is empty", NULL);
  text_fails (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\nclass-id=1;;2\n", INVALID,
      "[roi-filtering-stream-0] class-id: 1;;2: ", "item 2 is empty", NULL);
  text_fails ("[property]\nenable=1\nconfig-width=2147483648\nconfig-height=720\n", INVALID,
      "[property] config-width: 2147483648: ", "at most 2147483647", NULL);
  text_fails (PROPERTY "[direction-detection-stream-0]\ndirection-D=0;0;2147483648;0\n", INVALID,
      "[direction-detection-stream-0] direction-D: 0;0;2147483648;0: ", "2147483648: must be at most 2147483647", NULL);
}

/* The same label in two streams, or in features that don't share a map (an ROI and a direction or a line), is fine;
 * roi-filtering and overcrowding share objInROIcnt (see the errors). */
static void
test_labels_per_feature (void)
{
  RdsAnalyticsConfig *cfg = text_ok (PROPERTY
      "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\n[direction-detection-stream-0]\ndirection-A=0;0;10;0\n"
      "[line-crossing-stream-0]\nline-crossing-A=0;0;0;10;0;5;10;5\n"
      "[overcrowding-stream-0]\nroi-B=0;0;10;0;10;10\n[overcrowding-stream-1]\nroi-A=0;0;10;0;10;10\n");

  g_assert_cmpstr (rule (group (cfg, 0, RDS_ANALYTICS_DIRECTION), 0)->label, ==, "A");
  g_assert_cmpstr (rule (group (cfg, 0, RDS_ANALYTICS_LINE_CROSSING), 0)->label, ==, "A");
  g_assert_cmpstr (rule (group (cfg, 1, RDS_ANALYTICS_OVERCROWDING), 0)->label, ==, "A");
  rds_analytics_config_free (cfg);
}

/* [<feature>-<n>] (without "stream") is skipped with a log line naming the group (DeepStream 8.0 behaviour). */
static void
test_skipped_groups (void)
{
  static const gchar *const groups[] = {
    "roi-filtering-0", "overcrowding-0", "line-crossing-0", "direction-detection-0",
  };

  for (guint i = 0; i < G_N_ELEMENTS (groups); i++) {
    gchar *text = g_strdup_printf (PROPERTY "[%s]\nenable=1\nroi-A=0;0;10;0;10;10\n", groups[i]);
    gchar *pattern = g_strdup_printf ("*[%s]*ignored*", groups[i]);
    RdsAnalyticsConfig *cfg;

    capture_start ();
    cfg = text_ok (text);
    capture_stop (pattern, NULL);
    g_assert_cmpuint (cfg->streams->len, ==, 0);
    rds_analytics_config_free (cfg);
    g_free (pattern);
    g_free (text);
  }
}

/* Unknown keys: silent in [property], a log line in feature groups (DeepStream 8.0 behaviour); time-threshold is a
 * key of overcrowding groups only, a rule key of another feature is unknown. */
static void
test_unknown_keys (void)
{
  RdsAnalyticsConfig *cfg;

  capture_start ();
  cfg = text_ok (PROPERTY "bar-baz=1\nroi-A=1\n");
  capture_stop (NULL);
  rds_analytics_config_free (cfg);

  capture_start ();
  cfg = text_ok (PROPERTY "[overcrowding-stream-0]\nroi-A=0;0;10;0;10;10\nfoo-bar=1\n");
  capture_stop ("*[overcrowding-stream-0] foo-bar*unknown key*", NULL);
  rds_analytics_config_free (cfg);

  capture_start ();
  cfg = text_ok (PROPERTY "[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\ntime-threshold=5\n");
  capture_stop ("*[roi-filtering-stream-0] time-threshold*unknown key*", NULL);
  rds_analytics_config_free (cfg);

  capture_start ();
  cfg = text_ok (PROPERTY "[line-crossing-stream-0]\nline-crossing-L=0;0;0;10;0;5;10;5\nroi-A=0;0;10;0;10;10\n"
      "inverse-roi=1\n");
  capture_stop ("*[line-crossing-stream-0] roi-A*unknown key*", "*[line-crossing-stream-0] inverse-roi*unknown key*",
      NULL);
  g_assert_cmpuint (group (cfg, 0, RDS_ANALYTICS_LINE_CROSSING)->rules->len, ==, 1);
  rds_analytics_config_free (cfg);

  /* locale-suffixed keys are unknown keys: logged in feature groups, silent in [property] */
  capture_start ();
  cfg = text_ok (PROPERTY "osd-mode[fr]=1\n[roi-filtering-stream-0]\nroi-A=0;0;10;0;10;10\n"
      "roi-A[fr]=0;0;20;0;20;20\nenable[fr]=0\n");
  capture_stop ("*[roi-filtering-stream-0] roi-A[fr]*unknown key*", "*[roi-filtering-stream-0] enable[fr]*unknown key*",
      NULL);
  g_assert_cmpuint (cfg->osd_mode, ==, 2);
  g_assert_cmpuint (group (cfg, 0, RDS_ANALYTICS_ROI_FILTERING)->rules->len, ==, 1);
  assert_rule (rule (group (cfg, 0, RDS_ANALYTICS_ROI_FILTERING), 0), "A", "0,0 10,0 10,10");
  g_assert_true (group (cfg, 0, RDS_ANALYTICS_ROI_FILTERING)->enable);
  rds_analytics_config_free (cfg);

  /* a feature name followed by something other than -stream-<n> or -<n> is an unknown group */
  capture_start ();
  cfg = text_ok (PROPERTY "[roi-filtering-foo]\nroi-A=0;0;10;0;10;10\n");
  capture_stop ("*[roi-filtering-foo]*unknown group*", NULL);
  g_assert_cmpuint (cfg->streams->len, ==, 0);
  rds_analytics_config_free (cfg);

  /* a group that is no feature at all (ours: logged too) */
  capture_start ();
  cfg = text_ok (PROPERTY "[foo]\nx=1\n");
  capture_stop ("*[foo]*ignored*", NULL);
  rds_analytics_config_free (cfg);
}

/* Every key of the table: its default loads, a wrong type and the range ends fail naming the key. */
typedef struct
{
  guint n_keys;
  GHashTable *names;
} KeyWalk;

/* A config holding @line in a group of @key's first group kind, with that kind's rule. */
static gchar *
key_config (const RdsAnalyticsKeyDesc * key, const gchar * line, gchar ** group_name)
{
  static const struct
  {
    const gchar *kind, *rule;
  } kinds[] = {
    {"roi-filtering", "roi-Z=0;0;10;0;10;10\n"}, {"overcrowding", "roi-Z=0;0;10;0;10;10\n"},
    {"line-crossing", "line-crossing-Z=0;0;0;10;0;5;10;5\n"}, {"direction-detection", "direction-Z=0;0;0;10\n"},
  };

  if (!strcmp (key->groups, "property")) {
    *group_name = g_strdup ("property");
    return g_strdup_printf (PROPERTY "%s", line);
  }
  for (guint i = 0; i < G_N_ELEMENTS (kinds); i++)
    if (g_str_has_prefix (key->groups, kinds[i].kind)) {
      *group_name = g_strdup_printf ("%s-stream-0", kinds[i].kind);
      return g_strdup_printf (PROPERTY "[%s]\n%s%s", *group_name, kinds[i].rule, line);
    }
  g_error ("%s: unknown groups \"%s\"", key->name, key->groups);
  return NULL;
}

static void
walk_key (const RdsAnalyticsKeyDesc * key, gpointer user_data)
{
  KeyWalk *kw = user_data;
  gboolean label = strstr (key->name, "<label>") != NULL;
  gchar *name = label ? g_strndup (key->name, strstr (key->name, "<label>") - key->name) : g_strdup (key->name);
  gchar *gname = NULL;

  if (label) {
    gchar *full = g_strconcat (name, "Y", NULL);

    g_free (name);
    name = full;
  }
  kw->n_keys++;
  g_hash_table_add (kw->names, g_strconcat (key->groups, ".", key->name, NULL));
  if (key->status != RDS_ANALYTICS_KEY_SUPPORTED)
    g_assert_nonnull (key->note);

  if (key->default_value) {
    gchar *line = g_strdup_printf ("%s=%s\n", name, key->default_value);
    gchar *text = key_config (key, line, &gname);
    RdsAnalyticsConfig *cfg;

    /* [property]'s required keys are in PROPERTY already: written twice, the last wins */
    cfg = text_ok (text);
    rds_analytics_config_free (cfg);
    g_free (text);
    g_free (line);
    g_clear_pointer (&gname, g_free);
  }
  if (key->range || !strcmp (key->type, "bool")) {
    gchar *line = g_strdup_printf ("%s=abc\n", name);
    gchar *text = key_config (key, line, &gname);
    gchar *want = g_strdup_printf ("[%s] %s: abc: ", gname, name);

    text_fails (text, RDS_ANALYTICS_CONFIG_ERROR_INVALID, want, NULL);
    g_free (want);
    g_free (text);
    g_free (line);
    g_clear_pointer (&gname, g_free);
  }
  if (key->range) {
    gchar **ends = g_strsplit (g_str_has_prefix (key->range, ">= ") ? key->range + 3 : key->range, "..", 2);
    gdouble bounds[2] = { g_ascii_strtod (ends[0], NULL) - 1, ends[1] ? g_ascii_strtod (ends[1], NULL) + 1 : 0 };
    /* rules: a valid value with one coordinate out of range */
    const gchar *pad = !strcmp (key->type, "polygon") ? "0;0;10;0;10;" : !strcmp (key->type, "line")
        ? "0;0;0;10;0;5;10;" : !strcmp (key->type, "vector") ? "0;0;0;" : "";

    for (guint b = 0; b < (ends[1] ? 2u : 1u); b++) {
      gchar num[G_ASCII_DTOSTR_BUF_SIZE];
      const gchar *value = g_ascii_formatd (num, sizeof (num), "%.0f", bounds[b]);
      gchar *line = g_strdup_printf ("%s=%s%s\n", name, pad, value);
      gchar *text = key_config (key, line, &gname);
      gchar *want = g_strdup_printf ("[%s] %s: %s%s: ", gname, name, pad, value);

      text_fails (text, RDS_ANALYTICS_CONFIG_ERROR_INVALID, want, b == 0 ? "at least" : "at most", NULL);
      g_free (want);
      g_free (text);
      g_free (line);
      g_clear_pointer (&gname, g_free);
    }
    g_strfreev (ends);
  }
  g_free (name);
}

/* Every key DeepStream 8.0 reads (its public docs, and the probe's undocumented ones), by group kind. */
static const gchar *const known[] = {
  "property.enable", "property.config-width", "property.config-height", "property.osd-mode",
  "property.display-font-size", "property.obj-cnt-win-in-ms", "property.display-obj-cnt",
  "roi-filtering, overcrowding, line-crossing, direction-detection.enable",
  "roi-filtering, overcrowding, line-crossing, direction-detection.class-id",
  "roi-filtering.inverse-roi", "overcrowding.object-threshold", "overcrowding.time-threshold",
  "line-crossing.mode", "direction-detection.mode", "line-crossing.extended",
  "roi-filtering, overcrowding.roi-<label>", "line-crossing.line-crossing-<label>",
  "direction-detection.direction-<label>",
};

static void
test_key_table (void)
{
  KeyWalk kw = { 0, g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL) };
  GError *err = NULL;

  g_assert_true (rds_analytics_config_check_table (&err));
  g_assert_no_error (err);
  rds_analytics_config_foreach_key (walk_key, &kw);
  for (guint i = 0; i < G_N_ELEMENTS (known); i++)
    if (!g_hash_table_contains (kw.names, known[i]))
      g_error ("%s is not in the key table", known[i]);
  g_assert_cmpuint (kw.n_keys, ==, G_N_ELEMENTS (known));
  g_hash_table_unref (kw.names);
}

/* docs/compat-matrix.md lists every config key in the "rdsanalytics config keys" table, with the table's groups,
 * type, default, range, status and note; and nothing else there. Rows are
 * "| `KEY` | GROUPS | TYPE | `DEFAULT`, `DEFAULT` (ours), required or none | RANGE or empty | STATUS | notes |". */
typedef struct
{
  GHashTable *rows;             /* "groups.name" -> gchar ** cells (name, groups, type, default, range, status,
                                 * notes) */
  guint n_bad;
} AnalyticsMatrixCheck;

static const gchar *const analytics_matrix_status[] = {
  [RDS_ANALYTICS_KEY_SUPPORTED] = "supported",
  [RDS_ANALYTICS_KEY_NOOP] = "accepted no-op",
};

static void
check_matrix_key (const RdsAnalyticsKeyDesc * key, gpointer user_data)
{
  AnalyticsMatrixCheck *mc = user_data;
  gchar *id = g_strconcat (key->groups, ".", key->name, NULL);
  gchar *def = key->default_value ? g_strdup_printf ("`%s`%s", key->default_value, key->default_ours ? " (ours)" : "")
      : g_strdup (key->required ? "required" : "none");
  gchar **cells = g_hash_table_lookup (mc->rows, id);
  const gchar *range = key->range ? key->range : "";
  const gchar *status = analytics_matrix_status[key->status];

  if (!cells || strcmp (cells[1], key->groups) != 0 || strcmp (cells[2], key->type) != 0
      || strcmp (cells[3], def) != 0 || strcmp (cells[4], range) != 0 || strcmp (cells[5], status) != 0
      || (key->note && !strstr (cells[6], key->note))) {
    g_test_message ("compat matrix, rdsanalytics config keys: %s %s; expected: | `%s` | %s | %s | %s | %s | %s | %s |",
        id, cells ? "differs" : "is missing", key->name, key->groups, key->type, def, range, status,
        key->note ? key->note : "");
    mc->n_bad++;
  }
  g_hash_table_remove (mc->rows, id);
  g_free (def);
  g_free (id);
}

static void
test_compat_matrix (void)
{
  AnalyticsMatrixCheck mc = { g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_strfreev),
    0
  };
  gchar *text = NULL, **lines;
  gboolean in_table = FALSE;
  GHashTableIter it;
  gpointer id;
  GError *err = NULL;

  g_file_get_contents (RDS_COMPAT_MATRIX, &text, NULL, &err);
  g_assert_no_error (err);
  g_assert_nonnull (text);
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    gchar **cells, *id;

    if (lines[i][0] == '#') {
      in_table = strstr (lines[i], "rdsanalytics config keys") != NULL;
      continue;
    }
    if (!in_table || !g_str_has_prefix (lines[i], "| `"))
      continue;
    /* "| a | b |" splits into "", " a ", " b ", "" */
    cells = g_strsplit (lines[i] + 1, "|", -1);
    for (guint c = 0; cells[c]; c++)
      g_strstrip (cells[c]);
    if (g_strv_length (cells) != 8 || !g_str_has_prefix (cells[0], "`") || !g_str_has_suffix (cells[0], "`")) {
      g_test_message ("compat-matrix.md:%u: %s: expected 7 cells, the first a `key`", i + 1, lines[i]);
      mc.n_bad++;
      g_strfreev (cells);
      continue;
    }
    cells[0][strlen (cells[0]) - 1] = '\0';
    id = g_strconcat (cells[1], ".", cells[0] + 1, NULL);
    if (g_hash_table_contains (mc.rows, id)) {
      g_test_message ("compat-matrix.md:%u: %s: duplicate row", i + 1, lines[i]);
      mc.n_bad++;
      g_free (id);
      g_strfreev (cells);
      continue;
    }
    g_hash_table_insert (mc.rows, id, cells);
  }
  rds_analytics_config_foreach_key (check_matrix_key, &mc);
  g_hash_table_iter_init (&it, mc.rows);
  while (g_hash_table_iter_next (&it, &id, NULL)) {
    g_test_message ("compat matrix: %s is not in the key table", (const gchar *) id);
    mc.n_bad++;
  }
  g_assert_cmpuint (mc.n_bad, ==, 0);
  g_strfreev (lines);
  g_free (text);
  g_hash_table_unref (mc.rows);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/analytics-config/defaults", test_defaults);
  g_test_add_func ("/cpu/analytics-config/property", test_property);
  g_test_add_func ("/cpu/analytics-config/groups", test_groups);
  g_test_add_func ("/cpu/analytics-config/group-defaults", test_group_defaults);
  g_test_add_func ("/cpu/analytics-config/mode", test_mode);
  g_test_add_func ("/cpu/analytics-config/class-id", test_class_id);
  g_test_add_func ("/cpu/analytics-config/errors", test_errors);
  g_test_add_func ("/cpu/analytics-config/labels-per-feature", test_labels_per_feature);
  g_test_add_func ("/cpu/analytics-config/skipped-groups", test_skipped_groups);
  g_test_add_func ("/cpu/analytics-config/unknown-keys", test_unknown_keys);
  g_test_add_func ("/cpu/analytics-config/key-table", test_key_table);
  g_test_add_func ("/cpu/analytics-config/compat-matrix", test_compat_matrix);
  return g_test_run ();
}
