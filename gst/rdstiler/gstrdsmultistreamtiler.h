/* Redstream: rdsmultistreamtiler, composites the frames of a memory:RDS batch into one 2D tiled frame (the
 * counterpart of DeepStream's nvmultistreamtiler, with its property names).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_MULTI_STREAM_TILER_H
#define GST_RDS_MULTI_STREAM_TILER_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define GST_TYPE_RDS_MULTI_STREAM_TILER (gst_rds_multi_stream_tiler_get_type ())
G_DECLARE_FINAL_TYPE (GstRdsMultiStreamTiler, gst_rds_multi_stream_tiler, GST, RDS_MULTI_STREAM_TILER,
    GstBaseTransform)

GST_ELEMENT_REGISTER_DECLARE (rdsmultistreamtiler);

G_END_DECLS

#endif /* GST_RDS_MULTI_STREAM_TILER_H */
