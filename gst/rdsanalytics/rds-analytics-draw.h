/* Redstream: the rdsanalytics overlays (osd-mode, display-obj-cnt) as display meta (plugin-internal, not installed).
 * The element never draws pixels: rdsosd, or any OSD downstream, draws what this attaches. Its own static library,
 * rdsanalytics-draw, since it needs libgstrds' meta types and the core stays GLib-only.
 *
 * DeepStream 8.0 behaviour (observed in its display-meta dumps), unless marked ours:
 * - Display metas hold lines and texts only, every line 2 px wide, every text in "Serif" at display-font-size on a
 *   black background (set_bg_clr 1); coordinates are truncated to integers.
 * - At most 15 lines per display meta: the lines fill one, then the next. The texts all go in the first (ours: past
 *   RDS_MAX_ELEMENTS_IN_DISPLAY_META texts, the next; 8.0 never showed that many). A display meta is acquired only
 *   when something goes in it (ours: 8.0 adds an empty one after a full one). They are appended to the frame's
 *   display_meta_list in the order they fill (ours: 8.0 prepends them, the newest first).
 * - Texts: display-obj-cnt's first, then the rules' labels, in config order (ROI filtering, overcrowding,
 *   direction, line crossing; rules in key order). Lines in the same order.
 * - osd-mode 0 draws nothing but display-obj-cnt's text.
 * - osd-mode 1:
 *   - every enabled ROI-filtering polygon in yellow (1,1,0,1), every overcrowding polygon in orange (1,0.5,0,1): one
 *     line per edge, the closing edge last; the label at the first vertex;
 *   - every direction rule in red (1,0,0,1): the vector, then two arrowhead lines from 15 % of its length back off the
 *     tip, at -15 then +15 degrees from the vector (screen angles, y down), each ending at the tip; the label at the
 *     start point;
 *   - every line-crossing rule in green (0,1,0,1): its direction vector with the arrowhead, then the line; the label
 *     "<label> " (a trailing space) at the line's second point.
 * - osd-mode 2, as 1 with the counts in the labels: "<label>=<objInROIcnt>", "<label> OverCrowding=<True|False>,
 *   Count=<n>", the direction's "<label>" unchanged, "<label>=<objLCCumCnt>". And each object with an object info
 *   gets its objStatus as text_params (replacing any text it had) at (left, top - 10), white (1,1,1,1) on black; its
 *   border turns cyan (0,1,1,1) when it has an ROI-filtering or a line-crossing label on this frame (a direction,
 *   STOPPED or overcrowding alone leave the border as it was). Objects without an object info are untouched.
 * - display-obj-cnt=1, in every osd-mode: "Count for" then " ClassId<c>=<n>" per objCnt entry (sorted by the class
 *   id as unsigned) at (5,5) in yellow. Ours: class -1 prints as -1 (8.0 prints 4294967295).
 * - Ours: a coordinate below 0 (an arrowhead off the frame, an object's top under 10 px) becomes 0, as the fields
 *   are unsigned.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ANALYTICS_DRAW_H
#define RDS_ANALYTICS_DRAW_H

#include "rds-analytics-core.h"

#include <rds/rds-meta.h>

G_BEGIN_DECLS

/* Attaches the overlays of frame @fm (stream @fm->pad_index) to it, from @cfg's osd-mode, display-obj-cnt and
 * display-font-size, the rules of @core and the frame's result @res; @objs are the frame's object metas in the
 * order of @res->objects (only osd-mode 2 writes to them). Called with the batch meta lock held, before the next
 * rds_analytics_core_process_frame. Nothing is attached when there is nothing to draw. */
G_GNUC_INTERNAL void rds_analytics_draw_frame (RdsFrameMeta * fm, const RdsAnalyticsConfig * cfg,
    const RdsAnalyticsCore * core, const RdsAnalyticsFrameResult * res, RdsObjectMeta * const *objs);

G_END_DECLS

#endif /* RDS_ANALYTICS_DRAW_H */
