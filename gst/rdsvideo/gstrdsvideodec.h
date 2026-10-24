/* Redstream: rdsvideodec, H.264/H.265 decode with rocDecode into memory:RDS NV12 surfaces (the counterpart of
 * DeepStream's nvv4l2decoder, with its property names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_VIDEO_DEC_H
#define GST_RDS_VIDEO_DEC_H

#include <gst/video/gstvideodecoder.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_VIDEO_DEC (gst_rds_video_dec_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsVideoDec, gst_rds_video_dec, GST, RDS_VIDEO_DEC, GstVideoDecoder)

GST_ELEMENT_REGISTER_DECLARE (rdsvideodec);

G_END_DECLS

#endif /* GST_RDS_VIDEO_DEC_H */
