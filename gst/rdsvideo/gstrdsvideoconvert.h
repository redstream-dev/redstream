/* Redstream: rdsvideoconvert, colour conversion and scaling between memory:RDS and system-memory video (the
 * counterpart of DeepStream's nvvideoconvert, with its property names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_VIDEO_CONVERT_H
#define GST_RDS_VIDEO_CONVERT_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_VIDEO_CONVERT (gst_rds_video_convert_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsVideoConvert, gst_rds_video_convert, GST, RDS_VIDEO_CONVERT, GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE (rdsvideoconvert);

G_END_DECLS

#endif /* GST_RDS_VIDEO_CONVERT_H */
