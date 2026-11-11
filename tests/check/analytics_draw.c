/* rdsanalytics overlays: the display meta and the object text of each osd-mode and of display-obj-cnt, built from
 * the analytics core's result on scripted frames, field by field against DeepStream 8.0's display-meta dumps of the
 * same configs and objects (the numbers below), and the cases where ours differs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-draw.h"

#include <glib/gstdio.h>
#include <gst/gst.h>
#include <rds/rds.h>
#include <string.h>

/* ---- the scenarios ---- */

#define FRAME_30FPS(i) ((guint64) (i) * G_GUINT64_CONSTANT (1000000000) / 30)

/* One rule of each feature on stream 0, at 1280 x 720: an ROI on the left, an overcrowding area on the right
 * (threshold 1), a direction pointing east along the bottom, a horizontal line crossed downwards in the middle. */
#define FOUR_RULES_GROUPS \
  "[roi-filtering-stream-0]\nenable=@ROI@\nroi-RF=100;100;600;100;600;600;100;600\nclass-id=-1\n\n" \
  "[overcrowding-stream-0]\nenable=1\nroi-OC=700;100;1200;100;1200;600;700;600\nclass-id=-1\n" \
  "object-threshold=1\n\n" \
  "[direction-detection-stream-0]\nenable=1\ndirection-East=100;650;300;650\nclass-id=-1\n\n" \
  "[line-crossing-stream-0]\nenable=1\nline-crossing-Down=640;300;640;500;500;400;800;400\nclass-id=-1\n" \
  "extended=0\nmode=loose\n"

/* The walkers: 20 x 40 boxes of class 0. Object 1 walks east through the ROI from left 140, 5 px a frame; object 2
 * walks down across the line from top 260, 5 px a frame; object 3 stands in the overcrowding area. */
static guint
walkers (guint frame, RdsAnalyticsObject * o)
{
  o[0] = (RdsAnalyticsObject) {.id = 1,.class_id = 0,.left = 140 + 5 * frame,.top = 260,.width = 20,.height = 40 };
  o[1] = (RdsAnalyticsObject) {.id = 2,.class_id = 0,.left = 630,.top = 260 + 5 * frame,.width = 20,.height = 40 };
  o[2] = (RdsAnalyticsObject) {.id = 3,.class_id = 0,.left = 890,.top = 260,.width = 20,.height = 40 };
  return 3;
}

/* Twelve still 20 x 40 boxes, two of each class 0, 1, 2, 3, -1 and 7: the first six in the left half, the others in
 * the right. */
static guint
class_mix (guint frame, RdsAnalyticsObject * o)
{
  static const gint classes[] = { 0, 1, 2, 3, -1, 7 };

  (void) frame;
  for (guint i = 0; i < 6; i++) {
    o[i] = (RdsAnalyticsObject) {.id = 1 + i,.class_id = classes[i],.left = 90 + 80 * i,.top = 260,.width = 20,
      .height = 40 };
    o[6 + i] = (RdsAnalyticsObject) {.id = 20 + i,.class_id = classes[i],.left = 890 + 40 * i,.top = 260,
      .width = 20,.height = 40 };
  }
  return 12;
}

/* Four 40 x 100 boxes: one in the ROI, one between the areas, one in the overcrowding area and an untracked one of
 * class 2 in it too. */
static guint
mixed_boxes (guint frame, RdsAnalyticsObject * o)
{
  (void) frame;
  o[0] = (RdsAnalyticsObject) {.id = 1,.class_id = 0,.left = 200,.top = 200,.width = 40,.height = 100 };
  o[1] = (RdsAnalyticsObject) {.id = 2,.class_id = 0,.left = 620,.top = 250,.width = 40,.height = 100 };
  o[2] = (RdsAnalyticsObject) {.id = 3,.class_id = 0,.left = 800,.top = 200,.width = 40,.height = 100 };
  o[3] = (RdsAnalyticsObject) {.id = RDS_ANALYTICS_UNTRACKED,.class_id = 2,.left = 900,.top = 300,.width = 40,
    .height = 100 };
  return 4;
}

static guint
no_objects (guint frame, RdsAnalyticsObject * o)
{
  (void) frame;
  (void) o;
  return 0;
}

typedef guint (*ObjectsFunc) (guint frame, RdsAnalyticsObject * out);

#define MAX_OBJS 16

/* ---- expected values ---- */

#define YELLOW { 1, 1, 0, 1 }
#define ORANGE { 1, 0.5, 0, 1 }
#define RED { 1, 0, 0, 1 }
#define GREEN { 0, 1, 0, 1 }
#define CYAN { 0, 1, 1, 1 }
#define WHITE { 1, 1, 1, 1 }
#define BLACK { 0, 0, 0, 1 }

/* the scripted objects' own border, as the probe gave them */
static const RdsOSD_ColorParams own_border = RED;

typedef struct
{
  guint x1, y1, x2, y2;
  RdsOSD_ColorParams c;
} ExpLine;

typedef struct
{
  const gchar *s;
  guint x, y;
  RdsOSD_ColorParams c;
} ExpText;

/* An object's text and border after the overlays: s NULL is an untouched object (no text, its own border). */
typedef struct
{
  const gchar *s;
  guint x, y;
  gboolean cyan;
} ExpObj;

#define UNTOUCHED { NULL, 0, 0, FALSE }

/* The four rules' lines at 1280 x 720, in DeepStream 8.0's order. */
#define ROI_RF_LINES \
  { 100, 100, 600, 100, YELLOW }, { 600, 100, 600, 600, YELLOW }, { 600, 600, 100, 600, YELLOW }, \
  { 100, 600, 100, 100, YELLOW }
#define OC_LINES \
  { 700, 100, 1200, 100, ORANGE }, { 1200, 100, 1200, 600, ORANGE }, { 1200, 600, 700, 600, ORANGE }, \
  { 700, 600, 700, 100, ORANGE }
/* a 200 px vector: arrowhead lines 30 px long at -15 and +15 degrees */
#define DIR_EAST_LINES \
  { 100, 650, 300, 650, RED }, { 271, 657, 300, 650, RED }, { 271, 642, 300, 650, RED }
#define LC_DOWN_LINES \
  { 640, 300, 640, 500, GREEN }, { 632, 471, 640, 500, GREEN }, { 647, 471, 640, 500, GREEN }, \
  { 500, 400, 800, 400, GREEN }

static const ExpLine four_rules_lines[] = { ROI_RF_LINES, OC_LINES, DIR_EAST_LINES, LC_DOWN_LINES };

/* ---- running a scenario ---- */

/* @text written to a temp file and loaded; the file is gone on return. */
static RdsAnalyticsConfig *
load_config (const gchar * text)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("rds-analytics-draw-XXXXXX", &err);
  gchar *path;
  RdsAnalyticsConfig *cfg;

  g_assert_no_error (err);
  path = g_build_filename (dir, "analytics.txt", NULL);
  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  cfg = rds_analytics_config_load (path, &err);
  g_assert_no_error (err);
  g_assert_nonnull (cfg);
  g_unlink (path);
  g_rmdir (dir);
  g_free (path);
  g_free (dir);
  return cfg;
}

/* [property] with @osd_mode, @obj_cnt and @font_size (0: unset), then @groups. */
static gchar *
config_text (guint osd_mode, gboolean obj_cnt, guint font_size, const gchar * groups)
{
  GString *s = g_string_new (NULL);

  g_string_append_printf (s, "[property]\nenable=1\nconfig-width=1280\nconfig-height=720\nosd-mode=%u\n", osd_mode);
  if (obj_cnt)
    g_string_append (s, "display-obj-cnt=1\n");
  if (font_size)
    g_string_append_printf (s, "display-font-size=%u\n", font_size);
  g_string_append_printf (s, "\n%s", groups);
  return g_string_free (s, FALSE);
}

/* The four rules' groups, with the ROI group enabled or not. */
static gchar *
four_rules (gboolean roi_enabled)
{
  GString *s = g_string_new (FOUR_RULES_GROUPS);

  g_string_replace (s, "@ROI@", roi_enabled ? "1" : "0", 1);
  return g_string_free (s, FALSE);
}

typedef struct
{
  RdsAnalyticsConfig *cfg;
  RdsAnalyticsCore *core;
  RdsBatchMeta *bm;
  RdsFrameMeta *fm;
  RdsObjectMeta *objs[MAX_OBJS];
  guint n_objs;
} Drawn;

/* Runs frames 0 .. @frame - 1 of @objects through a core for @config at @w x @h, then frame @frame in a batch meta,
 * whose object metas get a 2 px border in the probe's red and no text, and draws it. */
static void
draw_scenario (Drawn * d, const gchar * config, guint w, guint h, ObjectsFunc objects, guint frame)
{
  RdsAnalyticsObject o[MAX_OBJS];
  RdsAnalyticsFrameResult res;
  guint n = 0;

  memset (d, 0, sizeof *d);
  d->cfg = load_config (config);
  d->core = rds_analytics_core_new (d->cfg, w, h);
  for (guint f = 0; f < frame; f++) {
    n = objects (f, o);
    rds_analytics_core_process_frame (d->core, 0, FRAME_30FPS (f), o, n, &res);
  }
  n = objects (frame, o);
  g_assert_cmpuint (n, <=, MAX_OBJS);
  d->bm = rds_create_batch_meta (1);
  d->fm = rds_acquire_frame_meta_from_pool (d->bm);
  d->fm->pad_index = 0;
  d->fm->buf_pts = FRAME_30FPS (frame);
  rds_add_frame_meta_to_batch (d->bm, d->fm);
  for (guint i = 0; i < n; i++) {
    RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (d->bm);

    om->object_id = o[i].id;
    om->class_id = o[i].class_id;
    om->rect_params.left = o[i].left;
    om->rect_params.top = o[i].top;
    om->rect_params.width = o[i].width;
    om->rect_params.height = o[i].height;
    om->rect_params.border_width = 2;
    om->rect_params.border_color = own_border;
    rds_add_obj_meta_to_frame (d->fm, om, NULL);
    d->objs[i] = om;
  }
  d->n_objs = n;
  rds_analytics_core_process_frame (d->core, 0, FRAME_30FPS (frame), o, n, &res);
  rds_acquire_meta_lock (d->bm);
  rds_analytics_draw_frame (d->fm, d->cfg, d->core, &res, d->objs);
  rds_release_meta_lock (d->bm);
}

static void
drawn_clear (Drawn * d)
{
  rds_destroy_batch_meta (d->bm);
  rds_analytics_core_free (d->core);
  rds_analytics_config_free (d->cfg);
}

static void
assert_color (const RdsOSD_ColorParams * got, const RdsOSD_ColorParams * want)
{
  g_assert_cmpfloat (got->red, ==, want->red);
  g_assert_cmpfloat (got->green, ==, want->green);
  g_assert_cmpfloat (got->blue, ==, want->blue);
  g_assert_cmpfloat (got->alpha, ==, want->alpha);
}

/* The frame's display metas: @n_metas of them, meta i holding lines[i] lines; the texts @texts all in the first;
 * the lines @lines in order across them; nothing else. Every text in Serif at @font_size on black, every line 2 px
 * (DeepStream 8.0 behaviour). */
static void
assert_display (const Drawn * d, guint n_metas, const guint * lines_per_meta, const ExpText * texts, guint n_texts,
    const ExpLine * lines, guint n_lines, guint font_size)
{
  static const RdsOSD_ColorParams black = BLACK;
  guint li = 0, mi = 0;

  g_assert_cmpuint (g_list_length (d->fm->display_meta_list), ==, n_metas);
  for (GList * l = d->fm->display_meta_list; l; l = l->next, mi++) {
    const RdsDisplayMeta *dm = l->data;

    g_assert_cmpuint (dm->num_rects, ==, 0);
    g_assert_cmpuint (dm->num_arrows, ==, 0);
    g_assert_cmpuint (dm->num_circles, ==, 0);
    g_assert_cmpuint (dm->num_labels, ==, mi == 0 ? n_texts : 0);
    g_assert_cmpuint (dm->num_lines, ==, lines_per_meta[mi]);
    for (guint t = 0; t < dm->num_labels; t++) {
      const RdsOSD_TextParams *tp = &dm->text_params[t];

      g_test_message ("text %u: '%s' at (%u, %u)", t, tp->display_text, tp->x_offset, tp->y_offset);
      g_assert_cmpstr (tp->display_text, ==, texts[t].s);
      g_assert_cmpuint (tp->x_offset, ==, texts[t].x);
      g_assert_cmpuint (tp->y_offset, ==, texts[t].y);
      g_assert_cmpstr (tp->font_params.font_name, ==, "Serif");
      g_assert_cmpuint (tp->font_params.font_size, ==, font_size);
      assert_color (&tp->font_params.font_color, &texts[t].c);
      g_assert_cmpint (tp->set_bg_clr, ==, 1);
      assert_color (&tp->text_bg_clr, &black);
    }
    for (guint k = 0; k < dm->num_lines; k++, li++) {
      const RdsOSD_LineParams *lp = &dm->line_params[k];

      g_assert_cmpuint (li, <, n_lines);
      g_test_message ("line %u: (%u, %u) - (%u, %u)", li, lp->x1, lp->y1, lp->x2, lp->y2);
      g_assert_cmpuint (lp->x1, ==, lines[li].x1);
      g_assert_cmpuint (lp->y1, ==, lines[li].y1);
      g_assert_cmpuint (lp->x2, ==, lines[li].x2);
      g_assert_cmpuint (lp->y2, ==, lines[li].y2);
      g_assert_cmpuint (lp->line_width, ==, 2);
      assert_color (&lp->line_color, &lines[li].c);
    }
  }
  g_assert_cmpuint (li, ==, n_lines);
}

/* The objects' text and border: osd-mode 2's objStatus text, white on black in Serif at @font_size, and the cyan
 * border (DeepStream 8.0 behaviour); or untouched. */
static void
assert_objects (const Drawn * d, const ExpObj * want, guint n, guint font_size)
{
  static const RdsOSD_ColorParams white = WHITE, black = BLACK, cyan = CYAN;

  g_assert_cmpuint (d->n_objs, ==, n);
  for (guint i = 0; i < n; i++) {
    const RdsObjectMeta *om = d->objs[i];
    const RdsOSD_TextParams *tp = &om->text_params;

    g_test_message ("object %u: '%s' at (%u, %u)", i, tp->display_text, tp->x_offset, tp->y_offset);
    g_assert_cmpuint (om->rect_params.border_width, ==, 2);
    assert_color (&om->rect_params.border_color, want[i].cyan ? &cyan : &own_border);
    if (!want[i].s) {
      g_assert_null (tp->display_text);
      g_assert_null (tp->font_params.font_name);
      g_assert_cmpint (tp->set_bg_clr, ==, 0);
      continue;
    }
    g_assert_cmpstr (tp->display_text, ==, want[i].s);
    g_assert_cmpuint (tp->x_offset, ==, want[i].x);
    g_assert_cmpuint (tp->y_offset, ==, want[i].y);
    g_assert_cmpstr (tp->font_params.font_name, ==, "Serif");
    g_assert_cmpuint (tp->font_params.font_size, ==, font_size);
    assert_color (&tp->font_params.font_color, &white);
    g_assert_cmpint (tp->set_bg_clr, ==, 1);
    assert_color (&tp->text_bg_clr, &black);
  }
}

static const guint one_meta_15[] = { 15 };

/* ---- osd-mode 0 ---- */

/* osd-mode 0: no display meta, the objects untouched (DeepStream 8.0 behaviour). */
static void
test_mode0 (void)
{
  static const ExpObj objs[] = { UNTOUCHED, UNTOUCHED, UNTOUCHED };
  gchar *groups = four_rules (TRUE), *config = config_text (0, FALSE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 0);
  g_assert_null (d.fm->display_meta_list);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* osd-mode 0 with display-obj-cnt: its text alone, in one display meta (DeepStream 8.0 behaviour). */
static void
test_mode0_obj_cnt (void)
{
  static const ExpText texts[] = { {"Count for ClassId0=3", 5, 5, YELLOW} };
  static const guint no_lines[] = { 0 };
  static const ExpObj objs[] = { UNTOUCHED, UNTOUCHED, UNTOUCHED };
  gchar *groups = four_rules (TRUE), *config = config_text (0, TRUE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 0);
  assert_display (&d, 1, no_lines, texts, G_N_ELEMENTS (texts), NULL, 0, 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* ---- osd-mode 1 ---- */

static const ExpText mode1_labels[] = {
  {"RF", 100, 100, YELLOW}, {"OC", 700, 100, ORANGE}, {"East", 100, 650, RED}, {"Down ", 800, 400, GREEN},
};

/* osd-mode 1: the rules and their bare labels (the line's with a trailing space, at its second point); the objects
 * untouched (DeepStream 8.0 behaviour). */
static void
test_mode1 (void)
{
  static const ExpObj objs[] = { UNTOUCHED, UNTOUCHED, UNTOUCHED };
  gchar *groups = four_rules (TRUE), *config = config_text (1, FALSE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 0);
  assert_display (&d, 1, one_meta_15, mode1_labels, G_N_ELEMENTS (mode1_labels), four_rules_lines,
      G_N_ELEMENTS (four_rules_lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* osd-mode 1 with display-obj-cnt: the count text comes first (DeepStream 8.0 behaviour). */
static void
test_mode1_obj_cnt (void)
{
  static const ExpText texts[] = {
    {"Count for ClassId0=3", 5, 5, YELLOW},
    {"RF", 100, 100, YELLOW}, {"OC", 700, 100, ORANGE}, {"East", 100, 650, RED}, {"Down ", 800, 400, GREEN},
  };
  gchar *groups = four_rules (TRUE), *config = config_text (1, TRUE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 0);
  assert_display (&d, 1, one_meta_15, texts, G_N_ELEMENTS (texts), four_rules_lines,
      G_N_ELEMENTS (four_rules_lines), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* A direction vector of zero length: the shaft and both arrowhead lines are the point (DeepStream 8.0 behaviour). */
static void
test_zero_vector (void)
{
  static const ExpText texts[] = { {"D", 100, 100, RED} };
  static const ExpLine lines[] = { {100, 100, 100, 100, RED}, {100, 100, 100, 100, RED}, {100, 100, 100, 100, RED} };
  static const guint three[] = { 3 };
  gchar *config = config_text (1, FALSE, 0,
      "[direction-detection-stream-0]\nenable=1\ndirection-D=100;100;100;100\nclass-id=-1\n");
  Drawn d;

  draw_scenario (&d, config, 1280, 720, no_objects, 0);
  assert_display (&d, 1, three, texts, G_N_ELEMENTS (texts), lines, G_N_ELEMENTS (lines), 12);
  drawn_clear (&d);
  g_free (config);
}

/* The rules drawn where they are scaled to: two ROIs and two directions (a 1000 px one) from 1280 x 720 to
 * 1920 x 1080, every coordinate truncated (DeepStream 8.0 behaviour). */
static void
test_scaled (void)
{
  static const ExpText texts[] = {
    {"SQ", 600, 300, YELLOW}, {"TRI", 1350, 150, YELLOW}, {"E", 150, 975, RED}, {"Long", 150, 1050, RED},
  };
  static const ExpLine lines[] = {
    {600, 300, 1200, 300, YELLOW}, {1200, 300, 1200, 750, YELLOW}, {1200, 750, 600, 750, YELLOW},
    {600, 750, 600, 300, YELLOW},
    {1350, 150, 1800, 150, YELLOW}, {1800, 150, 1575, 600, YELLOW}, {1575, 600, 1350, 150, YELLOW},
    {150, 975, 450, 975, RED}, {406, 986, 450, 975, RED}, {406, 963, 450, 975, RED},
    {150, 1050, 1650, 1050, RED}, {1432, 1108, 1650, 1050, RED}, {1432, 991, 1650, 1050, RED},
  };
  static const guint thirteen[] = { 13 };
  gchar *config = config_text (1, FALSE, 0,
      "[roi-filtering-stream-0]\nenable=1\nroi-SQ=400;200;800;200;800;500;400;500\n"
      "roi-TRI=900;100;1200;100;1050;400\ninverse-roi=0\nclass-id=-1\n"
      "[direction-detection-stream-0]\nenable=1\ndirection-E=100;650;300;650\n"
      "direction-Long=100;700;1100;700\nclass-id=-1\n");
  Drawn d;

  draw_scenario (&d, config, 1920, 1080, no_objects, 0);
  assert_display (&d, 1, thirteen, texts, G_N_ELEMENTS (texts), lines, G_N_ELEMENTS (lines), 12);
  drawn_clear (&d);
  g_free (config);
}

/* ---- osd-mode 2 ---- */

/* osd-mode 2 on the walkers' first frame: the counts in the labels; the object in the ROI gets its status text and
 * the cyan border, the one only in the overcrowding area an empty text and its own border, the other one nothing
 * (DeepStream 8.0 behaviour). */
static void
test_mode2 (void)
{
  static const ExpText texts[] = {
    {"RF=1", 100, 100, YELLOW}, {"OC OverCrowding=True, Count=1", 700, 100, ORANGE}, {"East", 100, 650, RED},
    {"Down=0", 800, 400, GREEN},
  };
  static const ExpObj objs[] = { {" ROI:RF", 140, 250, TRUE}, UNTOUCHED, {"", 890, 250, FALSE} };
  gchar *groups = four_rules (TRUE), *config = config_text (2, FALSE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 0);
  assert_display (&d, 1, one_meta_15, texts, G_N_ELEMENTS (texts), four_rules_lines,
      G_N_ELEMENTS (four_rules_lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* osd-mode 2 on the frame where object 2 crosses the line: the cumulative count in the line's label, and a cyan
 * border for the crossing object and for the one in the ROI that also has a direction (DeepStream 8.0 behaviour). */
static void
test_mode2_crossing (void)
{
  static const ExpText texts[] = {
    {"RF=1", 100, 100, YELLOW}, {"OC OverCrowding=True, Count=1", 700, 100, ORANGE}, {"East", 100, 650, RED},
    {"Down=1", 800, 400, GREEN},
  };
  static const ExpObj objs[] = {
    {" ROI:RF DIR:East ", 240, 250, TRUE}, {" LC:Down ", 630, 350, TRUE}, {"", 890, 250, FALSE},
  };
  gchar *groups = four_rules (TRUE), *config = config_text (2, FALSE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 20);
  assert_display (&d, 1, one_meta_15, texts, G_N_ELEMENTS (texts), four_rules_lines,
      G_N_ELEMENTS (four_rules_lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* osd-mode 2 once object 3 is STOPPED: its text says so but its border stays its own; the crossed object is back to
 * nothing, and the line's label keeps the cumulative count (DeepStream 8.0 behaviour). */
static void
test_mode2_stopped (void)
{
  static const ExpText texts[] = {
    {"RF=1", 100, 100, YELLOW}, {"OC OverCrowding=True, Count=1", 700, 100, ORANGE}, {"East", 100, 650, RED},
    {"Down=1", 800, 400, GREEN},
  };
  static const ExpObj objs[] = { {" ROI:RF DIR:East ", 390, 250, TRUE}, UNTOUCHED, {" STOPPED ", 890, 250, FALSE} };
  gchar *groups = four_rules (TRUE), *config = config_text (2, FALSE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 50);
  assert_display (&d, 1, one_meta_15, texts, G_N_ELEMENTS (texts), four_rules_lines,
      G_N_ELEMENTS (four_rules_lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* osd-mode 2 with the ROI group disabled: it is not drawn, and a direction alone leaves the border as it was
 * (DeepStream 8.0 behaviour). */
static void
test_mode2_disabled_group (void)
{
  static const ExpText texts[] = {
    {"OC OverCrowding=True, Count=1", 700, 100, ORANGE}, {"East", 100, 650, RED}, {"Down=0", 800, 400, GREEN},
  };
  static const ExpLine lines[] = { OC_LINES, DIR_EAST_LINES, LC_DOWN_LINES };
  static const ExpObj objs[] = { {" DIR:East ", 200, 250, FALSE}, UNTOUCHED, {"", 890, 250, FALSE} };
  static const guint eleven[] = { 11 };
  gchar *groups = four_rules (FALSE), *config = config_text (2, FALSE, 0, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 12);
  assert_display (&d, 1, eleven, texts, G_N_ELEMENTS (texts), lines, G_N_ELEMENTS (lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* display-font-size applies to every text, the labels and the objects' (DeepStream 8.0 behaviour). */
static void
test_mode2_font_size (void)
{
  static const ExpText texts[] = {
    {"RF=1", 100, 100, YELLOW}, {"OC OverCrowding=True, Count=1", 700, 100, ORANGE}, {"East", 100, 650, RED},
    {"Down=0", 800, 400, GREEN},
  };
  static const ExpObj objs[] = { {" ROI:RF", 140, 250, TRUE}, UNTOUCHED, {"", 890, 250, FALSE} };
  gchar *groups = four_rules (TRUE), *config = config_text (2, FALSE, 20, groups);
  Drawn d;

  draw_scenario (&d, config, 1280, 720, walkers, 0);
  assert_display (&d, 1, one_meta_15, texts, G_N_ELEMENTS (texts), four_rules_lines,
      G_N_ELEMENTS (four_rules_lines), 20);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 20);
  drawn_clear (&d);
  g_free (config);
  g_free (groups);
}

/* 18 lines: 15 in the first display meta, which also holds every text, the rest in a second (DeepStream 8.0
 * behaviour; 8.0 lists the newest first, ours in the order they fill). The untracked object only in the overcrowding
 * area gets an empty text too. */
static void
test_line_overflow (void)
{
  static const ExpText texts[] = {
    {"RF=1", 100, 100, YELLOW}, {"OC OverCrowding=True, Count=2", 700, 100, ORANGE}, {"East", 100, 650, RED},
    {"South", 640, 100, RED}, {"Down=0", 800, 400, GREEN},
  };
  static const ExpLine lines[] = {
    ROI_RF_LINES, OC_LINES, DIR_EAST_LINES,
    {640, 100, 640, 300, RED}, {632, 271, 640, 300, RED}, {647, 271, 640, 300, RED},
    LC_DOWN_LINES,
  };
  static const guint split[] = { 15, 3 };
  static const ExpObj objs[] = {
    {" ROI:RF", 200, 190, TRUE}, UNTOUCHED, {"", 800, 190, FALSE}, {"", 900, 290, FALSE},
  };
  gchar *config = config_text (2, FALSE, 12,
      "[roi-filtering-stream-0]\nenable=1\nroi-RF=100;100;600;100;600;600;100;600\ninverse-roi=0\nclass-id=-1\n\n"
      "[overcrowding-stream-0]\nenable=1\nroi-OC=700;100;1200;100;1200;600;700;600\nclass-id=-1\n"
      "object-threshold=1\n\n"
      "[direction-detection-stream-0]\nenable=1\ndirection-East=100;650;300;650\n"
      "direction-South=640;100;640;300\nclass-id=-1\n\n"
      "[line-crossing-stream-0]\nenable=1\nline-crossing-Down=640;300;640;500;500;400;800;400\nclass-id=-1\n"
      "extended=0\nmode=loose\n");
  Drawn d;

  draw_scenario (&d, config, 1280, 720, mixed_boxes, 0);
  assert_display (&d, 2, split, texts, G_N_ELEMENTS (texts), lines, G_N_ELEMENTS (lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
}

/* display-obj-cnt with six classes: sorted by the class id as unsigned, so -1 last (DeepStream 8.0 behaviour), and
 * printed as -1 (ours: 8.0 prints 4294967295). */
static void
test_obj_cnt_classes (void)
{
  static const ExpText texts[] = {
    {"Count for ClassId0=2 ClassId1=2 ClassId2=2 ClassId3=2 ClassId7=2 ClassId-1=2", 5, 5, YELLOW},
    {"A=6", 0, 0, YELLOW},
  };
  static const ExpLine lines[] = {
    {0, 0, 640, 0, YELLOW}, {640, 0, 640, 720, YELLOW}, {640, 720, 0, 720, YELLOW}, {0, 720, 0, 0, YELLOW},
  };
  static const guint four[] = { 4 };
  static const ExpObj objs[] = {
    {" ROI:A", 90, 250, TRUE}, {" ROI:A", 170, 250, TRUE}, {" ROI:A", 250, 250, TRUE}, {" ROI:A", 330, 250, TRUE},
    {" ROI:A", 410, 250, TRUE}, {" ROI:A", 490, 250, TRUE},
    UNTOUCHED, UNTOUCHED, UNTOUCHED, UNTOUCHED, UNTOUCHED, UNTOUCHED,
  };
  gchar *config = config_text (2, TRUE, 0,
      "[roi-filtering-stream-0]\nenable=1\nroi-A=0;0;640;0;640;720;0;720\nclass-id=-1\n");
  Drawn d;

  draw_scenario (&d, config, 1280, 720, class_mix, 0);
  assert_display (&d, 1, four, texts, G_N_ELEMENTS (texts), lines, G_N_ELEMENTS (lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
}

/* ---- ours ---- */

/* display-obj-cnt before any object: the bare "Count for" (ours: 8.0 not observed). */
static void
test_obj_cnt_empty (void)
{
  static const ExpText texts[] = { {"Count for", 5, 5, YELLOW} };
  static const guint no_lines[] = { 0 };
  gchar *config = config_text (0, TRUE, 0, "");
  Drawn d;

  draw_scenario (&d, config, 1280, 720, no_objects, 0);
  assert_display (&d, 1, no_lines, texts, G_N_ELEMENTS (texts), NULL, 0, 12);
  drawn_clear (&d);
  g_free (config);
}

/* An object near the top and an upward vector on the left edge: coordinates below 0 become 0 (ours: 8.0 not
 * observed; the fields are unsigned). An object's earlier text is replaced (and freed: leaks show under ASan). */
static void
test_clamped (void)
{
  static const ExpText texts[] = { {"RF=1", 0, 0, YELLOW}, {"Up", 0, 100, RED} };
  /* a 100 px vector straight up: arrowhead lines 15 px long, x = -/+ 3.88 */
  static const ExpLine lines[] = {
    {0, 0, 400, 0, YELLOW}, {400, 0, 400, 400, YELLOW}, {400, 400, 0, 400, YELLOW}, {0, 400, 0, 0, YELLOW},
    {0, 100, 0, 0, RED}, {3, 14, 0, 0, RED}, {0, 14, 0, 0, RED},
  };
  static const guint seven[] = { 7 };
  static const ExpObj objs[] = { {" ROI:RF", 100, 0, TRUE} };
  gchar *config = config_text (2, FALSE, 0,
      "[roi-filtering-stream-0]\nenable=1\nroi-RF=0;0;400;0;400;400;0;400\n\n"
      "[direction-detection-stream-0]\nenable=1\ndirection-Up=0;100;0;0\n");
  RdsAnalyticsObject o = {.id = 1,.class_id = 0,.left = 100,.top = 4.5,.width = 20,.height = 40 };
  RdsAnalyticsFrameResult res;
  Drawn d;

  /* draw_scenario with an object that already has a text */
  memset (&d, 0, sizeof d);
  d.cfg = load_config (config);
  d.core = rds_analytics_core_new (d.cfg, 1280, 720);
  d.bm = rds_create_batch_meta (1);
  d.fm = rds_acquire_frame_meta_from_pool (d.bm);
  rds_add_frame_meta_to_batch (d.bm, d.fm);
  d.objs[0] = rds_acquire_obj_meta_from_pool (d.bm);
  d.objs[0]->object_id = 1;
  d.objs[0]->rect_params = (RdsOSD_RectParams) {.left = 100,.top = 4.5,.width = 20,.height = 40,.border_width = 2,
    .border_color = own_border };
  d.objs[0]->text_params.display_text = g_strdup ("person 1");
  rds_add_obj_meta_to_frame (d.fm, d.objs[0], NULL);
  d.n_objs = 1;
  rds_analytics_core_process_frame (d.core, 0, 0, &o, 1, &res);
  rds_acquire_meta_lock (d.bm);
  rds_analytics_draw_frame (d.fm, d.cfg, d.core, &res, d.objs);
  rds_release_meta_lock (d.bm);

  assert_display (&d, 1, seven, texts, G_N_ELEMENTS (texts), lines, G_N_ELEMENTS (lines), 12);
  assert_objects (&d, objs, G_N_ELEMENTS (objs), 12);
  drawn_clear (&d);
  g_free (config);
}

/* More texts than one display meta holds: the rest go in the next (ours: 8.0 not observed). 40 ROIs give 40 labels
 * and 160 lines: texts 16 + 16 + 8, lines 15 a meta, so 11 display metas. */
static void
test_text_overflow (void)
{
  GString *groups = g_string_new ("[roi-filtering-stream-0]\nenable=1\n");
  gchar *config;
  Drawn d;
  guint metas = 0, labels = 0, lines = 0;

  for (guint i = 0; i < 40; i++)
    g_string_append_printf (groups, "roi-R%u=%u;0;%u;0;%u;10;%u;10\n", i, 20 * i, 20 * i + 10, 20 * i + 10, 20 * i);
  config = config_text (1, FALSE, 0, groups->str);
  draw_scenario (&d, config, 1280, 720, no_objects, 0);
  for (GList * l = d.fm->display_meta_list; l; l = l->next, metas++) {
    const RdsDisplayMeta *dm = l->data;

    g_assert_cmpuint (dm->num_labels, ==, metas < 2 ? 16 : metas == 2 ? 8 : 0);
    g_assert_cmpuint (dm->num_lines, ==, metas < 10 ? 15 : 10);
    for (guint t = 0; t < dm->num_labels; t++, labels++) {
      gchar *want = g_strdup_printf ("R%u", labels);

      g_assert_cmpstr (dm->text_params[t].display_text, ==, want);
      g_free (want);
    }
    lines += dm->num_lines;
  }
  g_assert_cmpuint (metas, ==, 11);
  g_assert_cmpuint (labels, ==, 40);
  g_assert_cmpuint (lines, ==, 160);
  drawn_clear (&d);
  g_free (config);
  g_string_free (groups, TRUE);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/analytics-draw/mode0", test_mode0);
  g_test_add_func ("/cpu/analytics-draw/mode0-obj-cnt", test_mode0_obj_cnt);
  g_test_add_func ("/cpu/analytics-draw/mode1", test_mode1);
  g_test_add_func ("/cpu/analytics-draw/mode1-obj-cnt", test_mode1_obj_cnt);
  g_test_add_func ("/cpu/analytics-draw/zero-vector", test_zero_vector);
  g_test_add_func ("/cpu/analytics-draw/scaled", test_scaled);
  g_test_add_func ("/cpu/analytics-draw/mode2", test_mode2);
  g_test_add_func ("/cpu/analytics-draw/mode2-crossing", test_mode2_crossing);
  g_test_add_func ("/cpu/analytics-draw/mode2-stopped", test_mode2_stopped);
  g_test_add_func ("/cpu/analytics-draw/mode2-disabled-group", test_mode2_disabled_group);
  g_test_add_func ("/cpu/analytics-draw/mode2-font-size", test_mode2_font_size);
  g_test_add_func ("/cpu/analytics-draw/line-overflow", test_line_overflow);
  g_test_add_func ("/cpu/analytics-draw/obj-cnt-classes", test_obj_cnt_classes);
  g_test_add_func ("/cpu/analytics-draw/obj-cnt-empty", test_obj_cnt_empty);
  g_test_add_func ("/cpu/analytics-draw/clamped", test_clamped);
  g_test_add_func ("/cpu/analytics-draw/text-overflow", test_text_overflow);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
