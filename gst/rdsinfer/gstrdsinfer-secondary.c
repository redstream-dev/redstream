/* Redstream: rdsinfer's secondary mode (process-mode=2): the objects of a batch read into inference units, a
 * detector's children mapped back to the frame, and a chunk's results attached to its objects. The per-batch flow
 * that calls these is in gstrdsinfer.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsinfer-private.h"
#include "rds-infer-units.h"

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

/* Secondary mode: the results of a chunk go to the unit's object (DeepStream 8.0 behaviour): its tensor meta in the
 * object's obj_user_meta_list (detectors included), a detector's children to the frame with the object as parent.
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
    if (tensor_meta)
      rds_add_user_meta_to_obj (om, gst_rds_infer_new_tensor_user_meta (self, bm, set, unit->image));
  }
  rds_release_meta_lock (bm);
}

