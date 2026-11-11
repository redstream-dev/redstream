/* Redstream: rdsanalytics, ROI filtering, overcrowding, direction and line crossing on the objects of memory:RDS
 * batches (the counterpart of DeepStream's nvdsanalytics, with its property names and config files).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_ANALYTICS_H
#define GST_RDS_ANALYTICS_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_ANALYTICS (gst_rds_analytics_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsAnalytics, gst_rds_analytics, GST, RDS_ANALYTICS, GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE (rdsanalytics);

/* Test hook, exported from the plugin for tests/check/pipeline_analytics.c only (looked up with g_module_symbol;
 * not API, no header installs it): the per-object histories the element's core holds over every stream (0 before
 * the first batch). Takes the element's core lock, so call it between buffers, e.g. from a probe on its source
 * pad. */
__attribute__ ((visibility ("default"))) guint rds_analytics_test_get_history_count (GstElement * element);

G_END_DECLS

#endif /* GST_RDS_ANALYTICS_H */
