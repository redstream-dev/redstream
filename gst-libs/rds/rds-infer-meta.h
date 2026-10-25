/* Redstream: rdsinfer's tensor output metadata (counterpart of DeepStream's NvDsInferTensorMeta, gstnvdsinfer.h).
 * C and C++. Field names, types and order follow NVIDIA's public API reference.
 *
 * With output-tensor-meta=1 (or network-type=100), rdsinfer attaches one RdsUserMeta per inferred frame to the
 * frame's frame_user_meta_list, with base_meta.meta_type RDS_INFER_TENSOR_OUTPUT_META and user_meta_data an
 * RdsInferTensorMeta:
 *
 *   for (RdsMetaList *l = frame_meta->frame_user_meta_list; l != NULL; l = l->next) {
 *     RdsUserMeta *um = l->data;
 *     if (um->base_meta.meta_type == RDS_INFER_TENSOR_OUTPUT_META) {
 *       RdsInferTensorMeta *tm = um->user_meta_data;
 *       const float *out0 = tm->out_buf_ptrs_host[0];   (this frame's slice of output layer 0)
 *     }
 *   }
 *
 * The buffers are rdsinfer's: valid (and unchanged) as long as the meta exists, also in copies of it (an
 * rdsstreamdemux output, rds_batch_meta_copy) and after the element has stopped. Never free or write them.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_META_H
#define RDS_INFER_META_H

#include <glib.h>
#include <rds/rds-infer.h>
#include <rds/rds-meta.h>

G_BEGIN_DECLS

typedef struct
{
  guint unique_id;                      /* gie-unique-id (unique-id) of the rdsinfer that attached it */
  guint num_output_layers;
  /* num_output_layers entries: data type, per-image dims, binding index (the output's index) and name. Their
   * buffer and deviceBuffer pointers are NULL (documented: not valid here); use the arrays below. */
  RdsInferLayerInfo *output_layers_info;
  void **out_buf_ptrs_host;             /* per layer: this frame's slice, host memory (pinned) */
  void **out_buf_ptrs_dev;              /* per layer: the same slice on the GPU (gpu_id) */
  gint gpu_id;
  void *priv_data;                      /* rdsinfer's: keeps the buffers alive */
  RdsInferNetworkInfo network_info;     /* the network input size */
  gboolean maintain_aspect_ratio;
  gboolean symmetric_padding;           /* meaningful with maintain_aspect_ratio only */
} RdsInferTensorMeta;

G_END_DECLS

#endif /* RDS_INFER_META_H */
