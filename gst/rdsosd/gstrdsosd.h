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

G_END_DECLS

#endif /* GST_RDS_OSD_H */
