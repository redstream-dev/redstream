/* Redstream: rdsosd's drawing core (see rds-osd-draw.h).
 *
 * Pixels: the frame is a plain RGBA plane, while Cairo's colour formats are premultiplied native-endian ARGB (BGRA
 * in memory here). Rather than swap and premultiply the whole frame around every draw, rectangles and text
 * backgrounds are filled by pixel loops, and every Cairo or Pango shape is drawn into a scratch A8 surface (a
 * coverage mask the size of the shape's box, clipped to the frame) that is then blended into the frame in the
 * shape's colour. So the frame's own bytes, alpha included, are only ever read and blended, never reinterpreted, and
 * only the pixels a shape covers are touched. Blending is out = a·src + (1 − a)·dst per channel, truncated
 * (DeepStream 8.0 behaviour: red at alpha 0.5 over 128 gives 191), with a = colour alpha × coverage; the alpha byte
 * blends the same way with src 255. Shapes are drawn with antialiasing off (hard edges, as DeepStream 8.0's GPU mode
 * draws them); text keeps greyscale antialiasing.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-draw.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

/* Text is rendered at 96 dpi: font_size is in points (DeepStream 8.0 behaviour: size 20 has 20 px capitals). */
#define TEXT_DPI 96.0
/* font sizes above this are drawn this size (ours: keeps Pango's fixed-point sizes in range) */
#define MAX_FONT_SIZE 1024
/* arrow heads: an open V, its wings this far back from the tip and this far to each side (measured on DeepStream
 * 8.0's GPU mode output at arrow widths 2 and 4) */
#define ARROW_HEAD_LENGTH 22.0
#define ARROW_HEAD_HALF_WIDTH 7.0
/* the ring of a circle whose circle_width is 0, in pixels (DeepStream 8.0 still outlines it, 3 px in GPU mode) */
#define ZERO_CIRCLE_WIDTH 3
/* shapes further than this outside the frame are not drawn; coordinates beyond it would overflow Cairo's fixed
 * point (ours: such shapes cannot touch the frame, or are degenerate) */
#define MAX_COORD 4194304.0
/* added before truncating a blend, so that a sum that is exact in decimal (127.5 + 127.5) is not lost to binary
 * rounding (254.9999...) */
#define BLEND_EPSILON 1e-6

struct _RdsOsdDraw
{
  PangoFontMap *font_map;
  PangoContext *context;
  PangoLayout *layout;
  GHashTable *fonts;            /* "name\x1fsize" -> PangoFontDescription */
  guint8 *scratch;              /* the A8 coverage of the shape being drawn */
  gsize scratch_size;
};

typedef struct
{
  guint8 *data;
  gint width, height, stride;
} Plane;

/* A colour ready to blend: the channel values (0..255, alpha 255), its alpha, and the blend of every destination
 * byte at full coverage. */
typedef struct
{
  gdouble value[4];
  gdouble alpha;
  guint8 lut[4][256];
} Paint;

static gdouble
clamp01 (gdouble v)
{
  return v > 0 ? (v < 1 ? v : 1) : 0;
}

/* FALSE when the colour is fully transparent (nothing to draw). */
static gboolean
paint_init (Paint * p, const RdsOSD_ColorParams * c)
{
  p->value[0] = clamp01 (c->red) * 255.0;
  p->value[1] = clamp01 (c->green) * 255.0;
  p->value[2] = clamp01 (c->blue) * 255.0;
  p->value[3] = 255.0;
  p->alpha = clamp01 (c->alpha);
  if (p->alpha <= 0)
    return FALSE;
  for (guint i = 0; i < 4; i++)
    for (guint d = 0; d < 256; d++)
      p->lut[i][d] = (guint8) MIN (p->alpha * p->value[i] + (1.0 - p->alpha) * d + BLEND_EPSILON, 255.0);
  return TRUE;
}

static inline void
blend_full (guint8 * px, const Paint * p)
{
  px[0] = p->lut[0][px[0]];
  px[1] = p->lut[1][px[1]];
  px[2] = p->lut[2][px[2]];
  px[3] = p->lut[3][px[3]];
}

static inline void
blend_coverage (guint8 * px, const Paint * p, guint8 coverage)
{
  gdouble a;

  if (coverage == 255) {
    blend_full (px, p);
    return;
  }
  a = p->alpha * coverage / 255.0;
  for (guint i = 0; i < 4; i++)
    px[i] = (guint8) MIN (a * p->value[i] + (1.0 - a) * px[i] + BLEND_EPSILON, 255.0);
}

/* Blends the pixels x0..x1 (inclusive) of row y, clipped to the plane. */
static void
span (const Plane * pl, gint64 y, gint64 x0, gint64 x1, const Paint * p)
{
  guint8 *px;

  if (y < 0 || y >= pl->height)
    return;
  x0 = MAX (x0, 0);
  x1 = MIN (x1, (gint64) pl->width - 1);
  px = pl->data + (gsize) y * pl->stride + 4 * x0;
  for (gint64 x = x0; x <= x1; x++, px += 4)
    blend_full (px, p);
}

/* Blends the box x0..x1 × y0..y1 (inclusive), clipped to the plane. */
static void
box (const Plane * pl, gint64 x0, gint64 y0, gint64 x1, gint64 y1, const Paint * p)
{
  for (gint64 y = MAX (y0, 0); y <= MIN (y1, (gint64) pl->height - 1); y++)
    span (pl, y, x0, x1, p);
}

/* ---- rectangles ---- */

/* A rect coordinate as a pixel: truncated (DeepStream 8.0 behaviour), saturated far outside any frame. Not NaN. */
static gint64
to_px (gfloat v)
{
  if (v > 1e15f)
    return (gint64) 1e15;
  if (v < -1e15f)
    return (gint64) - 1e15;
  return (gint64) v;
}

/* DeepStream 8.0 GPU-mode geometry. With L = left, R = L + width (each field truncated), and lo = (bw − 1) / 2,
 * hi = bw / 2: the left and top bands are [L − lo, L + hi], the right and bottom bands [R − hi, R + lo]. A fill
 * covers only the inside, strictly within the bands: [L + hi + 1, R − hi − 1] (bw 0: [L + 1, R − 1]). Clipped to the
 * frame, never moved into it. Every pixel is blended once, so semi-transparent borders have even corners. */
static void
draw_rect (const Plane * pl, const RdsOSD_RectParams * r, RdsOsdDrawFlags * flags)
{
  gint64 L, T, R, B, bw = r->border_width, lo, hi;
  Paint p;

  /* ours: a rect with a NaN field is not drawn */
  if (isnan (r->left) || isnan (r->top) || isnan (r->width) || isnan (r->height))
    return;
  L = to_px (r->left);
  T = to_px (r->top);
  R = L + to_px (r->width);
  B = T + to_px (r->height);
  if (bw > RDS_OSD_MAX_BORDER_WIDTH) {
    /* ours: DeepStream 8.0 stops the pipeline */
    bw = RDS_OSD_MAX_BORDER_WIDTH;
    *flags |= RDS_OSD_DRAW_BORDER_CLAMPED;
  }
  lo = bw > 0 ? (bw - 1) / 2 : 0;
  hi = bw / 2;

  if (r->has_bg_color && paint_init (&p, &r->bg_color))
    box (pl, L + hi + 1, T + hi + 1, R - hi - 1, B - hi - 1, &p);

  if (bw == 0 || !paint_init (&p, &r->border_color))
    return;
  for (gint64 y = MAX (T - lo, 0); y <= MIN (B + lo, (gint64) pl->height - 1); y++) {
    if (y <= T + hi || y >= B - hi) {
      span (pl, y, L - lo, R + lo, &p);
    } else if (L + hi + 1 >= R - hi) {
      /* the side bands meet */
      span (pl, y, L - lo, R + lo, &p);
    } else {
      span (pl, y, L - lo, L + hi, &p);
      span (pl, y, R - hi, R + lo, &p);
    }
  }
}

/* ---- Cairo shapes through a coverage mask ---- */

typedef struct
{
  cairo_surface_t *surface;
  cairo_t *cr;
  gint x0, y0, width, height, stride;
} Mask;

/* Starts a mask over the frame pixels x0..x1 × y0..y1 (inclusive, clipped to the frame), with the Cairo context in
 * frame coordinates. FALSE when nothing of it is in the frame. */
static gboolean
mask_begin (RdsOsdDraw * d, const Plane * pl, gdouble x0, gdouble y0, gdouble x1, gdouble y1, Mask * m)
{
  gsize size;

  x0 = floor (MAX (x0, 0));
  y0 = floor (MAX (y0, 0));
  x1 = ceil (MIN (x1, pl->width - 1));
  y1 = ceil (MIN (y1, pl->height - 1));
  if (!(x0 <= x1 && y0 <= y1))
    return FALSE;
  m->x0 = (gint) x0;
  m->y0 = (gint) y0;
  m->width = (gint) x1 - m->x0 + 1;
  m->height = (gint) y1 - m->y0 + 1;
  m->stride = cairo_format_stride_for_width (CAIRO_FORMAT_A8, m->width);
  size = (gsize) m->stride * m->height;
  if (size > d->scratch_size) {
    g_free (d->scratch);
    d->scratch = g_malloc (size);
    d->scratch_size = size;
  }
  memset (d->scratch, 0, size);
  m->surface = cairo_image_surface_create_for_data (d->scratch, CAIRO_FORMAT_A8, m->width, m->height, m->stride);
  m->cr = cairo_create (m->surface);
  cairo_translate (m->cr, -m->x0, -m->y0);
  cairo_set_antialias (m->cr, CAIRO_ANTIALIAS_NONE);
  return TRUE;
}

/* Blends the mask into the frame in @p, and frees the Cairo objects. */
static void
mask_end (RdsOsdDraw * d, const Plane * pl, Mask * m, const Paint * p)
{
  cairo_surface_flush (m->surface);
  for (gint y = 0; y < m->height; y++) {
    const guint8 *cov = d->scratch + (gsize) y * m->stride;
    guint8 *px = pl->data + (gsize) (m->y0 + y) * pl->stride + 4 * m->x0;

    for (gint x = 0; x < m->width; x++, px += 4)
      if (cov[x] != 0)
        blend_coverage (px, p, cov[x]);
  }
  cairo_destroy (m->cr);
  cairo_surface_destroy (m->surface);
}

/* Lines and arrow shafts: DeepStream 8.0 GPU mode draws (w − 1) / 2 pixels on each side of the segment (width 6 is
 * 5 pixels wide) and extends the ends as far. */
static gdouble
stroke_width (guint w)
{
  return 2.0 * (w > 0 ? (w - 1) / 2 : 0) + 1.0;
}

/* Clips the segment to the box [lo, hi]² (Liang-Barsky); FALSE when it misses. */
static gboolean
clip_segment (gdouble * x1, gdouble * y1, gdouble * x2, gdouble * y2, gdouble xlo, gdouble ylo, gdouble xhi,
    gdouble yhi)
{
  gdouble dx = *x2 - *x1, dy = *y2 - *y1, t0 = 0, t1 = 1;
  gdouble p[4] = { -dx, dx, -dy, dy }, q[4] = { *x1 - xlo, xhi - *x1, *y1 - ylo, yhi - *y1 };

  for (guint i = 0; i < 4; i++) {
    if (p[i] == 0) {
      if (q[i] < 0)
        return FALSE;
      continue;
    }
    gdouble t = q[i] / p[i];

    if (p[i] < 0)
      t0 = MAX (t0, t);
    else
      t1 = MIN (t1, t);
  }
  if (t0 > t1)
    return FALSE;
  *x2 = *x1 + t1 * dx;
  *y2 = *y1 + t1 * dy;
  *x1 = *x1 + t0 * dx;
  *y1 = *y1 + t0 * dy;
  return TRUE;
}

/* A segment between pixel centres, square caps; clipped first to a margin around the frame. */
static void
draw_segment (RdsOsdDraw * d, const Plane * pl, gdouble x1, gdouble y1, gdouble x2, gdouble y2, guint width,
    const RdsOSD_ColorParams * color)
{
  gdouble w = stroke_width (width), margin = w + 2;
  Paint p;
  Mask m;

  if (!paint_init (&p, color)
      || !clip_segment (&x1, &y1, &x2, &y2, -margin, -margin, pl->width + margin, pl->height + margin))
    return;
  if (!mask_begin (d, pl, MIN (x1, x2) - margin, MIN (y1, y2) - margin, MAX (x1, x2) + margin,
          MAX (y1, y2) + margin, &m))
    return;
  cairo_set_line_width (m.cr, w);
  cairo_set_line_cap (m.cr, CAIRO_LINE_CAP_SQUARE);
  cairo_move_to (m.cr, x1 + 0.5, y1 + 0.5);
  cairo_line_to (m.cr, x2 + 0.5, y2 + 0.5);
  cairo_stroke (m.cr);
  mask_end (d, pl, &m, &p);
}

static void
draw_line (RdsOsdDraw * d, const Plane * pl, const RdsOSD_LineParams * l)
{
  draw_segment (d, pl, l->x1, l->y1, l->x2, l->y2, l->line_width, &l->line_color);
}

/* An open V at @tip pointing away from @from. */
static void
draw_arrow_head (RdsOsdDraw * d, const Plane * pl, gdouble tx, gdouble ty, gdouble fx, gdouble fy, guint width,
    const Paint * p)
{
  gdouble len = hypot (tx - fx, ty - fy), ux, uy, w = stroke_width (width), margin = w + 2;
  gdouble ax, ay, bx, by;
  Mask m;

  if (len == 0)
    return;
  ux = (tx - fx) / len;
  uy = (ty - fy) / len;
  ax = tx - ARROW_HEAD_LENGTH * ux - ARROW_HEAD_HALF_WIDTH * uy;
  ay = ty - ARROW_HEAD_LENGTH * uy + ARROW_HEAD_HALF_WIDTH * ux;
  bx = tx - ARROW_HEAD_LENGTH * ux + ARROW_HEAD_HALF_WIDTH * uy;
  by = ty - ARROW_HEAD_LENGTH * uy - ARROW_HEAD_HALF_WIDTH * ux;
  if (!mask_begin (d, pl, MIN (tx, MIN (ax, bx)) - margin, MIN (ty, MIN (ay, by)) - margin,
          MAX (tx, MAX (ax, bx)) + margin, MAX (ty, MAX (ay, by)) + margin, &m))
    return;
  cairo_set_line_width (m.cr, w);
  cairo_set_line_join (m.cr, CAIRO_LINE_JOIN_BEVEL);
  cairo_move_to (m.cr, ax + 0.5, ay + 0.5);
  cairo_line_to (m.cr, tx + 0.5, ty + 0.5);
  cairo_line_to (m.cr, bx + 0.5, by + 0.5);
  cairo_stroke (m.cr);
  mask_end (d, pl, &m, p);
}

/* The shaft like a line, a head at (x1, y1) for START, at (x2, y2) for END (DeepStream 8.0 behaviour), at both ends
 * for BOTH (ours: DeepStream 8.0 draws one head or nothing). Other head values draw the shaft only (ours). */
static void
draw_arrow (RdsOsdDraw * d, const Plane * pl, const RdsOSD_ArrowParams * a)
{
  gdouble x1 = a->x1, y1 = a->y1, x2 = a->x2, y2 = a->y2;
  Paint p;

  if (!paint_init (&p, &a->arrow_color))
    return;
  draw_segment (d, pl, x1, y1, x2, y2, a->arrow_width, &a->arrow_color);
  /* a head is drawn only near the frame (and its direction needs sane coordinates) */
  if (MAX (MAX (x1, x2), MAX (y1, y2)) > MAX_COORD)
    return;
  if (a->arrow_head == RDS_OSD_ARROW_START_HEAD || a->arrow_head == RDS_OSD_ARROW_BOTH_HEAD)
    draw_arrow_head (d, pl, x1, y1, x2, y2, a->arrow_width, &p);
  if (a->arrow_head == RDS_OSD_ARROW_END_HEAD || a->arrow_head == RDS_OSD_ARROW_BOTH_HEAD)
    draw_arrow_head (d, pl, x2, y2, x1, y1, a->arrow_width, &p);
}

/* A ring straddling the radius (DeepStream 8.0 behaviour, GPU mode): circle_width w covers the pixels whose
 * distance from the centre pixel is in [r - floor(w/2), r + ceil(w/2)] (w 3, r 60: 59..62); w 0 still draws a
 * ZERO_CIRCLE_WIDTH ring centred on r (59..61). has_bg_color fills inside the ring with bg_color. The band edges
 * lie half a pixel outside those distances, so no pixel centre sits on an edge. */
static void
draw_circle (RdsOsdDraw * d, const Plane * pl, const RdsOSD_CircleParams * c)
{
  gdouble w = MIN (c->circle_width, MAX_COORD), r = c->radius, lo, hi;
  gdouble xc = c->xc + 0.5, yc = c->yc + 0.5, inner, outer;
  Paint p;
  Mask m;

  if (c->xc > MAX_COORD || c->yc > MAX_COORD || c->radius > MAX_COORD)
    return;
  if (c->circle_width > 0) {
    lo = floor (w / 2);
    hi = ceil (w / 2);
  } else {
    lo = hi = (ZERO_CIRCLE_WIDTH - 1) / 2;
  }
  inner = r - lo - 0.5;
  outer = r + hi + 0.5;
  if (c->has_bg_color && inner > 0 && paint_init (&p, &c->bg_color)
      && mask_begin (d, pl, xc - inner, yc - inner, xc + inner, yc + inner, &m)) {
    cairo_arc (m.cr, xc, yc, inner, 0, 2 * G_PI);
    cairo_fill (m.cr);
    mask_end (d, pl, &m, &p);
  }
  if (paint_init (&p, &c->circle_color)
      && mask_begin (d, pl, xc - outer - 1, yc - outer - 1, xc + outer + 1, yc + outer + 1, &m)) {
    cairo_set_line_width (m.cr, outer - MAX (inner, 0));
    cairo_arc (m.cr, xc, yc, (outer + MAX (inner, 0)) / 2, 0, 2 * G_PI);
    cairo_stroke (m.cr);
    mask_end (d, pl, &m, &p);
  }
}

/* ---- text ---- */

/* The font for @name at @size points: Pango's description syntax ("Serif", "DejaVu Sans Mono", "Sans Bold"); NULL,
 * empty or without a family gives RDS_OSD_DEFAULT_FONT (ours for NULL: DeepStream 8.0's GPU mode crashes). An
 * unknown family is fontconfig's fallback, a sans (DeepStream 8.0 behaviour). */
static const PangoFontDescription *
font_for (RdsOsdDraw * d, const gchar * name, guint size)
{
  gchar *key = g_strdup_printf ("%s\x1f%u", name != NULL ? name : "", size);
  PangoFontDescription *desc = g_hash_table_lookup (d->fonts, key);

  if (desc != NULL) {
    g_free (key);
    return desc;
  }
  desc = pango_font_description_from_string (name != NULL ? name : "");
  if (pango_font_description_get_family (desc) == NULL || *pango_font_description_get_family (desc) == '\0')
    pango_font_description_set_family (desc, RDS_OSD_DEFAULT_FONT);
  pango_font_description_set_size (desc, (gint) size * PANGO_SCALE);
  g_hash_table_insert (d->fonts, key, desc);
  return desc;
}

/* DeepStream 8.0 behaviour (GPU mode): the box's top left is (x_offset + 1, y_offset + 1); the box is the text's
 * extents plus padding (about 10 px left and right and 5 px above and below at size 20: ours, size / 2 and size / 4);
 * with set_bg_clr it is filled with text_bg_clr, then the text is drawn in font_color. Not moved into the frame,
 * clipped at its edges. font_size 0, NULL or empty text draw nothing, not even the box. */
static void
draw_text (RdsOsdDraw * d, const Plane * pl, const RdsOSD_TextParams * t)
{
  guint size = MIN (t->font_params.font_size, MAX_FONT_SIZE);
  gint64 bx = (gint64) t->x_offset + 1, by = (gint64) t->y_offset + 1, pad_x = size / 2, pad_y = size / 4;
  PangoRectangle ink, logical;
  gdouble ox, oy;
  Paint p;
  Mask m;

  if (size == 0 || t->display_text == NULL || t->display_text[0] == '\0' || bx >= pl->width || by >= pl->height)
    return;
  pango_layout_set_font_description (d->layout, font_for (d, t->font_params.font_name, size));
  if (g_utf8_validate (t->display_text, -1, NULL)) {
    pango_layout_set_text (d->layout, t->display_text, -1);
  } else {
    /* ours: invalid UTF-8 is drawn with U+FFFD for the bad bytes (Pango refuses such text) */
    gchar *valid = g_utf8_make_valid (t->display_text, -1);

    pango_layout_set_text (d->layout, valid, -1);
    g_free (valid);
  }
  pango_layout_get_pixel_extents (d->layout, &ink, &logical);

  if (t->set_bg_clr && paint_init (&p, &t->text_bg_clr))
    box (pl, bx, by, bx + logical.width + 2 * pad_x - 1, by + logical.height + 2 * pad_y - 1, &p);

  ox = (gdouble) (bx + pad_x - logical.x);
  oy = (gdouble) (by + pad_y - logical.y);
  if (!paint_init (&p, &t->font_params.font_color)
      || !mask_begin (d, pl, ox + ink.x - 1, oy + ink.y - 1, ox + ink.x + ink.width + 1,
          oy + ink.y + ink.height + 1, &m))
    return;
  cairo_move_to (m.cr, ox, oy);
  pango_cairo_show_layout (m.cr, d->layout);
  mask_end (d, pl, &m, &p);
}

/* ---- the frame ---- */

RdsOsdDrawFlags
rds_osd_draw_frame (RdsOsdDraw * d, const RdsOsdDrawOptions * opt, const RdsFrameMeta * frame, guint8 * data,
    gint width, gint height, gint stride)
{
  Plane pl = {.data = data,.width = width,.height = height,.stride = stride };
  RdsOsdDrawFlags flags = 0;
  GList *objs = frame != NULL ? frame->obj_meta_list : NULL, *dms = frame != NULL ? frame->display_meta_list : NULL;

  g_return_val_if_fail (d != NULL && opt != NULL && data != NULL, 0);
  if (width <= 0 || height <= 0)
    return 0;

  /* DeepStream 8.0 behaviour: every rect, then every text, lines, arrows, circles; objects before display metas;
   * list order within a kind (later on top); at most RDS_MAX_ELEMENTS_IN_DISPLAY_META of each kind per display meta */
  if (opt->display_bbox) {
    for (GList * l = objs; l != NULL; l = l->next)
      draw_rect (&pl, &((RdsObjectMeta *) l->data)->rect_params, &flags);
    for (GList * l = dms; l != NULL; l = l->next) {
      RdsDisplayMeta *dm = l->data;

      for (guint i = 0; i < MIN (dm->num_rects, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
        draw_rect (&pl, &dm->rect_params[i], &flags);
    }
  }
  if (opt->display_text) {
    for (GList * l = objs; l != NULL; l = l->next)
      draw_text (d, &pl, &((RdsObjectMeta *) l->data)->text_params);
    for (GList * l = dms; l != NULL; l = l->next) {
      RdsDisplayMeta *dm = l->data;

      for (guint i = 0; i < MIN (dm->num_labels, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
        draw_text (d, &pl, &dm->text_params[i]);
    }
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_lines, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      draw_line (d, &pl, &dm->line_params[i]);
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_arrows, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      draw_arrow (d, &pl, &dm->arrow_params[i]);
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_circles, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      draw_circle (d, &pl, &dm->circle_params[i]);
  }

  /* the clock is text like any other, without a box (DeepStream 8.0 behaviour: hidden by display-text too) */
  if (opt->display_clock && opt->display_text && opt->clock_text != NULL) {
    RdsOSD_TextParams clock = {
      .display_text = (gchar *) opt->clock_text,
      .x_offset = opt->x_clock_offset,
      .y_offset = opt->y_clock_offset,
      .font_params = {
            .font_name = (gchar *) (opt->clock_font != NULL ? opt->clock_font : RDS_OSD_CLOCK_FONT),
            .font_size = opt->clock_font_size != 0 ? opt->clock_font_size : RDS_OSD_CLOCK_FONT_SIZE,
          .font_color = opt->clock_color},
    };

    draw_text (d, &pl, &clock);
  }
  return flags;
}

gchar *
rds_osd_clock_text (GDateTime * t)
{
  return g_date_time_format (t, "%Y-%m-%d %H:%M:%S");
}

RdsOSD_ColorParams
rds_osd_color_from_rgba (guint32 rgba)
{
  return (RdsOSD_ColorParams) {
  ((rgba >> 24) & 0xff) / 255.0, ((rgba >> 16) & 0xff) / 255.0, ((rgba >> 8) & 0xff) / 255.0,
        (rgba & 0xff) / 255.0};
}

RdsOsdDraw *
rds_osd_draw_new (void)
{
  RdsOsdDraw *d = g_new0 (RdsOsdDraw, 1);
  cairo_font_options_t *fo = cairo_font_options_create ();

  d->font_map = pango_cairo_font_map_new ();
  d->context = pango_font_map_create_context (d->font_map);
  pango_cairo_context_set_resolution (d->context, TEXT_DPI);
  /* greyscale antialiasing (the mask is A8), no hinting of metrics: positions do not depend on the offsets */
  cairo_font_options_set_antialias (fo, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_metrics (fo, CAIRO_HINT_METRICS_OFF);
  pango_cairo_context_set_font_options (d->context, fo);
  cairo_font_options_destroy (fo);
  d->layout = pango_layout_new (d->context);
  d->fonts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      (GDestroyNotify) pango_font_description_free);
  return d;
}

void
rds_osd_draw_free (RdsOsdDraw * d)
{
  if (d == NULL)
    return;
  g_hash_table_unref (d->fonts);
  g_object_unref (d->layout);
  g_object_unref (d->context);
  g_object_unref (d->font_map);
  g_free (d->scratch);
  g_free (d);
}
