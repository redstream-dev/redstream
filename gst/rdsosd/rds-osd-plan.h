/* Redstream: rdsosd's draw planner. Turns a batch's frame metas into the draw commands of rds-osd-shapes.h, in draw
 * order, as flat arrays without pointers that the CPU shades directly and the GPU can take as they are: the commands,
 * the frames they belong to, and on request the 32 × 32 tiles each command touches (work items over command index
 * lists).
 *
 * Text is rasterized by Pango into A8 coverage masks in one mask heap (host memory here; the GPU keeps a copy). Masks
 * are cached by text, font and size (and where the frame's edge clips them), placed first fit and evicted least
 * recently used, those the rest of the batch needs last. A batch whose new masks do not fit the heap, even with
 * every mask it does not use evicted, is planned in several passes: each pass is a run of the batch's commands, in
 * order, whose masks are all in the heap at once.
 * Drawing the passes in order gives the same pixels as one pass with a heap large enough.
 *
 * Use: rds_osd_plan_begin, rds_osd_plan_add_frame for each frame of the batch, then rds_osd_plan_next_pass until it
 * returns FALSE, drawing each pass before asking for the next (the next pass may overwrite heap bytes the previous
 * one reads). Not thread-safe: one plan per streaming thread.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_OSD_PLAN_H
#define RDS_OSD_PLAN_H

#include "rds-osd-draw.h"
#include "rds-osd-shapes.h"

G_BEGIN_DECLS

/* the side of a tile in pixels */
#define RDS_OSD_TILE_SIZE 32
/* the mask heap of rdsosd (ours): a text whose mask is larger is not drawn */
#define RDS_OSD_MASK_HEAP_SIZE (8 * 1024 * 1024)
/* masks start at multiples of this in the heap */
#define RDS_OSD_MASK_ALIGN 16
/* frames wider or taller than this, or of more than RDS_OSD_MAX_PLAN_TILES tiles, are not drawn (ours) */
#define RDS_OSD_MAX_PLAN_FRAME RDS_OSD_MAX_COORD
#define RDS_OSD_MAX_PLAN_TILES (1 << 20)
/* the binning of a pass (its tile lists and its slots' tile counts) takes at most this many 32-bit words, beyond one
 * shape alone: a batch that would need more is planned in more passes */
#define RDS_OSD_PASS_BUDGET ((guint64) 1 << 26)

/* One frame's commands in a pass: cmds[first .. first + n_cmds − 1] of the pass, drawn on batch slot @slot (a
 * @width × @height plane). A slot can have several frames (frame metas sharing a batch_id), drawn in order. */
typedef struct
{
  guint32 slot;
  gint32 width, height;
  guint32 first, n_cmds;
} RdsOsdPlanFrame;

/* A non-empty tile of a slot: the pixels [tile_x · 32, tile_x · 32 + 31] × [tile_y · 32, tile_y · 32 + 31] within the
 * frame, shaded with the commands lists[list_offset .. list_offset + count − 1] (indices into the pass's cmds, in
 * draw order: every command whose bounding box touches the tile, a shape's commands all or none). */
typedef struct
{
  guint32 slot;
  guint16 tile_x, tile_y;       /* a frame is at most 65536 tiles a side (RDS_OSD_MAX_PLAN_FRAME) */
  guint32 list_offset, count;
} RdsOsdWorkItem;

G_STATIC_ASSERT (sizeof (RdsOsdWorkItem) == 16);
G_STATIC_ASSERT (RDS_OSD_MAX_PLAN_FRAME / RDS_OSD_TILE_SIZE <= 65536);

/* Mask bytes new in a pass: heap[offset .. offset + size − 1], to copy to the GPU's heap before drawing it (masks
 * placed back to back share one upload). */
typedef struct
{
  guint32 offset, size;
} RdsOsdMaskUpload;

/* A pass: arrays owned by the plan, valid until the next rds_osd_plan_next_pass or rds_osd_plan_begin. MASK commands
 * hold heap offsets. items and lists are filled by rds_osd_plan_bin (empty before). */
typedef struct
{
  const RdsOsdCmd *cmds;
  guint32 n_cmds;
  const RdsOsdPlanFrame *frames;
  guint32 n_frames;
  const RdsOsdMaskUpload *uploads;
  guint32 n_uploads;
  guint32 n_new_masks;          /* masks rasterized for this pass */
  const RdsOsdWorkItem *items;
  guint32 n_items;
  const guint32 *lists;
  guint32 n_lists;
} RdsOsdPass;

typedef struct _RdsOsdPlan RdsOsdPlan;

/* A plan with a mask heap of @heap_size bytes (RDS_OSD_MASK_HEAP_SIZE in rdsosd): a multiple of RDS_OSD_MASK_ALIGN,
 * below 4 GiB. */
RdsOsdPlan *rds_osd_plan_new (gsize heap_size);
void rds_osd_plan_free (RdsOsdPlan * plan);

/* The host mask heap that MASK commands point into. */
const guint8 *rds_osd_plan_heap (RdsOsdPlan * plan);

/* Starts a batch (drops the previous one; the mask cache stays). */
void rds_osd_plan_begin (RdsOsdPlan * plan);

/* Adds the commands of @frame (NULL: the clock only) on slot @slot, a @width × @height plane, in the order of
 * rds_osd_draw_frame. Before the first rds_osd_plan_next_pass of the batch. Returns what had to be adjusted. Every
 * frame of a slot has the slot's size: a frame of another size on a slot that already has commands is not drawn (a
 * critical warning). */
RdsOsdDrawFlags rds_osd_plan_add_frame (RdsOsdPlan * plan, const RdsOsdDrawOptions * options, guint slot,
    const RdsFrameMeta * frame, gint width, gint height);

/* The batch's next pass in @pass, its new masks rasterized into the heap; FALSE when every command has been
 * planned (a batch without commands has no pass). */
gboolean rds_osd_plan_next_pass (RdsOsdPlan * plan, RdsOsdPass * pass);

/* Empties the mask cache: the next passes rasterize and upload every mask they use again (after the GPU's heap was
 * lost or reallocated). */
void rds_osd_plan_drop_masks (RdsOsdPlan * plan);

/* (tests) The binning budget of a pass in 32-bit words (RDS_OSD_PASS_BUDGET by default). */
void rds_osd_plan_set_pass_budget (RdsOsdPlan * plan, guint64 words);

/* Fills @pass's work items and lists: for each slot in the order of its first frame, its non-empty tiles in row
 * order. */
void rds_osd_plan_bin (RdsOsdPlan * plan, RdsOsdPass * pass);

/* Loads the default fonts, so the first frame does not pay for fontconfig's start-up. */
void rds_osd_plan_warm_up (RdsOsdPlan * plan);

/* The family fontconfig resolves @name to; NULL if none. Free with g_free. */
gchar *rds_osd_plan_resolve_font (RdsOsdPlan * plan, const gchar * name);

G_END_DECLS

#endif /* RDS_OSD_PLAN_H */
