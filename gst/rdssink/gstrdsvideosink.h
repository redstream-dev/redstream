/* Redstream: rdsvideosink, a display sink for memory:RDS frames (stands in for DeepStream's nveglglessink and
 * nv3dsink, with their property names): a bin of rdsvideoconvert, videoconvert and a stock video sink.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_VIDEO_SINK_H
#define GST_RDS_VIDEO_SINK_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_VIDEO_SINK (gst_rds_video_sink_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsVideoSink, gst_rds_video_sink, GST, RDS_VIDEO_SINK, GstBin)

GST_ELEMENT_REGISTER_DECLARE (rdsvideosink);

G_END_DECLS

#endif /* GST_RDS_VIDEO_SINK_H */
