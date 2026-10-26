/* rds_buf_surf_transform_composite on the GPU: many source surfaces into one destination surface. Each entry must
 * give the bytes rds_buf_surf_transform gives for the same rects, so that call is the reference; solid colours check
 * the conversion on top. Skips without a ROCm device.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include <rds/rds-transform.h>

#include <string.h>

#define W 1280
#define H 720
#define N_SRC 4

/* An RGBA colour, or Y, U, V for a 4:2:0 surface. */
typedef struct
{
  guint8 c[4];
} Px;

static RdsBufSurface *
create_surf (RdsBufSurfaceColorFormat fmt, guint32 w, guint32 h, guint32 batch)
{
  RdsBufSurfaceCreateParams p = {
    .gpuId = 0, .width = w, .height = h, .colorFormat = fmt,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = RDS_BUF_MEM_DEFAULT,
  };
  RdsBufSurface *s = NULL;

  g_assert_cmpint (rds_buf_surface_create (&s, batch, &p), ==, 0);
  s->numFilled = batch;
  return s;
}

static gboolean
is_yuv420 (RdsBufSurfaceColorFormat f)
{
  return f == RDS_BUF_COLOR_FORMAT_NV12 || f == RDS_BUF_COLOR_FORMAT_YUV420;
}

/* Fills the part of surface @idx left of column @split with @a and the rest with @b (RGBA or NV12 only). */
static void
fill_split (RdsBufSurface * s, guint idx, guint split, Px a, Px b)
{
  RdsBufSurfaceParams *p = &s->surfaceList[idx];
  gboolean yuv = is_yuv420 (p->colorFormat);

  g_assert_true (p->colorFormat == RDS_BUF_COLOR_FORMAT_RGBA || p->colorFormat == RDS_BUF_COLOR_FORMAT_NV12);
  g_assert_cmpint (rds_buf_surface_map (s, idx, -1, RDS_BUF_MAP_WRITE), ==, 0);
  for (guint y = 0; y < p->height; y++) {
    guint8 *row = (guint8 *) p->mappedAddr.addr[0] + (gsize) y * p->planeParams.pitch[0];

    for (guint x = 0; x < p->width; x++) {
      const Px *c = x < split ? &a : &b;

      if (yuv)
        row[x] = c->c[0];
      else
        memcpy (row + 4 * x, c->c, 4);
    }
  }
  if (yuv) {
    for (guint y = 0; y < (p->height + 1) / 2; y++) {
      guint8 *row = (guint8 *) p->mappedAddr.addr[1] + (gsize) y * p->planeParams.pitch[1];

      for (guint x = 0; x < (p->width + 1) / 2; x++) {
        const Px *c = 2 * x < split ? &a : &b;

        row[2 * x] = c->c[1];
        row[2 * x + 1] = c->c[2];
      }
    }
  }
  g_assert_cmpint (rds_buf_surface_sync_for_device (s, idx, -1), ==, 0);
  g_assert_cmpint (rds_buf_surface_unmap (s, idx, -1), ==, 0);
}

static void
fill_solid (RdsBufSurface * s, guint idx, Px c)
{
  fill_split (s, idx, G_MAXUINT, c, c);
}

/* The bytes of pixel (x, y): RGBA, or Y and the U, V of its 2x2 block. */
static Px
pixel_at (const HostImage * h, guint x, guint y)
{
  Px p = { {0} };

  if (is_yuv420 (h->im.format)) {
    const guint8 *uv = h->im.plane[1] + (gsize) (y / 2) * h->im.pitch[1] + 2 * (x / 2);

    p.c[0] = h->im.plane[0][(gsize) y * h->im.pitch[0] + x];
    p.c[1] = uv[0];
    p.c[2] = uv[1];
  } else {
    memcpy (p.c, h->im.plane[0] + (gsize) y * h->im.pitch[0] + 4 * x, 4);
  }
  return p;
}

static guint8
store (double v)
{
  return (guint8) CLAMP ((gint) (v + 0.5), 0, 255);
}

/* @c (in @sfmt) converted to @dfmt with the documented BT.601 limited-range rules. */
static Px
convert (Px c, RdsBufSurfaceColorFormat sfmt, RdsBufSurfaceColorFormat dfmt)
{
  Px o = c;

  if (is_yuv420 (sfmt) && !is_yuv420 (dfmt)) {
    double y = c.c[0] - 16.0, u = c.c[1] - 128.0, v = c.c[2] - 128.0;

    o.c[0] = store (1.164 * y + 1.596 * v);
    o.c[1] = store (1.164 * y - 0.392 * u - 0.813 * v);
    o.c[2] = store (1.164 * y + 2.017 * u);
    o.c[3] = 255;
  } else if (!is_yuv420 (sfmt) && is_yuv420 (dfmt)) {
    double r = c.c[0], g = c.c[1], b = c.c[2];

    o.c[0] = store (16 + 0.257 * r + 0.504 * g + 0.098 * b);
    o.c[1] = store (128 - 0.148 * r - 0.291 * g + 0.439 * b);
    o.c[2] = store (128 + 0.439 * r - 0.368 * g - 0.071 * b);
    o.c[3] = 0;
  }
  return o;
}

static gboolean
px_close (Px a, Px b, guint n, guint tol)
{
  for (guint i = 0; i < n; i++)
    if ((guint) ABS ((gint) a.c[i] - (gint) b.c[i]) > tol)
      return FALSE;
  return TRUE;
}

static RdsBufSurfTransformRect
rect (guint left, guint top, guint width, guint height)
{
  RdsBufSurfTransformRect r = {.top = top,.left = left,.width = width,.height = height };
  return r;
}

static gboolean
in_rect (const RdsBufSurfTransformRect * r, guint x, guint y)
{
  return x >= r->left && (guint64) x < (guint64) r->left + r->width
      && y >= r->top && (guint64) y < (guint64) r->top + r->height;
}

/* Every pixel of @r (clipped to the image) is @want within @tol. */
static void
assert_rect_colour (const HostImage * h, RdsBufSurfTransformRect r, Px want, guint tol)
{
  guint n = is_yuv420 (h->im.format) ? 3 : 4;
  guint xe = MIN ((guint64) r.left + r.width, h->im.width), ye = MIN ((guint64) r.top + r.height, h->im.height);

  for (guint y = r.top; y < ye; y++) {
    for (guint x = r.left; x < xe; x++) {
      Px got = pixel_at (h, x, y);

      if (!px_close (got, want, n, tol))
        g_error ("pixel (%u, %u): got %u %u %u %u, want %u %u %u %u (tolerance %u)", x, y, got.c[0], got.c[1],
            got.c[2], got.c[3], want.c[0], want.c[1], want.c[2], want.c[3], tol);
    }
  }
}

/* Every byte outside the @n rects @written equals @before. On 4:2:0, a chroma sample counts as written when any
 * pixel of its 2x2 block is. */
static void
assert_untouched (const HostImage * got, const HostImage * before, const RdsBufSurfTransformRect * written, guint n)
{
  gboolean yuv = is_yuv420 (got->im.format);

  for (guint y = 0; y < got->im.height; y++) {
    for (guint x = 0; x < got->im.width; x++) {
      gboolean px_written = FALSE, block_written = FALSE;
      Px a = pixel_at (got, x, y), b = pixel_at (before, x, y);

      for (guint i = 0; i < n; i++) {
        px_written |= in_rect (&written[i], x, y);
        for (guint by = y & ~1u; by < (y | 1u) + 1; by++)
          for (guint bx = x & ~1u; bx < (x | 1u) + 1; bx++)
            block_written |= in_rect (&written[i], bx, by);
      }
      if (!px_written && a.c[0] != b.c[0])
        g_error ("pixel (%u, %u) outside the rects was written: %u, was %u", x, y, a.c[0], b.c[0]);
      if (yuv && !block_written && (a.c[1] != b.c[1] || a.c[2] != b.c[2]))
        g_error ("chroma of (%u, %u) outside the rects was written", x, y);
      if (!yuv && !px_written && memcmp (a.c, b.c, 4) != 0)
        g_error ("pixel (%u, %u) outside the rects was written", x, y);
    }
  }
}

/* ---- sources: N_SRC single surfaces, and one hand-built batch over them (mixed formats) ---- */

typedef struct
{
  RdsBufSurface *single[N_SRC];
  RdsBufSurfaceParams list[N_SRC];
  RdsBufSurface batch;
} Sources;

static void
sources_init (Sources * s, const RdsBufSurfaceColorFormat * fmts, guint w, guint h)
{
  memset (s, 0, sizeof (*s));
  for (guint i = 0; i < N_SRC; i++)
    s->single[i] = create_surf (fmts[i], w, h, 1);
  s->batch.gpuId = 0;
  s->batch.batchSize = N_SRC;
  s->batch.numFilled = N_SRC;
  s->batch.memType = RDS_BUF_MEM_DEFAULT;
  s->batch.surfaceList = s->list;
}

/* Copies the surfaces' params into the batch: call after the fills (maps leave mappedAddr cleared). */
static void
sources_sync (Sources * s)
{
  for (guint i = 0; i < N_SRC; i++)
    s->list[i] = s->single[i]->surfaceList[0];
}

static void
sources_clear (Sources * s)
{
  for (guint i = 0; i < N_SRC; i++)
    g_assert_cmpint (rds_buf_surface_destroy (s->single[i]), ==, 0);
}

static const RdsBufSurfaceColorFormat mixed[N_SRC] = {
  RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12,
};

/* RGBA colours for the RGBA sources, Y U V for the NV12 ones */
static const Px colours[N_SRC] = {
  {{200, 40, 60, 255}}, {{120, 90, 200, 0}}, {{30, 180, 90, 128}}, {{60, 170, 100, 0}},
};

static RdsBufSurfTransformCompositeParams
composite_params (RdsBufSurfTransformRect * src_rects, RdsBufSurfTransformRect * dst_rects,
    RdsBufSurfTransform_Inter filter)
{
  RdsBufSurfTransformCompositeParams cp = {
    .composite_flag = RDS_BUFSURF_TRANSFORM_COMPOSITE | RDS_BUFSURF_TRANSFORM_COMPOSITE_FILTER,
    .input_buf_count = N_SRC, .src_comp_rect = src_rects, .dst_comp_rect = dst_rects,
    .composite_filter = filter,
  };
  return cp;
}

/* The reference: each entry transformed on its own (rds_buf_surf_transform) into @ref, in order. */
static void
reference (Sources * s, guint n, RdsBufSurface * ref, const RdsBufSurfTransformRect * src_rects,
    const RdsBufSurfTransformRect * dst_rects, RdsBufSurfTransform_Inter filter)
{
  for (guint i = 0; i < n; i++) {
    RdsBufSurfTransformRect sr = src_rects[i], dr = dst_rects[i];
    RdsBufSurfTransformParams tp = {
      .transform_flag = RDS_BUFSURF_TRANSFORM_CROP_SRC | RDS_BUFSURF_TRANSFORM_CROP_DST
          | RDS_BUFSURF_TRANSFORM_FILTER,
      .transform_filter = filter, .src_rect = &sr, .dst_rect = &dr,
    };

    g_assert_cmpint (rds_buf_surf_transform (s->single[i], ref, &tp), ==, RdsBufSurfTransformError_Success);
  }
}

static void
grid_rects (RdsBufSurfTransformRect * src_rects, RdsBufSurfTransformRect * dst_rects, guint cw, guint ch,
    guint gap)
{
  for (guint i = 0; i < N_SRC; i++) {
    src_rects[i] = rect (0, 0, W, H);
    dst_rects[i] = rect ((i % 2) * (W / 2), (i / 2) * (H / 2), cw - gap, ch - gap);
  }
}

/* 4 solid sources (RGBA and NV12) into a 2x2 grid of an RGBA and of an NV12 destination: each cell is its colour,
 * cell borders are exact, and the bytes equal the per-entry transform. The second run uses its own stream. */
static void
test_grid (void)
{
  static const RdsBufSurfaceColorFormat dfmts[] = { RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12 };
  Sources s;

  RDS_TEST_REQUIRE_GPU ();
  sources_init (&s, mixed, W, H);
  for (guint i = 0; i < N_SRC; i++)
    fill_solid (s.single[i], 0, colours[i]);
  sources_sync (&s);

  for (guint d = 0; d < G_N_ELEMENTS (dfmts); d++) {
    RdsBufSurface *dst = create_surf (dfmts[d], W, H, 1), *ref = create_surf (dfmts[d], W, H, 1);
    RdsBufSurfTransformRect sr[N_SRC], dr[N_SRC];
    RdsBufSurfTransformCompositeParams cp = composite_params (sr, dr, RdsBufSurfTransformInter_Bilinear);
    RdsBufSurfTransformConfigParams session = {.compute_mode = RdsBufSurfTransformCompute_GPU };
    HostImage got, want;
    hipStream_t stream = NULL;

    grid_rects (sr, dr, W / 2, H / 2, 0);
    fill_random (dst, 0, 11 + d);
    fill_random (ref, 0, 11 + d);
    if (d == 1) {
      g_assert_cmpint (hipStreamCreateWithFlags (&stream, hipStreamNonBlocking), ==, hipSuccess);
      session.cuda_stream = stream;
      g_assert_cmpint (rds_buf_surf_transform_set_session_params (&session), ==,
          RdsBufSurfTransformError_Success);
    }
    g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==, RdsBufSurfTransformError_Success);
    g_assert_cmpint (rds_buf_surf_transform_set_default_session (), ==, RdsBufSurfTransformError_Success);
    if (stream != NULL)
      g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
    reference (&s, N_SRC, ref, sr, dr, RdsBufSurfTransformInter_Bilinear);

    snapshot (dst, 0, &got);
    snapshot (ref, 0, &want);
    g_assert_cmpuint (max_diff (&got, &want, TRUE), ==, 0);
    for (guint i = 0; i < N_SRC; i++) {
      Px c = convert (colours[i], mixed[i], dfmts[d]);

      if (!is_yuv420 (dfmts[d]) && is_yuv420 (mixed[i]))
        c.c[3] = 255;
      assert_rect_colour (&got, dr[i], c, mixed[i] == dfmts[d] ? 0 : 1);
    }
    host_image_clear (&got);
    host_image_clear (&want);
    g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
    g_assert_cmpint (rds_buf_surface_destroy (ref), ==, 0);
  }
  sources_clear (&s);
}

/* Source rects: one quadrant of a four-colour source fills a whole cell; odd source rects on random content with
 * either filter match the per-entry transform. */
static void
test_src_crop (void)
{
  static const RdsBufSurfaceColorFormat dfmts[] = { RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12 };
  static const RdsBufSurfTransform_Inter filters[] = {
    RdsBufSurfTransformInter_Nearest, RdsBufSurfTransformInter_Bilinear
  };
  Sources s;

  RDS_TEST_REQUIRE_GPU ();
  sources_init (&s, mixed, W, H);

  /* left half colour 0, right half colour 2: the right half's quadrant is all colour 2 */
  {
    RdsBufSurface *dst = create_surf (RDS_BUF_COLOR_FORMAT_RGBA, W, H, 1);
    RdsBufSurfTransformRect sr = rect (W / 2, H / 2, W / 2, H / 2), dr = rect (0, 0, 333, 211);
    RdsBufSurfTransformCompositeParams cp = composite_params (&sr, &dr, RdsBufSurfTransformInter_Bilinear);
    HostImage got;

    fill_split (s.single[0], 0, W / 2, colours[0], colours[2]);
    sources_sync (&s);
    s.batch.numFilled = 1;
    g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==, RdsBufSurfTransformError_Success);
    s.batch.numFilled = N_SRC;
    snapshot (dst, 0, &got);
    assert_rect_colour (&got, dr, colours[2], 0);
    host_image_clear (&got);
    g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  }

  for (guint i = 0; i < N_SRC; i++)
    fill_random (s.single[i], 0, 100 + i);
  sources_sync (&s);
  for (guint d = 0; d < G_N_ELEMENTS (dfmts); d++) {
    for (guint f = 0; f < G_N_ELEMENTS (filters); f++) {
      RdsBufSurface *dst = create_surf (dfmts[d], W, H, 1), *ref = create_surf (dfmts[d], W, H, 1);
      RdsBufSurfTransformRect sr[N_SRC] = {
        rect (13, 7, 301, 211), rect (1, 1, 1279, 719), rect (640, 360, 640, 360), rect (101, 51, 33, 17),
      };
      RdsBufSurfTransformRect dr[N_SRC];
      RdsBufSurfTransformCompositeParams cp = composite_params (sr, dr, filters[f]);
      HostImage got, want;

      for (guint i = 0; i < N_SRC; i++)
        dr[i] = rect ((i % 2) * (W / 2), (i / 2) * (H / 2), W / 2, H / 2);
      fill_random (dst, 0, 21);
      fill_random (ref, 0, 21);
      g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
          RdsBufSurfTransformError_Success);
      reference (&s, N_SRC, ref, sr, dr, filters[f]);
      snapshot (dst, 0, &got);
      snapshot (ref, 0, &want);
      g_assert_cmpuint (max_diff (&got, &want, TRUE), ==, 0);
      host_image_clear (&got);
      host_image_clear (&want);
      g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
      g_assert_cmpint (rds_buf_surface_destroy (ref), ==, 0);
    }
  }
  sources_clear (&s);
}

/* Destination pixels outside the dst rects keep their bytes; only slot 0 of a batch is written and numFilled is left
 * as it was; input_buf_count is ignored (src->numFilled decides). */
static void
test_untouched (void)
{
  static const RdsBufSurfaceColorFormat dfmts[] = { RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12 };
  static const guint counts[] = { 0, 2, N_SRC, 7 };
  Sources s;

  RDS_TEST_REQUIRE_GPU ();
  sources_init (&s, mixed, W, H);
  for (guint i = 0; i < N_SRC; i++)
    fill_solid (s.single[i], 0, colours[i]);
  sources_sync (&s);

  for (guint d = 0; d < G_N_ELEMENTS (dfmts); d++) {
    for (guint k = 0; k < G_N_ELEMENTS (counts); k++) {
      RdsBufSurface *dst = create_surf (dfmts[d], W, H, 2);
      RdsBufSurfTransformRect sr[N_SRC], dr[N_SRC];
      RdsBufSurfTransformCompositeParams cp = composite_params (sr, dr, RdsBufSurfTransformInter_Nearest);
      HostImage before, before1, got, got1;
      guint filled = k == 1 ? 3 : N_SRC;       /* one run with fewer filled sources than rects */

      grid_rects (sr, dr, W / 2, H / 2, 40);
      cp.input_buf_count = counts[k];
      cp.composite_flag = k;                    /* any flags */
      fill_random (dst, 0, 31);
      fill_random (dst, 1, 32);
      snapshot (dst, 0, &before);
      snapshot (dst, 1, &before1);
      s.batch.numFilled = filled;
      g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
          RdsBufSurfTransformError_Success);
      s.batch.numFilled = N_SRC;
      g_assert_cmpuint (dst->numFilled, ==, 2);
      snapshot (dst, 0, &got);
      snapshot (dst, 1, &got1);
      assert_untouched (&got, &before, dr, filled);
      g_assert_cmpuint (max_diff (&got1, &before1, TRUE), ==, 0);
      for (guint i = 0; i < filled; i++) {
        Px c = convert (colours[i], mixed[i], dfmts[d]);

        if (!is_yuv420 (dfmts[d]) && is_yuv420 (mixed[i]))
          c.c[3] = 255;
        assert_rect_colour (&got, dr[i], c, 1);
      }
      host_image_clear (&before);
      host_image_clear (&before1);
      host_image_clear (&got);
      host_image_clear (&got1);
      g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
    }
  }
  sources_clear (&s);
}

/* Overlapping dst rects: later entries are drawn on top. */
static void
test_overlap (void)
{
  static const RdsBufSurfaceColorFormat rgba[N_SRC] = {
    RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA,
  };
  static const Px c[N_SRC] = { {{255, 0, 0, 255}}, {{0, 255, 0, 255}}, {{0, 0, 255, 255}}, {{9, 9, 9, 9}} };
  RdsBufSurface *dst;
  RdsBufSurfTransformRect sr[N_SRC], dr[N_SRC] = {
    rect (0, 0, 800, 500), rect (400, 200, 800, 500), rect (100, 100, 200, 200), rect (0, 0, 1, 1),
  };
  RdsBufSurfTransformCompositeParams cp = composite_params (sr, dr, RdsBufSurfTransformInter_Nearest);
  HostImage got;
  Sources s;

  RDS_TEST_REQUIRE_GPU ();
  sources_init (&s, rgba, 64, 64);
  for (guint i = 0; i < N_SRC; i++) {
    fill_solid (s.single[i], 0, c[i]);
    sr[i] = rect (0, 0, 64, 64);
  }
  sources_sync (&s);
  s.batch.numFilled = 3;
  dst = create_surf (RDS_BUF_COLOR_FORMAT_RGBA, W, H, 1);
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==, RdsBufSurfTransformError_Success);
  snapshot (dst, 0, &got);
  assert_rect_colour (&got, rect (0, 0, 400, 100), c[0], 0);
  assert_rect_colour (&got, rect (400, 200, 800, 500), c[1], 0);   /* 1 over 0 */
  assert_rect_colour (&got, rect (100, 100, 200, 200), c[2], 0);   /* 2 over 0 */
  assert_rect_colour (&got, rect (300, 100, 100, 100), c[0], 0);
  host_image_clear (&got);
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  sources_clear (&s);
}

/* Rects reaching past their surface are clipped, not errors (ours: the destination keeps the full rect's mapping and
 * only its visible part is drawn; a source rect is cut to the source surface). A rect wholly outside draws nothing,
 * as does an empty one. */
static void
test_clip (void)
{
  static const RdsBufSurfaceColorFormat dfmts[] = { RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_NV12 };
  static const RdsBufSurfaceColorFormat rgba[N_SRC] = {
    RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA,
  };
  static const Px a = {{250, 20, 20, 255}}, b = {{20, 20, 250, 255}};
  Sources s;

  RDS_TEST_REQUIRE_GPU ();
  sources_init (&s, rgba, 640, 360);
  for (guint i = 0; i < N_SRC; i++)
    fill_split (s.single[i], 0, 320, a, b);    /* left half a, right half b */
  sources_sync (&s);

  for (guint d = 0; d < G_N_ELEMENTS (dfmts); d++) {
    RdsBufSurface *dst = create_surf (dfmts[d], W, H, 1);
    RdsBufSurfTransformRect sr[N_SRC] = {
      rect (0, 0, 640, 360), rect (320, 0, 640, 360), rect (0, 0, 640, 360), rect (0, 0, 640, 360),
    };
    /* 0: past the bottom-right corner (odd origin on NV12: rounded up); 1: from a source rect past the right edge;
     * 2: wholly outside; 3: empty */
    RdsBufSurfTransformRect dr[N_SRC] = {
      rect (1001, 601, 640, 360), rect (0, 0, 320, 180), rect (W, 0, 100, 100), rect (500, 500, 0, 10),
    };
    RdsBufSurfTransformRect drawn[2] = { rect (1002, 602, W - 1002, H - 602), rect (0, 0, 320, 180) };
    RdsBufSurfTransformCompositeParams cp = composite_params (sr, dr, RdsBufSurfTransformInter_Nearest);
    HostImage before, got;

    if (!is_yuv420 (dfmts[d]))
      drawn[0] = rect (1001, 601, W - 1001, H - 601);
    fill_random (dst, 0, 41);
    snapshot (dst, 0, &before);
    g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==, RdsBufSurfTransformError_Success);
    snapshot (dst, 0, &got);
    /* entry 0 shows only the left part of its source (its visible 279 columns are source columns 0..278) */
    assert_rect_colour (&got, drawn[0], convert (a, RDS_BUF_COLOR_FORMAT_RGBA, dfmts[d]), 1);
    /* entry 1 samples only source columns 320..639 */
    assert_rect_colour (&got, drawn[1], convert (b, RDS_BUF_COLOR_FORMAT_RGBA, dfmts[d]), 1);
    assert_untouched (&got, &before, drawn, 2);
    host_image_clear (&before);
    host_image_clear (&got);

    /* a rect larger than the largest surface is ROI_Error (ours), nothing written */
    dr[2] = rect (0, 0, RDS_BUF_SURFACE_MAX_DIM + 1, 100);
    snapshot (dst, 0, &before);
    g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==, RdsBufSurfTransformError_ROI_Error);
    snapshot (dst, 0, &got);
    g_assert_cmpuint (max_diff (&got, &before, TRUE), ==, 0);
    host_image_clear (&before);
    host_image_clear (&got);
    g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  }
  sources_clear (&s);
}

/* An odd origin on a 4:2:0 destination is rounded up to even and an odd right or bottom edge rounded down (unless
 * on the surface edge), DeepStream 8.0 behaviour; the source is scaled onto that rect. RGB destinations take odd
 * rects as given (ours). */
static void
test_odd_origin (void)
{
  Sources s;

  RDS_TEST_REQUIRE_GPU ();
  sources_init (&s, mixed, W, H);
  for (guint i = 0; i < N_SRC; i++)
    fill_random (s.single[i], 0, 50 + i);
  sources_sync (&s);
  s.batch.numFilled = 2;

  for (guint d = 0; d < 2; d++) {
    RdsBufSurfaceColorFormat fmt = d == 0 ? RDS_BUF_COLOR_FORMAT_NV12 : RDS_BUF_COLOR_FORMAT_RGBA;
    RdsBufSurface *dst = create_surf (fmt, W, H, 1), *ref = create_surf (fmt, W, H, 1);
    RdsBufSurfTransformRect sr[N_SRC] = { rect (0, 0, W, H), rect (7, 3, 501, 301) };
    RdsBufSurfTransformRect dr[N_SRC] = { rect (101, 51, 333, 201), rect (601, 401, 101, 51) };
    /* NV12: [102, 434) x [52, 252) and [602, 702) x [402, 452) */
    RdsBufSurfTransformRect want[N_SRC] = { rect (102, 52, 332, 200), rect (602, 402, 100, 50) };
    RdsBufSurfTransformCompositeParams cp = composite_params (sr, dr, RdsBufSurfTransformInter_Bilinear);
    HostImage got, exp;

    if (d == 1)
      memcpy (want, dr, sizeof (want));
    fill_random (dst, 0, 61);
    fill_random (ref, 0, 61);
    g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==, RdsBufSurfTransformError_Success);
    reference (&s, 2, ref, sr, want, RdsBufSurfTransformInter_Bilinear);
    snapshot (dst, 0, &got);
    snapshot (ref, 0, &exp);
    g_assert_cmpuint (max_diff (&got, &exp, TRUE), ==, 0);
    host_image_clear (&got);
    host_image_clear (&exp);
    g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
    g_assert_cmpint (rds_buf_surface_destroy (ref), ==, 0);
  }
  sources_clear (&s);
}

/* Invalid arguments return an error and leave the destination untouched. */
static void
test_invalid (void)
{
  RdsBufSurface *dst, *sys;
  RdsBufSurfTransformRect sr[N_SRC], dr[N_SRC];
  RdsBufSurfTransformCompositeParams cp = composite_params (sr, dr, RdsBufSurfTransformInter_Nearest);
  RdsBufSurfaceCreateParams sys_params = {
    .width = 64, .height = 64, .colorFormat = RDS_BUF_COLOR_FORMAT_RGBA, .memType = RDS_BUF_MEM_SYSTEM,
  };
  HostImage before, got;
  Sources s;

  RDS_TEST_REQUIRE_GPU ();
  sources_init (&s, mixed, 64, 64);
  for (guint i = 0; i < N_SRC; i++)
    fill_random (s.single[i], 0, 70 + i);
  sources_sync (&s);
  dst = create_surf (RDS_BUF_COLOR_FORMAT_RGBA, 128, 128, 1);
  fill_random (dst, 0, 71);
  snapshot (dst, 0, &before);
  for (guint i = 0; i < N_SRC; i++) {
    sr[i] = rect (0, 0, 64, 64);
    dr[i] = rect ((i % 2) * 64, (i / 2) * 64, 64, 64);
  }

  /* NULL rect arrays (DeepStream 8.0 returns Invalid_Params too) */
  cp.src_comp_rect = NULL;
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
      RdsBufSurfTransformError_Invalid_Params);
  cp.dst_comp_rect = NULL;
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
      RdsBufSurfTransformError_Invalid_Params);
  cp.src_comp_rect = sr;
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
      RdsBufSurfTransformError_Invalid_Params);
  cp.dst_comp_rect = dr;

  /* no filled source */
  s.batch.numFilled = 0;
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
      RdsBufSurfTransformError_Invalid_Params);
  s.batch.numFilled = N_SRC;

  /* filter out of range */
  cp.composite_filter = (RdsBufSurfTransform_Inter) 99;
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
      RdsBufSurfTransformError_Invalid_Params);
  cp.composite_filter = RdsBufSurfTransformInter_Nearest;

  /* a source sharing the destination's pixel memory */
  s.list[2].dataPtr = dst->surfaceList[0].dataPtr;
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
      RdsBufSurfTransformError_Invalid_Params);
  s.list[2].dataPtr = NULL;
  g_assert_cmpint (rds_buf_surf_transform_composite (&s.batch, dst, &cp), ==,
      RdsBufSurfTransformError_Invalid_Params);
  sources_sync (&s);

  /* system memory */
  g_assert_cmpint (rds_buf_surface_create (&sys, 1, &sys_params), ==, 0);
  sys->numFilled = 1;
  g_assert_cmpint (rds_buf_surf_transform_composite (sys, dst, &cp), ==, RdsBufSurfTransformError_Unsupported);
  g_assert_cmpint (rds_buf_surface_destroy (sys), ==, 0);

  snapshot (dst, 0, &got);
  g_assert_cmpuint (max_diff (&got, &before, TRUE), ==, 0);
  g_assert_cmpuint (dst->numFilled, ==, 1);
  host_image_clear (&before);
  host_image_clear (&got);
  g_assert_cmpint (rds_buf_surface_destroy (dst), ==, 0);
  sources_clear (&s);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/composite/grid", test_grid);
  g_test_add_func ("/gpu/composite/src-crop", test_src_crop);
  g_test_add_func ("/gpu/composite/untouched", test_untouched);
  g_test_add_func ("/gpu/composite/overlap", test_overlap);
  g_test_add_func ("/gpu/composite/clip", test_clip);
  g_test_add_func ("/gpu/composite/odd-origin", test_odd_origin);
  g_test_add_func ("/gpu/composite/invalid", test_invalid);
  return g_test_run ();
}
