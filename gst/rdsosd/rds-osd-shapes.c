/* Redstream: building rdsosd's draw commands from drawing parameters, and the CPU path's shading (see
 * rds-osd-shapes.h). Host only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-shapes.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* arrow heads: an open V, its wings this far back from the tip and this far to each side (measured on DeepStream
 * 8.0's GPU mode output at arrow widths 2 and 4) */
#define ARROW_HEAD_LENGTH 22.0
#define ARROW_HEAD_HALF_WIDTH 7.0
/* the ring of a circle whose circle_width is 0, in pixels (DeepStream 8.0 still outlines it, 3 px in GPU mode) */
#define ZERO_CIRCLE_WIDTH 3

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))

static double
clamp01 (double v)
{
  return v > 0 ? (v < 1 ? v : 1) : 0;
}

unsigned
rds_osd_color_init (RdsOsdColor * c, double red, double green, double blue, double alpha)
{
  c->value[0] = clamp01 (red) * 255.0;
  c->value[1] = clamp01 (green) * 255.0;
  c->value[2] = clamp01 (blue) * 255.0;
  c->alpha = clamp01 (alpha);
  return c->alpha > 0;
}

static int64_t
floor_div (int64_t a, int64_t b)
{
  return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/* A command over the pixel box x0..x1 × y0..y1 clipped to the frame; 0 when that is empty. */
static unsigned
cmd_init (RdsOsdCmd * out, RdsOsdCmdType type, int64_t x0, int64_t y0, int64_t x1, int64_t y1,
    const RdsOsdColor * c, int32_t width, int32_t height)
{
  x0 = MAX2 (x0, 0);
  y0 = MAX2 (y0, 0);
  x1 = MIN2 (x1, (int64_t) width - 1);
  y1 = MIN2 (y1, (int64_t) height - 1);
  if (x0 > x1 || y0 > y1)
    return 0;
  memset (out, 0, sizeof *out);
  out->type = type;
  out->x0 = (int32_t) x0;
  out->y0 = (int32_t) y0;
  out->x1 = (int32_t) x1;
  out->y1 = (int32_t) y1;
  out->color = *c;
  return 1;
}

unsigned
rds_osd_shape_box (RdsOsdCmd * out, int64_t x0, int64_t y0, int64_t x1, int64_t y1, const RdsOsdColor * c,
    int32_t width, int32_t height)
{
  return cmd_init (out, RDS_OSD_CMD_BOX, x0, y0, x1, y1, c, width, height);
}

/* Row by row (DeepStream 8.0 GPU mode): rows T − lo .. B + lo; a row in the top or bottom band, or one where the side
 * bands meet, is covered from L − lo to R + lo, other rows only in the side bands. As disjoint boxes: the top band,
 * the bottom band below it, and the rows between. */
unsigned
rds_osd_shape_rect_border (RdsOsdCmd out[4], int64_t left, int64_t top, int64_t right, int64_t bottom, unsigned bw,
    const RdsOsdColor * c, int32_t width, int32_t height)
{
  int64_t lo = bw > 0 ? (bw - 1) / 2 : 0, hi = bw / 2, x0 = left - lo, x1 = right + lo;
  int64_t mid0 = top + hi + 1, mid1 = bottom - hi - 1;
  unsigned n = 0;

  if (bw == 0)
    return 0;
  n += rds_osd_shape_box (&out[n], x0, top - lo, x1, MIN2 (top + hi, bottom + lo), c, width, height);
  n += rds_osd_shape_box (&out[n], x0, MAX2 (bottom - hi, mid0), x1, bottom + lo, c, width, height);
  if (left + hi + 1 >= right - hi) {
    n += rds_osd_shape_box (&out[n], x0, mid0, x1, mid1, c, width, height);
  } else {
    n += rds_osd_shape_box (&out[n], x0, mid0, left + hi, mid1, c, width, height);
    n += rds_osd_shape_box (&out[n], right - hi, mid0, x1, mid1, c, width, height);
  }
  return n;
}

/* ---- segments ---- */

/* floor (sqrt (v)) */
static uint64_t
isqrt128 (unsigned __int128 v)
{
  uint64_t r = (uint64_t) sqrt ((double) v);

  while ((unsigned __int128) r * r > v)
    r--;
  while ((unsigned __int128) (r + 1) * (r + 1) <= v)
    r++;
  return r;
}

static int32_t
to_fixed (double v)
{
  return (int32_t) llrint (v * RDS_OSD_FIXED_ONE);
}

/* The pixels whose centres are within the rectangle along a → b (fixed point), @hf (fixed) to each side, extended
 * by @hf at both ends with @square caps, else ending at a and b (butt). A zero-length segment with square caps is a
 * square, as Cairo draws it; with butt ends nothing. */
static unsigned
segment (RdsOsdCmd * out, int32_t ax, int32_t ay, int32_t bx, int32_t by, int64_t hf, int square,
    const RdsOsdColor * c, int32_t width, int32_t height)
{
  const int64_t one = RDS_OSD_FIXED_ONE, half = RDS_OSD_FIXED_ONE / 2;
  int64_t dx = (int64_t) bx - ax, dy = (int64_t) by - ay, len2, t;
  int64_t reach = square ? hf : 0;

  if (dx == 0 && dy == 0) {
    if (!square)
      return 0;
    /* pixel centres x·256 + 128 within hf of a */
    return rds_osd_shape_box (out, -floor_div (hf - ax + half, one), -floor_div (hf - ay + half, one),
        floor_div (ax + hf - half, one), floor_div (ay + hf - half, one), c, width, height);
  }
  if (!cmd_init (out, RDS_OSD_CMD_SEGMENT, floor_div (MIN2 (ax, bx) - hf - reach, one) - 1,
          floor_div (MIN2 (ay, by) - hf - reach, one) - 1, floor_div (MAX2 (ax, bx) + hf + reach, one) + 1,
          floor_div (MAX2 (ay, by) + hf + reach, one) + 1, c, width, height))
    return 0;
  len2 = dx * dx + dy * dy;
  /* |d × p| <= hf·|d|  ⟺  |d × p| <= ⌊hf·|d|⌋ (d × p is an integer) */
  t = (int64_t) isqrt128 ((unsigned __int128) (hf * hf) * (unsigned __int128) len2);
  out->segment.ax = ax;
  out->segment.ay = ay;
  out->segment.dx = (int32_t) dx;
  out->segment.dy = (int32_t) dy;
  out->segment.t = t;
  out->segment.dot_lo = square ? -t : 0;
  out->segment.dot_hi = square ? len2 + t : len2;
  return 1;
}

/* Lines and arrow shafts: DeepStream 8.0 GPU mode draws (w − 1) / 2 pixels on each side of the segment (width 6 is
 * 5 pixels wide). Returns the drawn (odd) width. */
static int64_t
stroke_width (unsigned w, int32_t width, int32_t height)
{
  /* ours: a stroke wider than twice the frame covers it anyway */
  unsigned max = 2 * (unsigned) MAX2 (width, height) + 1;

  w = MIN2 (w, max);
  return 2 * (int64_t) (w > 0 ? (w - 1) / 2 : 0) + 1;
}

/* Clips the segment to the box [lo, hi]² (Liang-Barsky); 0 when it misses. */
static int
clip_segment (double *x1, double *y1, double *x2, double *y2, double xlo, double ylo, double xhi, double yhi)
{
  double dx = *x2 - *x1, dy = *y2 - *y1, t0 = 0, t1 = 1;
  double p[4] = { -dx, dx, -dy, dy }, q[4] = { *x1 - xlo, xhi - *x1, *y1 - ylo, yhi - *y1 };

  for (unsigned i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0)
        return 0;
      continue;
    }
    double t = q[i] / p[i];

    if (p[i] < 0)
      t0 = MAX2 (t0, t);
    else
      t1 = MIN2 (t1, t);
  }
  if (t0 > t1)
    return 0;
  *x2 = *x1 + t1 * dx;
  *y2 = *y1 + t1 * dy;
  *x1 = *x1 + t0 * dx;
  *y1 = *y1 + t0 * dy;
  return 1;
}

static int
shape_frame_ok (int32_t width, int32_t height)
{
  return width > 0 && height > 0 && width <= RDS_OSD_MAX_SHAPE_FRAME && height <= RDS_OSD_MAX_SHAPE_FRAME;
}

unsigned
rds_osd_shape_line (RdsOsdCmd * out, double x1, double y1, double x2, double y2, unsigned line_width,
    const RdsOsdColor * c, int32_t width, int32_t height)
{
  int64_t s;
  double margin;

  /* first: stroke_width's clamp would wrap on larger frames */
  if (!shape_frame_ok (width, height))
    return 0;
  s = stroke_width (line_width, width, height);
  margin = (double) s + 2;
  /* clipped to a margin around the frame first, which keeps every coordinate in fixed-point range */
  if (!clip_segment (&x1, &y1, &x2, &y2, -margin, -margin, width + margin, height + margin))
    return 0;
  return segment (out, to_fixed (x1 + 0.5), to_fixed (y1 + 0.5), to_fixed (x2 + 0.5), to_fixed (y2 + 0.5),
      s * RDS_OSD_FIXED_ONE / 2, 1, c, width, height);
}

/* ---- arrow heads ---- */

/* The bevel's corner on the wing along @e (fixed, from the tip): @hf off the tip, square to the wing, on the side
 * away from the other wing @o. */
static void
bevel_corner (int32_t tx, int32_t ty, int64_t ex, int64_t ey, int64_t ox, int64_t oy, int64_t hf, int32_t * cx,
    int32_t * cy)
{
  double len = hypot ((double) ex, (double) ey), nx = -(double) ey / len, ny = (double) ex / len;

  if (nx * (double) ox + ny * (double) oy > 0) {
    nx = -nx;
    ny = -ny;
  }
  *cx = (int32_t) (tx + llrint (nx * (double) hf));
  *cy = (int32_t) (ty + llrint (ny * (double) hf));
}

unsigned
rds_osd_shape_arrow_head (RdsOsdCmd out[3], double tx, double ty, double fx, double fy, unsigned line_width,
    const RdsOsdColor * c, int32_t width, int32_t height)
{
  double len = hypot (tx - fx, ty - fy), ux, uy;
  int64_t hf;
  int32_t tipx, tipy, wx[2], wy[2];
  RdsOsdTriangle tri;
  unsigned n = 0;

  if (!shape_frame_ok (width, height) || len == 0 || fabs (tx) > RDS_OSD_MAX_COORD || fabs (ty) > RDS_OSD_MAX_COORD
      || fabs (fx) > RDS_OSD_MAX_COORD || fabs (fy) > RDS_OSD_MAX_COORD)
    return 0;
  hf = stroke_width (line_width, width, height) * RDS_OSD_FIXED_ONE / 2;
  ux = (tx - fx) / len;
  uy = (ty - fy) / len;
  tipx = to_fixed (tx + 0.5);
  tipy = to_fixed (ty + 0.5);
  wx[0] = to_fixed (tx - ARROW_HEAD_LENGTH * ux - ARROW_HEAD_HALF_WIDTH * uy + 0.5);
  wy[0] = to_fixed (ty - ARROW_HEAD_LENGTH * uy + ARROW_HEAD_HALF_WIDTH * ux + 0.5);
  wx[1] = to_fixed (tx - ARROW_HEAD_LENGTH * ux + ARROW_HEAD_HALF_WIDTH * uy + 0.5);
  wy[1] = to_fixed (ty - ARROW_HEAD_LENGTH * uy - ARROW_HEAD_HALF_WIDTH * ux + 0.5);

  /* the polyline wing → tip → wing, butt ends: two wings, and the bevel join's triangle on the outer side */
  n += segment (&out[n], tipx, tipy, wx[0], wy[0], hf, 0, c, width, height);
  n += segment (&out[n], tipx, tipy, wx[1], wy[1], hf, 0, c, width, height);
  tri.x[0] = tipx;
  tri.y[0] = tipy;
  bevel_corner (tipx, tipy, (int64_t) wx[0] - tipx, (int64_t) wy[0] - tipy, (int64_t) wx[1] - tipx,
      (int64_t) wy[1] - tipy, hf, &tri.x[1], &tri.y[1]);
  bevel_corner (tipx, tipy, (int64_t) wx[1] - tipx, (int64_t) wy[1] - tipy, (int64_t) wx[0] - tipx,
      (int64_t) wy[0] - tipy, hf, &tri.x[2], &tri.y[2]);
  if (cmd_init (&out[n], RDS_OSD_CMD_TRIANGLE,
          floor_div (MIN2 (tri.x[0], MIN2 (tri.x[1], tri.x[2])), RDS_OSD_FIXED_ONE) - 1,
          floor_div (MIN2 (tri.y[0], MIN2 (tri.y[1], tri.y[2])), RDS_OSD_FIXED_ONE) - 1,
          floor_div (MAX2 (tri.x[0], MAX2 (tri.x[1], tri.x[2])), RDS_OSD_FIXED_ONE) + 1,
          floor_div (MAX2 (tri.y[0], MAX2 (tri.y[1], tri.y[2])), RDS_OSD_FIXED_ONE) + 1, c, width, height)) {
    out[n].triangle = tri;
    n++;
  }
  /* one shape: blended once */
  for (unsigned i = 0; i + 1 < n; i++)
    out[i].flags |= RDS_OSD_CMD_UNION_NEXT;
  return n;
}

/* ---- circles ---- */

/* The pixels at squared distances [d2_lo, d2_hi] from (xc, yc), none farther than @reach on either axis. Exact
 * shortcuts: nothing when no pixel of the box can be in the band, a BOX when every one is. */
static unsigned
ring (RdsOsdCmd * out, int64_t xc, int64_t yc, int64_t d2_lo, int64_t d2_hi, int64_t reach, const RdsOsdColor * c,
    int32_t width, int32_t height)
{
  int64_t nx, ny, near2, fx, fy, far2;

  if (!cmd_init (out, RDS_OSD_CMD_RING, xc - reach, yc - reach, xc + reach, yc + reach, c, width, height))
    return 0;
  nx = MIN2 (MAX2 (xc, (int64_t) out->x0), (int64_t) out->x1) - xc;
  ny = MIN2 (MAX2 (yc, (int64_t) out->y0), (int64_t) out->y1) - yc;
  near2 = nx * nx + ny * ny;
  fx = MAX2 (xc - out->x0, out->x1 - xc);
  fy = MAX2 (yc - out->y0, out->y1 - yc);
  far2 = fx * fx + fy * fy;
  if (d2_hi < near2 || d2_lo > far2)
    return 0;
  if (d2_lo <= near2 && d2_hi >= far2) {
    out->type = RDS_OSD_CMD_BOX;
    return 1;
  }
  out->ring.xc = (int32_t) xc;
  out->ring.yc = (int32_t) yc;
  out->ring.d2_lo = d2_lo;
  out->ring.d2_hi = d2_hi;
  return 1;
}

/* The band's edges lie half a pixel outside r − lo and r + hi: covered when (r − lo − 0.5)² < d² < (r + hi + 0.5)²,
 * that is m² − m < d² <= n² + n for m = r − lo, n = r + hi (d² is an integer). */
unsigned
rds_osd_shape_circle (RdsOsdCmd out[2], unsigned xc, unsigned yc, unsigned radius, unsigned circle_width,
    const RdsOsdColor * ring_color, const RdsOsdColor * fill, int32_t width, int32_t height)
{
  int64_t w = MIN2 (circle_width, RDS_OSD_MAX_COORD), lo, hi, m, n;
  unsigned k = 0;

  if (!shape_frame_ok (width, height) || xc > RDS_OSD_MAX_COORD || yc > RDS_OSD_MAX_COORD
      || radius > RDS_OSD_MAX_COORD)
    return 0;
  if (w > 0) {
    lo = w / 2;
    hi = (w + 1) / 2;
  } else {
    lo = hi = (ZERO_CIRCLE_WIDTH - 1) / 2;
  }
  m = (int64_t) radius - lo;
  n = (int64_t) radius + hi;
  if (fill != NULL && m >= 1)
    k += ring (&out[k], xc, yc, 0, m * m - m, m, fill, width, height);
  if (ring_color != NULL)
    k += ring (&out[k], xc, yc, m >= 1 ? m * m - m + 1 : 0, n * n + n, n, ring_color, width, height);
  return k;
}

/* ---- masks ---- */

unsigned
rds_osd_shape_mask (RdsOsdCmd * out, int32_t x, int32_t y, int32_t mask_width, int32_t mask_height, uint32_t stride,
    uint32_t offset, const RdsOsdColor * c, int32_t width, int32_t height)
{
  if (mask_width <= 0 || mask_height <= 0
      || !cmd_init (out, RDS_OSD_CMD_MASK, x, y, (int64_t) x + mask_width - 1, (int64_t) y + mask_height - 1, c,
          width, height))
    return 0;
  out->mask.offset = offset;
  out->mask.stride = stride;
  out->mask.x = x;
  out->mask.y = y;
  return 1;
}

/* ---- the CPU path ---- */

/* Full coverage blends through a table per colour (the same bytes, filled by rds_osd_blend): a frame uses few colours,
 * so a few tables, reused round robin, cover it. */
#define N_LUTS 4

typedef struct
{
  RdsOsdColor color[N_LUTS];
  uint8_t lut[N_LUTS][256][4];
  unsigned n, next;
} LutCache;

static const uint8_t (*lut_for (LutCache * cache, const RdsOsdColor * c))[4]
{
  unsigned k;

  for (k = 0; k < cache->n; k++)
    if (memcmp (&cache->color[k], c, sizeof *c) == 0)
      return (const uint8_t (*)[4]) cache->lut[k];
  if (cache->n < N_LUTS) {
    k = cache->n++;
  } else {
    k = cache->next;
    cache->next = (cache->next + 1) % N_LUTS;
  }
  cache->color[k] = *c;
  for (unsigned d = 0; d < 256; d++) {
    cache->lut[k][d][0] = cache->lut[k][d][1] = cache->lut[k][d][2] = cache->lut[k][d][3] = (uint8_t) d;
    rds_osd_blend (cache->lut[k][d], c, 255);
  }
  return (const uint8_t (*)[4]) cache->lut[k];
}

/* One 32-bit load and store a pixel: byte by byte, GCC -O3 vectorizes the lookups into slow emulated gathers. */
static inline void
lut_blend (uint8_t * px, const uint8_t (*lut)[4])
{
  uint8_t p[4];

  memcpy (p, px, 4);
  p[0] = lut[p[0]][0];
  p[1] = lut[p[1]][1];
  p[2] = lut[p[2]][2];
  p[3] = lut[p[3]][3];
  memcpy (px, p, 4);
}

static __int128
floor_div128 (__int128 a, __int128 b)
{
  __int128 q = a / b;

  return q * b != a && ((a < 0) != (b < 0)) ? q - 1 : q;
}

/* Narrows [*lo, *hi] to the x where l <= a + b·x <= h. */
static void
narrow (int64_t * lo, int64_t * hi, __int128 a, __int128 b, __int128 l, __int128 h)
{
  __int128 from, to;

  if (b == 0) {
    if (a < l || a > h)
      *hi = *lo - 1;
    return;
  }
  if (b > 0) {
    from = -floor_div128 (a - l, b);
    to = floor_div128 (h - a, b);
  } else {
    from = -floor_div128 (h - a, -b);
    to = floor_div128 (a - l, -b);
  }
  if (from > *lo)
    *lo = from > *hi ? *hi + 1 : (int64_t) from;
  if (to < *hi)
    *hi = to < *lo ? *lo - 1 : (int64_t) to;
}

/* Where in row y the command can cover pixels (up to two spans in out[], within its box, a pixel of slack on each
 * side): the CPU path tests only these pixels of long segments and large rings, whose boxes are mostly empty. */
static unsigned
row_spans (const RdsOsdCmd * c, int32_t y, int64_t out[2][2])
{
  int64_t lo = c->x0, hi = c->x1;

  if (c->type == RDS_OSD_CMD_SEGMENT) {
    const RdsOsdSegment *s = &c->segment;
    __int128 py = (__int128) y * RDS_OSD_FIXED_ONE + RDS_OSD_FIXED_ONE / 2 - s->ay;
    __int128 px0 = RDS_OSD_FIXED_ONE / 2 - (__int128) s->ax;

    narrow (&lo, &hi, s->dx * py - s->dy * px0, -(__int128) s->dy * RDS_OSD_FIXED_ONE, -s->t, s->t);
    narrow (&lo, &hi, s->dx * px0 + s->dy * py, (__int128) s->dx * RDS_OSD_FIXED_ONE, s->dot_lo, s->dot_hi);
    out[0][0] = MAX2 (lo - 1, (int64_t) c->x0);
    out[0][1] = MIN2 (hi + 1, (int64_t) c->x1);
    return lo <= hi;
  }
  if (c->type == RDS_OSD_CMD_RING) {
    const RdsOsdRing *r = &c->ring;
    int64_t dy = (int64_t) y - r->yc, rest_hi = r->d2_hi - dy * dy, rest_lo = r->d2_lo - dy * dy, outer, inner;

    if (rest_hi < 0)
      return 0;
    outer = (int64_t) isqrt128 ((unsigned __int128) rest_hi) + 1;
    /* pixels with dx² < rest_lo are in the hole: |dx| <= isqrt (rest_lo − 1) */
    inner = rest_lo > 0 ? (int64_t) isqrt128 ((unsigned __int128) (rest_lo - 1)) : -1;
    if (inner < 1) {
      out[0][0] = MAX2 (r->xc - outer, (int64_t) c->x0);
      out[0][1] = MIN2 (r->xc + outer, (int64_t) c->x1);
      return 1;
    }
    out[0][0] = MAX2 (r->xc - outer, (int64_t) c->x0);
    out[0][1] = MIN2 (r->xc - inner, (int64_t) c->x1);
    out[1][0] = MAX2 (r->xc + inner, (int64_t) c->x0);
    out[1][1] = MIN2 (r->xc + outer, (int64_t) c->x1);
    return 2;
  }
  out[0][0] = lo;
  out[0][1] = hi;
  return 1;
}

void
rds_osd_shade_commands (const RdsOsdCmd * cmds, uint32_t n, const uint8_t * masks, uint8_t * data, int32_t width,
    int32_t height, size_t stride)
{
  LutCache cache = {.n = 0 };

  /* the list never ends inside a shape (rds_osd_shade_pixel); if it does, its last command ends the shape there too */
  assert (n == 0 || !(cmds[n - 1].flags & RDS_OSD_CMD_UNION_NEXT));
  for (uint32_t i = 0, j; i < n; i = j + 1) {
    int32_t x0, y0, x1, y1;
    const uint8_t (*lut)[4] = NULL;

    /* one shape: cmds[i .. j] */
    for (j = i; j + 1 < n && (cmds[j].flags & RDS_OSD_CMD_UNION_NEXT); j++);
    x0 = cmds[i].x0;
    y0 = cmds[i].y0;
    x1 = cmds[i].x1;
    y1 = cmds[i].y1;
    for (uint32_t k = i + 1; k <= j; k++) {
      x0 = MIN2 (x0, cmds[k].x0);
      y0 = MIN2 (y0, cmds[k].y0);
      x1 = MAX2 (x1, cmds[k].x1);
      y1 = MAX2 (y1, cmds[k].y1);
    }
    x0 = MAX2 (x0, 0);
    y0 = MAX2 (y0, 0);
    x1 = MIN2 (x1, width - 1);
    y1 = MIN2 (y1, height - 1);
    if (x0 > x1 || y0 > y1)
      continue;
    if (i == j && (cmds[i].type == RDS_OSD_CMD_BOX || cmds[i].type == RDS_OSD_CMD_MASK))
      lut = lut_for (&cache, &cmds[i].color);

    for (int32_t y = y0; y <= y1; y++) {
      uint8_t *px = data + (size_t) y * stride + 4 * (size_t) x0;

      if (i == j && cmds[i].type == RDS_OSD_CMD_BOX) {
        for (int32_t x = x0; x <= x1; x++, px += 4)
          lut_blend (px, lut);
        continue;
      }
      if (i == j && cmds[i].type == RDS_OSD_CMD_MASK) {
        /* a mask: its row of coverage */
        const RdsOsdMask *m = &cmds[i].mask;
        const uint8_t *cov = masks + m->offset + (size_t) (y - m->y) * m->stride + (size_t) (x0 - m->x);

        for (int32_t x = x0; x <= x1; x++, px += 4, cov++)
          if (*cov == 255)
            lut_blend (px, lut);
          else if (*cov != 0)
            rds_osd_blend (px, &cmds[i].color, *cov);
        continue;
      }
      if (i == j) {
        int64_t spans[2][2];
        unsigned n_spans = row_spans (&cmds[i], y, spans);

        for (unsigned k = 0; k < n_spans; k++)
          for (int64_t x = spans[k][0]; x <= spans[k][1]; x++) {
            uint8_t coverage = rds_osd_cmd_coverage (&cmds[i], (int32_t) x, y, masks);

            if (coverage != 0)
              rds_osd_blend (data + (size_t) y * stride + 4 * (size_t) x, &cmds[i].color, coverage);
          }
        continue;
      }
      for (int32_t x = x0; x <= x1; x++, px += 4) {
        uint8_t coverage = 0;

        for (uint32_t k = i; k <= j; k++) {
          uint8_t v = rds_osd_cmd_coverage (&cmds[k], x, y, masks);

          coverage = MAX2 (coverage, v);
        }
        if (coverage != 0)
          rds_osd_blend (px, &cmds[j].color, coverage);
      }
    }
  }
}
