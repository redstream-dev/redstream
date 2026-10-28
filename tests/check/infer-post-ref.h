/* A deliberately naive CPU reference of rdsinfer's detection post-processing (rds-infer-post.h documents the
 * steps), for the randomised test in infer_post.c. Written for obviousness, not speed: NMS repeatedly picks the
 * best object not looked at yet (over all classes, O(n^2)) and compares it with every object kept so far.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_INFER_POST_REF_H
#define RDS_TEST_INFER_POST_REF_H

#include "rds-infer-post.h"

#include <math.h>

enum
{
  REF_DROPPED,
  REF_WAITING,                  /* passed step 1, not looked at by NMS yet */
  REF_KEPT,
};

static gdouble
ref_iou (const RdsInferObjectDetectionInfo * a, const RdsInferObjectDetectionInfo * b)
{
  gdouble iw = MIN ((gdouble) a->left + a->width, (gdouble) b->left + b->width) - MAX (a->left, b->left);
  gdouble ih = MIN ((gdouble) a->top + a->height, (gdouble) b->top + b->height) - MAX (a->top, b->top);
  gdouble inter, uni;

  if (iw < 0)
    iw = 0;
  if (ih < 0)
    ih = 0;
  inter = iw * ih;
  uni = (gdouble) a->width * a->height + (gdouble) b->width * b->height - inter;
  if (uni <= 0)
    return 0;
  return inter / uni;
}

/* TRUE if @a goes before @b in NMS order: higher confidence, then lower index. */
static gboolean
ref_before (const RdsInferObjectDetectionInfo * in, guint a, guint b)
{
  if (in[a].detectionConfidence != in[b].detectionConfidence)
    return in[a].detectionConfidence > in[b].detectionConfidence;
  return a < b;
}

static void
ref_postprocess (const RdsInferConfig * cfg, const RdsInferLetterbox * lb, const RdsInferObjectDetectionInfo * in,
    guint n, GArray * out)
{
  gint *state = g_new0 (gint, MAX (n, 1));
  guint i, j;

  /* 1. validation and the pre-cluster threshold */
  for (i = 0; i < n; i++) {
    const RdsInferObjectDetectionInfo *o = &in[i];

    state[i] = REF_DROPPED;
    if (o->classId >= cfg->num_detected_classes)
      continue;
    if (!isfinite (o->left) || !isfinite (o->top) || !isfinite (o->width) || !isfinite (o->height)
        || !isfinite (o->detectionConfidence))
      continue;
    if (o->width <= 0 || o->height <= 0)
      continue;
    if (o->detectionConfidence < cfg->class_attrs[o->classId].pre_cluster_threshold)
      continue;
    state[i] = REF_WAITING;
  }

  /* 2 and 3. NMS and top-k */
  if (cfg->cluster_mode == 2) {
    for (;;) {
      gint best = -1;
      guint kept_in_class = 0;
      gboolean suppressed = FALSE;
      const RdsInferClassAttrs *attrs;

      for (i = 0; i < n; i++)
        if (state[i] == REF_WAITING && (best < 0 || ref_before (in, i, best)))
          best = i;
      if (best < 0)
        break;
      attrs = &cfg->class_attrs[in[best].classId];
      for (j = 0; j < n; j++) {
        if (state[j] != REF_KEPT || in[j].classId != in[best].classId)
          continue;
        kept_in_class++;
        if (ref_iou (&in[j], &in[best]) > attrs->nms_iou_threshold)
          suppressed = TRUE;
      }
      if (suppressed || (attrs->topk >= 0 && kept_in_class >= (guint) attrs->topk))
        state[best] = REF_DROPPED;
      else
        state[best] = REF_KEPT;
    }
  } else {
    for (i = 0; i < n; i++)
      if (state[i] == REF_WAITING)
        state[i] = REF_KEPT;
  }

  /* 4 to 9, object by object in input order */
  for (i = 0; i < n; i++) {
    const RdsInferObjectDetectionInfo *o = &in[i];
    const RdsInferClassAttrs *attrs;
    RdsInferDetection d;
    gfloat right, bottom, cy;
    gboolean filtered = FALSE;

    if (state[i] != REF_KEPT)
      continue;
    attrs = &cfg->class_attrs[o->classId];
    if (o->detectionConfidence < attrs->post_cluster_threshold)
      continue;

    d.class_id = o->classId;
    d.confidence = o->detectionConfidence;
    d.box.left = (gfloat) (((gdouble) o->left - lb->pad_left) * lb->frame_w / lb->scaled_w);
    d.box.top = (gfloat) (((gdouble) o->top - lb->pad_top) * lb->frame_h / lb->scaled_h);
    d.box.width = (gfloat) ((gdouble) o->width * lb->frame_w / lb->scaled_w);
    d.box.height = (gfloat) ((gdouble) o->height * lb->frame_h / lb->scaled_h);

    for (j = 0; j < cfg->filter_out_class_ids->len; j++)
      if (g_array_index (cfg->filter_out_class_ids, guint, j) == o->classId)
        filtered = TRUE;
    if (filtered)
      continue;

    if (attrs->roi_top_offset > 0 || attrs->roi_bottom_offset > 0) {
      cy = d.box.top + d.box.height / 2;
      if (cy < (gfloat) attrs->roi_top_offset
          || cy >= (gfloat) lb->frame_h - (gfloat) attrs->roi_bottom_offset)
        continue;
    }

    if (d.box.width < attrs->detected_min_w || d.box.height < attrs->detected_min_h)
      continue;
    if (attrs->detected_max_w > 0 && d.box.width > attrs->detected_max_w)
      continue;
    if (attrs->detected_max_h > 0 && d.box.height > attrs->detected_max_h)
      continue;

    d.rect.left = MAX (d.box.left, 0);
    d.rect.top = MAX (d.box.top, 0);
    right = MIN (d.box.left + d.box.width, (gfloat) lb->frame_w);
    bottom = MIN (d.box.top + d.box.height, (gfloat) lb->frame_h);
    d.rect.width = right - d.rect.left;
    d.rect.height = bottom - d.rect.top;
    if (d.rect.width <= 0 || d.rect.height <= 0)
      continue;
    g_array_append_val (out, d);
  }
  g_free (state);
}

#endif /* RDS_TEST_INFER_POST_REF_H */
