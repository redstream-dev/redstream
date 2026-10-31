/* Redstream: rdsinfer's secondary-mode geometry (plugin-internal, not installed): which objects are inferred on,
 * the crop of a selected object, where the crop goes in the network input, and the mapping of the objects found in
 * a crop back to the frame. C, no GPU.
 * Each rule is marked "DeepStream 8.0 behaviour" (measured) or "ours" (our choice where 8.0 is undocumented or
 * fails).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_UNITS_H
#define RDS_INFER_UNITS_H

#include <glib.h>
#include <rds/rds-transform.h>

#include "rds-infer-config.h"
#include "rds-infer-post.h"

G_BEGIN_DECLS

/* Whether an object (its unique_component_id, class_id and rect_params size) is inferred on in secondary mode, by
 * the final config (after the element's infer-on-gie-id and infer-on-class-ids overrides). All three must hold:
 * 1. operate_on_gie_id is -1 (every producer, this element's own id and 0 included) or equals
 *    @unique_component_id (0 is a real id). DeepStream 8.0 behaviour.
 * 2. operate_on_class_ids is empty (every class; a -1 in the file or the property empties it) or contains
 *    @class_id. DeepStream 8.0 behaviour. With a list set, a negative @class_id is never selected (ours: 8.0
 *    aborts).
 * 3. input-object-min-width <= @w <= input-object-max-width and the same for the height, inclusive, on the float
 *    size; a limit of 0 is no limit. DeepStream 8.0 behaviour. A @w or @h that is not finite or not > 0 is not selected
 *    (ours). */
G_GNUC_INTERNAL gboolean rds_infer_object_selected (const RdsInferConfig * cfg, gint unique_component_id,
    gint class_id, gfloat w, gfloat h);

/* The crop of an object's rect_params (@left, @top, @w, @h, frame pixels) in a frame_w x frame_h slot, as the
 * source rect of its unit: even origin and even size, at least 2x2, inside the frame. Per axis:
 * 1. A side < 1 (or a value that is not finite) is skipped: FALSE. DeepStream 8.0 behaviour (not finite: ours).
 * 2. Alignment: origin = floor (left), rounded up to even (11 -> 12, 43.7 -> 44); size = floor (w), rounded down
 *    to even, at least 2 (15.6 -> 14, 17 -> 16, 1 -> 2). DeepStream 8.0 behaviour.
 * 3. Clipping, after the alignment: the aligned span [origin, origin + size) is cut to [0, frame). Overflow on
 *    the right or bottom is clipped (DeepStream 8.0 behaviour); a negative origin, a span past the frame on both
 *    sides and a span fully outside it are clipped the same way (ours: 8.0 fails).
 * 4. The clipped size is rounded down to even again (an odd frame size, say); the origin stays even, since it was
 *    even or became 0. Less than 2 left: FALSE (ours).
 * Alignment comes first so that every box that fits in the frame gets exactly 8.0's crop; clipping second never
 * moves an edge outward, so the crop always lies inside both the aligned span and the frame.
 * Examples, frame 100 wide: left -3, w 10 -> aligned [-2, 8) -> [0, 8); left 95, w 10 -> [96, 106) -> [96, 100);
 * frame 99 wide: left 95, w 10 -> [96, 99) -> [96, 98); left 99, w 3 -> [100, 102): nothing left, FALSE.
 * On FALSE, @out is left untouched. */
G_GNUC_INTERNAL gboolean rds_infer_crop_rect (gfloat left, gfloat top, gfloat w, gfloat h, guint frame_w,
    guint frame_h, RdsBufSurfTransformRect * out);

/* Where a crop (crop_w x crop_h, from rds_infer_crop_rect) goes in the network input, in the RdsInferLetterbox of
 * primary mode with frame_w x frame_h = the crop size, so rds_infer_letterbox_map, rds_infer_postprocess and the
 * preprocessing take it as they are. It differs from rds_infer_letterbox (primary mode, unchanged) in the rounding:
 * - maintain-aspect-ratio: scale = min (net_w / crop_w, net_h / crop_h); the exact content size is crop * scale
 *   per axis, and scaled_w, scaled_h are those floored to even (an odd network size on the axis the crop fills
 *   too). DeepStream 8.0 behaviour. At least 2 and at most the network size (1 for a 1-pixel network; ours).
 *   The content is at the top-left, or with symmetric-padding at floor ((net - exact) / 2) per axis, from the exact
 *   size, not the even one (DeepStream 8.0 behaviour); the content always fits. Computed exactly in integers.
 * - Otherwise the crop is stretched to the network: scaled_w = net_w, scaled_h = net_h, no padding. DeepStream 8.0
 *   behaviour.
 * scale_x = scaled_w / crop_w and scale_y = scaled_h / crop_h, as floats for information.
 * All sizes are 1..G_MAXINT; a bad size is a critical and leaves @out zeroed. */
G_GNUC_INTERNAL void rds_infer_crop_letterbox (guint net_w, guint net_h, guint crop_w, guint crop_h,
    gboolean keep_aspect, gboolean symmetric, RdsInferLetterbox * out);

/* An object found in a crop, to frame pixels (ours: one geometry throughout, network -> crop -> frame; 8.0 mixes the
 * unaligned parent origin with the aligned crop's scale and is off by up to a pixel).
 * @in is an object from rds_infer_postprocess run with the crop's letterbox (rds_infer_crop_letterbox): its box is
 * in crop pixels, with the origin at the crop's top-left (network pixels mapped with rds_infer_letterbox_map from
 * the aligned crop), and its rect is that box clipped to the crop. @crop is the aligned crop (rds_infer_crop_rect)
 * in a frame_w x frame_h frame.
 * @out gets @in's class and confidence, box = @in's box moved by the crop's origin (frame pixels, unclipped: it may
 * reach outside the crop and the frame), and rect = that box clipped to [0, frame_w] x [0, frame_h] (so an object
 * partly outside the crop keeps its part outside the crop, inside the frame; rds_infer_box_clip). @in's rect is not
 * used. FALSE, @out untouched, when the clipped rect is empty. @out may be @in.
 * Running rds_infer_postprocess in crop pixels has two effects for the caller:
 * - a child wholly outside the crop is dropped there (its step 9, the clip to the crop) and never gets here; a child
 *   partly outside the crop gets here and keeps its part inside the frame;
 * - roi-top-offset and roi-bottom-offset would act in crop pixels, from the crop's top and bottom edges, so the
 *   element doesn't apply them in secondary mode: it runs the post-processing with both at 0 (ours, not measured on
 *   DeepStream 8.0; the config warns when they are set). The size filters (detected-min/max-w/h) are the same in crop
 *   and frame pixels. */
G_GNUC_INTERNAL gboolean rds_infer_map_child (const RdsBufSurfTransformRect * crop, guint frame_w, guint frame_h,
    const RdsInferDetection * in, RdsInferDetection * out);

G_END_DECLS

#endif /* RDS_INFER_UNITS_H */
