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

/* Test hooks, exported from the plugin for tests/check/element_infer.c only (looked up with g_module_symbol; not
 * API, no header installs them). Call them while no buffer is being processed.
 * - get_config: the configuration the element would start with (the config file plus the properties set), as a
 *   structure named "rdsinfer-config" whose fields are config key names; NULL with @error as start would fail.
 * - get_output_sets: the output sets allocated and the ones in the free list; FALSE when not started.
 * - read_input: copies the first @n_floats of the network input tensor (batch-size images) to @dst; FALSE when
 *   not started or @n_floats is too large. */
__attribute__ ((visibility ("default"))) GstStructure *rds_infer_test_get_config (GstElement * element,
    GError ** error);
__attribute__ ((visibility ("default"))) gboolean rds_infer_test_get_output_sets (GstElement * element,
    guint * n_sets, guint * n_free);
__attribute__ ((visibility ("default"))) gboolean rds_infer_test_read_input (GstElement * element, gfloat * dst,
    gsize n_floats);

G_END_DECLS

#endif /* GST_RDS_INFER_H */
