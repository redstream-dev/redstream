/* Redstream: rdsinfer's detection post-processing. The step order is ours (DeepStream documents the steps, not
 * their order); see rds-infer-post.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-post.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void
rds_infer_letterbox (guint net_w, guint net_h, guint frame_w, guint frame_h, gboolean keep_aspect,
    gboolean symmetric, RdsInferLetterbox * out)
{
  g_return_if_fail (out != NULL);
  memset (out, 0, sizeof (*out));
  /* Up to G_MAXINT: 2 * frame * net then fits in a guint64 (< 2^63). */
  g_return_if_fail (net_w > 0 && net_h > 0 && frame_w > 0 && frame_h > 0);
  g_return_if_fail (net_w <= G_MAXINT && net_h <= G_MAXINT && frame_w <= G_MAXINT && frame_h <= G_MAXINT);

  out->net_w = net_w;
  out->net_h = net_h;
  out->frame_w = frame_w;
  out->frame_h = frame_h;
  out->pad_left = out->pad_top = 0;
  out->scaled_w = net_w;
  out->scaled_h = net_h;
  if (keep_aspect) {
    /* net_w / frame_w <= net_h / frame_h: the width fills the network, the height is rounded (halves up), and the
     * other way round. Exact in integers. */
    if ((guint64) net_w * frame_h <= (guint64) net_h * frame_w)
      out->scaled_h = (guint) ((2 * (guint64) frame_h * net_w + frame_w) / (2 * (guint64) frame_w));
    else
      out->scaled_w = (guint) ((2 * (guint64) frame_w * net_h + frame_h) / (2 * (guint64) frame_h));
    /* Ours: a frame too thin to cover one network pixel still gets one. */
    out->scaled_w = CLAMP (out->scaled_w, 1, net_w);
    out->scaled_h = CLAMP (out->scaled_h, 1, net_h);
    if (symmetric) {
      out->pad_left = (net_w - out->scaled_w) / 2;
      out->pad_top = (net_h - out->scaled_h) / 2;
    }
  }
  out->scale_x = (gfloat) out->scaled_w / frame_w;
  out->scale_y = (gfloat) out->scaled_h / frame_h;
}

void
rds_infer_letterbox_map (const RdsInferLetterbox * lb, gfloat left, gfloat top, gfloat width, gfloat height,
    gfloat out[4])
{
  out[0] = (gfloat) (((gdouble) left - lb->pad_left) * lb->frame_w / lb->scaled_w);
  out[1] = (gfloat) (((gdouble) top - lb->pad_top) * lb->frame_h / lb->scaled_h);
  out[2] = (gfloat) ((gdouble) width * lb->frame_w / lb->scaled_w);
  out[3] = (gfloat) ((gdouble) height * lb->frame_h / lb->scaled_h);
}

/* An object that passed step 1. */
typedef struct
{
  guint index;                  /* in the parser output */
  guint class_id;
  gfloat confidence;
  gdouble left, top, right, bottom, area;
} Candidate;

static gboolean
valid (const RdsInferObjectDetectionInfo * o, guint num_classes)
{
  return o->classId < num_classes && isfinite (o->left) && isfinite (o->top) && isfinite (o->width)
      && isfinite (o->height) && isfinite (o->detectionConfidence) && o->width > 0 && o->height > 0;
}

/* Class, then confidence (highest first), then input index. */
static gint
candidate_cmp (gconstpointer pa, gconstpointer pb)
{
  const Candidate *a = pa, *b = pb;

  if (a->class_id != b->class_id)
    return a->class_id < b->class_id ? -1 : 1;
  if (a->confidence != b->confidence)
    return a->confidence > b->confidence ? -1 : 1;
  return a->index < b->index ? -1 : a->index > b->index;
}

static gint
index_cmp (gconstpointer pa, gconstpointer pb)
{
  const Candidate *a = pa, *b = pb;

  return a->index < b->index ? -1 : a->index > b->index;
}

static gdouble
iou (const Candidate * a, const Candidate * b)
{
  gdouble iw = MIN (a->right, b->right) - MAX (a->left, b->left);
  gdouble ih = MIN (a->bottom, b->bottom) - MAX (a->top, b->top);
  gdouble inter, uni;

  if (iw <= 0 || ih <= 0)
    return 0;
  inter = iw * ih;
  uni = a->area + b->area - inter;
  return uni > 0 ? inter / uni : 0;
}

/* Steps 2 and 3 on @c (sorted by candidate_cmp): moves the survivors to the front, returns how many. */
static guint
nms_topk (const RdsInferConfig * cfg, Candidate * c, guint n)
{
  guint n_kept = 0, start = 0;

  while (start < n) {
    const RdsInferClassAttrs *attrs = &cfg->class_attrs[c[start].class_id];
    guint end = start, class_start = n_kept;

    while (end < n && c[end].class_id == c[start].class_id)
      end++;
    for (guint i = start; i < end; i++) {
      gboolean suppressed = FALSE;

      if (attrs->topk >= 0 && n_kept - class_start >= (guint) attrs->topk)
        break;
      for (guint k = class_start; k < n_kept && !suppressed; k++)
        suppressed = iou (&c[k], &c[i]) > attrs->nms_iou_threshold;
      if (!suppressed)
        c[n_kept++] = c[i];
    }
    start = end;
  }
  return n_kept;
}

RdsInferPostFlags
rds_infer_postprocess (const RdsInferConfig * cfg, const RdsInferLetterbox * lb,
    const RdsInferObjectDetectionInfo * in, guint n_in, GByteArray * scratch, GArray * out)
{
  RdsInferPostFlags flags = 0;
  Candidate *c;
  guint n = 0;

  g_return_val_if_fail (cfg != NULL && lb != NULL && out != NULL, 0);
  g_return_val_if_fail (lb->scaled_w > 0 && lb->scaled_h > 0, 0);
  g_return_val_if_fail (n_in == 0 || in != NULL, 0);
  g_return_val_if_fail (n_in <= G_MAXUINT / sizeof (Candidate), 0);
  g_return_val_if_fail (g_array_get_element_size (out) == sizeof (RdsInferDetection), 0);
  g_return_val_if_fail (cfg->cluster_mode == 2 || cfg->cluster_mode == 4, 0);

  if (n_in == 0)
    return 0;
  if (scratch) {
    g_byte_array_set_size (scratch, n_in * sizeof (Candidate));
    c = (Candidate *) (gpointer) scratch->data;
  } else {
    c = g_new (Candidate, n_in);
  }

  /* 1. Validation, then the pre-cluster threshold (as the parser contract asks parsers to; custom ones may not). */
  for (guint i = 0; i < n_in; i++) {
    const RdsInferObjectDetectionInfo *o = &in[i];
    Candidate *k = &c[n];

    if (!valid (o, cfg->num_detected_classes)) {
      flags |= RDS_INFER_POST_INVALID;
      continue;
    }
    if (o->rotation_angle != 0.0f)
      flags |= RDS_INFER_POST_ROTATED;
    if (o->detectionConfidence < cfg->class_attrs[o->classId].pre_cluster_threshold)
      continue;
    k->index = i;
    k->class_id = o->classId;
    k->confidence = o->detectionConfidence;
    k->left = o->left;
    k->top = o->top;
    k->right = (gdouble) o->left + o->width;
    k->bottom = (gdouble) o->top + o->height;
    k->area = (gdouble) o->width * o->height;
    n++;
  }

  /* 2 and 3. NMS and topk, per class; then back to input order. */
  if (cfg->cluster_mode == 2 && n > 0) {
    qsort (c, n, sizeof (Candidate), candidate_cmp);
    n = nms_topk (cfg, c, n);
    qsort (c, n, sizeof (Candidate), index_cmp);
  }

  for (guint i = 0; i < n; i++) {
    const RdsInferObjectDetectionInfo *o = &in[c[i].index];
    const RdsInferClassAttrs *attrs = &cfg->class_attrs[o->classId];
    RdsInferDetection d;
    gfloat m[4], right, bottom, cy;
    gboolean filtered = FALSE;

    /* 4. Post-cluster threshold. */
    if (o->detectionConfidence < attrs->post_cluster_threshold)
      continue;

    /* 5. To frame pixels. */
    rds_infer_letterbox_map (lb, o->left, o->top, o->width, o->height, m);
    d.class_id = o->classId;
    d.confidence = o->detectionConfidence;
    d.box.left = m[0];
    d.box.top = m[1];
    d.box.width = m[2];
    d.box.height = m[3];
    /* Finite in network pixels, but too big for a float in frame pixels. */
    if (!isfinite (m[0]) || !isfinite (m[1]) || !isfinite (m[2]) || !isfinite (m[3])) {
      flags |= RDS_INFER_POST_INVALID;
      continue;
    }

    /* 6. filter-out-class-ids. */
    for (guint j = 0; j < cfg->filter_out_class_ids->len && !filtered; j++)
      filtered = g_array_index (cfg->filter_out_class_ids, guint, j) == d.class_id;
    if (filtered)
      continue;

    /* 7. The ROI band, on the box's centre (ours); off when both offsets are 0 (ours). */
    if (attrs->roi_top_offset > 0 || attrs->roi_bottom_offset > 0) {
      cy = d.box.top + d.box.height / 2;
      if (cy < (gfloat) attrs->roi_top_offset || cy >= (gfloat) lb->frame_h - (gfloat) attrs->roi_bottom_offset)
        continue;
    }

    /* 8. Size limits on the unclipped box. */
    if (d.box.width < attrs->detected_min_w || d.box.height < attrs->detected_min_h)
      continue;
    if ((attrs->detected_max_w > 0 && d.box.width > attrs->detected_max_w)
        || (attrs->detected_max_h > 0 && d.box.height > attrs->detected_max_h))
      continue;

    /* 9. Clip to the frame. */
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
  if (!scratch)
    g_free (c);
  return flags;
}
