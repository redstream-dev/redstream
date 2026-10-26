/* Redstream: how the object metas our elements create are drawn (plugin-internal, not installed). The defaults are
 * ours: DeepStream documents none. Header-only: it comes with rds-plugin-common's include directory (rdsinfer gets
 * that through rdsinfer-core, rdstracker through rdstracker-core), and there is nothing to link.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_OBJECT_STYLE_H
#define RDS_OBJECT_STYLE_H

#include <rds/rds-meta.h>

G_BEGIN_DECLS

#define RDS_OBJECT_BORDER_WIDTH 3
#define RDS_OBJECT_TEXT_FONT "Serif"
#define RDS_OBJECT_TEXT_FONT_SIZE 10
#define RDS_OBJECT_TEXT_Y_ABOVE 20

/* A text offset from a box coordinate: 0 for a negative or NaN one, saturated above (a float-to-int cast of a value
 * out of range is undefined). */
static inline guint
rds_object_text_offset (gfloat v)
{
  return v > 0 ? (guint) MIN ((gdouble) v, (gdouble) G_MAXUINT) : 0;
}

/* Sets @om's drawing fields from its rect_params box: a red border, no background, and the label text white on black
 * above the box (at its left, 20 px up, clamped to 0). The box and text_params.display_text are the caller's. */
static inline void
rds_object_meta_set_drawing_defaults (RdsObjectMeta * om)
{
  RdsOSD_TextParams *tp = &om->text_params;
  const RdsOSD_ColorParams red = { 1.0, 0.0, 0.0, 1.0 }, white = { 1.0, 1.0, 1.0, 1.0 }, black = { 0.0, 0.0, 0.0,
    1.0
  };

  om->rect_params.border_width = RDS_OBJECT_BORDER_WIDTH;
  om->rect_params.border_color = red;
  om->rect_params.has_bg_color = 0;
  tp->x_offset = rds_object_text_offset (om->rect_params.left);
  tp->y_offset = rds_object_text_offset (om->rect_params.top - RDS_OBJECT_TEXT_Y_ABOVE);
  tp->font_params.font_name = (gchar *) RDS_OBJECT_TEXT_FONT;
  tp->font_params.font_size = RDS_OBJECT_TEXT_FONT_SIZE;
  tp->font_params.font_color = white;
  tp->set_bg_clr = 1;
  tp->text_bg_clr = black;
}

G_END_DECLS

#endif /* RDS_OBJECT_STYLE_H */
