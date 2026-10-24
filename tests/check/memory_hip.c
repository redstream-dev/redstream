/* memory:RDS on HIP device memory; skips without a ROCm device.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include <rds/rds-memory.h>
#include <rds/rds-transform.h>

#include <string.h>

static const RdsBufSurfaceCreateParams nv12_640 = {
  .width = 640, .height = 480, .colorFormat = RDS_BUF_COLOR_FORMAT_NV12, .layout = RDS_BUF_LAYOUT_PITCH,
};

/* Reads the first byte of surface 0 by copying the device surface into system memory. */
static guint8
first_byte (const RdsBufSurface * dev)
{
  RdsBufSurfaceCreateParams p = nv12_640;
  RdsBufSurface *sys = NULL;
  guint8 v;

  p.memType = RDS_BUF_MEM_SYSTEM;
  g_assert_cmpint (rds_buf_surface_create (&sys, dev->batchSize, &p), ==, 0);
  g_assert_cmpint (rds_buf_surface_copy (dev, sys), ==, 0);
  v = ((guint8 *) sys->surfaceList[0].dataPtr)[0];
  g_assert_cmpint (rds_buf_surface_destroy (sys), ==, 0);
  return v;
}

static void
test_default_mem_type (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstMemory *mem = rds_allocator_alloc_surface (alloc, 1, &nv12_640);

  g_assert_nonnull (mem);
  g_assert_cmpint (rds_memory_get_surface (mem)->memType, ==, RDS_BUF_MEM_DEFAULT);
  gst_memory_unref (mem);
  gst_object_unref (alloc);
}

static void
test_copy_is_deep (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstBuffer *buf = gst_buffer_new ();
  RdsBufSurface *a, *b;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (alloc, 1, &nv12_640));
  a = rds_buffer_get_surface (buf);
  g_assert_nonnull (a);
  a->numFilled = 1;
  g_assert_cmpint (rds_buf_surface_memset (a, 0, -1, 42), ==, 0);

  GstBuffer *shallow = gst_buffer_copy (buf);
  g_assert_true (rds_buffer_get_surface (shallow) == a);
  GstBuffer *deep = gst_buffer_copy_deep (buf);
  b = rds_buffer_get_surface (deep);
  g_assert_nonnull (b);
  g_assert_true (b != a);
  g_assert_true (b->surfaceList[0].dataPtr != a->surfaceList[0].dataPtr);
  g_assert_cmpint (b->memType, ==, RDS_BUF_MEM_DEFAULT);
  g_assert_cmpuint (b->numFilled, ==, 1);
  g_assert_cmpuint (first_byte (b), ==, 42);
  g_assert_cmpint (rds_buf_surface_memset (a, 0, -1, 1), ==, 0);
  g_assert_cmpuint (first_byte (a), ==, 1);
  g_assert_cmpuint (first_byte (b), ==, 42);

  gst_buffer_unref (deep);
  gst_buffer_unref (shallow);
  gst_buffer_unref (buf);
  gst_object_unref (alloc);
}

/* ---- surface views of a device batch ---- */

/* The value of pixel byte (x, y) of plane p of slot i. */
static guint8
pattern (guint i, guint p, guint x, guint y)
{
  return (guint8) (x * 3 + y * 5 + p * 11 + i * 37);
}

/* Checks every pixel byte of surface @index of host-readable @planes (per-plane base pointers) against slot @slot. */
static gboolean
matches_slot (const RdsBufSurfaceParams * sp, guint8 * const *planes, guint slot)
{
  const RdsBufSurfacePlaneParams *pp = &sp->planeParams;

  for (guint p = 0; p < pp->num_planes; p++) {
    for (guint32 y = 0; y < pp->height[p]; y++) {
      for (guint32 x = 0; x < pp->width[p] * pp->bytesPerPix[p]; x++) {
        if (planes[p][(gsize) y * pp->pitch[p] + x] != pattern (slot, p, x, y))
          return FALSE;
      }
    }
  }
  return TRUE;
}

/* Copies @s (one surface, any memory type) to system memory and checks it against slot @slot. */
static gboolean
surface_matches_slot (const RdsBufSurface * s, guint slot)
{
  RdsBufSurfaceCreateParams p = nv12_640;
  RdsBufSurface *sys = NULL;
  guint8 *planes[RDS_BUF_SURFACE_MAX_PLANES];
  gboolean ok;

  p.memType = RDS_BUF_MEM_SYSTEM;
  g_assert_cmpuint (s->numFilled, ==, 1);
  g_assert_cmpint (rds_buf_surface_create (&sys, 1, &p), ==, 0);
  g_assert_cmpint (rds_buf_surface_copy (s, sys), ==, 0);
  for (guint i = 0; i < sys->surfaceList[0].planeParams.num_planes; i++)
    planes[i] = (guint8 *) sys->surfaceList[0].dataPtr + sys->surfaceList[0].planeParams.offset[i];
  ok = matches_slot (&sys->surfaceList[0], planes, slot);
  g_assert_cmpint (rds_buf_surface_destroy (sys), ==, 0);
  return ok;
}

/* A device batch of @n (numFilled = n) whose slot i holds pattern (i, ...). */
static GstBuffer *
new_device_batch_n (GstAllocator * alloc, guint n)
{
  RdsBufSurfaceCreateParams p = nv12_640;
  RdsBufSurface *sys = NULL, *dev;
  GstBuffer *buf = gst_buffer_new ();

  p.memType = RDS_BUF_MEM_SYSTEM;
  g_assert_cmpint (rds_buf_surface_create (&sys, n, &p), ==, 0);
  for (guint i = 0; i < n; i++) {
    const RdsBufSurfaceParams *sp = &sys->surfaceList[i];

    for (guint pl = 0; pl < sp->planeParams.num_planes; pl++) {
      guint8 *ptr = (guint8 *) sp->dataPtr + sp->planeParams.offset[pl];

      for (guint32 y = 0; y < sp->planeParams.height[pl]; y++)
        for (guint32 x = 0; x < sp->planeParams.pitch[pl]; x++)
          ptr[(gsize) y * sp->planeParams.pitch[pl] + x] = pattern (i, pl, x, y);
    }
  }
  sys->numFilled = n;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (alloc, n, &nv12_640));
  dev = rds_buffer_get_surface (buf);
  g_assert_nonnull (dev);
  g_assert_cmpint (rds_buf_surface_copy (sys, dev), ==, 0);
  g_assert_cmpuint (dev->numFilled, ==, n);
  g_assert_cmpint (rds_buf_surface_destroy (sys), ==, 0);
  return buf;
}

static GstBuffer *
new_device_batch (GstAllocator * alloc)
{
  return new_device_batch_n (alloc, 3);
}

static void
test_view_device (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstBuffer *batch = new_device_batch (alloc);
  RdsBufSurface *bs = rds_buffer_get_surface (batch);

  g_assert_null (rds_buffer_new_surface_view (batch, 3));
  for (guint i = 0; i < 3; i++) {
    GstBuffer *view = rds_buffer_new_surface_view (batch, i);
    RdsBufSurface *vs;
    guint8 *planes[RDS_BUF_SURFACE_MAX_PLANES];
    GstMapInfo info;

    g_assert_nonnull (view);
    g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 2);
    g_assert_true (rds_memory_is_view (gst_buffer_peek_memory (view, 0)));
    vs = rds_buffer_get_surface (view);
    g_assert_cmpint (vs->memType, ==, RDS_BUF_MEM_DEFAULT);
    g_assert_cmpuint (vs->batchSize, ==, 1);
    g_assert_cmpuint (vs->numFilled, ==, 1);
    g_assert_true (vs->surfaceList[0].dataPtr == bs->surfaceList[i].dataPtr);
    g_assert_true (surface_matches_slot (vs, i));

    /* A CPU map of a device view reads the slot through the view's own staging mirror. */
    g_assert_cmpint (rds_buf_surface_map (vs, 0, -1, RDS_BUF_MAP_READ), ==, 0);
    g_assert_cmpint (rds_buf_surface_sync_for_cpu (vs, 0, -1), ==, 0);
    for (guint p = 0; p < vs->surfaceList[0].planeParams.num_planes; p++) {
      planes[p] = vs->surfaceList[0].mappedAddr.addr[p];
      g_assert_nonnull (planes[p]);
    }
    g_assert_true (matches_slot (&vs->surfaceList[0], planes, i));
    g_assert_cmpint (rds_buf_surface_unmap (vs, 0, -1), ==, 0);
    g_assert_null (bs->surfaceList[i].mappedAddr.addr[0]);

    g_assert_false (gst_memory_map (gst_buffer_peek_memory (view, 0), &info, GST_MAP_WRITE));

    gst_buffer_unref (view);
    g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 1);
  }

  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

/* Views map through the batch's staging mirror: no per-view allocation, the slot's part of the batch's mirror. A
 * mapping of the batch made before the view is created doesn't leak into the view. */
static void
test_view_device_shared_mirror (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstBuffer *batch = new_device_batch (alloc);
  RdsBufSurface *bs = rds_buffer_get_surface (batch);
  GstBuffer *v1, *v2, *vv;
  RdsBufSurface *vs1, *vs2, *vvs;

  g_assert_cmpint (rds_buf_surface_map (bs, -1, -1, RDS_BUF_MAP_READ), ==, 0);
  g_assert_nonnull (bs->surfaceList[1].mappedAddr.addr[0]);
  v1 = rds_buffer_new_surface_view (batch, 1);
  v2 = rds_buffer_new_surface_view (batch, 2);
  vv = rds_buffer_new_surface_view (v2, 0);
  vs1 = rds_buffer_get_surface (v1);
  vs2 = rds_buffer_get_surface (v2);
  vvs = rds_buffer_get_surface (vv);
  for (guint p = 0; p < RDS_BUF_SURFACE_MAX_PLANES; p++)
    g_assert_null (vs1->surfaceList[0].mappedAddr.addr[p]);

  g_assert_cmpint (rds_buf_surface_map (vs1, 0, -1, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_map (vs2, 0, -1, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_map (vvs, 0, -1, RDS_BUF_MAP_READ), ==, 0);
  for (guint p = 0; p < 2; p++) {
    g_assert_true (vs1->surfaceList[0].mappedAddr.addr[p] == bs->surfaceList[1].mappedAddr.addr[p]);
    g_assert_true (vs2->surfaceList[0].mappedAddr.addr[p] == bs->surfaceList[2].mappedAddr.addr[p]);
    g_assert_true (vvs->surfaceList[0].mappedAddr.addr[p] == bs->surfaceList[2].mappedAddr.addr[p]);
  }
  /* A sync through a view fills the slot's part of the batch's mirror. */
  memset (bs->surfaceList[1].mappedAddr.addr[0], 0, bs->surfaceList[1].planeParams.psize[0]);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (vs1, 0, 0), ==, 0);
  g_assert_cmpuint (((guint8 *) bs->surfaceList[1].mappedAddr.addr[0])[1], ==, pattern (1, 0, 1, 0));
  g_assert_cmpint (rds_buf_surface_unmap (vs1, 0, -1), ==, 0);
  g_assert_nonnull (bs->surfaceList[1].mappedAddr.addr[0]);
  g_assert_cmpint (rds_buf_surface_destroy (vs1), ==, -1);

  gst_buffer_unref (vv);
  gst_buffer_unref (v2);
  gst_buffer_unref (v1);
  g_assert_cmpint (rds_buf_surface_unmap (bs, -1, -1), ==, 0);
  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

/* ---- concurrent view mapping ---- */

#define CONC_SLOTS 4
#define CONC_THREADS 8
#define CONC_ROUNDS 12
#define CONC_ITERS 3

typedef struct
{
  GMutex lock;
  GCond cond;
  GstBuffer *batch;             /* this round's batch, set by the main thread */
  guint round;                  /* incremented to start a round */
  guint done;                   /* threads finished with the current round */
  gint failures;
} ConcState;

typedef struct
{
  ConcState *st;
  guint slot;
} ConcWorker;

/* Maps a view through the batch's shared mirror and checks both planes. Returns FALSE on any mismatch. */
static gboolean
map_and_check (GstBuffer * view, guint slot)
{
  RdsBufSurface *vs = rds_buffer_get_surface (view);
  guint8 *planes[RDS_BUF_SURFACE_MAX_PLANES];
  gboolean ok;

  if (vs == NULL || rds_buf_surface_map (vs, 0, -1, RDS_BUF_MAP_READ) != 0)
    return FALSE;
  ok = rds_buf_surface_sync_for_cpu (vs, 0, -1) == 0;
  for (guint p = 0; p < vs->surfaceList[0].planeParams.num_planes; p++)
    planes[p] = vs->surfaceList[0].mappedAddr.addr[p];
  ok = ok && matches_slot (&vs->surfaceList[0], planes, slot);
  return rds_buf_surface_unmap (vs, 0, -1) == 0 && ok;
}

static gpointer
conc_worker (gpointer data)
{
  ConcWorker *w = data;
  ConcState *st = w->st;

  for (guint round = 1; round <= CONC_ROUNDS; round++) {
    GstBuffer *batch;

    g_mutex_lock (&st->lock);
    while (st->round < round)
      g_cond_wait (&st->cond, &st->lock);
    batch = gst_buffer_ref (st->batch);
    g_mutex_unlock (&st->lock);

    /* Every thread's first map of a round races to allocate the fresh batch's mirror. */
    for (guint i = 0; i < CONC_ITERS; i++) {
      GstBuffer *view = rds_buffer_new_surface_view (batch, w->slot);
      GstBuffer *vv = view != NULL ? rds_buffer_new_surface_view (view, 0) : NULL;

      if (vv == NULL || !map_and_check (view, w->slot) || !map_and_check (vv, w->slot))
        g_atomic_int_inc (&st->failures);
      if (vv != NULL)
        gst_buffer_unref (vv);
      if (view != NULL)
        gst_buffer_unref (view);
    }
    gst_buffer_unref (batch);

    g_mutex_lock (&st->lock);
    st->done++;
    g_cond_broadcast (&st->cond);
    g_mutex_unlock (&st->lock);
  }
  return NULL;
}

/* Views of one device batch mapped from several threads at once: the batch's staging mirror is allocated exactly
 * once (a lost race must not leave a thread reading a mirror that is no longer the batch's) and every thread reads
 * its slot. Two threads share each slot: they sync the same unchanging bytes into the same mirror region and only
 * read them, which is benign; writing through a shared slot from two threads would be the documented race. */
static void
test_view_device_concurrent (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  ConcState st = { 0 };
  ConcWorker workers[CONC_THREADS];
  GThread *threads[CONC_THREADS];

  g_mutex_init (&st.lock);
  g_cond_init (&st.cond);
  for (guint t = 0; t < CONC_THREADS; t++) {
    workers[t] = (ConcWorker) {.st = &st,.slot = t % CONC_SLOTS };
    threads[t] = g_thread_new ("view-map", conc_worker, &workers[t]);
  }

  for (guint round = 1; round <= CONC_ROUNDS; round++) {
    GstBuffer *batch = new_device_batch_n (alloc, CONC_SLOTS);

    g_mutex_lock (&st.lock);
    gst_buffer_replace (&st.batch, batch);
    st.done = 0;
    st.round = round;
    g_cond_broadcast (&st.cond);
    while (st.done < CONC_THREADS)
      g_cond_wait (&st.cond, &st.lock);
    g_mutex_unlock (&st.lock);
    gst_buffer_unref (batch);
  }

  for (guint t = 0; t < CONC_THREADS; t++)
    g_thread_join (threads[t]);
  gst_buffer_replace (&st.batch, NULL);
  g_assert_cmpint (st.failures, ==, 0);
  g_cond_clear (&st.cond);
  g_mutex_clear (&st.lock);
  gst_object_unref (alloc);
}

static gsize
vm_rss_bytes (void)
{
  gchar *status = NULL, *line;
  gsize kib;

  g_assert_true (g_file_get_contents ("/proc/self/status", &status, NULL, NULL));
  line = strstr (status, "\nVmRSS:");
  g_assert_nonnull (line);
  kib = g_ascii_strtoull (line + strlen ("\nVmRSS:"), NULL, 10);
  g_free (status);
  return kib * 1024;
}

/* Creating, CPU-mapping and freeing many device views allocates no memory per view. Pinned staging memory counts in
 * VmRSS but is invisible to LeakSanitizer, so this is the check that catches a leaked mirror. */
static void
test_view_device_map_many (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstBuffer *batch = new_device_batch (alloc);
  gsize rss0 = 0, rss1;

  for (guint i = 0; i < 520; i++) {
    GstBuffer *view = rds_buffer_new_surface_view (batch, i % 3);
    RdsBufSurface *vs = rds_buffer_get_surface (view);

    g_assert_cmpint (rds_buf_surface_map (vs, 0, -1, RDS_BUF_MAP_READ), ==, 0);
    g_assert_cmpint (rds_buf_surface_sync_for_cpu (vs, 0, -1), ==, 0);
    g_assert_cmpint (rds_buf_surface_unmap (vs, 0, -1), ==, 0);
    gst_buffer_unref (view);
    if (i == 19)
      rss0 = vm_rss_bytes ();
  }
  rss1 = vm_rss_bytes ();
  g_test_message ("VmRSS over 500 mapped views: %+.2f MiB", ((gdouble) rss1 - (gdouble) rss0) / (1024.0 * 1024.0));
  g_assert_cmpint ((gssize) rss1 - (gssize) rss0, <, 8 * 1024 * 1024);

  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

/* make_writable + a WRITE map swaps an owned device copy of the slot into the buffer; the batch is untouched. */
static void
test_view_device_write_map (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstBuffer *batch = new_device_batch (alloc);
  RdsBufSurface *bs = rds_buffer_get_surface (batch);
  GstBuffer *view = rds_buffer_new_surface_view (batch, 2);
  RdsBufSurface *vs = rds_buffer_get_surface (view);
  RdsBufSurface *ws;
  GstMapInfo info;

  view = gst_buffer_make_writable (view);
  g_assert_true (gst_buffer_map (view, &info, GST_MAP_WRITE));
  ws = (RdsBufSurface *) info.data;
  g_assert_true (ws != vs);
  g_assert_false (rds_memory_is_view (gst_buffer_peek_memory (view, 0)));
  g_assert_cmpint (ws->memType, ==, RDS_BUF_MEM_DEFAULT);
  g_assert_cmpuint (ws->batchSize, ==, 1);
  g_assert_true (ws->surfaceList[0].dataPtr != bs->surfaceList[2].dataPtr);
  g_assert_true (surface_matches_slot (ws, 2));
  g_assert_cmpint (rds_buf_surface_memset (ws, 0, -1, 0), ==, 0);
  gst_buffer_unmap (view, &info);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 1);

  for (guint i = 0; i < 3; i++) {
    RdsBufSurface one = *bs;

    one.surfaceList = &bs->surfaceList[i];
    one.batchSize = one.numFilled = 1;
    one._priv = NULL;
    g_assert_true (surface_matches_slot (&one, i));
  }

  gst_buffer_unref (view);
  gst_buffer_unref (batch);
  gst_object_unref (alloc);
}

/* A view of a view refs the original batch; views (and their staging mirrors) outlive the batch's external ref. */
static void
test_view_device_outlives_batch (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstBuffer *batch = new_device_batch (alloc);
  GstBuffer *v = rds_buffer_new_surface_view (batch, 1);
  GstBuffer *vv = rds_buffer_new_surface_view (v, 0);
  RdsBufSurface *vs;

  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (batch), ==, 3);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (v), ==, 1);
  gst_buffer_unref (batch);
  gst_buffer_unref (v);

  vs = rds_buffer_get_surface (vv);
  g_assert_true (surface_matches_slot (vs, 1));
  /* Leave the staging mirror mapped: freeing the view must release it. */
  g_assert_cmpint (rds_buf_surface_map (vs, 0, -1, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (vs, 0, -1), ==, 0);
  gst_buffer_unref (vv);
  gst_object_unref (alloc);
}

/* rds_buf_surf_transform takes a view as its source; a same-size nearest transform reproduces the slot. */
static void
test_view_transform_source (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  GstBuffer *batch = new_device_batch (alloc);
  GstBuffer *view = rds_buffer_new_surface_view (batch, 1);
  RdsBufSurfaceCreateParams p = nv12_640;
  RdsBufSurface *dst = NULL;
  RdsBufSurfTransformParams tp = {
    .transform_flag = RDS_BUFSURF_TRANSFORM_FILTER, .transform_filter = RdsBufSurfTransformInter_Nearest,
  };

  p.memType = RDS_BUF_MEM_DEFAULT;
  g_assert_cmpint (rds_buf_surface_create (&dst, 1, &p), ==, 0);
  g_assert_cmpint (rds_buf_surf_transform (rds_buffer_get_surface (view), dst, &tp), ==,
      RdsBufSurfTransformError_Success);
  g_assert_cmpuint (dst->numFilled, ==, 1);
  g_assert_true (surface_matches_slot (dst, 1));
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
  g_test_add_func ("/memory-hip/default-mem-type", test_default_mem_type);
  g_test_add_func ("/memory-hip/copy-is-deep", test_copy_is_deep);
  g_test_add_func ("/memory-hip/view-device", test_view_device);
  g_test_add_func ("/memory-hip/view-device-write-map", test_view_device_write_map);
  g_test_add_func ("/memory-hip/view-device-shared-mirror", test_view_device_shared_mirror);
  g_test_add_func ("/memory-hip/view-device-map-many", test_view_device_map_many);
  g_test_add_func ("/memory-hip/view-device-concurrent", test_view_device_concurrent);
  g_test_add_func ("/memory-hip/view-device-outlives-batch", test_view_device_outlives_batch);
  g_test_add_func ("/memory-hip/view-transform-source", test_view_transform_source);
  return g_test_run ();
}
