/* rds_buf_surf_transform public API: enum values, error reporting, per-thread session parameters; needs no GPU.
 * Surfaces are hand-built on the stack with NULL pixel pointers, so every transform call here must fail
 * validation (the rules are tested one by one in transform_validate).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <rds/rds.h>

#include <string.h>

typedef struct
{
  RdsBufSurface surf;
  RdsBufSurfaceParams params[2];
} HandSurface;

/* Plane geometry as rds_buf_surface_create () lays it out (validation checks it). */
static void
hand_planes (RdsBufSurfaceParams * p)
{
  guint n = 1, bpp0 = 4, cbpp = 1;
  guint32 offset = 0;

  switch (p->colorFormat) {
    case RDS_BUF_COLOR_FORMAT_GRAY8:
      bpp0 = 1;
      break;
    case RDS_BUF_COLOR_FORMAT_YUV420:
      n = 3, bpp0 = 1, cbpp = 1;
      break;
    case RDS_BUF_COLOR_FORMAT_NV12:
      n = 2, bpp0 = 1, cbpp = 2;
      break;
    case RDS_BUF_COLOR_FORMAT_RGB:
    case RDS_BUF_COLOR_FORMAT_BGR:
      bpp0 = 3;
      break;
    default:
      break;
  }
  p->planeParams.num_planes = n;
  for (guint pl = 0; pl < n; pl++) {
    RdsBufSurfacePlaneParams *pp = &p->planeParams;

    pp->width[pl] = pl == 0 ? p->width : (p->width + 1) / 2;
    pp->height[pl] = pl == 0 ? p->height : (p->height + 1) / 2;
    pp->bytesPerPix[pl] = pl == 0 ? bpp0 : cbpp;
    pp->pitch[pl] = GST_ROUND_UP_N (pp->width[pl] * pp->bytesPerPix[pl], RDS_BUF_SURFACE_PITCH_ALIGN);
    pp->psize[pl] = pp->pitch[pl] * pp->height[pl];
    pp->offset[pl] = offset;
    offset += pp->psize[pl];
  }
  p->pitch = p->planeParams.pitch[0];
  p->dataSize = offset;
}

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
    p->colorFormat = fmt;
    p->layout = RDS_BUF_LAYOUT_PITCH;
    p->dataPtr = NULL;
    hand_planes (p);
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

/* NULL arguments (g_return_val_if_fail, so the critical is expected). */
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

/* The public entry point reports validation errors unchanged (the rules themselves are covered by
 * transform_validate). Only invalid calls here: a valid one would reach the GPU with NULL pixel pointers. */
static void
test_errors_reported (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };
  RdsBufSurfTransformRect r = {.top = 0,.left = 0,.width = 321,.height = 240 };

  make_pair (&src, &dst);
  src.surf.numFilled = 0;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);

  make_pair (&src, &dst);
  src.surf.memType = RDS_BUF_MEM_SYSTEM;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_Unsupported);

  make_pair (&src, &dst);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  tp.dst_rect = &r;
  g_assert_cmpint (run (&src, &dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
  g_assert_cmpuint (dst.surf.numFilled, ==, 0);
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
  g_test_add_func ("/transform/null-args", test_null_args);
  g_test_add_func ("/transform/errors-reported", test_errors_reported);
  g_test_add_func ("/transform/session/default", test_session_default);
  g_test_add_func ("/transform/session/roundtrip", test_session_roundtrip);
  g_test_add_func ("/transform/session/rejects", test_session_rejects);
  g_test_add_func ("/transform/session/per-thread", test_session_per_thread);
  return g_test_run ();
}
