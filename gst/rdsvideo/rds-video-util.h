/* Redstream: helpers shared by the rdsvideo elements (plugin-internal, not installed).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_VIDEO_UTIL_H
#define RDS_VIDEO_UTIL_H

#include <gst/video/video.h>
#include <rds/rds.h>

G_BEGIN_DECLS

/* Describes a mapped system-memory frame as a hand-built single-surface RdsBufSurface (memType SYSTEM, _priv NULL,
 * numFilled 1) whose planes point into the frame: dataPtr is plane 0's data, other planes are offsets from it.
 * @params receives the surface parameters and must outlive @surf. FALSE if the format has no RdsBufSurface
 * equivalent, a stride is not positive, or a plane lies before plane 0 or more than 4 GiB after it (the caller
 * then copies plane by plane; GStreamer's own video pools lay planes out in ascending order). */
G_GNUC_INTERNAL gboolean rds_video_frame_wrap_surface (GstVideoFrame * frame, RdsBufSurface * surf,
    RdsBufSurfaceParams * params);

/* Parses "left:top:width:height" (exactly four non-negative decimal integers, each at most G_MAXUINT32).
 * NULL or "" means the whole frame: TRUE with every field 0. On failure @rect is left untouched. */
G_GNUC_INTERNAL gboolean rds_video_parse_rect (const gchar * str, RdsBufSurfTransformRect * rect);

G_END_DECLS

#endif /* RDS_VIDEO_UTIL_H */
