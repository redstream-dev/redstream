/* Redstream: rdsosd's GPU drawing. A pass of the draw planner (rds-osd-plan.h) is drawn by one kernel launch on RGBA
 * device planes, byte for byte as the CPU path (rds_osd_shade_commands) draws it: one block per work item shades its
 * 32 × 32 tile, each pixel read once, shaded with the tile's command list by the coverage and blend of
 * rds-osd-shapes.h, and written once.
 *
 * The pass goes to the GPU in one copy: its commands, tile lists and work items, the batch slots' planes, a blend
 * table per colour and the pass's new mask bytes are packed into a pinned staging block, copied to a device block,
 * and the mask bytes scattered from there into the device mask heap, a mirror of the plan's host heap (MASK commands
 * hold heap offsets). Full coverage blends through the colour's table, 256 × 4 bytes filled by rds_osd_blend as the
 * CPU path fills its own: the same bytes without the per-pixel double-precision blend (slow on small GPUs); partial
 * coverage (text edges) still blends in double precision, or in integers for opaque colours with whole channel values
 * (rds_osd_blend); an opaque colour is stored at full coverage without a table.
 *
 * The part shared with the kernels (rds-osd-gpu.hip) is plain C with no GLib and no HIP types (streams are void *);
 * the C API below it is hidden from the HIP compiler.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_OSD_GPU_H
#define RDS_OSD_GPU_H

#include "rds-osd-shapes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* colours with a blend table in one pass, at most (ours: the others blend in double precision) */
#define RDS_OSD_GPU_MAX_LUTS 256
/* a command without a blend table */
#define RDS_OSD_GPU_NO_LUT 0xffff

/* A batch slot: an RGBA plane in device memory, @pitch bytes a row (a multiple of 4). */
typedef struct
{
  uint8_t *data;
  size_t pitch;
  int32_t width, height;
} RdsOsdGpuSurface;

/* RdsOsdWorkItem's layout, without GLib */
typedef struct
{
  uint32_t slot;
  uint16_t tile_x, tile_y;
  uint32_t list_offset, count;
} RdsOsdGpuItem;

/* Mask bytes to copy: @size bytes from @src in the upload area to @dst in the heap, both multiples of 16. */
typedef struct
{
  uint32_t src, dst, size, reserved;
} RdsOsdGpuCopy;

/* How a command's colour blends at full coverage: @solid when RDS_OSD_GPU_SOLID (an opaque colour: every pixel
 * becomes the same), else through table @lut (RDS_OSD_GPU_NO_LUT: rds_osd_blend). */
typedef struct
{
  uint32_t solid;               /* RGBA bytes in memory order */
  uint16_t lut, flags;
} RdsOsdGpuBlend;

enum
{
  RDS_OSD_GPU_SOLID = 1 << 0,
};

/* A pass in device memory. blends[i]: the full-coverage blend of cmds[i]; luts: 1024 bytes a table, indexed
 * [value][channel]. */
typedef struct
{
  const RdsOsdCmd *cmds;
  const uint32_t *lists;
  const RdsOsdGpuItem *items;
  const RdsOsdGpuSurface *surfaces;     /* indexed by slot */
  const RdsOsdGpuBlend *blends;
  const uint8_t *luts;
  uint32_t n_items;
  const RdsOsdGpuCopy *copies;
  const uint8_t *upload;        /* the upload area the copies read */
  uint32_t n_copies, max_copy;  /* max_copy: the largest copy's size */
  uint8_t *heap;                /* the device mask heap */
} RdsOsdGpuJob;

/* Queues the job's mask copies (if any), then the draw of its work items, on @stream (a hipStream_t) on the current
 * device. Returns 0 (hipSuccess), or the hipError_t of the launch that failed. */
__attribute__ ((visibility ("hidden"))) int rds_osd_gpu_launch (const RdsOsdGpuJob * job, void *stream);

#ifdef __cplusplus
}
#endif

#ifndef __HIP__
#include "rds-osd-plan.h"

G_BEGIN_DECLS

#define RDS_OSD_GPU_ERROR (rds_osd_gpu_error_quark ())
G_GNUC_INTERNAL GQuark rds_osd_gpu_error_quark (void);

typedef struct _RdsOsdGpu RdsOsdGpu;

/* A GPU drawing context on the current device, for a plan with a mask heap of @heap_size bytes (the device heap
 * mirrors it: the plan's mask cache must be empty, or dropped, when it starts drawing with a new context). It owns a
 * non-blocking stream, and grows its staging and device blocks to what passes need. NULL with @error set when the
 * device allocations fail. Not thread-safe: one per streaming thread, like the plan. */
RdsOsdGpu *rds_osd_gpu_new (gsize heap_size, GError ** error);
void rds_osd_gpu_free (RdsOsdGpu * gpu);

/* Draws @pass of @plan, binning it first (rds_osd_plan_bin), on the planes @surfaces[slot] (each frame of the pass
 * needs its slot's plane, of the frame's size), and waits for it: the planes are ready when it returns (ready-on-push).
 * The staging block is reused by the next pass; the wait keeps it from being overwritten while a copy reads it.
 * On failure (an invalid plane, a HIP error) returns FALSE with @error set and drops the plan's mask cache: the device
 * heap may no longer mirror it. Never falls back to the CPU. */
gboolean rds_osd_gpu_draw_pass (RdsOsdGpu * gpu, RdsOsdPlan * plan, RdsOsdPass * pass,
    const RdsOsdGpuSurface * surfaces, guint n_surfaces, GError ** error);

/* (tests) Whether full coverage blends through the colour tables (TRUE by default) or in double precision. */
void rds_osd_gpu_set_luts (RdsOsdGpu * gpu, gboolean use_luts);

G_END_DECLS
#endif

#endif /* RDS_OSD_GPU_H */
