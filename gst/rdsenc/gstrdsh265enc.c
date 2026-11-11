/* Redstream: rdsh265enc, standing in for DeepStream's nvv4l2h265enc: GstRdsEncBin over vah265enc.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsh26xenc.h"
#include "gstrdsencbin-private.h"

#include "rds-enc-props.h"

struct _GstRdsH265Enc
{
  GstRdsEncBin parent;
};

/* DeepStream 8.0's values and nicks; the type name and descriptions are ours */
static GType gst_rds_h265_enc_profile_get_type (void);
RDS_ENC_DEFINE_ENUM (gst_rds_h265_enc_profile_get_type, "GstRdsH265EncProfile",
    {0, "Main", "Main"},
    {1, "Main 10 (not supported)", "Main10"},
    {3, "Format range extensions (not supported)", "FREXT"});

static const GstRdsEncCodecInfo h265_info = {
  .codec = RDS_ENC_CODEC_H265,
  .element = "rdsh265enc",
  .codec_name = "H.265",
  .va_factory = "vah265enc",
  .caps_name = "video/x-h265",
  .profile_type = gst_rds_h265_enc_profile_get_type,
  .va_profile = rds_enc_h265_profile,
  .has_cabac = FALSE,
};

G_DEFINE_FINAL_TYPE (GstRdsH265Enc, gst_rds_h265_enc, GST_TYPE_RDS_ENC_BIN);
GST_ELEMENT_REGISTER_DEFINE (rdsh265enc, "rdsh265enc", GST_RANK_NONE, GST_TYPE_RDS_H265_ENC);

static void
gst_rds_h265_enc_init (G_GNUC_UNUSED GstRdsH265Enc * self)
{
}

static void
gst_rds_h265_enc_class_init (GstRdsH265EncClass * klass)
{
  gst_rds_enc_bin_class_setup (GST_RDS_ENC_BIN_CLASS (klass), &h265_info);
}
