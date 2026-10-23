/* Redstream: drawing parameter structs used by object and display metadata.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_OSD_TYPES_H
#define RDS_OSD_TYPES_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct
{
  double red, green, blue, alpha;                   /* 0.0 .. 1.0 */
} RdsOSD_ColorParams;

typedef struct
{
  gchar *font_name;                                 /* not owned: usually a string literal */
  guint font_size;
  RdsOSD_ColorParams font_color;
} RdsOSD_FontParams;

typedef struct
{
  gchar *display_text;                              /* owned: g_malloc'd, freed when the meta is released */
  guint x_offset;
  guint y_offset;
  RdsOSD_FontParams font_params;
  gint set_bg_clr;
  RdsOSD_ColorParams text_bg_clr;
} RdsOSD_TextParams;

typedef struct
{
  gfloat left, top, width, height;
  guint border_width;
  RdsOSD_ColorParams border_color;
  guint has_bg_color;
  guint reserved;
  RdsOSD_ColorParams bg_color;
  gint has_color_info;
  gint color_id;
} RdsOSD_RectParams;

typedef struct
{
  guint x1, y1, x2, y2;
  guint line_width;
  RdsOSD_ColorParams line_color;
} RdsOSD_LineParams;

typedef enum
{
  RDS_OSD_ARROW_START_HEAD,
  RDS_OSD_ARROW_END_HEAD,
  RDS_OSD_ARROW_BOTH_HEAD,
} RdsOSD_Arrow_Head_Direction;

typedef struct
{
  guint x1, y1, x2, y2;
  guint arrow_width;
  RdsOSD_Arrow_Head_Direction arrow_head;
  RdsOSD_ColorParams arrow_color;
  guint reserved;
} RdsOSD_ArrowParams;

typedef struct
{
  guint xc, yc;
  guint radius;
  RdsOSD_ColorParams circle_color;
  guint has_bg_color;
  RdsOSD_ColorParams bg_color;
  guint reserved;
} RdsOSD_CircleParams;

typedef struct
{
  gfloat *data;                                     /* owned: g_malloc'd, freed when the meta is released */
  guint size;                                       /* bytes */
  gfloat threshold;
  guint width, height;
} RdsOSD_MaskParams;

G_END_DECLS

#endif /* RDS_OSD_TYPES_H */
