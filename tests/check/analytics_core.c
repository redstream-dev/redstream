/* rdsanalytics core: rule scaling, the reference point and the inside test, against DeepStream 8.0's observed
 * results (the observed boxes below: left, top, width, height in frame pixels, and the ROIs 8.0 reported them in).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-core.h"

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
  return g_test_run ();
}
