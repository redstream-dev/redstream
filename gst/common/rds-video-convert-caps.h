/* Redstream: the caps rdsvideoconvert takes on either pad (memory:RDS or system memory), shared with the bins that
 * wrap it (rdsvideosink, rdsh264enc, rdsh265enc) so their ghost pads' templates stay in step. Header only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_VIDEO_CONVERT_CAPS_H
#define RDS_VIDEO_CONVERT_CAPS_H

#include <rds/rds-memory.h>

#define RDS_VIDEO_CONVERT_FORMATS "{ NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR, GRAY8 }"
#define RDS_VIDEO_CONVERT_FIELDS \
  "format = (string) " RDS_VIDEO_CONVERT_FORMATS ", width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], " \
  "framerate = (fraction) [ 0/1, 2147483647/1 ]"
#define RDS_VIDEO_CONVERT_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), " RDS_VIDEO_CONVERT_FIELDS "; video/x-raw, " RDS_VIDEO_CONVERT_FIELDS

#endif /* RDS_VIDEO_CONVERT_CAPS_H */
