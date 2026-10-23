/* memory:RDS on HIP device memory; skips without a ROCm device.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include <rds/rds-memory.h>

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

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/memory-hip/default-mem-type", test_default_mem_type);
  g_test_add_func ("/memory-hip/copy-is-deep", test_copy_is_deep);
  return g_test_run ();
}
