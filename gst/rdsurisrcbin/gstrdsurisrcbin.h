/* Redstream: rdsurisrcbin, a source bin for file and RTSP URIs with memory:RDS output (stands in for DeepStream's
 * nvurisrcbin, with its property names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_URI_SRC_BIN_H
#define GST_RDS_URI_SRC_BIN_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_URI_SRC_BIN (gst_rds_uri_src_bin_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsUriSrcBin, gst_rds_uri_src_bin, GST, RDS_URI_SRC_BIN, GstBin)

GST_ELEMENT_REGISTER_DECLARE (rdsurisrcbin);

G_END_DECLS

#endif /* GST_RDS_URI_SRC_BIN_H */
