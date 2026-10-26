/* Redstream: rdsosd's drawing kernels, compiled by amdclang++ (contraction off) and linked in as an object.
 * C-compatible C++ only: no exceptions, no STL. The job layout and the launcher are in rds-osd-gpu.h.
 *
 * The draw: one block per work item, 32 × 4 threads over the 32 × 32 tile, each thread 8 pixels of a column (rows r,
 * r + 4, ..., r + 28, so a wave's loads and stores are whole tile rows; of 1, 2, 4 and 8 pixels a thread, 8 drew
 * fastest on an RDNA2 iGPU). A thread reads its pixels once, walks the tile's command list once for all of them (the
 * same steps as rds_osd_shade_pixel, with full coverage blended through the colour's table when it has one) and
 * writes back the pixels a command changed.
 *
 * The mask upload: one launch for all the pass's copies (usually one: new masks are placed back to back), blocks of
 * 16 bytes a thread.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <hip/hip_runtime.h>

#include "rds-osd-gpu.h"

#define TILE 32
#define ROWS 8                  /* pixels a thread */
#define UPLOAD_BLOCK 256

RDS_OSD_STATIC_ASSERT (TILE % ROWS == 0, "tile rows");

__global__ static void
rds_osd_upload_kernel (const RdsOsdGpuCopy * copies, const uint8_t * upload, uint8_t * heap, uint32_t blocks_a_copy)
{
  RdsOsdGpuCopy c = copies[blockIdx.x / blocks_a_copy];
  uint32_t first = (blockIdx.x % blocks_a_copy) * UPLOAD_BLOCK + threadIdx.x, step = blocks_a_copy * UPLOAD_BLOCK;
  const uint4 *src = (const uint4 *) (upload + c.src);
  uint4 *dst = (uint4 *) (heap + c.dst);

  for (uint32_t i = first; i < c.size / 16; i += step)
    dst[i] = src[i];
  for (uint32_t i = c.size / 16 * 16 + first; i < c.size; i += step)
    heap[c.dst + i] = upload[c.src + i];
}

__global__ static void __launch_bounds__ (TILE * TILE / ROWS)
rds_osd_draw_kernel (RdsOsdGpuJob job)
{
  const RdsOsdGpuItem w = job.items[blockIdx.x];
  const RdsOsdGpuSurface s = job.surfaces[w.slot];
  const uint32_t *list = job.lists + w.list_offset;
  int32_t x = (int32_t) w.tile_x * TILE + (int32_t) threadIdx.x;
  int32_t y0 = (int32_t) w.tile_y * TILE + (int32_t) threadIdx.y;
  uint32_t px[ROWS];
  uint8_t coverage[ROWS];
  unsigned changed = 0;

  if (x >= s.width || y0 >= s.height)
    return;
#pragma unroll
  for (int r = 0; r < ROWS; r++) {
    int32_t y = y0 + r * (TILE / ROWS);

    /* rows below the frame: no command covers them (boxes are inside the frame) */
    px[r] = y < s.height ? *(const uint32_t *) (s.data + (size_t) y * s.pitch + 4 * (size_t) x) : 0;
    coverage[r] = 0;
  }

  for (uint32_t i = 0; i < w.count; i++) {
    uint32_t k = list[i];
    const RdsOsdCmd *c = &job.cmds[k];

    /* rds_osd_cmd_coverage, with its column test done once for the rows and boxes inline */
    if (x >= c->x0 && x <= c->x1) {
      if (c->type == RDS_OSD_CMD_BOX) {
#pragma unroll
        for (int r = 0; r < ROWS; r++) {
          int32_t y = y0 + r * (TILE / ROWS);

          if (y >= c->y0 && y <= c->y1)
            coverage[r] = 255;
        }
      } else {
#pragma unroll
        for (int r = 0; r < ROWS; r++) {
          uint8_t v = rds_osd_cmd_coverage (c, x, y0 + r * (TILE / ROWS), job.heap);

          coverage[r] = v > coverage[r] ? v : coverage[r];
        }
      }
    }
    if ((c->flags & RDS_OSD_CMD_UNION_NEXT) && i + 1 < w.count)
      continue;
    /* the shape ends: blended once, in this command's colour */
#pragma unroll
    for (int r = 0; r < ROWS; r++) {
      if (coverage[r] == 0)
        continue;
      if (coverage[r] == 255 && job.lut_of[k] != RDS_OSD_GPU_NO_LUT) {
        const uint8_t *lut = job.luts + (size_t) job.lut_of[k] * 1024;
        uint32_t p = px[r];

        px[r] = lut[(p & 0xff) * 4] | (uint32_t) lut[((p >> 8) & 0xff) * 4 + 1] << 8
            | (uint32_t) lut[((p >> 16) & 0xff) * 4 + 2] << 16 | (uint32_t) lut[(p >> 24) * 4 + 3] << 24;
      } else {
        uint8_t b[4];

        memcpy (b, &px[r], 4);
        rds_osd_blend (b, &c->color, coverage[r]);
        memcpy (&px[r], b, 4);
      }
      coverage[r] = 0;
      changed |= 1u << r;
    }
  }

#pragma unroll
  for (int r = 0; r < ROWS; r++)
    if (changed & (1u << r))
      *(uint32_t *) (s.data + (size_t) (y0 + r * (TILE / ROWS)) * s.pitch + 4 * (size_t) x) = px[r];
}

extern "C" int
rds_osd_gpu_launch (const RdsOsdGpuJob * job, void *stream)
{
  (void) hipGetLastError ();    /* clear a stale error from an earlier call */
  if (job->n_copies > 0) {
    /* up to 64 blocks a copy, of 16 bytes a thread */
    uint32_t blocks = (job->max_copy + UPLOAD_BLOCK * 16 - 1) / (UPLOAD_BLOCK * 16);

    blocks = blocks < 1 ? 1 : blocks > 64 ? 64 : blocks;
    hipLaunchKernelGGL (rds_osd_upload_kernel, dim3 (job->n_copies * blocks), dim3 (UPLOAD_BLOCK), 0,
        (hipStream_t) stream, job->copies, job->upload, job->heap, blocks);
    if (hipGetLastError () != hipSuccess)
      return -1;
  }
  if (job->n_items > 0)
    hipLaunchKernelGGL (rds_osd_draw_kernel, dim3 (job->n_items), dim3 (TILE, TILE / ROWS), 0,
        (hipStream_t) stream, *job);
  return hipGetLastError () == hipSuccess ? 0 : -1;
}
