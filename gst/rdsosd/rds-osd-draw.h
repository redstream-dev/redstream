/* Redstream: rdsosd's drawing core. Draws one frame's object meta, display meta and clock onto a CPU RGBA plane, with
 * Cairo (antialiasing off) for lines, arrows and circles, Pango for text, and plain pixel loops for rectangles. No
 * GStreamer and no GPU: the element maps and syncs the surface, the tests draw onto plain host buffers.
 *
 * The geometry follows DeepStream 8.0's default (GPU) mode, where its docs are silent ("DeepStream 8.0 behaviour" in
 * rds-osd-draw.c); our own choices say "ours". docs/compat-matrix.md lists both.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_OSD_DRAW_H
#define RDS_OSD_DRAW_H

#include <rds/rds-meta.h>

G_BEGIN_DECLS

/* MAX_BORDER_WIDTH in DeepStream: wider borders are drawn this wide (ours; DeepStream 8.0 stops the pipeline) */
#define RDS_OSD_MAX_BORDER_WIDTH 32
/* clock defaults when clock-font / clock-font-size are unset (DeepStream 8.0 behaviour: a serif, ~12 px digits) */
#define RDS_OSD_CLOCK_FONT "Serif"
#define RDS_OSD_CLOCK_FONT_SIZE 12
/* text whose font_name is NULL, empty or unknown to fontconfig */
#define RDS_OSD_DEFAULT_FONT "Sans"

typedef struct
{
  gboolean display_bbox;        /* object and display-meta rects */
  gboolean display_text;        /* object text, display-meta labels and the clock */
  gboolean display_clock;
  const gchar *clock_text;      /* drawn when display_clock and display_text; NULL draws nothing */
  const gchar *clock_font;      /* NULL: RDS_OSD_CLOCK_FONT */
  guint clock_font_size;        /* points; 0: RDS_OSD_CLOCK_FONT_SIZE */
  guint x_clock_offset, y_clock_offset;
  RdsOSD_ColorParams clock_color;
} RdsOsdDrawOptions;

/* What rds_osd_draw_frame had to adjust, for the caller to report (once). */
typedef enum
{
  RDS_OSD_DRAW_BORDER_CLAMPED = 1 << 0,         /* a border_width above RDS_OSD_MAX_BORDER_WIDTH */
} RdsOsdDrawFlags;

typedef struct _RdsOsdDraw RdsOsdDraw;

/* A drawing context: font map, layout, font cache and scratch memory. Not thread-safe: one per streaming thread. */
RdsOsdDraw *rds_osd_draw_new (void);
void rds_osd_draw_free (RdsOsdDraw * draw);

/* Draws @frame's objects (rect_params, text_params) and display metas, then the clock, onto the RGBA plane @data
 * (@width x @height pixels, @stride bytes per row). Order (DeepStream 8.0 behaviour): every rect (objects, then
 * display metas), every text (objects, then display-meta labels), lines, arrows, circles, the clock; within each kind
 * in list order, later on top. Masks are not drawn. @frame may be NULL (the clock only). */
RdsOsdDrawFlags rds_osd_draw_frame (RdsOsdDraw * draw, const RdsOsdDrawOptions * options,
    const RdsFrameMeta * frame, guint8 * data, gint width, gint height, gint stride);

/* FALSE when rds_osd_draw_frame would draw nothing for @frame (may be NULL) under @options: no visible object rect
 * or text, no display-meta element the switches show, no clock. The caller can then skip mapping the frame. */
gboolean rds_osd_draw_frame_has_work (const RdsOsdDrawOptions * options, const RdsFrameMeta * frame);

/* Loads the default fonts, so the first frame does not pay for fontconfig's start-up. */
void rds_osd_draw_warm_up (RdsOsdDraw * draw);

/* The family fontconfig resolves @name (as a text's font_name) to, e.g. "DejaVu Serif"; NULL if none. Free with
 * g_free. */
gchar *rds_osd_draw_resolve_font (RdsOsdDraw * draw, const gchar * name);

/* The clock text for @t: "YYYY-MM-DD HH:MM:SS" (DeepStream 8.0 behaviour). Free with g_free. */
gchar *rds_osd_clock_text (GDateTime * t);

/* 0xRRGGBBAA (the clock-color property) as colour params. */
RdsOSD_ColorParams rds_osd_color_from_rgba (guint32 rgba);

G_END_DECLS

#endif /* RDS_OSD_DRAW_H */
