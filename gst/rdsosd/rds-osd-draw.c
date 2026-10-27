/* Redstream: rdsosd's drawing core (see rds-osd-draw.h).
 *
 * A frame is drawn as a batch of one: the planner (rds-osd-plan.h) turns its metas into draw commands in draw order
 * and rasterizes the text masks into its heap; then rds_osd_shade_commands blends each shape over its bounding box
 * into the RGBA plane, pass by pass. The coverage and blend live in rds-osd-shapes.h, plain C that the GPU kernel
 * (rds-osd-gpu.hip) compiles too, so that both paths draw the same bytes. The frame's own bytes, alpha included, are
 * only ever read and blended, never reinterpreted, and only the pixels a command covers are touched.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-draw.h"
#include "rds-osd-plan.h"

struct _RdsOsdDraw
{
  RdsOsdPlan *plan;
};

/* ---- the frame ---- */

RdsOsdDrawFlags
rds_osd_draw_frame (RdsOsdDraw * d, const RdsOsdDrawOptions * opt, const RdsFrameMeta * frame, guint8 * data,
    gint width, gint height, gint stride)
{
  RdsOsdDrawFlags flags;
  RdsOsdPass pass;

  g_return_val_if_fail (d != NULL && opt != NULL && data != NULL, 0);
  if (width <= 0 || height <= 0)
    return 0;
  rds_osd_plan_begin (d->plan);
  flags = rds_osd_plan_add_frame (d->plan, opt, 0, frame, width, height);
  while (rds_osd_plan_next_pass (d->plan, &pass))
    for (guint32 i = 0; i < pass.n_frames; i++)
      rds_osd_shade_commands (pass.cmds + pass.frames[i].first, pass.frames[i].n_cmds, rds_osd_plan_heap (d->plan),
          data, width, height, (gsize) stride);
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
  rds_osd_plan_warm_up (d->plan);
}

gchar *
rds_osd_draw_resolve_font (RdsOsdDraw * d, const gchar * name)
{
  return rds_osd_plan_resolve_font (d->plan, name);
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

  d->plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  return d;
}

void
rds_osd_draw_free (RdsOsdDraw * d)
{
  if (d == NULL)
    return;
  rds_osd_plan_free (d->plan);
  g_free (d);
}
