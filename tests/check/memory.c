/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds-memory.h>

static const RdsBufSurfaceCreateParams nv12_640 = {
  .width = 640, .height = 480, .colorFormat = RDS_BUF_COLOR_FORMAT_NV12, .layout = RDS_BUF_LAYOUT_PITCH,
};

static void
test_map_returns_surface (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstMemory *mem = rds_allocator_alloc_surface (alloc, 1, &nv12_640);
  GstMapInfo info;

  g_assert_nonnull (mem);
  g_assert_true (rds_is_rds_memory (mem));
  g_assert_true (gst_memory_is_type (mem, RDS_MEMORY_TYPE));
  /* Shareable as a whole (buffer copies ref it), but no sub-region shares or partial copies. */
  g_assert_false (GST_MEMORY_FLAG_IS_SET (mem, GST_MEMORY_FLAG_NO_SHARE));
  g_assert_null (gst_memory_share (mem, 0, -1));
  g_assert_null (gst_memory_copy (mem, 1, -1));
  g_assert_true (gst_memory_map (mem, &info, GST_MAP_READ));
  g_assert_true (info.data == (guint8 *) rds_memory_get_surface (mem));
  g_assert_cmpuint (info.size, ==, sizeof (RdsBufSurface));
  g_assert_cmpint (((RdsBufSurface *) info.data)->memType, ==, RDS_BUF_MEM_SYSTEM);
  g_assert_cmpuint (((RdsBufSurface *) info.data)->surfaceList[0].width, ==, 640);
  gst_memory_unmap (mem, &info);

  g_assert_null (gst_allocator_alloc (alloc, 100, NULL));
  gst_memory_unref (mem);
  gst_object_unref (alloc);
}

static void
test_buffer_helpers (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *buf = gst_buffer_new ();

  g_assert_null (rds_buffer_get_surface (buf));
  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (alloc, 1, &nv12_640));
  g_assert_nonnull (rds_buffer_get_surface (buf));
  g_assert_true (rds_buffer_get_surface (buf) == rds_memory_get_surface (gst_buffer_peek_memory (buf, 0)));

  GstBuffer *sys = gst_buffer_new_allocate (NULL, 16, NULL);
  g_assert_false (rds_is_rds_memory (gst_buffer_peek_memory (sys, 0)));
  g_assert_null (rds_buffer_get_surface (sys));
  gst_buffer_unref (sys);
  gst_buffer_unref (buf);
  gst_object_unref (alloc);
}

static void
test_copy_is_deep (void)
{
  GstAllocator *alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  GstBuffer *buf = gst_buffer_new ();
  RdsBufSurface *a, *b;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (alloc, 1, &nv12_640));
  a = rds_buffer_get_surface (buf);
  a->numFilled = 1;
  rds_buf_surface_memset (a, 0, -1, 42);

  /* A shallow buffer copy shares the memory; a deep copy duplicates the surface. */
  GstBuffer *shallow = gst_buffer_copy (buf);
  g_assert_true (rds_buffer_get_surface (shallow) == a);
  GstBuffer *deep = gst_buffer_copy_deep (buf);
  b = rds_buffer_get_surface (deep);
  g_assert_nonnull (b);
  g_assert_true (b != a);
  g_assert_true (b->surfaceList[0].dataPtr != a->surfaceList[0].dataPtr);
  g_assert_cmpuint (b->numFilled, ==, 1);
  g_assert_cmpuint (((guint8 *) b->surfaceList[0].dataPtr)[0], ==, 42);
  rds_buf_surface_memset (a, 0, -1, 1);
  g_assert_cmpuint (((guint8 *) b->surfaceList[0].dataPtr)[0], ==, 42);

  gst_buffer_unref (deep);
  gst_buffer_unref (shallow);
  gst_buffer_unref (buf);
  gst_object_unref (alloc);
}

static void
test_caps_feature (void)
{
  GstCaps *rds = gst_caps_from_string ("video/x-raw(memory:RDS), format=NV12, width=64, height=64");
  GstCaps *sys = gst_caps_from_string ("video/x-raw, format=NV12, width=64, height=64");

  g_assert_true (rds_caps_has_memory_rds (rds));
  g_assert_false (rds_caps_has_memory_rds (sys));
  gst_caps_unref (rds);
  gst_caps_unref (sys);
}

static void
test_format_mapping (void)
{
  static const struct { GstVideoFormat v; RdsBufSurfaceColorFormat r; } map[] = {
    {GST_VIDEO_FORMAT_GRAY8, RDS_BUF_COLOR_FORMAT_GRAY8}, {GST_VIDEO_FORMAT_I420, RDS_BUF_COLOR_FORMAT_YUV420},
    {GST_VIDEO_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_NV12}, {GST_VIDEO_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA},
    {GST_VIDEO_FORMAT_BGRA, RDS_BUF_COLOR_FORMAT_BGRA}, {GST_VIDEO_FORMAT_RGBx, RDS_BUF_COLOR_FORMAT_RGBx},
    {GST_VIDEO_FORMAT_BGRx, RDS_BUF_COLOR_FORMAT_BGRx}, {GST_VIDEO_FORMAT_RGB, RDS_BUF_COLOR_FORMAT_RGB},
    {GST_VIDEO_FORMAT_BGR, RDS_BUF_COLOR_FORMAT_BGR},
  };

  for (gsize i = 0; i < G_N_ELEMENTS (map); i++) {
    g_assert_cmpint (rds_buf_color_format_from_video_format (map[i].v), ==, map[i].r);
    g_assert_cmpint (rds_video_format_from_buf_color_format (map[i].r), ==, map[i].v);
  }
  g_assert_cmpint (rds_buf_color_format_from_video_format (GST_VIDEO_FORMAT_YUY2), ==,
      RDS_BUF_COLOR_FORMAT_INVALID);
  g_assert_cmpint (rds_video_format_from_buf_color_format (RDS_BUF_COLOR_FORMAT_INVALID), ==,
      GST_VIDEO_FORMAT_UNKNOWN);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/memory/map-returns-surface", test_map_returns_surface);
  g_test_add_func ("/memory/buffer-helpers", test_buffer_helpers);
  g_test_add_func ("/memory/copy-is-deep", test_copy_is_deep);
  g_test_add_func ("/memory/caps-feature", test_caps_feature);
  g_test_add_func ("/memory/format-mapping", test_format_mapping);
  return g_test_run ();
}
