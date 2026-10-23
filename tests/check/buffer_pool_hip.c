/* RdsBufferPool on HIP device memory; skips without a ROCm device.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include <rds/rds-buffer-pool.h>

static void
test_single_frame (void)
{
  RDS_TEST_REQUIRE_GPU ();

  GstBufferPool *pool = rds_buffer_pool_new ();
  GstStructure *config = gst_buffer_pool_get_config (pool);
  GstCaps *caps = gst_caps_from_string ("video/x-raw(memory:RDS), format=NV12, width=1280, height=720");
  GstBuffer *buf = NULL, *again = NULL;
  GstVideoMeta *vmeta;
  RdsBufSurface *s;

  /* One buffer, so the recycled one is observable (the free queue is FIFO). */
  gst_buffer_pool_config_set_params (config, caps, 0, 1, 1);
  rds_buffer_pool_config_set_surface_params (config, 1, RDS_BUF_MEM_DEFAULT, 0);
  gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  g_assert_true (gst_buffer_pool_set_config (pool, config));
  gst_caps_unref (caps);

  g_assert_true (gst_buffer_pool_set_active (pool, TRUE));
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &buf, NULL), ==, GST_FLOW_OK);
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  g_assert_cmpint (s->memType, ==, RDS_BUF_MEM_DEFAULT);
  g_assert_cmpuint (s->gpuId, ==, 0);
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
  g_assert_true (rds_buffer_get_surface (again) == s);
  g_assert_nonnull (gst_buffer_get_video_meta (again));
  gst_buffer_unref (again);
  g_assert_true (gst_buffer_pool_set_active (pool, FALSE));
  gst_object_unref (pool);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/buffer-pool-hip/single-frame", test_single_frame);
  return g_test_run ();
}
