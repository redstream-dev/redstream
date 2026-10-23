/* RdsBufSurface on HIP memory types; skips without a ROCm device.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include <rds/rds-buf-surface.h>

#include <string.h>

static RdsBufSurface *
create_fmt (RdsBufSurfaceMemType mt, RdsBufSurfaceColorFormat fmt, guint32 w, guint32 h, guint32 batch)
{
  RdsBufSurfaceCreateParams p = {
    .width = w, .height = h, .colorFormat = fmt,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = mt,
  };
  RdsBufSurface *s = NULL;

  g_assert_cmpint (rds_buf_surface_create (&s, batch, &p), ==, 0);
  g_assert_nonnull (s);
  return s;
}

static RdsBufSurface *
create (RdsBufSurfaceMemType mt, guint32 w, guint32 h, guint32 batch)
{
  return create_fmt (mt, RDS_BUF_COLOR_FORMAT_NV12, w, h, batch);
}

static gboolean
is_host_accessible (RdsBufSurfaceMemType mt)
{
  return mt == RDS_BUF_MEM_HIP_PINNED || mt == RDS_BUF_MEM_HIP_UNIFIED;
}

static int
current_device (void)
{
  int dev = -1;

  g_assert_cmpint (hipGetDevice (&dev), ==, hipSuccess);
  return dev;
}

/* Fills every row's pixel bytes of every plane with a pattern derived from (surface, plane, row, column).
 * The surface must be host-accessible. */
static void
fill_pattern (RdsBufSurface * s, guint idx)
{
  RdsBufSurfaceParams *p = &s->surfaceList[idx];

  for (guint pl = 0; pl < p->planeParams.num_planes; pl++) {
    guint row_bytes = p->planeParams.width[pl] * p->planeParams.bytesPerPix[pl];
    for (guint y = 0; y < p->planeParams.height[pl]; y++) {
      guint8 *row = (guint8 *) p->dataPtr + p->planeParams.offset[pl] + y * p->planeParams.pitch[pl];
      for (guint x = 0; x < row_bytes; x++)
        row[x] = (guint8) (idx * 31 + pl * 17 + y * 7 + x);
    }
  }
}

/* Asserts that surface @idx of @dst holds the same pixel bytes as surface @idx of @src. Both host-accessible. */
static void
assert_rows_equal (RdsBufSurface * src, RdsBufSurface * dst, guint idx)
{
  RdsBufSurfaceParams *sp = &src->surfaceList[idx];
  RdsBufSurfaceParams *dp = &dst->surfaceList[idx];

  for (guint pl = 0; pl < sp->planeParams.num_planes; pl++) {
    guint row_bytes = sp->planeParams.width[pl] * sp->planeParams.bytesPerPix[pl];
    for (guint y = 0; y < sp->planeParams.height[pl]; y++) {
      guint8 *srow = (guint8 *) sp->dataPtr + sp->planeParams.offset[pl] + y * sp->planeParams.pitch[pl];
      guint8 *drow = (guint8 *) dp->dataPtr + dp->planeParams.offset[pl] + y * dp->planeParams.pitch[pl];
      g_assert_cmpmem (drow, row_bytes, srow, row_bytes);
    }
  }
}

/* Asserts that every pixel byte of one plane of a host-accessible surface equals @value. */
static void
assert_plane_value (RdsBufSurface * s, guint idx, guint pl, guint8 value)
{
  RdsBufSurfaceParams *p = &s->surfaceList[idx];
  guint row_bytes = p->planeParams.width[pl] * p->planeParams.bytesPerPix[pl];

  for (guint y = 0; y < p->planeParams.height[pl]; y++) {
    guint8 *row = (guint8 *) p->dataPtr + p->planeParams.offset[pl] + y * p->planeParams.pitch[pl];
    for (guint x = 0; x < row_bytes; x++)
      g_assert_cmpuint (row[x], ==, value);
  }
}

static void
test_round_trip (gconstpointer data)
{
  RdsBufSurfaceMemType mt = GPOINTER_TO_INT (data);
  RdsBufSurface *src, *mid, *back;

  RDS_TEST_REQUIRE_GPU ();
  src = create (RDS_BUF_MEM_SYSTEM, 1366, 768, 2);
  mid = create (mt, 1366, 768, 2);
  back = create (RDS_BUF_MEM_SYSTEM, 1366, 768, 2);
  g_assert_cmpint (mid->memType, ==, mt);

  fill_pattern (src, 0);
  fill_pattern (src, 1);
  src->numFilled = 2;
  /* System to device, default or unified memory goes through hipMemcpy2D (hipMemcpyDefault) with pageable
   * memory on one side; system to pinned is a plain memcpy. */
  g_assert_cmpint (rds_buf_surface_copy (src, mid), ==, 0);
  g_assert_cmpuint (mid->numFilled, ==, 2);
  g_assert_cmpint (rds_buf_surface_copy (mid, back), ==, 0);
  g_assert_cmpuint (back->numFilled, ==, 2);
  assert_rows_equal (src, back, 0);
  assert_rows_equal (src, back, 1);

  g_assert_cmpint (rds_buf_surface_destroy (back), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (mid), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

static void
test_map_sync (gconstpointer data)
{
  RdsBufSurfaceMemType mt = GPOINTER_TO_INT (data);
  RdsBufSurface *s, *out;
  RdsBufSurfaceParams *p;

  RDS_TEST_REQUIRE_GPU ();
  s = create (mt, 1366, 768, 1);
  out = create (RDS_BUF_MEM_SYSTEM, 1366, 768, 1);
  p = &s->surfaceList[0];

  g_assert_cmpint (rds_buf_surface_memset (s, 0, -1, 7), ==, 0);
  g_assert_cmpint (rds_buf_surface_map (s, 0, -1, RDS_BUF_MAP_READ_WRITE), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 0, -1), ==, 0);
  for (guint pl = 0; pl < 2; pl++) {
    guint8 *m = p->mappedAddr.addr[pl];

    g_assert_nonnull (m);
    if (is_host_accessible (mt))
      g_assert_true (m == (guint8 *) p->dataPtr + p->planeParams.offset[pl]);
    for (guint32 b = 0; b < p->planeParams.psize[pl]; b++)
      g_assert_cmpuint (m[b], ==, 7);
  }

  memset (p->mappedAddr.addr[1], 9, p->planeParams.psize[1]);
  g_assert_cmpint (rds_buf_surface_sync_for_device (s, 0, 1), ==, 0);
  g_assert_cmpint (rds_buf_surface_unmap (s, 0, -1), ==, 0);
  g_assert_null (p->mappedAddr.addr[0]);
  g_assert_null (p->mappedAddr.addr[1]);

  s->numFilled = 1;
  g_assert_cmpint (rds_buf_surface_copy (s, out), ==, 0);
  assert_plane_value (out, 0, 0, 7);
  assert_plane_value (out, 0, 1, 9);

  g_assert_cmpint (rds_buf_surface_destroy (out), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

/* Contract and regression test, not a discriminating one: a copy between HIP surfaces has finished when it
 * returns, so the result is read straight away, through a second copy to system memory or directly from
 * host-accessible memory, with no sync in between. On ROCm, hipMemcpy2D and hipMemset already block the host,
 * so this passes even without the library's own stream syncs; it guards against a future async path. */
static void
test_copy_ready (gconstpointer data)
{
  RdsBufSurfaceMemType mt = GPOINTER_TO_INT (data);
  const guint32 w = 1920, h = 1080;
  RdsBufSurface *src, *a, *dev, *pinned, *unified, *back;

  RDS_TEST_REQUIRE_GPU ();
  src = create (RDS_BUF_MEM_SYSTEM, w, h, 2);
  a = create (mt, w, h, 2);
  dev = create (RDS_BUF_MEM_HIP_DEVICE, w, h, 2);
  pinned = create (RDS_BUF_MEM_HIP_PINNED, w, h, 2);
  unified = create (RDS_BUF_MEM_HIP_UNIFIED, w, h, 2);
  back = create (RDS_BUF_MEM_SYSTEM, w, h, 2);

  fill_pattern (src, 0);
  fill_pattern (src, 1);
  src->numFilled = 2;
  g_assert_cmpint (rds_buf_surface_copy (src, a), ==, 0);
  g_assert_cmpint (rds_buf_surface_copy (a, dev), ==, 0);         /* HIP -> device */
  g_assert_cmpint (rds_buf_surface_copy (dev, back), ==, 0);
  assert_rows_equal (src, back, 0);
  assert_rows_equal (src, back, 1);

  g_assert_cmpint (rds_buf_surface_copy (dev, pinned), ==, 0);    /* device -> pinned, read by the CPU */
  assert_rows_equal (src, pinned, 0);
  assert_rows_equal (src, pinned, 1);
  g_assert_cmpint (rds_buf_surface_copy (dev, unified), ==, 0);   /* device -> unified, read by the CPU */
  assert_rows_equal (src, unified, 0);
  assert_rows_equal (src, unified, 1);

  /* memset is synchronous too: host-accessible memory reads the new value at once. */
  g_assert_cmpint (rds_buf_surface_memset (unified, 1, 0, 0x3c), ==, 0);
  assert_plane_value (unified, 1, 0, 0x3c);

  g_assert_cmpint (rds_buf_surface_destroy (back), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (unified), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (pinned), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (dev), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (a), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

/* HIP entry points select surf->gpuId and restore the caller's device. A surface whose gpuId names no device
 * fails at create, and at the staging allocation in map, while the current device stays put. */
static void
test_device_select (gconstpointer data)
{
  RdsBufSurfaceMemType mt = GPOINTER_TO_INT (data);
  RdsBufSurfaceCreateParams p = {
    .width = 64, .height = 64, .colorFormat = RDS_BUF_COLOR_FORMAT_GRAY8,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = mt,
  };
  RdsBufSurface *s = (RdsBufSurface *) 0x1;
  int n = 0, before;

  RDS_TEST_REQUIRE_GPU ();
  g_assert_cmpint (hipGetDeviceCount (&n), ==, hipSuccess);
  before = current_device ();

  p.gpuId = (guint32) n;                                           /* one past the last device */
  g_assert_cmpint (rds_buf_surface_create (&s, 1, &p), ==, -1);
  g_assert_null (s);
  g_assert_cmpint (current_device (), ==, before);

  p.gpuId = (guint32) before;
  g_assert_cmpint (rds_buf_surface_create (&s, 1, &p), ==, 0);
  g_assert_cmpint (current_device (), ==, before);

  if (!is_host_accessible (mt)) {
    /* Device memory maps through a staging mirror allocated on the surface's GPU. */
    s->gpuId = (guint32) n;
    g_assert_cmpint (rds_buf_surface_map (s, 0, 0, RDS_BUF_MAP_READ), ==, -1);
    g_assert_null (s->surfaceList[0].mappedAddr.addr[0]);
    g_assert_cmpint (rds_buf_surface_memset (s, 0, 0, 1), ==, -1);
    g_assert_cmpint (current_device (), ==, before);
    s->gpuId = (guint32) before;
  }

  g_assert_cmpint (rds_buf_surface_map (s, 0, 0, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 0, 0), ==, 0);
  g_assert_cmpint (rds_buf_surface_unmap (s, 0, 0), ==, 0);
  g_assert_cmpint (current_device (), ==, before);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
  g_assert_cmpint (current_device (), ==, before);

  if (n < 2) {
    g_test_message ("single GPU: restoring a different current device is not exercised");
    return;
  }
  /* Operate on a gpuId-0 surface while device 1 is current: every call must leave device 1 current. */
  {
    RdsBufSurface *sys = NULL;
    RdsBufSurfaceCreateParams sp = p;

    p.gpuId = 0;
    sp.memType = RDS_BUF_MEM_SYSTEM;
    g_assert_cmpint (hipSetDevice (1), ==, hipSuccess);
    g_assert_cmpint (rds_buf_surface_create (&s, 1, &p), ==, 0);
    g_assert_cmpint (rds_buf_surface_create (&sys, 1, &sp), ==, 0);
    g_assert_cmpint (current_device (), ==, 1);
    g_assert_cmpint (rds_buf_surface_memset (s, 0, -1, 5), ==, 0);
    g_assert_cmpint (rds_buf_surface_map (s, 0, -1, RDS_BUF_MAP_READ), ==, 0);
    g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 0, -1), ==, 0);
    g_assert_cmpint (rds_buf_surface_unmap (s, 0, -1), ==, 0);
    s->numFilled = 1;
    g_assert_cmpint (rds_buf_surface_copy (s, sys), ==, 0);
    g_assert_cmpuint (((guint8 *) sys->surfaceList[0].dataPtr)[0], ==, 5);
    g_assert_cmpint (current_device (), ==, 1);
    g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
    g_assert_cmpint (rds_buf_surface_destroy (sys), ==, 0);
    g_assert_cmpint (current_device (), ==, 1);
    g_assert_cmpint (hipSetDevice (before), ==, hipSuccess);
  }
}

static void
test_sync_unmapped (void)
{
  RdsBufSurface *s;

  RDS_TEST_REQUIRE_GPU ();
  s = create (RDS_BUF_MEM_DEFAULT, 64, 32, 2);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 0, -1), ==, -1);
  g_assert_cmpint (rds_buf_surface_sync_for_device (s, 0, -1), ==, -1);
  /* Mapping one plane does not make the other syncable. */
  g_assert_cmpint (rds_buf_surface_map (s, 1, 0, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 1, 0), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 1, 1), ==, -1);
  g_assert_cmpint (rds_buf_surface_unmap (s, 1, 0), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 1, 0), ==, -1);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

/* Resident set size of this process in KiB. Pinned hipHostMalloc memory is counted here, not in VmPin or VmLck
 * (checked with ROCm 10.1: a 64 MiB hipHostMalloc raises VmRSS by 64 MiB and leaves VmPin/VmLck at 0). */
static gsize
vm_rss_kib (void)
{
  gchar *status = NULL;
  const gchar *line;
  gsize kib = 0;

  g_assert_true (g_file_get_contents ("/proc/self/status", &status, NULL, NULL));
  line = strstr (status, "\nVmRSS:");
  g_assert_nonnull (line);
  kib = g_ascii_strtoull (line + strlen ("\nVmRSS:"), NULL, 10);
  g_free (status);
  g_assert_cmpuint (kib, >, 0);
  return kib;
}

static void
test_no_leak (void)
{
  size_t free_before = 0, free_after = 0, total = 0;
  gsize rss_before, rss_after;

  RDS_TEST_REQUIRE_GPU ();
  /* One warm-up cycle so lazy HIP runtime allocations don't count. */
  {
    RdsBufSurface *s = create (RDS_BUF_MEM_DEFAULT, 1920, 1080, 4);

    g_assert_cmpint (rds_buf_surface_map (s, -1, -1, RDS_BUF_MAP_READ), ==, 0);
    g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
  }
  /* Assumes no other KFD user allocates during the test (the gpu suite does not run in parallel). Device memory
   * shows in hipMemGetInfo; the pinned staging mirrors (12 MiB each, 50 maps below) show only in VmRSS. */
  g_assert_cmpint (hipMemGetInfo (&free_before, &total), ==, hipSuccess);
  rss_before = vm_rss_kib ();
  for (guint i = 0; i < 500; i++) {
    RdsBufSurface *s = create (RDS_BUF_MEM_DEFAULT, 1920, 1080, 4);

    if (i % 10 == 0)
      g_assert_cmpint (rds_buf_surface_map (s, -1, -1, RDS_BUF_MAP_READ), ==, 0);
    g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
  }
  g_assert_cmpint (hipMemGetInfo (&free_after, &total), ==, hipSuccess);
  rss_after = vm_rss_kib ();
  g_assert_cmpuint (free_after + 4 * 1024 * 1024, >=, free_before);
  g_assert_cmpuint (rss_after, <=, rss_before + 64 * 1024);
}

static const struct
{
  const gchar *name;
  RdsBufSurfaceMemType mt;
} mem_types[] = {
  {"default", RDS_BUF_MEM_DEFAULT},
  {"device", RDS_BUF_MEM_HIP_DEVICE},
  {"pinned", RDS_BUF_MEM_HIP_PINNED},
  {"unified", RDS_BUF_MEM_HIP_UNIFIED},
};

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  for (gsize i = 0; i < G_N_ELEMENTS (mem_types); i++) {
    gconstpointer mt = GINT_TO_POINTER (mem_types[i].mt);
    gchar *path;

    path = g_strdup_printf ("/buf-surface-hip/%s/round-trip", mem_types[i].name);
    g_test_add_data_func (path, mt, test_round_trip);
    g_free (path);
    path = g_strdup_printf ("/buf-surface-hip/%s/map-sync", mem_types[i].name);
    g_test_add_data_func (path, mt, test_map_sync);
    g_free (path);
    path = g_strdup_printf ("/buf-surface-hip/%s/copy-ready", mem_types[i].name);
    g_test_add_data_func (path, mt, test_copy_ready);
    g_free (path);
    path = g_strdup_printf ("/buf-surface-hip/%s/device-select", mem_types[i].name);
    g_test_add_data_func (path, mt, test_device_select);
    g_free (path);
  }
  g_test_add_func ("/buf-surface-hip/device/sync-unmapped", test_sync_unmapped);
  g_test_add_func ("/buf-surface-hip/device/no-leak", test_no_leak);
  return g_test_run ();
}
