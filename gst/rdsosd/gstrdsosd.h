/* Redstream: rdsosd, draws the boxes, text, lines, arrows and circles of the object and display meta onto
 * memory:RDS RGBA frames (the counterpart of DeepStream's nvdsosd, with its property names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_OSD_H
#define GST_RDS_OSD_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_OSD (gst_rds_osd_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsOsd, gst_rds_osd, GST, RDS_OSD, GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE (rdsosd);

/* Test hooks, exported from the plugin for tests/check/element_osd.c only (looked up with g_module_symbol; not API,
 * no header installs them). Call them while the stream is idle (between buffers).
 * - get_counts: since start, the frames synced to the CPU to be drawn there (CPU staging) and the passes drawn on
 *   the GPU.
 * - fail_next_pass: the next GPU pass fails as on a HIP error (it is given no plane to draw on). */
__attribute__ ((visibility ("default"))) void rds_osd_test_get_counts (GstElement * element, guint64 * cpu_frames,
    guint64 * gpu_passes);
__attribute__ ((visibility ("default"))) void rds_osd_test_fail_next_pass (GstElement * element);

G_END_DECLS

#endif /* GST_RDS_OSD_H */
