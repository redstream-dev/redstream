/* Redstream: rdsh264enc and rdsh265enc (DeepStream's nvv4l2h264enc and nvv4l2h265enc), the two GstRdsEncBin codecs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_H26X_ENC_H
#define GST_RDS_H26X_ENC_H

#include "gstrdsencbin.h"

G_BEGIN_DECLS

#define GST_TYPE_RDS_H264_ENC (gst_rds_h264_enc_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsH264Enc, gst_rds_h264_enc, GST, RDS_H264_ENC, GstRdsEncBin)
#define GST_TYPE_RDS_H265_ENC (gst_rds_h265_enc_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsH265Enc, gst_rds_h265_enc, GST, RDS_H265_ENC, GstRdsEncBin)

GST_ELEMENT_REGISTER_DECLARE (rdsh264enc);
GST_ELEMENT_REGISTER_DECLARE (rdsh265enc);

G_END_DECLS

#endif /* GST_RDS_H26X_ENC_H */
