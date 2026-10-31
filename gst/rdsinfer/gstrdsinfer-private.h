/* Redstream: rdsinfer's element internals (plugin-internal, not installed): the element struct and the functions
 * gstrdsinfer.c (configuration, start and stop, the per-batch flow, primary mode) and gstrdsinfer-secondary.c
 * (secondary mode: the object snapshot and the results attached to objects) share. Includes HIP.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_INFER_PRIVATE_H
#define GST_RDS_INFER_PRIVATE_H

#include "gstrdsinfer.h"
#include "rds-infer-config.h"
#include "rds-infer-engine.h"
#include "rds-infer-output.h"
#include "rds-infer-parse.h"
#include "rds-infer-post.h"

#include <rds/rds.h>
#include <rds/rds-meta-private.h>
/* after GLib/GStreamer: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>

G_BEGIN_DECLS

GST_DEBUG_CATEGORY_EXTERN (gst_rds_infer_debug);

/* Warnings posted once per element (then logged at debug level). */
typedef enum
{
  WARN_PARSE = 1 << 0,          /* a parse function returned false or threw */
  WARN_POST_INVALID = 1 << 1,
  WARN_POST_ROTATED = 1 << 2,
  WARN_RAW_OUTPUT = 1 << 3,
} WarnOnce;

/* One inference unit of the batch: a source rect of a frame's slot, inferred as one image of its chunk. */
typedef struct
{
  RdsMetaRef frame;             /* resolved under the batch lock before use */
  RdsMetaRef obj;               /* the object it crops; unused (zeroed) in primary mode */
  guint source_id, slot;
  RdsBufSurfTransformRect src;  /* in slot pixels; primary mode: the whole slot, secondary mode: the object's crop */
  guint image;                  /* in the chunk's network input and output set; primary mode: the slot */
  RdsInferLetterbox lb;         /* for src's size (secondary mode: rds_infer_crop_letterbox) */
  guint det_start, n_dets;      /* in self->dets; secondary mode: in frame pixels */
} InferUnit;

struct _GstRdsInfer
{
  GstBaseTransform parent;

  /* properties (object lock); set_mask has PROP_BIT of every property set explicitly */
  gchar *config_file_path, *infer_on_class_ids, *filter_out_class_ids, *model_engine_file;
  guint unique_id, process_mode, batch_size, gpu_id;
  gint infer_on_gie_id, interval;
  gboolean output_tensor_meta, raw_output_file_write, output_instance_mask, input_tensor_meta,
      crop_objects_to_roi_boundary, clip_object_outside_roi;
  gpointer raw_output_generated_callback, raw_output_generated_userdata;
  guint32 set_mask;

  /* from start to stop (streaming thread; test hooks read it while idle) */
  RdsInferConfig *cfg;
  RdsInferEngine *engine;
  RdsInferParser *parser;
  RdsInferOutputPool *outputs;
  hipStream_t stream;
  guint gpu;
  gfloat *input;                /* batch_size network images on the GPU */
  gsize image_floats;
  guint channels;
  RdsInferNetworkInfo net;
  guint n_outputs;
  RdsInferDims *out_dims;
  const gchar **out_names;      /* the engine's */
  guint batch_count;            /* batches seen since start or the last flush */
  guint64 unit_count;           /* units inferred since start */
  guint warned;                 /* WarnOnce */
  gboolean secondary;           /* process-mode=2 */
  GArray *units;                /* InferUnit, the current batch's */
  GArray *objects;              /* RdsInferObjectDetectionInfo, one frame's parser output */
  GArray *dets;                 /* RdsInferDetection, every frame's */
  GByteArray *scratch;
  RdsInferLayerInfo *layers;    /* one frame's output layers, for the parser */
  gboolean *slot_used;          /* batch_size entries: the current batch has a frame in that slot */
  gboolean *image_used;         /* batch_size entries: the current chunk has a unit in that image */
};
/* gstrdsinfer.c */

/* Posts @msg as an element warning the first time @what happens, then logs it at debug level. */
G_GNUC_INTERNAL void gst_rds_infer_warn_once (GstRdsInfer * self, WarnOnce what, const gchar * msg);
/* An object meta for detection @d, added to @fm with @parent (NULL in primary mode). Under the batch lock. */
G_GNUC_INTERNAL void gst_rds_infer_add_object (GstRdsInfer * self, RdsBatchMeta * bm, RdsFrameMeta * fm,
    const RdsInferDetection * d, RdsObjectMeta * parent);
/* A tensor meta for image @image of @set, in a user meta from @bm's pool. Under the batch lock. */
G_GNUC_INTERNAL RdsUserMeta *gst_rds_infer_new_tensor_user_meta (GstRdsInfer * self, RdsBatchMeta * bm,
    RdsInferOutputSet * set, guint image);

/* gstrdsinfer-secondary.c */

G_GNUC_INTERNAL gboolean gst_rds_infer_snapshot_objects (GstRdsInfer * self, RdsBatchMeta * bm,
    const RdsBufSurface * surf);
G_GNUC_INTERNAL void gst_rds_infer_map_children (GstRdsInfer * self, const InferUnit * unit,
    const RdsBufSurfaceParams * sp);
G_GNUC_INTERNAL void gst_rds_infer_attach_object_results (GstRdsInfer * self, RdsBatchMeta * bm,
    RdsInferOutputSet * set, guint first, guint n);

G_END_DECLS

#endif /* GST_RDS_INFER_PRIVATE_H */
