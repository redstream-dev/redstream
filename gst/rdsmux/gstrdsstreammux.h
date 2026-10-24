/* Redstream: rdsstreammux, batches frames from several sources into one memory:RDS buffer with an RdsBatchMeta,
 * scaling each frame into a fixed-size slot (the counterpart of DeepStream's default nvstreammux, with its property
 * names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_STREAM_MUX_H
#define GST_RDS_STREAM_MUX_H

#include <gst/base/gstaggregator.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_STREAM_MUX (gst_rds_stream_mux_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsStreamMux, gst_rds_stream_mux, GST, RDS_STREAM_MUX, GstAggregator)

#define GST_TYPE_RDS_STREAM_MUX_PAD (gst_rds_stream_mux_pad_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsStreamMuxPad, gst_rds_stream_mux_pad, GST, RDS_STREAM_MUX_PAD, GstAggregatorPad)

GST_ELEMENT_REGISTER_DECLARE (rdsstreammux);

G_END_DECLS

#endif /* GST_RDS_STREAM_MUX_H */
