/* Redstream: the rdsanalytics overlays as display meta; see rds-analytics-draw.h for what is drawn.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-draw.h"

#include <math.h>
#include <string.h>

/* DeepStream 8.0 behaviour */
#define LINES_PER_META 15
#define LINE_WIDTH 2
#define FONT_NAME "Serif"
#define OBJ_TEXT_ABOVE 10               /* px from the object's top to its text's */
#define OBJ_CNT_X 5
#define OBJ_CNT_Y 5
#define ARROW_HEAD_RATIO 0.15           /* of the vector's length */
#define ARROW_HEAD_ANGLE (G_PI / 12)    /* 15 degrees */
/* ours: 8.0 never showed more texts than fit one display meta */
#define TEXTS_PER_META RDS_MAX_ELEMENTS_IN_DISPLAY_META

static const RdsOSD_ColorParams yellow = { 1, 1, 0, 1 };
static const RdsOSD_ColorParams orange = { 1, 0.5, 0, 1 };
static const RdsOSD_ColorParams red = { 1, 0, 0, 1 };
static const RdsOSD_ColorParams green = { 0, 1, 0, 1 };
static const RdsOSD_ColorParams cyan = { 0, 1, 1, 1 };
static const RdsOSD_ColorParams white = { 1, 1, 1, 1 };
static const RdsOSD_ColorParams black = { 0, 0, 0, 1 };

/* The frame's display metas while they fill: texts and lines each go in the first with room. */
typedef struct
{
  RdsFrameMeta *fm;
  RdsBatchMeta *bm;
  guint font_size;
  /* the metas in the frame's list order: in local, or in a heap copy past its size */
  RdsDisplayMeta *local[8];
  RdsDisplayMeta **metas;
  guint n_metas, cap;
  guint text_meta, line_meta;   /* the first meta that may have room */
} Overlay;

/* A coordinate truncated to an integer (DeepStream 8.0 behaviour); below 0 (or NaN) it becomes 0 (ours: the fields
 * are unsigned). */
static guint
coord (gdouble v)
{
  if (!(v > 0))
    return 0;
  if (v >= (gdouble) G_MAXUINT)
    return G_MAXUINT;
  return (guint) v;
}

/* The display meta @*cursor, or the next one with room (acquired and added to the frame when there is none). NULL
 * when the pool gives none or the frame refuses it. */
static RdsDisplayMeta *
meta_with_room (Overlay * o, guint * cursor, gboolean for_line)
{
  for (;;) {
    RdsDisplayMeta *dm;

    if (*cursor == o->n_metas) {
      if (!(dm = rds_acquire_display_meta_from_pool (o->bm)))
        return NULL;
      /* the add can refuse the meta (it logs why) without saying so: check it landed. One refused holds nothing
       * yet, and the batch releases metas acquired but never added when it is destroyed, so it is just skipped */
      rds_add_display_meta_to_frame (o->fm, dm);
      if (!g_list_find (o->fm->display_meta_list, dm))
        return NULL;
      if (o->n_metas == o->cap) {
        RdsDisplayMeta **grown = g_new (RdsDisplayMeta *, o->cap * 2);

        memcpy (grown, o->metas, o->n_metas * sizeof *grown);
        if (o->metas != o->local)
          g_free (o->metas);
        o->metas = grown;
        o->cap *= 2;
      }
      o->metas[o->n_metas++] = dm;
    }
    dm = o->metas[*cursor];
    if (for_line ? dm->num_lines < LINES_PER_META : dm->num_labels < TEXTS_PER_META)
      return dm;
    (*cursor)++;
  }
}

static void
add_line (Overlay * o, guint x1, guint y1, guint x2, guint y2, const RdsOSD_ColorParams * c)
{
  RdsDisplayMeta *dm = meta_with_room (o, &o->line_meta, TRUE);
  RdsOSD_LineParams *lp;

  if (!dm)
    return;
  lp = &dm->line_params[dm->num_lines++];
  *lp = (RdsOSD_LineParams) {.x1 = x1,.y1 = y1,.x2 = x2,.y2 = y2,.line_width = LINE_WIDTH,.line_color = *c };
}

static void
set_text (RdsOSD_TextParams * tp, gchar * text, guint x, guint y, guint font_size, const RdsOSD_ColorParams * c)
{
  g_free (tp->display_text);
  tp->display_text = text;
  tp->x_offset = x;
  tp->y_offset = y;
  tp->font_params.font_name = (gchar *) FONT_NAME;
  tp->font_params.font_size = font_size;
  tp->font_params.font_color = *c;
  tp->set_bg_clr = 1;
  tp->text_bg_clr = black;
}

/* Takes @text. */
static void
add_text (Overlay * o, gchar * text, guint x, guint y, const RdsOSD_ColorParams * c)
{
  RdsDisplayMeta *dm = meta_with_room (o, &o->text_meta, FALSE);

  if (!dm) {
    g_free (text);
    return;
  }
  set_text (&dm->text_params[dm->num_labels++], text, x, y, o->font_size, c);
}

static void
add_point_text (Overlay * o, gchar * text, RdsAnalyticsPoint p, const RdsOSD_ColorParams * c)
{
  add_text (o, text, coord (p.x), coord (p.y), c);
}

/* A polygon's edges, the closing one last (DeepStream 8.0 behaviour). */
static void
add_polygon (Overlay * o, const RdsAnalyticsRule * r, const RdsOSD_ColorParams * c)
{
  for (guint i = 0; i < r->n_points; i++) {
    RdsAnalyticsPoint a = r->points[i], b = r->points[(i + 1) % r->n_points];

    add_line (o, coord (a.x), coord (a.y), coord (b.x), coord (b.y), c);
  }
}

/* The vector @a -> @b, then its two arrowhead lines from 15 % of its length back off the tip, at -15 then +15
 * degrees, to the tip (DeepStream 8.0 behaviour). */
static void
add_arrow (Overlay * o, RdsAnalyticsPoint a, RdsAnalyticsPoint b, const RdsOSD_ColorParams * c)
{
  gdouble dx = (gdouble) b.x - a.x, dy = (gdouble) b.y - a.y;
  gdouble head = ARROW_HEAD_RATIO * sqrt (dx * dx + dy * dy), theta = atan2 (dy, dx);
  guint tx = coord (b.x), ty = coord (b.y);

  add_line (o, coord (a.x), coord (a.y), tx, ty, c);
  add_line (o, coord (b.x - head * cos (theta - ARROW_HEAD_ANGLE)), coord (b.y - head * sin (theta - ARROW_HEAD_ANGLE)),
      tx, ty, c);
  add_line (o, coord (b.x - head * cos (theta + ARROW_HEAD_ANGLE)), coord (b.y - head * sin (theta + ARROW_HEAD_ANGLE)),
      tx, ty, c);
}

/* "Count for" and " ClassId<c>=<n>" per objCnt entry, in its order (DeepStream 8.0 behaviour), the class signed
 * (ours: 8.0 prints -1 as 4294967295). */
static void
add_obj_cnt (Overlay * o, const RdsAnalyticsFrameResult * res)
{
  GString *s = g_string_new ("Count for");

  for (guint i = 0; i < res->n_obj_cnt; i++)
    g_string_append_printf (s, " ClassId%d=%u", res->obj_cnt[i].class_id, res->obj_cnt[i].count);
  add_text (o, g_string_free (s, FALSE), OBJ_CNT_X, OBJ_CNT_Y, &yellow);
}

/* The count of ROI @label of @feature in the result (0 if it isn't there, which the core never does). */
static guint
roi_count (const RdsAnalyticsFrameResult * res, RdsAnalyticsFeature feature, const gchar * label)
{
  for (guint i = 0; i < res->n_roi_counts; i++)
    if (res->roi_counts[i].feature == feature && g_strcmp0 (res->roi_counts[i].label, label) == 0)
      return res->roi_counts[i].count;
  return 0;
}

static void
add_rules (Overlay * o, const RdsAnalyticsCore * core, guint pad, const RdsAnalyticsFrameResult * res,
    gboolean counts)
{
  const RdsAnalyticsRule *r;
  guint n;

  r = rds_analytics_core_get_rules (core, pad, RDS_ANALYTICS_ROI_FILTERING, &n);
  for (guint i = 0; i < n; i++) {
    add_polygon (o, &r[i], &yellow);
    add_point_text (o, counts ? g_strdup_printf ("%s=%u", r[i].label, roi_count (res,
                RDS_ANALYTICS_ROI_FILTERING, r[i].label)) : g_strdup (r[i].label), r[i].points[0], &yellow);
  }

  r = rds_analytics_core_get_rules (core, pad, RDS_ANALYTICS_OVERCROWDING, &n);
  for (guint i = 0; i < n; i++) {
    gchar *text = NULL;

    add_polygon (o, &r[i], &orange);
    for (guint k = 0; counts && k < res->n_oc && !text; k++)
      if (g_strcmp0 (res->oc[k].label, r[i].label) == 0)
        text = g_strdup_printf ("%s OverCrowding=%s, Count=%u", r[i].label, res->oc[k].status ? "True" : "False",
            res->oc[k].count);
    /* text stays NULL in osd-mode 1; in 2 it never does with the core, which lists every overcrowding label */
    add_point_text (o, text ? text : g_strdup (r[i].label), r[i].points[0], &orange);
  }

  /* the label doesn't change in osd-mode 2 */
  r = rds_analytics_core_get_rules (core, pad, RDS_ANALYTICS_DIRECTION, &n);
  for (guint i = 0; i < n; i++) {
    add_arrow (o, r[i].points[0], r[i].points[1], &red);
    add_point_text (o, g_strdup (r[i].label), r[i].points[0], &red);
  }

  /* the direction vector, then the line; the label at the line's second point */
  r = rds_analytics_core_get_rules (core, pad, RDS_ANALYTICS_LINE_CROSSING, &n);
  for (guint i = 0; i < n; i++) {
    guint64 cum = 0;            /* never left 0 by a missing label with the core, which lists every line */

    add_arrow (o, r[i].points[0], r[i].points[1], &green);
    add_line (o, coord (r[i].points[2].x), coord (r[i].points[2].y), coord (r[i].points[3].x),
        coord (r[i].points[3].y), &green);
    for (guint k = 0; k < res->n_lc; k++)
      if (g_strcmp0 (res->lc[k].label, r[i].label) == 0)
        cum = res->lc[k].cumulative;
    add_point_text (o, counts ? g_strdup_printf ("%s=%" G_GUINT64_FORMAT, r[i].label, cum) :
        g_strdup_printf ("%s ", r[i].label), r[i].points[3], &green);
  }
}

/* osd-mode 2's object text and border (DeepStream 8.0 behaviour). */
static void
write_objects (const RdsAnalyticsFrameResult * res, RdsObjectMeta * const *objs, guint font_size)
{
  for (guint i = 0; i < res->n_objects; i++) {
    const RdsAnalyticsObjectResult *r = &res->objects[i];
    RdsObjectMeta *om = objs[i];

    if (!r->has_info)
      continue;
    set_text (&om->text_params, g_strdup (r->obj_status), coord (om->rect_params.left),
        coord ((gdouble) om->rect_params.top - OBJ_TEXT_ABOVE), font_size, &white);
    /* an ROI or a line crossing; a direction, STOPPED or overcrowding alone keep the border */
    if (r->n_roi_labels > 0 || r->n_lc_labels > 0)
      om->rect_params.border_color = cyan;
  }
}

void
rds_analytics_draw_frame (RdsFrameMeta * fm, const RdsAnalyticsConfig * cfg, const RdsAnalyticsCore * core,
    const RdsAnalyticsFrameResult * res, RdsObjectMeta * const *objs)
{
  Overlay o = { 0 };

  g_return_if_fail (fm != NULL && cfg != NULL && core != NULL && res != NULL);
  g_return_if_fail (res->n_objects == 0 || objs != NULL);
  if (!cfg->display_obj_cnt && cfg->osd_mode == 0)
    return;                     /* nothing to draw (DeepStream 8.0 behaviour) */

  o.fm = fm;
  o.bm = fm->base_meta.batch_meta;
  o.font_size = cfg->display_font_size;
  o.metas = o.local;
  o.cap = G_N_ELEMENTS (o.local);
  /* in every osd-mode, before the labels (DeepStream 8.0 behaviour) */
  if (cfg->display_obj_cnt)
    add_obj_cnt (&o, res);
  if (cfg->osd_mode >= 1)
    add_rules (&o, core, fm->pad_index, res, cfg->osd_mode >= 2);
  if (cfg->osd_mode >= 2)
    write_objects (res, objs, cfg->display_font_size);
  if (o.metas != o.local)
    g_free (o.metas);
}
