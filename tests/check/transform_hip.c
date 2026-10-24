/* rds_buf_surf_transform on the GPU, compared with the CPU reference in transform-ref.h. Skips without a ROCm
 * device. Links the static internal library only (for rds_transform_force_kernel).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include <rds/rds-transform-private.h>

#include <string.h>

static const RdsBufSurfaceColorFormat all_formats[] = {
  RDS_BUF_COLOR_FORMAT_GRAY8, RDS_BUF_COLOR_FORMAT_YUV420, RDS_BUF_COLOR_FORMAT_NV12,
  RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_BGRA, RDS_BUF_COLOR_FORMAT_RGBx,
  RDS_BUF_COLOR_FORMAT_BGRx, RDS_BUF_COLOR_FORMAT_RGB, RDS_BUF_COLOR_FORMAT_BGR,
};

static const gchar *
fmt_name (RdsBufSurfaceColorFormat f)
{
  static const gchar *names[] = { "INVALID", "GRAY8", "I420", "NV12", "RGBA", "BGRA", "RGBx", "BGRx", "RGB",
    "BGR"
  };
  return (guint) f < G_N_ELEMENTS (names) ? names[f] : "?";
}

/* ---- surfaces and host snapshots ---- */

static RdsBufSurface *
create_surf (RdsBufSurfaceMemType mt, RdsBufSurfaceColorFormat fmt, guint32 w, guint32 h, guint32 batch,
    guint32 gpu)
{
  RdsBufSurfaceCreateParams p = {
    .gpuId = gpu, .width = w, .height = h, .colorFormat = fmt,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = mt,
  };
  RdsBufSurface *s = NULL;

  g_assert_cmpint (rds_buf_surface_create (&s, batch, &p), ==, 0);
  g_assert_nonnull (s);
  return s;
}


static gboolean
is_yuv420 (RdsBufSurfaceColorFormat f)
{
  return f == RDS_BUF_COLOR_FORMAT_NV12 || f == RDS_BUF_COLOR_FORMAT_YUV420;
}

/* The documented outward alignment of a 4:2:0 destination rect (ALLOW_ODD_CROP), clamped to the surface. */
static RefRect
align_dst (RefRect r, RdsBufSurfaceColorFormat f, guint w, guint h)
{
  RefRect o = r;
  guint right = r.left + r.width, bottom = r.top + r.height;

  if (!is_yuv420 (f))
    return r;
  o.left = r.left & ~1u;
  o.top = r.top & ~1u;
  right = MIN ((right + 1) & ~1u, w);
  bottom = MIN ((bottom + 1) & ~1u, h);
  o.width = right - o.left;
  o.height = bottom - o.top;
  return o;
}

static RdsBufSurfTransformRect
to_rect (RefRect r)
{
  RdsBufSurfTransformRect o = {.top = r.top,.left = r.left,.width = r.width,.height = r.height };
  return o;
}

/* ---- one transform case ---- */

typedef struct
{
  RdsBufSurfaceColorFormat sfmt, dfmt;
  guint sw, sh, dw, dh;
  RdsBufSurfaceMemType smem, dmem;
  guint32 flags;
  RdsBufSurfTransform_Inter filter;
  RefRect srect, drect;         /* used when the matching crop flag is set */
} Case;

static Case
simple_case (RdsBufSurfaceColorFormat sfmt, guint sw, guint sh, RdsBufSurfaceColorFormat dfmt, guint dw,
    guint dh, gboolean bilinear)
{
  Case c = {
    .sfmt = sfmt, .dfmt = dfmt, .sw = sw, .sh = sh, .dw = dw, .dh = dh,
    .smem = RDS_BUF_MEM_DEFAULT, .dmem = RDS_BUF_MEM_DEFAULT,
    .flags = bilinear ? RDS_BUFSURF_TRANSFORM_FILTER : 0,
    .filter = bilinear ? RdsBufSurfTransformInter_Bilinear : RdsBufSurfTransformInter_Nearest,
  };
  return c;
}

static gboolean
filter_is_bilinear (const Case * c)
{
  if (!(c->flags & RDS_BUFSURF_TRANSFORM_FILTER))
    return FALSE;
  return c->filter >= RdsBufSurfTransformInter_Bilinear && c->filter <= RdsBufSurfTransformInter_Algo3;
}

/* Runs @c on the GPU and on the CPU reference; returns the max difference. If @out is not NULL, it receives the
 * GPU result. */
static guint
run_case (const Case * c, HostImage * out)
{
  RdsBufSurface *src = create_surf (c->smem, c->sfmt, c->sw, c->sh, 1, 0);
  RdsBufSurface *dst = create_surf (c->dmem, c->dfmt, c->dw, c->dh, 1, 0);
  RdsBufSurfTransformRect sr = to_rect (c->srect), dr = to_rect (c->drect);
  RdsBufSurfTransformParams tp = {
    .transform_flag = c->flags, .transform_filter = c->filter, .src_rect = &sr, .dst_rect = &dr,
  };
  HostImage hsrc, href, hgot;
  RefRect s = { 0, 0, c->sw, c->sh }, d = { 0, 0, c->dw, c->dh };
  guint m;

  fill_random (src, 0, 1 + c->sfmt * 31 + c->sw);
  fill_random (dst, 0, 7 + c->dfmt * 17 + c->dw);
  src->numFilled = 1;
  snapshot (src, 0, &hsrc);
  snapshot (dst, 0, &href);

  g_assert_cmpint (rds_buf_surf_transform (src, dst, &tp), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpuint (dst->numFilled, ==, 1);
  snapshot (dst, 0, &hgot);

  if (c->flags & RDS_BUFSURF_TRANSFORM_CROP_SRC)
    s = c->srect;
  if (c->flags & RDS_BUFSURF_TRANSFORM_CROP_DST)
    d = align_dst (c->drect, c->dfmt, c->dw, c->dh);
  ref_transform (&hsrc.im, &href.im, s, d, filter_is_bilinear (c));
  m = max_diff (&hgot, &href, TRUE);

  host_image_clear (&hsrc);
  host_image_clear (&href);
  if (out != NULL)
    *out = hgot;
  else
    host_image_clear (&hgot);
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
  return m;
}

static void
check_case (const Case * c, guint tolerance, const gchar * what)
{
  guint m = run_case (c, NULL);

  g_test_message ("%s %s %ux%u -> %s %ux%u: max diff %u", what, fmt_name (c->sfmt), c->sw, c->sh,
      fmt_name (c->dfmt), c->dw, c->dh, m);
  if (m > tolerance)
    g_error ("%s %s %ux%u -> %s %ux%u: max diff %u > %u", what, fmt_name (c->sfmt), c->sw, c->sh,
        fmt_name (c->dfmt), c->dw, c->dh, m, tolerance);
}

/* TRUE when the pair involves no colour maths (copies, repacks, channel reorders, GRAY expansions), so the
 * nearest same-size result must match exactly. */
static gboolean
pair_is_exact (RdsBufSurfaceColorFormat s, RdsBufSurfaceColorFormat d)
{
  RefKind sk = ref_kind (s), dk = ref_kind (d);

  if (sk == REF_KIND_RGB)
    return dk == REF_KIND_RGB;
  if (sk == REF_KIND_YUV420)
    return dk != REF_KIND_RGB;
  return TRUE;                  /* GRAY8 -> anything */
}

/* ---- tests ---- */

static void
test_all_pairs (void)
{
  guint max_exact = 0, max_math = 0;

  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < G_N_ELEMENTS (all_formats); i++) {
    for (guint j = 0; j < G_N_ELEMENTS (all_formats); j++) {
      Case c = simple_case (all_formats[i], 64, 48, all_formats[j], 64, 48, FALSE);
      gboolean exact = pair_is_exact (all_formats[i], all_formats[j]);
      guint m = run_case (&c, NULL);

      if (m > (exact ? 0u : 1u))
        g_error ("%s -> %s nearest 64x48: max diff %u (%s)", fmt_name (all_formats[i]),
            fmt_name (all_formats[j]), m, exact ? "exact required" : "<= 1 required");
      if (exact)
        max_exact = MAX (max_exact, m);
      else
        max_math = MAX (max_math, m);
    }
  }
  g_test_message ("81 pairs: max diff %u (exact pairs), %u (colour maths)", max_exact, max_math);
}

static void
test_scale (void)
{
  static const RdsBufSurfaceColorFormat pairs[][2] = {
    {RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_NV12},
    {RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_RGBA},
    {RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA},
    {RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12},
    {RDS_BUF_COLOR_FORMAT_YUV420, RDS_BUF_COLOR_FORMAT_BGRx},
  };
  static const guint sizes[][2] = { {37, 29}, {130, 98} };

  RDS_TEST_REQUIRE_GPU ();
  for (guint p = 0; p < G_N_ELEMENTS (pairs); p++) {
    for (guint s = 0; s < G_N_ELEMENTS (sizes); s++) {
      for (gint bl = 0; bl <= 1; bl++) {
        Case c = simple_case (pairs[p][0], 64, 48, pairs[p][1], sizes[s][0], sizes[s][1], bl);
        check_case (&c, 1, bl ? "bilinear" : "nearest");
        /* and back from the odd size */
        c = simple_case (pairs[p][0], sizes[s][0], sizes[s][1], pairs[p][1], 64, 48, bl);
        check_case (&c, 1, bl ? "bilinear" : "nearest");
      }
    }
  }
}

/* Odd 4:2:0 surfaces: partial 2x2 blocks on the right and bottom edges. */
static void
test_odd_size (void)
{
  Case c;

  RDS_TEST_REQUIRE_GPU ();
  c = simple_case (RDS_BUF_COLOR_FORMAT_NV12, 37, 29, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, FALSE);
  check_case (&c, 0, "identity");
  c = simple_case (RDS_BUF_COLOR_FORMAT_NV12, 37, 29, RDS_BUF_COLOR_FORMAT_YUV420, 37, 29, FALSE);
  check_case (&c, 0, "repack");
  c = simple_case (RDS_BUF_COLOR_FORMAT_RGBA, 37, 29, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, FALSE);
  check_case (&c, 1, "nearest");
  c = simple_case (RDS_BUF_COLOR_FORMAT_RGBA, 64, 48, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, TRUE);
  check_case (&c, 1, "bilinear");
  c = simple_case (RDS_BUF_COLOR_FORMAT_NV12, 37, 29, RDS_BUF_COLOR_FORMAT_RGBA, 37, 29, FALSE);
  check_case (&c, 1, "nearest");
}

static void
test_crop (void)
{
  static const RdsBufSurfaceColorFormat pairs[][2] = {
    {RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_NV12},
    {RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_RGBA},
    {RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12},
    {RDS_BUF_COLOR_FORMAT_YUV420, RDS_BUF_COLOR_FORMAT_YUV420},
    {RDS_BUF_COLOR_FORMAT_BGR, RDS_BUF_COLOR_FORMAT_RGBx},
  };
  RDS_TEST_REQUIRE_GPU ();
  for (guint p = 0; p < G_N_ELEMENTS (pairs); p++) {
    for (gint bl = 0; bl <= 1; bl++) {
      Case c = simple_case (pairs[p][0], 64, 48, pairs[p][1], 64, 48, bl);

      /* odd source rect only (whole destination) */
      c.flags |= RDS_BUFSURF_TRANSFORM_CROP_SRC;
      c.srect = (RefRect) { 3, 5, 41, 27 };
      check_case (&c, 1, "src-crop");

      /* odd source rect into an odd destination rect, aligned outward on 4:2:0 */
      c.flags |= RDS_BUFSURF_TRANSFORM_CROP_DST | RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP;
      c.drect = (RefRect) { 7, 3, 33, 21 };
      check_case (&c, 1, "src+dst-crop");

      /* same-size crop: nearest is an exact copy of the source rect for same-family pairs */
      c.srect = (RefRect) { 10, 6, 34, 22 };
      c.drect = (RefRect) { 6, 2, 34, 22 };
      check_case (&c, pair_is_exact (c.sfmt, c.dfmt) && !bl ? 0 : 1, "same-size-crop");

      /* same size from an odd source corner: on 4:2:0 this is not a plain plane copy (kernel path) */
      c.srect = (RefRect) { 3, 5, 34, 22 };
      check_case (&c, pair_is_exact (c.sfmt, c.dfmt) && !bl ? 0 : 1, "same-size-odd-corner");
    }
  }
}

/* A destination rect ending on the odd edge of a 4:2:0 surface is valid without ALLOW_ODD_CROP. */
static void
test_crop_odd_edge (void)
{
  Case c;

  RDS_TEST_REQUIRE_GPU ();
  c = simple_case (RDS_BUF_COLOR_FORMAT_RGBA, 64, 48, RDS_BUF_COLOR_FORMAT_NV12, 37, 29, TRUE);
  c.flags |= RDS_BUFSURF_TRANSFORM_CROP_DST;
  c.drect = (RefRect) { 4, 2, 33, 27 };
  check_case (&c, 1, "odd-edge");
  c.sfmt = RDS_BUF_COLOR_FORMAT_NV12;
  check_case (&c, 1, "odd-edge");
}

static void
test_batch (void)
{
  RdsBufSurface *src, *dst;
  RdsBufSurfTransformRect sr[3] = {
    {.top = 0,.left = 0,.width = 64,.height = 48},
    {.top = 5,.left = 3,.width = 31,.height = 23},
    {.top = 10,.left = 20,.width = 44,.height = 30},
  };
  RdsBufSurfTransformRect dr[3] = {
    {.top = 0,.left = 0,.width = 40,.height = 30},
    {.top = 4,.left = 2,.width = 20,.height = 16},
    {.top = 11,.left = 9,.width = 25,.height = 13},
  };
  RdsBufSurfTransformParams tp = {
    .transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC | RDS_BUFSURF_TRANSFORM_CROP_DST
        | RDS_BUFSURF_TRANSFORM_FILTER,
    .transform_filter = RdsBufSurfTransformInter_Bilinear, .src_rect = sr, .dst_rect = dr,
  };
  HostImage hsrc[3], href[3], hgot;

  RDS_TEST_REQUIRE_GPU ();
  src = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_NV12, 64, 48, 3, 0);
  dst = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_RGBA, 40, 30, 4, 0);
  for (guint i = 0; i < 3; i++) {
    fill_random (src, i, 100 + i);
    fill_random (dst, i, 200 + i);
    snapshot (src, i, &hsrc[i]);
    snapshot (dst, i, &href[i]);
  }
  src->numFilled = 3;

  /* an error leaves dst->numFilled alone */
  dr[2].width = 32;
  g_assert_cmpint (rds_buf_surf_transform (src, dst, &tp), ==, RdsBufSurfTransformError_ROI_Error);
  g_assert_cmpuint (dst->numFilled, ==, 0);
  dr[2].width = 25;

  g_assert_cmpint (rds_buf_surf_transform (src, dst, &tp), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpuint (dst->numFilled, ==, 3);
  for (guint i = 0; i < 3; i++) {
    RefRect s = { sr[i].left, sr[i].top, sr[i].width, sr[i].height };
    RefRect d = { dr[i].left, dr[i].top, dr[i].width, dr[i].height };
    guint m;

    ref_transform (&hsrc[i].im, &href[i].im, s, d, TRUE);
    snapshot (dst, i, &hgot);
    m = max_diff (&hgot, &href[i], TRUE);
    g_test_message ("batch surface %u: max diff %u", i, m);
    g_assert_cmpuint (m, <=, 1);
    host_image_clear (&hgot);
    host_image_clear (&hsrc[i]);
    host_image_clear (&href[i]);
  }
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

/* A single-surface view of one slot of a batch is a valid transform destination: each frame lands in its own slot,
 * the view's numFilled becomes 1 and the batch's own numFilled is left alone (the caller sets it after the last
 * slot). The last slot gets a top-left destination rect, so the rest of that slot must keep its old content. */
static void
test_batch_slot_view (void)
{
  static const struct
  {
    RdsBufSurfaceColorFormat fmt;
    guint w, h;
  } srcs[3] = {
    {RDS_BUF_COLOR_FORMAT_NV12, 37, 29},
    {RDS_BUF_COLOR_FORMAT_RGBA, 64, 48},
    {RDS_BUF_COLOR_FORMAT_YUV420, 130, 98},
  };
  const guint bw = 80, bh = 60;
  RdsBufSurface *batch, *src[3];
  RdsBufSurfTransformRect dr = {.top = 0,.left = 0,.width = 80,.height = 46 };
  HostImage hsrc[3], href[3], hgot;

  RDS_TEST_REQUIRE_GPU ();
  batch = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_NV12, bw, bh, 3, 0);
  for (guint i = 0; i < 3; i++) {
    src[i] = create_surf (RDS_BUF_MEM_DEFAULT, srcs[i].fmt, srcs[i].w, srcs[i].h, 1, 0);
    fill_random (src[i], 0, 300 + i);
    src[i]->numFilled = 1;
    snapshot (src[i], 0, &hsrc[i]);
    fill_random (batch, i, 400 + i);
    snapshot (batch, i, &href[i]);
  }
  g_assert_cmpuint (batch->numFilled, ==, 0);

  for (guint i = 0; i < 3; i++) {
    RdsBufSurface view = {
      .gpuId = batch->gpuId, .batchSize = 1, .numFilled = 0, .memType = batch->memType,
      .surfaceList = &batch->surfaceList[i], ._priv = NULL,
    };
    RdsBufSurfTransformParams tp = {
      .transform_flag = RDS_BUFSURF_TRANSFORM_FILTER, .transform_filter = RdsBufSurfTransformInter_Bilinear,
    };
    RefRect s = { 0, 0, srcs[i].w, srcs[i].h }, d = { 0, 0, bw, bh };

    if (i == 2) {
      tp.transform_flag |= RDS_BUFSURF_TRANSFORM_CROP_DST;
      tp.dst_rect = &dr;
      d = (RefRect) { dr.left, dr.top, dr.width, dr.height };
    }
    g_assert_cmpint (rds_buf_surf_transform (src[i], &view, &tp), ==, RdsBufSurfTransformError_Success);
    g_assert_cmpuint (view.numFilled, ==, 1);
    g_assert_cmpuint (batch->numFilled, ==, 0);
    ref_transform (&hsrc[i].im, &href[i].im, s, d, TRUE);
  }

  /* checked after all three: a later slot's transform must not touch an earlier slot */
  for (guint i = 0; i < 3; i++) {
    guint m;

    snapshot (batch, i, &hgot);
    m = max_diff (&hgot, &href[i], TRUE);
    g_test_message ("slot %u (%s %ux%u): max diff %u", i, fmt_name (srcs[i].fmt), srcs[i].w, srcs[i].h, m);
    g_assert_cmpuint (m, <=, 1);
    host_image_clear (&hgot);
    host_image_clear (&hsrc[i]);
    host_image_clear (&href[i]);
    g_assert_cmpint (rds_buf_surface_destroy (src[i]), ==, 0);
  }
  g_assert_cmpint (rds_buf_surface_destroy (batch), ==, 0);
}

static void
test_mem_types (void)
{
  static const RdsBufSurfaceMemType types[] = {
    RDS_BUF_MEM_DEFAULT, RDS_BUF_MEM_HIP_DEVICE, RDS_BUF_MEM_HIP_PINNED, RDS_BUF_MEM_HIP_UNIFIED,
  };

  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < G_N_ELEMENTS (types); i++) {
    for (guint j = 0; j < G_N_ELEMENTS (types); j++) {
      Case c = simple_case (RDS_BUF_COLOR_FORMAT_NV12, 64, 48, RDS_BUF_COLOR_FORMAT_RGBA, 37, 29, TRUE);
      c.smem = types[i];
      c.dmem = types[j];
      check_case (&c, 1, "mem-types");
      /* same-format copy: the fast path, across memory types */
      c = simple_case (RDS_BUF_COLOR_FORMAT_NV12, 64, 48, RDS_BUF_COLOR_FORMAT_NV12, 64, 48, FALSE);
      c.smem = types[i];
      c.dmem = types[j];
      check_case (&c, 0, "mem-types-copy");
    }
  }
}

/* The transform runs on the session stream, after work already queued there. The stream is non-blocking, so it
 * doesn't synchronize with the null stream: a transform launched on the null stream (or synchronizing only that)
 * would read the source before the queued copy lands, or return before its own work is done. The queue ahead of
 * the copy is a long run of fill kernels on a scratch buffer, so the window is wide. */
static void
test_session_stream (void)
{
  const gsize scratch_size = 64 << 20;
  hipStream_t stream = NULL;
  RdsBufSurfTransformConfigParams cfg = {.compute_mode = RdsBufSurfTransformCompute_GPU,.gpu_id = 0 };
  RdsBufSurfTransformParams tp = {.transform_flag = RDS_BUFSURF_TRANSFORM_FILTER,
    .transform_filter = RdsBufSurfTransformInter_Bilinear
  };
  RdsBufSurface *src, *pattern, *dst, *dst2;
  HostImage hpat, href, hgot;
  RefRect s = { 0, 0, 64, 48 }, d = { 0, 0, 130, 98 };
  void *scratch = NULL;

  RDS_TEST_REQUIRE_GPU ();
  src = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_NV12, 64, 48, 1, 0);
  pattern = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_NV12, 64, 48, 1, 0);
  dst = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_BGRA, 130, 98, 1, 0);
  fill_random (src, 0, 11);     /* stale content the transform must not see */
  fill_random (pattern, 0, 12);
  fill_random (dst, 0, 13);
  snapshot (pattern, 0, &hpat);
  snapshot (dst, 0, &href);
  src->numFilled = 1;

  g_assert_cmpint (hipStreamCreateWithFlags (&stream, hipStreamNonBlocking), ==, hipSuccess);
  g_assert_cmpint (hipMalloc (&scratch, scratch_size), ==, hipSuccess);
  cfg.cuda_stream = stream;
  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&cfg), ==, RdsBufSurfTransformError_Success);

  for (guint i = 0; i < 200; i++)
    g_assert_cmpint (rds_transform_kernel_smoke (scratch, scratch_size, (guint8) i, stream), ==, 0);
  g_assert_cmpint (hipMemcpyAsync (src->surfaceList[0].dataPtr, pattern->surfaceList[0].dataPtr,
          src->surfaceList[0].dataSize, hipMemcpyDeviceToDevice, stream), ==, hipSuccess);
  g_assert_cmpint (rds_buf_surf_transform (src, dst, &tp), ==, RdsBufSurfTransformError_Success);
  /* no further synchronization: the transform has synchronized its stream */
  g_assert_cmpint (hipStreamQuery (stream), ==, hipSuccess);

  snapshot (dst, 0, &hgot);
  ref_transform (&hpat.im, &href.im, s, d, TRUE);
  g_assert_cmpuint (max_diff (&hgot, &href, TRUE), <=, 1);
  host_image_clear (&hgot);

  /* Same again through the same-format, same-size fast path (a 2D copy instead of a kernel): it must be queued
   * on the session stream too, behind the source copy. */
  dst2 = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_NV12, 64, 48, 1, 0);
  fill_random (src, 0, 14);     /* stale again */
  fill_random (dst2, 0, 15);
  for (guint i = 0; i < 200; i++)
    g_assert_cmpint (rds_transform_kernel_smoke (scratch, scratch_size, (guint8) i, stream), ==, 0);
  g_assert_cmpint (hipMemcpyAsync (src->surfaceList[0].dataPtr, pattern->surfaceList[0].dataPtr,
          src->surfaceList[0].dataSize, hipMemcpyDeviceToDevice, stream), ==, hipSuccess);
  g_assert_cmpint (rds_buf_surf_transform (src, dst2, &tp), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (hipStreamQuery (stream), ==, hipSuccess);
  snapshot (dst2, 0, &hgot);
  g_assert_cmpuint (max_diff (&hgot, &hpat, TRUE), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (dst2), ==, 0);

  g_assert_cmpint (rds_buf_surf_transform_set_default_session (), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (hipFree (scratch), ==, hipSuccess);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
  host_image_clear (&hgot);
  host_image_clear (&href);
  host_image_clear (&hpat);
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (pattern), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

static void
test_second_gpu (void)
{
  RdsBufSurfTransformConfigParams cfg = {.compute_mode = RdsBufSurfTransformCompute_Default,.gpu_id = 1 };
  RdsBufSurface *src, *dst;
  RdsBufSurfTransformParams tp = { 0 };
  HostImage hsrc, href, hgot;
  RefRect r = { 0, 0, 64, 48 };
  int n = 0;

  RDS_TEST_REQUIRE_GPU ();
  if (hipGetDeviceCount (&n) != hipSuccess || n < 2) {
    g_test_skip ("needs two ROCm devices");
    return;
  }
  src = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_NV12, 64, 48, 1, 1);
  dst = create_surf (RDS_BUF_MEM_DEFAULT, RDS_BUF_COLOR_FORMAT_RGBA, 64, 48, 1, 1);
  fill_random (src, 0, 5);
  fill_random (dst, 0, 6);
  src->numFilled = 1;
  snapshot (src, 0, &hsrc);
  snapshot (dst, 0, &href);

  /* the session is still GPU 0: device memory on GPU 1 is refused */
  g_assert_cmpint (rds_buf_surf_transform (src, dst, &tp), ==, RdsBufSurfTransformError_Invalid_Params);
  g_assert_cmpuint (dst->numFilled, ==, 0);

  g_assert_cmpint (rds_buf_surf_transform_set_session_params (&cfg), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (rds_buf_surf_transform (src, dst, &tp), ==, RdsBufSurfTransformError_Success);
  g_assert_cmpint (rds_buf_surf_transform_set_default_session (), ==, RdsBufSurfTransformError_Success);
  snapshot (dst, 0, &hgot);
  ref_transform (&hsrc.im, &href.im, r, r, FALSE);
  g_assert_cmpuint (max_diff (&hgot, &href, TRUE), <=, 1);

  host_image_clear (&hgot);
  host_image_clear (&hsrc);
  host_image_clear (&href);
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

/* The same-format copy (hipMemcpy2DAsync) and the kernel give byte-identical results. */
static void
test_fast_path (void)
{
  static const RdsBufSurfaceColorFormat fmts[] = { RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_RGBA };

  RDS_TEST_REQUIRE_GPU ();
  for (guint f = 0; f < G_N_ELEMENTS (fmts); f++) {
    for (guint variant = 0; variant < 3; variant++) {
      Case c = simple_case (fmts[f], 64, 48, fmts[f], 64, 48, variant == 2);
      HostImage fast, kernel;

      if (variant >= 1) {
        c.flags |= RDS_BUFSURF_TRANSFORM_CROP_SRC | RDS_BUFSURF_TRANSFORM_CROP_DST;
        c.srect = (RefRect) { 10, 6, 34, 22 };
        c.drect = (RefRect) { 4, 20, 34, 22 };
      }
      if (fmts[f] == RDS_BUF_COLOR_FORMAT_NV12 && variant == 0) {
        c.sw = c.dw = 37;       /* odd 4:2:0 surface: whole-surface copy with a partial chroma column */
        c.sh = c.dh = 29;
      }
      g_assert_cmpuint (run_case (&c, &fast), ==, 0);
      rds_transform_force_kernel (TRUE);
      g_assert_cmpuint (run_case (&c, &kernel), ==, 0);
      rds_transform_force_kernel (FALSE);
      g_assert_cmpuint (max_diff (&fast, &kernel, TRUE), ==, 0);
      host_image_clear (&fast);
      host_image_clear (&kernel);
    }
  }
}

/* Algo1 runs as bilinear; without FILTER the filter field is ignored (nearest). */
static void
test_filters (void)
{
  Case c;
  HostImage algo, bil;

  RDS_TEST_REQUIRE_GPU ();
  c = simple_case (RDS_BUF_COLOR_FORMAT_NV12, 64, 48, RDS_BUF_COLOR_FORMAT_RGBA, 130, 98, TRUE);
  c.filter = RdsBufSurfTransformInter_Algo1;
  g_assert_cmpuint (run_case (&c, &algo), <=, 1);
  c.filter = RdsBufSurfTransformInter_Bilinear;
  g_assert_cmpuint (run_case (&c, &bil), <=, 1);
  g_assert_cmpuint (max_diff (&algo, &bil, TRUE), ==, 0);
  host_image_clear (&algo);
  host_image_clear (&bil);

  c.filter = RdsBufSurfTransformInter_Algo4;    /* nearest */
  check_case (&c, 1, "algo4");
  c.filter = RdsBufSurfTransformInter_Default;  /* nearest */
  check_case (&c, 1, "default");

  c.flags = 0;
  c.filter = (RdsBufSurfTransform_Inter) 99;    /* never read without FILTER */
  check_case (&c, 1, "no-filter-flag");
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/transform/all-pairs", test_all_pairs);
  g_test_add_func ("/gpu/transform/scale", test_scale);
  g_test_add_func ("/gpu/transform/odd-size", test_odd_size);
  g_test_add_func ("/gpu/transform/crop", test_crop);
  g_test_add_func ("/gpu/transform/crop-odd-edge", test_crop_odd_edge);
  g_test_add_func ("/gpu/transform/batch", test_batch);
  g_test_add_func ("/gpu/transform/batch-slot-view", test_batch_slot_view);
  g_test_add_func ("/gpu/transform/mem-types", test_mem_types);
  g_test_add_func ("/gpu/transform/session-stream", test_session_stream);
  g_test_add_func ("/gpu/transform/second-gpu", test_second_gpu);
  g_test_add_func ("/gpu/transform/fast-path", test_fast_path);
  g_test_add_func ("/gpu/transform/filters", test_filters);
  return g_test_run ();
}
