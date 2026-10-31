/* Redstream: rdsinfer's secondary-mode geometry, see rds-infer-units.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-units.h"

#include <math.h>
#include <string.h>

gboolean
rds_infer_object_selected (const RdsInferConfig * cfg, gint unique_component_id, gint class_id, gfloat w, gfloat h)
{
  gboolean listed = FALSE;

  /* 1. The producer: -1 is every one (DeepStream 8.0 behaviour). */
  if (cfg->operate_on_gie_id != -1 && cfg->operate_on_gie_id != unique_component_id)
    return FALSE;

  /* 2. The class: an empty list is every class (DeepStream 8.0 behaviour); with a list, a negative class id is never
   * selected (ours). */
  if (cfg->operate_on_class_ids->len > 0) {
    if (class_id < 0)
      return FALSE;
    for (guint i = 0; i < cfg->operate_on_class_ids->len && !listed; i++)
      listed = g_array_index (cfg->operate_on_class_ids, gint, i) == class_id;
    if (!listed)
      return FALSE;
  }

  /* 3. The size, inclusive, 0: no limit (DeepStream 8.0 behaviour); not finite or not > 0: not selected (ours). */
  if (!isfinite (w) || !isfinite (h) || w <= 0 || h <= 0)
    return FALSE;
  if ((cfg->input_object_min_width > 0 && w < (gdouble) cfg->input_object_min_width)
      || (cfg->input_object_min_height > 0 && h < (gdouble) cfg->input_object_min_height))
    return FALSE;
  if ((cfg->input_object_max_width > 0 && w > (gdouble) cfg->input_object_max_width)
      || (cfg->input_object_max_height > 0 && h > (gdouble) cfg->input_object_max_height))
    return FALSE;
  return TRUE;
}

/* Steps 2-4 of rds_infer_crop_rect on one axis (@size >= 1, both finite). In double: a float's integer part is exact
 * there, and so is fmod. */
static gboolean
crop_axis (gfloat pos, gfloat size, guint frame, guint32 * out_pos, guint32 * out_size)
{
  gdouble start = floor (pos), len = floor (size), end;

  /* 2. Origin up to even, size down to even, at least 2. */
  if (fmod (start, 2) != 0)
    start += 1;
  len = MAX (len - fmod (len, 2), 2);
  /* 3. Clip the aligned span to the frame; start stays even (or becomes 0). */
  end = MIN (start + len, (gdouble) frame);
  start = MAX (start, 0);
  if (end - start < 2)
    return FALSE;
  /* 4. Down to even again. */
  len = end - start;
  len -= fmod (len, 2);
  *out_pos = (guint32) start;
  *out_size = (guint32) len;
  return TRUE;
}

gboolean
rds_infer_crop_rect (gfloat left, gfloat top, gfloat w, gfloat h, guint frame_w, guint frame_h,
    RdsBufSurfTransformRect * out)
{
  RdsBufSurfTransformRect r;

  /* 1. Under a pixel (DeepStream 8.0 behaviour), or not finite (ours). */
  if (!isfinite (left) || !isfinite (top) || !isfinite (w) || !isfinite (h) || w < 1 || h < 1)
    return FALSE;
  if (!crop_axis (left, w, frame_w, &r.left, &r.width) || !crop_axis (top, h, frame_h, &r.top, &r.height))
    return FALSE;
  *out = r;
  return TRUE;
}

/* The content size on an axis whose exact size is @num / @den (floored), floored to even, in [min (2, net), net]. */
static guint
content_size (guint64 num, guint64 den, guint net)
{
  guint64 s = num / den;

  s -= s % 2;
  return (guint) CLAMP (s, MIN (2, net), net);
}

void
rds_infer_crop_letterbox (guint net_w, guint net_h, guint crop_w, guint crop_h, gboolean keep_aspect,
    gboolean symmetric, RdsInferLetterbox * out)
{
  g_return_if_fail (out != NULL);
  memset (out, 0, sizeof (*out));
  /* Up to G_MAXINT: net * crop then fits in a guint64. */
  g_return_if_fail (net_w > 0 && net_h > 0 && crop_w > 0 && crop_h > 0);
  g_return_if_fail (net_w <= G_MAXINT && net_h <= G_MAXINT && crop_w <= G_MAXINT && crop_h <= G_MAXINT);

  out->net_w = net_w;
  out->net_h = net_h;
  out->frame_w = crop_w;
  out->frame_h = crop_h;
  out->scaled_w = net_w;
  out->scaled_h = net_h;
  if (keep_aspect) {
    /* net_w / crop_w <= net_h / crop_h: the width fills the network (exact size net_w), the exact height is
     * crop_h * net_w / crop_w; and the other way round. The pad on the filling axis is 0. */
    guint64 wh = (guint64) net_w * crop_h, hw = (guint64) net_h * crop_w;

    if (wh <= hw) {
      out->scaled_w = content_size (net_w, 1, net_w);
      out->scaled_h = content_size ((guint64) crop_h * net_w, crop_w, net_h);
      if (symmetric)
        out->pad_top = (guint) ((hw - wh) / (2 * (guint64) crop_w));
    } else {
      out->scaled_w = content_size ((guint64) crop_w * net_h, crop_h, net_w);
      out->scaled_h = content_size (net_h, 1, net_h);
      if (symmetric)
        out->pad_left = (guint) ((wh - hw) / (2 * (guint64) crop_h));
    }
  }
  out->scale_x = (gfloat) out->scaled_w / crop_w;
  out->scale_y = (gfloat) out->scaled_h / crop_h;
}

gboolean
rds_infer_map_child (const RdsBufSurfTransformRect * crop, guint frame_w, guint frame_h,
    const RdsInferDetection * in, RdsInferDetection * out)
{
  RdsInferDetection d = *in;

  d.box.left = (gfloat) ((gdouble) in->box.left + crop->left);
  d.box.top = (gfloat) ((gdouble) in->box.top + crop->top);
  if (!rds_infer_box_clip (&d.box, frame_w, frame_h, &d.rect))
    return FALSE;
  *out = d;
  return TRUE;
}
