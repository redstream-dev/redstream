/* Redstream: rdsosd's drawing core (see rds-osd-draw.h).
 *
 * A frame is drawn in two steps: its metas become draw commands (rds-osd-shapes.h: boxes for rectangle bands, fills
 * and text backgrounds, segments for lines and arrow shafts, wings and a bevel join for arrow heads, rings for
 * circles, coverage masks for text), in draw order; then rds_osd_shade_commands blends each command over its
 * bounding box into the RGBA plane. The coverage and blend live in rds-osd-shapes.h, plain C written to compile for
 * the planned GPU kernel too, so that both paths can draw the same bytes. Shapes have hard edges (as DeepStream
 * 8.0's GPU mode draws them); text is rasterized by Pango into A8 masks with greyscale antialiasing, the only use of
 * Cairo. The frame's own bytes, alpha included, are only ever
 * read and blended, never reinterpreted, and only the pixels a command covers are touched.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-draw.h"
#include "rds-osd-shapes.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

/* Text is rendered at 96 dpi: font_size is in points (DeepStream 8.0 behaviour: size 20 has 20 px capitals). */
#define TEXT_DPI 96.0
/* font sizes above this are drawn this size (ours: keeps Pango's fixed-point sizes in range) */
#define MAX_FONT_SIZE 1024
/* text masks start at multiples of this in the mask buffer */
#define MASK_ALIGN 16

struct _RdsOsdDraw
{
  PangoFontMap *font_map;
  PangoContext *context;
  PangoLayout *layout;
  GHashTable *fonts;            /* "name\x1fsize" -> PangoFontDescription */
  /* the frame being drawn: its commands and the text masks they point into */
  RdsOsdCmd *cmds;
  guint n_cmds, cmds_size;
  guint8 *masks;
  gsize masks_len, masks_size;
  gint width, height;
};

/* Room for @n more commands at d->cmds + d->n_cmds; the builder's count is then added to d->n_cmds. */
static RdsOsdCmd *
cmds_reserve (RdsOsdDraw * d, guint n)
{
  if (d->n_cmds + n > d->cmds_size) {
    d->cmds_size = MAX (2 * d->cmds_size, d->n_cmds + n);
    d->cmds = g_renew (RdsOsdCmd, d->cmds, d->cmds_size);
  }
  return d->cmds + d->n_cmds;
}

static gboolean
color_init (RdsOsdColor * c, const RdsOSD_ColorParams * p)
{
  return rds_osd_color_init (c, p->red, p->green, p->blue, p->alpha);
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
build_rect (RdsOsdDraw * d, const RdsOSD_RectParams * r, RdsOsdDrawFlags * flags)
{
  gint64 L, T, R, B, bw = r->border_width, hi;
  RdsOsdColor c;

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
  hi = bw / 2;

  if (r->has_bg_color && color_init (&c, &r->bg_color))
    d->n_cmds += rds_osd_shape_box (cmds_reserve (d, 1), L + hi + 1, T + hi + 1, R - hi - 1, B - hi - 1, &c,
        d->width, d->height);
  if (bw > 0 && color_init (&c, &r->border_color))
    d->n_cmds += rds_osd_shape_rect_border (cmds_reserve (d, 4), L, T, R, B, (guint) bw, &c, d->width, d->height);
}

/* ---- lines, arrows, circles ---- */

static void
build_line (RdsOsdDraw * d, const RdsOSD_LineParams * l)
{
  RdsOsdColor c;

  if (color_init (&c, &l->line_color))
    d->n_cmds += rds_osd_shape_line (cmds_reserve (d, 1), l->x1, l->y1, l->x2, l->y2, l->line_width, &c, d->width,
        d->height);
}

/* The shaft like a line, a head at (x1, y1) for START, at (x2, y2) for END (DeepStream 8.0 behaviour), at both ends
 * for BOTH (ours: DeepStream 8.0 draws one head or nothing). Other head values draw the shaft only (ours). The shaft
 * and each head are blended on their own. */
static void
build_arrow (RdsOsdDraw * d, const RdsOSD_ArrowParams * a)
{
  RdsOsdColor c;

  if (!color_init (&c, &a->arrow_color))
    return;
  d->n_cmds += rds_osd_shape_line (cmds_reserve (d, 1), a->x1, a->y1, a->x2, a->y2, a->arrow_width, &c, d->width,
      d->height);
  /* a head is drawn only near the frame (and its direction needs sane coordinates) */
  if (MAX (MAX (a->x1, a->x2), MAX (a->y1, a->y2)) > RDS_OSD_MAX_COORD)
    return;
  if (a->arrow_head == RDS_OSD_ARROW_START_HEAD || a->arrow_head == RDS_OSD_ARROW_BOTH_HEAD)
    d->n_cmds += rds_osd_shape_arrow_head (cmds_reserve (d, 3), a->x1, a->y1, a->x2, a->y2, a->arrow_width, &c,
        d->width, d->height);
  if (a->arrow_head == RDS_OSD_ARROW_END_HEAD || a->arrow_head == RDS_OSD_ARROW_BOTH_HEAD)
    d->n_cmds += rds_osd_shape_arrow_head (cmds_reserve (d, 3), a->x2, a->y2, a->x1, a->y1, a->arrow_width, &c,
        d->width, d->height);
}

/* A ring straddling the radius (DeepStream 8.0 behaviour, GPU mode, see rds_osd_shape_circle); has_bg_color fills
 * inside the ring with bg_color first. */
static void
build_circle (RdsOsdDraw * d, const RdsOSD_CircleParams * c)
{
  RdsOsdColor ring, fill;
  gboolean has_ring = color_init (&ring, &c->circle_color);
  gboolean has_fill = c->has_bg_color && color_init (&fill, &c->bg_color);

  d->n_cmds += rds_osd_shape_circle (cmds_reserve (d, 2), c->xc, c->yc, c->radius, c->circle_width,
      has_ring ? &ring : NULL, has_fill ? &fill : NULL, d->width, d->height);
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
build_text (RdsOsdDraw * d, const RdsOSD_TextParams * t)
{
  guint size = MIN (t->font_params.font_size, MAX_FONT_SIZE);
  gint64 bx = (gint64) t->x_offset + 1, by = (gint64) t->y_offset + 1, pad_x = size / 2, pad_y = size / 4;
  PangoRectangle ink, logical;
  gdouble ox, oy, x0, y0, x1, y1;
  gint mw, mh, mstride;
  gsize offset;
  RdsOsdColor c;
  cairo_surface_t *surface;
  cairo_t *cr;

  if (size == 0 || t->display_text == NULL || t->display_text[0] == '\0' || bx >= d->width || by >= d->height)
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

  if (t->set_bg_clr && color_init (&c, &t->text_bg_clr))
    d->n_cmds += rds_osd_shape_box (cmds_reserve (d, 1), bx, by, bx + logical.width + 2 * pad_x - 1,
        by + logical.height + 2 * pad_y - 1, &c, d->width, d->height);

  /* the glyphs: an A8 mask over their ink box (one pixel more around it), clipped to the frame */
  ox = (gdouble) (bx + pad_x - logical.x);
  oy = (gdouble) (by + pad_y - logical.y);
  x0 = floor (MAX (ox + ink.x - 1, 0));
  y0 = floor (MAX (oy + ink.y - 1, 0));
  x1 = ceil (MIN (ox + ink.x + ink.width + 1, d->width - 1));
  y1 = ceil (MIN (oy + ink.y + ink.height + 1, d->height - 1));
  if (!color_init (&c, &t->font_params.font_color) || !(x0 <= x1 && y0 <= y1))
    return;
  mw = (gint) x1 - (gint) x0 + 1;
  mh = (gint) y1 - (gint) y0 + 1;
  mstride = cairo_format_stride_for_width (CAIRO_FORMAT_A8, mw);
  offset = (d->masks_len + MASK_ALIGN - 1) / MASK_ALIGN * MASK_ALIGN;
  /* ours: a text whose mask would end past 4 GiB in the frame's mask buffer (commands hold 32-bit offsets) is not
   * drawn. Unreachable in practice: each mask is clipped to the frame and fonts to MAX_FONT_SIZE points. */
  if (offset + (gsize) mstride * mh > G_MAXUINT32)
    return;
  if (offset + (gsize) mstride * mh > d->masks_size) {
    d->masks_size = MAX (2 * d->masks_size, offset + (gsize) mstride * mh);
    d->masks = g_realloc (d->masks, d->masks_size);
  }
  d->masks_len = offset + (gsize) mstride * mh;
  memset (d->masks + offset, 0, (gsize) mstride * mh);
  surface = cairo_image_surface_create_for_data (d->masks + offset, CAIRO_FORMAT_A8, mw, mh, mstride);
  cr = cairo_create (surface);
  cairo_translate (cr, -x0, -y0);
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
  cairo_move_to (cr, ox, oy);
  pango_cairo_show_layout (cr, d->layout);
  cairo_destroy (cr);
  cairo_surface_flush (surface);
  cairo_surface_destroy (surface);
  d->n_cmds += rds_osd_shape_mask (cmds_reserve (d, 1), (gint32) x0, (gint32) y0, mw, mh, (guint32) mstride,
      (guint32) offset, &c, d->width, d->height);
}

/* ---- the frame ---- */

RdsOsdDrawFlags
rds_osd_draw_frame (RdsOsdDraw * d, const RdsOsdDrawOptions * opt, const RdsFrameMeta * frame, guint8 * data,
    gint width, gint height, gint stride)
{
  RdsOsdDrawFlags flags = 0;
  GList *objs = frame != NULL ? frame->obj_meta_list : NULL, *dms = frame != NULL ? frame->display_meta_list : NULL;

  g_return_val_if_fail (d != NULL && opt != NULL && data != NULL, 0);
  if (width <= 0 || height <= 0)
    return 0;
  d->n_cmds = 0;
  d->masks_len = 0;
  d->width = width;
  d->height = height;

  /* DeepStream 8.0 behaviour: every rect, then every text, lines, arrows, circles; objects before display metas;
   * list order within a kind (later on top); at most RDS_MAX_ELEMENTS_IN_DISPLAY_META of each kind per display meta */
  if (opt->display_bbox) {
    for (GList * l = objs; l != NULL; l = l->next)
      build_rect (d, &((RdsObjectMeta *) l->data)->rect_params, &flags);
    for (GList * l = dms; l != NULL; l = l->next) {
      RdsDisplayMeta *dm = l->data;

      for (guint i = 0; i < MIN (dm->num_rects, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
        build_rect (d, &dm->rect_params[i], &flags);
    }
  }
  if (opt->display_text) {
    for (GList * l = objs; l != NULL; l = l->next)
      build_text (d, &((RdsObjectMeta *) l->data)->text_params);
    for (GList * l = dms; l != NULL; l = l->next) {
      RdsDisplayMeta *dm = l->data;

      for (guint i = 0; i < MIN (dm->num_labels, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
        build_text (d, &dm->text_params[i]);
    }
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_lines, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      build_line (d, &dm->line_params[i]);
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_arrows, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      build_arrow (d, &dm->arrow_params[i]);
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_circles, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      build_circle (d, &dm->circle_params[i]);
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

    build_text (d, &clock);
  }

  rds_osd_shade_commands (d->cmds, d->n_cmds, d->masks, data, width, height, (gsize) stride);
  return flags;
}

static gboolean
text_visible (const RdsOSD_TextParams * t)
{
  return t->font_params.font_size > 0 && t->display_text != NULL && t->display_text[0] != '\0';
}

static gboolean
rect_visible (const RdsOSD_RectParams * r)
{
  return r->border_width > 0 || r->has_bg_color;
}

gboolean
rds_osd_draw_frame_has_work (const RdsOsdDrawOptions * opt, const RdsFrameMeta * frame)
{
  if (opt->display_clock && opt->display_text && opt->clock_text != NULL)
    return TRUE;
  if (frame == NULL)
    return FALSE;
  for (GList * l = frame->obj_meta_list; l != NULL; l = l->next) {
    RdsObjectMeta *om = l->data;

    if ((opt->display_bbox && rect_visible (&om->rect_params)) || (opt->display_text && text_visible
            (&om->text_params)))
      return TRUE;
  }
  for (GList * l = frame->display_meta_list; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    if (dm->num_lines > 0 || dm->num_arrows > 0 || dm->num_circles > 0 || (opt->display_bbox && dm->num_rects > 0)
        || (opt->display_text && dm->num_labels > 0))
      return TRUE;
  }
  return FALSE;
}

void
rds_osd_draw_warm_up (RdsOsdDraw * d)
{
  PangoRectangle ink, logical;

  /* fontconfig's first lookup reads its caches (tens of ms): do it before the first frame */
  pango_layout_set_text (d->layout, "0", -1);
  pango_layout_set_font_description (d->layout, font_for (d, RDS_OSD_DEFAULT_FONT, RDS_OSD_CLOCK_FONT_SIZE));
  pango_layout_get_pixel_extents (d->layout, &ink, &logical);
  pango_layout_set_font_description (d->layout, font_for (d, RDS_OSD_CLOCK_FONT, RDS_OSD_CLOCK_FONT_SIZE));
  pango_layout_get_pixel_extents (d->layout, &ink, &logical);
}

gchar *
rds_osd_draw_resolve_font (RdsOsdDraw * d, const gchar * name)
{
  PangoFont *font = pango_font_map_load_font (d->font_map, d->context, font_for (d, name, 12));
  PangoFontDescription *desc;
  gchar *family;

  if (font == NULL)
    return NULL;
  desc = pango_font_describe (font);
  family = g_strdup (pango_font_description_get_family (desc));
  pango_font_description_free (desc);
  g_object_unref (font);
  return family;
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
  g_free (d->cmds);
  g_free (d->masks);
  g_free (d);
}
