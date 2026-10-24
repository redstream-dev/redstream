/* Redstream: rdsstreamdemux, splits rdsstreammux batches back into one stream per source, each frame a zero-copy
 * view of its batch slot (the counterpart of DeepStream's nvstreamdemux, with its property names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_STREAM_DEMUX_H
#define GST_RDS_STREAM_DEMUX_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_STREAM_DEMUX (gst_rds_stream_demux_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsStreamDemux, gst_rds_stream_demux, GST, RDS_STREAM_DEMUX, GstElement)

GST_ELEMENT_REGISTER_DECLARE (rdsstreamdemux);

G_END_DECLS

#endif /* GST_RDS_STREAM_DEMUX_H */
