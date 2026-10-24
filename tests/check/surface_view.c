/* Surface views (rds_buffer_new_surface_view) of a system-memory batch.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds-memory.h>

static const RdsBufSurfaceCreateParams nv12_64 = {
  .width = 64, .height = 48, .colorFormat = RDS_BUF_COLOR_FORMAT_NV12, .layout = RDS_BUF_LAYOUT_PITCH,
};

/* A batch of 3 with numFilled = 3, slot i filled with 10 * (i + 1). */
static GstBuffer *
new_batch (GstAllocator * alloc)
{
  GstBuffer *buf = gst_buffer_new ();
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (alloc, 3, &nv12_64));
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  for (guint i = 0; i < 3; i++)
    g_assert_cmpint (rds_buf_surface_memset (s, i, -1, 10 * (i + 1)), ==, 0);
  s->numFilled = 3;
  return buf;
}

/* TRUE if every pixel byte of every plane of surface 0 of @s equals @v (row padding is not copied, so skipped). */
static gboolean
slot_is (const RdsBufSurface * s, guint8 v)
{
  const RdsBufSurfaceParams *sp = &s->surfaceList[0];
  const RdsBufSurfacePlaneParams *pp = &sp->planeParams;

  for (guint p = 0; p < pp->num_planes; p++) {
    const guint8 *ptr = (const guint8 *) sp->dataPtr + pp->offset[p];

    for (guint32 y = 0; y < pp->height[p]; y++) {
      for (guint32 x = 0; x < pp->width[p] * pp->bytesPerPix[p]; x++) {
        if (ptr[(gsize) y * pp->pitch[p] + x] != v)
          return FALSE;
      }
    }
  }
  return TRUE;
}

static void
test_shows_slot (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  RdsBufSurface *bs = rds_buffer_get_surface (batch);

  for (guint i = 0; i < 3; i++) {
    GstBuffer *view = rds_buffer_new_surface_view (batch, i);
    GstMemory *mem;
    RdsBufSurface *vs;
    GstMapInfo info;

    g_assert_nonnull (view);
    g_assert_cmpuint (gst_buffer_n_memory (view), ==, 1);
    mem = gst_buffer_peek_memory (view, 0);
    g_assert_true (rds_is_rds_memory (mem));
    g_assert_true (rds_memory_is_view (mem));
    g_assert_true (GST_MEMORY_IS_READONLY (mem));
    g_assert_false (rds_memory_is_view (gst_buffer_peek_memory (batch, 0)));

    vs = rds_buffer_get_surface (view);
    g_assert_nonnull (vs);
    g_assert_true (vs != bs);
    g_assert_cmpuint (vs->batchSize, ==, 1);
    g_assert_cmpuint (vs->numFilled, ==, 1);
    g_assert_cmpint (vs->memType, ==, RDS_BUF_MEM_SYSTEM);
    g_assert_cmpuint (vs->gpuId, ==, bs->gpuId);
    g_assert_true (vs->surfaceList[0].dataPtr == bs->surfaceList[i].dataPtr);
    g_assert_cmpuint (vs->surfaceList[0].width, ==, 64);
    g_assert_cmpuint (vs->surfaceList[0].dataSize, ==, bs->surfaceList[i].dataSize);
    g_assert_true (slot_is (vs, 10 * (i + 1)));

    /* A read map yields the view surface. */
    g_assert_true (gst_buffer_map (view, &info, GST_MAP_READ));
    g_assert_true (info.data == (guint8 *) vs);
    gst_buffer_unmap (view, &info);

    /* rds_buf_surface map/unmap work on a view, in place for host memory. */
    g_assert_cmpint (rds_buf_surface_map (vs, 0, -1, RDS_BUF_MAP_READ), ==, 0);
    g_assert_true (vs->surfaceList[0].mappedAddr.addr[0] == vs->surfaceList[0].dataPtr);
    g_assert_cmpint (rds_buf_surface_sync_for_cpu (vs, 0, -1), ==, 0);
    g_assert_cmpint (rds_buf_surface_unmap (vs, 0, -1), ==, 0);
    /* The parent's mapping is not touched by the view's. */
    g_assert_null (bs->surfaceList[i].mappedAddr.addr[0]);

    gst_buffer_unref (view);
  }

  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

static void
test_refs_parent (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  GstBuffer *v0, *v1;

  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 1);
  v0 = rds_buffer_new_surface_view (batch, 0);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 2);
  v1 = rds_buffer_new_surface_view (batch, 1);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 3);
  g_assert_false (gst_buffer_is_writable (batch));
  gst_buffer_unref (v0);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 2);
  gst_buffer_unref (v1);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 1);

  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

/* The view keeps the batch alive after its last external ref is gone (checked under ASan). */
static void
test_outlives_batch (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  GstBuffer *view = rds_buffer_new_surface_view (batch, 2);
  GstBuffer *copy;

  gst_buffer_unref (batch);
  g_assert_true (slot_is (rds_buffer_get_surface (view), 30));
  copy = gst_buffer_copy_deep (view);
  gst_buffer_unref (view);
  g_assert_true (slot_is (rds_buffer_get_surface (copy), 30));
  gst_buffer_unref (copy);
  gst_object_unref (alloc);
}

static void
test_write_map_fails (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  GstBuffer *view = rds_buffer_new_surface_view (batch, 1);
  GstMemory *mem = gst_buffer_peek_memory (view, 0);
  GstMapInfo info;

  g_assert_false (gst_memory_map (mem, &info, GST_MAP_WRITE));
  g_assert_false (gst_memory_map (mem, &info, GST_MAP_READWRITE));
  g_assert_true (gst_memory_map (mem, &info, GST_MAP_READ));
  gst_memory_unmap (mem, &info);
  /* Still the same memory: a failed memory map swaps nothing. */
  g_assert_true (gst_buffer_peek_memory (view, 0) == mem);

  gst_buffer_unref (view);
  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

/* The documented recipe for writing pixels: make the buffer writable, then map it for writing. GStreamer swaps a
 * private owned copy of the slot into the buffer; the batch is untouched. */
static void
check_make_writable_write_map (gboolean extra_ref)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  RdsBufSurface *bs = rds_buffer_get_surface (batch);
  GstBuffer *view = rds_buffer_new_surface_view (batch, 1);
  GstBuffer *other = extra_ref ? gst_buffer_ref (view) : NULL;
  RdsBufSurface *vs = rds_buffer_get_surface (view);
  RdsBufSurface *ws;
  GstMemory *wmem;
  GstMapInfo info;

  view = gst_buffer_make_writable (view);
  g_assert_true (gst_buffer_map (view, &info, GST_MAP_WRITE));
  ws = (RdsBufSurface *) info.data;
  g_assert_true (ws != vs);
  g_assert_true (ws == rds_buffer_get_surface (view));
  wmem = gst_buffer_peek_memory (view, 0);
  g_assert_false (rds_memory_is_view (wmem));
  g_assert_false (GST_MEMORY_IS_READONLY (wmem));
  g_assert_cmpuint (ws->batchSize, ==, 1);
  g_assert_cmpuint (ws->numFilled, ==, 1);
  g_assert_cmpint (ws->memType, ==, RDS_BUF_MEM_SYSTEM);
  g_assert_true (ws->surfaceList[0].dataPtr != bs->surfaceList[1].dataPtr);
  g_assert_true (slot_is (ws, 20));
  g_assert_cmpint (rds_buf_surface_memset (ws, 0, -1, 99), ==, 0);
  gst_buffer_unmap (view, &info);

  for (guint i = 0; i < 3; i++) {
    RdsBufSurface one = *bs;

    one.surfaceList = &bs->surfaceList[i];
    g_assert_true (slot_is (&one, 10 * (i + 1)));
  }
  if (other != NULL) {
    g_assert_true (rds_buffer_get_surface (other) == vs);
    g_assert_true (slot_is (vs, 20));
    gst_buffer_unref (other);
  }
  /* The copy no longer refs the batch. */
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 1);

  gst_buffer_unref (view);
  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

static void
test_make_writable_write_map (void)
{
  check_make_writable_write_map (FALSE);
}

static void
test_make_writable_write_map_shared (void)
{
  check_make_writable_write_map (TRUE);
}

static void
test_memory_copy (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  GstBuffer *view = rds_buffer_new_surface_view (batch, 0);
  GstMemory *copy = gst_memory_copy (gst_buffer_peek_memory (view, 0), 0, -1);
  RdsBufSurface *cs;
  GstMapInfo info;

  g_assert_nonnull (copy);
  g_assert_false (rds_memory_is_view (copy));
  g_assert_false (GST_MEMORY_IS_READONLY (copy));
  cs = rds_memory_get_surface (copy);
  g_assert_cmpuint (cs->batchSize, ==, 1);
  g_assert_cmpuint (cs->numFilled, ==, 1);
  g_assert_true (slot_is (cs, 10));
  g_assert_true (gst_memory_map (copy, &info, GST_MAP_WRITE));
  gst_memory_unmap (copy, &info);
  /* Views refuse sub-region shares, as owned memory does. */
  g_assert_null (gst_memory_share (gst_buffer_peek_memory (view, 0), 0, -1));

  gst_memory_unref (copy);
  gst_buffer_unref (view);
  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

static void
test_index_out_of_range (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  GstBuffer *sys = gst_buffer_new_allocate (NULL, 16, NULL);

  rds_buffer_get_surface (batch)->numFilled = 2;
  g_assert_nonnull (rds_buffer_get_surface (batch));
  g_assert_null (rds_buffer_new_surface_view (batch, 2));
  g_assert_null (rds_buffer_new_surface_view (batch, 3));
  g_assert_null (rds_buffer_new_surface_view (sys, 0));
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 1);
  g_assert_false (rds_memory_is_view (gst_buffer_peek_memory (sys, 0)));
  g_assert_false (rds_memory_is_view (NULL));

  gst_buffer_unref (sys);
  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

/* A view of a view shows the same slot and refs the original batch, not the intermediate view. */
static void
test_view_of_view (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  RdsBufSurface *bs = rds_buffer_get_surface (batch);
  GstBuffer *v = rds_buffer_new_surface_view (batch, 2);
  GstBuffer *vv = rds_buffer_new_surface_view (v, 0);

  g_assert_nonnull (vv);
  g_assert_null (rds_buffer_new_surface_view (v, 1));
  g_assert_true (rds_memory_is_view (gst_buffer_peek_memory (vv, 0)));
  g_assert_true (rds_buffer_get_surface (vv)->surfaceList[0].dataPtr == bs->surfaceList[2].dataPtr);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (v), ==, 1);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 3);

  gst_buffer_unref (v);
  gst_buffer_unref (batch);
  g_assert_true (slot_is (rds_buffer_get_surface (vv), 30));
  gst_buffer_unref (vv);
  gst_object_unref (alloc);
}

/* rds_buf_surface_copy takes a view as an ordinary source. */
static void
test_copy_from_view (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *batch = new_batch (alloc);
  GstBuffer *view = rds_buffer_new_surface_view (batch, 1);
  RdsBufSurfaceCreateParams p = nv12_64;
  RdsBufSurface *dst = NULL;

  p.memType = RDS_BUF_MEM_SYSTEM;
  g_assert_cmpint (rds_buf_surface_create (&dst, 1, &p), ==, 0);
  g_assert_cmpint (rds_buf_surface_copy (rds_buffer_get_surface (view), dst), ==, 0);
  g_assert_cmpuint (dst->numFilled, ==, 1);
  g_assert_true (slot_is (dst, 20));
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);

  gst_buffer_unref (view);
  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/surface-view/shows-slot", test_shows_slot);
  g_test_add_func ("/surface-view/refs-parent", test_refs_parent);
  g_test_add_func ("/surface-view/outlives-batch", test_outlives_batch);
  g_test_add_func ("/surface-view/write-map-fails", test_write_map_fails);
  g_test_add_func ("/surface-view/make-writable-write-map", test_make_writable_write_map);
  g_test_add_func ("/surface-view/make-writable-write-map-shared", test_make_writable_write_map_shared);
  g_test_add_func ("/surface-view/memory-copy", test_memory_copy);
  g_test_add_func ("/surface-view/index-out-of-range", test_index_out_of_range);
  g_test_add_func ("/surface-view/view-of-view", test_view_of_view);
  g_test_add_func ("/surface-view/copy-from-view", test_copy_from_view);
  return g_test_run ();
}
