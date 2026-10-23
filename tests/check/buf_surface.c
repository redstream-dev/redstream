/* RdsBufSurface layout and the system-memory path; needs no GPU.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <rds/rds-buf-surface.h>

#include <string.h>

static RdsBufSurface *
create_sys (RdsBufSurfaceColorFormat fmt, guint32 w, guint32 h, guint32 batch)
{
  RdsBufSurfaceCreateParams p = {
    .width = w, .height = h, .colorFormat = fmt,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = RDS_BUF_MEM_SYSTEM,
  };
  RdsBufSurface *s = NULL;

  g_assert_cmpint (rds_buf_surface_create (&s, batch, &p), ==, 0);
  g_assert_nonnull (s);
  return s;
}

static void
test_nv12_layout (void)
{
  RdsBufSurface *s = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 1366, 768, 2);
  RdsBufSurfaceParams *p = &s->surfaceList[0];
  RdsBufSurfacePlaneParams *pp = &p->planeParams;

  g_assert_cmpuint (s->batchSize, ==, 2);
  g_assert_cmpuint (s->numFilled, ==, 0);
  g_assert_true (s->isContiguous);
  g_assert_cmpint (s->memType, ==, RDS_BUF_MEM_SYSTEM);
  g_assert_cmpuint (p->width, ==, 1366);
  g_assert_cmpuint (p->height, ==, 768);
  g_assert_cmpuint (pp->num_planes, ==, 2);
  g_assert_cmpuint (pp->pitch[0], ==, 1536);
  g_assert_cmpuint (p->pitch, ==, 1536);
  g_assert_cmpuint (pp->width[1], ==, 683);
  g_assert_cmpuint (pp->height[1], ==, 384);
  g_assert_cmpuint (pp->bytesPerPix[0], ==, 1);
  g_assert_cmpuint (pp->bytesPerPix[1], ==, 2);
  g_assert_cmpuint (pp->pitch[1], ==, 1536);
  g_assert_cmpuint (pp->offset[0], ==, 0);
  g_assert_cmpuint (pp->offset[1], ==, 1536 * 768);
  g_assert_cmpuint (pp->psize[1], ==, 1536 * 384);
  g_assert_cmpuint (p->dataSize, ==, 1536 * 768 + 1536 * 384);
  g_assert_cmpuint ((guintptr) p->dataPtr % RDS_BUF_SURFACE_PITCH_ALIGN, ==, 0);
  g_assert_cmpuint ((guint8 *) s->surfaceList[1].dataPtr - (guint8 *) p->dataPtr, ==,
      GST_ROUND_UP_N (p->dataSize, RDS_BUF_SURFACE_PITCH_ALIGN));
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

static void
test_odd_sizes (void)
{
  RdsBufSurface *s = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 1367, 769, 1);
  RdsBufSurfacePlaneParams *pp = &s->surfaceList[0].planeParams;

  g_assert_cmpuint (pp->width[1], ==, 684);     /* ceil (1367 / 2) */
  g_assert_cmpuint (pp->height[1], ==, 385);    /* ceil (769 / 2) */
  g_assert_cmpuint (pp->pitch[1], ==, 1536);    /* 684 * 2 = 1368 -> 1536 */
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);

  s = create_sys (RDS_BUF_COLOR_FORMAT_YUV420, 5, 3, 1);
  pp = &s->surfaceList[0].planeParams;
  g_assert_cmpuint (pp->num_planes, ==, 3);
  g_assert_cmpuint (pp->width[2], ==, 3);
  g_assert_cmpuint (pp->height[2], ==, 2);
  g_assert_cmpuint (pp->pitch[2], ==, 256);
  g_assert_cmpuint (pp->offset[2], ==, 256 * 3 + 256 * 2);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

static void
test_packed_layouts (void)
{
  RdsBufSurface *s = create_sys (RDS_BUF_COLOR_FORMAT_RGBA, 640, 640, 1);

  g_assert_cmpuint (s->surfaceList[0].planeParams.num_planes, ==, 1);
  g_assert_cmpuint (s->surfaceList[0].pitch, ==, 2560);
  g_assert_cmpuint (s->surfaceList[0].planeParams.bytesPerPix[0], ==, 4);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);

  s = create_sys (RDS_BUF_COLOR_FORMAT_RGB, 641, 2, 1);
  g_assert_cmpuint (s->surfaceList[0].pitch, ==, 2048);       /* 1923 -> 2048 */
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);

  s = create_sys (RDS_BUF_COLOR_FORMAT_GRAY8, 300, 2, 1);
  g_assert_cmpuint (s->surfaceList[0].pitch, ==, 512);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

static void
test_rejects (void)
{
  RdsBufSurfaceCreateParams p = {
    .width = 64, .height = 64, .colorFormat = RDS_BUF_COLOR_FORMAT_NV12,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = RDS_BUF_MEM_SYSTEM,
  };
  RdsBufSurface *s = (RdsBufSurface *) 0x1;
  RdsBufSurfaceCreateParams bad;

#define EXPECT_REJECT(batch, params) G_STMT_START { \
    s = (RdsBufSurface *) 0x1; \
    g_assert_cmpint (rds_buf_surface_create (&s, (batch), (params)), ==, -1); \
    g_assert_null (s); \
  } G_STMT_END

  bad = p; bad.colorFormat = RDS_BUF_COLOR_FORMAT_INVALID;
  EXPECT_REJECT (1, &bad);
  bad = p; bad.width = 0;
  EXPECT_REJECT (1, &bad);
  bad = p; bad.height = RDS_BUF_SURFACE_MAX_DIM + 1;
  EXPECT_REJECT (1, &bad);
  bad = p; bad.layout = RDS_BUF_LAYOUT_BLOCK_LINEAR;
  EXPECT_REJECT (1, &bad);
  bad = p; bad.memType = RDS_BUF_MEM_SURFACE_ARRAY;
  EXPECT_REJECT (1, &bad);
  bad = p; bad.memType = RDS_BUF_MEM_HANDLE;
  EXPECT_REJECT (1, &bad);
  EXPECT_REJECT (0, &p);

  /* Batches far too large for memory fail cleanly instead of aborting. Surfaces are 2^32 bytes (size override).
   * 2^31 of them (2^63 bytes) exceed G_MAXSSIZE and are refused up front; 2^30 of them (2^62 bytes) pass that
   * check but exceed any 64-bit address space, so the allocation itself fails without touching memory. */
  bad = p; bad.colorFormat = RDS_BUF_COLOR_FORMAT_GRAY8; bad.size = G_MAXUINT32;
  EXPECT_REJECT (1u << 31, &bad);
  EXPECT_REJECT (1u << 30, &bad);
#undef EXPECT_REJECT

  /* A hand-built surface was not allocated by us, so destroy refuses it. */
  RdsBufSurface hand = { .batchSize = 1 };
  g_assert_cmpint (rds_buf_surface_destroy (&hand), ==, -1);
}

static void
test_size_override (void)
{
  RdsBufSurfaceCreateParams p = {
    .width = 16, .height = 16, .size = 100000, .colorFormat = RDS_BUF_COLOR_FORMAT_GRAY8,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = RDS_BUF_MEM_SYSTEM,
  };
  RdsBufSurface *s = NULL;

  g_assert_cmpint (rds_buf_surface_create (&s, 1, &p), ==, 0);
  g_assert_cmpuint (s->surfaceList[0].dataSize, ==, 100000);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

static void
test_system_map_memset (void)
{
  RdsBufSurface *s = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 64, 32, 2);
  RdsBufSurfaceParams *p1 = &s->surfaceList[1];
  guint8 *uv;

  g_assert_cmpint (rds_buf_surface_map (s, -1, -1, RDS_BUF_MAP_READ_WRITE), ==, 0);
  g_assert_true (p1->mappedAddr.addr[0] == p1->dataPtr);
  g_assert_true (p1->mappedAddr.addr[1] == (guint8 *) p1->dataPtr + p1->planeParams.offset[1]);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, 1, -1), ==, 0);       /* no-op on host memory */

  g_assert_cmpint (rds_buf_surface_memset (s, 1, 1, 128), ==, 0);
  uv = p1->mappedAddr.addr[1];
  for (guint i = 0; i < p1->planeParams.psize[1]; i++)
    g_assert_cmpuint (uv[i], ==, 128);
  g_assert_cmpuint (((guint8 *) p1->mappedAddr.addr[0])[0], ==, 0);       /* plane 0 untouched */

  g_assert_cmpint (rds_buf_surface_sync_for_device (s, 1, -1), ==, 0);
  g_assert_cmpint (rds_buf_surface_unmap (s, -1, -1), ==, 0);
  g_assert_null (p1->mappedAddr.addr[0]);

  g_assert_cmpint (rds_buf_surface_map (s, 2, 0, RDS_BUF_MAP_READ), ==, -1);      /* bad index */
  g_assert_cmpint (rds_buf_surface_map (s, 0, 2, RDS_BUF_MAP_READ), ==, -1);      /* bad plane */
  g_assert_cmpint (rds_buf_surface_map (s, 0, 0, (RdsBufSurfaceMemMapFlags) 3), ==, -1);  /* bad flags */
  g_assert_null (s->surfaceList[0].mappedAddr.addr[0]);
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

/* Fills every row's pixel bytes of every plane with a pattern derived from (surface, plane, row, column). */
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

/* Asserts that surface @idx of @dst holds the same pixel bytes as surface @idx of @src, plane by plane. */
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

static void
test_system_copy (void)
{
  RdsBufSurface *src = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 100, 50, 2);
  RdsBufSurface *dst = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 100, 50, 2);
  RdsBufSurfaceParams *d0 = &dst->surfaceList[0];

  fill_pattern (src, 0);
  fill_pattern (src, 1);
  src->numFilled = 1;
  g_assert_cmpint (rds_buf_surface_memset (dst, -1, -1, 0xaa), ==, 0);

  g_assert_cmpint (rds_buf_surface_copy (src, dst), ==, 0);
  g_assert_cmpuint (dst->numFilled, ==, 1);
  for (guint pl = 0; pl < 2; pl++) {
    guint row_bytes = d0->planeParams.width[pl] * d0->planeParams.bytesPerPix[pl];
    for (guint y = 0; y < d0->planeParams.height[pl]; y++) {
      guint8 *drow = (guint8 *) d0->dataPtr + d0->planeParams.offset[pl] + y * d0->planeParams.pitch[pl];
      guint8 *srow = (guint8 *) src->surfaceList[0].dataPtr + d0->planeParams.offset[pl]
          + y * d0->planeParams.pitch[pl];
      g_assert_cmpmem (drow, row_bytes, srow, row_bytes);
      g_assert_cmpuint (drow[row_bytes], ==, 0xaa);                       /* padding untouched */
    }
  }
  /* Surface 1 was beyond numFilled: not copied. */
  g_assert_cmpuint (((guint8 *) dst->surfaceList[1].dataPtr)[0], ==, 0xaa);

  /* Mismatched geometry is refused. */
  RdsBufSurface *other = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 102, 50, 2);
  g_assert_cmpint (rds_buf_surface_copy (src, other), ==, -1);
  g_assert_cmpint (rds_buf_surface_destroy (other), ==, 0);

  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
}

static void
test_system_memset_all (void)
{
  RdsBufSurface *s = create_sys (RDS_BUF_COLOR_FORMAT_YUV420, 33, 17, 3);

  g_assert_cmpint (rds_buf_surface_memset (s, -1, -1, 0x5c), ==, 0);
  for (guint i = 0; i < s->batchSize; i++) {
    RdsBufSurfaceParams *p = &s->surfaceList[i];
    guint8 *data = p->dataPtr;
    guint32 end = p->planeParams.offset[2] + p->planeParams.psize[2];

    for (guint32 b = 0; b < end; b++)
      g_assert_cmpuint (data[b], ==, 0x5c);
  }
  g_assert_cmpint (rds_buf_surface_memset (s, 3, -1, 0), ==, -1);         /* bad index */
  g_assert_cmpint (rds_buf_surface_destroy (s), ==, 0);
}

static void
test_system_copy_yuv420 (void)
{
  RdsBufSurface *src = create_sys (RDS_BUF_COLOR_FORMAT_YUV420, 37, 21, 2);
  RdsBufSurface *dst = create_sys (RDS_BUF_COLOR_FORMAT_YUV420, 37, 21, 2);

  fill_pattern (src, 0);
  fill_pattern (src, 1);
  src->numFilled = 2;
  g_assert_cmpint (rds_buf_surface_copy (src, dst), ==, 0);
  g_assert_cmpuint (dst->numFilled, ==, 2);
  assert_rows_equal (src, dst, 0);
  assert_rows_equal (src, dst, 1);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
}

static void
test_copy_rejects (void)
{
  RdsBufSurface *src = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 64, 32, 2);
  RdsBufSurface *small = create_sys (RDS_BUF_COLOR_FORMAT_NV12, 64, 32, 1);
  RdsBufSurfaceParams hand_params;
  RdsBufSurface hand = { .batchSize = 1, .memType = RDS_BUF_MEM_SYSTEM, .surfaceList = &hand_params };
  gpointer hand_data;

  fill_pattern (src, 0);
  fill_pattern (src, 1);

  /* More filled surfaces than the destination holds. */
  src->numFilled = 2;
  g_assert_cmpint (rds_buf_surface_copy (src, small), ==, -1);
  g_assert_cmpuint (small->numFilled, ==, 0);

  /* Copying a surface onto itself is a no-op. */
  g_assert_cmpint (rds_buf_surface_copy (src, src), ==, 0);
  g_assert_cmpuint (src->numFilled, ==, 2);

  /* A hand-built destination whose plane 1 pitch is smaller than a row of pixels is refused, untouched. */
  src->numFilled = 1;
  hand_params = src->surfaceList[0];
  hand_data = g_malloc0 (hand_params.dataSize);
  hand_params.dataPtr = hand_data;
  hand_params.mappedAddr = (RdsBufSurfaceMappedAddr) { 0 };
  hand_params.planeParams.pitch[1] = 64;                                   /* row is 32 * 2 = 64: still fits */
  g_assert_cmpint (rds_buf_surface_copy (src, &hand), ==, 0);
  hand_params.planeParams.pitch[1] = 63;
  memset (hand_data, 0, hand_params.dataSize);
  g_assert_cmpint (rds_buf_surface_copy (src, &hand), ==, -1);
  g_assert_cmpuint (((guint8 *) hand_data)[0], ==, 0);
  g_free (hand_data);

  g_assert_cmpint (rds_buf_surface_destroy (small), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/buf-surface/nv12-layout", test_nv12_layout);
  g_test_add_func ("/buf-surface/odd-sizes", test_odd_sizes);
  g_test_add_func ("/buf-surface/packed-layouts", test_packed_layouts);
  g_test_add_func ("/buf-surface/rejects", test_rejects);
  g_test_add_func ("/buf-surface/size-override", test_size_override);
  g_test_add_func ("/buf-surface/system-map-memset", test_system_map_memset);
  g_test_add_func ("/buf-surface/system-copy", test_system_copy);
  g_test_add_func ("/buf-surface/system-memset-all", test_system_memset_all);
  g_test_add_func ("/buf-surface/system-copy-yuv420", test_system_copy_yuv420);
  g_test_add_func ("/buf-surface/copy-rejects", test_copy_rejects);
  return g_test_run ();
}
