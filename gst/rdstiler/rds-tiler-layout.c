/* Redstream: the rdsmultistreamtiler layout (see rds-tiler-layout.h).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tiler-layout.h"

/* ⌊√n⌋, exact for every guint */
static guint
isqrt_floor (guint n)
{
  guint64 lo = 0, hi = 65536;

  /* largest r with r² <= n, in [0, 65536) */
  while (hi - lo > 1) {
    guint64 mid = (lo + hi) / 2;

    if (mid * mid <= n)
      lo = mid;
    else
      hi = mid;
  }
  return (guint) lo;
}

static guint
isqrt_ceil (guint n)
{
  guint r = isqrt_floor (n);

  return (guint64) r * r == n ? r : r + 1;
}

guint
rds_tiler_round_output_size (guint v)
{
  if (v > (G_MAXUINT & ~3u))
    return G_MAXUINT & ~3u;
  return (v + 3) & ~3u;
}

void
rds_tiler_grid (guint rows, guint columns, gboolean square_seq_grid, guint n, guint * out_rows,
    guint * out_columns)
{
  guint r, c;

  n = MAX (n, 1);
  rows = MAX (rows, 1);
  columns = MAX (columns, 1);
  if (square_seq_grid) {
    r = c = isqrt_ceil (n);
  } else if ((guint64) rows * columns >= n) {
    r = rows;
    c = columns;
  } else {
    r = isqrt_floor (n);
    c = (guint) (((guint64) n + r - 1) / r);
  }
  *out_rows = r;
  *out_columns = c;
}

gboolean
rds_tiler_layout_init (RdsTilerLayout * layout, guint rows, guint columns, gboolean square_seq_grid, guint n,
    guint width, guint height)
{
  RdsTilerLayout l;

  rds_tiler_grid (rows, columns, square_seq_grid, n, &l.rows, &l.columns);
  l.width = rds_tiler_round_output_size (width);
  l.height = rds_tiler_round_output_size (height);
  l.cell_width = (l.width / l.columns) & ~1u;
  l.cell_height = (l.height / l.rows) & ~1u;
  if (l.cell_width == 0 || l.cell_height == 0)
    return FALSE;
  *layout = l;
  return TRUE;
}

gboolean
rds_tiler_layout_cell (const RdsTilerLayout * layout, guint index, RdsTilerRect * cell)
{
  if ((guint64) index >= (guint64) layout->rows * layout->columns)
    return FALSE;
  cell->left = (index % layout->columns) * layout->cell_width;
  cell->top = (index / layout->columns) * layout->cell_height;
  cell->width = layout->cell_width;
  cell->height = layout->cell_height;
  return TRUE;
}

void
rds_tiler_scale_init (RdsTilerScale * scale, guint frame_width, guint frame_height, const RdsTilerRect * dst)
{
  scale->sx = frame_width > 0 ? (gdouble) dst->width / frame_width : 1.0;
  scale->sy = frame_height > 0 ? (gdouble) dst->height / frame_height : 1.0;
  scale->ox = dst->left;
  scale->oy = dst->top;
}

static void
scale_rect (const RdsTilerScale * s, RdsOSD_RectParams * r)
{
  r->left = (gfloat) (s->ox + r->left * s->sx);
  r->top = (gfloat) (s->oy + r->top * s->sy);
  r->width = (gfloat) (r->width * s->sx);
  r->height = (gfloat) (r->height * s->sy);
}

/* The metadata values and scale factors are nonnegative. Avoid undefined/out-of-range float-to-uint casts. */
static guint
scale_uint (gdouble value)
{
  if (value > G_MAXUINT)
    return G_MAXUINT;
  return (guint) value;
}

/* truncated toward zero, as DeepStream 8.0 stores its integer coordinates */
static guint
scale_x (const RdsTilerScale * s, guint x)
{
  return scale_uint (s->ox + x * s->sx);
}

static guint
scale_y (const RdsTilerScale * s, guint y)
{
  return scale_uint (s->oy + y * s->sy);
}

static void
scale_text (const RdsTilerScale * s, RdsOSD_TextParams * t)
{
  t->x_offset = scale_x (s, t->x_offset);
  t->y_offset = scale_y (s, t->y_offset);
}

void
rds_tiler_scale_object (const RdsTilerScale * scale, RdsObjectMeta * obj)
{
  scale_rect (scale, &obj->rect_params);
  scale_text (scale, &obj->text_params);
}

void
rds_tiler_scale_display (const RdsTilerScale * scale, RdsDisplayMeta * d)
{
  const guint max = RDS_MAX_ELEMENTS_IN_DISPLAY_META;
  const gdouble smin = MIN (scale->sx, scale->sy);

  for (guint i = 0; i < MIN (d->num_rects, max); i++)
    scale_rect (scale, &d->rect_params[i]);
  for (guint i = 0; i < MIN (d->num_labels, max); i++)
    scale_text (scale, &d->text_params[i]);
  for (guint i = 0; i < MIN (d->num_lines, max); i++) {
    RdsOSD_LineParams *p = &d->line_params[i];

    p->x1 = scale_x (scale, p->x1);
    p->y1 = scale_y (scale, p->y1);
    p->x2 = scale_x (scale, p->x2);
    p->y2 = scale_y (scale, p->y2);
  }
  for (guint i = 0; i < MIN (d->num_arrows, max); i++) {
    RdsOSD_ArrowParams *p = &d->arrow_params[i];

    p->x1 = scale_x (scale, p->x1);
    p->y1 = scale_y (scale, p->y1);
    p->x2 = scale_x (scale, p->x2);
    p->y2 = scale_y (scale, p->y2);
  }
  for (guint i = 0; i < MIN (d->num_circles, max); i++) {
    RdsOSD_CircleParams *p = &d->circle_params[i];

    p->xc = scale_x (scale, p->xc);
    p->yc = scale_y (scale, p->yc);
    p->radius = scale_uint (p->radius * smin);
  }
}
