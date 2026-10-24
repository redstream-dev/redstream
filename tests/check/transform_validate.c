/* rds_buf_surf_transform argument validation, through the internal rds_transform_validate (): every rule returns
 * its documented error and valid input returns Success. Nothing here launches GPU work, so the hand-built
 * surfaces (NULL pixel pointers) are safe with or without a GPU. Links the static internal library only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <rds/rds-transform-private.h>

#include <string.h>

#define OK RdsBufSurfTransformError_Success
#define INVALID RdsBufSurfTransformError_Invalid_Params
#define UNSUPPORTED RdsBufSurfTransformError_Unsupported
#define ROI RdsBufSurfTransformError_ROI_Error

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

/* A valid NV12 640x480 -> RGBA 320x240 pair of the given batch size, all source surfaces filled. */
static void
make_pair_n (HandSurface * src, HandSurface * dst, guint32 batch)
{
  hand_surface_init (src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, batch, batch);
  hand_surface_init (dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, batch, 0);
}

static void
make_pair (HandSurface * src, HandSurface * dst)
{
  make_pair_n (src, dst, 1);
}

static RdsBufSurfTransform_Error
validate (HandSurface * src, HandSurface * dst, RdsBufSurfTransformParams * tp)
{
  return rds_transform_validate (&src->surf, &dst->surf, tp);
}

static void
test_valid (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  make_pair (&src, &dst);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  make_pair_n (&src, &dst, 2);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  /* dst may be larger than the filled count */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 2, 0);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
}

/* Rule 1: NULL arguments (g_return_val_if_fail, so the critical is expected). */
static void
test_null_args (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  make_pair (&src, &dst);
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*src != NULL*");
  g_assert_cmpint (rds_transform_validate (NULL, &dst.surf, &tp), ==, INVALID);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*dst != NULL*");
  g_assert_cmpint (rds_transform_validate (&src.surf, NULL, &tp), ==, INVALID);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*transform_params != NULL*");
  g_assert_cmpint (rds_transform_validate (&src.surf, &dst.surf, NULL), ==, INVALID);
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
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  /* src numFilled 2 > src batchSize 1, although dst could take 2 */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 1, 2);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 2, 0);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  /* dst batchSize 1 < src numFilled 2 */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 2, 2);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 1, 0);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  make_pair (&src, &dst);
  src.surf.surfaceList = NULL;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  make_pair (&src, &dst);
  dst.surf.surfaceList = NULL;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
}

/* Rule 3: NORMALIZE and a real flip. FLIP with None, or a flip value without FLIP, is fine. */
static void
test_unsupported_modes (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  make_pair (&src, &dst);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_NORMALIZE;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);

  tp.transform_flag = RDS_BUFSURF_TRANSFORM_FLIP;
  tp.transform_flip = RdsBufSurfTransform_Rotate90;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);

  tp.transform_flag = 0;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);

  tp.transform_flag = RDS_BUFSURF_TRANSFORM_FLIP;
  tp.transform_flip = RdsBufSurfTransform_None;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);

  /* The VIC check reads the session, but set_session_params refuses VIC, so a session never holds it and that
   * branch can't be reached from a test; the session tests in transform_params cover the refusal. */
}

/* Rule 4: memory types, both ways. */
static void
test_mem_types (void)
{
  static const RdsBufSurfaceMemType bad[] = {
    RDS_BUF_MEM_SYSTEM, RDS_BUF_MEM_SURFACE_ARRAY, RDS_BUF_MEM_HANDLE, (RdsBufSurfaceMemType) 42,
  };
  static const RdsBufSurfaceMemType good[] = {
    RDS_BUF_MEM_DEFAULT, RDS_BUF_MEM_HIP_DEVICE, RDS_BUF_MEM_HIP_PINNED, RDS_BUF_MEM_HIP_UNIFIED,
  };
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
    make_pair (&src, &dst);
    src.surf.memType = bad[i];
    g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);
    make_pair (&src, &dst);
    dst.surf.memType = bad[i];
    g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);
  }
  for (guint i = 0; i < G_N_ELEMENTS (good); i++) {
    for (guint j = 0; j < G_N_ELEMENTS (good); j++) {
      make_pair (&src, &dst);
      src.surf.memType = good[i];
      dst.surf.memType = good[j];
      g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
    }
  }
}

/* Rule 4b: device memory (default, device) must live on the session's GPU (GPU 0 here, the default session);
 * pinned and unified memory is reachable from any GPU. */
static void
test_gpu_id (void)
{
  static const RdsBufSurfaceMemType device[] = { RDS_BUF_MEM_DEFAULT, RDS_BUF_MEM_HIP_DEVICE };
  static const RdsBufSurfaceMemType host[] = { RDS_BUF_MEM_HIP_PINNED, RDS_BUF_MEM_HIP_UNIFIED };
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  for (guint i = 0; i < G_N_ELEMENTS (device); i++) {
    make_pair (&src, &dst);
    src.surf.memType = device[i];
    src.surf.gpuId = 1;
    g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
    make_pair (&src, &dst);
    dst.surf.memType = device[i];
    dst.surf.gpuId = 1;
    g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  }
  for (guint i = 0; i < G_N_ELEMENTS (host); i++) {
    make_pair (&src, &dst);
    src.surf.memType = host[i];
    src.surf.gpuId = 1;
    dst.surf.memType = host[i];
    dst.surf.gpuId = 1;
    g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  }
}

/* Rule 5: colour format and layout, on any filled surface of either side. */
static void
test_formats (void)
{
  static const RdsBufSurfaceColorFormat all[] = {
    RDS_BUF_COLOR_FORMAT_GRAY8, RDS_BUF_COLOR_FORMAT_YUV420, RDS_BUF_COLOR_FORMAT_NV12,
    RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_BGRA, RDS_BUF_COLOR_FORMAT_RGBx,
    RDS_BUF_COLOR_FORMAT_BGRx, RDS_BUF_COLOR_FORMAT_RGB, RDS_BUF_COLOR_FORMAT_BGR,
  };
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  for (guint i = 0; i < G_N_ELEMENTS (all); i++) {
    for (guint j = 0; j < G_N_ELEMENTS (all); j++) {
      hand_surface_init (&src, all[i], 64, 64, 1, 1);
      hand_surface_init (&dst, all[j], 32, 32, 1, 0);
      g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
    }
  }

  make_pair (&src, &dst);
  src.params[0].colorFormat = RDS_BUF_COLOR_FORMAT_INVALID;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);

  make_pair (&src, &dst);
  dst.params[0].colorFormat = RDS_BUF_COLOR_FORMAT_LAST;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);

  make_pair (&src, &dst);
  src.params[0].layout = RDS_BUF_LAYOUT_BLOCK_LINEAR;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);

  /* only the second surface of a batch of 2 is bad */
  make_pair_n (&src, &dst, 2);
  src.params[1].colorFormat = RDS_BUF_COLOR_FORMAT_INVALID;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);
  make_pair_n (&src, &dst, 2);
  dst.params[1].colorFormat = RDS_BUF_COLOR_FORMAT_LAST;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);
  make_pair_n (&src, &dst, 2);
  dst.params[1].layout = RDS_BUF_LAYOUT_BLOCK_LINEAR;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, UNSUPPORTED);

  /* an unfilled dst surface past numFilled isn't looked at */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 2, 0);
  dst.params[1].colorFormat = RDS_BUF_COLOR_FORMAT_INVALID;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
}

/* Rule 5b: sizes up to RDS_BUF_SURFACE_MAX_DIM and plane geometry that holds the format; surfaces made by
 * rds_buf_surface_create () always pass. */
static void
test_geometry (void)
{
  static const RdsBufSurfaceColorFormat all[] = {
    RDS_BUF_COLOR_FORMAT_GRAY8, RDS_BUF_COLOR_FORMAT_YUV420, RDS_BUF_COLOR_FORMAT_NV12,
    RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_BGRA, RDS_BUF_COLOR_FORMAT_RGBx,
    RDS_BUF_COLOR_FORMAT_BGRx, RDS_BUF_COLOR_FORMAT_RGB, RDS_BUF_COLOR_FORMAT_BGR,
  };
  static const guint32 sizes[][2] = { {1, 1}, {37, 29}, {640, 480}, {RDS_BUF_SURFACE_MAX_DIM, 2} };
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };

  /* system-memory surfaces from rds_buf_surface_create: only the geometry matters to validation */
  for (guint f = 0; f < G_N_ELEMENTS (all); f++) {
    for (guint z = 0; z < G_N_ELEMENTS (sizes); z++) {
      RdsBufSurfaceCreateParams cp = {
        .width = sizes[z][0], .height = sizes[z][1], .colorFormat = all[f],
        .layout = RDS_BUF_LAYOUT_PITCH, .memType = RDS_BUF_MEM_SYSTEM,
      };
      RdsBufSurface *made = NULL;

      g_assert_cmpint (rds_buf_surface_create (&made, 1, &cp), ==, 0);
      made->numFilled = 1;
      made->memType = RDS_BUF_MEM_DEFAULT;      /* passes rule 4; nothing is launched */
      hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 32, 32, 1, 0);
      g_assert_cmpint (rds_transform_validate (made, &dst.surf, &tp), ==, OK);
      hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_RGBA, 32, 32, 1, 1);
      g_assert_cmpint (rds_transform_validate (&src.surf, made, &tp), ==, OK);
      made->memType = RDS_BUF_MEM_SYSTEM;
      g_assert_cmpint (rds_buf_surface_destroy (made), ==, 0);
    }
  }

  /* too large, either side, either dimension */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_GRAY8, RDS_BUF_SURFACE_MAX_DIM + 1, 2, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 32, 32, 1, 0);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_GRAY8, 2, RDS_BUF_SURFACE_MAX_DIM + 1, 1, 1);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_RGBA, 32, 32, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_SURFACE_MAX_DIM + 2, 2, 1, 0);
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  /* plane count */
  make_pair (&src, &dst);
  src.params[0].planeParams.num_planes = 1;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  make_pair (&src, &dst);
  src.params[0].planeParams.num_planes = 3;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  make_pair (&src, &dst);
  dst.params[0].planeParams.num_planes = 0;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  /* pitch and height per plane (NV12: plane 1 rows are 2 * ceil (w / 2) bytes, ceil (h / 2) rows) */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGB, 37, 29, 1, 0);
  src.params[0].planeParams.pitch[1] = 37;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  src.params[0].planeParams.pitch[1] = 38;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  src.params[0].planeParams.height[1] = 14;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, 1, 1);
  src.params[0].planeParams.height[0] = 28;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, 1, 1);
  dst.params[0].planeParams.pitch[0] = 110;     /* RGB needs 111 */
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  dst.params[0].planeParams.pitch[0] = 111;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  /* I420 chroma planes */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_YUV420, 37, 29, 1, 1);
  src.params[0].planeParams.pitch[2] = 18;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  /* only filled surfaces are checked */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 640, 480, 2, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_RGBA, 320, 240, 2, 0);
  src.params[1].planeParams.num_planes = 0;
  dst.params[1].planeParams.pitch[0] = 0;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
}

/* Rule 6: rect arrays and bounds. */
static void
test_rects (void)
{
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };
  RdsBufSurfTransformRect r, r2[2];

  make_pair (&src, &dst);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC;
  tp.src_rect = &r;
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 0,.height = 10 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 10,.height = 0 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 631,.width = 10,.height = 10 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
  r = (RdsBufSurfTransformRect) {.top = 471,.left = 0,.width = 10,.height = 10 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
  /* left + width wraps to 0x10 in 32 bits */
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0xFFFFFFF0u,.width = 0x20,.height = 10 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
  r = (RdsBufSurfTransformRect) {.top = 0xFFFFFFF0u,.left = 0,.width = 10,.height = 0x20 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);

  /* the exact edge is in bounds, and odd source rects are fine on 4:2:0 */
  r = (RdsBufSurfTransformRect) {.top = 1,.left = 3,.width = 637,.height = 479 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);

  /* the dst rect is checked against the dst surface (320x240) */
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  tp.src_rect = NULL;
  tp.dst_rect = &r;
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 322,.height = 240 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 320,.height = 240 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);

  /* batch of 2: only the second rect is bad, on either side */
  make_pair_n (&src, &dst, 2);
  r2[0] = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 100,.height = 100 };
  r2[1] = (RdsBufSurfTransformRect) {.top = 0,.left = 600,.width = 100,.height = 100 };
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC;
  tp.src_rect = r2;
  tp.dst_rect = NULL;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
  r2[1].left = 540;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  r2[1].left = 300;             /* 300 + 100 > 320 on the dst side */
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  tp.src_rect = NULL;
  tp.dst_rect = r2;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, ROI);
}

/* Rule 7: odd destination rect on 4:2:0, the surface-edge exception, and ALLOW_ODD_CROP. */
static void
test_odd_crop (void)
{
  static const RdsBufSurfTransformRect odd[] = {
    {.top = 0,.left = 1,.width = 10,.height = 10},  /* odd left */
    {.top = 1,.left = 0,.width = 10,.height = 10},  /* odd top */
    {.top = 0,.left = 0,.width = 11,.height = 10},  /* odd interior right edge */
    {.top = 0,.left = 0,.width = 10,.height = 11},  /* odd interior bottom edge */
  };
  static const RdsBufSurfaceColorFormat yuv[] = { RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_YUV420 };
  HandSurface src, dst;
  RdsBufSurfTransformParams tp = { 0 };
  RdsBufSurfTransformRect r, r2[2];

  tp.dst_rect = &r;
  for (guint f = 0; f < G_N_ELEMENTS (yuv); f++) {
    for (guint i = 0; i < G_N_ELEMENTS (odd); i++) {
      hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_RGBA, 64, 64, 1, 1);
      hand_surface_init (&dst, yuv[f], 64, 64, 1, 0);
      r = odd[i];
      tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
      g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
      tp.transform_flag |= RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP;
      g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
    }
  }

  /* An odd right/bottom edge on the surface's own edge counts as even. */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_RGBA, 64, 64, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, 1, 0);
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 37,.height = 29 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  r = (RdsBufSurfTransformRect) {.top = 2,.left = 4,.width = 33,.height = 27 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  /* but not an odd left/top, nor an odd edge short of the surface edge */
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 1,.width = 36,.height = 29 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  r = (RdsBufSurfTransformRect) {.top = 1,.left = 0,.width = 37,.height = 28 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 35,.height = 29 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  r = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 37,.height = 27 };
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);

  /* Odd rects on RGB destinations and odd source rects on 4:2:0 are always fine. */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_NV12, 64, 64, 1, 1);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_BGRx, 64, 64, 1, 0);
  r = odd[0];
  tp.src_rect = &r;
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC | RDS_BUFSURF_TRANSFORM_CROP_DST;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);

  /* batch of 2: only the second dst rect is odd */
  hand_surface_init (&src, RDS_BUF_COLOR_FORMAT_RGBA, 64, 64, 2, 2);
  hand_surface_init (&dst, RDS_BUF_COLOR_FORMAT_NV12, 64, 64, 2, 0);
  r2[0] = (RdsBufSurfTransformRect) {.top = 0,.left = 0,.width = 10,.height = 10 };
  r2[1] = odd[0];
  tp.src_rect = NULL;
  tp.dst_rect = r2;
  tp.transform_flag = RDS_BUFSURF_TRANSFORM_CROP_DST;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  tp.transform_flag |= RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
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
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  tp.transform_filter = (RdsBufSurfTransform_Inter) - 1;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, INVALID);
  for (gint f = 0; f <= 6; f++) {
    tp.transform_filter = (RdsBufSurfTransform_Inter) f;
    g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
  }
  /* without FILTER the field isn't read */
  tp.transform_flag = 0;
  tp.transform_filter = (RdsBufSurfTransform_Inter) 99;
  g_assert_cmpint (validate (&src, &dst, &tp), ==, OK);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/transform/validate/valid", test_valid);
  g_test_add_func ("/transform/validate/1-null-args", test_null_args);
  g_test_add_func ("/transform/validate/2-batch-counts", test_batch_counts);
  g_test_add_func ("/transform/validate/3-unsupported-modes", test_unsupported_modes);
  g_test_add_func ("/transform/validate/4-mem-types", test_mem_types);
  g_test_add_func ("/transform/validate/4b-gpu-id", test_gpu_id);
  g_test_add_func ("/transform/validate/5-formats", test_formats);
  g_test_add_func ("/transform/validate/5b-geometry", test_geometry);
  g_test_add_func ("/transform/validate/6-rects", test_rects);
  g_test_add_func ("/transform/validate/7-odd-crop", test_odd_crop);
  g_test_add_func ("/transform/validate/8-filter", test_filter);
  return g_test_run ();
}
