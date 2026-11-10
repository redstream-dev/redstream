/* rdsanalytics core: rule scaling, the reference point and the inside test, against DeepStream 8.0's observed
 * results (the observed boxes below: left, top, width, height in frame pixels, and the ROIs 8.0 reported them in);
 * ROI filtering, overcrowding and the object counts on scripted frames; direction and STOPPED on the scenarios
 * 8.0 was observed on, against its states frame by frame; line crossing; the stream state life cycle (resets and
 * eviction).
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

/* the observed runs' grid of 8 columns, 220 x 120 px apart */
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
  {120, 4, 27}, {200, 2, 31}, {200, 4, 33},
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
    "-@0-49 STOP@50-63 -@64-104 STOP@105-129",  /* h 120, 4 px: STOPPED at 63 and 105 needs a coefficient >= 0.0999 */
    "-@0-49 STOP@50-129",                       /* h 200, 2 px */
    "-@0-49 STOP@50-66 -@67-101 STOP@102-129",  /* h 200, 4 px */
  };

  check_paths (DIR_E, 1920, 1080, 130, ty_path, want, G_N_ELEMENTS (want));
}

/* The sy test is inclusive (DeepStream 8.0 behaviour, as sx's is): an h = 100 box stepping 2 px south at frame 60
 * reaches sy = 1.0 exactly (25 points at each y) against a limit of 0.1 * sqrt (100) = 1.0, both exact in double,
 * and stays STOPPED */
static gboolean
ty_exact_path (guint i G_GNUC_UNUSED, guint f, gdouble * cx, gdouble * by, gdouble * h)
{
  *cx = 300;
  *by = 400 + (f >= 60 ? 2 : 0);
  *h = 100;
  return TRUE;
}

static void
test_dir_box_height_inclusive (void)
{
  const gchar *want[] = { "-@0-49 STOP@50-129" };

  check_paths (DIR_E, 1920, 1080, 130, ty_exact_path, want, G_N_ELEMENTS (want));
}

/* x is clamped at 0 (DeepStream 8.0 behaviour): moving west from x 100 at 1.4 px/frame, the object is moving at
 * observation 50 but STOPPED once the clamped points (x = 0 from frame 71) hold sx under 20; the other speeds,
 * 1.0 to 2.0 px/f, never reach x = 0 */
static const gdouble west_speeds[] = { 1.0, 1.2, 1.3, 1.35, 1.36, 1.37, 1.38, 1.39, 1.4, 1.45, 1.5, 2.0 };

static gboolean
west_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  grid8_110 (i, cx, by);
  *cx -= west_speeds[i] * f;
  *by += 20;
  return TRUE;
}

static void
test_dir_clamp_x (void)
{
  const gchar *want[] = {
    "-@0-49 STOP@50-79", "-@0-49 STOP@50-79", "-@0-49 STOP@50-79", "-@0-49 STOP@50-79", "-@0-49 STOP@50-79",
    "-@0-49 STOP@50-79", "-@0-49 STOP@50-79",   /* 1.0 .. 1.38 px/f: sx <= 20 at observation 50 */
    "-@0-79",                                   /* 1.39 */
    "-@0-73 STOP@74-79",                        /* 1.4, from x 100: clamped at 0 */
    "-@0-79", "-@0-79", "-@0-79",               /* 1.45 .. 2.0 */
  };

  check_paths (DIR_E, 1920, 1080, 80, west_path, want, G_N_ELEMENTS (want));
}

/* The direction group's class-id filters direction and STOPPED (DeepStream 8.0 behaviour): under class-id=0 a
 * class-1 object gets neither, a class-0 one both */
static void
test_dir_class_filter (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[direction-detection-stream-0]\nenable=1\n"
      "direction-E=400;360;600;360\nclass-id=0\n");
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f < 60; f++) {
    RdsAnalyticsObject objs[] = { obj_at (1, 1, 100 + 10 * f, 300), obj_at (2, 0, 100 + 10 * f, 500),
      obj_at (3, 1, 900, 600), obj_at (4, 0, 900, 650)
    };

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, G_N_ELEMENTS (objs), &r);
    g_assert_false (r.objects[0].has_info);
    g_assert_cmpstr (r.objects[0].dir_status, ==, "");
    g_assert_cmpstr (r.objects[1].dir_status, ==, f >= 10 ? " DIR:E " : "");
    g_assert_false (r.objects[2].has_info);
    g_assert_false (r.objects[2].stopped);
    g_assert_cmpint (r.objects[3].stopped, ==, f >= 50);
  }
  rds_analytics_core_free (core);
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
 * stream, and only from a stream with a direction or line-crossing group (ours) */
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

/* ------------------------------------------------------------------------------------------------------------ */
/* Line crossing. As for direction, the scenarios are the ones DeepStream 8.0 was run on, and the expected strings
 * its observed crossings, object by object: "<label>@<frame>" for each frame the object got that lcStatus label. */

#define LC_RULE_DN "line-crossing-Dn=640;300;640;500;40;400;1240;400\n"
#define LC_GROUP "[line-crossing-stream-0]\nenable=1\n"
#define LC_DN(mode) PROPERTY LC_GROUP LC_RULE_DN "class-id=-1\nextended=0\n" mode
#define LC_LOOSE LC_DN ("mode=loose\n")
#define LC_BALANCED LC_DN ("mode=balanced\n")
#define LC_STRICT LC_DN ("mode=strict\n")

/* Runs @n_objs objects with ids 1..@n_objs, @bw x 40 boxes at the bottom centres @fn gives, for @n_frames frames of
 * stream 0 through @core. Returns each object's crossings, "<label>@<frame>" space-separated, as a NULL-terminated
 * array. Checks every frame that each line's objLCCurrCnt is the number of objects with its label, objLCCumCnt the
 * sum of those so far, and that objStatus ends with the " LC:<l> " pieces. */
static gchar **
run_lc (RdsAnalyticsCore * core, guint n_objs, guint n_frames, gdouble bw, PathFunc fn)
{
  RdsAnalyticsObject *objs = g_new (RdsAnalyticsObject, n_objs);
  guint *index = g_new (guint, n_objs);
  GString **runs = g_new (GString *, n_objs);
  gchar **out = g_new0 (gchar *, n_objs + 1);
  GHashTable *cum = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  for (guint i = 0; i < n_objs; i++)
    runs[i] = g_string_new (NULL);
  for (guint f = 0; f < n_frames; f++) {
    RdsAnalyticsFrameResult r;
    guint n = 0;

    for (guint i = 0; i < n_objs; i++) {
      gdouble cx, by, h = 40;

      if (!fn (i, f, &cx, &by, &h))
        continue;
      objs[n] = (RdsAnalyticsObject) { i + 1, 0, (gfloat) (cx - bw / 2), (gfloat) (by - h), (gfloat) bw,
        (gfloat) h };
      index[n++] = i;
    }
    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, n, &r);
    g_assert_cmpuint (r.n_objects, ==, n);
    for (guint l = 0; l < r.n_lc; l++) {
      guint64 counted = 0, *c = g_hash_table_lookup (cum, r.lc[l].label);

      for (guint k = 0; k < n; k++)
        for (guint j = 0; j < r.objects[k].n_lc_labels; j++)
          counted += g_str_equal (r.objects[k].lc_labels[j], r.lc[l].label);
      if (c == NULL) {
        c = g_new0 (guint64, 1);
        g_hash_table_insert (cum, g_strdup (r.lc[l].label), c);
      }
      *c += counted;
      g_assert_cmpuint (r.lc[l].current, ==, counted);
      g_assert_cmpuint (r.lc[l].cumulative, ==, *c);
    }
    for (guint k = 0; k < n; k++) {
      const RdsAnalyticsObjectResult *o = &r.objects[k];
      GString *pieces = g_string_new (NULL);

      for (guint j = 0; j < o->n_lc_labels; j++) {
        g_string_append_printf (runs[index[k]], "%s%s@%u", runs[index[k]]->len ? " " : "", o->lc_labels[j], f);
        g_string_append_printf (pieces, " LC:%s ", o->lc_labels[j]);
      }
      g_assert_true (g_str_has_suffix (o->obj_status, pieces->str));
      if (o->n_lc_labels > 0)
        g_assert_true (o->has_info);
      g_string_free (pieces, TRUE);
    }
  }
  for (guint i = 0; i < n_objs; i++)
    out[i] = g_string_free (runs[i], FALSE);
  g_hash_table_unref (cum);
  g_free (objs);
  g_free (index);
  g_free (runs);
  return out;
}

static void
check_lc (const gchar * config, guint n_frames, gdouble bw, PathFunc fn, const gchar * const *want, guint n_objs)
{
  RdsAnalyticsCore *core = core_for_size (config, 1280, 720);

  assert_runs (run_lc (core, n_objs, n_frames, bw, fn), want, n_objs);
  rds_analytics_core_free (core);
}

/* Objects crossing y = 400 at 8 px/frame, at a degrees from straight down, from 120 px above */
static const gdouble lc_sweep_angles[] = { 0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 70, 75, 80, 85, 87,
  89
};

static gboolean
lc_sweep_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  gdouble a = RAD (lc_sweep_angles[i]);

  *cx = 80 + i * 55 + 8 * sin (a) * f;
  *by = 280 + 8 * cos (a) * f;
  return TRUE;
}

/* The crossing comes once |d| reaches the mode's displacement within its angle; a mode unset is loose */
static void
test_lc_angle_sweep (void)
{
  const gchar *loose[] = {
    "Dn@15", "Dn@16", "Dn@16", "Dn@16", "Dn@16", "Dn@17", "Dn@18", "Dn@19", "Dn@20", "Dn@22", "Dn@24", "Dn@27",
    "Dn@30", "Dn@36", "Dn@44", "", "", "", "", "",      /* 75 .. 89: past the line's end at x 1240 first */
  };
  const gchar *balanced[] = {
    "Dn@16", "Dn@16", "Dn@16", "Dn@16", "Dn@17", "Dn@17", "Dn@18", "Dn@19", "Dn@20", "Dn@22", "Dn@24", "Dn@27",
    "Dn@31", "", "", "", "", "", "", "",
  };
  const gchar *strict[] = {
    "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "", "", "", "", "", "", "", "", "", "",
    "", "",
  };

  check_lc (LC_DN (""), 60, 20, lc_sweep_path, loose, G_N_ELEMENTS (loose));
  check_lc (LC_LOOSE, 60, 20, lc_sweep_path, loose, G_N_ELEMENTS (loose));
  check_lc (LC_BALANCED, 60, 20, lc_sweep_path, balanced, G_N_ELEMENTS (balanced));
  check_lc (LC_STRICT, 60, 20, lc_sweep_path, strict, G_N_ELEMENTS (strict));
}

/* Objects at a degrees from straight down, @lc_angle_speed px/frame, their bottom centre on y = 400 at frame
 * @lc_angle_tcross, spread along the line */
static const gdouble *lc_angles;
static guint lc_n_angles;
static gdouble lc_angle_speed, lc_angle_tcross;

static gboolean
lc_angle_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  gdouble a = RAD (lc_angles[i]), vx = lc_angle_speed * sin (a), vy = lc_angle_speed * cos (a);

  *cx = 60 + i * (1160.0 / MAX (1, lc_n_angles - 1)) - vx * lc_angle_tcross + vx * f;
  *by = 400 - vy * lc_angle_tcross + vy * f;
  return TRUE;
}

static void
check_lc_angles (const gchar * config, const gdouble * angles, guint n, gdouble speed, gdouble tcross,
    guint n_frames, const gchar * const *want)
{
  lc_angles = angles;
  lc_n_angles = n;
  lc_angle_speed = speed;
  lc_angle_tcross = tcross;
  check_lc (config, n_frames, 20, lc_angle_path, want, n);
}

/* The angle limits, on both sides: loose under 90 degrees, balanced under 60, strict under 36.87 (cos > 0.8);
 * 8 px/frame, crossing at frame 30.5 */
static void
test_lc_angle_limits (void)
{
  static const gdouble loose_a[] = { 75, 78, 80, 82, 84, 85, 86, 88, 89, -80, -85, -88 };
  static const gdouble balanced_a[] = { 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, -58, -60, -62 };
  static const gdouble strict_a[] = { 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, -33, -35, -37 };
  const gchar *loose[] = {
    "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31",
  };
  const gchar *balanced[] = {
    "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31",
    "Dn@54",                                    /* 60: the integer points make d a little wider than 60 until then */
    "", "", "", "", "", "Dn@31", "Dn@31", "",
  };
  /* 36 degrees counts (its d is 36.24 degrees off at frame 31); -37 never does, though its d is exactly 3-4-5
   * (cos = 0.8) at frame 50: the limit is strict */
  const gchar *strict[] = {
    "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "Dn@31", "", "", "", "", "Dn@31", "Dn@31", "",
  };

  check_lc_angles (LC_LOOSE, loose_a, G_N_ELEMENTS (loose_a), 8, 30.5, 60, loose);
  check_lc_angles (LC_BALANCED, balanced_a, G_N_ELEMENTS (balanced_a), 8, 30.5, 60, balanced);
  check_lc_angles (LC_STRICT, strict_a, G_N_ELEMENTS (strict_a), 8, 30.5, 60, strict);
}

/* A crossing against the direction never counts: 8 px/frame upwards across the line, at 91 to 180 degrees from
 * the rule's direction */
static void
test_lc_wrong_way (void)
{
  static const gdouble up_a[] = { 91, 95, 100, 120, 150, 180, -95, -120 };
  const gchar *want[] = { "", "", "", "", "", "", "", "" };

  check_lc_angles (LC_LOOSE, up_a, G_N_ELEMENTS (up_a), 8, 30.5, 60, want);
  check_lc_angles (LC_BALANCED, up_a, G_N_ELEMENTS (up_a), 8, 30.5, 60, want);
  check_lc_angles (LC_STRICT, up_a, G_N_ELEMENTS (up_a), 8, 30.5, 60, want);
}

/* Static 2 px above the line, then a jump of J px down at frame 20 */
static const gdouble *lc_jumps;
static guint lc_n_jumps;

static gboolean
lc_jump_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 40 + i * (1200.0 / lc_n_jumps);
  *by = 398 + (f >= 20 ? lc_jumps[i] : 0);
  return TRUE;
}

/* The displacement limits: |d| >= 32 (loose), 55 (balanced), 78 (strict), inclusive */
static void
test_lc_displacement (void)
{
  static const gdouble modes_j[] = { 20, 31, 32, 40, 50, 54, 55, 56, 60, 70, 78, 79, 80, 81, 85, 100 };
  static const gdouble fine_j[] = { 31.5, 31.6, 31.65, 31.7, 31.75, 31.8, 31.9, 32 };
  static const gdouble strict_j[] = { 70, 71, 72, 73, 74, 75, 76, 77, 77.5, 78 };
  const gchar *loose[] = {
    "", "", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20",
    "Dn@20", "Dn@20", "Dn@20",
  };
  const gchar *balanced[] = {
    "", "", "", "", "", "", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20",
  };
  const gchar *strict[] = {
    "", "", "", "", "", "", "", "", "", "", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20", "Dn@20",
  };
  const gchar *fine[] = { "", "", "", "", "", "", "", "Dn@20" };
  const gchar *strict_fine[] = { "", "", "", "", "", "", "", "", "", "Dn@20" };

  lc_jumps = modes_j;
  lc_n_jumps = G_N_ELEMENTS (modes_j);
  check_lc (LC_LOOSE, 40, 20, lc_jump_path, loose, G_N_ELEMENTS (loose));
  check_lc (LC_BALANCED, 40, 20, lc_jump_path, balanced, G_N_ELEMENTS (balanced));
  check_lc (LC_STRICT, 40, 20, lc_jump_path, strict, G_N_ELEMENTS (strict));
  lc_jumps = fine_j;
  lc_n_jumps = G_N_ELEMENTS (fine_j);
  check_lc (LC_LOOSE, 30, 20, lc_jump_path, fine, G_N_ELEMENTS (fine));
  lc_jumps = strict_j;
  lc_n_jumps = G_N_ELEMENTS (strict_j);
  check_lc (LC_STRICT, 30, 20, lc_jump_path, strict_fine, G_N_ELEMENTS (strict_fine));
}

/* Straight down at v px/frame from dist px above the line (plus half a pixel), and one moving up across it */
static const gdouble lc_speeds[][2] = {
  {1, 30}, {1, 60}, {2, 30}, {2, 60}, {5, 30}, {5, 100}, {20, 30}, {20, 100}, {60, 30}, {200, 150}, {0.5, 5}, {3, 3},
};

static gboolean
lc_speed_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  if (i == G_N_ELEMENTS (lc_speeds)) {
    *cx = 1180;
    *by = 500 - 5.0 * f;
    return TRUE;
  }
  *cx = 80 + i * 90;
  *by = 400 - lc_speeds[i][1] + 0.5 + lc_speeds[i][0] * f;
  return TRUE;
}

/* The earliest observation: 3 (loose), 10 (balanced), 20 (strict); an earlier crossing counts then, when the rest
 * holds. 60 px/f from 30 px above is past the line at observation 1, so its reference is too: never counted. */
static void
test_lc_earliest (void)
{
  const gchar *loose[] = {
    "Dn@32", "Dn@60", "Dn@17", "Dn@30", "Dn@7", "Dn@20", "Dn@3", "Dn@5", "", "Dn@3", "", "Dn@11", "",
  };
  const gchar *balanced[] = {
    "", "", "Dn@28", "Dn@31", "Dn@12", "Dn@21", "Dn@10", "Dn@10", "", "Dn@10", "", "Dn@19", "",
  };
  const gchar *strict[] = {
    "", "", "Dn@40", "Dn@40", "Dn@20", "Dn@21", "Dn@20", "Dn@20", "", "Dn@20", "", "Dn@27", "",
  };

  check_lc (LC_LOOSE, 80, 20, lc_speed_path, loose, G_N_ELEMENTS (loose));
  check_lc (LC_BALANCED, 80, 20, lc_speed_path, balanced, G_N_ELEMENTS (balanced));
  check_lc (LC_STRICT, 80, 20, lc_speed_path, strict, G_N_ELEMENTS (strict));
}

/* At y 350, then 450 from frame k (k = 1..5); then four objects at 380 at frame 0 and y1 = 419, 420, 421, 440 at
 * frame 1, moving down 2 px/f */
static gboolean
lc_early_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  static const gdouble y1[] = { 419, 420, 421, 440 };

  *cx = 80 + i * 100;
  if (i < 5)
    *by = f < i + 1 ? 350 : 450;
  else
    *by = f == 0 ? 380 : y1[i - 5] + 2.0 * (f - 1);
  return TRUE;
}

/* Already past the line at observation 1 never counts: the reference, the mean of the first two points, is on the
 * line or past it */
static void
test_lc_past_at_start (void)
{
  const gchar *want[] = {
    "",                                         /* jump at frame 1: reference (350 + 450) / 2 = 400, on the line */
    "Dn@3", "Dn@3", "Dn@4", "Dn@5",             /* jumps at frames 2..5: counted from observation 3 */
    "Dn@7",                                     /* 380, 419: reference 399 */
    "", "", "",                                 /* 380, 420 / 421 / 440: reference 400 or more */
  };

  check_lc (LC_LOOSE, 40, 20, lc_early_path, want, G_N_ELEMENTS (want));
}

/* Down across a segment x 500..800 at 5 px/frame, at bottom-centre x */
static const gdouble *lc_seg_xs;

static gboolean
lc_seg_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = lc_seg_xs[i];
  *by = 300 + 5.0 * f;
  return TRUE;
}

#define LC_SEG(ext) PROPERTY LC_GROUP "line-crossing-S=650;300;650;500;500;400;800;400\nclass-id=-1\nextended=" ext \
  "\nmode=loose\n"

/* The segment test: the bottom centre within half the box width of the segment, strictly (a 20 px box counts from
 * x 491 to 809, not at 490 or 810); extended=1 counts the infinite line, a slanted one too */
static void
test_lc_segment (void)
{
  static const gdouble edge_xs[] = { 480, 485, 488, 489, 490, 491, 492, 495, 505, 795, 805, 808, 809, 810, 811, 812,
    815, 820
  };
  static const gdouble xs[] = { 100, 300, 499, 499.5, 500, 500.5, 650, 799.5, 800, 800.5, 801, 900, 1200 };
  static const gdouble w60_xs[] = { 470, 471, 829, 830 };
  const gchar *w20[] = {
    "", "", "", "", "", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "", "", "", "", "",
  };
  const gchar *w60[] = {
    "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20",
    "S@20", "S@20", "S@20", "S@20",
  };
  const gchar *w60_edges[] = { "", "S@20", "S@20", "" };      /* 30 px: from 471 to 829 */
  const gchar *seg[] = {
    "", "", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "", "",
  };
  const gchar *ext[] = {
    "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20", "S@20",
  };
  /* the line from (500, 380) to (800, 420), extended. Ours: x 650 and 800 reach the line exactly at frames 20 and
   * 24, and loose counts a point on the line; 8.0 counted them one frame later, as a float slope would (x 500, on
   * the line's first end at frame 16, it counted at once) */
  const gchar *slant[] = {
    "S@7", "S@11", "S@16", "S@16", "S@16", "S@16", "S@20", "S@24", "S@24", "S@24", "S@25", "S@27", "S@35",
  };

  lc_seg_xs = edge_xs;
  check_lc (LC_SEG ("0"), 60, 20, lc_seg_path, w20, G_N_ELEMENTS (w20));
  check_lc (LC_SEG ("0"), 60, 60, lc_seg_path, w60, G_N_ELEMENTS (w60));
  lc_seg_xs = w60_xs;
  check_lc (LC_SEG ("0"), 60, 60, lc_seg_path, w60_edges, G_N_ELEMENTS (w60_edges));
  lc_seg_xs = xs;
  check_lc (LC_SEG ("0"), 60, 20, lc_seg_path, seg, G_N_ELEMENTS (seg));
  check_lc (LC_SEG ("1"), 60, 20, lc_seg_path, ext, G_N_ELEMENTS (ext));
  check_lc (PROPERTY LC_GROUP "line-crossing-S=650;300;650;500;500;380;800;420\nclass-id=-1\nextended=1\n"
      "mode=loose\n", 60, 20, lc_seg_path, slant, G_N_ELEMENTS (slant));
}

/* Segment ends on other lines (ours: the crossing point projected on the line, against the segment lengthened by
 * w/2 = 10 px at each end, strictly), crossed perpendicularly, so the ties are exact:
 * - the vertical line x 700 from y 200 to 500, crossed east at 10 px/frame at bottom-centre y 190, 191, 350, 509,
 *   510;
 * - the 3-4-5 slanted line (400, 300) - (700, 700), 500 px long, crossed along its normal at 10 px/frame where the
 *   projection is -10, -5, 250, 505 and 510 px.
 * Each object is on the line at frame 10. */
static const gdouble lc_vert_ys[] = { 190, 191, 350, 509, 510 };
static const gdouble lc_slant_ts[] = { -10, -5, 250, 505, 510 };

static gboolean
lc_vert_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 600 + 10.0 * f;
  *by = lc_vert_ys[i];
  return TRUE;
}

static gboolean
lc_slant_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  /* the point at t on the line, minus 10 frames of (8, -6) */
  *cx = 400 + 0.6 * lc_slant_ts[i] + 8.0 * ((gdouble) f - 10);
  *by = 300 + 0.8 * lc_slant_ts[i] - 6.0 * ((gdouble) f - 10);
  return TRUE;
}

static void
test_lc_segment_ends (void)
{
  const gchar *want[] = { "", "S@10", "S@10", "S@10", "" };

  check_lc (PROPERTY LC_GROUP "line-crossing-S=600;300;800;300;700;200;700;500\nextended=0\n", 30, 20,
      lc_vert_path, want, G_N_ELEMENTS (want));
  check_lc (PROPERTY LC_GROUP "line-crossing-S=550;500;950;200;400;300;700;700\nextended=0\n", 30, 20,
      lc_slant_path, want, G_N_ELEMENTS (want));
}

/* Degenerate rules count nothing and break nothing (ours): a vector along its line, a line of one point, a vector
 * of zero length; the stream's ordinary rule still counts */
static gboolean
lc_down_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 600 + 100.0 * i;
  *by = 300 + 10.0 * f;
  return TRUE;
}

static void
test_lc_degenerate (void)
{
  const gchar *want[] = { "Dn@10", "Dn@10" };
  RdsAnalyticsCore *core;
  RdsAnalyticsFrameResult r;

  for (guint m = 0; m < 2; m++) {
    gchar *cfg = g_strdup_printf (PROPERTY LC_GROUP "line-crossing-Along=500;400;700;400;400;400;800;400\n"
        "line-crossing-Point=640;300;640;500;600;400;600;400\nline-crossing-Zero=640;300;640;300;40;400;1240;400\n"
        LC_RULE_DN "extended=%u\n", m);

    check_lc (cfg, 30, 20, lc_down_path, want, G_N_ELEMENTS (want));
    core = core_for (cfg);
    for (guint f = 0; f < 30; f++) {
      RdsAnalyticsObject o = obj_at (1, 0, 600, 300 + 10 * f);

      rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), &o, 1, &r);
    }
    g_assert_cmpuint (r.n_lc, ==, 4);
    for (guint l = 0; l < 3; l++)
      g_assert_cmpuint (r.lc[l].cumulative, ==, 0);
    g_assert_cmpuint (r.lc[3].cumulative, ==, 1);
    rds_analytics_core_free (core);
    g_free (cfg);
  }
}

/* Two objects with the same id in one frame both past the line count once (ours: each adds its point to the one
 * history, and the history's flag stops the second) */
static void
test_lc_duplicate_ids (void)
{
  RdsAnalyticsCore *core = core_for (LC_LOOSE);
  RdsAnalyticsFrameResult r;
  guint labels = 0;

  for (guint f = 0; f < 30; f++) {
    RdsAnalyticsObject objs[] = { obj_at (1, 0, 300, 300 + 10 * f), obj_at (1, 0, 320, 300 + 10 * f) };

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, 2, &r);
    labels += r.objects[0].n_lc_labels + r.objects[1].n_lc_labels;
    g_assert_cmpuint (r.objects[0].n_lc_labels, ==, f == 10);
    g_assert_cmpuint (r.objects[1].n_lc_labels, ==, 0);
    g_assert_cmpuint (r.lc[0].current, ==, f == 10);
  }
  g_assert_cmpuint (labels, ==, 1);
  g_assert_cmpuint (r.lc[0].cumulative, ==, 1);
  rds_analytics_core_free (core);
}

/* Extreme coordinates, clamped to the int range: a line across the whole int range and points at the clamps. The
 * sides and their difference must not overflow (run under UBSan) */
static void
test_lc_extreme (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY LC_GROUP
      "line-crossing-L=0;0;10;10;0;2147483647;2147483647;0\nextended=0\nmode=loose\n");
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f < 5; f++) {
    RdsAnalyticsObject o = { 1, 0, f < 2 ? -10.0f : 3e9f, f < 2 ? -1e10f : 3e9f, 20, 40 };

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), &o, 1, &r);
    g_assert_cmpuint (r.n_lc, ==, 1);
    g_assert_cmpuint (r.objects[0].n_lc_labels, <=, 1);
  }
  g_assert_cmpuint (r.lc[0].cumulative, <=, 1);
  rds_analytics_core_free (core);
}

/* Behaviours on the line y = 400 (Dn), with its reverse Up and a second line Dn2 at y = 550 */
static gboolean
lc_behav_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 80 + i * 100;
  switch (i) {
    case 0:                    /* down and up between 300 and 500 at 5 px/f, 40-frame legs */
      *by = (f / 40) % 2 == 0 ? 300 + 5.0 * (f % 40) : 500 - 5.0 * (f % 40);
      break;
    case 1:                    /* jitter 398 / 402 from frame 19 to 59, then down */
      *by = f < 19 ? 300 + 5.0 * f : f < 60 ? (f % 2 ? 398 : 402) : 402 + 5.0 * (f - 60);
      break;
    case 2:                    /* lost for frames 18..27 (y 390 -> 440 in the gap) */
      if (f >= 18 && f < 28)
        return FALSE;
      *by = 300 + 5.0 * f;
      break;
    case 3:                    /* lost at frame 18 for good */
    case 4:
      if (f >= 18)
        return FALSE;
      *by = 300 + 5.0 * f;
      break;
    case 5:                    /* stops at 395 for frames 19..29, then crosses */
      *by = 300 + 5.0 * MIN (f, 19) + (f >= 30 ? 5.0 * (f - 30) : 0);
      break;
    case 6:                    /* on the line exactly from frame 1 to 29, then down */
      *by = f < 1 ? 395 : f < 30 ? 400 : 400 + 5.0 * (f - 29);
      break;
    case 7:                    /* down across, then back up once */
      *by = f < 30 ? 300 + 5.0 * f : 450 - 5.0 * (f - 30);
      break;
    default:                   /* 20 px/f, on the line exactly at frame 10 */
      *by = f < 10 ? 200 + 20.0 * f : 400 + 20.0 * (f - 10);
      break;
  }
  return TRUE;
}

/* Counted once per object and line: back and forth, jitter around the line, a gap, a stop before the line, a point
 * exactly on it (loose counts it), a way back that the reverse rule counts once the reference has slid past the
 * line */
static void
test_lc_once (void)
{
  const gchar *want[] = {
    "Dn@20 Up@68", "Dn@20 Dn2@90", "Dn@28 Dn2@50", "", "", "Dn@31 Dn2@61", "Dn@35 Dn2@59", "Dn@20 Up@68",
    "Dn@10 Dn2@18",
  };
  const gchar *want_dn[] = {
    "Dn@20", "Dn@20", "Dn@28", "", "", "Dn@31", "Dn@35", "Dn@20", "Dn@10",
  };

  check_lc (PROPERTY LC_GROUP LC_RULE_DN "line-crossing-Up=640;500;640;300;40;400;1240;400\n"
      "line-crossing-Dn2=640;450;640;650;40;550;1240;550\nclass-id=-1\nextended=0\nmode=loose\n", 120, 20,
      lc_behav_path, want, G_N_ELEMENTS (want));
  check_lc (LC_LOOSE, 120, 20, lc_behav_path, want_dn, G_N_ELEMENTS (want_dn));
}

/* Re-crossing inside the window: down across, back above, down again */
static gboolean
lc_recross_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  if (i == 0) {
    *cx = 300;
    *by = f >= 30 ? 420 + 4.0 * (f - 30) : f < 10 ? 380 + 4.0 * f : f < 20 ? 420 - 4.0 * (f - 10) :
        380 + 4.0 * (f - 20);
  } else {
    *cx = 600;
    *by = f < 20 ? 300 + 8.0 * f : f < 40 ? 460 - 8.0 * (f - 20) : 300 + 8.0 * (f - 40);
  }
  return TRUE;
}

static void
test_lc_recross (void)
{
  const gchar *want[] = { "Dn@9", "Dn@13" };

  check_lc (LC_LOOSE, 80, 20, lc_recross_path, want, G_N_ELEMENTS (want));
}

/* After 60 observations at rest (a full window), crossings count on the frame |d| reaches the mode's displacement:
 * no STOPPED gate. Seven objects at y 380 moving down from frame 60 at v px/frame, then twelve at y 398
 * jumping J px at frame 60. */
static const gdouble lc_wait_speeds[] = { 1, 2, 3, 5, 10, 20, 50 };
static const gdouble lc_wait_jumps[] = { 25, 31, 32, 40, 54, 55, 60, 77, 78, 80, 100, 150 };

static gboolean
lc_wait_path (guint i, guint f, gdouble * cx, gdouble * by, gdouble * h G_GNUC_UNUSED)
{
  *cx = 60 + i * 61;
  if (i < G_N_ELEMENTS (lc_wait_speeds))
    *by = 380 + lc_wait_speeds[i] * MAX (0, (gint) f - 60);
  else
    *by = 398 + (f >= 60 ? lc_wait_jumps[i - G_N_ELEMENTS (lc_wait_speeds)] : 0);
  return TRUE;
}

static void
test_lc_after_wait (void)
{
  const gchar *loose[] = {
    "Dn@92", "Dn@76", "Dn@71", "Dn@67", "Dn@64", "Dn@62", "Dn@61",
    "", "", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60",
  };
  const gchar *balanced[] = {
    "", "Dn@88", "Dn@79", "Dn@71", "Dn@66", "Dn@63", "Dn@62",
    "", "", "", "", "", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60", "Dn@60",
  };
  const gchar *strict[] = {
    "", "Dn@99", "Dn@86", "Dn@76", "Dn@68", "Dn@64", "Dn@62",
    "", "", "", "", "", "", "", "", "Dn@60", "Dn@60", "Dn@60", "Dn@60",
  };

  check_lc (LC_LOOSE, 140, 20, lc_wait_path, loose, G_N_ELEMENTS (loose));
  check_lc (LC_BALANCED, 140, 20, lc_wait_path, balanced, G_N_ELEMENTS (balanced));
  check_lc (LC_STRICT, 140, 20, lc_wait_path, strict, G_N_ELEMENTS (strict));
  /* with a direction group too, where the objects are STOPPED from observation 50 */
  check_lc (LC_LOOSE "[direction-detection-stream-0]\nenable=1\ndirection-S=400;360;400;560\n", 140, 20,
      lc_wait_path, loose, G_N_ELEMENTS (loose));
}

/* Untracked objects are skipped (ours) and leave the tracked ones counting; the group's class-id filter applies
 * to every line; the counts are per line, in config order, and per stream, cumulative over the objects. Everything
 * moves down 10 px/frame from y 300: on Dn (y 400, loose counts a point on the line) at frame 10, on Far (y 600) at
 * frame 30. Objects: an untracked class 0, id 1 class 0, id 2 class 1, id 3 class 2, then ids 10..13 of class 0
 * starting at frames 0, 5, 10, 15. */
static guint
lc_counts_frame (guint f, RdsAnalyticsObject * objs)
{
  guint n = 0;

  objs[n++] = obj_at (RDS_ANALYTICS_UNTRACKED, 0, 100, 300 + 10 * f);
  objs[n++] = obj_at (1, 0, 200, 300 + 10 * f);
  objs[n++] = obj_at (2, 1, 300, 300 + 10 * f);
  objs[n++] = obj_at (3, 2, 400, 300 + 10 * f);
  for (guint k = 0; k < 4; k++)
    if (f >= 5 * k)
      objs[n++] = obj_at (10 + k, 0, 500 + 100 * k, 300 + 10 * (f - 5 * k));
  return n;
}

static void
test_lc_counts (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY LC_GROUP LC_RULE_DN
      "line-crossing-Far=640;300;640;500;40;600;1240;600\nclass-id=0;2\n"
      "[line-crossing-stream-1]\nenable=1\n" LC_RULE_DN);
  RdsAnalyticsFrameResult r;
  guint64 cum_dn = 0, cum_far = 0;

  for (guint f = 0; f < 40; f++) {
    RdsAnalyticsObject objs[8];
    guint n = lc_counts_frame (f, objs);
    /* the object indices counted on this frame, per line */
    const gchar *dn = f == 10 ? "1,3,4" : f == 15 ? "5" : f == 20 ? "6" : f == 25 ? "7" : "";
    const gchar *far = f == 30 ? "1,3,4" : f == 35 ? "5" : "";
    GString *got_dn = g_string_new (NULL), *got_far = g_string_new (NULL);

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, n, &r);
    g_assert_cmpint (r.untracked_warning, ==, f == 0);
    g_assert_cmpuint (r.n_lc, ==, 2);
    g_assert_cmpstr (r.lc[0].label, ==, "Dn");
    g_assert_cmpstr (r.lc[1].label, ==, "Far");
    for (guint k = 0; k < n; k++) {
      const RdsAnalyticsObjectResult *o = &r.objects[k];

      for (guint j = 0; j < o->n_lc_labels; j++) {
        GString *g = g_str_equal (o->lc_labels[j], "Dn") ? got_dn : got_far;

        g_string_append_printf (g, "%s%u", g->len ? "," : "", k);
      }
      g_assert_cmpint (o->has_info, ==, o->n_lc_labels > 0);
    }
    g_assert_cmpstr (got_dn->str, ==, dn);
    g_assert_cmpstr (got_far->str, ==, far);
    g_string_free (got_dn, TRUE);
    g_string_free (got_far, TRUE);
    if (f == 10)
      g_assert_cmpstr (r.objects[1].obj_status, ==, " LC:Dn ");
    cum_dn += f == 10 ? 3 : (f == 15 || f == 20 || f == 25) ? 1 : 0;
    cum_far += f == 30 ? 3 : f == 35 ? 1 : 0;
    g_assert_cmpuint (r.lc[0].current, ==, f == 10 ? 3 : (f == 15 || f == 20 || f == 25) ? 1 : 0);
    g_assert_cmpuint (r.lc[0].cumulative, ==, cum_dn);
    g_assert_cmpuint (r.lc[1].current, ==, f == 30 ? 3 : f == 35 ? 1 : 0);
    g_assert_cmpuint (r.lc[1].cumulative, ==, cum_far);

    /* stream 1, no class filter: the same objects until frame 20, then only the first two; its own histories and
     * counts (ids 1, 2, 3 and 10 at frame 10, 11 at 15; 12 is gone before it reaches the line) */
    rds_analytics_core_process_frame (core, 1, FRAME_30FPS (f), objs, f < 20 ? n : 2, &r);
    g_assert_cmpint (r.untracked_warning, ==, f == 0);
    g_assert_cmpuint (r.n_lc, ==, 1);
    g_assert_cmpuint (r.lc[0].current, ==, f == 10 ? 4 : f == 15 ? 1 : 0);
    g_assert_cmpuint (r.lc[0].cumulative, ==, f < 10 ? 0 : f < 15 ? 4 : 5);
  }
  g_assert_cmpuint (cum_dn, ==, 6);
  g_assert_cmpuint (cum_far, ==, 4);
  rds_analytics_core_free (core);
}

/* A stream with only other groups has no line counts; a line-crossing group with enable=0 none either */
static void
test_lc_no_group (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=1\n"
      "roi-RF=0;0;1280;0;1280;720;0;720\n" "[line-crossing-stream-1]\nenable=0\n" LC_RULE_DN);
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f < 20; f++) {
    RdsAnalyticsObject o = obj_at (1, 0, 200, 300 + 10 * f);

    for (guint pad = 0; pad < 3; pad++) {
      rds_analytics_core_process_frame (core, pad, FRAME_30FPS (f), &o, 1, &r);
      g_assert_cmpuint (r.n_lc, ==, 0);
      g_assert_cmpuint (r.objects[0].n_lc_labels, ==, 0);
      g_assert_cmpstr (r.objects[0].obj_status, ==, pad == 0 ? " ROI:RF" : "");
    }
  }
  rds_analytics_core_free (core);
}

/* objStatus order: ROI, DIR, LC, STOPPED (DeepStream 8.0 behaviour) */
static void
test_lc_obj_status (void)
{
  RdsAnalyticsCore *core = core_for (PROPERTY "[roi-filtering-stream-0]\nenable=1\n"
      "roi-RF=0;0;1280;0;1280;720;0;720\n" LC_GROUP LC_RULE_DN "line-crossing-E=600;300;800;300;700;100;700;600\n"
      "[direction-detection-stream-0]\nenable=1\ndirection-S=400;360;400;560\n");
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f < 70; f++) {
    /* down 10 px/f, on Dn at frame 10; static 10 px before E (x 700) for 60 frames, then 40 px east across it: too
     * small a step to leave STOPPED */
    RdsAnalyticsObject objs[] = { obj_at (1, 0, 200, 300 + 10 * f), obj_at (2, 0, f < 60 ? 690 : 730, 400) };

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, 2, &r);
    if (f == 10)
      g_assert_cmpstr (r.objects[0].obj_status, ==, " ROI:RF DIR:S  LC:Dn ");
    if (f == 11)
      g_assert_cmpstr (r.objects[0].obj_status, ==, " ROI:RF DIR:S ");
    if (f == 60)
      g_assert_cmpstr (r.objects[1].obj_status, ==, " ROI:RF LC:E  STOPPED ");
    if (f == 61)
      g_assert_cmpstr (r.objects[1].obj_status, ==, " ROI:RF STOPPED ");
  }
  rds_analytics_core_free (core);
}

/* ------------------------------------------------------------------------------------------------------------ */
/* The stream state life cycle: resets and eviction (ours), on line LC_LOOSE's Dn (y 400, counting down) */

#define SECONDS(s) ((guint64) (s) * G_GUINT64_CONSTANT (1000000000))

/* One frame of stream @pad_index with id @id at (640, @y) when @y >= 0, nothing otherwise; returns the objects
 * counted on Dn on this frame */
static guint
lc_step (RdsAnalyticsCore * core, guint pad_index, guint64 pts, guint64 id, gint class_id, gint y,
    RdsAnalyticsFrameResult * r)
{
  RdsAnalyticsObject o = obj_at (id, class_id, 640, y);

  rds_analytics_core_process_frame (core, pad_index, pts, &o, y >= 0 ? 1 : 0, r);
  g_assert_cmpuint (r->n_lc, ==, 1);
  return (guint) r->lc[0].current;
}

/* A reset clears the histories and their "already counted" flags, the untracked warning and objCnt, and keeps the
 * cumulative counts (ours); it leaves the other streams alone, and an unknown pad is a no-op */
static void
test_life_reset (void)
{
  RdsAnalyticsCore *core = core_for (LC_LOOSE "[line-crossing-stream-1]\nenable=1\n" LC_RULE_DN);
  RdsAnalyticsFrameResult r;
  guint f = 0;

  rds_analytics_core_reset_stream (core, 7);
  /* id 1 (class 5) and an untracked object down across Dn: counted at frame 10 on both streams */
  for (; f < 15; f++) {
    RdsAnalyticsObject objs[2] = { obj_at (1, 5, 640, 300 + 10 * f), obj_at (RDS_ANALYTICS_UNTRACKED, 5, 300, 100) };

    for (guint s = 0; s < 2; s++) {
      rds_analytics_core_process_frame (core, s, FRAME_30FPS (f), objs, 2, &r);
      g_assert_cmpuint (r.lc[0].current, ==, f == 10);
      g_assert_cmpint (r.untracked_warning, ==, f == 0);
      g_assert_cmpint (obj_cnt (&r, 5), ==, 2);
    }
  }
  rds_analytics_core_reset_stream (core, 0);
  rds_analytics_core_reset_stream (core, 7);
  /* id 1 again from y 300, now class 0: stream 0 counts it again 10 frames on, stream 1 still has it counted */
  for (guint k = 0; k < 15; k++, f++) {
    RdsAnalyticsObject objs[2] = { obj_at (1, 0, 640, 300 + 10 * k), obj_at (RDS_ANALYTICS_UNTRACKED, 0, 300, 100) };

    rds_analytics_core_process_frame (core, 0, FRAME_30FPS (f), objs, 2, &r);
    g_assert_cmpuint (r.lc[0].current, ==, k == 10);
    g_assert_cmpuint (r.lc[0].cumulative, ==, k < 10 ? 1 : 2);
    /* the warning is given again, and objCnt starts fresh: class 5 is gone */
    g_assert_cmpint (r.untracked_warning, ==, k == 0);
    g_assert_cmpint (obj_cnt (&r, 5), ==, -1);
    g_assert_cmpint (obj_cnt (&r, 0), ==, 2);
    g_assert_cmpuint (r.n_obj_cnt, ==, 1);

    rds_analytics_core_process_frame (core, 1, FRAME_30FPS (f), objs, 2, &r);
    g_assert_cmpuint (r.lc[0].current, ==, 0);
    g_assert_cmpuint (r.lc[0].cumulative, ==, 1);
    g_assert_false (r.untracked_warning);
    /* class 5 is still seen there: 0 (on the tie at k = 0, the newest value) */
    g_assert_cmpint (obj_cnt (&r, 5), ==, 0);
  }
  rds_analytics_core_free (core);
}

/* id 1 down across Dn from y 300, counted at frame 10, seen until frame 12, then absent until frame @back, at
 * @pts (frame) each; then down again from y 300 for 20 frames. Returns the frame of its second count, or 0. */
static guint
evict_run (guint back, guint64 (*pts) (guint))
{
  RdsAnalyticsCore *core = core_for (LC_LOOSE);
  RdsAnalyticsFrameResult r;
  guint second = 0;

  for (guint f = 0; f < back + 20; f++) {
    gint y = f <= 12 ? 300 + 10 * (gint) f : f >= back ? 300 + 10 * (gint) (f - back) : -1;
    guint n = lc_step (core, 0, pts (f), 1, 0, y, &r);

    if (f < back)
      g_assert_cmpuint (n, ==, f == 10);
    else if (n > 0) {
      g_assert_cmpuint (second, ==, 0);
      second = f;
    }
    g_assert_cmpuint (r.lc[0].cumulative, ==, f < 10 ? 0 : second ? 2 : 1);
  }
  rds_analytics_core_free (core);
  return second;
}

static guint64
pts_1fps (guint f)
{
  return SECONDS (f);
}

static guint64
pts_30fps (guint f)
{
  return FRAME_30FPS (f);
}

static guint64
pts_none (guint f G_GNUC_UNUSED)
{
  return RDS_ANALYTICS_PTS_NONE;
}

/* A history unseen for more than 30 s of PTS is dropped (ours): its id comes back fresh and counts again; one
 * seen 30 s ago is kept, and keeps its "already counted" flag */
static void
test_life_evict (void)
{
  /* 1 fps: last seen at 12 s, back at 42 s (30 s unseen: kept) or 43 s (31 s: dropped) */
  g_assert_cmpuint (evict_run (42, pts_1fps), ==, 0);
  g_assert_cmpuint (evict_run (43, pts_1fps), ==, 53);
  /* 30 fps, swept at most once a second: last seen at 0.4 s; back at 30.4 s kept, at 31.5 s dropped */
  g_assert_cmpuint (evict_run (12 + 900, pts_30fps), ==, 0);
  g_assert_cmpuint (evict_run (12 + 933, pts_30fps), ==, 12 + 933 + 10);
}

/* Without PTS, a history unseen for more than 900 frames is dropped (ours), swept every 30 frames */
static void
test_life_evict_no_pts (void)
{
  g_assert_cmpuint (evict_run (12 + 900, pts_none), ==, 0);
  g_assert_cmpuint (evict_run (12 + 931, pts_none), ==, 12 + 931 + 10);
}

/* A sweep keeps the ids seen within 30 s: at 1 fps, id 2 seen every frame, id 1 every 20 s */
static void
test_life_evict_keeps_seen (void)
{
  RdsAnalyticsCore *core = core_for (LC_LOOSE);
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f <= 200; f++) {
    /* id 2 down across Dn at frame 10; id 1 down 10 px per sighting, across on its 11th, at frame 200 */
    RdsAnalyticsObject objs[2] = { obj_at (2, 0, 200, 300 + 10 * MIN (f, 20)),
      obj_at (1, 0, 640, 300 + 10 * (f / 20))
    };

    rds_analytics_core_process_frame (core, 0, SECONDS (f), objs, f % 20 == 0 ? 2 : 1, &r);
    g_assert_cmpuint (r.lc[0].current, ==, (f == 10) + (f == 200));
  }
  g_assert_cmpuint (r.lc[0].cumulative, ==, 2);
  rds_analytics_core_free (core);
}

/* DeepStream 8.0 behaviour: a history is kept across a 30-frame absence. id 1 at y 300, 310, 320 (frames 0-2),
 * absent 30 frames, back at y 420: its 4th observation, ref 305, so it counts at once */
static void
test_life_absence (void)
{
  RdsAnalyticsCore *core = core_for (LC_LOOSE);
  RdsAnalyticsFrameResult r;

  for (guint f = 0; f < 34; f++) {
    gint y = f < 3 ? 300 + 10 * (gint) f : f == 33 ? 420 : -1;

    g_assert_cmpuint (lc_step (core, 0, FRAME_30FPS (f), 1, 0, y, &r), ==, f == 33);
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
  g_test_add_func ("/cpu/analytics-core/direction/box-height-inclusive", test_dir_box_height_inclusive);
  g_test_add_func ("/cpu/analytics-core/direction/clamp-x", test_dir_clamp_x);
  g_test_add_func ("/cpu/analytics-core/direction/class-filter", test_dir_class_filter);
  g_test_add_func ("/cpu/analytics-core/direction/angle", test_dir_angle);
  g_test_add_func ("/cpu/analytics-core/direction/multi", test_dir_multi);
  g_test_add_func ("/cpu/analytics-core/direction/obj-status", test_dir_obj_status);
  g_test_add_func ("/cpu/analytics-core/direction/needs-group", test_dir_needs_group);
  g_test_add_func ("/cpu/analytics-core/direction/untracked", test_dir_untracked);
  g_test_add_func ("/cpu/analytics-core/line-crossing/angle-sweep", test_lc_angle_sweep);
  g_test_add_func ("/cpu/analytics-core/line-crossing/angle-limits", test_lc_angle_limits);
  g_test_add_func ("/cpu/analytics-core/line-crossing/wrong-way", test_lc_wrong_way);
  g_test_add_func ("/cpu/analytics-core/line-crossing/displacement", test_lc_displacement);
  g_test_add_func ("/cpu/analytics-core/line-crossing/earliest", test_lc_earliest);
  g_test_add_func ("/cpu/analytics-core/line-crossing/past-at-start", test_lc_past_at_start);
  g_test_add_func ("/cpu/analytics-core/line-crossing/segment", test_lc_segment);
  g_test_add_func ("/cpu/analytics-core/line-crossing/once", test_lc_once);
  g_test_add_func ("/cpu/analytics-core/line-crossing/recross", test_lc_recross);
  g_test_add_func ("/cpu/analytics-core/line-crossing/after-wait", test_lc_after_wait);
  g_test_add_func ("/cpu/analytics-core/line-crossing/counts", test_lc_counts);
  g_test_add_func ("/cpu/analytics-core/line-crossing/no-group", test_lc_no_group);
  g_test_add_func ("/cpu/analytics-core/line-crossing/obj-status", test_lc_obj_status);
  g_test_add_func ("/cpu/analytics-core/line-crossing/segment-ends", test_lc_segment_ends);
  g_test_add_func ("/cpu/analytics-core/line-crossing/degenerate", test_lc_degenerate);
  g_test_add_func ("/cpu/analytics-core/line-crossing/duplicate-ids", test_lc_duplicate_ids);
  g_test_add_func ("/cpu/analytics-core/line-crossing/extreme", test_lc_extreme);
  g_test_add_func ("/cpu/analytics-core/life-cycle/reset", test_life_reset);
  g_test_add_func ("/cpu/analytics-core/life-cycle/evict", test_life_evict);
  g_test_add_func ("/cpu/analytics-core/life-cycle/evict-no-pts", test_life_evict_no_pts);
  g_test_add_func ("/cpu/analytics-core/life-cycle/evict-keeps-seen", test_life_evict_keeps_seen);
  g_test_add_func ("/cpu/analytics-core/life-cycle/absence", test_life_absence);
  return g_test_run ();
}
