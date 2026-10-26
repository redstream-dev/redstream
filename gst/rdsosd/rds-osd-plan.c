/* Redstream: rdsosd's draw planner (see rds-osd-plan.h).
 *
 * A batch is planned in two steps. rds_osd_plan_add_frame turns each frame's metas into draw commands
 * (rds-osd-shapes.h: boxes for rectangle bands, fills and text backgrounds, segments for lines and arrow shafts, wings
 * and a bevel join for arrow heads, rings for circles, coverage masks for text), in draw order, all of the batch's in
 * one array. A text's MASK command then names a mask request (its index, in place of the heap offset), and the text's
 * layout is cached.
 * rds_osd_plan_next_pass walks the commands from where the last pass ended, finds or places each mask in the heap
 * (rasterizing the new ones) and ends the pass before the first mask that does not fit beside the ones the pass uses,
 * or before the first shape that would take the pass's binning past its budget.
 * Shapes have hard edges (as DeepStream 8.0's GPU mode draws them); text is rasterized by Pango into A8 masks with
 * greyscale antialiasing, the only use of Cairo.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-plan.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

/* Text is rendered at 96 dpi: font_size is in points (DeepStream 8.0 behaviour: size 20 has 20 px capitals). */
#define TEXT_DPI 96.0
/* font sizes above this are drawn this size (ours: keeps Pango's fixed-point sizes in range) */
#define MAX_FONT_SIZE 1024
/* the layout cache is emptied when it holds this many texts; the font cache, at the start of a batch, when it holds
 * more than this many fonts */
#define MAX_LAYOUTS 4096
#define MAX_FONTS 256

/* A slot's tile grid in the pass: its tiles are tiles[base .. base + tiles_x · tiles_y − 1], in row order. */
typedef struct
{
  guint32 slot, tiles_x, tiles_y, base;
} Grid;

#define TILES(n) (((guint32) (n) + RDS_OSD_TILE_SIZE - 1) / RDS_OSD_TILE_SIZE)

/* A text's extents, by its text key: "<length of font>:<font><size>:<text>" (the font's length makes it
 * unambiguous whatever the font and text contain). */
typedef struct
{
  PangoRectangle ink, logical;
} Layout;

/* A mask in the heap, by its request key: heap[offset .. offset + size − 1] (size aligned). */
typedef struct
{
  gchar *key;
  guint32 offset, size;
  guint64 pass;                 /* the last pass that used it */
  guint64 wanted;               /* the last pass whose batch still needed it when the pass started */
  GList link;                   /* in the LRU queue, least recently used first */
} Mask;

/* A text mask the batch needs: its text in @desc with the layout origin at (dx, dy) of an mw × mh A8 mask. Its key
 * and text are offsets into the batch's key arena (which moves while the batch is built). */
typedef struct
{
  gsize key;                    /* "<dx>,<dy>,<mw>,<mh>," and the text key */
  gsize text;                   /* the text as given (the end of the key) */
  const PangoFontDescription *desc;
  gint dx, dy, mw, mh, stride;
} MaskRequest;

struct _RdsOsdPlan
{
  PangoFontMap *font_map;
  PangoContext *context;
  PangoLayout *layout;
  GHashTable *fonts;            /* "<size>:<name>" -> PangoFontDescription */
  GHashTable *layouts;          /* text key -> Layout */
  GString *font_key, *text_key; /* scratch */

  /* the mask heap */
  guint8 *heap;
  gsize heap_size;
  GHashTable *masks;            /* key -> Mask (owns it) */
  GPtrArray *by_offset;         /* the masks in heap order */
  GQueue lru;
  guint64 pass_serial;

  /* the batch */
  RdsOsdCmd *cmds;
  guint32 n_cmds, cmds_size;
  GArray *frames;               /* RdsOsdPlanFrame, first indexing the batch's commands */
  GArray *requests;             /* MaskRequest */
  GString *keys;                /* the requests' keys, each NUL-terminated */
  guint32 next_cmd;             /* where the next pass starts */
  guint next_frame;             /* the frame next_cmd is in */
  guint64 pass_budget;          /* words of tile lists and tile counts a pass may need */
  gboolean started;             /* a pass has been planned */
  /* the current frame while adding it */
  gint width, height;

  /* the current pass */
  GArray *pass_frames;          /* RdsOsdPlanFrame */
  GArray *uploads;              /* RdsOsdMaskUpload */
  GArray *items;                /* RdsOsdWorkItem */
  GArray *lists;                /* guint32 */
  GArray *tiles;                /* guint32 per tile: counts, then write positions */
  GArray *pass_slots;           /* guint32: the slots of the pass, in the order of their first frame */
  GArray *grids;                /* Grid per pass slot */
  GArray *grid_of;              /* guint per pass frame: its grid */
  guint32 n_new_masks;
};

/* Room for @n more commands at p->cmds + p->n_cmds; the builder's count is then added to p->n_cmds. */
static RdsOsdCmd *
cmds_reserve (RdsOsdPlan * p, guint n)
{
  if (p->n_cmds + n > p->cmds_size) {
    p->cmds_size = MAX (2 * p->cmds_size, p->n_cmds + n);
    p->cmds = g_renew (RdsOsdCmd, p->cmds, p->cmds_size);
  }
  return p->cmds + p->n_cmds;
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
build_rect (RdsOsdPlan * p, const RdsOSD_RectParams * r, RdsOsdDrawFlags * flags)
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
    p->n_cmds += rds_osd_shape_box (cmds_reserve (p, 1), L + hi + 1, T + hi + 1, R - hi - 1, B - hi - 1, &c,
            p->width, p->height);
  if (bw > 0 && color_init (&c, &r->border_color))
    p->n_cmds += rds_osd_shape_rect_border (cmds_reserve (p, 4), L, T, R, B, (guint) bw, &c, p->width,
            p->height);
}

/* ---- lines, arrows, circles ---- */

static void
build_line (RdsOsdPlan * p, const RdsOSD_LineParams * l)
{
  RdsOsdColor c;

  if (color_init (&c, &l->line_color))
    p->n_cmds += rds_osd_shape_line (cmds_reserve (p, 1), l->x1, l->y1, l->x2, l->y2, l->line_width, &c,
            p->width, p->height);
}

/* The shaft like a line, a head at (x1, y1) for START, at (x2, y2) for END (DeepStream 8.0 behaviour), at both ends
 * for BOTH (ours: DeepStream 8.0 draws one head or nothing). Other head values draw the shaft only (ours). The shaft
 * and each head are blended on their own. */
static void
build_arrow (RdsOsdPlan * p, const RdsOSD_ArrowParams * a)
{
  RdsOsdColor c;

  if (!color_init (&c, &a->arrow_color))
    return;
  p->n_cmds += rds_osd_shape_line (cmds_reserve (p, 1), a->x1, a->y1, a->x2, a->y2, a->arrow_width, &c, p->width,
          p->height);
  /* a head is drawn only near the frame (and its direction needs sane coordinates) */
  if (MAX (MAX (a->x1, a->x2), MAX (a->y1, a->y2)) > RDS_OSD_MAX_COORD)
    return;
  if (a->arrow_head == RDS_OSD_ARROW_START_HEAD || a->arrow_head == RDS_OSD_ARROW_BOTH_HEAD)
    p->n_cmds += rds_osd_shape_arrow_head (cmds_reserve (p, 3), a->x1, a->y1, a->x2, a->y2, a->arrow_width, &c,
            p->width, p->height);
  if (a->arrow_head == RDS_OSD_ARROW_END_HEAD || a->arrow_head == RDS_OSD_ARROW_BOTH_HEAD)
    p->n_cmds += rds_osd_shape_arrow_head (cmds_reserve (p, 3), a->x2, a->y2, a->x1, a->y1, a->arrow_width, &c,
            p->width, p->height);
}

/* A ring straddling the radius (DeepStream 8.0 behaviour, GPU mode, see rds_osd_shape_circle); has_bg_color fills
 * inside the ring with bg_color first. */
static void
build_circle (RdsOsdPlan * p, const RdsOSD_CircleParams * c)
{
  RdsOsdColor ring, fill;
  gboolean has_ring = color_init (&ring, &c->circle_color);
  gboolean has_fill = c->has_bg_color && color_init (&fill, &c->bg_color);

  p->n_cmds += rds_osd_shape_circle (cmds_reserve (p, 2), c->xc, c->yc, c->radius, c->circle_width,
          has_ring ? &ring : NULL, has_fill ? &fill : NULL, p->width, p->height);
}

/* ---- text ---- */

/* The font for @name at @size points: Pango's description syntax ("Serif", "DejaVu Sans Mono", "Sans Bold"); NULL,
 * empty or without a family gives RDS_OSD_DEFAULT_FONT (ours for NULL: DeepStream 8.0's GPU mode crashes). An
 * unknown family is fontconfig's fallback, a sans (DeepStream 8.0 behaviour). */
static const PangoFontDescription *
font_for (RdsOsdPlan * p, const gchar * name, guint size)
{
  PangoFontDescription *desc;

  g_string_printf (p->font_key, "%u:%s", size, name != NULL ? name : "");
  desc = g_hash_table_lookup (p->fonts, p->font_key->str);
  if (desc != NULL)
    return desc;
  desc = pango_font_description_from_string (name != NULL ? name : "");
  if (pango_font_description_get_family (desc) == NULL || *pango_font_description_get_family (desc) == '\0')
    pango_font_description_set_family (desc, RDS_OSD_DEFAULT_FONT);
  pango_font_description_set_size (desc, (gint) size * PANGO_SCALE);
  g_hash_table_insert (p->fonts, g_strdup (p->font_key->str), desc);
  return desc;
}

/* @s as valid UTF-8 (ours: invalid bytes become U+FFFD; Pango refuses such text). Free with g_free. */
static gchar *
valid_text (const gchar * s)
{
  return g_utf8_validate (s, -1, NULL) ? g_strdup (s) : g_utf8_make_valid (s, -1);
}

/* Sets @text in @desc on the plan's layout. */
static void
set_layout (RdsOsdPlan * p, const PangoFontDescription * desc, const gchar * text)
{
  pango_layout_set_font_description (p->layout, desc);
  pango_layout_set_text (p->layout, text, -1);
}

/* DeepStream 8.0 behaviour (GPU mode): the box's top left is (x_offset + 1, y_offset + 1); the box is the text's
 * extents plus padding (about 10 px left and right and 5 px above and below at size 20: ours, size / 2 and size / 4);
 * with set_bg_clr it is filled with text_bg_clr, then the text is drawn in font_color. Not moved into the frame,
 * clipped at its edges. font_size 0, NULL or empty text draw nothing, not even the box. The glyphs are a mask request,
 * rasterized when a pass places it. A text whose layout is cached allocates nothing here. */
static void
build_text (RdsOsdPlan * p, const RdsOSD_TextParams * t)
{
  guint size = MIN (t->font_params.font_size, MAX_FONT_SIZE);
  gint64 bx = (gint64) t->x_offset + 1, by = (gint64) t->y_offset + 1, pad_x = size / 2, pad_y = size / 4;
  const gchar *font = t->font_params.font_name != NULL ? t->font_params.font_name : "";
  const PangoFontDescription *desc;
  gdouble ox, oy, x0, y0, x1, y1;
  gint mw, mh, mstride;
  gsize text_at;
  Layout *lay;
  RdsOsdColor c;
  MaskRequest req;

  if (size == 0 || t->display_text == NULL || t->display_text[0] == '\0' || bx >= p->width || by >= p->height)
    return;
  desc = font_for (p, t->font_params.font_name, size);
  /* the text key: "<length of font>:<font><size>:<text>" */
  g_string_printf (p->text_key, "%zu:%s%u:", strlen (font), font, size);
  text_at = p->text_key->len;
  g_string_append (p->text_key, t->display_text);
  lay = g_hash_table_lookup (p->layouts, p->text_key->str);
  if (lay == NULL) {
    gchar *valid = valid_text (t->display_text);

    set_layout (p, desc, valid);
    g_free (valid);
    lay = g_new (Layout, 1);
    pango_layout_get_pixel_extents (p->layout, &lay->ink, &lay->logical);
    if (g_hash_table_size (p->layouts) >= MAX_LAYOUTS)
      g_hash_table_remove_all (p->layouts);
    g_hash_table_insert (p->layouts, g_strdup (p->text_key->str), lay);
  }

  if (t->set_bg_clr && color_init (&c, &t->text_bg_clr))
    p->n_cmds += rds_osd_shape_box (cmds_reserve (p, 1), bx, by, bx + lay->logical.width + 2 * pad_x - 1,
            by + lay->logical.height + 2 * pad_y - 1, &c, p->width, p->height);

  /* the glyphs: an A8 mask over their ink box (one pixel more around it), clipped to the frame */
  ox = (gdouble) (bx + pad_x - lay->logical.x);
  oy = (gdouble) (by + pad_y - lay->logical.y);
  x0 = floor (MAX (ox + lay->ink.x - 1, 0));
  y0 = floor (MAX (oy + lay->ink.y - 1, 0));
  x1 = ceil (MIN (ox + lay->ink.x + lay->ink.width + 1, p->width - 1));
  y1 = ceil (MIN (oy + lay->ink.y + lay->ink.height + 1, p->height - 1));
  if (!color_init (&c, &t->font_params.font_color) || !(x0 <= x1 && y0 <= y1))
    return;
  mw = (gint) x1 - (gint) x0 + 1;
  mh = (gint) y1 - (gint) y0 + 1;
  mstride = cairo_format_stride_for_width (CAIRO_FORMAT_A8, mw);
  /* ours: a text whose mask is larger than the heap is not drawn (its box is). Only absurd texts: a mask is clipped
   * to the frame and fonts to MAX_FONT_SIZE points. */
  if (((gsize) mstride * mh + RDS_OSD_MASK_ALIGN - 1) / RDS_OSD_MASK_ALIGN * RDS_OSD_MASK_ALIGN > p->heap_size)
    return;
  /* cached by text, font and size, and by where the frame's edge clips it: (dx, dy, mw, mh) */
  req.key = p->keys->len;
  g_string_append_printf (p->keys, "%d,%d,%d,%d,", (gint) (ox - x0), (gint) (oy - y0), mw, mh);
  req.text = p->keys->len + text_at;
  g_string_append_len (p->keys, p->text_key->str, (gssize) p->text_key->len + 1);
  req.desc = desc;
  req.dx = (gint) (ox - x0);
  req.dy = (gint) (oy - y0);
  req.mw = mw;
  req.mh = mh;
  req.stride = mstride;
  /* the command holds the request's index until a pass places the mask */
  if (rds_osd_shape_mask (cmds_reserve (p, 1), (gint32) x0, (gint32) y0, mw, mh, (guint32) mstride,
          p->requests->len, &c, p->width, p->height) == 1) {
    g_array_append_val (p->requests, req);
    p->n_cmds += 1;
  } else {
    g_string_truncate (p->keys, req.key);
  }
}

/* ---- the batch ---- */

RdsOsdDrawFlags
rds_osd_plan_add_frame (RdsOsdPlan * p, const RdsOsdDrawOptions * opt, guint slot, const RdsFrameMeta * frame,
    gint width, gint height)
{
  RdsOsdDrawFlags flags = 0;
  GList *objs = frame != NULL ? frame->obj_meta_list : NULL, *dms = frame != NULL ? frame->display_meta_list : NULL;
  RdsOsdPlanFrame bf;

  g_return_val_if_fail (p != NULL && opt != NULL, 0);
  g_return_val_if_fail (!p->started, 0);
  if (width <= 0 || height <= 0)
    return 0;
  /* ours: larger frames are not drawn (tile numbers are 16-bit, and a frame's tile counts stay bounded) */
  if (width > RDS_OSD_MAX_PLAN_FRAME || height > RDS_OSD_MAX_PLAN_FRAME
      || (guint64) TILES (width) * TILES (height) > RDS_OSD_MAX_PLAN_TILES) {
    g_debug ("frame of %dx%d on slot %u is larger than rdsosd draws: not drawn", width, height, slot);
    return 0;
  }
  for (guint i = 0; i < p->frames->len; i++) {
    const RdsOsdPlanFrame *f = &g_array_index (p->frames, RdsOsdPlanFrame, i);

    if (f->slot == slot && (f->width != width || f->height != height)) {
      g_critical ("rdsosd: frame of %dx%d on slot %u, which has a %dx%d frame in this batch: not drawn", width,
          height, slot, f->width, f->height);
      return 0;
    }
  }
  bf.slot = slot;
  bf.width = width;
  bf.height = height;
  bf.first = p->n_cmds;
  bf.n_cmds = 0;
  p->width = width;
  p->height = height;

  /* DeepStream 8.0 behaviour: every rect, then every text, lines, arrows, circles; objects before display metas;
   * list order within a kind (later on top); at most RDS_MAX_ELEMENTS_IN_DISPLAY_META of each kind per display meta */
  if (opt->display_bbox) {
    for (GList * l = objs; l != NULL; l = l->next)
      build_rect (p, &((RdsObjectMeta *) l->data)->rect_params, &flags);
    for (GList * l = dms; l != NULL; l = l->next) {
      RdsDisplayMeta *dm = l->data;

      for (guint i = 0; i < MIN (dm->num_rects, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
        build_rect (p, &dm->rect_params[i], &flags);
    }
  }
  if (opt->display_text) {
    for (GList * l = objs; l != NULL; l = l->next)
      build_text (p, &((RdsObjectMeta *) l->data)->text_params);
    for (GList * l = dms; l != NULL; l = l->next) {
      RdsDisplayMeta *dm = l->data;

      for (guint i = 0; i < MIN (dm->num_labels, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
        build_text (p, &dm->text_params[i]);
    }
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_lines, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      build_line (p, &dm->line_params[i]);
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_arrows, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      build_arrow (p, &dm->arrow_params[i]);
  }
  for (GList * l = dms; l != NULL; l = l->next) {
    RdsDisplayMeta *dm = l->data;

    for (guint i = 0; i < MIN (dm->num_circles, RDS_MAX_ELEMENTS_IN_DISPLAY_META); i++)
      build_circle (p, &dm->circle_params[i]);
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

    build_text (p, &clock);
  }

  bf.n_cmds = p->n_cmds - bf.first;
  if (bf.n_cmds > 0)
    g_array_append_val (p->frames, bf);
  return flags;
}

void
rds_osd_plan_begin (RdsOsdPlan * p)
{
  g_return_if_fail (p != NULL);
  p->n_cmds = 0;
  g_array_set_size (p->frames, 0);
  g_array_set_size (p->requests, 0);
  g_string_truncate (p->keys, 0);
  p->next_cmd = 0;
  p->next_frame = 0;
  p->started = FALSE;
  /* the requests of the last batch held its font descriptions */
  if (g_hash_table_size (p->fonts) > MAX_FONTS)
    g_hash_table_remove_all (p->fonts);
}

/* ---- the mask heap ---- */

static gsize
mask_aligned (gsize size)
{
  return (size + RDS_OSD_MASK_ALIGN - 1) / RDS_OSD_MASK_ALIGN * RDS_OSD_MASK_ALIGN;
}

static void
mask_free (Mask * m)
{
  g_free (m->key);
  g_free (m);
}

static void
mask_evict (RdsOsdPlan * p, Mask * m)
{
  g_queue_unlink (&p->lru, &m->link);
  g_ptr_array_remove (p->by_offset, m);
  g_hash_table_remove (p->masks, m->key);
}

void
rds_osd_plan_drop_masks (RdsOsdPlan * p)
{
  g_return_if_fail (p != NULL);
  g_ptr_array_set_size (p->by_offset, 0);
  g_queue_init (&p->lru);
  g_hash_table_remove_all (p->masks);
}

/* The first gap of @size bytes in the heap: its offset, and where it goes in by_offset. */
static gboolean
first_fit (RdsOsdPlan * p, gsize size, guint32 * offset, guint * index)
{
  gsize end = 0;
  guint i;

  for (i = 0; i < p->by_offset->len; i++) {
    Mask *m = g_ptr_array_index (p->by_offset, i);

    if (m->offset - end >= size)
      break;
    end = (gsize) m->offset + m->size;
  }
  if (i == p->by_offset->len && p->heap_size - end < size)
    return FALSE;
  *offset = (guint32) end;
  *index = i;
  return TRUE;
}

static void
rasterize (RdsOsdPlan * p, const MaskRequest * req, guint32 offset)
{
  gchar *valid = valid_text (p->keys->str + req->text);
  cairo_surface_t *surface;
  cairo_t *cr;

  /* the alignment padding too: uploads of neighbouring masks are merged */
  memset (p->heap + offset, 0, mask_aligned ((gsize) req->stride * req->mh));
  set_layout (p, req->desc, valid);
  g_free (valid);
  surface = cairo_image_surface_create_for_data (p->heap + offset, CAIRO_FORMAT_A8, req->mw, req->mh, req->stride);
  cr = cairo_create (surface);
  cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
  cairo_move_to (cr, req->dx, req->dy);
  pango_cairo_show_layout (cr, p->layout);
  cairo_destroy (cr);
  cairo_surface_flush (surface);
  cairo_surface_destroy (surface);
}

/* Finds or places @req's mask for this pass, evicting the least recently used masks this pass does not use (those
 * the rest of the batch does not need first); FALSE when it does not fit beside them. Each eviction rescans the heap
 * and the LRU queue: quadratic in the number of masks when a fragmented heap needs many evictions for one mask, which
 * stays cheap for the few hundred masks a heap holds. */
static gboolean
mask_place (RdsOsdPlan * p, const MaskRequest * req, guint32 * offset)
{
  const gchar *key = p->keys->str + req->key;
  Mask *m = g_hash_table_lookup (p->masks, key);
  gsize size = mask_aligned ((gsize) req->stride * req->mh);
  RdsOsdMaskUpload *last;
  guint index;

  if (m != NULL) {
    /* a hit: now the most recently used */
    g_queue_unlink (&p->lru, &m->link);
    g_queue_push_tail_link (&p->lru, &m->link);
    m->pass = p->pass_serial;
    *offset = m->offset;
    return TRUE;
  }
  while (!first_fit (p, size, offset, &index)) {
    Mask *victim = NULL;

    /* the least recently used mask the rest of the batch does not need, else the least recently used one this pass
     * does not use (those it uses are at the tail) */
    for (GList * l = g_queue_peek_head_link (&p->lru); l != NULL; l = l->next) {
      Mask *c = l->data;

      if (c->pass == p->pass_serial)
        break;
      if (victim == NULL || (victim->wanted == p->pass_serial && c->wanted != p->pass_serial))
        victim = c;
      if (victim->wanted != p->pass_serial)
        break;
    }
    if (victim == NULL)
      return FALSE;
    mask_evict (p, victim);
  }
  m = g_new0 (Mask, 1);
  m->key = g_strdup (key);
  m->offset = *offset;
  m->size = (guint32) size;
  m->pass = p->pass_serial;
  m->link.data = m;
  g_queue_push_tail_link (&p->lru, &m->link);
  g_ptr_array_insert (p->by_offset, (gint) index, m);
  g_hash_table_insert (p->masks, m->key, m);
  rasterize (p, req, *offset);
  p->n_new_masks++;
  /* one upload for masks placed back to back */
  last = p->uploads->len > 0 ? &g_array_index (p->uploads, RdsOsdMaskUpload, p->uploads->len - 1) : NULL;
  if (last != NULL && mask_aligned ((gsize) last->offset + last->size) == *offset) {
    last->size = *offset - last->offset + (guint32) req->stride * (guint32) req->mh;
  } else {
    RdsOsdMaskUpload up = { *offset, (guint32) req->stride * (guint32) req->mh };

    g_array_append_val (p->uploads, up);
  }
  return TRUE;
}

/* ---- passes ---- */

/* The shape of @f starting at cmds[i]: returns its last command, and in @t the tile range [t0, t1] × [t2, t3] its
 * commands' union box touches. */
static guint32
shape_tiles (const RdsOsdCmd * cmds, const RdsOsdPlanFrame * f, guint32 i, guint32 t[4])
{
  gint32 x0 = cmds[i].x0, y0 = cmds[i].y0, x1 = cmds[i].x1, y1 = cmds[i].y1;
  guint32 j;

  for (j = i; j + 1 < f->first + f->n_cmds && (cmds[j].flags & RDS_OSD_CMD_UNION_NEXT); j++) {
    x0 = MIN (x0, cmds[j + 1].x0);
    y0 = MIN (y0, cmds[j + 1].y0);
    x1 = MAX (x1, cmds[j + 1].x1);
    y1 = MAX (y1, cmds[j + 1].y1);
  }
  /* boxes are inside the frame */
  t[0] = (guint32) x0 / RDS_OSD_TILE_SIZE;
  t[1] = (guint32) x1 / RDS_OSD_TILE_SIZE;
  t[2] = (guint32) y0 / RDS_OSD_TILE_SIZE;
  t[3] = (guint32) y1 / RDS_OSD_TILE_SIZE;
  return j;
}

gboolean
rds_osd_plan_next_pass (RdsOsdPlan * p, RdsOsdPass * pass)
{
  guint32 start, end, j;
  guint fi;
  guint64 cost = 0;
  RdsOsdCmd *cmds;

  g_return_val_if_fail (p != NULL && pass != NULL, FALSE);
  start = p->next_cmd;
  cmds = p->cmds;
  p->started = TRUE;
  if (start >= p->n_cmds)
    return FALSE;
  p->pass_serial++;
  p->n_new_masks = 0;
  g_array_set_size (p->pass_frames, 0);
  g_array_set_size (p->pass_slots, 0);
  g_array_set_size (p->uploads, 0);
  g_array_set_size (p->items, 0);
  g_array_set_size (p->lists, 0);
  /* the cached masks the rest of the batch needs: evicted last */
  for (guint32 i = start; i < p->n_cmds; i++)
    if (cmds[i].type == RDS_OSD_CMD_MASK) {
      Mask *m = g_hash_table_lookup (p->masks,
          p->keys->str + g_array_index (p->requests, MaskRequest, cmds[i].mask.offset).key);

      if (m != NULL)
        m->wanted = p->pass_serial;
    }

  /* shape by shape (every command is in a frame, every shape in one): the pass ends before the first shape whose
   * binning would take it past the budget (tile lists and the tile counts of a slot's grid, 32-bit words each) or
   * whose mask does not fit beside those the pass uses. It holds at least one shape. */
  fi = p->next_frame;
  for (end = start; end < p->n_cmds; end = j + 1) {
    const RdsOsdPlanFrame *f;
    guint32 t[4];
    guint64 need;
    gboolean new_slot = TRUE;

    while (end >= g_array_index (p->frames, RdsOsdPlanFrame, fi).first
        + g_array_index (p->frames, RdsOsdPlanFrame, fi).n_cmds)
      fi++;
    f = &g_array_index (p->frames, RdsOsdPlanFrame, fi);
    j = shape_tiles (cmds, f, end, t);
    need = (guint64) (j - end + 1) * (t[1] - t[0] + 1) * (t[3] - t[2] + 1);
    for (guint k = 0; k < p->pass_slots->len && new_slot; k++)
      new_slot = g_array_index (p->pass_slots, guint32, k) != f->slot;
    if (new_slot)
      need += (guint64) TILES (f->width) * TILES (f->height);
    if (end > start && cost + need > p->pass_budget)
      break;
    /* a mask is never part of a larger shape */
    if (cmds[end].type == RDS_OSD_CMD_MASK) {
      guint32 offset;

      /* only at the first command of a pass can every other mask be evicted: then it fits (none exceeds the heap) */
      if (!mask_place (p, &g_array_index (p->requests, MaskRequest, cmds[end].mask.offset), &offset)) {
        g_assert (end > start);
        break;
      }
      cmds[end].mask.offset = offset;
    }
    cost += need;
    if (new_slot)
      g_array_append_val (p->pass_slots, f->slot);
  }
  p->next_cmd = end;
  p->next_frame = fi;

  for (guint i = 0; i < p->frames->len; i++) {
    const RdsOsdPlanFrame *bf = &g_array_index (p->frames, RdsOsdPlanFrame, i);
    guint32 first = MAX (bf->first, start), last = MIN (bf->first + bf->n_cmds, end);

    if (first < last) {
      RdsOsdPlanFrame pf = { bf->slot, bf->width, bf->height, first - start, last - first };

      g_array_append_val (p->pass_frames, pf);
    }
  }

  memset (pass, 0, sizeof *pass);
  pass->cmds = cmds + start;
  pass->n_cmds = end - start;
  pass->frames = (const RdsOsdPlanFrame *) p->pass_frames->data;
  pass->n_frames = p->pass_frames->len;
  pass->uploads = (const RdsOsdMaskUpload *) p->uploads->data;
  pass->n_uploads = p->uploads->len;
  pass->n_new_masks = p->n_new_masks;
  return TRUE;
}

void
rds_osd_plan_bin (RdsOsdPlan * p, RdsOsdPass * pass)
{
  guint64 n_tiles = 0, total = 0;
  guint32 *tiles, *lists;

  g_return_if_fail (p != NULL && pass != NULL);
  g_array_set_size (p->grids, 0);
  g_array_set_size (p->grid_of, pass->n_frames);
  /* a grid per slot, in the order of the slot's first frame */
  for (guint32 k = 0; k < pass->n_frames; k++) {
    const RdsOsdPlanFrame *f = &pass->frames[k];
    guint gi;

    for (gi = 0; gi < p->grids->len && g_array_index (p->grids, Grid, gi).slot != f->slot; gi++);
    if (gi == p->grids->len) {
      Grid g = { f->slot, TILES (f->width), TILES (f->height), (guint32) n_tiles };

      g_array_append_val (p->grids, g);
      n_tiles += (guint64) g.tiles_x * g.tiles_y;
    }
    g_array_index (p->grid_of, guint, k) = gi;
  }
  /* the pass budget bounds the tile counts and the lists (one shape alone may exceed a small budget, never 32 bits:
   * a frame has at most RDS_OSD_MAX_PLAN_TILES tiles) */
  g_assert (n_tiles <= G_MAXUINT32);
  g_array_set_size (p->tiles, (guint) n_tiles);
  tiles = (guint32 *) p->tiles->data;
  memset (tiles, 0, n_tiles * sizeof (guint32));

  /* each tile's count; a shape's commands go to every tile its union box touches */
  for (guint32 k = 0; k < pass->n_frames; k++) {
    const RdsOsdPlanFrame *f = &pass->frames[k];
    const Grid *g = &g_array_index (p->grids, Grid, g_array_index (p->grid_of, guint, k));

    for (guint32 i = f->first, j, t[4]; i < f->first + f->n_cmds; i = j + 1) {
      j = shape_tiles (pass->cmds, f, i, t);
      for (guint32 ty = t[2]; ty <= t[3]; ty++)
        for (guint32 tx = t[0]; tx <= t[1]; tx++)
          tiles[g->base + ty * g->tiles_x + tx] += j - i + 1;
    }
  }
  /* the work items of the non-empty tiles; each tile's count becomes its list's write position */
  g_array_set_size (p->items, 0);
  for (guint gi = 0; gi < p->grids->len; gi++) {
    const Grid *g = &g_array_index (p->grids, Grid, gi);

    for (guint32 t = 0; t < g->tiles_x * g->tiles_y; t++) {
      guint32 count = tiles[g->base + t];

      if (count > 0) {
        RdsOsdWorkItem w = { g->slot, (guint16) (t % g->tiles_x), (guint16) (t / g->tiles_x), (guint32) total,
          count
        };

        g_array_append_val (p->items, w);
      }
      tiles[g->base + t] = (guint32) total;
      total += count;
      g_assert (total <= G_MAXUINT32);
    }
  }
  /* the lists, in draw order */
  g_array_set_size (p->lists, (guint) total);
  lists = (guint32 *) p->lists->data;
  for (guint32 k = 0; k < pass->n_frames; k++) {
    const RdsOsdPlanFrame *f = &pass->frames[k];
    const Grid *g = &g_array_index (p->grids, Grid, g_array_index (p->grid_of, guint, k));

    for (guint32 i = f->first, j, t[4]; i < f->first + f->n_cmds; i = j + 1) {
      j = shape_tiles (pass->cmds, f, i, t);
      for (guint32 ty = t[2]; ty <= t[3]; ty++)
        for (guint32 tx = t[0]; tx <= t[1]; tx++) {
          guint32 *pos = &tiles[g->base + ty * g->tiles_x + tx];

          for (guint32 c = i; c <= j; c++)
            lists[(*pos)++] = c;
        }
    }
  }

  pass->items = (const RdsOsdWorkItem *) p->items->data;
  pass->n_items = p->items->len;
  pass->lists = (const guint32 *) p->lists->data;
  pass->n_lists = p->lists->len;
}

void
rds_osd_plan_set_pass_budget (RdsOsdPlan * p, guint64 words)
{
  g_return_if_fail (p != NULL && words > 0);
  p->pass_budget = words;
}

/* ---- the plan ---- */

const guint8 *
rds_osd_plan_heap (RdsOsdPlan * p)
{
  return p->heap;
}

void
rds_osd_plan_warm_up (RdsOsdPlan * p)
{
  PangoRectangle ink, logical;

  /* fontconfig's first lookup reads its caches (tens of ms): do it before the first frame */
  set_layout (p, font_for (p, RDS_OSD_DEFAULT_FONT, RDS_OSD_CLOCK_FONT_SIZE), "0");
  pango_layout_get_pixel_extents (p->layout, &ink, &logical);
  set_layout (p, font_for (p, RDS_OSD_CLOCK_FONT, RDS_OSD_CLOCK_FONT_SIZE), "0");
  pango_layout_get_pixel_extents (p->layout, &ink, &logical);
}

gchar *
rds_osd_plan_resolve_font (RdsOsdPlan * p, const gchar * name)
{
  PangoFont *font = pango_font_map_load_font (p->font_map, p->context, font_for (p, name, 12));
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

RdsOsdPlan *
rds_osd_plan_new (gsize heap_size)
{
  RdsOsdPlan *p;
  cairo_font_options_t *fo;

  g_return_val_if_fail (heap_size > 0 && heap_size <= G_MAXUINT32 - RDS_OSD_MASK_ALIGN + 1, NULL);
  g_return_val_if_fail (heap_size % RDS_OSD_MASK_ALIGN == 0, NULL);
  p = g_new0 (RdsOsdPlan, 1);
  fo = cairo_font_options_create ();
  p->font_map = pango_cairo_font_map_new ();
  p->context = pango_font_map_create_context (p->font_map);
  pango_cairo_context_set_resolution (p->context, TEXT_DPI);
  /* greyscale antialiasing (the mask is A8), no hinting of metrics: positions do not depend on the offsets */
  cairo_font_options_set_antialias (fo, CAIRO_ANTIALIAS_GRAY);
  cairo_font_options_set_hint_metrics (fo, CAIRO_HINT_METRICS_OFF);
  pango_cairo_context_set_font_options (p->context, fo);
  cairo_font_options_destroy (fo);
  p->layout = pango_layout_new (p->context);
  p->fonts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      (GDestroyNotify) pango_font_description_free);
  p->layouts = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /* untouched pages cost nothing until a mask lands there */
  p->heap = g_malloc (heap_size);
  p->heap_size = heap_size;
  p->masks = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, (GDestroyNotify) mask_free);
  p->by_offset = g_ptr_array_new ();
  g_queue_init (&p->lru);

  p->frames = g_array_new (FALSE, FALSE, sizeof (RdsOsdPlanFrame));
  p->requests = g_array_new (FALSE, FALSE, sizeof (MaskRequest));
  p->keys = g_string_new (NULL);
  p->font_key = g_string_new (NULL);
  p->text_key = g_string_new (NULL);
  p->pass_budget = RDS_OSD_PASS_BUDGET;
  p->pass_frames = g_array_new (FALSE, FALSE, sizeof (RdsOsdPlanFrame));
  p->uploads = g_array_new (FALSE, FALSE, sizeof (RdsOsdMaskUpload));
  p->items = g_array_new (FALSE, FALSE, sizeof (RdsOsdWorkItem));
  p->lists = g_array_new (FALSE, FALSE, sizeof (guint32));
  p->tiles = g_array_new (FALSE, FALSE, sizeof (guint32));
  p->pass_slots = g_array_new (FALSE, FALSE, sizeof (guint32));
  p->grids = g_array_new (FALSE, FALSE, sizeof (Grid));
  p->grid_of = g_array_new (FALSE, FALSE, sizeof (guint));
  return p;
}

void
rds_osd_plan_free (RdsOsdPlan * p)
{
  if (p == NULL)
    return;
  g_free (p->cmds);
  g_array_unref (p->frames);
  g_array_unref (p->requests);
  g_array_unref (p->pass_frames);
  g_array_unref (p->uploads);
  g_array_unref (p->items);
  g_array_unref (p->lists);
  g_array_unref (p->tiles);
  g_array_unref (p->pass_slots);
  g_array_unref (p->grids);
  g_array_unref (p->grid_of);
  g_string_free (p->keys, TRUE);
  g_string_free (p->font_key, TRUE);
  g_string_free (p->text_key, TRUE);
  g_ptr_array_unref (p->by_offset);
  g_hash_table_unref (p->masks);
  g_free (p->heap);
  g_hash_table_unref (p->layouts);
  g_hash_table_unref (p->fonts);
  g_object_unref (p->layout);
  g_object_unref (p->context);
  g_object_unref (p->font_map);
  g_free (p);
}
