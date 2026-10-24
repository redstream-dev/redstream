/* rds_buf_surf_transform: argument validation, session parameters and enum values; needs no GPU.
 * Surfaces are hand-built on the stack with NULL pixel pointers: validation never dereferences them.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <rds/rds.h>

#include <string.h>

typedef struct
{
  RdsBufSurface surf;
  RdsBufSurfaceParams params[2];
} HandSurface;

static void
hand_surface_init (HandSurface * h, RdsBufSurfaceColorFormat fmt, guint32 w, guint32 h_, guint32 batch,
    guint32 filled)
{
  memset (h, 0, sizeof (*h));
  g_assert_cmpuint (batch, <=, G_N_ELEMENTS (h->params));
  h->surf.batchSize = batch;
  h->surf.numFilled = filled;
  h->surf.isContiguous = TRUE;
  h->surf.memType = RDS_BUF_MEM_DEFAULT;
  h->surf.surfaceList = h->params;
  for (guint i = 0; i < batch; i++) {
    RdsBufSurfaceParams *p = &h->params[i];

    p->width = w;
    p->height = h_;
    p->pitch = GST_ROUND_UP_N (w * 4, RDS_BUF_SURFACE_PITCH_ALIGN);
    p->colorFormat = fmt;
    p->layout = RDS_BUF_LAYOUT_PITCH;
    p->dataPtr = NULL;
  }
}

/* A valid NV12 640x480 -> RGBA 320x240 pair with one filled surface. */
static void
make_pair (HandSurface * src, HandSurface * dst)
{
  hand_surface_init (src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 1, 1);
  hand_surface_init (dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 1, 0);
}

static RdsBufSurfTransform_Error
run (HandSurface * src, HandSurface * dst, RdsBufSurfTransformParams * tp)
{
  return rds_buf_surf_transform (&src->surf, &dst->surf, tp);
}

static void
test_enum_values (void)
{
  g_assert_cmpint (RdsBufSurfTransformCompute_Default, ==, 0);
  g_assert_cmpint (RdsBufSurfTransformCompute_GPU, ==, 1);
  g_assert_cmpint (RdsBufSurfTransformCompute_VIC, ==, 2);
  g_assert_cmpint (RdsBufSurfTransform_None, ==, 0);
  g_assert_cmpint (RdsBufSurfTransform_Rotate90, ==, 1);
  g_assert_cmpint (RdsBufSurfTransform_Rotate180, ==, 2);
  g_assert_cmpint (RdsBufSurfTransform_Rotate270, ==, 3);
  g_assert_cmpint (RdsBufSurfTransform_FlipX, ==, 4);
  g_assert_cmpint (RdsBufSurfTransform_FlipY, ==, 5);
  g_assert_cmpint (RdsBufSurfTransform_Transpose, ==, 6);
  g_assert_cmpint (RdsBufSurfTransform_InvTranspose, ==, 7);
  g_assert_cmpint (RdsBufSurfTransformInter_Nearest, ==, 0);
  g_assert_cmpint (RdsBufSurfTransformInter_Bilinear, ==, 1);
  g_assert_cmpint (RdsBufSurfTransformInter_Algo1, ==, 2);
  g_assert_cmpint (RdsBufSurfTransformInter_Algo2, ==, 3);
  g_assert_cmpint (RdsBufSurfTransformInter_Algo3, ==, 4);
  g_assert_cmpint (RdsBufSurfTransformInter_Algo4, ==, 5);
  g_assert_cmpint (RdsBufSurfTransformInter_Default, ==, 6);
  g_assert_cmpint (RdsBufSurfTransformError_ROI_Error, ==, -4);
  g_assert_cmpint (RdsBufSurfTransformError_Invalid_Params, ==, -3);
  g_assert_cmpint (RdsBufSurfTransformError_Execution_Error, ==, -2);
  g_assert_cmpint (RdsBufSurfTransformError_Unsupported, ==, -1);
  g_assert_cmpint (RdsBufSurfTransformError_Success, ==, 0);
  g_assert_cmpint (RDS_BUFSURF_TRANSFORM_CROP_SRC, ==, 1);
  g_assert_cmpint (RDS_BUFSURF_TRANSFORM_CROP_DST, ==, 2);
  g_assert_cmpint (RDS_BUFSURF_TRANSFORM_FILTER, ==, 4);
  g_assert_cmpint (RDS_BUFSURF_TRANSFORM_FLIP, ==, 8);
  g_assert_cmpint (RDS_BUFSURF_TRANSFORM_NORMALIZE, ==, 16);
  g_assert_cmpint (RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP, ==, 32);
}

/* Rule 1: NULL arguments (g_return_val_if_fail, so the critical is expected). */
static void
test_null_args (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  make_pair (&src, &dst);
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*src != NULL*");
  g_assert_cmpint (rds_buf_surf_transform (NULL, &dst.surf, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*dst != NULL*");
  g_assert_cmpint (rds_buf_surf_transform (&src.surf, NULL, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*transform_params != NULL*");
  g_assert_cmpint (rds_buf_surf_transform (&src.surf, &dst.surf, NULL), ==,
      RdsBufSurfTransformError_Invalid_Params);
  g_test_assert_expected_messages ();
}

/* Rule 2: batch counts and surface lists. */
static void
test_batch_counts (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  make_pair (&src, &dst);
  src.surf.numFilled = 0;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  make_pair (&src, &dst);
  src.surf.numFilled = 2;       /* > src batchSize 1 */
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 2, 2);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 1, 0);  /* dst batchSize 1 < 2 */
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  make_pair (&src, &dst);
  src.surf.surfaceList = NULL;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  make_pair (&src, &dst);
  dst.surf.surfaceList = NULL;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
}

/* Rule 3: VIC, NORMALIZE, and a real flip. FLIP with None is allowed. */
static void
test_unsupported_modes (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };
  RdsBufSurfTransformConfigParams cfg = { 0 };

  make_pair (&src, &dst);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_NORMALIZE;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);

  tp.transform_flag = RDS_BUFSURF_TRANSFORM_FLIP;
  tp.transform_flip = RdsBufSurfTransform_Rotate90;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);

  /* A flip value without the FLIP flag is ignored, so this must get past rule 3 (to a later stage). Rule 8 is
   * checked with an out-of-range filter to tell the stages apart. */
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_FILTER;
  tp.transform_filter = 7;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  tp.transform_flag = RDS_BUFSURF_TRANSFORM_FLIP | RDS_BUFSURF_TRANSFORM_FILTER;
  tp.transform_flip = RdsBufSurfTransform_None;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  /* VIC can't be stored in a session (set_session_params rejects it), so the transform's own VIC check is a
   * safety net that the public API can't reach. Check that the rejection is reported here too. */
  cfg.compute_mode = RdsBufSurfTransformCompute_VIC;
  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&cfg), ==, RdsBufSurfTransformError_Unsupported);
}

/* Rule 4: memory types that aren't device-accessible. */
static void
test_mem_types (void)
{
  static const RdsBufSurfaceMemType bad[] = {
    RDS_BUF_MEM_SYSTEM, RDS_BUF_MEM_SURFACE_ARRAY, RDS_BUF_MEM_HANDLE, (RdsBufSurfaceMemType) 42,
  };
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
    make_pair (&src, &dst);
    src.surf.memType = bad[i];
    g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);
    make_pair (&src, &dst);
    dst.surf.memType = bad[i];
    g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);
  }
}

/* Rule 5: colour format and layout, on any filled surface of either side. */
static void
test_formats (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  make_pair (&src, &dst);
  src.params[0].colorFormat = RDS_BUF_COLOR_FORMAT_INVALID;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);

  make_pair (&src, &dst);
  dst.params[0].colorFormat = RDS_BUF_COLOR_FORMAT_LAST;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);

  make_pair (&src, &dst);
  src.params[0].layout = RDS_BUF_LAYOUT_BLOCK_LINEAR;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);

  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 2, 2);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 2, 0);
  dst.params[1].layout = RDS_BUF_LAYOUT_BLOCK_LINEAR;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);
}

/* Rule 6: rect arrays and bounds. */
static void
test_rects (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };
  RdsBufSurfTransformRect r;

  make_pair (&src, &dst);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC;
  tp.src_rect = &r;
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 0,.height = 10 };
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 10,.height = 0 };
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 631,.width = 10,.height = 10 };
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
  r = (RdsBufSurfTransformRect) {.top = 471,.left = 0,.width = 10,.height = 10 };
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
  /* left + width wraps to 0x10 in 32 bits */
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0xFFFFFFF0u,.width = 0x20,.height = 10 };
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
  r = (RdsBufSurfTransformRect) {.top = 0xFFFFFFF0u,.left = 0,.width = 10,.height = 0x20 };
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);

  /* The exact edge is in bounds (and odd source rects are fine): the call passes rule 6, rule 8 catches it. */
  r = (RdsBufSurfTransformRect) {.top = 1,.left = 3,.width = 637,.height = 479 };
  tp.transform_flag |= RDS_BUFSURF_TRANSFORM_FILTER;
  tp.transform_filter = (RdsBufSurfTransform_Inter) 99;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  /* dst rect checked against the dst surface (320x240) */
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  tp.src_rect = NULL;
  tp.dst_rect = &r;
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 322,.height = 240 };
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
}

/* Rule 7: odd destination rect on 4:2:0, and ALLOW_ODD_CROP. */
static void
test_odd_crop (void)
{
  static const RdsBufSurfTransformRect odd[] = {
    {.top = 0,.left = 1,.width = 10,.height = 10},
    {.top = 1,.left = 0,.width = 10,.height = 10},
    {.top = 0,.left = 0,.width = 11,.height = 10},
    {.top = 0,.left = 0,.width = 10,.height = 11},
  };
  static const RdsBufSurfaceColorFormat yuv[] = { RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_YUV420 };
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };
  RdsBufSurfTransformRect r;

  tp.dst_rect = &r;
  for (guint f = 0; f < G_N_ELEMENTS (yuv); f++) {
    for (guint i = 0; i < G_N_ELEMENTS (odd); i++) {
      hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_RGBA, 64, 64, 1, 1);
      hand_surface_init (&dst, yuv[f], 64, 64, 1, 0);
      r = odd[i];
      tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
      g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
      /* With the flag the call reaches the launch stage: Execution_Error without a GPU, or the interim
       * Unsupported. */
      tp.transform_flag |= RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP;
      g_assert_cmpint (run (&src, &dst, &tp), !=, RdsBufSurfTransformError_Invalid_Params);
    }
  }

  /* Odd rects on RGB destinations and odd source rects on 4:2:0 are always fine. */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 64, 64, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_BGRx, 64, 64, 1, 0);
  r = odd[0];
  tp.src_rect = &r;
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC | RDS_BUFSURF_TRANSFORM_CROP_DST;
  g_assert_cmpint (run (&src, &dst, &tp), !=, RdsBufSurfTransformError_Invalid_Params);
}

/* Rule 8: filter range (read when FILTER is set). */
static void
test_filter (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  make_pair (&src, &dst);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_FILTER;
  tp.transform_filter = (RdsBufSurfTransform_Inter) 7;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
  tp.transform_filter = (RdsBufSurfTransform_Inter) - 1;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
  for (gint f = 0; f <= 6; f++) {
    tp.transform_filter = (RdsBufSurfTransform_Inter) f;
    g_assert_cmpint (run (&src, &dst, &tp), !=, RdsBufSurfTransformError_Invalid_Params);
  }
}

/* A fully valid call reaches the launch stage. */
static void
test_valid_reaches_launch (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };
  RdsBufSurfTransform_Error ret;

  make_pair (&src, &dst);
  ret = run (&src, &dst, &tp);
  g_assert_true (ret == RdsBufSurfTransformError_Execution_Error || ret == RdsBufSurfTransformError_Unsupported);
}

static void
test_session_default (void)
{
  RdsBufSurfTransformConfigParams cfg;

  memset (&cfg, 0xAB, sizeof (cfg));
  g_assert_cmpint (rds_buf_surf_transform_get_session_params (&cfg), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (cfg.compute_mode, ==, RdsBufSurfTransformCompute_Default);
  g_assert_cmpint (cfg.gpu_id, ==, 0);
  g_assert_null (cfg.cuda_stream);
}

static void
test_session_roundtrip (void)
{
  RdsBufSurfTransformConfigParams in = {
    .compute_mode = RdsBufSurfTransformCompute_GPU,.gpu_id = 0,.cuda_stream = GSIZE_TO_POINTER (0x1234),
  };
  RdsBufSurfTransformConfigParams out = { 0 };

  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&in), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (rds_buf_surf_transform_get_session_params (&out), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (out.compute_mode, ==, RdsBufSurfTransformCompute_GPU);
  g_assert_cmpint (out.gpu_id, ==, 0);
  g_assert_true (out.cuda_stream == GSIZE_TO_POINTER (0x1234));

  g_assert_cmpint (rds_buf_surf_transform_set_default_session (), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (rds_buf_surf_transform_get_session_params (&out), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (out.compute_mode, ==, RdsBufSurfTransformCompute_Default);
  g_assert_null (out.cuda_stream);
}

static void
test_session_rejects (void)
{
  RdsBufSurfTransformConfigParams cfg = {.compute_mode = RdsBufSurfTransformCompute_VIC };
  RdsBufSurfTransformConfigParams out = { 0 };

  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&cfg), ==, RdsBufSurfTransformError_Unsupported);
  cfg.compute_mode = RdsBufSurfTransformCompute_GPU;
  cfg.gpu_id = -1;
  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&cfg), ==, RdsBufSurfTransformError_Invalid_Params);
  cfg.gpu_id = 0;
  cfg.compute_mode = (RdsBufSurfTransform_Compute) 7;
  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&cfg), ==, RdsBufSurfTransformError_Invalid_Params);

  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*config_params != NULL*");
  g_assert_cmpint (rds_buf_surf_transform_set_session_params (NULL), ==, RdsBufSurfTransformError_Invalid_Params);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*config_params != NULL*");
  g_assert_cmpint (rds_buf_surf_transform_get_session_params (NULL), ==, RdsBufSurfTransformError_Invalid_Params);
  g_test_assert_expected_messages ();

  /* rejected calls leave the session unchanged */
  g_assert_cmpint (rds_buf_surf_transform_get_session_params (&out), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (out.compute_mode, ==, RdsBufSurfTransformCompute_Default);
  g_assert_cmpint (out.gpu_id, ==, 0);
}

static gpointer
read_session (gpointer data)
{
  RdsBufSurfTransformConfigParams *out = data;

  g_assert_cmpint (rds_buf_surf_transform_get_session_params (out), ==, RdsBufSurfTransformError_Success);
  return NULL;
}

static void
test_session_per_thread (void)
{
  RdsBufSurfTransformConfigParams in = {
    .compute_mode = RdsBufSurfTransformCompute_GPU,.gpu_id = 0,.cuda_stream = GSIZE_TO_POINTER (0x5678),
  };
  RdsBufSurfTransformConfigParams other;
  GThread *t;

  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&in), ==, RdsBufSurfTransformError_Success);
  memset (&other, 0xAB, sizeof (other));
  t = g_thread_new ("session", read_session, &other);
  g_thread_join (t);
  g_assert_cmpint (other.compute_mode, ==, RdsBufSurfTransformCompute_Default);
  g_assert_cmpint (other.gpu_id, ==, 0);
  g_assert_null (other.cuda_stream);
  g_assert_cmpint (rds_buf_surf_transform_set_default_session (), ==, RdsBufSurfTransformError_Success);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/transform/enum-values", test_enum_values);
  g_test_add_func ("/transform/validate/1-null-args", test_null_args);
  g_test_add_func ("/transform/validate/2-batch-counts", test_batch_counts);
  g_test_add_func ("/transform/validate/3-unsupported-modes", test_unsupported_modes);
  g_test_add_func ("/transform/validate/4-mem-types", test_mem_types);
  g_test_add_func ("/transform/validate/5-formats", test_formats);
  g_test_add_func ("/transform/validate/6-rects", test_rects);
  g_test_add_func ("/transform/validate/7-odd-crop", test_odd_crop);
  g_test_add_func ("/transform/validate/8-filter", test_filter);
  g_test_add_func ("/transform/validate/valid-reaches-launch", test_valid_reaches_launch);
  g_test_add_func ("/transform/session/default", test_session_default);
  g_test_add_func ("/transform/session/roundtrip", test_session_roundtrip);
  g_test_add_func ("/transform/session/rejects", test_session_rejects);
  g_test_add_func ("/transform/session/per-thread", test_session_per_thread);
  return g_test_run ();
}
