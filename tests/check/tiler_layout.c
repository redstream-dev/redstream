/* rdsmultistreamtiler's layout core: the grid for n sources (configured, √n growth, square-seq-grid), output and
 * cell rounding, cell rects, and the scaling of object and display meta into a cell (floats vs truncated integers,
 * fields left alone). All DeepStream 8.0 behaviour.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tiler-layout.h"

#include <string.h>

#define CLOSE(a, b) (ABS ((gdouble) (a) - (gdouble) (b)) < 1e-3)

static void
assert_grid (guint rows, guint columns, gboolean square, guint n, guint want_rows, guint want_columns)
{
  guint r = 0, c = 0;

  rds_tiler_grid (rows, columns, square, n, &r, &c);
  if (r != want_rows || c != want_columns)
    g_error ("grid %ux%u square %d, n %u: got %ux%u, want %ux%u", rows, columns, square, n, r, c, want_rows,
        want_columns);
}

static void
test_grid_configured (void)
{
  /* n fits: the configured grid, unused cells and all */
  assert_grid (2, 2, FALSE, 1, 2, 2);
  assert_grid (2, 2, FALSE, 4, 2, 2);
  assert_grid (3, 2, FALSE, 5, 3, 2);
  assert_grid (1, 1, FALSE, 1, 1, 1);
  assert_grid (1, 1, FALSE, 0, 1, 1);
  assert_grid (2, 3, FALSE, 6, 2, 3);
}

static void
test_grid_growth (void)
{
  /* n > rows x columns: floor (sqrt n) rows, ceil (n / rows) columns */
  assert_grid (1, 1, FALSE, 2, 1, 2);
  assert_grid (1, 1, FALSE, 3, 1, 3);
  assert_grid (1, 1, FALSE, 4, 2, 2);
  assert_grid (1, 1, FALSE, 5, 2, 3);
  assert_grid (1, 1, FALSE, 6, 2, 3);
  assert_grid (1, 1, FALSE, 7, 2, 4);
  assert_grid (2, 2, FALSE, 5, 2, 3);
  assert_grid (2, 2, FALSE, 9, 3, 3);
  assert_grid (2, 2, FALSE, 10, 3, 4);
  assert_grid (1, 1, FALSE, 16, 4, 4);
  assert_grid (1, 1, FALSE, 17, 4, 5);
  /* a non-square configured grid that holds n stays */
  assert_grid (1, 4, FALSE, 3, 1, 4);
}

static void
test_grid_square (void)
{
  /* ceil (sqrt n) both ways, rows and columns ignored */
  assert_grid (1, 1, TRUE, 1, 1, 1);
  assert_grid (4, 1, TRUE, 2, 2, 2);
  assert_grid (1, 1, TRUE, 3, 2, 2);
  assert_grid (1, 1, TRUE, 4, 2, 2);
  assert_grid (1, 1, TRUE, 5, 3, 3);
  assert_grid (5, 5, TRUE, 7, 3, 3);
  assert_grid (1, 1, TRUE, 10, 4, 4);
  assert_grid (1, 1, TRUE, 0, 1, 1);
}

static void
test_round_output (void)
{
  g_assert_cmpuint (rds_tiler_round_output_size (1001), ==, 1004);
  g_assert_cmpuint (rds_tiler_round_output_size (701), ==, 704);
  g_assert_cmpuint (rds_tiler_round_output_size (1280), ==, 1280);
  g_assert_cmpuint (rds_tiler_round_output_size (16), ==, 16);
  g_assert_cmpuint (rds_tiler_round_output_size (17), ==, 20);
  g_assert_cmpuint (rds_tiler_round_output_size (G_MAXUINT), ==, G_MAXUINT & ~3u);
}

static void
test_cells (void)
{
  RdsTilerLayout l;
  RdsTilerRect r;

  /* 1280 / 3 = 426 (even); 720 / 2 = 360 */
  g_assert_true (rds_tiler_layout_init (&l, 2, 3, FALSE, 6, 1280, 720));
  g_assert_cmpuint (l.rows, ==, 2);
  g_assert_cmpuint (l.columns, ==, 3);
  g_assert_cmpuint (l.width, ==, 1280);
  g_assert_cmpuint (l.height, ==, 720);
  g_assert_cmpuint (l.cell_width, ==, 426);
  g_assert_cmpuint (l.cell_height, ==, 360);
  g_assert_true (rds_tiler_layout_cell (&l, 0, &r));
  g_assert_cmpuint (r.left, ==, 0);
  g_assert_cmpuint (r.top, ==, 0);
  g_assert_cmpuint (r.width, ==, 426);
  g_assert_cmpuint (r.height, ==, 360);
  g_assert_true (rds_tiler_layout_cell (&l, 5, &r));
  g_assert_cmpuint (r.left, ==, 852);
  g_assert_cmpuint (r.top, ==, 360);
  g_assert_false (rds_tiler_layout_cell (&l, 6, &r));

  /* 1001 x 701 -> 1004 x 704 on 2 x 3: 334 x 352 cells */
  g_assert_true (rds_tiler_layout_init (&l, 2, 3, FALSE, 1, 1001, 701));
  g_assert_cmpuint (l.width, ==, 1004);
  g_assert_cmpuint (l.height, ==, 704);
  g_assert_cmpuint (l.cell_width, ==, 334);
  g_assert_cmpuint (l.cell_height, ==, 352);

  /* 1000 x 700 on 3 x 3: 332 x 232, odd quotients rounded down to even (333 -> 332, 233 -> 232) */
  g_assert_true (rds_tiler_layout_init (&l, 3, 3, FALSE, 9, 1000, 700));
  g_assert_cmpuint (l.cell_width, ==, 332);
  g_assert_cmpuint (l.cell_height, ==, 232);
  g_assert_true (rds_tiler_layout_cell (&l, 4, &r));
  g_assert_cmpuint (r.left, ==, 332);
  g_assert_cmpuint (r.top, ==, 232);

  /* growth applies in the layout too */
  g_assert_true (rds_tiler_layout_init (&l, 1, 1, FALSE, 7, 1920, 1080));
  g_assert_cmpuint (l.rows, ==, 2);
  g_assert_cmpuint (l.columns, ==, 4);
  g_assert_cmpuint (l.cell_width, ==, 480);
  g_assert_cmpuint (l.cell_height, ==, 540);
}

static void
test_cells_too_small (void)
{
  RdsTilerLayout l;

  memset (&l, 0x5a, sizeof l);
  /* 16 / 11 = 1 -> 0 after even rounding: no cells */
  g_assert_false (rds_tiler_layout_init (&l, 1, 1, FALSE, 101, 16, 16));
  g_assert_cmpuint (l.rows, ==, 0x5a5a5a5a);
  /* 16 / 8 = 2 still works */
  g_assert_true (rds_tiler_layout_init (&l, 1, 8, FALSE, 8, 16, 16));
  g_assert_cmpuint (l.cell_width, ==, 2);
  /* a huge pad index */
  g_assert_false (rds_tiler_layout_init (&l, 1, 1, FALSE, G_MAXUINT, 1920, 1080));
}

static RdsObjectMeta
make_object (void)
{
  RdsObjectMeta o;

  memset (&o, 0, sizeof o);
  o.rect_params.left = 100;
  o.rect_params.top = 100;
  o.rect_params.width = 200;
  o.rect_params.height = 100;
  o.rect_params.border_width = 4;
  o.text_params.x_offset = 100;
  o.text_params.y_offset = 80;
  o.text_params.font_params.font_size = 12;
  o.detector_bbox_info.org_bbox_coords = (RdsBbox_Coords) {101, 102, 203, 104};
  o.tracker_bbox_info.org_bbox_coords = (RdsBbox_Coords) {105, 106, 207, 108};
  o.mask_params.width = 20;
  o.mask_params.height = 10;
  o.object_id = 101;
  o.class_id = 3;
  o.confidence = 0.6f;
  o.unique_component_id = 1;
  g_strlcpy (o.obj_label, "p1", sizeof o.obj_label);
  return o;
}

static void
test_scale_object (void)
{
  RdsTilerRect cell = { 426, 0, 426, 360 };     /* cell 1 of 2 x 3 on 1280 x 720 */
  RdsTilerScale s;
  RdsObjectMeta o = make_object (), before = o;

  rds_tiler_scale_init (&s, 1280, 720, &cell);
  rds_tiler_scale_object (&s, &o);
  /* floats: 426 + 100 * 426 / 1280 = 459.28125 */
  g_assert_true (CLOSE (o.rect_params.left, 459.28125));
  g_assert_true (CLOSE (o.rect_params.top, 50.0));
  g_assert_true (CLOSE (o.rect_params.width, 66.5625));
  g_assert_true (CLOSE (o.rect_params.height, 50.0));
  /* integers truncated: 426 + 33.28 -> 459; 80 * 0.5 = 40 */
  g_assert_cmpuint (o.text_params.x_offset, ==, 459);
  g_assert_cmpuint (o.text_params.y_offset, ==, 40);

  /* everything else as it was */
  g_assert_cmpuint (o.rect_params.border_width, ==, 4);
  g_assert_cmpuint (o.text_params.font_params.font_size, ==, 12);
  g_assert_cmpmem (&o.detector_bbox_info, sizeof o.detector_bbox_info, &before.detector_bbox_info,
      sizeof before.detector_bbox_info);
  g_assert_cmpmem (&o.tracker_bbox_info, sizeof o.tracker_bbox_info, &before.tracker_bbox_info,
      sizeof before.tracker_bbox_info);
  g_assert_cmpmem (&o.mask_params, sizeof o.mask_params, &before.mask_params, sizeof before.mask_params);
  g_assert_cmpuint (o.object_id, ==, 101);
  g_assert_cmpint (o.class_id, ==, 3);
  g_assert_cmpfloat (o.confidence, ==, 0.6f);
  g_assert_cmpint (o.unique_component_id, ==, 1);
  g_assert_cmpstr (o.obj_label, ==, "p1");
}

static void
test_scale_full_output (void)
{
  /* show-source on a 640 x 640 output: [300,300,60,50] -> [150,266.667,30,44.444] */
  RdsTilerRect all = { 0, 0, 640, 640 };
  RdsTilerScale s;
  RdsObjectMeta o = make_object ();

  o.rect_params.left = 300;
  o.rect_params.top = 300;
  o.rect_params.width = 60;
  o.rect_params.height = 50;
  rds_tiler_scale_init (&s, 1280, 720, &all);
  rds_tiler_scale_object (&s, &o);
  g_assert_true (CLOSE (o.rect_params.left, 150));
  g_assert_true (CLOSE (o.rect_params.top, 266.6667));
  g_assert_true (CLOSE (o.rect_params.width, 30));
  g_assert_true (CLOSE (o.rect_params.height, 44.4444));
  /* 80 * 640 / 720 = 71.1 -> 71 */
  g_assert_cmpuint (o.text_params.y_offset, ==, 71);
}

static void
test_scale_display (void)
{
  RdsTilerRect cell = { 640, 360, 640, 360 };   /* cell 3 of 2 x 2 */
  RdsTilerScale s;
  RdsDisplayMeta d;

  memset (&d, 0, sizeof d);
  d.num_rects = 1;
  d.rect_params[0] = (RdsOSD_RectParams) {.left = 640,.top = 360,.width = 100,.height = 100,.border_width = 3 };
  d.rect_params[1].left = 7;    /* past num_rects: untouched */
  d.num_labels = 1;
  d.text_params[0].x_offset = 11;
  d.text_params[0].y_offset = 11;
  d.text_params[0].font_params.font_size = 20;
  d.num_lines = 1;
  d.line_params[0] = (RdsOSD_LineParams) {.x1 = 0,.y1 = 0,.x2 = 1279,.y2 = 719,.line_width = 4 };
  d.num_arrows = 1;
  d.arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 10,.y1 = 700,.x2 = 201,.y2 = 700,.arrow_width = 4 };
  d.num_circles = 1;
  d.circle_params[0] = (RdsOSD_CircleParams) {.xc = 640,.yc = 360,.radius = 51 };
  /* num_* past the array size: only the first 16 are read */
  d.num_lines = 40;
  for (guint i = 1; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
    d.line_params[i] = d.line_params[0];

  /* a 1280 x 640 frame: sx 0.5, sy 0.5625 */
  rds_tiler_scale_init (&s, 1280, 640, &cell);
  rds_tiler_scale_display (&s, &d);

  g_assert_true (CLOSE (d.rect_params[0].left, 960));
  g_assert_true (CLOSE (d.rect_params[0].top, 562.5));
  g_assert_true (CLOSE (d.rect_params[0].width, 50));
  g_assert_true (CLOSE (d.rect_params[0].height, 56.25));
  g_assert_cmpuint (d.rect_params[0].border_width, ==, 3);
  g_assert_true (CLOSE (d.rect_params[1].left, 7));
  /* 640 + 5.5 -> 645; 360 + 6.1875 -> 366 */
  g_assert_cmpuint (d.text_params[0].x_offset, ==, 645);
  g_assert_cmpuint (d.text_params[0].y_offset, ==, 366);
  g_assert_cmpuint (d.text_params[0].font_params.font_size, ==, 20);
  for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++) {
    g_assert_cmpuint (d.line_params[i].x1, ==, 640);
    g_assert_cmpuint (d.line_params[i].y1, ==, 360);
    /* 640 + 639.5 -> 1279; 360 + 404.4375 -> 764 */
    g_assert_cmpuint (d.line_params[i].x2, ==, 1279);
    g_assert_cmpuint (d.line_params[i].y2, ==, 764);
    g_assert_cmpuint (d.line_params[i].line_width, ==, 4);
  }
  /* 640 + 5 = 645; 360 + 393.75 -> 753; 640 + 100.5 -> 740 */
  g_assert_cmpuint (d.arrow_params[0].x1, ==, 645);
  g_assert_cmpuint (d.arrow_params[0].y1, ==, 753);
  g_assert_cmpuint (d.arrow_params[0].x2, ==, 740);
  g_assert_cmpuint (d.arrow_params[0].arrow_width, ==, 4);
  /* centre 960, 562 (562.5 truncated); radius 51 * min (0.5, 0.5625) = 25.5 -> 25 */
  g_assert_cmpuint (d.circle_params[0].xc, ==, 960);
  g_assert_cmpuint (d.circle_params[0].yc, ==, 562);
  g_assert_cmpuint (d.circle_params[0].radius, ==, 25);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/tiler/grid/configured", test_grid_configured);
  g_test_add_func ("/cpu/tiler/grid/growth", test_grid_growth);
  g_test_add_func ("/cpu/tiler/grid/square", test_grid_square);
  g_test_add_func ("/cpu/tiler/round-output", test_round_output);
  g_test_add_func ("/cpu/tiler/cells", test_cells);
  g_test_add_func ("/cpu/tiler/cells-too-small", test_cells_too_small);
  g_test_add_func ("/cpu/tiler/scale/object", test_scale_object);
  g_test_add_func ("/cpu/tiler/scale/full-output", test_scale_full_output);
  g_test_add_func ("/cpu/tiler/scale/display", test_scale_display);
  return g_test_run ();
}
