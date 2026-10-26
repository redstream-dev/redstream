/* Redstream: rdsosd's shapes as draw commands, and the per-pixel coverage and blend that turn them into pixels.
 * The same code runs on the CPU (rds_osd_shade_commands) and, compiled as HIP, on the GPU: plain C, fixed-size
 * commands without pointers (a text mask is an offset into a mask buffer), and integer coverage tests, so both
 * paths draw the same bytes.
 *
 * Coverage: pixel (x, y) is covered when its centre (x + 0.5, y + 0.5) is inside the shape, as Cairo draws with
 * antialiasing off. Every test is exact integer arithmetic, and a centre exactly on an edge counts as inside. Boxes
 * are inclusive pixel boxes. Segments and triangles are in 24.8 fixed point (Cairo's own precision), so shapes
 * whose corners are not on the grid (arrow heads) are rounded as Cairo rounds them; with integer endpoints and the
 * odd stroke widths drawn, no pixel centre ever falls on a segment's edge. Rings compare squared distances between
 * pixel centres, whose band edges lie half a pixel off the integer distances: no ties either.
 *
 * Commands are drawn in order, each blended once into the pixels it covers. Commands flagged
 * RDS_OSD_CMD_UNION_NEXT form one shape with the commands after them, up to the first without the flag: their
 * coverage is united and blended once, in the last one's colour (an arrow head: two wings and their bevel join).
 *
 * Blend (DeepStream 8.0 behaviour): out = a·src + (1 − a)·dst per channel, truncated, with a = alpha × coverage /
 * 255; the alpha byte blends the same way with src 255. In double precision with no fused multiply-add (contraction
 * is off here and in the build), so the CPU and the GPU round alike; an opaque colour with whole channel values
 * blends in integers instead, which gives the same bytes (see rds_osd_blend).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_OSD_SHAPES_H
#define RDS_OSD_SHAPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __HIPCC__
#define RDS_OSD_FN __host__ __device__ static inline
#else
#define RDS_OSD_FN static inline
#endif

#ifdef __clang__
#define RDS_OSD_NO_CONTRACT _Pragma ("clang fp contract(off)")
#else
#define RDS_OSD_NO_CONTRACT     /* GCC ignores the pragma: whoever includes this header builds with -ffp-contract=off
                                 * (rdsosd_core_dep passes it on) */
#endif

#ifdef __cplusplus
#define RDS_OSD_STATIC_ASSERT(e, msg) static_assert (e, msg)
#else
#define RDS_OSD_STATIC_ASSERT(e, msg) _Static_assert (e, msg)
#endif

/* Circles and arrow heads with a coordinate, radius or width beyond this are not drawn (ours); widths are taken as
 * this at most. Lines, arrows and circles are drawn on frames up to RDS_OSD_MAX_SHAPE_FRAME pixels a side (ours):
 * with both limits every fixed-point product fits in 64 bits. */
#define RDS_OSD_MAX_COORD 2097152
#define RDS_OSD_MAX_SHAPE_FRAME 1048576
/* segment and triangle coordinates: 24.8 fixed point, the pixel centre of x is x * 256 + 128 */
#define RDS_OSD_FIXED_ONE 256
/* added before truncating a blend, so that a sum that is exact in decimal (127.5 + 127.5) is not lost to binary
 * rounding (254.9999...) */
#define RDS_OSD_BLEND_EPSILON 1e-6

typedef enum
{
  RDS_OSD_CMD_BOX = 1,          /* the bounding box itself */
  RDS_OSD_CMD_SEGMENT,
  RDS_OSD_CMD_TRIANGLE,
  RDS_OSD_CMD_RING,
  RDS_OSD_CMD_MASK,
} RdsOsdCmdType;

enum
{
  RDS_OSD_CMD_UNION_NEXT = 1 << 0,
  RDS_OSD_CMD_WHOLE_COLOR = 1 << 1,     /* the colour is opaque with whole channel values (rds_osd_color_is_whole) */
};

/* A colour ready to blend: the channel values (0..255) and an alpha in (0, 1]. */
typedef struct
{
  double value[3];
  double alpha;
} RdsOsdColor;

/* A rectangle along a segment from a = (ax, ay) by d = (dx, dy), fixed point: with p the pixel centre minus a,
 * covered when |d × p| <= t (t = half width × |d|, rounded down) and dot_lo <= d · p <= dot_hi (butt ends: 0 and
 * |d|²; square caps: −t and |d|² + t). */
typedef struct
{
  int32_t ax, ay, dx, dy;
  int64_t t, dot_lo, dot_hi;
} RdsOsdSegment;

/* A triangle, fixed point, either orientation; its edges count as inside. */
typedef struct
{
  int32_t x[3], y[3];
} RdsOsdTriangle;

/* The pixels whose squared distance from the pixel (xc, yc), centre to centre, is in [d2_lo, d2_hi]. */
typedef struct
{
  int32_t xc, yc;
  int64_t d2_lo, d2_hi;
} RdsOsdRing;

/* An A8 coverage mask whose pixel (0, 0) lies on the frame pixel (x, y), at @offset in the mask buffer. */
typedef struct
{
  uint32_t offset, stride;
  int32_t x, y;
} RdsOsdMask;

/* One draw command: 96 bytes, the same layout on the host and the device. */
typedef struct
{
  uint8_t type;                 /* RdsOsdCmdType */
  uint8_t flags;
  uint16_t reserved0;
  int32_t x0, y0, x1, y1;       /* bounding box, inclusive, inside the frame: nothing outside it is covered */
  uint32_t reserved1;
  RdsOsdColor color;
  union
  {
    RdsOsdSegment segment;
    RdsOsdTriangle triangle;
    RdsOsdRing ring;
    RdsOsdMask mask;
  };
} RdsOsdCmd;

RDS_OSD_STATIC_ASSERT (sizeof (RdsOsdColor) == 32, "RdsOsdColor layout");
RDS_OSD_STATIC_ASSERT (sizeof (RdsOsdSegment) == 40, "RdsOsdSegment layout");
RDS_OSD_STATIC_ASSERT (sizeof (RdsOsdTriangle) == 24, "RdsOsdTriangle layout");
RDS_OSD_STATIC_ASSERT (sizeof (RdsOsdRing) == 24, "RdsOsdRing layout");
RDS_OSD_STATIC_ASSERT (sizeof (RdsOsdMask) == 16, "RdsOsdMask layout");
RDS_OSD_STATIC_ASSERT (sizeof (RdsOsdCmd) == 96, "RdsOsdCmd layout");
RDS_OSD_STATIC_ASSERT (offsetof (RdsOsdCmd, x0) == 4, "RdsOsdCmd layout");
RDS_OSD_STATIC_ASSERT (offsetof (RdsOsdCmd, color) == 24, "RdsOsdCmd layout");
RDS_OSD_STATIC_ASSERT (offsetof (RdsOsdCmd, segment) == 56, "RdsOsdCmd layout");
RDS_OSD_STATIC_ASSERT (offsetof (RdsOsdSegment, t) == 16, "RdsOsdSegment layout");
RDS_OSD_STATIC_ASSERT (offsetof (RdsOsdRing, d2_lo) == 8, "RdsOsdRing layout");

/* The command's coverage of pixel (x, y): 0 (not covered) .. 255. @masks: the mask buffer (MASK commands only). */
RDS_OSD_FN uint8_t
rds_osd_cmd_coverage (const RdsOsdCmd * c, int32_t x, int32_t y, const uint8_t * masks)
{
  if (x < c->x0 || x > c->x1 || y < c->y0 || y > c->y1)
    return 0;
  switch (c->type) {
    case RDS_OSD_CMD_BOX:
      return 255;
    case RDS_OSD_CMD_SEGMENT:{
      const RdsOsdSegment *s = &c->segment;
      /* Within RDS_OSD_MAX_SHAPE_FRAME and the builders' clipping, |px|, |py| < 2^30 (~1.07e9) and |dx|, |dy| < 1.4e9:
       * px and py fit in 32 bits (so the products are 32 × 32 → 64-bit multiplies, cheaper on the GPU), each product
       * stays below 1.5e18 and their sum and difference below 3e18, inside int64. */
      int32_t px = x * RDS_OSD_FIXED_ONE + RDS_OSD_FIXED_ONE / 2 - s->ax;
      int32_t py = y * RDS_OSD_FIXED_ONE + RDS_OSD_FIXED_ONE / 2 - s->ay;
      int64_t cross = (int64_t) s->dx * py - (int64_t) s->dy * px;
      int64_t dot = (int64_t) s->dx * px + (int64_t) s->dy * py;

      return cross >= -s->t && cross <= s->t && dot >= s->dot_lo && dot <= s->dot_hi ? 255 : 0;
    }
    case RDS_OSD_CMD_TRIANGLE:{
      const RdsOsdTriangle *t = &c->triangle;
      /* arrow heads: corners within RDS_OSD_MAX_COORD plus a stroke of up to 2^21 + 1 px, so below 2^29 + 2^28 in
       * fixed point, and pixel centres below 2^28: every difference fits in 32 bits, every product in 60 */
      int32_t px = x * RDS_OSD_FIXED_ONE + RDS_OSD_FIXED_ONE / 2;
      int32_t py = y * RDS_OSD_FIXED_ONE + RDS_OSD_FIXED_ONE / 2;
      int neg = 0, pos = 0;

      for (int i = 0; i < 3; i++) {
        int j = i == 2 ? 0 : i + 1;
        int64_t e = (int64_t) (t->x[j] - t->x[i]) * (py - t->y[i]) - (int64_t) (t->y[j] - t->y[i]) * (px - t->x[i]);

        neg |= e < 0;
        pos |= e > 0;
      }
      return neg && pos ? 0 : 255;
    }
    case RDS_OSD_CMD_RING:{
      const RdsOsdRing *r = &c->ring;
      /* centres within RDS_OSD_MAX_COORD: the differences fit in 32 bits */
      int32_t dx = x - r->xc, dy = y - r->yc;
      int64_t d2 = (int64_t) dx * dx + (int64_t) dy * dy;

      return d2 >= r->d2_lo && d2 <= r->d2_hi ? 255 : 0;
    }
    case RDS_OSD_CMD_MASK:{
      const RdsOsdMask *m = &c->mask;

      return masks[(size_t) m->offset + (size_t) (y - m->y) * m->stride + (size_t) (x - m->x)];
    }
    default:
      return 0;
  }
}

/* Whether @color is opaque with whole channel values (as colour params that are multiples of 1/255 give). */
RDS_OSD_FN int
rds_osd_color_is_whole (const RdsOsdColor * color)
{
  for (int i = 0; i < 3; i++)
    if (color->value[i] != (double) (int) color->value[i])
      return 0;
  return color->alpha == 1.0;
}

/* Blends @color into the RGBA pixel @px at @coverage (1..255); @whole: rds_osd_color_is_whole (@color), else 0.
 *
 * A whole colour blends in integers: out = ⌊(c·s + (255 − c)·d) / 255⌋ for coverage c, source s and destination d,
 * the same bytes as the double formula. With alpha 1, that formula computes k / 255 for the integer k = c·s +
 * (255 − c)·d, within 1e-13 (a handful of roundings of values below 2^9). If k / 255 is a whole number, the
 * epsilon (1e-6) lifts any rounding below it back over; otherwise its fraction is between 1/255 and 254/255, and
 * neither the error nor the epsilon reaches a whole number. k / 255 <= 255, so the clamp never differs either. The
 * osd_shapes blend test checks every s, c and d. */
RDS_OSD_FN void
rds_osd_blend (uint8_t * px, const RdsOsdColor * color, uint8_t coverage, int whole)
{
  RDS_OSD_NO_CONTRACT;
  double a;

  if (whole) {
    for (int i = 0; i < 4; i++) {
      uint32_t src = i < 3 ? (uint32_t) color->value[i] : 255;

      px[i] = (uint8_t) ((coverage * src + (255u - coverage) * px[i]) / 255u);
    }
    return;
  }
  a = coverage == 255 ? color->alpha : color->alpha * coverage / 255.0;
  for (int i = 0; i < 4; i++) {
    double v = a * (i < 3 ? color->value[i] : 255.0) + (1.0 - a) * px[i] + RDS_OSD_BLEND_EPSILON;

    px[i] = v < 255.0 ? (uint8_t) v : 255;
  }
}

/* Shades the RGBA pixel @rgba at (x, y) with the commands @list[0 .. n−1] (indices into @cmds; NULL: @cmds[0 .. n−1]),
 * in order. A shape's commands (RDS_OSD_CMD_UNION_NEXT) must be consecutive and complete in the list: a list that
 * holds any command of a shape holds all of them, and never ends on a command flagged RDS_OSD_CMD_UNION_NEXT. (The
 * last command of a list ends its shape whatever its flag, here and in rds_osd_shade_commands.) */
RDS_OSD_FN void
rds_osd_shade_pixel (const RdsOsdCmd * cmds, const uint32_t * list, uint32_t n, int32_t x, int32_t y, uint8_t * rgba,
    const uint8_t * masks)
{
  uint8_t coverage = 0;

  for (uint32_t i = 0; i < n; i++) {
    const RdsOsdCmd *c = &cmds[list != NULL ? list[i] : i];
    uint8_t v = rds_osd_cmd_coverage (c, x, y, masks);

    if (v > coverage)
      coverage = v;
    if ((c->flags & RDS_OSD_CMD_UNION_NEXT) && i + 1 < n)
      continue;
    if (coverage != 0)
      rds_osd_blend (rgba, &c->color, coverage, c->flags & RDS_OSD_CMD_WHOLE_COLOR);
    coverage = 0;
  }
}

#ifndef __HIP_DEVICE_COMPILE__

#ifdef __cplusplus
extern "C"
{
#endif

/* ---- building commands (host): each returns the number of commands written, 0 when nothing would be drawn ---- */

/* @c from colour params (0..1, clamped); 0 when fully transparent. */
unsigned rds_osd_color_init (RdsOsdColor * c, double red, double green, double blue, double alpha);

/* The box x0..x1 × y0..y1 (inclusive), clipped to the @width × @height frame. 1 command. */
unsigned rds_osd_shape_box (RdsOsdCmd * out, int64_t x0, int64_t y0, int64_t x1, int64_t y1, const RdsOsdColor * c,
    int32_t width, int32_t height);

/* The border bands of a rectangle with edges L, T, R, B and border width @bw (DeepStream 8.0 GPU-mode geometry):
 * with lo = (bw − 1) / 2, hi = bw / 2, the left and top bands are [L − lo, L + hi], the right and bottom bands
 * [R − hi, R + lo]. Up to 4 disjoint boxes, so every pixel is blended once. */
unsigned rds_osd_shape_rect_border (RdsOsdCmd out[4], int64_t left, int64_t top, int64_t right, int64_t bottom,
    unsigned bw, const RdsOsdColor * c, int32_t width, int32_t height);

/* A line between the pixels (x1, y1) and (x2, y2) (their centres), square caps: DeepStream 8.0 GPU mode draws
 * (w − 1) / 2 pixels on each side of the segment (width 6 is 5 pixels wide) and extends the ends as far. Widths
 * above twice the frame's larger side are clamped to it (ours: the stroke covers the frame anyway). 1 command. */
unsigned rds_osd_shape_line (RdsOsdCmd * out, double x1, double y1, double x2, double y2, unsigned line_width,
    const RdsOsdColor * c, int32_t width, int32_t height);

/* An arrow head at the tip (tx, ty) pointing away from (fx, fy): an open V whose wings end 22 px back and 7 px to
 * each side (measured on DeepStream 8.0's GPU mode), stroked like a line of @line_width with butt ends and a bevel
 * join. 3 commands (wing, wing, join) blended as one shape. */
unsigned rds_osd_shape_arrow_head (RdsOsdCmd out[3], double tx, double ty, double fx, double fy, unsigned line_width,
    const RdsOsdColor * c, int32_t width, int32_t height);

/* A circle (DeepStream 8.0 behaviour, GPU mode): circle_width w covers the pixels whose distance from the centre
 * pixel is in [r − ⌊w/2⌋, r + ⌈w/2⌉] (w 3, r 60: 59..62); w 0 draws a 3 px ring (59..61). @fill (NULL: none)
 * fills inside the ring first. Up to 2 commands (fill, ring), each blended on its own. */
unsigned rds_osd_shape_circle (RdsOsdCmd out[2], unsigned xc, unsigned yc, unsigned radius, unsigned circle_width,
    const RdsOsdColor * ring, const RdsOsdColor * fill, int32_t width, int32_t height);

/* A mask of @mask_width × @mask_height at the frame pixel (x, y), at @offset (@stride bytes a row) in the mask
 * buffer; the part outside the frame is not drawn. 1 command. */
unsigned rds_osd_shape_mask (RdsOsdCmd * out, int32_t x, int32_t y, int32_t mask_width, int32_t mask_height,
    uint32_t stride, uint32_t offset, const RdsOsdColor * c, int32_t width, int32_t height);

/* Draws @cmds in order onto the RGBA plane @data (@width × @height pixels, @stride bytes a row), each shape over its
 * bounding box: the same pixels as rds_osd_shade_pixel over the whole frame. */
void rds_osd_shade_commands (const RdsOsdCmd * cmds, uint32_t n, const uint8_t * masks, uint8_t * data, int32_t width,
    int32_t height, size_t stride);

#ifdef __cplusplus
}
#endif

#endif /* __HIP_DEVICE_COMPILE__ */

#endif /* RDS_OSD_SHAPES_H */
