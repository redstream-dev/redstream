/* Redstream: rdsinfer, runs a MIGraphX model on memory:RDS batches and attaches the results as metadata (the
 * counterpart of DeepStream's nvinfer, with its property names and config files).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_INFER_H
#define GST_RDS_INFER_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_INFER (gst_rds_infer_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsInfer, gst_rds_infer, GST, RDS_INFER, GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE (rdsinfer);

G_END_DECLS

#endif /* GST_RDS_INFER_H */
