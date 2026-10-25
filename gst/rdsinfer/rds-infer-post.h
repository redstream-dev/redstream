/* Redstream: rdsinfer's detection post-processing (plugin-internal, not installed): the letterbox geometry shared
 * with the preprocessing, and the steps from parser output (network-input pixels) to the objects attached to a
 * frame (frame pixels). C.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_POST_H
#define RDS_INFER_POST_H

#include <glib.h>
#include <rds/rds-infer.h>

#include "rds-infer-config.h"

G_BEGIN_DECLS

/* Where a frame goes in the network input. The frame (frame_w x frame_h) is scaled to scaled_w x scaled_h and
 * placed at (pad_left, pad_top); the rest of the input is padding.
 * - maintain-aspect-ratio: scale = min (net_w / frame_w, net_h / frame_h); scaled_w = round (frame_w * scale) and
 *   scaled_h = round (frame_h * scale), rounded to nearest with halves up, computed exactly in integers, and at
 *   least 1. The scaled frame is at the top-left, or centred with symmetric-padding at
 *   (floor ((net_w - scaled_w) / 2), floor ((net_h - scaled_h) / 2)).
 * - Otherwise each axis is scaled on its own: scaled_w = net_w, scaled_h = net_h, no padding.
 * scale_x = scaled_w / frame_w and scale_y = scaled_h / frame_h are the effective per-axis scales of the rounded
 * sizes, so the scaled frame's edges map exactly onto the frame's. They are rounded floats, for information: the
 * preprocessing derives its sampling step from the integers (frame_w / scaled_w, frame_h / scaled_h, offset by
 * pad_left and pad_top), as rds_infer_letterbox_map does in reverse, so both agree exactly.
 * All sizes are 1..G_MAXINT; a bad size is a critical and leaves @out zeroed. */
typedef struct
{
  guint net_w, net_h, frame_w, frame_h;
  guint pad_left, pad_top, scaled_w, scaled_h;
  gfloat scale_x, scale_y;
} RdsInferLetterbox;

G_GNUC_INTERNAL void rds_infer_letterbox (guint net_w, guint net_h, guint frame_w, guint frame_h,
    gboolean keep_aspect, gboolean symmetric, RdsInferLetterbox * out);

/* Network-input pixels to frame pixels: x_frame = (x_net - pad_left) * frame_w / scaled_w (y likewise), and
 * sizes w_frame = w_net * frame_w / scaled_w, computed in double and stored as float. */
G_GNUC_INTERNAL void rds_infer_letterbox_map (const RdsInferLetterbox * lb, gfloat left, gfloat top, gfloat width,
    gfloat height, gfloat out[4]);

typedef struct
{
  gfloat left, top, width, height;
} RdsInferBox;

/* One object after post-processing, in frame pixels. */
typedef struct
{
  guint class_id;
  gfloat confidence;
  RdsInferBox box;              /* the mapped, unclipped detector box (detector_bbox_info.org_bbox_coords) */
  RdsInferBox rect;             /* box clipped to the frame (rect_params); never empty */
} RdsInferDetection;

/* What rds_infer_postprocess saw, for the caller to report (once per element, say). */
typedef enum
{
  RDS_INFER_POST_INVALID = 1 << 0,      /* objects dropped: class id >= num-detected-classes, a coordinate or the
                                         * confidence not finite, or width or height <= 0 */
  RDS_INFER_POST_ROTATED = 1 << 1,      /* objects with a non-zero rotation_angle (ignored, the objects kept) */
} RdsInferPostFlags;

/* The post-processing of one frame's parser output (@n_in objects in network-input pixels), in this order:
 * 1. validation (see RDS_INFER_POST_INVALID) and the class's pre-cluster-threshold (kept: confidence >= it);
 * 2. cluster-mode=2: per-class greedy NMS with the class's nms-iou-threshold, highest confidence first, ties by
 *    lower input index. An object is suppressed by a kept one when their IoU is strictly greater than the
 *    threshold (IoU equal to the threshold is kept). IoU = intersection / union of the boxes (right = left + width,
 *    bottom = top + height, area = width * height, union = area1 + area2 - intersection), 0 for a zero union.
 *    O(kept x candidates) per class: about 30 ms (release build) for 8400 boxes of one class that don't overlap,
 *    which takes near-zero thresholds;
 * 3. cluster-mode=2: the class's topk best (in the same order) after NMS; -1 keeps all, 0 none;
 * 4. post-cluster-threshold (kept: confidence >= it);
 * 5. mapping to frame pixels (rds_infer_letterbox_map); a box that is no longer finite (float overflow) is
 *    dropped as invalid;
 * 6. filter-out-class-ids;
 * 7. when roi-top-offset or roi-bottom-offset is set: the box's centre y must be in
 *    [roi-top-offset, frame_h - roi-bottom-offset) (frame pixels from the top and the bottom edge);
 * 8. detected-min-w/h and detected-max-w/h (0: no maximum) on the mapped, unclipped size;
 * 9. the box clipped to [0, frame_w] x [0, frame_h] gives rect; an empty rect drops the object.
 * cluster-mode=4 skips 2 and 3. Steps 1 and 3-8 use the class's attributes. The survivors are appended to @out
 * (a GArray of RdsInferDetection) in input order.
 * @scratch, if not NULL, is working memory the caller keeps between calls (to avoid an allocation per frame); its
 * contents are overwritten. */
G_GNUC_INTERNAL RdsInferPostFlags rds_infer_postprocess (const RdsInferConfig * cfg, const RdsInferLetterbox * lb,
    const RdsInferObjectDetectionInfo * in, guint n_in, GByteArray * scratch, GArray * out);

G_END_DECLS

#endif /* RDS_INFER_POST_H */
