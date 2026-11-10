/* rdsanalytics core: rule scaling, the reference point and the inside test, against DeepStream 8.0's observed
 * results (the observed boxes below: left, top, width, height in frame pixels, and the ROIs 8.0 reported them in);
 * ROI filtering, overcrowding and the object counts on scripted frames.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-core.h"

#include <glib/gstdio.h>
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
  return g_test_run ();
}
