/* Redstream: rdsosd's GPU drawing context (see rds-osd-gpu.h): packing a pass into one upload, and the kernels'
 * launch on the context's stream.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-gpu.h"

#include <string.h>

/* GLib first: HIP's headers define __noinline__, which breaks GLib if HIP comes first. */
#include <hip/hip_runtime_api.h>

G_DEFINE_QUARK (rds-osd-gpu-error-quark, rds_osd_gpu_error)

G_STATIC_ASSERT (sizeof (RdsOsdGpuItem) == sizeof (RdsOsdWorkItem));
G_STATIC_ASSERT (G_STRUCT_OFFSET (RdsOsdGpuItem, tile_x) == G_STRUCT_OFFSET (RdsOsdWorkItem, tile_x));
G_STATIC_ASSERT (G_STRUCT_OFFSET (RdsOsdGpuItem, tile_y) == G_STRUCT_OFFSET (RdsOsdWorkItem, tile_y));
G_STATIC_ASSERT (G_STRUCT_OFFSET (RdsOsdGpuItem, list_offset) == G_STRUCT_OFFSET (RdsOsdWorkItem, list_offset));
G_STATIC_ASSERT (G_STRUCT_OFFSET (RdsOsdGpuItem, count) == G_STRUCT_OFFSET (RdsOsdWorkItem, count));

#define LUT_SIZE 1024
/* the upload kernel launches up to 64 blocks a copy */
#define MAX_COPIES (G_MAXINT32 / 64)

struct _RdsOsdGpu
{
  hipStream_t stream;
  guint8 *heap;                 /* device */
  gsize heap_size;
  guint8 *staging;              /* pinned host, grow-only */
  guint8 *block;                /* device, grow-only */
  gsize capacity;               /* of both */
  gboolean use_luts;
  GHashTable *colors;           /* RdsOsdColor * (in the pass) -> table index + 1 */
  GArray *lut_of;               /* guint16 per command */
  GByteArray *luts;
  GArray *copies;               /* RdsOsdGpuCopy */
};

/* ---- colour tables ---- */

static guint
color_hash (gconstpointer key)
{
  const guint8 *b = key;
  guint h = 2166136261u;

  for (gsize i = 0; i < sizeof (RdsOsdColor); i++)
    h = (h ^ b[i]) * 16777619u;
  return h;
}

static gboolean
color_equal (gconstpointer a, gconstpointer b)
{
  return memcmp (a, b, sizeof (RdsOsdColor)) == 0;
}

/* The table of each command that ends a shape (blended in its colour), as the CPU path fills its own: the bytes
 * rds_osd_blend gives every value at full coverage. */
static void
build_luts (RdsOsdGpu * gpu, const RdsOsdPass * pass)
{
  g_hash_table_remove_all (gpu->colors);
  g_array_set_size (gpu->lut_of, pass->n_cmds);
  g_byte_array_set_size (gpu->luts, 0);
  for (guint32 i = 0; i < pass->n_cmds; i++) {
    const RdsOsdCmd *c = &pass->cmds[i];
    guint16 *lut_of = &g_array_index (gpu->lut_of, guint16, i);
    guint index = GPOINTER_TO_UINT (g_hash_table_lookup (gpu->colors, &c->color));

    *lut_of = RDS_OSD_GPU_NO_LUT;
    if (!gpu->use_luts || (c->flags & RDS_OSD_CMD_UNION_NEXT))
      continue;
    if (index == 0) {
      guint8 *lut;

      if (g_hash_table_size (gpu->colors) >= RDS_OSD_GPU_MAX_LUTS)
        continue;
      index = g_hash_table_size (gpu->colors) + 1;
      g_hash_table_insert (gpu->colors, (gpointer) & c->color, GUINT_TO_POINTER (index));
      g_byte_array_set_size (gpu->luts, index * LUT_SIZE);
      lut = gpu->luts->data + (index - 1) * LUT_SIZE;
      for (guint d = 0; d < 256; d++) {
        memset (lut + 4 * d, (int) d, 4);
        rds_osd_blend (lut + 4 * d, &c->color, 255);
      }
    }
    *lut_of = (guint16) (index - 1);
  }
}

/* ---- the pass ---- */

static gsize
align16 (gsize v)
{
  return (v + 15) & ~(gsize) 15;
}

static gboolean
hip_ok (hipError_t e, const gchar * what, GError ** error)
{
  if (e == hipSuccess)
    return TRUE;
  g_set_error (error, RDS_OSD_GPU_ERROR, 0, "%s: %s", what, hipGetErrorString (e));
  return FALSE;
}

/* Grows the staging and device blocks to @size bytes at least; their contents are not kept. */
static gboolean
reserve (RdsOsdGpu * gpu, gsize size, GError ** error)
{
  gsize capacity = MAX (gpu->capacity, 64 * 1024);

  if (size <= gpu->capacity)
    return TRUE;
  while (capacity < size)
    capacity *= 2;
  if (gpu->staging != NULL)
    (void) hipHostFree (gpu->staging);
  if (gpu->block != NULL)
    (void) hipFree (gpu->block);
  gpu->staging = NULL;
  gpu->block = NULL;
  gpu->capacity = 0;
  if (!hip_ok (hipHostMalloc ((void **) &gpu->staging, capacity, hipHostMallocDefault), "hipHostMalloc", error)
      || !hip_ok (hipMalloc ((void **) &gpu->block, capacity), "hipMalloc", error))
    return FALSE;
  gpu->capacity = capacity;
  return TRUE;
}

/* Copies @size bytes from @src to @at in the staging block (@src may be NULL when @size is 0). */
static void
stage (RdsOsdGpu * gpu, gsize at, gconstpointer src, gsize size)
{
  if (size > 0)
    memcpy (gpu->staging + at, src, size);
}

static gboolean
surfaces_ok (const RdsOsdPass * pass, const RdsOsdGpuSurface * surfaces, guint n_surfaces, GError ** error)
{
  for (guint32 i = 0; i < pass->n_frames; i++) {
    const RdsOsdPlanFrame *f = &pass->frames[i];
    const RdsOsdGpuSurface *s = f->slot < n_surfaces ? &surfaces[f->slot] : NULL;

    if (s == NULL || s->data == NULL || ((guintptr) s->data & 3) != 0 || s->width != f->width
        || s->height != f->height || s->pitch % 4 != 0 || s->pitch < 4 * (gsize) s->width) {
      g_set_error (error, RDS_OSD_GPU_ERROR, 0, "no valid %dx%d RGBA plane for slot %u", f->width, f->height,
          f->slot);
      return FALSE;
    }
  }
  return TRUE;
}

/* Packs @pass into the staging block, copies it and launches the kernels; does not wait. */
static gboolean
draw_pass (RdsOsdGpu * gpu, RdsOsdPlan * plan, RdsOsdPass * pass, const RdsOsdGpuSurface * surfaces,
    guint n_surfaces, GError ** error)
{
  const guint8 *host_heap = rds_osd_plan_heap (plan);
  gsize at_surfaces, at_cmds, at_lists, at_items, at_lut_of, at_luts, at_copies, at_upload, size;
  guint32 max_copy = 0, upload_size = 0;
  RdsOsdGpuJob job;

  if (!surfaces_ok (pass, surfaces, n_surfaces, error))
    return FALSE;
  rds_osd_plan_bin (plan, pass);
  build_luts (gpu, pass);
  g_array_set_size (gpu->copies, 0);
  for (guint32 i = 0; i < pass->n_uploads; i++) {
    const RdsOsdMaskUpload *up = &pass->uploads[i];
    RdsOsdGpuCopy c = { upload_size, up->offset, up->size, 0 };

    if ((guint64) up->offset + up->size > gpu->heap_size || i >= MAX_COPIES) {
      g_set_error (error, RDS_OSD_GPU_ERROR, 0, "mask upload %u+%u outside the %" G_GSIZE_FORMAT "-byte heap",
          up->offset, up->size, gpu->heap_size);
      return FALSE;
    }
    g_array_append_val (gpu->copies, c);
    max_copy = MAX (max_copy, up->size);
    /* below the heap's size, itself below 4 GiB */
    upload_size = (guint32) align16 ((gsize) upload_size + up->size);
  }

  /* the layout of the block, every array 16-byte aligned */
  at_surfaces = 0;
  at_cmds = align16 (at_surfaces + n_surfaces * sizeof (RdsOsdGpuSurface));
  at_lists = align16 (at_cmds + (gsize) pass->n_cmds * sizeof (RdsOsdCmd));
  at_items = align16 (at_lists + (gsize) pass->n_lists * sizeof (guint32));
  at_lut_of = align16 (at_items + (gsize) pass->n_items * sizeof (RdsOsdGpuItem));
  at_luts = align16 (at_lut_of + (gsize) pass->n_cmds * sizeof (guint16));
  at_copies = align16 (at_luts + gpu->luts->len);
  at_upload = align16 (at_copies + gpu->copies->len * sizeof (RdsOsdGpuCopy));
  size = at_upload + upload_size;
  if (!reserve (gpu, size, error))
    return FALSE;

  /* one host copy of each array, one copy to the device */
  stage (gpu, at_surfaces, surfaces, n_surfaces * sizeof (RdsOsdGpuSurface));
  stage (gpu, at_cmds, pass->cmds, (gsize) pass->n_cmds * sizeof (RdsOsdCmd));
  stage (gpu, at_lists, pass->lists, (gsize) pass->n_lists * sizeof (guint32));
  stage (gpu, at_items, pass->items, (gsize) pass->n_items * sizeof (RdsOsdGpuItem));
  stage (gpu, at_lut_of, gpu->lut_of->data, (gsize) pass->n_cmds * sizeof (guint16));
  stage (gpu, at_luts, gpu->luts->data, gpu->luts->len);
  stage (gpu, at_copies, gpu->copies->data, gpu->copies->len * sizeof (RdsOsdGpuCopy));
  for (guint32 i = 0; i < gpu->copies->len; i++) {
    const RdsOsdGpuCopy *c = &g_array_index (gpu->copies, RdsOsdGpuCopy, i);

    stage (gpu, at_upload + c->src, host_heap + c->dst, c->size);
  }
  if (!hip_ok (hipMemcpyAsync (gpu->block, gpu->staging, size, hipMemcpyHostToDevice, gpu->stream),
          "hipMemcpyAsync", error))
    return FALSE;

  job = (RdsOsdGpuJob) {
    .cmds = (const RdsOsdCmd *) (gpu->block + at_cmds),
    .lists = (const uint32_t *) (gpu->block + at_lists),
    .items = (const RdsOsdGpuItem *) (gpu->block + at_items),
    .surfaces = (const RdsOsdGpuSurface *) (gpu->block + at_surfaces),
    .lut_of = (const uint16_t *) (gpu->block + at_lut_of),
    .luts = gpu->block + at_luts,
    .n_items = pass->n_items,
    .copies = (const RdsOsdGpuCopy *) (gpu->block + at_copies),
    .upload = gpu->block + at_upload,
    .n_copies = gpu->copies->len,
    .max_copy = max_copy,
    .heap = gpu->heap,
  };
  if (rds_osd_gpu_launch (&job, gpu->stream) != 0) {
    g_set_error (error, RDS_OSD_GPU_ERROR, 0, "drawing kernel launch failed: %s",
        hipGetErrorString (hipGetLastError ()));
    return FALSE;
  }
  return TRUE;
}

gboolean
rds_osd_gpu_draw_pass (RdsOsdGpu * gpu, RdsOsdPlan * plan, RdsOsdPass * pass, const RdsOsdGpuSurface * surfaces,
    guint n_surfaces, GError ** error)
{
  gboolean ok;

  g_return_val_if_fail (gpu != NULL && plan != NULL && pass != NULL, FALSE);
  g_return_val_if_fail (surfaces != NULL || n_surfaces == 0, FALSE);
  if (pass->n_cmds == 0 && pass->n_uploads == 0)
    return TRUE;
  ok = draw_pass (gpu, plan, pass, surfaces, n_surfaces, error);
  /* ready-on-push; and the staging block is free again for the next pass. A failure after the copy was queued
   * waits too, so that no copy still reads the block. */
  if (!hip_ok (hipStreamSynchronize (gpu->stream), "hipStreamSynchronize", ok ? error : NULL))
    ok = FALSE;
  /* the pass's new masks may be missing from the device heap, or only partly there */
  if (!ok)
    rds_osd_plan_drop_masks (plan);
  return ok;
}

/* ---- the context ---- */

void
rds_osd_gpu_set_luts (RdsOsdGpu * gpu, gboolean use_luts)
{
  g_return_if_fail (gpu != NULL);
  gpu->use_luts = use_luts;
}

RdsOsdGpu *
rds_osd_gpu_new (gsize heap_size, GError ** error)
{
  RdsOsdGpu *gpu;

  g_return_val_if_fail (heap_size > 0 && heap_size % RDS_OSD_MASK_ALIGN == 0 && heap_size <= G_MAXUINT32, NULL);
  gpu = g_new0 (RdsOsdGpu, 1);
  gpu->heap_size = heap_size;
  gpu->use_luts = TRUE;
  gpu->colors = g_hash_table_new (color_hash, color_equal);
  gpu->lut_of = g_array_new (FALSE, FALSE, sizeof (guint16));
  gpu->luts = g_byte_array_new ();
  gpu->copies = g_array_new (FALSE, FALSE, sizeof (RdsOsdGpuCopy));
  if (!hip_ok (hipStreamCreateWithFlags (&gpu->stream, hipStreamNonBlocking), "hipStreamCreateWithFlags", error)
      || !hip_ok (hipMalloc ((void **) &gpu->heap, heap_size), "hipMalloc", error)) {
    rds_osd_gpu_free (gpu);
    return NULL;
  }
  return gpu;
}

void
rds_osd_gpu_free (RdsOsdGpu * gpu)
{
  if (gpu == NULL)
    return;
  if (gpu->stream != NULL)
    (void) hipStreamSynchronize (gpu->stream);
  if (gpu->staging != NULL)
    (void) hipHostFree (gpu->staging);
  if (gpu->block != NULL)
    (void) hipFree (gpu->block);
  if (gpu->heap != NULL)
    (void) hipFree (gpu->heap);
  if (gpu->stream != NULL)
    (void) hipStreamDestroy (gpu->stream);
  g_hash_table_unref (gpu->colors);
  g_array_unref (gpu->lut_of);
  g_byte_array_unref (gpu->luts);
  g_array_unref (gpu->copies);
  g_free (gpu);
}
