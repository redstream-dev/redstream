/* rdsanalytics core: rule scaling, the reference point and the inside test, against DeepStream 8.0's observed
 * results (the observed boxes below: left, top, width, height in frame pixels, and the ROIs 8.0 reported them in);
 * ROI filtering, overcrowding and the object counts on scripted frames; direction and STOPPED on the scenarios
 * 8.0 was observed on, against its states frame by frame.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-core.h"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#define PT(x, y) ((RdsAnalyticsPoint) { (x), (y) })

static gboolean
inside (gint x, gint y, const RdsAnalyticsPoint * poly, guint n)
{
  return rds_analytics_point_in_polygon (PT (x, y), poly, n);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Scaling */

static void
test_scale (void)
{
  /* 1920x1080 config space into a 1280x720 mux: 2/3 on both axes */
  RdsAnalyticsPoint pts[] = { {1050, 400}, {1920, 1080}, {0, 0}, {1, 1}, {301, 599} };
  /* 1280x720 config space into 1000x1000: 0.78125 on x, 1.3889 on y */
  RdsAnalyticsPoint sq[] = { {400, 200}, {800, 200}, {800, 500}, {400, 500} };

  rds_analytics_scale_points (pts, G_N_ELEMENTS (pts), 1920, 1080, 1280, 720, pts);
  g_assert_cmpint (pts[0].x, ==, 700);
  g_assert_cmpint (pts[0].y, ==, 266);       /* 266.67, truncated */
  g_assert_cmpint (pts[1].x, ==, 1280);
  g_assert_cmpint (pts[1].y, ==, 720);
  g_assert_cmpint (pts[2].x, ==, 0);
  g_assert_cmpint (pts[2].y, ==, 0);
  g_assert_cmpint (pts[3].x, ==, 0);
  g_assert_cmpint (pts[3].y, ==, 0);
  g_assert_cmpint (pts[4].x, ==, 200);       /* 200.67 */
  g_assert_cmpint (pts[4].y, ==, 399);       /* 399.33 */

  rds_analytics_scale_points (sq, G_N_ELEMENTS (sq), 1280, 720, 1000, 1000, sq);
  g_assert_cmpint (sq[0].x, ==, 312);        /* 312.5 */
  g_assert_cmpint (sq[0].y, ==, 277);        /* 277.78 */
  g_assert_cmpint (sq[1].x, ==, 625);
  g_assert_cmpint (sq[2].y, ==, 694);        /* 694.44 */

  /* 1280x720 into 1920x1200: y 200 -> 333.33 -> 333 */
  {
    RdsAnalyticsPoint p = { 400, 200 }, q;

    rds_analytics_scale_points (&p, 1, 1280, 720, 1920, 1200, &q);
    g_assert_cmpint (q.x, ==, 600);
    g_assert_cmpint (q.y, ==, 333);
    g_assert_cmpint (p.x, ==, 400);           /* the input is left alone */
  }

  /* the same size is the identity, and large coordinates don't overflow */
  {
    RdsAnalyticsPoint p = { G_MAXINT, 7 }, q;

    rds_analytics_scale_points (&p, 1, 1280, 720, 1280, 720, &q);
    g_assert_cmpint (q.x, ==, G_MAXINT);
    g_assert_cmpint (q.y, ==, 7);
    rds_analytics_scale_points (&p, 1, 1000, 1000, 500, 500, &q);
    g_assert_cmpint (q.x, ==, G_MAXINT / 2);
  }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* The reference point */

static void
test_ref_point (void)
{
  RdsAnalyticsPoint p;

  p = rds_analytics_ref_point (100, 200, 50, 80);
  g_assert_cmpint (p.x, ==, 125);
  g_assert_cmpint (p.y, ==, 280);
  /* floored, not rounded */
  p = rds_analytics_ref_point (394.9f, 330, 10, 20);
  g_assert_cmpint (p.x, ==, 399);
  p = rds_analytics_ref_point (395.1f, 330, 10, 20);
  g_assert_cmpint (p.x, ==, 400);
  p = rds_analytics_ref_point (100, 100, 11, 20.9f);
  g_assert_cmpint (p.x, ==, 105);           /* 105.5 */
  g_assert_cmpint (p.y, ==, 120);           /* 120.9 */
  /* floor, not truncation toward zero, left of the frame */
  p = rds_analytics_ref_point (-30.5f, 0, 10, 0.5f);
  g_assert_cmpint (p.x, ==, -26);           /* -25.5 */
  g_assert_cmpint (p.y, ==, 0);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* The inside test */

static const RdsAnalyticsPoint SQ_CW[] = { {400, 200}, {800, 200}, {800, 500}, {400, 500} };
static const RdsAnalyticsPoint SQ_CCW[] = { {400, 200}, {400, 500}, {800, 500}, {800, 200} };
static const RdsAnalyticsPoint TRI_PTS[] = { {900, 100}, {1200, 100}, {1050, 400} };
static const RdsAnalyticsPoint BOW_PTS[] = { {100, 550}, {300, 700}, {300, 550}, {100, 700} };
static const RdsAnalyticsPoint SL_PTS[] = { {100, 550}, {600, 560}, {600, 700}, {100, 700} };

/* left and top edges in, right and bottom edges out; of the vertices only the top-left one is in */
static void
test_square_edges (void)
{
  const RdsAnalyticsPoint *sqs[] = { SQ_CW, SQ_CCW };

  for (guint i = 0; i < G_N_ELEMENTS (sqs); i++) {
    const RdsAnalyticsPoint *sq = sqs[i];

    g_assert_true (inside (600, 350, sq, 4));
    g_assert_true (inside (400, 350, sq, 4));
    g_assert_false (inside (399, 350, sq, 4));
    g_assert_true (inside (799, 350, sq, 4));
    g_assert_false (inside (800, 350, sq, 4));
    g_assert_true (inside (600, 200, sq, 4));
    g_assert_false (inside (600, 199, sq, 4));
    g_assert_true (inside (600, 499, sq, 4));
    g_assert_false (inside (600, 500, sq, 4));
    g_assert_true (inside (400, 200, sq, 4));
    g_assert_false (inside (800, 200, sq, 4));
    g_assert_false (inside (800, 500, sq, 4));
    g_assert_false (inside (400, 500, sq, 4));
  }
}

/* the apex row has no inside pixel: at (1050, 399) the right edge crosses at x 1050.5, truncated to 1050 */
static void
test_triangle_apex (void)
{
  g_assert_true (inside (1050, 150, TRI_PTS, 3));
  g_assert_true (inside (1049, 399, TRI_PTS, 3));
  g_assert_false (inside (1050, 399, TRI_PTS, 3));
  g_assert_false (inside (1050, 400, TRI_PTS, 3));
  g_assert_false (inside (1050, 401, TRI_PTS, 3));
  g_assert_true (inside (1050, 398, TRI_PTS, 3));
  g_assert_false (inside (1051, 398, TRI_PTS, 3));
}

/* self-intersecting: even-odd */
static void
test_bow_tie (void)
{
  g_assert_true (inside (150, 620, BOW_PTS, 4));    /* left lobe */
  g_assert_true (inside (250, 625, BOW_PTS, 4));    /* right lobe */
  g_assert_false (inside (150, 560, BOW_PTS, 4));   /* outside the left lobe */
  g_assert_false (inside (200, 600, BOW_PTS, 4));   /* the gap above the crossing */
  g_assert_false (inside (200, 650, BOW_PTS, 4));   /* the gap below */
  g_assert_false (inside (250, 690, BOW_PTS, 4));
  g_assert_true (inside (200, 625, BOW_PTS, 4));    /* the crossing point */
}

/* the vertex order doesn't matter: every rotation and its reverse agree on every pixel around the polygons, edge
 * lines included */
static void
test_orientation (void)
{
  const struct
  {
    const RdsAnalyticsPoint *pts;
    guint n;
  } polys[] = { {SQ_CW, 4}, {TRI_PTS, 3}, {BOW_PTS, 4}, {SL_PTS, 4} };

  for (guint k = 0; k < G_N_ELEMENTS (polys); k++) {
    guint n = polys[k].n;
    RdsAnalyticsPoint var[4];

    for (guint rot = 0; rot < n; rot++) {
      for (guint rev = 0; rev < 2; rev++) {
        for (guint i = 0; i < n; i++)
          var[i] = polys[k].pts[rev ? (rot + n - i) % n : (rot + i) % n];
        for (gint y = 80; y <= 720; y++)
          for (gint x = 80; x <= 1220; x++)
            g_assert_cmpint (inside (x, y, var, n), ==, inside (x, y, polys[k].pts, n));
      }
    }
  }
}

static void
test_degenerate (void)
{
  const RdsAnalyticsPoint seg[] = { {0, 0}, {100, 100} };

  g_assert_false (inside (10, 50, seg, 2));
  g_assert_false (inside (0, 0, seg, 1));
  g_assert_false (inside (0, 0, NULL, 0));
}

/* ------------------------------------------------------------------------------------------------------------ */
/* DeepStream 8.0's observed results: every box through the reference point and the inside test */

enum
{
  SQ = 1 << 0,
  SQCCW = 1 << 1,
  TRI = 1 << 2,
  BOW = 1 << 3,
  SL = 1 << 4,
};

typedef struct
{
  gfloat left, top, width, height;
  guint rois;                   /* the ROIs DeepStream 8.0 put the box in */
} ObservedBox;

typedef struct
{
  guint flag;
  const RdsAnalyticsPoint *pts;
  guint n;
} ObservedRoi;

/* 10x20 boxes on the square's edges and corners, inside and outside the triangle and the bow-tie's lobes, gap
 * and crossing; 1280x720 */
static const ObservedBox shape_boxes[] = {
  {595, 330, 10, 20, SQ | SQCCW}, {395, 330, 10, 20, SQ | SQCCW}, {394.9, 330, 10, 20, 0},
  {395.1, 330, 10, 20, SQ | SQCCW}, {394.5, 330, 10, 20, 0}, {395.5, 330, 10, 20, SQ | SQCCW}, {795, 330, 10, 20, 0},
  {795.1, 330, 10, 20, 0}, {794.9, 330, 10, 20, SQ | SQCCW}, {595, 180, 10, 20, SQ | SQCCW}, {595, 179.9, 10, 20, 0},
  {595, 180.1, 10, 20, SQ | SQCCW}, {595, 480, 10, 20, 0}, {595, 480.1, 10, 20, 0}, {595, 479.9, 10, 20, SQ | SQCCW},
  {395, 180, 10, 20, SQ | SQCCW}, {795, 480, 10, 20, 0}, {395, 480, 10, 20, 0}, {795, 180, 10, 20, 0},
  {1045, 130, 10, 20, TRI}, {1045, 379, 10, 20, 0}, {1045, 380, 10, 20, 0}, {1045, 381, 10, 20, 0},
  {995, 230, 10, 20, TRI}, {145, 540, 10, 20, 0}, {145, 600, 10, 20, BOW}, {195, 580, 10, 20, 0},
  {195, 630, 10, 20, 0}, {245, 670, 10, 20, 0}, {195, 605, 10, 20, BOW}, {995, 480, 10, 20, 0},
  {995, 630, 10, 20, 0}, {905, 440, 10, 20, 0}, {1185, 670, 10, 20, 0}
};

/* 4x8 boxes a fraction of a pixel apart: the triangle's apex rows, its slanted and top edges, the square's corners and
 * the nearly horizontal top edge of SL; 1280x720 */
static const ObservedBox apex_edge_boxes[] = {
  {1046.6, 389, 4, 8, TRI}, {1047, 389, 4, 8, TRI}, {1047.3, 389, 4, 8, TRI}, {1047.6, 389, 4, 8, TRI},
  {1048, 389, 4, 8, TRI}, {1048.3, 389, 4, 8, TRI}, {1048.6, 389, 4, 8, TRI}, {1049, 389, 4, 8, 0},
  {1046.6, 389.5, 4, 8, TRI}, {1047, 389.5, 4, 8, TRI}, {1047.3, 389.5, 4, 8, TRI}, {1047.6, 389.5, 4, 8, TRI},
  {1048, 389.5, 4, 8, TRI}, {1048.3, 389.5, 4, 8, TRI}, {1048.6, 389.5, 4, 8, TRI}, {1049, 389.5, 4, 8, 0},
  {1046.6, 390, 4, 8, 0}, {1047, 390, 4, 8, TRI}, {1047.3, 390, 4, 8, TRI}, {1047.6, 390, 4, 8, TRI},
  {1048, 390, 4, 8, TRI}, {1048.3, 390, 4, 8, TRI}, {1048.6, 390, 4, 8, TRI}, {1049, 390, 4, 8, 0},
  {1046.6, 390.2, 4, 8, 0}, {1047, 390.2, 4, 8, TRI}, {1047.3, 390.2, 4, 8, TRI}, {1047.6, 390.2, 4, 8, TRI},
  {1048, 390.2, 4, 8, TRI}, {1048.3, 390.2, 4, 8, TRI}, {1048.6, 390.2, 4, 8, TRI}, {1049, 390.2, 4, 8, 0},
  {1046.6, 390.7, 4, 8, 0}, {1047, 390.7, 4, 8, TRI}, {1047.3, 390.7, 4, 8, TRI}, {1047.6, 390.7, 4, 8, TRI},
  {1048, 390.7, 4, 8, TRI}, {1048.3, 390.7, 4, 8, TRI}, {1048.6, 390.7, 4, 8, TRI}, {1049, 390.7, 4, 8, 0},
  {1046.6, 391, 4, 8, 0}, {1047, 391, 4, 8, TRI}, {1047.3, 391, 4, 8, TRI}, {1047.6, 391, 4, 8, TRI},
  {1048, 391, 4, 8, 0}, {1048.3, 391, 4, 8, 0}, {1048.6, 391, 4, 8, 0}, {1049, 391, 4, 8, 0},
  {1046.6, 391.4, 4, 8, 0}, {1047, 391.4, 4, 8, TRI}, {1047.3, 391.4, 4, 8, TRI}, {1047.6, 391.4, 4, 8, TRI},
  {1048, 391.4, 4, 8, 0}, {1048.3, 391.4, 4, 8, 0}, {1048.6, 391.4, 4, 8, 0}, {1049, 391.4, 4, 8, 0},
  {1046.6, 391.6, 4, 8, 0}, {1047, 391.6, 4, 8, TRI}, {1047.3, 391.6, 4, 8, TRI}, {1047.6, 391.6, 4, 8, TRI},
  {1048, 391.6, 4, 8, 0}, {1048.3, 391.6, 4, 8, 0}, {1048.6, 391.6, 4, 8, 0}, {1049, 391.6, 4, 8, 0},
  {1046.6, 391.9, 4, 8, 0}, {1047, 391.9, 4, 8, TRI}, {1047.3, 391.9, 4, 8, TRI}, {1047.6, 391.9, 4, 8, TRI},
  {1048, 391.9, 4, 8, 0}, {1048.3, 391.9, 4, 8, 0}, {1048.6, 391.9, 4, 8, 0}, {1049, 391.9, 4, 8, 0},
  {1046.6, 392, 4, 8, 0}, {1047, 392, 4, 8, 0}, {1047.3, 392, 4, 8, 0}, {1047.6, 392, 4, 8, 0}, {1048, 392, 4, 8, 0},
  {1048.3, 392, 4, 8, 0}, {1048.6, 392, 4, 8, 0}, {1049, 392, 4, 8, 0}, {997.7, 292, 4, 8, 0}, {998, 292, 4, 8, TRI},
  {998.1, 292, 4, 8, TRI}, {998.3, 292, 4, 8, TRI}, {998.2, 292.9, 4, 8, TRI}, {998, 292.6, 4, 8, TRI},
  {998.4, 292.6, 4, 8, TRI}, {997.6, 291.4, 4, 8, TRI}, {1097.7, 292, 4, 8, TRI}, {1098, 292, 4, 8, 0},
  {1098.3, 292, 4, 8, 0}, {1097.6, 292.6, 4, 8, TRI}, {1097.9, 292.4, 4, 8, TRI}, {1097.4, 292.9, 4, 8, TRI},
  {898, 92, 4, 8, TRI}, {898.5, 92.5, 4, 8, TRI}, {899, 93, 4, 8, TRI}, {1198, 92, 4, 8, 0}, {1197, 92, 4, 8, TRI},
  {1197.5, 92.2, 4, 8, TRI}, {1048, 92, 4, 8, TRI}, {1048, 91.9, 4, 8, 0}, {397.5, 191.5, 4, 8, 0},
  {398, 191.99, 4, 8, 0}, {797.99, 491.99, 4, 8, SQ}, {797.5, 192, 4, 8, SQ}, {398, 491.5, 4, 8, SQ},
  {398.49, 342, 4, 8, SQ}, {397.51, 342, 4, 8, 0}, {348, 546.9, 4, 8, 0}, {348, 547, 4, 8, 0}, {348, 547.1, 4, 8, 0},
  {348, 547.4, 4, 8, 0}, {348, 547.6, 4, 8, 0}, {373, 547.4, 4, 8, 0}, {373, 547.6, 4, 8, 0}, {323, 546.6, 4, 8, 0},
  {323, 546.4, 4, 8, 0}, {347.5, 547, 4, 8, SL}
};

/* the square's scaled top, bottom, left and right edges at 1000x1000 */
static const ObservedBox edge_boxes_1000[] = {
  {498, 269, 4, 8, SQ}, {498, 269.4, 4, 8, SQ}, {498, 269.5, 4, 8, SQ}, {498, 269.6, 4, 8, SQ},
  {498, 269.8, 4, 8, SQ}, {498, 269.9, 4, 8, SQ}, {498, 270, 4, 8, SQ}, {498, 686, 4, 8, 0}, {498, 686.3, 4, 8, 0},
  {498, 686.4, 4, 8, 0}, {498, 686.5, 4, 8, 0}, {498, 686.6, 4, 8, 0}, {310, 392, 4, 8, SQ}, {310.4, 392, 4, 8, SQ},
  {310.5, 392, 4, 8, SQ}, {310.6, 392, 4, 8, SQ}, {622.4, 392, 4, 8, SQ}, {622.6, 392, 4, 8, SQ},
  {622.9, 392, 4, 8, SQ}, {623, 392, 4, 8, 0}
};

/* The ROIs, in a 1280x720 config space, are scaled to the @out_w x @out_h mux. */
static void
check_observed (const ObservedBox * boxes, guint n_boxes, const ObservedRoi * rois, guint n_rois, guint out_w,
    guint out_h)
{
  for (guint r = 0; r < n_rois; r++) {
    RdsAnalyticsPoint scaled[4];

    g_assert_cmpuint (rois[r].n, <=, G_N_ELEMENTS (scaled));
    rds_analytics_scale_points (rois[r].pts, rois[r].n, 1280, 720, out_w, out_h, scaled);
    for (guint i = 0; i < n_boxes; i++) {
      const ObservedBox *b = &boxes[i];
      RdsAnalyticsPoint p = rds_analytics_ref_point (b->left, b->top, b->width, b->height);
      gboolean want = (b->rois & rois[r].flag) != 0;

      if (rds_analytics_point_in_polygon (p, scaled, rois[r].n) != want)
        g_error ("box %u (%g, %g, %g, %g), point (%d, %d): ROI %#x should be %s", i, b->left, b->top, b->width,
            b->height, p.x, p.y, rois[r].flag, want ? "in" : "out");
    }
  }
}

static void
test_observed_shapes (void)
{
  const ObservedRoi rois[] = { {SQ, SQ_CW, 4}, {SQCCW, SQ_CCW, 4}, {TRI, TRI_PTS, 3}, {BOW, BOW_PTS, 4} };

  check_observed (shape_boxes, G_N_ELEMENTS (shape_boxes), rois, G_N_ELEMENTS (rois), 1280, 720);
}

static void
test_observed_edges (void)
{
  const ObservedRoi rois[] = { {TRI, TRI_PTS, 3}, {SQ, SQ_CW, 4}, {SL, SL_PTS, 4} };

  check_observed (apex_edge_boxes, G_N_ELEMENTS (apex_edge_boxes), rois, G_N_ELEMENTS (rois), 1280, 720);
  /* non-integer scaling */
  check_observed (edge_boxes_1000, G_N_ELEMENTS (edge_boxes_1000), rois, G_N_ELEMENTS (rois), 1000, 1000);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* ROI filtering, overcrowding and objCnt */

#define PROPERTY "[property]\nenable=1\nconfig-width=1280\nconfig-height=720\n"
#define FRAME_30FPS(i) ((guint64) (i) * G_GUINT64_CONSTANT (1000000000) / 30)
#define FRAME_10FPS(i) ((guint64) (i) * G_GUINT64_CONSTANT (100000000))

/* @text written to a temp file and loaded; the file is gone on return. */
static RdsAnalyticsConfig *
load_config (const gchar * text)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("rds-analytics-core-XXXXXX", &err);
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

/* A core for @text on a 1280x720 mux; the config is freed at once (the core keeps copies). */
static RdsAnalyticsCore *
core_for (const gchar * text)
{
  RdsAnalyticsConfig *cfg = load_config (text);
  RdsAnalyticsCore *core = rds_analytics_core_new (cfg, 1280, 720);

  rds_analytics_config_free (cfg);
  g_assert_nonnull (core);
  return core;
}

/* A 20x40 box whose reference point is (x, y). */
static RdsAnalyticsObject
obj_at (guint64 id, gint class_id, gint x, gint y)
{
  RdsAnalyticsObject o = { id, class_id, x - 10, y - 40, 20, 40 };

  return o;
}

static const RdsAnalyticsRoiCount *
roi_count (const RdsAnalyticsFrameResult * r, const gchar * label)
{
  for (guint i = 0; i < r->n_roi_counts; i++)
    if (g_str_equal (r->roi_counts[i].label, label))
      return &r->roi_counts[i];
  return NULL;
}

/* objCnt of @class_id; -1 when the class has no entry */
static gint
obj_cnt (const RdsAnalyticsFrameResult * r, gint class_id)
{
  for (guint i = 0; i < r->n_obj_cnt; i++)
    if (r->obj_cnt[i].class_id == class_id)
      return (gint) r->obj_cnt[i].count;
  return -1;
}

/* The labels as one string, "A,B" */
static gchar *
join_labels (const gchar * const *labels, guint n)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < n; i++)
    g_string_append_printf (s, "%s%s", i ? "," : "", labels[i]);
  return g_string_free (s, FALSE);
}

static void
assert_labels (const gchar * const *labels, guint n, const gchar * want)
{
  gchar *got = join_labels (labels, n);

  g_assert_cmpstr (got, ==, want);
  g_free (got);
}

/* The overcrowding sequence DeepStream 8.0 was observed on: per frame, that many class-0 objects in the left half
 * (an overcrowding ROI 0;0;640;0;640;720;0;720), plus one class-1 object in the right half throughout. */
static const guint oc_sequence[] = { 0, 1, 2, 3, 4, 3, 2, 3, 4, 5, 0, 3, 3, 3, 4, 4, 2, 2, 2, 3 };

#define OC_CONFIG(thr) PROPERTY "[overcrowding-stream-0]\nenable=1\nroi-OC=0;0;640;0;640;720;0;720\n" \
  "class-id=-1\nobject-threshold=" thr "\n"

/* The objects of frame @i of oc_sequence into @objs (room for 6); returns their number. */
static guint
oc_sequence_frame (guint i, RdsAnalyticsObject * objs)
{
  guint n = 0;

  for (guint k = 0; k < oc_sequence[i]; k++, n++)
    objs[n] = obj_at (1 + k, 0, 60 + 50 * k, 300);
  objs[n++] = obj_at (100, 1, 1000, 300);
  return n;
}

/* count >= threshold && count > 0, on the frame itself: DeepStream 8.0's observed flags at thresholds 3 and 0 */
static void
test_oc_threshold (void)
{
  static const gboolean want_t3[] = { 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1 };
  static const gboolean want_t0[] = { 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
  const struct
  {
    const gchar *config;
    const gboolean *want;
  } cases[] = { {OC_CONFIG ("3"), want_t3}, {OC_CONFIG ("0"), want_t0} };

  for (guint c = 0; c < G_N_ELEMENTS (cases); c++) {
    RdsAnalyticsCore *core = core_for (cases[c].config);

    for (guint i = 0; i < G_N_ELEMENTS (oc_sequence); i++) {
      RdsAnalyticsObject objs[6];
      guint n = oc_sequence_frame (i, objs);
      RdsAnalyticsFrameResult r;

      rds_analytics_core_process_frame (core, 0, FRAME_30FPS (i), objs, n, &r);
      g_assert_cmpuint (r.n_oc, ==, 1);
      g_assert_cmpstr (r.oc[0].label, ==, "OC");
      g_assert_cmpuint (r.oc[0].count, ==, oc_sequence[i]);
      g_assert_cmpint (r.oc[0].status, ==, cases[c].want[i]);
      /* the overcrowding ROI is in objInROIcnt too */
      g_assert_cmpuint (r.n_roi_counts, ==, 1);
      g_assert_cmpstr (r.roi_counts[0].label, ==, "OC");
      g_assert_cmpint (r.roi_counts[0].feature, ==, RDS_ANALYTICS_OVERCROWDING);
      g_assert_cmpuint (r.roi_counts[0].count, ==, oc_sequence[i]);
      /* the objects in the ROI carry its label (whatever the status) with no objStatus; the other one nothing */
      g_assert_cmpuint (r.n_objects, ==, n);
      for (guint k = 0; k < n; k++) {
        const RdsAnalyticsObjectResult *o = &r.objects[k];
        gboolean in = k < oc_sequence[i];

        assert_labels (o->oc_labels, o->n_oc_labels, in ? "OC" : "");
        g_assert_cmpuint (o->n_roi_labels, ==, 0);
        g_assert_cmpstr (o->obj_status, ==, "");
        g_assert_cmpint (o->has_info, ==, in);
      }
    }
    rds_analytics_core_free (core);
  }
}

/* threshold - 1, threshold and threshold + 1, with two rules sharing the group's threshold */
static void
test_oc_boundaries (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[overcrowding-stream-0]\nenable=1\n"
      "roi-L=0;0;640;0;640;720;0;720\nroi-R=640;0;1280;0;1280;720;640;720\nobject-threshold=2\n");
  RdsAnalyticsObject objs[] = {
    obj_at (1, 0, 100, 300), obj_at (2, 0, 700, 300), obj_at (3, 0, 800, 300), obj_at (4, 0, 900, 300),
  };
  RdsAnalyticsFrameResult r;

  /* L: 1, R: 3 */
  rds_analytics_core_process_frame (core, 0, 0, objs, 4, &r);
  g_assert_cmpuint (r.n_oc, ==, 2);
  g_assert_cmpstr (r.oc[0].label, ==, "L");
  g_assert_cmpuint (r.oc[0].count, ==, 1);
  g_assert_false (r.oc[0].status);
  g_assert_cmpstr (r.oc[1].label, ==, "R");
  g_assert_cmpuint (r.oc[1].count, ==, 3);
  g_assert_true (r.oc[1].status);
  /* L: 2, R: 2 */
  objs[1].left = 200;
  rds_analytics_core_process_frame (core, 0, 1, objs, 4, &r);
  g_assert_cmpuint (r.oc[0].count, ==, 2);
  g_assert_true (r.oc[0].status);
  g_assert_cmpuint (r.oc[1].count, ==, 2);
  g_assert_true (r.oc[1].status);
  /* the flag falls on the frame the count does */
  rds_analytics_core_process_frame (core, 0, 2, objs, 1, &r);
  g_assert_cmpuint (r.oc[0].count, ==, 1);
  g_assert_false (r.oc[0].status);
  g_assert_cmpuint (r.oc[1].count, ==, 0);
  g_assert_false (r.oc[1].status);
  rds_analytics_core_free (core);
}

/* Labels and counts in config order, zero entries from the first frame, objStatus " ROI:<l>" per label */
static void
test_roi_labels (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=1\n"
      "roi-Z=0;0;1280;0;1280;720;0;720\nroi-A=0;0;640;0;640;720;0;720\nroi-M=1000;0;1280;0;1280;100;1000;100\n"
      "[overcrowding-stream-0]\nenable=1\nroi-B=0;0;640;0;640;720;0;720\nobject-threshold=1\n");
  RdsAnalyticsObject objs[] = { obj_at (1, 0, 100, 300), obj_at (2, 3, 700, 300) };
  RdsAnalyticsFrameResult r;
  static const gchar *order[] = { "Z", "A", "M", "B" };

  /* no objects: every label at 0 */
  rds_analytics_core_process_frame (core, 0, 0, NULL, 0, &r);
  g_assert_cmpuint (r.n_roi_counts, ==, 4);
  for (guint i = 0; i < 4; i++) {
    g_assert_cmpstr (r.roi_counts[i].label, ==, order[i]);
    g_assert_cmpuint (r.roi_counts[i].count, ==, 0);
    g_assert_cmpint (r.roi_counts[i].feature, ==, i < 3 ? RDS_ANALYTICS_ROI_FILTERING : RDS_ANALYTICS_OVERCROWDING);
  }
  g_assert_cmpuint (r.n_oc, ==, 1);
  g_assert_cmpuint (r.oc[0].count, ==, 0);
  g_assert_false (r.oc[0].status);
  g_assert_cmpuint (r.n_obj_cnt, ==, 0);
  g_assert_cmpuint (r.n_objects, ==, 0);

  rds_analytics_core_process_frame (core, 0, 1, objs, 2, &r);
  g_assert_cmpuint (roi_count (&r, "Z")->count, ==, 2);
  g_assert_cmpuint (roi_count (&r, "A")->count, ==, 1);
  g_assert_cmpuint (roi_count (&r, "M")->count, ==, 0);
  g_assert_cmpuint (roi_count (&r, "B")->count, ==, 1);
  g_assert_cmpuint (r.n_objects, ==, 2);
  assert_labels (r.objects[0].roi_labels, r.objects[0].n_roi_labels, "Z,A");
  assert_labels (r.objects[0].oc_labels, r.objects[0].n_oc_labels, "B");
  g_assert_cmpstr (r.objects[0].obj_status, ==, " ROI:Z ROI:A");
  g_assert_true (r.objects[0].has_info);
  assert_labels (r.objects[1].roi_labels, r.objects[1].n_roi_labels, "Z");
  assert_labels (r.objects[1].oc_labels, r.objects[1].n_oc_labels, "");
  g_assert_cmpstr (r.objects[1].obj_status, ==, " ROI:Z");
  g_assert_true (r.objects[1].has_info);
  rds_analytics_core_free (core);
}

/* inverse-roi: the label goes on the objects outside, and those are counted */
static void
test_roi_inverse (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=1\n"
      "roi-SQ=400;200;800;200;800;500;400;500\nroi-ALL=0;0;1280;0;1280;720;0;720\ninverse-roi=1\n"
      "[overcrowding-stream-0]\nenable=1\nroi-OC=400;200;800;200;800;500;400;500\nobject-threshold=1\n");
  /* inside SQ, outside SQ, on SQ's left edge (inside), on its right edge (outside) */
  RdsAnalyticsObject objs[] = {
    obj_at (1, 0, 600, 350), obj_at (2, 0, 100, 650), obj_at (3, 0, 400, 350), obj_at (4, 0, 800, 350),
  };
  RdsAnalyticsFrameResult r;

  rds_analytics_core_process_frame (core, 0, 0, objs, 4, &r);
  g_assert_cmpuint (roi_count (&r, "SQ")->count, ==, 2);
  g_assert_cmpuint (roi_count (&r, "ALL")->count, ==, 0);
  /* overcrowding has no inverse */
  g_assert_cmpuint (roi_count (&r, "OC")->count, ==, 2);
  assert_labels (r.objects[0].roi_labels, r.objects[0].n_roi_labels, "");
  assert_labels (r.objects[0].oc_labels, r.objects[0].n_oc_labels, "OC");
  g_assert_true (r.objects[0].has_info);
  assert_labels (r.objects[1].roi_labels, r.objects[1].n_roi_labels, "SQ");
  g_assert_cmpstr (r.objects[1].obj_status, ==, " ROI:SQ");
  assert_labels (r.objects[2].roi_labels, r.objects[2].n_roi_labels, "");
  assert_labels (r.objects[3].roi_labels, r.objects[3].n_roi_labels, "SQ");
  rds_analytics_core_free (core);
}

/* class-id: filtered-out objects get nothing and aren't counted; objCnt counts every class (-1 included); each
 * group has its own filter */
static void
test_class_filter (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=1\n"
      "roi-RF=0;0;1280;0;1280;720;0;720\nclass-id=0;2\n"
      "[overcrowding-stream-0]\nenable=1\nroi-OC=0;0;1280;0;1280;720;0;720\nclass-id=1\nobject-threshold=1\n");
  RdsAnalyticsObject objs[] = {
    obj_at (1, 0, 100, 300), obj_at (2, 1, 200, 300), obj_at (3, 2, 300, 300), obj_at (4, -1, 400, 300),
    obj_at (5, 5, 500, 300),
  };
  RdsAnalyticsFrameResult r;

  rds_analytics_core_process_frame (core, 0, 0, objs, 5, &r);
  g_assert_cmpuint (roi_count (&r, "RF")->count, ==, 2);
  g_assert_cmpuint (roi_count (&r, "OC")->count, ==, 1);
  assert_labels (r.objects[0].roi_labels, r.objects[0].n_roi_labels, "RF");
  assert_labels (r.objects[0].oc_labels, r.objects[0].n_oc_labels, "");
  assert_labels (r.objects[1].roi_labels, r.objects[1].n_roi_labels, "");
  assert_labels (r.objects[1].oc_labels, r.objects[1].n_oc_labels, "OC");
  g_assert_cmpstr (r.objects[1].obj_status, ==, "");
  g_assert_true (r.objects[1].has_info);
  assert_labels (r.objects[2].roi_labels, r.objects[2].n_roi_labels, "RF");
  for (guint i = 3; i < 5; i++) {
    g_assert_cmpuint (r.objects[i].n_roi_labels, ==, 0);
    g_assert_cmpuint (r.objects[i].n_oc_labels, ==, 0);
    g_assert_cmpstr (r.objects[i].obj_status, ==, "");
    g_assert_false (r.objects[i].has_info);
  }
  /* objCnt: every class, -1 last */
  g_assert_cmpuint (r.n_obj_cnt, ==, 5);
  g_assert_cmpint (r.obj_cnt[0].class_id, ==, 0);
  g_assert_cmpint (r.obj_cnt[1].class_id, ==, 1);
  g_assert_cmpint (r.obj_cnt[2].class_id, ==, 2);
  g_assert_cmpint (r.obj_cnt[3].class_id, ==, 5);
  g_assert_cmpint (r.obj_cnt[4].class_id, ==, -1);
  for (guint i = 0; i < 5; i++)
    g_assert_cmpuint (r.obj_cnt[i].count, ==, 1);
  rds_analytics_core_free (core);

  /* class-id=-1 among others means every class */
  core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=1\nroi-RF=0;0;1280;0;1280;720;0;720\n"
      "class-id=-1;1\n");
  rds_analytics_core_process_frame (core, 0, 0, objs, 5, &r);
  g_assert_cmpuint (roi_count (&r, "RF")->count, ==, 5);
  rds_analytics_core_free (core);
}

/* A disabled group has no entries; untracked objects count like the others */
static void
test_disabled_and_untracked (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=0\n"
      "roi-RF=0;0;1280;0;1280;720;0;720\n"
      "[overcrowding-stream-0]\nenable=1\nroi-OC=0;0;1280;0;1280;720;0;720\nobject-threshold=2\n");
  RdsAnalyticsObject objs[] = {
    obj_at (RDS_ANALYTICS_UNTRACKED, 0, 100, 300), obj_at (RDS_ANALYTICS_UNTRACKED, 0, 200, 300),
  };
  RdsAnalyticsFrameResult r;

  rds_analytics_core_process_frame (core, 0, 0, objs, 2, &r);
  g_assert_cmpuint (r.n_roi_counts, ==, 1);
  g_assert_cmpstr (r.roi_counts[0].label, ==, "OC");
  g_assert_cmpuint (r.roi_counts[0].count, ==, 2);
  g_assert_true (r.oc[0].status);
  g_assert_cmpuint (r.objects[0].n_roi_labels, ==, 0);
  assert_labels (r.objects[1].oc_labels, r.objects[1].n_oc_labels, "OC");
  g_assert_cmpint (obj_cnt (&r, 0), ==, 2);
  rds_analytics_core_free (core);
}

/* objCnt over oc_sequence, against DeepStream 8.0's observed values for each window and frame rate. Ties are ours
 * (the value seen most recently); 8.0 breaks them no stable way, so its value at a tie is any of the tied ones.
 * The window includes both ends: at 10 fps, 100 ms holds 2 frames (the default 50 ms holds 1), at 30 fps 100 ms
 * holds 4 frames (the default holds 2), and 1000 ms at 10 fps holds 11. */
static void
test_obj_cnt_window (void)
{
  const struct
  {
    const gchar *win;           /* obj-cnt-win-in-ms; NULL: the default */
    guint fps;
    /* class 0 per frame (-1: no entry yet); 8.0's observed values except at ties */
    gint want[G_N_ELEMENTS (oc_sequence)];
  } cases[] = {
    /* 8.0: 4 at frame 5, 3 at 6, 0 at 11, 4 at 16 (ties) */
    {NULL, 30, {-1, 1, 2, 3, 4, 3, 2, 3, 4, 5, 0, 3, 3, 3, 4, 4, 2, 2, 2, 3}},
    {NULL, 10, {-1, 1, 2, 3, 4, 3, 2, 3, 4, 5, 0, 3, 3, 3, 4, 4, 2, 2, 2, 3}},
    /* 8.0: 0 at frame 11 (a 4-way tie of 0, 3, 4 and 5), 4 at 17 */
    {"100", 30, {-1, 1, 2, 3, 4, 3, 3, 3, 3, 5, 0, 3, 3, 3, 3, 4, 4, 2, 2, 2}},
    /* the same as the default at 30 fps: 2 frames */
    {"100", 10, {-1, 1, 2, 3, 4, 3, 2, 3, 4, 5, 0, 3, 3, 3, 4, 4, 2, 2, 2, 3}},
    /* 8.0: 3 at frame 6 */
    {"1000", 30, {-1, 1, 2, 3, 4, 3, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3}},
    /* 8.0: 3 at frame 6, 4 at 18 (a 3-way tie that only the closed window gives) */
    {"1000", 10, {-1, 1, 2, 3, 4, 3, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 3}},
  };

  for (guint c = 0; c < G_N_ELEMENTS (cases); c++) {
    gchar *text = cases[c].win ? g_strdup_printf (PROPERTY "obj-cnt-win-in-ms=%s\n", cases[c].win)
        : g_strdup (PROPERTY);
    RdsAnalyticsCore *core = core_for (text);

    for (guint i = 0; i < G_N_ELEMENTS (oc_sequence); i++) {
      RdsAnalyticsObject objs[6];
      guint n = oc_sequence_frame (i, objs);
      guint64 pts = cases[c].fps == 30 ? FRAME_30FPS (i) : FRAME_10FPS (i);
      RdsAnalyticsFrameResult r;

      rds_analytics_core_process_frame (core, 0, pts, objs, n, &r);
      if (obj_cnt (&r, 0) != cases[c].want[i])
        g_error ("window %s at %u fps, frame %u: objCnt %d, want %d", cases[c].win ? cases[c].win : "default",
            cases[c].fps, i, obj_cnt (&r, 0), cases[c].want[i]);
      g_assert_cmpint (obj_cnt (&r, 1), ==, 1);
      /* no groups: empty maps */
      g_assert_cmpuint (r.n_roi_counts, ==, 0);
      g_assert_cmpuint (r.n_oc, ==, 0);
    }
    rds_analytics_core_free (core);
    g_free (text);
  }
}

static void
feed_counts (RdsAnalyticsCore * core, guint pad_index, guint64 pts, gint class_id, guint count,
    RdsAnalyticsFrameResult * r)
{
  RdsAnalyticsObject objs[16];

  g_assert_cmpuint (count, <=, G_N_ELEMENTS (objs));
  for (guint k = 0; k < count; k++)
    objs[k] = obj_at (k, class_id, 10 + 20 * k, 100);
  rds_analytics_core_process_frame (core, pad_index, pts, objs, count, r);
}

/* Ties go to the value seen most recently (ours), whatever the order of the values */
static void
test_obj_cnt_ties (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "obj-cnt-win-in-ms=1000\n");
  RdsAnalyticsFrameResult r;

  feed_counts (core, 0, FRAME_30FPS (0), 0, 4, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 4);
  feed_counts (core, 0, FRAME_30FPS (1), 0, 3, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 3);
  feed_counts (core, 0, FRAME_30FPS (2), 0, 4, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 4);
  feed_counts (core, 0, FRAME_30FPS (3), 0, 3, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 3);
  /* a lower count of the same frequency, and a higher one */
  feed_counts (core, 0, FRAME_30FPS (4), 0, 1, &r);
  feed_counts (core, 0, FRAME_30FPS (5), 0, 1, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 1);
  feed_counts (core, 0, FRAME_30FPS (6), 0, 4, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 4);
  rds_analytics_core_free (core);
}

/* objCnt is sorted by the class id as unsigned, -1 last (DeepStream 8.0 behaviour: its display-obj-cnt text reads
 * ClassId0 ... ClassId7 ClassId4294967295), whatever the order the classes are first seen in */
static void
test_obj_cnt_order (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY);
  RdsAnalyticsObject first[] = { obj_at (1, 5, 100, 100), obj_at (2, -1, 200, 100), obj_at (3, 7, 300, 100) };
  RdsAnalyticsObject later[] = { obj_at (4, 2, 100, 100), obj_at (5, -3, 200, 100), obj_at (6, 0, 300, 100) };
  static const gint want[] = { 0, 2, 5, 7, -3, -1 };
  RdsAnalyticsFrameResult r;

  rds_analytics_core_process_frame (core, 0, 0, first, 3, &r);
  g_assert_cmpuint (r.n_obj_cnt, ==, 3);
  g_assert_cmpint (r.obj_cnt[0].class_id, ==, 5);
  g_assert_cmpint (r.obj_cnt[1].class_id, ==, 7);
  g_assert_cmpint (r.obj_cnt[2].class_id, ==, -1);
  rds_analytics_core_process_frame (core, 0, FRAME_30FPS (1), later, 3, &r);
  g_assert_cmpuint (r.n_obj_cnt, ==, G_N_ELEMENTS (want));
  for (guint i = 0; i < G_N_ELEMENTS (want); i++)
    g_assert_cmpint (r.obj_cnt[i].class_id, ==, want[i]);
  /* each class keeps its own window: 5 was 1 then 0 (a tie, the newer), 2 was 0 then 1 */
  g_assert_cmpuint (r.obj_cnt[2].count, ==, 0);
  g_assert_cmpuint (r.obj_cnt[1].count, ==, 1);
  rds_analytics_core_free (core);
}

/* The window holds the frames whose PTS is at most W before the current one: 5, 5, 3 gives 5 when the first frame
 * is exactly W back, and the tie 5, 3 (so 3) when it is 1 ns further */
static void
test_obj_cnt_window_end (void)
{
  const guint64 ms = 1000000;

  for (guint past = 0; past < 2; past++) {
    RdsAnalyticsCore *core = core_for (PROPERTY "obj-cnt-win-in-ms=100\n");
    RdsAnalyticsFrameResult r;

    feed_counts (core, 0, 0, 0, 5, &r);
    feed_counts (core, 0, 50 * ms, 0, 5, &r);
    feed_counts (core, 0, 100 * ms + past, 0, 3, &r);
    g_assert_cmpint (obj_cnt (&r, 0), ==, (past ? 3 : 5));
    rds_analytics_core_free (core);
  }
}

/* A class keeps its entry at 0 once gone, the frames before it was first seen count 0, and a PTS going back or
 * missing restarts the window (ours) */
static void
test_obj_cnt_edges (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "obj-cnt-win-in-ms=100\n");
  RdsAnalyticsFrameResult r;
  const guint64 ms = 1000000;

  feed_counts (core, 0, 0, 0, 0, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, -1);
  feed_counts (core, 0, 10 * ms, 0, 0, &r);
  /* 0, 0, 2: the frames before the class count 0 */
  feed_counts (core, 0, 20 * ms, 0, 2, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 0);
  feed_counts (core, 0, 30 * ms, 0, 2, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 2);
  /* gone: the entry stays, at 0 once the window holds more empty frames */
  for (guint i = 1; i <= 5; i++)
    feed_counts (core, 0, 30 * ms + i * 20 * ms, 1, 1, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 0);
  g_assert_cmpint (obj_cnt (&r, 1), ==, 1);
  g_assert_cmpuint (r.n_obj_cnt, ==, 2);

  /* back in time: only the new frame */
  feed_counts (core, 0, 0, 0, 3, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 3);
  g_assert_cmpint (obj_cnt (&r, 1), ==, 0);
  feed_counts (core, 0, 10 * ms, 0, 5, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 5);
  feed_counts (core, 0, 20 * ms, 0, 3, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 3);
  /* no PTS: only the frame itself, then the next PTS starts afresh */
  feed_counts (core, 0, RDS_ANALYTICS_PTS_NONE, 0, 7, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 7);
  feed_counts (core, 0, 30 * ms, 0, 6, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 6);
  rds_analytics_core_free (core);
}

/* At most 9000 frames in the window (ours): 4501 frames of 1 then 4500 of 2 in a window of 10^9 ms tie once the
 * first frame is out, so 2 */
static void
test_obj_cnt_max_frames (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "obj-cnt-win-in-ms=1000000000\n");
  RdsAnalyticsFrameResult r;
  guint i;

  for (i = 0; i < 4501; i++)
    feed_counts (core, 0, FRAME_30FPS (i), 0, 1, &r);
  for (; i < 9000; i++)
    feed_counts (core, 0, FRAME_30FPS (i), 0, 2, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 1);
  feed_counts (core, 0, FRAME_30FPS (i), 0, 2, &r);
  g_assert_cmpint (obj_cnt (&r, 0), ==, 2);
  rds_analytics_core_free (core);
}

/* A stream whose groups are all disabled is a stream without groups: empty maps, no object info, objCnt filled */
static void
test_all_disabled (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=0\n"
      "roi-RF=0;0;1280;0;1280;720;0;720\n"
      "[overcrowding-stream-0]\nenable=0\nroi-OC=0;0;1280;0;1280;720;0;720\nobject-threshold=1\n");
  RdsAnalyticsObject objs[] = { obj_at (1, 0, 100, 300), obj_at (2, 4, 200, 300) };
  RdsAnalyticsFrameResult r;

  rds_analytics_core_process_frame (core, 0, 0, objs, 2, &r);
  g_assert_cmpuint (r.n_roi_counts, ==, 0);
  g_assert_cmpuint (r.n_oc, ==, 0);
  g_assert_cmpuint (r.n_objects, ==, 2);
  for (guint i = 0; i < 2; i++) {
    g_assert_cmpuint (r.objects[i].n_roi_labels, ==, 0);
    g_assert_cmpuint (r.objects[i].n_oc_labels, ==, 0);
    g_assert_cmpstr (r.objects[i].obj_status, ==, "");
    g_assert_false (r.objects[i].has_info);
  }
  g_assert_cmpint (obj_cnt (&r, 0), ==, 1);
  g_assert_cmpint (obj_cnt (&r, 4), ==, 1);
  rds_analytics_core_free (core);
}

/* Streams are kept apart: their own groups, and their own objCnt windows */
static void
test_two_streams (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "obj-cnt-win-in-ms=1000\n"
      "[roi-filtering-stream-1]\nenable=1\nroi-RF=0;0;1280;0;1280;720;0;720\n");
  RdsAnalyticsFrameResult r;

  for (guint i = 0; i < 3; i++) {
    feed_counts (core, 0, FRAME_30FPS (i), 0, 2, &r);
    g_assert_cmpuint (r.n_roi_counts, ==, 0);
    g_assert_cmpuint (r.n_objects, ==, 2);
    g_assert_false (r.objects[0].has_info);
    g_assert_cmpint (obj_cnt (&r, 0), ==, 2);
    g_assert_cmpint (obj_cnt (&r, 1), ==, -1);
    feed_counts (core, 1, FRAME_30FPS (i), 1, 5, &r);
    g_assert_cmpuint (r.n_roi_counts, ==, 1);
    g_assert_cmpuint (r.roi_counts[0].count, ==, 5);
    g_assert_true (r.objects[0].has_info);
    g_assert_cmpint (obj_cnt (&r, 0), ==, -1);
    g_assert_cmpint (obj_cnt (&r, 1), ==, 5);
  }
  /* a pad with no groups and no frames yet */
  feed_counts (core, 7, 0, 3, 1, &r);
  g_assert_cmpuint (r.n_roi_counts, ==, 0);
  g_assert_cmpuint (r.n_obj_cnt, ==, 1);
  g_assert_cmpint (obj_cnt (&r, 3), ==, 1);
  rds_analytics_core_free (core);
}

/* The rules are scaled to the mux output */
static void
test_scaled_rules (void)
{
  RdsAnalyticsConfig *cfg = load_config ("[property]\nenable=1\nconfig-width=1920\nconfig-height=1080\n"
      "[roi-filtering-stream-0]\nenable=1\nroi-SQ=600;300;1200;300;1200;750;600;750\n");
  RdsAnalyticsCore *core = rds_analytics_core_new (cfg, 1280, 720);
  /* SQ at 1280x720: 400;200;800;200;800;500;400;500 */
  RdsAnalyticsObject objs[] = { obj_at (1, 0, 400, 350), obj_at (2, 0, 799, 499), obj_at (3, 0, 800, 350) };
  RdsAnalyticsFrameResult r;

  rds_analytics_config_free (cfg);
  rds_analytics_core_process_frame (core, 0, 0, objs, 3, &r);
  g_assert_cmpuint (roi_count (&r, "SQ")->count, ==, 2);
  g_assert_cmpuint (r.objects[2].n_roi_labels, ==, 0);
  rds_analytics_core_free (core);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Direction and STOPPED
 *
 * The scenarios below are the ones DeepStream 8.0 was run on, object by object, and the expected strings are its
 * observed states, frame by frame: runs of "<state>@<first frame>-<last frame>", where the state is STOP
 * (" STOPPED "), the direction labels joined by "+", or "-" (neither). A run also ends at an absence. */

#define DIR_E PROPERTY "[direction-detection-stream-0]\nenable=1\ndirection-E=400;360;600;360\nclass-id=-1\n"
#define DIR_S PROPERTY "[direction-detection-stream-0]\nenable=1\ndirection-S=400;360;400;560\nclass-id=-1\n"

/* A core for @text on a @w x @h mux */
static RdsAnalyticsCore *
core_for_size (const gchar * text, guint w, guint h)
{
  RdsAnalyticsConfig *cfg = load_config (text);
  RdsAnalyticsCore *core = rds_analytics_core_new (cfg, w, h);

  rds_analytics_config_free (cfg);
  g_assert_nonnull (core);
  return core;
}

/* Where object @i (from 0) is at frame @f: its bottom centre, and its box height when it isn't the default; FALSE
 * when it is absent */
typedef gboolean (*PathFunc) (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h);

/* One object's state, checking that its strings agree with it */
static gchar *
dir_state (const RdsAnalyticsObjectResult * r)
{
  gchar **pieces;
  GString *s;

  g_assert_nonnull (r->dir_status);
  if (r->stopped) {
    g_assert_cmpstr (r->dir_status, ==, "");
    g_assert_true (g_str_has_suffix (r->obj_status, " STOPPED "));
    g_assert_true (r->has_info);
    return g_strdup ("STOP");
  }
  g_assert_null (strstr (r->obj_status, "STOPPED"));
  g_assert_true (g_str_has_suffix (r->obj_status, r->dir_status));
  if (r->dir_status[0] == '\0')
    return g_strdup ("-");
  g_assert_true (r->has_info);
  /* " DIR:E  DIR:SE " -> "E+SE" */
  g_assert_true (g_str_has_prefix (r->dir_status, " DIR:") && g_str_has_suffix (r->dir_status, " "));
  pieces = g_strsplit (r->dir_status, " DIR:", -1);
  s = g_string_new (NULL);
  for (guint k = 1; pieces[k] != NULL; k++) {
    gchar *label = pieces[k];

    g_assert_true (g_str_has_suffix (label, " "));
    label[strlen (label) - 1] = '\0';
    g_string_append_printf (s, "%s%s", k > 1 ? "+" : "", label);
  }
  g_strfreev (pieces);
  return g_string_free (s, FALSE);
}

/* Runs @n_objs objects with ids 1..@n_objs, @bw x @bh boxes (unless @fn gives another height) at the bottom
 * centres @fn gives, for @n_frames frames of stream 0 through @core. Returns each object's states as runs (a
 * NULL-terminated array for g_strfreev). */
static gchar **
run_paths (RdsAnalyticsCore * core, guint n_objs, guint n_frames, gdouble bw, gdouble bh, PathFunc fn)
{
  RdsAnalyticsObject *objs = g_new (RdsAnalyticsObject, n_objs);
  guint *index = g_new (guint, n_objs);
  GString **runs = g_new (GString *, n_objs);
  gchar **prev = g_new0 (gchar *, n_objs);
  gint *first = g_new (gint, n_objs), *last = g_new (gint, n_objs);
  gchar **out = g_new0 (gchar *, n_objs + 1);

  for (guint i = 0; i < n_objs; i++)
    runs[i] = g_string_new (NULL);
  for (guint f = 0; f < n_frames; f++) {
    RdsAnalyticsFrameResult r;
    guint n = 0;

    for (guint i = 0; i < n_objs; i++) {
      gdouble cx, by, h = bh;

      if (!fn (i, f, &cx, &by, &h))
        continue;
      objs[n] = (RdsAnalyticsObject) { i + 1, 0, (gfloat) (cx - bw / 2), (gfloat) (by - h), (gfloat) bw,
        (gfloat) h };
      index[n++] = i;
    }
    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, n, &r);
    g_assert_cmpuint (r.n_objects, ==, n);
    for (guint k = 0; k < n; k++) {
      guint i = index[k];
      gchar *st = dir_state (&r.objects[k]);

      if (prev[i] != NULL && g_str_equal (prev[i], st) && last[i] == (gint) f - 1) {
        last[i] = f;
        g_free (st);
        continue;
      }
      if (prev[i] != NULL)
        g_string_append_printf (runs[i], "%s%s@%d-%d", runs[i]->len ? " " : "", prev[i], first[i], last[i]);
      g_free (prev[i]);
      prev[i] = st;
      first[i] = last[i] = f;
    }
  }
  for (guint i = 0; i < n_objs; i++) {
    if (prev[i] != NULL)
      g_string_append_printf (runs[i], "%s%s@%d-%d", runs[i]->len ? " " : "", prev[i], first[i], last[i]);
    g_free (prev[i]);
    out[i] = g_string_free (runs[i], FALSE);
  }
  g_free (objs);
  g_free (index);
  g_free (runs);
  g_free (prev);
  g_free (first);
  g_free (last);
  return out;
}

static void
assert_runs (gchar ** got, const gchar * const *want, guint n)
{
  g_assert_cmpuint (g_strv_length (got), ==, n);
  for (guint i = 0; i < n; i++) {
    if (!g_str_equal (got[i], want[i]))
      g_test_message ("object %u", i + 1);
    g_assert_cmpstr (got[i], ==, want[i]);
  }
  g_strfreev (got);
}

/* Runs the paths on a fresh core for @config on a @w x @h mux, 20x40 boxes, and checks the states */
static void
check_paths (const gchar * config, guint w, guint h, guint n_frames, PathFunc fn, const gchar * const *want,
    guint n_objs)
{
  RdsAnalyticsCore *core = core_for_size (config, w, h);

  assert_runs (run_paths (core, n_objs, n_frames, 20, 40, fn), want, n_objs);
  rds_analytics_core_free (core);
}

#define RAD(deg) ((deg) * (G_PI / 180.0))

/* the probe's grid of 8 columns, 220 x 120 px apart */
static void
grid8 (guint i, gdouble * x, gdouble * y)
{
  *x = 100 + (i % 8) * 220;
  *y = 100 + (i / 8) * 120;
}

/* ... and the one 110 px apart, from y 80 */
static void
grid8_110 (guint i, gdouble * x, gdouble * y)
{
  *x = 100 + (i % 8) * 220;
  *y = 80 + (i / 8) * 110;
}

/* Ramps from rest: static, then east at v px/frame from frame 60 on (3840x1080 frames). The first direction comes
 * 3 frames after the start at 50 px/f, 41 frames after at 1.5 px/f, and never at 1 px/f. */
static const gdouble ramp_speeds[] = { 1, 1.5, 2, 3, 5, 8, 10, 15, 20, 30, 50 };

static gboolean
ramp_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  grid8 (i, cx, by);
  *cx += ramp_speeds[i] * MAX (0, (gint) f - 60);
  return TRUE;
}

static void
test_dir_ramp (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-119",                       /* 1 px/f */
    "-@0-49 STOP@50-100 E@101-119",             /* 1.5 */
    "-@0-49 STOP@50-89 E@90-119",               /* 2 */
    "-@0-49 STOP@50-80 E@81-119",               /* 3 */
    "-@0-49 STOP@50-74 E@75-119",               /* 5 */
    "-@0-49 STOP@50-69 E@70-119",               /* 8 */
    "-@0-49 STOP@50-68 E@69-119",               /* 10 */
    "-@0-49 STOP@50-66 E@67-119",               /* 15 */
    "-@0-49 STOP@50-64 E@65-119",               /* 20 */
    "-@0-49 STOP@50-63 E@64-119",               /* 30 */
    "-@0-49 STOP@50-62 E@63-119",               /* 50 */
  };

  check_paths (DIR_E, 3840, 1080, 120, ramp_path, want, G_N_ELEMENTS (want));
}

/* East from frame 0 at v px/frame (1920x1080): the first direction comes at observation 10 from 6 px/f on, never
 * at 9; below 1.39 px/f the object is STOPPED from observation 50 */
static const gdouble onset_speeds[] = { 0.25, 0.5, 0.75, 1, 1.5, 2, 2.5, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 20 };

static gboolean
onset_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 150 + (i % 6) * 180 + onset_speeds[i] * f;
  *by = 150 + (i / 6) * 170;
  return TRUE;
}

static void
test_dir_onset (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-79", "-@0-49 STOP@50-79", "-@0-49 STOP@50-79", "-@0-49 STOP@50-79",   /* 0.25 .. 1 */
    "-@0-36 E@37-79",                           /* 1.5 */
    "-@0-27 E@28-79",                           /* 2 */
    "-@0-22 E@23-79",                           /* 2.5 */
    "-@0-18 E@19-79",                           /* 3 */
    "-@0-14 E@15-79",                           /* 4: the mean of the first two points as the reference */
    "-@0-11 E@12-79",                           /* 5 */
    "-@0-9 E@10-79", "-@0-9 E@10-79", "-@0-9 E@10-79", "-@0-9 E@10-79", "-@0-9 E@10-79", "-@0-9 E@10-79",
    "-@0-9 E@10-79", "-@0-9 E@10-79",           /* 6 .. 20 */
  };

  check_paths (DIR_E, 1920, 1080, 80, onset_path, want, G_N_ELEMENTS (want));
}

/* Static, then a one-frame jump of J px east at frame jump_at, then static again (1920x1080) */
static const gdouble jump_sizes[] = { 30, 50, 52, 53, 54, 55, 56, 58, 60, 70, 100, 200 };
static guint jump_at;

static gboolean
jump_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  grid8 (i, cx, by);
  if (f >= jump_at)
    *cx += jump_sizes[i];
  return TRUE;
}

/* A jump at frame 5 shows at observation 10, not before; at frame 20, 55 px shows at once and 54 never */
static void
test_dir_jump_filling (void)
{
  const gchar *want5[] = {
    "-@0-49 STOP@50-149", "-@0-49 STOP@50-149", "-@0-49 STOP@50-149", "-@0-49 STOP@50-149",
    "-@0-49 STOP@50-149",                       /* 30 .. 54 */
    "-@0-9 E@10-49 STOP@50-149", "-@0-9 E@10-49 STOP@50-149", "-@0-9 E@10-49 STOP@50-149",
    "-@0-9 E@10-49 STOP@50-149", "-@0-9 E@10-49 STOP@50-149",       /* 55 .. 70 */
    "-@0-9 E@10-51 STOP@52-149",                /* 100 */
    "-@0-9 E@10-52 -@53-53 STOP@54-149",        /* 200 */
  };
  const gchar *want20[] = {
    "-@0-49 STOP@50-149",                       /* 30 */
    "-@0-58 STOP@59-149",                       /* 50 */
    "-@0-59 STOP@60-149",                       /* 52 */
    "-@0-60 STOP@61-149",                       /* 53 */
    "-@0-60 STOP@61-149",                       /* 54 */
    "-@0-19 E@20-61 STOP@62-149",               /* 55 */
    "-@0-19 E@20-61 STOP@62-149",               /* 56 */
    "-@0-19 E@20-62 STOP@63-149",               /* 58 */
    "-@0-19 E@20-62 STOP@63-149",               /* 60 */
    "-@0-19 E@20-64 STOP@65-149",               /* 70 */
    "-@0-19 E@20-66 STOP@67-149",               /* 100 */
    "-@0-19 E@20-67 -@68-68 STOP@69-149",       /* 200 */
  };

  jump_at = 5;
  check_paths (DIR_E, 1920, 1080, 150, jump_path, want5, G_N_ELEMENTS (want5));
  jump_at = 20;
  check_paths (DIR_E, 1920, 1080, 150, jump_path, want20, G_N_ELEMENTS (want20));
}

/* After 60 static frames, a jump of J px east leaves STOPPED 7 frames later at 55 px, at once from 150 px, and
 * comes back to it 49 frames after the jump less that delay (1920x1080) */
static const gdouble jumpfine_sizes[] = {
  55, 56, 57, 58, 59, 60, 62, 64, 66, 68, 70, 73, 76, 80, 85, 90, 95, 100, 110, 120, 135, 150, 175, 200, 250, 300,
  400, 600, 800, 1000
};

static gboolean
jumpfine_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 20 + (i % 6) * 20 + (f >= 60 ? jumpfine_sizes[i] : 0);
  *by = 60 + (i / 6) * 200;
  return TRUE;
}

static void
test_dir_jump_stopped (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-66 E@67-101 STOP@102-169", "-@0-49 STOP@50-66 E@67-101 STOP@102-169",
    "-@0-49 STOP@50-66 E@67-101 STOP@102-169",  /* 55 .. 57 */
    "-@0-49 STOP@50-65 E@66-102 STOP@103-169", "-@0-49 STOP@50-65 E@66-102 STOP@103-169",
    "-@0-49 STOP@50-65 E@66-102 STOP@103-169",  /* 58 .. 60 */
    "-@0-49 STOP@50-64 E@65-103 STOP@104-169", "-@0-49 STOP@50-64 E@65-103 STOP@104-169",
    "-@0-49 STOP@50-64 E@65-103 STOP@104-169",  /* 62 .. 66 */
    "-@0-49 STOP@50-63 E@64-104 STOP@105-169", "-@0-49 STOP@50-63 E@64-104 STOP@105-169",
    "-@0-49 STOP@50-63 E@64-104 STOP@105-169",  /* 68 .. 73 */
    "-@0-49 STOP@50-62 E@63-105 STOP@106-169", "-@0-49 STOP@50-62 E@63-105 STOP@106-169",   /* 76, 80 */
    "-@0-49 STOP@50-61 E@62-106 STOP@107-169", "-@0-49 STOP@50-61 E@62-106 STOP@107-169",
    "-@0-49 STOP@50-61 E@62-106 STOP@107-169", "-@0-49 STOP@50-61 E@62-106 STOP@107-169",   /* 85 .. 100 */
    "-@0-49 STOP@50-60 E@61-107 STOP@108-169", "-@0-49 STOP@50-60 E@61-107 STOP@108-169",
    "-@0-49 STOP@50-60 E@61-107 STOP@108-169",  /* 110 .. 135 */
    "-@0-49 STOP@50-59 E@60-107 -@108-108 STOP@109-169", "-@0-49 STOP@50-59 E@60-107 -@108-108 STOP@109-169",
    "-@0-49 STOP@50-59 E@60-107 -@108-108 STOP@109-169",    /* 150 .. 200 */
    "-@0-49 STOP@50-59 E@60-108 STOP@109-169", "-@0-49 STOP@50-59 E@60-108 STOP@109-169",
    "-@0-49 STOP@50-59 E@60-108 STOP@109-169", "-@0-49 STOP@50-59 E@60-108 STOP@109-169",
    "-@0-49 STOP@50-59 E@60-108 STOP@109-169", "-@0-49 STOP@50-59 E@60-108 STOP@109-169",   /* 250 .. 1000 */
  };

  check_paths (DIR_E, 1920, 1080, 170, jumpfine_path, want, G_N_ELEMENTS (want));
}

/* Steps east at frame 60 of 30..50 px: up to 40 px the object stays STOPPED (sx peaks at J / 2 = 20.0, and the
 * test is inclusive), from 42 px it leaves it for a while, with no direction (below 55 px) */
static const gdouble stop_x_sizes[] = { 30, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50, 31, 33, 35 };

static gboolean
stop_x_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  grid8_110 (i, cx, by);
  if (f >= 60)
    *cx += stop_x_sizes[i];
  return TRUE;
}

static void
test_dir_stopped_step (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-129", "-@0-49 STOP@50-129", "-@0-49 STOP@50-129", "-@0-49 STOP@50-129",
    "-@0-49 STOP@50-129", "-@0-49 STOP@50-129", /* 30 .. 40 */
    "-@0-49 STOP@50-76 -@77-91 STOP@92-129",    /* 42 */
    "-@0-49 STOP@50-73 -@74-94 STOP@95-129",    /* 44 */
    "-@0-49 STOP@50-71 -@72-96 STOP@97-129",    /* 46 */
    "-@0-49 STOP@50-70 -@71-97 STOP@98-129",    /* 48 */
    "-@0-49 STOP@50-69 -@70-98 STOP@99-129",    /* 50 */
    "-@0-49 STOP@50-129", "-@0-49 STOP@50-129", "-@0-49 STOP@50-129",       /* 31, 33, 35 */
  };

  /* 1920x1080 frames, the rule in a 1280x720 config space */
  check_paths (DIR_E, 1920, 1080, 130, stop_x_path, want, G_N_ELEMENTS (want));
}

/* Moving east at v px/frame for M frames, then stopping: STOPPED this many frames after the stop, whatever M; and
 * static objects appearing at frames 0, 10 and 30, STOPPED at exactly their observation 50 */
static const struct
{
  gdouble v;
  guint m;
} stop_moves[] = {
  {2, 15}, {2, 25}, {2, 40}, {3, 15}, {3, 25}, {3, 40}, {5, 15}, {5, 25}, {5, 40}, {10, 15}, {10, 25}, {10, 40},
  {20, 15}, {20, 25}, {20, 40},
};

static gboolean
stop_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  static const guint appear[] = { 0, 10, 30 };

  grid8 (i, cx, by);
  if (i < G_N_ELEMENTS (stop_moves)) {
    *cx += stop_moves[i].v * MIN (f, stop_moves[i].m);
    return TRUE;
  }
  return f >= appear[i - G_N_ELEMENTS (stop_moves)];
}

static void
test_dir_stopped (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-149",                       /* 2 px/f for 15 frames */
    "-@0-49 STOP@50-149",                       /* 2, 25 */
    "-@0-27 E@28-59 STOP@60-149",               /* 2, 40 */
    "-@0-49 STOP@50-149",                       /* 3, 15 */
    "-@0-18 E@19-53 STOP@54-149",               /* 3, 25 */
    "-@0-18 E@19-68 STOP@69-149",               /* 3, 40 */
    "-@0-11 E@12-49 STOP@50-149",               /* 5, 15 */
    "-@0-11 E@12-59 STOP@60-149",               /* 5, 25 */
    "-@0-11 E@12-74 STOP@75-149",               /* 5, 40 */
    "-@0-9 E@10-55 STOP@56-149",                /* 10, 15 */
    "-@0-9 E@10-65 STOP@66-149",                /* 10, 25 */
    "-@0-9 E@10-80 STOP@81-149",                /* 10, 40 */
    "-@0-9 E@10-59 STOP@60-149",                /* 20, 15 */
    "-@0-9 E@10-69 STOP@70-149",                /* 20, 25 */
    "-@0-9 E@10-84 STOP@85-149",                /* 20, 40 */
    "-@0-49 STOP@50-149",                       /* static from frame 0 */
    "-@10-59 STOP@60-149",                      /* from 10 */
    "-@30-79 STOP@80-149",                      /* from 30 */
  };

  check_paths (DIR_E, 1920, 1080, 150, stop_path, want, G_N_ELEMENTS (want));
}

/* Out-and-back spikes and double steps from STOPPED, at frame 60 (1920x1080): a 1- or 2-frame spike of 100 px
 * gives nothing, a 3-frame one a direction for one frame */
static gboolean
spike_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  gdouble dx = 0;

  grid8_110 (i, cx, by);
  switch (i) {
    case 0:                    /* a step of 100 */
      dx = f >= 60 ? 100 : 0;
      break;
    case 1: case 2: case 3: case 4: case 5:    /* 100 px for 1, 2, 3, 5, 8 frames */
    {
      static const guint len[] = { 1, 2, 3, 5, 8 };

      dx = f >= 60 && f < 60 + len[i - 1] ? 100 : 0;
      break;
    }
    case 6:                    /* 200 px for 1 frame */
      dx = f == 60 ? 200 : 0;
      break;
    case 7:                    /* 30 at 60, 30 more at 61 */
      dx = (f >= 60 ? 30 : 0) + (f >= 61 ? 30 : 0);
      break;
    case 8:                    /* a step of 60 */
      dx = f >= 60 ? 60 : 0;
      break;
    case 9:                    /* 20 at 60, 61 and 62 */
      dx = 20 * CLAMP ((gint) f - 59, 0, 3);
      break;
    case 10:                   /* 40 at 60, 20 more at 61 */
      dx = (f >= 60 ? 40 : 0) + (f >= 61 ? 20 : 0);
      break;
    case 11:                   /* 30 at 60, 30 more at 65 */
      dx = (f >= 60 ? 30 : 0) + (f >= 65 ? 30 : 0);
      break;
    case 12:                   /* 100 at 60, back 50 at 61 */
      dx = (f >= 60 ? 100 : 0) - (f >= 61 ? 50 : 0);
      break;
    default:                   /* steps of 55, 70, 150 */
    {
      static const gdouble j[] = { 55, 70, 150 };

      dx = f >= 60 ? j[i - 13] : 0;
      break;
    }
  }
  *cx += dx;
  return TRUE;
}

static void
test_dir_spikes (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-61 E@62-106 STOP@107-129",  /* step 100 */
    "-@0-49 STOP@50-129",                       /* spike 100 for 1 frame */
    "-@0-49 STOP@50-129",                       /* for 2 */
    "-@0-49 STOP@50-61 E@62-62 -@63-109 STOP@110-129",      /* for 3 */
    "-@0-49 STOP@50-61 E@62-64 -@65-111 STOP@112-129",      /* for 5 */
    "-@0-49 STOP@50-61 E@62-67 -@68-114 STOP@115-129",      /* for 8 */
    "-@0-49 STOP@50-59 E@60-60 -@61-109 STOP@110-129",      /* spike 200 for 1 */
    "-@0-49 STOP@50-66 E@67-102 STOP@103-129",  /* 30 + 30 */
    "-@0-49 STOP@50-65 E@66-102 STOP@103-129",  /* step 60 */
    "-@0-49 STOP@50-66 E@67-103 STOP@104-129",  /* 20 x 3 */
    "-@0-49 STOP@50-66 E@67-102 STOP@103-129",  /* 40 + 20 */
    "-@0-49 STOP@50-69 E@70-103 STOP@104-129",  /* 30, then 30 at 65 */
    "-@0-49 STOP@50-64 -@65-101 STOP@102-129",  /* 100, back 50 */
    "-@0-49 STOP@50-66 E@67-101 STOP@102-129",  /* step 55 */
    "-@0-49 STOP@50-63 E@64-104 STOP@105-129",  /* step 70 */
    "-@0-49 STOP@50-59 E@60-107 -@108-108 STOP@109-129",    /* step 150 */
  };

  check_paths (DIR_E, 1920, 1080, 130, spike_path, want, G_N_ELEMENTS (want));
}

/* Absences inside the window shift every edge by the absence: the window counts observations, not frames */
static gboolean
absent_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  static const struct
  {
    gdouble step;               /* px east from frame 60 on; < 0: a ramp of 5 px/f from frame 60 */
    guint gap_first, gap_end;   /* absent in [first, end) */
  } p[] = {
    {200, 0, 0}, {200, 70, 80}, {200, 61, 63}, {55, 0, 0}, {55, 61, 63}, {0, 20, 30}, {0, 55, 57}, {-1, 0, 0},
    {-1, 63, 65},
  };

  if (f >= p[i].gap_first && f < p[i].gap_end)
    return FALSE;
  grid8_110 (i, cx, by);
  if (p[i].step < 0)
    *cx += 5 * MAX (0, (gint) f - 60);
  else if (f >= 60)
    *cx += p[i].step;
  return TRUE;
}

static void
test_dir_absences (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-59 E@60-107 -@108-108 STOP@109-139",    /* step 200 */
    "-@0-49 STOP@50-59 E@60-69 E@80-117 -@118-118 STOP@119-139",    /* step 200, absent 70-79 */
    "-@0-49 STOP@50-59 E@60-60 E@63-109 -@110-110 STOP@111-139",    /* step 200, absent 61-62 */
    "-@0-49 STOP@50-66 E@67-101 STOP@102-139",  /* step 55 */
    "-@0-49 STOP@50-60 STOP@63-68 E@69-103 STOP@104-139",   /* step 55, absent 61-62 */
    "-@0-19 -@30-59 STOP@60-139",               /* static, absent 20-29: STOPPED at its observation 50 */
    "-@0-49 STOP@50-54 STOP@57-139",            /* static, absent 55-56 */
    "-@0-49 STOP@50-74 E@75-139",               /* ramp 5 px/f */
    "-@0-49 STOP@50-62 STOP@65-74 E@75-139",    /* ramp, absent 63-64 */
  };

  check_paths (DIR_E, 1920, 1080, 140, absent_path, want, G_N_ELEMENTS (want));
}

/* South steps at frame 60, rule S: 55 px gives a direction and 54 doesn't (|d| >= 55); every step leaves STOPPED
 * at once, as sy's limit is 0.1 * sqrt (40) = 0.63 px */
static const gdouble ystep_sizes[] = { 50, 53, 54, 55, 56, 58, 60, 65, 70, 100 };

static gboolean
ystep_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  grid8_110 (i, cx, by);
  if (f >= 60)
    *by += ystep_sizes[i];
  return TRUE;
}

static void
test_dir_threshold (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-59 -@60-108 STOP@109-129", "-@0-49 STOP@50-59 -@60-108 STOP@109-129",
    "-@0-49 STOP@50-59 -@60-108 STOP@109-129",  /* 50, 53, 54 */
    "-@0-49 STOP@50-59 S@60-105 -@106-108 STOP@109-129",    /* 55 */
    "-@0-49 STOP@50-59 S@60-105 -@106-108 STOP@109-129", "-@0-49 STOP@50-59 S@60-105 -@106-108 STOP@109-129",
    "-@0-49 STOP@50-59 S@60-105 -@106-108 STOP@109-129", "-@0-49 STOP@50-59 S@60-105 -@106-108 STOP@109-129",
    "-@0-49 STOP@50-59 S@60-105 -@106-108 STOP@109-129",    /* 56 .. 70 */
    "-@0-49 STOP@50-59 S@60-106 -@107-108 STOP@109-129",    /* 100 */
  };

  check_paths (DIR_S, 1920, 1080, 130, ystep_path, want, G_N_ELEMENTS (want));
}

/* sy's limit grows with the box height, 0.1 * sqrt (h): steps south of J px at frame 60, for boxes 10 to 200 px
 * tall (20 wide). sy peaks at J / 2, so 1 px stays STOPPED from h = 40 (limit 0.63) and 2 px from h = 120 (1.10) */
static const struct
{
  gdouble h, j;
  guint slot;                   /* the position in 8.0's run */
} ty_steps[] = {
  {10, 1, 0}, {10, 2, 1}, {20, 2, 7}, {40, 1, 12}, {40, 2, 13}, {80, 1, 18}, {80, 2, 19}, {120, 2, 25}, {120, 3, 26},
  {200, 2, 31}, {200, 4, 33},
};

static gboolean
ty_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h)
{
  *cx = 60 + (ty_steps[i].slot % 10) * 185;
  *by = 220 + (ty_steps[i].slot / 10) * 150 + (f >= 60 ? ty_steps[i].j : 0);
  *h = ty_steps[i].h;
  return TRUE;
}

static void
test_dir_box_height (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-64 -@65-103 STOP@104-129",  /* h 10, 1 px */
    "-@0-49 STOP@50-60 -@61-107 STOP@108-129",  /* h 10, 2 px */
    "-@0-49 STOP@50-61 -@62-106 STOP@107-129",  /* h 20, 2 px */
    "-@0-49 STOP@50-129",                       /* h 40, 1 px */
    "-@0-49 STOP@50-64 -@65-103 STOP@104-129",  /* h 40, 2 px */
    "-@0-49 STOP@50-129",                       /* h 80, 1 px */
    "-@0-49 STOP@50-72 -@73-95 STOP@96-129",    /* h 80, 2 px */
    "-@0-49 STOP@50-129",                       /* h 120, 2 px */
    "-@0-49 STOP@50-66 -@67-101 STOP@102-129",  /* h 120, 3 px */
    "-@0-49 STOP@50-129",                       /* h 200, 2 px */
    "-@0-49 STOP@50-66 -@67-101 STOP@102-129",  /* h 200, 4 px */
  };

  check_paths (DIR_E, 1920, 1080, 130, ty_path, want, G_N_ELEMENTS (want));
}

/* The angle limit is 60 degrees, inclusive, on both sides: 10 px/frame at each angle from east */
static const gdouble fine_angles[] = { 50, 55, 57, 58, 59, 59.5, 60, 60.5, 61, 62, 63, 65, 70, -55, -59, -60, -61,
  -65
};

static gboolean
angle_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 150 + (i % 6) * 180 + 10 * f * cos (RAD (fine_angles[i]));
  *by = 150 + (i / 6) * 170 + 10 * f * sin (RAD (fine_angles[i]));
  return TRUE;
}

static void
test_dir_angle (void)
{
  const gchar *want[] = {
    "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29",
    "-@0-9 E@10-29",                            /* 50 .. 60 */
    "-@0-29", "-@0-29", "-@0-29", "-@0-29", "-@0-29", "-@0-29",     /* 60.5 .. 70 */
    "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29",      /* -55 .. -60 */
    "-@0-29", "-@0-29",                         /* -61, -65 */
  };

  check_paths (DIR_E, 1280, 720, 30, angle_path, want, G_N_ELEMENTS (want));
}

/* Every rule within 60 degrees, in config order: 6 px/frame at 0..100 degrees from east (y down), with E, SE and S
 * written in both orders, and E with a zero vector */
static gboolean
multi_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  gdouble a = RAD (i < 5 ? 10.0 * i : i == 5 ? 45.0 : 10.0 * (i - 1));

  *cx = 150 + (i % 6) * 180 + 6 * f * cos (a);
  *by = 150 + (i / 6) * 170 + 6 * f * sin (a);
  return TRUE;
}

static void
test_dir_multi (void)
{
  const gchar *want[] = {
    "-@0-9 E+SE@10-29", "-@0-9 E+SE@10-29", "-@0-9 E+SE@10-29",     /* 0, 10, 20 */
    "-@0-9 E+SE+S@10-29", "-@0-9 E+SE+S@10-29", "-@0-9 E+SE+S@10-29", "-@0-9 E+SE+S@10-29",
    "-@0-9 E+SE+S@10-29",                       /* 30 .. 60 */
    "-@0-9 SE+S@10-29", "-@0-9 SE+S@10-29", "-@0-9 SE+S@10-29", "-@0-9 SE+S@10-29",       /* 70 .. 100 */
  };
  const gchar *want_rev[] = {
    "-@0-9 SE+E@10-29", "-@0-9 SE+E@10-29", "-@0-9 SE+E@10-29",
    "-@0-9 S+SE+E@10-29", "-@0-9 S+SE+E@10-29", "-@0-9 S+SE+E@10-29", "-@0-9 S+SE+E@10-29",
    "-@0-9 S+SE+E@10-29",
    "-@0-9 S+SE@10-29", "-@0-9 S+SE@10-29", "-@0-9 S+SE@10-29", "-@0-9 S+SE@10-29",
  };
  const gchar *want_e[] = {
    "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-9 E@10-29",
    "-@0-9 E@10-29", "-@0-9 E@10-29", "-@0-29", "-@0-29", "-@0-29", "-@0-29",
  };

  check_paths (PROPERTY "[direction-detection-stream-0]\nenable=1\ndirection-E=400;360;600;360\n"
      "direction-SE=400;360;541;501\ndirection-S=400;360;400;560\nclass-id=-1\n", 1280, 720, 30, multi_path,
      want, G_N_ELEMENTS (want));
  check_paths (PROPERTY "[direction-detection-stream-0]\nenable=1\ndirection-S=400;360;400;560\n"
      "direction-SE=400;360;541;501\ndirection-E=400;360;600;360\nclass-id=-1\n", 1280, 720, 30, multi_path,
      want_rev, G_N_ELEMENTS (want_rev));
  /* a zero vector never matches (DeepStream 8.0 behaviour) */
  check_paths (PROPERTY "[direction-detection-stream-0]\nenable=1\ndirection-Z=400;360;400;360\n"
      "direction-E=400;360;600;360\n", 1280, 720, 30, multi_path, want_e, G_N_ELEMENTS (want_e));
}

/* objStatus: the ROI pieces, then the direction or STOPPED piece; has_info follows */
static void
test_dir_obj_status (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=1\n"
      "roi-RF=100;100;600;100;600;600;100;600\n"
      "[direction-detection-stream-0]\nenable=1\ndirection-East=100;650;300;650\n");
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f <= 50; f++) {
    /* east 8 px/f in RF, static in RF, static outside it */
    RdsAnalyticsObject objs[] = { obj_at (1, 0, 110 + 8 * f, 300), obj_at (2, 0, 300, 400),
      obj_at (3, 0, 900, 400)
    };

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, 3, &r);
    if (f == 9) {
      g_assert_cmpstr (r.objects[0].obj_status, ==, " ROI:RF");
      g_assert_cmpstr (r.objects[0].dir_status, ==, "");
    }
    if (f == 10) {
      g_assert_cmpstr (r.objects[0].obj_status, ==, " ROI:RF DIR:East ");
      g_assert_cmpstr (r.objects[0].dir_status, ==, " DIR:East ");
      g_assert_false (r.objects[0].stopped);
    }
    if (f < 50) {
      g_assert_false (r.objects[1].stopped);
      g_assert_false (r.objects[2].has_info);
      g_assert_cmpstr (r.objects[2].obj_status, ==, "");
    }
  }
  g_assert_cmpstr (r.objects[0].obj_status, ==, " ROI:RF DIR:East ");
  g_assert_true (r.objects[1].stopped);
  g_assert_cmpstr (r.objects[1].obj_status, ==, " ROI:RF STOPPED ");
  g_assert_cmpstr (r.objects[1].dir_status, ==, "");
  assert_labels (r.objects[1].roi_labels, r.objects[1].n_roi_labels, "RF");
  g_assert_true (r.objects[2].stopped);
  g_assert_cmpstr (r.objects[2].obj_status, ==, " STOPPED ");
  g_assert_true (r.objects[2].has_info);
  g_assert_false (r.untracked_warning);
  rds_analytics_core_free (core);
}

/* No STOPPED without a direction group, whatever else the stream has: static objects in an ROI, in an overcrowding
 * ROI and outside, one moving east 1 px/f, one moving down across a line */
static gboolean
feat_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  static const gint p[][2] = { {300, 300}, {900, 300}, {300, 680}, {900, 680}, {640, 300} };

  *cx = p[i][0] + (i == 3 ? f : 0);
  *by = p[i][1] + (i == 4 ? 5 * f : 0);
  return TRUE;
}

static void
test_dir_needs_group (void)
{
  const gchar *configs[] = {
    PROPERTY "[roi-filtering-stream-0]\nenable=1\nroi-RF=100;100;600;100;600;600;100;600\n",
    PROPERTY "[overcrowding-stream-0]\nenable=1\nroi-OC=700;100;1200;100;1200;600;700;600\nobject-threshold=1\n",
    PROPERTY "[line-crossing-stream-0]\nenable=1\nline-crossing-Down=640;300;640;500;500;400;800;400\n",
    PROPERTY "[direction-detection-stream-0]\nenable=0\ndirection-East=100;650;300;650\n",
  };
  const gchar *want[] = { "-@0-69", "-@0-69", "-@0-69", "-@0-69", "-@0-69" };
  const gchar *want_dir[] = {
    "-@0-49 STOP@50-69", "-@0-49 STOP@50-69", "-@0-49 STOP@50-69", "-@0-49 STOP@50-69", "-@0-69",
  };

  for (guint c = 0; c < G_N_ELEMENTS (configs); c++)
    check_paths (configs[c], 1280, 720, 70, feat_path, want, G_N_ELEMENTS (want));
  check_paths (PROPERTY "[direction-detection-stream-0]\nenable=1\ndirection-East=100;650;300;650\n", 1280, 720,
      70, feat_path, want_dir, G_N_ELEMENTS (want_dir));
}

/* Untracked objects get no direction and no STOPPED, and leave the tracked ones alone; the warning comes once per
 * stream, and only from a stream with a direction group (ours) */
static void
test_dir_untracked (void)
{
  RdsAnalyticsCore *core = core_for (DIR_E "[direction-detection-stream-1]\nenable=1\n"
      "direction-E=400;360;600;360\n");
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f < 60; f++) {
    RdsAnalyticsObject objs[] = { obj_at (RDS_ANALYTICS_UNTRACKED, 0, 100 + 10 * f, 300),
      obj_at (1, 0, 100 + 10 * f, 500), obj_at (RDS_ANALYTICS_UNTRACKED, 0, 900, 600)
    };
    guint first = f < 3 ? 1 : 0, n = f < 3 ? 1 : 3;     /* the untracked ones join at frame 3 */

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs + first, n, &r);
    g_assert_cmpint (r.untracked_warning, ==, f == 3);
    for (guint k = 0; k < r.n_objects; k++) {
      const RdsAnalyticsObjectResult *o = &r.objects[k];

      if (objs[first + k].id == RDS_ANALYTICS_UNTRACKED) {
        g_assert_cmpstr (o->dir_status, ==, "");
        g_assert_false (o->stopped);
        g_assert_false (o->has_info);
      } else {
        g_assert_cmpstr (o->dir_status, ==, f >= 10 ? " DIR:E " : "");
      }
    }
    /* stream 1 warns on its own first untracked object, at frame 20; stream 2 has no direction group */
    rds_analytics_core_process_frame (core, 1, FRAME_30FPS (f), f < 20 ? objs + 1 : objs, 1, &r);
    g_assert_cmpint (r.untracked_warning, ==, f == 20);
    rds_analytics_core_process_frame (core, 2, FRAME_30FPS (f), objs, 1, &r);
    g_assert_false (r.untracked_warning);
    g_assert_false (r.objects[0].has_info);
  }
  rds_analytics_core_free (core);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/analytics-core/geometry/scale", test_scale);
  g_test_add_func ("/cpu/analytics-core/geometry/ref-point", test_ref_point);
  g_test_add_func ("/cpu/analytics-core/geometry/square-edges", test_square_edges);
  g_test_add_func ("/cpu/analytics-core/geometry/triangle-apex", test_triangle_apex);
  g_test_add_func ("/cpu/analytics-core/geometry/bow-tie", test_bow_tie);
  g_test_add_func ("/cpu/analytics-core/geometry/orientation", test_orientation);
  g_test_add_func ("/cpu/analytics-core/geometry/degenerate", test_degenerate);
  g_test_add_func ("/cpu/analytics-core/geometry/observed-shapes", test_observed_shapes);
  g_test_add_func ("/cpu/analytics-core/geometry/observed-edges", test_observed_edges);
  g_test_add_func ("/cpu/analytics-core/oc/threshold", test_oc_threshold);
  g_test_add_func ("/cpu/analytics-core/oc/boundaries", test_oc_boundaries);
  g_test_add_func ("/cpu/analytics-core/roi/labels", test_roi_labels);
  g_test_add_func ("/cpu/analytics-core/roi/inverse", test_roi_inverse);
  g_test_add_func ("/cpu/analytics-core/roi/class-filter", test_class_filter);
  g_test_add_func ("/cpu/analytics-core/roi/disabled-untracked", test_disabled_and_untracked);
  g_test_add_func ("/cpu/analytics-core/roi/scaled", test_scaled_rules);
  g_test_add_func ("/cpu/analytics-core/roi/all-disabled", test_all_disabled);
  g_test_add_func ("/cpu/analytics-core/obj-cnt/window", test_obj_cnt_window);
  g_test_add_func ("/cpu/analytics-core/obj-cnt/window-end", test_obj_cnt_window_end);
  g_test_add_func ("/cpu/analytics-core/obj-cnt/order", test_obj_cnt_order);
  g_test_add_func ("/cpu/analytics-core/obj-cnt/ties", test_obj_cnt_ties);
  g_test_add_func ("/cpu/analytics-core/obj-cnt/max-frames", test_obj_cnt_max_frames);
  g_test_add_func ("/cpu/analytics-core/obj-cnt/edges", test_obj_cnt_edges);
  g_test_add_func ("/cpu/analytics-core/streams", test_two_streams);
  g_test_add_func ("/cpu/analytics-core/direction/ramp", test_dir_ramp);
  g_test_add_func ("/cpu/analytics-core/direction/onset", test_dir_onset);
  g_test_add_func ("/cpu/analytics-core/direction/jump-filling", test_dir_jump_filling);
  g_test_add_func ("/cpu/analytics-core/direction/jump-stopped", test_dir_jump_stopped);
  g_test_add_func ("/cpu/analytics-core/direction/stopped-step", test_dir_stopped_step);
  g_test_add_func ("/cpu/analytics-core/direction/stopped", test_dir_stopped);
  g_test_add_func ("/cpu/analytics-core/direction/spikes", test_dir_spikes);
  g_test_add_func ("/cpu/analytics-core/direction/absences", test_dir_absences);
  g_test_add_func ("/cpu/analytics-core/direction/threshold", test_dir_threshold);
  g_test_add_func ("/cpu/analytics-core/direction/box-height", test_dir_box_height);
  g_test_add_func ("/cpu/analytics-core/direction/angle", test_dir_angle);
  g_test_add_func ("/cpu/analytics-core/direction/multi", test_dir_multi);
  g_test_add_func ("/cpu/analytics-core/direction/obj-status", test_dir_obj_status);
  g_test_add_func ("/cpu/analytics-core/direction/needs-group", test_dir_needs_group);
  g_test_add_func ("/cpu/analytics-core/direction/untracked", test_dir_untracked);
  return g_test_run ();
}
