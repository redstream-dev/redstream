/* Redstream: rdsh264enc, standing in for DeepStream's nvv4l2h264enc: GstRdsEncBin over vah264enc.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsh26xenc.h"
#include "gstrdsencbin-private.h"

#include "rds-enc-props.h"

struct _GstRdsH264Enc
{
  GstRdsEncBin parent;
};

/* DeepStream 8.0's values and nicks; the type name and descriptions are ours */
static GType gst_rds_h264_enc_profile_get_type (void);
RDS_ENC_DEFINE_ENUM (gst_rds_h264_enc_profile_get_type, "GstRdsH264EncProfile",
    {0, "Baseline (encoded as Constrained Baseline)", "Baseline"},
    {1, "Constrained Baseline", "Constrained-Baseline"},
    {2, "Main", "Main"},
    {4, "High", "High"},
    {17, "Constrained High (encoded as High)", "Constrained-High"},
    {7, "High 4:4:4 Predictive (not supported)", "High444"});

static const GstRdsEncCodecInfo h264_info = {
  .codec = RDS_ENC_CODEC_H264,
  .element = "rdsh264enc",
  .codec_name = "H.264",
  .va_factory = "vah264enc",
  .caps_name = "video/x-h264",
  .profile_type = gst_rds_h264_enc_profile_get_type,
  .va_profile = rds_enc_h264_profile,
  .has_cabac = TRUE,
};

G_DEFINE_FINAL_TYPE (GstRdsH264Enc, gst_rds_h264_enc, GST_TYPE_RDS_ENC_BIN);
GST_ELEMENT_REGISTER_DEFINE (rdsh264enc, "rdsh264enc", GST_RANK_NONE, GST_TYPE_RDS_H264_ENC);

static void
gst_rds_h264_enc_init (G_GNUC_UNUSED GstRdsH264Enc * self)
{
}

static void
gst_rds_h264_enc_class_init (GstRdsH264EncClass * klass)
{
  gst_rds_enc_bin_class_setup (GST_RDS_ENC_BIN_CLASS (klass), &h264_info);
}
