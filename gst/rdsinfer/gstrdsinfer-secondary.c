/* Redstream: rdsinfer's secondary mode (process-mode=2): the objects of a batch read into inference units, a
 * detector's children mapped back to the frame, a classifier's results, and a chunk's results attached to its
 * objects. The per-batch flow that calls these is in gstrdsinfer.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsinfer-private.h"
#include "rds-infer-units.h"

#include <string.h>

#define GST_CAT_DEFAULT gst_rds_infer_debug

/* Secondary mode: reads the objects of every frame (under the meta lock) into self->units, one unit per selected
 * object (rds_infer_object_selected) whose crop isn't empty (rds_infer_crop_rect), in frame list order, then object
 * list order (DeepStream 8.0 behaviour). Every frame is read, bInferDone or not. Unit i goes into image
 * i % batch-size: the units run in chunks of batch-size in this order. FALSE with an element error on a bad
 * batch. */
gboolean
gst_rds_infer_snapshot_objects (GstRdsInfer * self, RdsBatchMeta * bm, const RdsBufSurface * surf)
{
  RdsInferConfig *cfg = self->cfg;
  gboolean ok = TRUE;

  g_array_set_size (self->units, 0);
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    const RdsBufSurfaceParams *sp;

    if (fm->batch_id >= surf->batchSize) {
      ok = FALSE;
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("Frame in batch slot %u, but the batch has %u slots", fm->batch_id,
              surf->batchSize), (NULL));
      break;
    }
    sp = &surf->surfaceList[fm->batch_id];
    for (GList * o = fm->obj_meta_list; o; o = o->next) {
      RdsObjectMeta *om = o->data;
      const RdsOSD_RectParams *r = &om->rect_params;
      InferUnit unit = {.source_id = fm->source_id,.slot = fm->batch_id };

      if (!rds_infer_object_selected (cfg, om->unique_component_id, om->class_id, r->width, r->height))
        continue;
      if (!rds_infer_crop_rect (r->left, r->top, r->width, r->height, sp->width, sp->height, &unit.src)) {
        GST_DEBUG_OBJECT (self, "source %u frame %d: object at (%g, %g) %gx%g has an empty crop in %ux%u, skipped",
            fm->source_id, fm->frame_num, r->left, r->top, r->width, r->height, sp->width, sp->height);
        continue;
      }
      unit.frame = rds_meta_ref (fm);
      unit.obj = rds_meta_ref (om);
      unit.image = self->units->len % cfg->batch_size;
      rds_infer_crop_letterbox (self->net.width, self->net.height, unit.src.width, unit.src.height,
          cfg->maintain_aspect_ratio, cfg->symmetric_padding, &unit.lb);
      g_array_append_val (self->units, unit);
    }
  }
  rds_release_meta_lock (bm);
  return ok;
}

/* Secondary mode: @unit's detections (the last ones in self->dets, in crop pixels) to frame pixels in its @sp-sized
 * slot (rds_infer_map_child), dropping those left empty. */
void
gst_rds_infer_map_children (GstRdsInfer * self, const InferUnit * unit, const RdsBufSurfaceParams * sp)
{
  guint kept = unit->det_start;

  for (guint i = unit->det_start; i < self->dets->len; i++)
    if (rds_infer_map_child (&unit->src, sp->width, sp->height, &g_array_index (self->dets, RdsInferDetection, i),
            &g_array_index (self->dets, RdsInferDetection, kept)))
      kept++;
  g_array_set_size (self->dets, kept);
}

/* Classifiers: the classifier parser on each unit of @first .. @first + @n - 1 (a chunk), on the host outputs of its
 * @set, once per object (DeepStream 8.0 behaviour), into self->cls_results (entry u - first). A parser that returns
 * false or throws costs only that object its classifier meta (ours: DeepStream 8.0 crashes): NULL there, and one
 * warning per element. */
void
gst_rds_infer_classify_units (GstRdsInfer * self, RdsInferOutputSet * set, guint first, guint n)
{
  g_ptr_array_set_size (self->cls_results, 0);
  for (guint u = first; u < first + n; u++) {
    InferUnit *unit = &g_array_index (self->units, InferUnit, u);
    RdsInferClassifierResult *result = rds_infer_classifier_result_new ();
    GError *err = NULL;

    gst_rds_infer_point_layers (self, set, unit->image);
    if (!rds_infer_parser_classify (self->parser, self->layers, self->n_outputs, &self->net, result, &err)) {
      gchar *msg = g_strdup_printf ("Parser %s failed on an object of source %u, which gets no classifier meta: %s",
          rds_infer_parser_get_func_name (self->parser), unit->source_id, err->message);

      gst_rds_infer_warn_once (self, WARN_PARSE, msg);
      g_free (msg);
      g_error_free (err);
      g_clear_pointer (&result, rds_infer_classifier_result_free);
    }
    g_ptr_array_add (self->cls_results, result);
  }
}

/* Attaches the classifier @result to @om, with the batch lock held (@bm is @om's batch). @result is only read, so
 * the same result can be attached to other objects or again later. A result without labels attaches nothing and
 * leaves the display text alone; otherwise:
 * - one RdsClassifierMeta (DeepStream 8.0 behaviour: num_labels, unique_component_id @unique_id, classifier_type
 *   @classifier_type, stored as is: the metas don't own it, so it must be interned or static; NULL for none),
 *   appended to the object's classifier metas (ours: 8.0 prepends);
 * - one RdsLabelInfo per label, in the result's order (ours: 8.0 prepends): label_id, result_class_id, result_prob
 *   and result_label (DeepStream 8.0 behaviour); num_classes, the layer's class count (ours: 8.0 sets 0); a label
 *   longer than result_label is truncated there and kept whole in pResult_label (ours: 8.0 sets none);
 * - the display text (DeepStream 8.0 behaviour): "<old> <desc>" with a description (a custom parser's non-empty
 *   descString), else "<old> " followed by "<label> " for each label (also for a custom parser whose descString is
 *   empty: ours, 8.0 not observed); a NULL old text counts as "". obj_label is never changed. */
void
gst_rds_infer_attach_classifier_result (RdsBatchMeta * bm, RdsObjectMeta * om, const RdsInferClassifierResult * result,
    gint unique_id, const gchar * classifier_type)
{
  RdsClassifierMeta *cm;
  GString *text;

  if (result->labels->len == 0)
    return;
  cm = rds_acquire_classifier_meta_from_pool (bm);
  cm->unique_component_id = unique_id;
  cm->classifier_type = classifier_type;
  text = g_string_new (om->text_params.display_text);
  g_string_append_c (text, ' ');
  if (result->desc)
    g_string_append (text, result->desc);
  for (guint i = 0; i < result->labels->len; i++) {
    const RdsInferClassifierLabel *l = &g_array_index (result->labels, RdsInferClassifierLabel, i);
    RdsLabelInfo *li = rds_acquire_label_info_meta_from_pool (bm);

    li->num_classes = l->num_classes;
    li->result_class_id = l->class_id;
    li->label_id = l->label_id;
    li->result_prob = l->prob;
    if (strlen (l->label) >= sizeof (li->result_label))
      li->pResult_label = g_strdup (l->label);
    g_strlcpy (li->result_label, l->label, sizeof (li->result_label));
    rds_add_label_info_meta_to_classifier (cm, li);
    if (!result->desc)
      g_string_append_printf (text, "%s ", l->label);
  }
  rds_add_classifier_meta_to_object (om, cm);
  g_free (om->text_params.display_text);
  om->text_params.display_text = g_string_free (text, FALSE);
}

/* Secondary mode: the results of a chunk go to the unit's object (DeepStream 8.0 behaviour): its tensor meta in the
 * object's obj_user_meta_list (detectors and classifiers included), a detector's children to the frame with the
 * object as parent, a classifier's result as its classifier meta (gst_rds_infer_attach_classifier_result).
 * bInferDone is left alone (DeepStream 8.0 behaviour). A frame or object removed while the lock was released gets
 * nothing, even when the pool reused its address. */
void
gst_rds_infer_attach_object_results (GstRdsInfer * self, RdsBatchMeta * bm, RdsInferOutputSet * set, guint first,
    guint n)
{
  RdsInferConfig *cfg = self->cfg;
  gboolean tensor_meta = cfg->output_tensor_meta || cfg->network_type == 100;
  RdsMetaRef frame = { 0 };
  RdsFrameMeta *fm = NULL;

  rds_acquire_meta_lock (bm);
  for (guint u = first; u < first + n; u++) {
    InferUnit *unit = &g_array_index (self->units, InferUnit, u);
    RdsObjectMeta *om;

    /* consecutive units of a frame resolve it once: the lock is held throughout */
    if (unit->frame.meta != frame.meta || unit->frame.generation != frame.generation) {
      frame = unit->frame;
      fm = rds_meta_ref_resolve (bm->frame_meta_list, &unit->frame);
    }
    om = fm ? rds_meta_ref_resolve (fm->obj_meta_list, &unit->obj) : NULL;
    if (!om) {
      GST_DEBUG_OBJECT (self, "source %u: the %s of a unit is gone, its results are dropped", unit->source_id,
          fm ? "object" : "frame");
      continue;
    }
    for (guint i = 0; i < unit->n_dets; i++)
      gst_rds_infer_add_object (self, bm, fm, &g_array_index (self->dets, RdsInferDetection, unit->det_start + i), om);
    if (cfg->network_type == 1 && g_ptr_array_index (self->cls_results, u - first))
      gst_rds_infer_attach_classifier_result (bm, om, g_ptr_array_index (self->cls_results, u - first),
          (gint) cfg->gie_unique_id, self->classifier_type);
    if (tensor_meta)
      rds_add_user_meta_to_obj (om, gst_rds_infer_new_tensor_user_meta (self, bm, set, unit->image));
  }
  rds_release_meta_lock (bm);
}

