/* Redstream: rdstracker, assigns persistent IDs to the objects in memory:RDS batches (the counterpart of DeepStream's
 * nvtracker, with its property names and config files).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_TRACKER_H
#define GST_RDS_TRACKER_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

/* compute-hw: the hardware for the tracker's frame processing. */
typedef enum
{
  GST_RDS_TRACKER_COMPUTE_HW_DEFAULT = 0,
  GST_RDS_TRACKER_COMPUTE_HW_GPU = 1,
  GST_RDS_TRACKER_COMPUTE_HW_VIC = 2,
} GstRdsTrackerComputeHw;

#define GST_TYPE_RDS_TRACKER_COMPUTE_HW (gst_rds_tracker_compute_hw_get_type ())
GType gst_rds_tracker_compute_hw_get_type (void);

#define GST_TYPE_RDS_TRACKER (gst_rds_tracker_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsTracker, gst_rds_tracker, GST, RDS_TRACKER, GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE (rdstracker);

G_END_DECLS

#endif /* GST_RDS_TRACKER_H */
