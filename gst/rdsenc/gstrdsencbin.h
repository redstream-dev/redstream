/* Redstream: GstRdsEncBin, the base class of rdsh264enc and rdsh265enc (DeepStream's nvv4l2h264enc and
 * nvv4l2h265enc, with their property names): a bin of rdsvideoconvert, an NV12 capsfilter, the stock VA encoder and
 * a capsfilter, parameterised by a codec descriptor.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_ENC_BIN_H
#define GST_RDS_ENC_BIN_H

#include <gst/gst.h>

#include "rds-enc-nal.h"

G_BEGIN_DECLS

/* What tells the two codecs apart. */
typedef struct
{
  RdsEncCodec codec;
  const gchar *element;         /* "rdsh264enc" */
  const gchar *codec_name;      /* "H.264" */
  const gchar *va_factory;      /* "vah264enc" */
  const gchar *caps_name;       /* "video/x-h264" */
  GType (*profile_type) (void); /* the `profile` enum */
  /* the VA caps profile for a `profile` value; FALSE with an error naming it when VA can't produce it */
  gboolean (*va_profile) (gint value, const gchar ** va_profile, GError ** error);
  gboolean has_cabac;           /* `disable-cabac` exists (H.264) */
} GstRdsEncCodecInfo;

#define GST_TYPE_RDS_ENC_BIN (gst_rds_enc_bin_get_type ())
G_DECLARE_DERIVABLE_TYPE (GstRdsEncBin, gst_rds_enc_bin, GST, RDS_ENC_BIN, GstBin)

struct _GstRdsEncBinClass
{
  GstBinClass parent_class;

  const GstRdsEncCodecInfo *info;
  GParamSpec **specs;           /* indexed by property id; NULL where the codec has no such property */
};

/* Called from a codec's class_init: installs the codec's properties, pad templates and metadata. */
G_GNUC_INTERNAL void gst_rds_enc_bin_class_setup (GstRdsEncBinClass * klass, const GstRdsEncCodecInfo * info);

/* The enum types both codecs share. */
G_GNUC_INTERNAL GType gst_rds_enc_rate_control_get_type (void);
G_GNUC_INTERNAL GType gst_rds_enc_io_mode_get_type (void);
G_GNUC_INTERNAL GType gst_rds_enc_cuda_mem_type_get_type (void);
G_GNUC_INTERNAL GType gst_rds_enc_tuning_info_get_type (void);

G_END_DECLS

#endif /* GST_RDS_ENC_BIN_H */
