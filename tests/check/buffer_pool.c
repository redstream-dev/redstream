/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds-buffer-pool.h>

static GstBufferPool *
make_pool (const gchar * caps_str, guint batch, guint min, guint max, gboolean video_meta)
{
  GstBufferPool *pool = rds_buffer_pool_new ();
  GstStructure *config = gst_buffer_pool_get_config (pool);
  GstCaps *caps = gst_caps_from_string (caps_str);

  gst_buffer_pool_config_set_params (config, caps, 0, min, max);
  rds_buffer_pool_config_set_surface_params (config, batch, RDS_BUF_MEM_SYSTEM, 0);
  if (video_meta)
    gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  g_assert_true (gst_buffer_pool_set_config (pool, config));
  gst_caps_unref (caps);
  return pool;
}

static void
test_single_frame (void)
{
  GstBufferPool *pool = make_pool ("video/x-raw(memory:RDS), format=NV12, width=1280, height=720",
      1, 1, 1, TRUE);       /* one buffer, so the recycled one is observable (the free queue is FIFO) */
  GstBuffer *buf = NULL, *again = NULL;
  RdsBufSurface *s;
  GstVideoMeta *vmeta;
  GstStructure *config;
  guint size;

  config = gst_buffer_pool_get_config (pool);
  g_assert_true (gst_buffer_pool_config_get_params (config, NULL, &size, NULL, NULL));
  g_assert_cmpuint (size, ==, sizeof (RdsBufSurface));
  gst_structure_free (config);

  g_assert_true (gst_buffer_pool_set_active (pool, TRUE));
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &buf, NULL), ==, GST_FLOW_OK);
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  g_assert_cmpuint (s->batchSize, ==, 1);
  g_assert_cmpuint (s->numFilled, ==, 1);
  g_assert_cmpuint (s->surfaceList[0].width, ==, 1280);
  g_assert_cmpint (s->surfaceList[0].colorFormat, ==, RDS_BUF_COLOR_FORMAT_NV12);
  vmeta = gst_buffer_get_video_meta (buf);
  g_assert_nonnull (vmeta);
  g_assert_cmpint (vmeta->format, ==, GST_VIDEO_FORMAT_NV12);
  g_assert_cmpint (vmeta->stride[0], ==, 1280);
  g_assert_cmpuint (vmeta->offset[1], ==, 1280 * 720);

  gst_buffer_unref (buf);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &again, NULL), ==, GST_FLOW_OK);
  g_assert_true (rds_buffer_get_surface (again) == s);          /* recycled */
  g_assert_nonnull (gst_buffer_get_video_meta (again));         /* pooled meta survives release */
  gst_buffer_unref (again);
  g_assert_true (gst_buffer_pool_set_active (pool, FALSE));
  gst_object_unref (pool);
}

static void
test_batch_reset (void)
{
  GstBufferPool *pool = make_pool ("video/x-raw(memory:RDS), format=RGBA, width=640, height=640",
      4, 1, 1, TRUE);
  GstBuffer *buf = NULL;
  RdsBufSurface *s;

  g_assert_true (gst_buffer_pool_set_active (pool, TRUE));
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &buf, NULL), ==, GST_FLOW_OK);
  s = rds_buffer_get_surface (buf);
  g_assert_cmpuint (s->batchSize, ==, 4);
  g_assert_cmpuint (s->numFilled, ==, 0);
  g_assert_null (gst_buffer_get_video_meta (buf));              /* batches carry no single-frame meta */
  s->numFilled = 3;
  gst_buffer_add_video_crop_meta (buf);                         /* non-pooled meta: dropped on release */
  gst_buffer_unref (buf);

  buf = NULL;
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &buf, NULL), ==, GST_FLOW_OK);
  g_assert_cmpuint (rds_buffer_get_surface (buf)->numFilled, ==, 0);
  g_assert_null (gst_buffer_get_video_crop_meta (buf));

  /* max-buffers=1 is respected. */
  GstBufferPoolAcquireParams params = { .flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT };
  GstBuffer *second = NULL;
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &second, &params), ==, GST_FLOW_EOS);
  gst_buffer_unref (buf);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

static void
test_bad_config (void)
{
  GstBufferPool *pool = rds_buffer_pool_new ();
  GstStructure *config = gst_buffer_pool_get_config (pool);
  GstCaps *caps = gst_caps_from_string ("video/x-raw(memory:RDS), format=YUY2, width=64, height=64");

  gst_buffer_pool_config_set_params (config, caps, 0, 0, 0);
  g_assert_false (gst_buffer_pool_set_config (pool, config));
  gst_caps_unref (caps);

  config = gst_buffer_pool_get_config (pool);
  caps = gst_caps_from_string ("video/x-raw(memory:RDS), format=NV12, width=64, height=64");
  gst_buffer_pool_config_set_params (config, caps, 0, 0, 0);
  rds_buffer_pool_config_set_surface_params (config, 0, RDS_BUF_MEM_SYSTEM, 0);      /* batch 0 */
  g_assert_false (gst_buffer_pool_set_config (pool, config));
  gst_caps_unref (caps);
  gst_object_unref (pool);
}

static void
test_config_params (void)
{
  GstStructure *config = gst_structure_new_empty ("config");
  guint batch = 99, gpu = 99;
  RdsBufSurfaceMemType mt = RDS_BUF_MEM_HANDLE;

  g_assert_false (rds_buffer_pool_config_get_surface_params (config, &batch, &mt, &gpu));
  g_assert_cmpuint (batch, ==, 99);
  rds_buffer_pool_config_set_surface_params (config, 8, RDS_BUF_MEM_HIP_PINNED, 1);
  g_assert_true (rds_buffer_pool_config_get_surface_params (config, &batch, &mt, &gpu));
  g_assert_cmpuint (batch, ==, 8);
  g_assert_cmpint (mt, ==, RDS_BUF_MEM_HIP_PINNED);
  g_assert_cmpuint (gpu, ==, 1);
  gst_structure_free (config);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/buffer-pool/single-frame", test_single_frame);
  g_test_add_func ("/buffer-pool/batch-reset", test_batch_reset);
  g_test_add_func ("/buffer-pool/bad-config", test_bad_config);
  g_test_add_func ("/buffer-pool/config-params", test_config_params);
  return g_test_run ();
}
