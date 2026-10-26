/* rdsosd's shared shapes (rds-osd-shapes.h): exact pixel sets of every command type built from drawing parameters
 * (boxes, rectangle bands, segments, arrow heads with their bevel join, rings and fills, masks), the blend, the order
 * of overlapping shapes, shapes blended once, and that drawing each shape over its bounding box (the CPU path) gives
 * the same bytes as shading every pixel through the whole command list (the GPU path). No GStreamer, no GPU.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-shapes.h"

#include <glib.h>
#include <math.h>
#include <string.h>

#define GREY 128
#define ROW_PAD 8
#define PAD_BYTE 0xEE

typedef struct
{
  guint8 *data;
  gint width, height;
  gsize stride;
} Img;

static RdsOsdColor RED, RED_HALF, GREEN_HALF;

static void
img_init (Img * im, gint w, gint h)
{
  im->width = w;
  im->height = h;
  im->stride = (gsize) w * 4 + ROW_PAD;
  im->data = g_malloc (im->stride * h);
  memset (im->data, PAD_BYTE, im->stride * h);
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++) {
      guint8 *p = im->data + y * im->stride + 4 * x;

      p[0] = p[1] = p[2] = GREY;
      p[3] = 255;
    }
}

static void
img_clear (Img * im)
{
  for (gint y = 0; y < im->height; y++)
    for (gint i = 0; i < ROW_PAD; i++)
      g_assert_cmpuint (im->data[y * im->stride + 4 * im->width + i], ==, PAD_BYTE);
  g_free (im->data);
}

static const guint8 *
px (const Img * im, gint x, gint y)
{
  return im->data + y * im->stride + 4 * x;
}

static gboolean
is_rgba (const Img * im, gint x, gint y, guint8 r, guint8 g, guint8 b, guint8 a)
{
  const guint8 *p = px (im, x, y);

  return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

static gboolean
is_grey (const Img * im, gint x, gint y)
{
  return is_rgba (im, x, y, GREY, GREY, GREY, 255);
}

static void
draw (Img * im, const RdsOsdCmd * cmds, guint n, const guint8 * masks)
{
  rds_osd_shade_commands (cmds, n, masks, im->data, im->width, im->height, im->stride);
}

/* Draws @cmds in RED_HALF-like colours and checks that the drawn pixels are exactly those @want says, each blended
 * once (red at alpha 0.5 over grey 128 is 191, 64, 64). */
static void
check_set (const RdsOsdCmd * cmds, guint n, gint w, gint h, gboolean (*want) (gint x, gint y, gpointer data),
    gpointer data, const gchar * what)
{
  Img im;

  img_init (&im, w, h);
  draw (&im, cmds, n, NULL);
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++) {
      gboolean in = want (x, y, data);

      if (in ? !is_rgba (&im, x, y, 191, 64, 64, 255) : !is_grey (&im, x, y))
        g_error ("%s: pixel %d,%d: expected %s, got %u,%u,%u", what, x, y, in ? "drawn once" : "untouched",
            px (&im, x, y)[0], px (&im, x, y)[1], px (&im, x, y)[2]);
    }
  img_clear (&im);
}

/* ---- layout ---- */

static void
test_layout (void)
{
  /* the GPU kernel reads the same bytes: fixed size, no pointers */
  g_assert_cmpuint (sizeof (RdsOsdCmd), ==, 96);
  g_assert_cmpuint (G_STRUCT_OFFSET (RdsOsdCmd, color), ==, 24);
  g_assert_cmpuint (G_STRUCT_OFFSET (RdsOsdCmd, segment), ==, 56);
  g_assert_cmpuint (G_STRUCT_OFFSET (RdsOsdCmd, ring), ==, 56);
  g_assert_cmpuint (G_STRUCT_OFFSET (RdsOsdCmd, mask), ==, 56);
  g_assert_cmpuint (G_STRUCT_OFFSET (RdsOsdCmd, triangle), ==, 56);
}

/* ---- boxes and rectangle bands ---- */

typedef struct
{
  gint64 x0, y0, x1, y1;
} Box;

static gboolean
in_box (gint x, gint y, gpointer data)
{
  const Box *b = data;

  return x >= b->x0 && x <= b->x1 && y >= b->y0 && y <= b->y1;
}

static void
test_box (void)
{
  RdsOsdCmd c;
  Box b = { 5, 3, 9, 6 }, clipped = { 0, 0, 2, 1 };

  g_assert_cmpuint (rds_osd_shape_box (&c, 5, 3, 9, 6, &RED_HALF, 16, 12), ==, 1);
  g_assert_cmpint (c.type, ==, RDS_OSD_CMD_BOX);
  check_set (&c, 1, 16, 12, in_box, &b, "box");
  /* clipped to the frame */
  g_assert_cmpuint (rds_osd_shape_box (&c, -5, -7, 2, 1, &RED_HALF, 16, 12), ==, 1);
  g_assert_cmpint (c.x0, ==, 0);
  g_assert_cmpint (c.y0, ==, 0);
  check_set (&c, 1, 16, 12, in_box, &clipped, "clipped box");
  /* outside, empty, inverted: nothing */
  g_assert_cmpuint (rds_osd_shape_box (&c, 16, 0, 20, 5, &RED_HALF, 16, 12), ==, 0);
  g_assert_cmpuint (rds_osd_shape_box (&c, 0, -9, 5, -1, &RED_HALF, 16, 12), ==, 0);
  g_assert_cmpuint (rds_osd_shape_box (&c, 6, 2, 5, 4, &RED_HALF, 16, 12), ==, 0);
  g_assert_cmpuint (rds_osd_shape_box (&c, -(gint64) 1e15, -(gint64) 1e15, (gint64) 1e15, 0, &RED_HALF, 16, 12), ==,
      1);
  g_assert_cmpint (c.x1, ==, 15);
}

typedef struct
{
  gint64 L, T, R, B, bw;
} Rect;

/* DeepStream 8.0 GPU mode, written row by row: rows T − lo .. B + lo; a row within the top or bottom band, or one
 * where the side bands meet, is covered from L − lo to R + lo; other rows only in the side bands. */
static gboolean
in_bands (gint x, gint y, gpointer data)
{
  const Rect *r = data;
  gint64 lo = r->bw > 0 ? (r->bw - 1) / 2 : 0, hi = r->bw / 2;

  if (r->bw == 0 || y < r->T - lo || y > r->B + lo || x < r->L - lo || x > r->R + lo)
    return FALSE;
  if (y <= r->T + hi || y >= r->B - hi || r->L + hi + 1 >= r->R - hi)
    return TRUE;
  return x <= r->L + hi || x >= r->R - hi;
}

static void
test_rect_bands (void)
{
  const gint64 shapes[][4] = {
    {10, 8, 40, 30},            /* a box */
    {10, 8, 14, 30},            /* narrow: the side bands meet */
    {10, 8, 40, 11},            /* flat: the top and bottom bands overlap */
    {10, 8, 10, 8},             /* a point */
    {20, 20, 15, 18},           /* negative sizes */
    {-30, -40, 20, 25},         /* partly outside */
    {40, 5, 70, 60},            /* past the right and bottom edges */
    {-200, -200, -100, -100},   /* outside */
  };
  const guint widths[] = { 0, 1, 2, 3, 4, 5, 6, 10, 11, 31, 32 };

  for (guint s = 0; s < G_N_ELEMENTS (shapes); s++)
    for (guint k = 0; k < G_N_ELEMENTS (widths); k++) {
      RdsOsdCmd c[4];
      Rect r = { shapes[s][0], shapes[s][1], shapes[s][2], shapes[s][3], widths[k] };
      guint n = rds_osd_shape_rect_border (c, r.L, r.T, r.R, r.B, widths[k], &RED_HALF, 64, 48);
      gchar *what = g_strdup_printf ("rect %d: bw %u", s, widths[k]);

      g_assert_cmpuint (n, <=, 4);
      if (widths[k] == 0)
        g_assert_cmpuint (n, ==, 0);
      check_set (c, n, 64, 48, in_bands, &r, what);
      g_free (what);
    }
}

/* ---- segments ---- */

typedef struct
{
  gdouble x1, y1, x2, y2, h;    /* pixel coordinates, half width */
  gboolean square;
} Seg;

/* Reference: the pixel centre's distance along and across the segment (between pixel centres), in doubles. No pixel
 * centre is near an edge for integer endpoints and odd widths, so double rounding never matters. */
static gboolean
in_seg (gint x, gint y, gpointer data)
{
  const Seg *s = data;
  gdouble dx = s->x2 - s->x1, dy = s->y2 - s->y1, len = hypot (dx, dy), ox = x - s->x1, oy = y - s->y1;
  gdouble along = (ox * dx + oy * dy) / len, across = (oy * dx - ox * dy) / len, ext = s->square ? s->h : 0;

  /* a zero-length line with square caps: a square, as Cairo draws it */
  if (len == 0)
    return fabs (ox) < s->h && fabs (oy) < s->h;
  return fabs (across) < s->h && along > -ext && along < len + ext;
}

static void
check_line (gint x1, gint y1, gint x2, gint y2, guint w, gdouble drawn_half)
{
  RdsOsdCmd c;
  Seg s = { x1, y1, x2, y2, drawn_half, TRUE };
  gchar *what = g_strdup_printf ("line %d,%d-%d,%d w %u", x1, y1, x2, y2, w);

  g_assert_cmpuint (rds_osd_shape_line (&c, x1, y1, x2, y2, w, &RED_HALF, 48, 40), ==, 1);
  g_assert_cmpint (c.type, ==, x1 == x2 && y1 == y2 ? RDS_OSD_CMD_BOX : RDS_OSD_CMD_SEGMENT);
  check_set (&c, 1, 48, 40, in_seg, &s, what);
  g_free (what);
}

static void
test_segments (void)
{
  const gint ends[][4] = {
    {5, 20, 40, 20}, {40, 20, 5, 20},   /* horizontal, both ways */
    {24, 3, 24, 36},            /* vertical */
    {6, 6, 34, 34}, {34, 6, 6, 34},     /* 45° */
    {3, 10, 44, 17},            /* shallow */
    {10, 2, 15, 37},            /* steep */
    {4, 4, 16, 20},             /* 3-4-5 */
    {20, 20, 20, 20},           /* a point: a square */
  };
  /* DeepStream 8.0 GPU mode: (w − 1) / 2 pixels each side: w 0, 1, 2 draw 1 pixel, 3 and 4 draw 3, 6 draws 5 */
  const struct
  {
    guint w;
    gdouble half;
  } widths[] = { {0, 0.5}, {1, 0.5}, {2, 0.5}, {3, 1.5}, {4, 1.5}, {6, 2.5}, {7, 3.5} };
  RdsOsdCmd c;
  Img im;

  for (guint e = 0; e < G_N_ELEMENTS (ends); e++)
    for (guint k = 0; k < G_N_ELEMENTS (widths); k++)
      check_line (ends[e][0], ends[e][1], ends[e][2], ends[e][3], widths[k].w, widths[k].half);

  /* by hand: width 6 from 5,20 to 40,20 covers rows 18..22 and, with its square caps, columns 3..42 */
  img_init (&im, 48, 40);
  rds_osd_shape_line (&c, 5, 20, 40, 20, 6, &RED, 48, 40);
  draw (&im, &c, 1, NULL);
  g_assert_true (is_grey (&im, 20, 17));
  g_assert_true (is_rgba (&im, 20, 18, 255, 0, 0, 255));
  g_assert_true (is_rgba (&im, 20, 22, 255, 0, 0, 255));
  g_assert_true (is_grey (&im, 20, 23));
  g_assert_true (is_grey (&im, 2, 20));
  g_assert_true (is_rgba (&im, 3, 20, 255, 0, 0, 255));
  g_assert_true (is_rgba (&im, 42, 20, 255, 0, 0, 255));
  g_assert_true (is_grey (&im, 43, 20));
  img_clear (&im);

  /* by hand: width 1 at 45° is the diagonal only, end pixels included */
  img_init (&im, 48, 40);
  rds_osd_shape_line (&c, 6, 6, 12, 12, 1, &RED, 48, 40);
  draw (&im, &c, 1, NULL);
  for (gint i = 6; i <= 12; i++) {
    g_assert_true (is_rgba (&im, i, i, 255, 0, 0, 255));
    g_assert_true (is_grey (&im, i + 1, i));
    g_assert_true (is_grey (&im, i, i + 1));
  }
  g_assert_true (is_grey (&im, 5, 5));
  g_assert_true (is_grey (&im, 13, 13));
  img_clear (&im);

  /* far outside or past the frame: clipped, the same pixels inside it */
  check_line (-500, 20, 4000, 20, 3, 1.5);
  check_line (24, -1000000, 24, 1000000, 5, 2.5);
  g_assert_cmpuint (rds_osd_shape_line (&c, 0, 200, 47, 200, 3, &RED, 48, 40), ==, 0);
  g_assert_cmpuint (rds_osd_shape_line (&c, 4e9, 4e9, 4e9 - 5, 7, 3, &RED, 48, 40), ==, 0);
}

static void
test_wide_line (void)
{
  RdsOsdCmd c;
  Img im;

  /* ours: a width far beyond the frame is clamped to twice its larger side plus one, which covers it */
  img_init (&im, 48, 40);
  g_assert_cmpuint (rds_osd_shape_line (&c, 10, 20, 30, 20, G_MAXUINT, &RED, 48, 40), ==, 1);
  draw (&im, &c, 1, NULL);
  for (gint y = 0; y < 40; y++)
    for (gint x = 0; x < 48; x++)
      g_assert_true (is_rgba (&im, x, y, 255, 0, 0, 255));
  img_clear (&im);
}

/* ---- arrow heads ---- */

typedef struct
{
  gdouble tx, ty, fx, fy, h;
} Head;

/* The point's signed distance inside the convex polygon @p (n corners), negative outside. */
static gdouble
inside_polygon (const gdouble (*p)[2], guint n, gdouble x, gdouble y)
{
  gdouble area = 0, d = G_MAXDOUBLE;

  for (guint i = 0; i < n; i++) {
    guint j = (i + 1) % n;

    area += p[i][0] * p[j][1] - p[j][0] * p[i][1];
  }
  for (guint i = 0; i < n; i++) {
    guint j = (i + 1) % n;
    gdouble ex = p[j][0] - p[i][0], ey = p[j][1] - p[i][1];
    gdouble e = (ex * (y - p[i][1]) - ey * (x - p[i][0])) / hypot (ex, ey);

    d = MIN (d, area > 0 ? e : -e);
  }
  return d;
}

/* Reference: the stroke of the polyline wing → tip → wing, butt ends, bevel join, as three convex polygons in
 * doubles. Returns the pixel's signed distance inside the shape (negative: outside). */
static gdouble
head_depth (const Head * hd, gint x, gint y)
{
  gdouble len = hypot (hd->tx - hd->fx, hd->ty - hd->fy), ux = (hd->tx - hd->fx) / len, uy = (hd->ty - hd->fy) / len;
  gdouble w[2][2] = {
    {hd->tx - 22 * ux - 7 * uy, hd->ty - 22 * uy + 7 * ux},
    {hd->tx - 22 * ux + 7 * uy, hd->ty - 22 * uy - 7 * ux},
  };
  gdouble corner[2][2], best = -G_MAXDOUBLE;

  for (guint i = 0; i < 2; i++) {
    gdouble ex = w[i][0] - hd->tx, ey = w[i][1] - hd->ty, el = hypot (ex, ey), nx = -ey / el * hd->h,
        ny = ex / el * hd->h;
    gdouble other_x = w[1 - i][0] - hd->tx, other_y = w[1 - i][1] - hd->ty;
    gdouble rect[4][2] = {
      {hd->tx + nx, hd->ty + ny}, {w[i][0] + nx, w[i][1] + ny}, {w[i][0] - nx, w[i][1] - ny},
      {hd->tx - nx, hd->ty - ny}
    };

    best = MAX (best, inside_polygon ((const gdouble (*)[2]) rect, 4, x, y));
    /* the bevel's corner on this wing: the side away from the other wing */
    if (nx * other_x + ny * other_y > 0) {
      nx = -nx;
      ny = -ny;
    }
    corner[i][0] = hd->tx + nx;
    corner[i][1] = hd->ty + ny;
  }
  {
    gdouble tri[3][2] = { {hd->tx, hd->ty}, {corner[0][0], corner[0][1]}, {corner[1][0], corner[1][1]} };

    best = MAX (best, inside_polygon ((const gdouble (*)[2]) tri, 3, x, y));
  }
  return best;
}

static void
check_head (gint tx, gint ty, gint fx, gint fy, guint w, gdouble drawn_half)
{
  RdsOsdCmd c[3];
  Img im;
  Head hd = { tx, ty, fx, fy, drawn_half };
  guint n = rds_osd_shape_arrow_head (c, tx, ty, fx, fy, w, &RED_HALF, 96, 80);

  g_assert_cmpuint (n, >=, 1);
  g_assert_cmpuint (n, <=, 3);
  for (guint i = 0; i + 1 < n; i++)
    g_assert_true (c[i].flags & RDS_OSD_CMD_UNION_NEXT);
  g_assert_false (c[n - 1].flags & RDS_OSD_CMD_UNION_NEXT);
  img_init (&im, 96, 80);
  draw (&im, c, n, NULL);
  for (gint y = 0; y < 80; y++)
    for (gint x = 0; x < 96; x++) {
      gdouble depth = head_depth (&hd, x, y);

      /* the wing ends are rounded to Cairo's 1/256 px: pixels that close to an edge may go either way */
      if (fabs (depth) < 1.0 / 128)
        continue;
      if (depth > 0 ? !is_rgba (&im, x, y, 191, 64, 64, 255) : !is_grey (&im, x, y))
        g_error ("head at %d,%d from %d,%d w %u: pixel %d,%d (depth %.4f) got %u,%u,%u", tx, ty, fx, fy, w, x, y,
            depth, px (&im, x, y)[0], px (&im, x, y)[1], px (&im, x, y)[2]);
    }
  img_clear (&im);
}

static void
test_arrow_heads (void)
{
  RdsOsdCmd c[3];
  Img im;

  /* every direction, thin and wide; blended once where the wings overlap */
  for (guint a = 0; a < 16; a++) {
    gdouble ang = a * G_PI / 8 + 0.1;
    gint fx = 48 - (gint) lround (30 * cos (ang)), fy = 40 - (gint) lround (30 * sin (ang));

    check_head (48, 40, fx, fy, 1, 0.5);
    check_head (48, 40, fx, fy, 3, 1.5);
    check_head (48, 40, fx, fy, 6, 2.5);
    check_head (48, 40, fx, fy, 21, 10.5);
  }

  /* the bevel join: a width-21 head pointing right at 48,40 reaches 3 px past its tip (the bevel's corners are
   * 10.5 px off the tip, square to the wings), not 4 (round) or 34 (miter) */
  img_init (&im, 96, 80);
  g_assert_cmpuint (rds_osd_shape_arrow_head (c, 48, 40, 10, 40, 21, &RED, 96, 80), ==, 3);
  draw (&im, c, 3, NULL);
  g_assert_true (is_rgba (&im, 51, 40, 255, 0, 0, 255));
  g_assert_true (is_grey (&im, 52, 40));
  g_assert_true (is_grey (&im, 60, 40));
  /* the wings end 22 px back and 7 px to each side */
  g_assert_true (is_rgba (&im, 27, 33, 255, 0, 0, 255));
  g_assert_true (is_rgba (&im, 27, 47, 255, 0, 0, 255));
  img_clear (&im);

  /* the same head without the join (the wings alone, a complete shape): its pixels past the tip go */
  img_init (&im, 96, 80);
  c[1].flags &= ~RDS_OSD_CMD_UNION_NEXT;
  draw (&im, c, 2, NULL);
  g_assert_true (is_grey (&im, 51, 40));
  img_clear (&im);

  /* outside the frame: nothing; a tip on its source point: nothing */
  g_assert_cmpuint (rds_osd_shape_arrow_head (c, 500, 40, 400, 40, 3, &RED, 96, 80), ==, 0);
  g_assert_cmpuint (rds_osd_shape_arrow_head (c, 48, 40, 48, 40, 3, &RED, 96, 80), ==, 0);
}

/* ---- circles ---- */

typedef struct
{
  gint xc, yc;
  gdouble inner, outer;         /* band edges: drawn when inner < distance < outer */
} Ring;

static gboolean
in_ring (gint x, gint y, gpointer data)
{
  const Ring *r = data;
  gdouble d = hypot (x - r->xc, y - r->yc);

  return d > r->inner && d < r->outer;
}

static void
test_rings (void)
{
  const struct
  {
    guint r, w;
    gint lo, hi;
  } cases[] = {
    /* DeepStream 8.0 GPU mode: distances r − ⌊w/2⌋ .. r + ⌈w/2⌉; w 0 is a 3 px ring */
    {60, 3, 59, 62}, {60, 0, 59, 61}, {60, 1, 60, 61}, {60, 2, 59, 61}, {60, 4, 58, 62}, {5, 3, 4, 7},
    {1, 4, -1, 3}, {0, 0, -1, 1}, {2, 9, -2, 7},
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    RdsOsdCmd c[2];
    Ring ring = { 70, 66, cases[i].lo - 0.5, cases[i].hi + 0.5 };
    Ring fill = { 70, 66, -1, cases[i].lo - 0.5 };
    gchar *what = g_strdup_printf ("r %u w %u", cases[i].r, cases[i].w);
    guint n;

    n = rds_osd_shape_circle (c, 70, 66, cases[i].r, cases[i].w, &RED_HALF, NULL, 150, 140);
    g_assert_cmpuint (n, ==, 1);
    check_set (c, n, 150, 140, in_ring, &ring, what);
    /* the fill alone, inside the ring: to r − ⌊w/2⌋ − 1 */
    n = rds_osd_shape_circle (c, 70, 66, cases[i].r, cases[i].w, NULL, &RED_HALF, 150, 140);
    if (cases[i].lo >= 1) {
      g_assert_cmpuint (n, ==, 1);
      check_set (c, n, 150, 140, in_ring, &fill, what);
    } else {
      g_assert_cmpuint (n, ==, 0);
    }
    g_free (what);
  }
}

static void
test_circle_limits (void)
{
  RdsOsdCmd c[2];
  Img im;

  /* the fill first, then the ring on top */
  img_init (&im, 150, 140);
  g_assert_cmpuint (rds_osd_shape_circle (c, 70, 66, 60, 3, &RED, &GREEN_HALF, 150, 140), ==, 2);
  draw (&im, c, 2, NULL);
  g_assert_true (is_rgba (&im, 70, 66, 64, 191, 64, 255));
  g_assert_true (is_rgba (&im, 70 + 58, 66, 64, 191, 64, 255));
  g_assert_true (is_rgba (&im, 70 + 59, 66, 255, 0, 0, 255));
  g_assert_true (is_rgba (&im, 70 + 62, 66, 255, 0, 0, 255));
  g_assert_true (is_grey (&im, 70 + 63, 66));
  img_clear (&im);

  /* ours: a centre or radius beyond 2^21 is not drawn; a wider ring is taken as 2^21 */
  g_assert_cmpuint (rds_osd_shape_circle (c, 2097153, 5, 10, 3, &RED, &RED, 150, 140), ==, 0);
  g_assert_cmpuint (rds_osd_shape_circle (c, 5, 5, 2097153, 3, &RED, &RED, 150, 140), ==, 0);
  /* a ring whose band misses the frame: nothing; one whose band covers it: the frame */
  g_assert_cmpuint (rds_osd_shape_circle (c, 70, 66, 2097152, 3, &RED, NULL, 150, 140), ==, 0);
  img_init (&im, 150, 140);
  g_assert_cmpuint (rds_osd_shape_circle (c, 70, 66, 30, G_MAXUINT, &RED, NULL, 150, 140), ==, 1);
  draw (&im, c, 1, NULL);
  for (gint y = 0; y < 140; y++)
    for (gint x = 0; x < 150; x++)
      g_assert_true (is_rgba (&im, x, y, 255, 0, 0, 255));
  img_clear (&im);
}

/* ---- masks and blending ---- */

/* The blend written out: a·src + (1 − a)·dst + 1e-6, truncated, a = alpha × coverage / 255 (alpha at 255). */
static guint8
blend_ref (gdouble alpha, gdouble src, guint8 dst, guint8 coverage)
{
  gdouble a = coverage == 255 ? alpha : alpha * coverage / 255.0, v = a * src + (1.0 - a) * dst + 1e-6;

  return (guint8) MIN (v, 255.0);
}

static void
test_mask (void)
{
  /* a hand-made 4 × 2 mask (stride 8) at frame pixel 3, 2, behind 5 bytes of another mask */
  guint8 masks[5 + 16] = { 9, 9, 9, 9, 9,
    0, 1, 64, 128, 0xAA, 0xAA, 0xAA, 0xAA,
    200, 254, 255, 255, 0xAA, 0xAA, 0xAA, 0xAA
  };
  const guint8 cov[2][4] = { {0, 1, 64, 128}, {200, 254, 255, 255} };
  RdsOsdColor c;
  RdsOsdCmd cmd;
  Img im;

  g_assert_cmpuint (rds_osd_color_init (&c, 1, 0.2, 0.7, 0.6), ==, 1);
  g_assert_cmpuint (rds_osd_shape_mask (&cmd, 3, 2, 4, 2, 8, 5, &c, 16, 8), ==, 1);
  g_assert_cmpint (cmd.type, ==, RDS_OSD_CMD_MASK);
  img_init (&im, 16, 8);
  draw (&im, &cmd, 1, masks);
  for (gint y = 0; y < 8; y++)
    for (gint x = 0; x < 16; x++) {
      gboolean in = x >= 3 && x < 7 && y >= 2 && y < 4;
      guint8 k = in ? cov[y - 2][x - 3] : 0;

      if (k == 0) {
        g_assert_true (is_grey (&im, x, y));
        continue;
      }
      g_assert_cmpuint (px (&im, x, y)[0], ==, blend_ref (0.6, 255.0, GREY, k));
      g_assert_cmpuint (px (&im, x, y)[1], ==, blend_ref (0.6, 0.2 * 255.0, GREY, k));
      g_assert_cmpuint (px (&im, x, y)[2], ==, blend_ref (0.6, 0.7 * 255.0, GREY, k));
      g_assert_cmpuint (px (&im, x, y)[3], ==, blend_ref (0.6, 255.0, 255, k));
    }
  /* by hand: opaque red at coverage 128 over 128 is 128 + 63.75 (191), green 63 */
  img_clear (&im);
  img_init (&im, 16, 8);
  rds_osd_shape_mask (&cmd, 3, 2, 4, 2, 8, 5, &RED, 16, 8);
  draw (&im, &cmd, 1, masks);
  g_assert_true (is_rgba (&im, 6, 2, 191, 63, 63, 255));
  img_clear (&im);

  /* clipped at the frame's edges */
  img_init (&im, 16, 8);
  g_assert_cmpuint (rds_osd_shape_mask (&cmd, -2, 7, 4, 2, 8, 5, &RED, 16, 8), ==, 1);
  g_assert_cmpint (cmd.x0, ==, 0);
  g_assert_cmpint (cmd.x1, ==, 1);
  g_assert_cmpint (cmd.y1, ==, 7);
  draw (&im, &cmd, 1, masks);
  g_assert_true (is_rgba (&im, 1, 7, 191, 63, 63, 255));
  img_clear (&im);
  g_assert_cmpuint (rds_osd_shape_mask (&cmd, 16, 0, 4, 2, 8, 5, &RED, 16, 8), ==, 0);
}

static void
test_blend (void)
{
  GRand *rand = g_rand_new_with_seed (7);

  /* every colour, alpha, coverage and destination byte as the blend written out, alpha 0 or less draws nothing */
  for (guint i = 0; i < 20000; i++) {
    gdouble r = g_rand_double_range (rand, -0.2, 1.2), a = g_rand_double_range (rand, -0.1, 1.1);
    guint8 dst[4], got[4], cov = i % 3 == 0 ? 255 : (guint8) g_rand_int_range (rand, 1, 256);
    RdsOsdColor c;

    for (guint k = 0; k < 4; k++)
      dst[k] = got[k] = (guint8) g_rand_int_range (rand, 0, 256);
    if (!rds_osd_color_init (&c, r, 0.5, 1.0, a)) {
      g_assert_cmpfloat (a, <=, 0);
      continue;
    }
    rds_osd_blend (got, &c, cov);
    g_assert_cmpuint (got[0], ==, blend_ref (MIN (a, 1.0), CLAMP (r, 0.0, 1.0) * 255.0, dst[0], cov));
    g_assert_cmpuint (got[1], ==, blend_ref (MIN (a, 1.0), 0.5 * 255.0, dst[1], cov));
    g_assert_cmpuint (got[2], ==, blend_ref (MIN (a, 1.0), 255.0, dst[2], cov));
    g_assert_cmpuint (got[3], ==, blend_ref (MIN (a, 1.0), 255.0, dst[3], cov));
  }
  g_rand_free (rand);
}

static void
test_order (void)
{
  RdsOsdCmd c[2];
  Img im;

  /* later on top: red then green at alpha 0.5 over 128: (191, 64, 64), then (95, 159, 32) */
  img_init (&im, 16, 8);
  rds_osd_shape_box (&c[0], 0, 0, 9, 7, &RED_HALF, 16, 8);
  rds_osd_shape_box (&c[1], 5, 0, 15, 7, &GREEN_HALF, 16, 8);
  draw (&im, c, 2, NULL);
  g_assert_true (is_rgba (&im, 2, 2, 191, 64, 64, 255));
  g_assert_true (is_rgba (&im, 7, 2, 95, 159, 32, 255));
  g_assert_true (is_rgba (&im, 12, 2, 64, 191, 64, 255));
  img_clear (&im);

  /* one shape of two parts (RDS_OSD_CMD_UNION_NEXT): blended once where they overlap, in the last part's colour */
  img_init (&im, 16, 8);
  rds_osd_shape_box (&c[0], 0, 0, 9, 7, &GREEN_HALF, 16, 8);
  rds_osd_shape_box (&c[1], 5, 0, 15, 7, &RED_HALF, 16, 8);
  c[0].flags |= RDS_OSD_CMD_UNION_NEXT;
  draw (&im, c, 2, NULL);
  g_assert_true (is_rgba (&im, 2, 2, 191, 64, 64, 255));
  g_assert_true (is_rgba (&im, 7, 2, 191, 64, 64, 255));
  g_assert_true (is_rgba (&im, 12, 2, 191, 64, 64, 255));
  img_clear (&im);
}

/* ---- 64-bit headroom ---- */

static void
test_headroom (void)
{
  /* the widest frame shapes are drawn on, a few rows: the largest coordinates and widths the builders accept, as
   * shade_commands draws them and as shade_pixel does */
  const gint w = RDS_OSD_MAX_SHAPE_FRAME, h = 3;
  RdsOsdColor c[4];
  RdsOsdCmd cmds[16];
  guint n = 0;
  Img a, b;

  rds_osd_color_init (&c[0], 1, 0, 0, 0.5);
  rds_osd_color_init (&c[1], 0, 1, 0, 0.5);
  rds_osd_color_init (&c[2], 0, 0, 1, 0.5);
  rds_osd_color_init (&c[3], 1, 1, 0, 0.5);
  /* lines as long as coordinates go, at the widest stroke (clamped to 2·2^20 + 1) and thin */
  n += rds_osd_shape_line (&cmds[n], 0, 1, G_MAXUINT, 2, G_MAXUINT, &c[0], w, h);
  n += rds_osd_shape_line (&cmds[n], G_MAXUINT, 0, 0, G_MAXUINT, 3, &c[1], w, h);
  n += rds_osd_shape_line (&cmds[n], 0, 1, G_MAXUINT, 1, 5, &c[2], w, h);
  /* arrow heads at RDS_OSD_MAX_COORD, widest and thin */
  n += rds_osd_shape_arrow_head (&cmds[n], RDS_OSD_MAX_COORD, 1, 0, 1, G_MAXUINT, &c[3], w, h);
  n += rds_osd_shape_arrow_head (&cmds[n], w - 3, 1, RDS_OSD_MAX_COORD, RDS_OSD_MAX_COORD, G_MAXUINT, &c[1], w, h);
  n += rds_osd_shape_arrow_head (&cmds[n], w - 10, 1, 0, 1, 9, &c[2], w, h);
  /* rings as large as allowed */
  n += rds_osd_shape_circle (&cmds[n], RDS_OSD_MAX_COORD, RDS_OSD_MAX_COORD, RDS_OSD_MAX_COORD, G_MAXUINT, &c[0],
      &c[1], w, h);
  n += rds_osd_shape_circle (&cmds[n], w / 2, 1, RDS_OSD_MAX_COORD, 3, &c[2], &c[3], w, h);
  g_assert_cmpuint (n, >=, 8);

  img_init (&a, w, h);
  img_init (&b, w, h);
  rds_osd_shade_commands (cmds, n, NULL, a.data, w, h, a.stride);
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++)
      rds_osd_shade_pixel (cmds, NULL, n, x, y, b.data + y * b.stride + 4 * x, NULL);
  for (gint y = 0; y < h; y++)
    if (memcmp (a.data + y * a.stride, b.data + y * b.stride, 4 * (gsize) w) != 0)
      g_error ("row %d differs", y);
  /* the widest line covers the whole frame */
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x += 4099)
      g_assert_false (is_grey (&a, x, y));
  img_clear (&a);
  img_clear (&b);

  /* one pixel wider: lines, arrow heads and circles are not drawn (ours) */
  g_assert_cmpuint (rds_osd_shape_line (cmds, 0, 1, 10, 1, 3, &c[0], w + 1, h), ==, 0);
  g_assert_cmpuint (rds_osd_shape_arrow_head (cmds, 10, 1, 0, 1, 3, &c[0], w + 1, h), ==, 0);
  g_assert_cmpuint (rds_osd_shape_circle (cmds, 10, 1, 5, 3, &c[0], NULL, w + 1, h), ==, 0);
  g_assert_cmpuint (rds_osd_shape_line (cmds, 0, 1, 10, 1, G_MAXUINT, &c[0], 4, G_MAXINT), ==, 0);
}

/* ---- per bounding box == per pixel ---- */

static void
random_color (GRand * rand, RdsOsdColor * c)
{
  while (!rds_osd_color_init (c, g_rand_double (rand), g_rand_double (rand), g_rand_double (rand),
          g_rand_double_range (rand, -0.1, 1.0)));
}

static void
test_bbox_equals_per_pixel (void)
{
  GRand *rand = g_rand_new_with_seed (2026);
  guint8 masks[64 * 64];

  for (guint i = 0; i < sizeof masks; i++)
    masks[i] = i % 7 == 0 ? 0 : (guint8) g_rand_int_range (rand, 0, 256);
  for (guint scene = 0; scene < 200; scene++) {
    gint w = g_rand_int_range (rand, 1, 120), h = g_rand_int_range (rand, 1, 90);
    RdsOsdCmd cmds[256];
    guint32 list[256];
    guint n = 0;
    Img a, b;

    while (n < 200) {
      RdsOsdColor c, f;
      gint kind = g_rand_int_range (rand, 0, 6);
      gint64 x0 = g_rand_int_range (rand, -40, w + 40), y0 = g_rand_int_range (rand, -40, h + 40);
      gint64 x1 = g_rand_int_range (rand, -40, w + 40), y1 = g_rand_int_range (rand, -40, h + 40);
      guint width = g_rand_boolean (rand) ? g_rand_int_range (rand, 0, 12) : g_rand_int_range (rand, 0, 400);

      random_color (rand, &c);
      random_color (rand, &f);
      switch (kind) {
        case 0:
          n += rds_osd_shape_box (&cmds[n], x0, y0, x1, y1, &c, w, h);
          break;
        case 1:
          n += rds_osd_shape_rect_border (&cmds[n], x0, y0, x1, y1, g_rand_int_range (rand, 0, 33), &c, w, h);
          break;
        case 2:
          n += rds_osd_shape_line (&cmds[n], MAX (x0, 0), MAX (y0, 0), MAX (x1, 0), MAX (y1, 0), width, &c, w, h);
          break;
        case 3:
          n += rds_osd_shape_arrow_head (&cmds[n], MAX (x0, 0), MAX (y0, 0), MAX (x1, 0), MAX (y1, 0), width, &c, w,
              h);
          break;
        case 4:
          n += rds_osd_shape_circle (&cmds[n], MAX (x0, 0), MAX (y0, 0), g_rand_int_range (rand, 0, 200), width,
              g_rand_boolean (rand) ? &c : NULL, g_rand_boolean (rand) ? &f : NULL, w, h);
          break;
        default:
          n += rds_osd_shape_mask (&cmds[n], x0, y0, g_rand_int_range (rand, 1, 33), g_rand_int_range (rand, 1, 33),
              64, (guint) g_rand_int_range (rand, 0, 32), &c, w, h);
          break;
      }
    }
    for (guint i = 0; i < n; i++)
      list[i] = i;

    img_init (&a, w, h);
    img_init (&b, w, h);
    rds_osd_shade_commands (cmds, n, masks, a.data, w, h, a.stride);
    for (gint y = 0; y < h; y++)
      for (gint x = 0; x < w; x++)
        rds_osd_shade_pixel (cmds, (scene & 1) ? list : NULL, n, x, y, b.data + y * b.stride + 4 * x, masks);
    for (gint y = 0; y < h; y++)
      if (memcmp (a.data + y * a.stride, b.data + y * b.stride, 4 * (gsize) w) != 0)
        g_error ("scene %u (%dx%d, %u commands): row %d differs", scene, w, h, n, y);
    img_clear (&a);
    img_clear (&b);
  }
  g_rand_free (rand);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  rds_osd_color_init (&RED, 1, 0, 0, 1);
  rds_osd_color_init (&RED_HALF, 1, 0, 0, 0.5);
  rds_osd_color_init (&GREEN_HALF, 0, 1, 0, 0.5);

  g_test_add_func ("/cpu/osd-shapes/layout", test_layout);
  g_test_add_func ("/cpu/osd-shapes/box", test_box);
  g_test_add_func ("/cpu/osd-shapes/rect-bands", test_rect_bands);
  g_test_add_func ("/cpu/osd-shapes/segments", test_segments);
  g_test_add_func ("/cpu/osd-shapes/wide-line", test_wide_line);
  g_test_add_func ("/cpu/osd-shapes/arrow-heads", test_arrow_heads);
  g_test_add_func ("/cpu/osd-shapes/rings", test_rings);
  g_test_add_func ("/cpu/osd-shapes/circle-limits", test_circle_limits);
  g_test_add_func ("/cpu/osd-shapes/mask", test_mask);
  g_test_add_func ("/cpu/osd-shapes/blend", test_blend);
  g_test_add_func ("/cpu/osd-shapes/order", test_order);
  g_test_add_func ("/cpu/osd-shapes/bbox-equals-per-pixel", test_bbox_equals_per_pixel);
  g_test_add_func ("/cpu/osd-shapes/headroom", test_headroom);
  return g_test_run ();
}
