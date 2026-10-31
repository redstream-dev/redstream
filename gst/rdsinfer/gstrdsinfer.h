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
 * API, no header installs them). Call them while the stream is idle (between buffers, e.g. after pulling one
 * downstream); get_output_sets only reads counts, under the pool's lock.
 * - get_config: the configuration the element would start with (the config file plus the properties set), as a
 *   structure named "rdsinfer-config" whose fields are config key names; NULL with @error as start would fail.
 * - get_output_sets: the output sets allocated and the ones in the free list; FALSE when not started.
 * - get_output_set_totals: the output sets allocated and freed since start; FALSE when not started.
 * - read_input: copies the first @n_floats of the network input tensor (batch-size images) to @dst; FALSE when
 *   not started or @n_floats is too large.
 * - get_unit_count: the inference units inferred since start (primary mode: one per frame, secondary mode: one per
 *   object inferred on).
 * - get_cache_size: the entries in the per-object cache (secondary-mode classifiers; 0 without a cache). Takes only
 *   the cache's own lock, so it may also be called from a pad probe while the stream runs.
 * - attach_classifier_result: gst_rds_infer_attach_classifier_result (gstrdsinfer-private.h), for a test that builds
 *   the batch meta and the RdsInferClassifierResult (rds-infer-parse.h) itself; no element needed. */
__attribute__ ((visibility ("default"))) GstStructure *rds_infer_test_get_config (GstElement * element,
    GError ** error);
__attribute__ ((visibility ("default"))) gboolean rds_infer_test_get_output_sets (GstElement * element,
    guint * n_sets, guint * n_free);
__attribute__ ((visibility ("default"))) gboolean rds_infer_test_read_input (GstElement * element, gfloat * dst,
    gsize n_floats);
__attribute__ ((visibility ("default"))) guint64 rds_infer_test_get_unit_count (GstElement * element);
__attribute__ ((visibility ("default"))) guint rds_infer_test_get_cache_size (GstElement * element);
__attribute__ ((visibility ("default"))) gboolean rds_infer_test_get_output_set_totals (GstElement * element,
    guint64 * allocated, guint64 * freed);
__attribute__ ((visibility ("default"))) void rds_infer_test_attach_classifier_result (gpointer batch_meta,
    gpointer obj_meta, gconstpointer result, gint unique_id, const gchar * classifier_type);

G_END_DECLS

#endif /* GST_RDS_INFER_H */
