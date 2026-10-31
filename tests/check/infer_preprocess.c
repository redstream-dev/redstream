/* rdsinfer's preprocessing kernel on the GPU, whole slots and source rects, compared with the CPU reference in
 * infer-preprocess-ref.h and with rds_buf_surf_transform (src_rect for the rects) followed by normalisation. Skips
 * without a ROCm device. Links the static internal libgstrds (surfaces, the transform) and the plugin's kernel
 * object; no plugin is loaded.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include "infer-preprocess-ref.h"
#include "rds-infer-preprocess.h"

#include <string.h>

/* The kernel rounds sampled values to bytes like the converter, but in float, and the reference in double: a value
 * within delta of a rounding boundary may legitimately land on the neighbouring byte. Exactness (same bytes as
 * rds_buf_surf_transform) is checked separately by /gpu/preprocess/matches-transform; here the window only has to
 * cover float error.
 * - Nearest: positions are integers (exact). Only the BT.601 conversion is in float: a few operations at magnitude
 *   <= 255 with float-rounded coefficients, ~1e-4; DELTA_NEAREST has a 20x margin.
 * - Bilinear: the position p = (k + 0.5) * (len / dlen) - 0.5 (chroma: half of it) costs one division, one
 *   multiplication and one subtraction in float, so |error of p| <= ~3 * len * 2^-24 (len: the source side, as
 *   the position reaches it): 2.3e-4 at len = 1280, 6e-5 at len = 333, and 0 when len / dlen and the products are
 *   exact (the power-of-two ratios most of the matrix has). The weight carries the same error and multiplies a
 *   neighbour contrast of up to 255, on each of the two axes. A YUV source into RGB adds the conversion gain: an
 *   error in Y and in U reaches B as 1.164 + 2.017. So delta = 2 * 255 * 3 * len * 2^-24 * gain + DELTA_NEAREST,
 *   with gain 3.181 for YUV into RGB/BGR and 1 otherwise (RGB sources, GRAY: the gray weights sum to 1): 0.37 at
 *   worst (NV12 1280x720 into RGB), 0.03 for RGB 333x187. bilinear_delta () computes it per job. */
#define DELTA_NEAREST 0.002
#define TOL 1e-4

static const RdsBufSurfaceColorFormat formats[] = {
  RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_BGRA, RDS_BUF_COLOR_FORMAT_RGBx,
  RDS_BUF_COLOR_FORMAT_BGRx,
};

static const gchar *
fmt_name (RdsBufSurfaceColorFormat f)
{
  switch (f) {
    case RDS_BUF_COLOR_FORMAT_NV12:
      return "NV12";
    case RDS_BUF_COLOR_FORMAT_RGBA:
      return "RGBA";
    case RDS_BUF_COLOR_FORMAT_BGRA:
      return "BGRA";
    case RDS_BUF_COLOR_FORMAT_RGBx:
      return "RGBx";
    case RDS_BUF_COLOR_FORMAT_BGRx:
      return "BGRx";
    default:
      return "?";
  }
}

typedef enum
{
  ASPECT_OFF,
  ASPECT_TOP_LEFT,
  ASPECT_SYMMETRIC,
} Aspect;

static const gchar *aspect_names[] = { "stretch", "top-left", "symmetric" };

static const gchar *color_names[] = { "RGB", "BGR", "GRAY" };

/* One output layout and normalisation; combos[] lists all twelve. */
typedef struct
{
  int color, nhwc;
  gboolean norm;                /* scale 1/255 and the ImageNet offsets, else scale 1 and offsets 0 */
} Combo;

static Combo combos[12];

static void
init_combos (void)
{
  guint i = 0;

  for (int color = 0; color < 3; color++)
    for (int nhwc = 0; nhwc < 2; nhwc++)
      for (int norm = 0; norm < 2; norm++)
        combos[i++] = (Combo) {.color = color,.nhwc = nhwc,.norm = norm };
}

static RdsBufSurface *
create_surf (RdsBufSurfaceColorFormat fmt, guint w, guint h, guint batch)
{
  RdsBufSurfaceCreateParams p = {
    .gpuId = 0, .width = w, .height = h, .colorFormat = fmt,
    .layout = RDS_BUF_LAYOUT_PITCH, .memType = RDS_BUF_MEM_DEFAULT,
  };
  RdsBufSurface *s = NULL;

  g_assert_cmpint (rds_buf_surface_create (&s, batch, &p), ==, 0);
  return s;
}

/* A job for @rect (NULL: all) of slot @slot of @surf into a net_w x net_h input, with the geometry from
 * rds_infer_letterbox. */
static RdsPreprocessJob
make_job_rect (RdsBufSurface * surf, guint slot, const RdsBufSurfTransformRect * rect, guint net_w, guint net_h,
    Aspect aspect, gboolean bilinear, const Combo * cb)
{
  RdsPreprocessJob job;
  RdsInferLetterbox lb;
  const RdsBufSurfaceParams *p = &surf->surfaceList[slot];

  memset (&job, 0, sizeof (job));
  rds_infer_letterbox (net_w, net_h, rect ? rect->width : p->width, rect ? rect->height : p->height,
      aspect != ASPECT_OFF, aspect == ASPECT_SYMMETRIC, &lb);
  g_assert_true (rds_infer_preprocess_job_set_source (&job, p, rect, &lb));
  job.channels = cb->color == RDS_PREPROCESS_GRAY ? 1 : 3;
  job.color = cb->color;
  job.nhwc = cb->nhwc;
  job.bilinear = bilinear;
  job.scale = cb->norm ? 1.0f / 255.0f : 1.0f;
  if (cb->norm) {
    job.offsets[0] = 123.675f;
    job.offsets[1] = 116.28f;
    job.offsets[2] = 103.53f;
  }
  return job;
}

static RdsPreprocessJob
make_job (RdsBufSurface * surf, guint slot, guint net_w, guint net_h, Aspect aspect, gboolean bilinear,
    const Combo * cb)
{
  return make_job_rect (surf, slot, NULL, net_w, net_h, aspect, bilinear, cb);
}

static gsize
image_floats (const RdsPreprocessJob * job)
{
  return (gsize) job->out_w * job->out_h * job->channels;
}

static gsize
tensor_index (const RdsPreprocessJob * job, guint x, guint y, guint c)
{
  if (job->nhwc)
    return ((gsize) y * job->out_w + x) * job->channels + c;
  return (gsize) c * job->out_h * job->out_w + (gsize) y * job->out_w + x;
}

/* Runs @job into image @index of a fresh @n_images tensor, returns the whole tensor (host copy). Every float of
 * the device tensor starts as @sentinel. */
static float *
run_job (const RdsPreprocessJob * job, guint n_images, guint index, float sentinel, hipStream_t stream)
{
  gsize n = image_floats (job) * n_images;
  float *host = g_new (float, n), *dev = NULL;

  for (gsize i = 0; i < n; i++)
    host[i] = sentinel;
  g_assert_cmpint (hipMalloc ((void **) &dev, n * sizeof (float)), ==, hipSuccess);
  g_assert_cmpint (hipMemcpy (dev, host, n * sizeof (float), hipMemcpyHostToDevice), ==, hipSuccess);
  g_assert_cmpint (rds_infer_preprocess_launch (job, dev, index, stream), ==, 0);
  g_assert_cmpint (hipStreamSynchronize (stream), ==, hipSuccess);
  g_assert_cmpint (hipMemcpy (host, dev, n * sizeof (float), hipMemcpyDeviceToHost), ==, hipSuccess);
  g_assert_cmpint (hipFree (dev), ==, hipSuccess);
  return host;
}

/* Unrounded reference samples of one case: rgb (3 per pixel) and gray, for every pixel of the scaled rect. */
typedef struct
{
  double *rgb, *gray;
} RefSamples;

static void
ref_samples_compute (const RefImage * src, const RdsPreprocessJob * job, RefSamples * rs)
{
  gsize n = (gsize) job->dw * job->dh;
  gboolean whole = job->sw == 0;

  rs->rgb = g_new (double, 3 * n);
  rs->gray = g_new (double, n);
  for (guint y = 0; y < job->dh; y++)
    for (guint x = 0; x < job->dw; x++) {
      gsize i = (gsize) y * job->dw + x;

      ref_pre_sample_rect (src, job->dl + x, job->dt + y, job->sx, job->sy, whole ? src->width : job->sw,
          whole ? src->height : job->sh, job->dl, job->dt, job->dw, job->dh, job->bilinear, &rs->rgb[3 * i],
          &rs->gray[i]);
    }
}

static void
ref_samples_clear (RefSamples * rs)
{
  g_free (rs->rgb);
  g_free (rs->gray);
}

typedef struct
{
  double max_diff;
  guint64 values, alt;
} Stats;

static double
bilinear_delta (const RdsPreprocessJob * job)
{
  gboolean yuv = job->src.format == RDS_KFMT_NV12 || job->src.format == RDS_KFMT_YUV420;
  double gain = yuv && job->color != RDS_PREPROCESS_GRAY ? 1.164 + 2.017 : 1.0;
  double len = MAX (job->src.width, job->src.height);

  return 2.0 * 255.0 * 3.0 * len * ldexp (1.0, -24) * gain + DELTA_NEAREST;
}

/* Checks image @out (one image of the tensor) against the reference samples. */
static void
check_image (const RdsPreprocessJob * job, const float *out, const RefSamples * rs, Stats * st)
{
  double delta = job->bilinear ? bilinear_delta (job) : DELTA_NEAREST;

  for (guint y = 0; y < job->out_h; y++) {
    for (guint x = 0; x < job->out_w; x++) {
      gboolean inside = x >= job->dl && x < job->dl + job->dw && y >= job->dt && y < job->dt + job->dh;
      gsize ri = inside ? (gsize) (y - job->dt) * job->dw + (x - job->dl) : 0;

      for (guint c = 0; c < job->channels; c++) {
        float got = out[tensor_index (job, x, y, c)];
        double d;
        gboolean alt = FALSE;

        if (isnan (got)) {
          d = G_MAXDOUBLE;      /* never written: the tensor starts as NaN */
        } else if (!inside) {
          d = fabs ((double) got - (double) job->scale * (0.0 - (double) job->offsets[c]));
        } else {
          /* channel c of RGB is R, G, B; of BGR B, G, R */
          double v = job->color == RDS_PREPROCESS_GRAY ? rs->gray[ri]
              : rs->rgb[3 * ri + (job->color == RDS_PREPROCESS_BGR ? 2 - c : c)];
          d = ref_pre_diff (got, v, delta, job->scale, job->offsets[c], &alt);
        }
        if (d > TOL && st->max_diff <= TOL)
          g_test_message ("first mismatch at (%u, %u) channel %u: got %f (diff %g)%s", x, y, c, got, d,
              inside ? "" : " (padding)");
        st->max_diff = MAX (st->max_diff, d);
        st->values++;
        st->alt += alt;
      }
    }
  }
}

/* One source format and size (@rect of it, NULL: all) into one network, aspect mode and filter, for the given
 * output combos. */
static void
check_case (RdsBufSurfaceColorFormat fmt, guint sw, guint sh, const RdsBufSurfTransformRect * rect, guint net_w,
    guint net_h, Aspect aspect, gboolean bilinear, const Combo * const *cbs, guint n_cbs, hipStream_t stream,
    Stats * total)
{
  RdsBufSurface *src = create_surf (fmt, sw, sh, 1);
  HostImage hsrc;
  RefSamples rs;
  RdsPreprocessJob job0 = make_job_rect (src, 0, rect, net_w, net_h, aspect, bilinear, cbs[0]);
  Stats st = { 0 };

  fill_random (src, 0, 3 + fmt * 7 + sw + sh * 3);
  snapshot (src, 0, &hsrc);
  ref_samples_compute (&hsrc.im, &job0, &rs);

  for (guint i = 0; i < n_cbs; i++) {
    RdsPreprocessJob job = make_job_rect (src, 0, rect, net_w, net_h, aspect, bilinear, cbs[i]);
    /* image 1 of 2, so the image offset is exercised */
    float *t = run_job (&job, 2, 1, NAN, stream);

    check_image (&job, t + image_floats (&job), &rs, &st);
    g_free (t);
    if (st.max_diff > TOL)
      g_test_message ("%s %ux%u (rect %u,%u %ux%u) -> %ux%u %s %s %s %s scale %s: max diff %g", fmt_name (fmt), sw,
          sh, job.sx, job.sy, job.sw, job.sh, net_w, net_h, aspect_names[aspect], bilinear ? "bilinear" : "nearest",
          color_names[cbs[i]->color], cbs[i]->nhwc ? "NHWC" : "NCHW", cbs[i]->norm ? "1/255" : "1", st.max_diff);
    g_assert_cmpfloat (st.max_diff, <=, TOL);
  }
  total->max_diff = MAX (total->max_diff, st.max_diff);
  total->values += st.values;
  total->alt += st.alt;

  ref_samples_clear (&rs);
  host_image_clear (&hsrc);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

static hipStream_t
new_stream (void)
{
  hipStream_t s = NULL;

  g_assert_cmpint (hipStreamCreateWithFlags (&s, hipStreamNonBlocking), ==, hipSuccess);
  return s;
}

static void
report (const gchar * what, guint cases, const Stats * st)
{
  g_test_message ("%s: %u cases, %" G_GUINT64_FORMAT " values, max |diff| %g, %" G_GUINT64_FORMAT
      " values (%.3f%%) on the neighbouring byte", what, cases, st->values, st->max_diff, st->alt,
      st->values ? 100.0 * (double) st->alt / (double) st->values : 0.0);
}

/* Every format, network, aspect mode and filter on a 1280x720 slot, each with all twelve combos (colour, order,
 * normalisation). */
static void
test_matrix_full (void)
{
  static const guint nets[][2] = { {640, 640}, {320, 192} };
  const Combo *cbs[12];
  hipStream_t stream;
  Stats st = { 0 };
  guint cases = 0;

  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < 12; i++)
    cbs[i] = &combos[i];
  stream = new_stream ();
  for (guint f = 0; f < G_N_ELEMENTS (formats); f++)
    for (guint n = 0; n < G_N_ELEMENTS (nets); n++)
      for (int a = ASPECT_OFF; a <= ASPECT_SYMMETRIC; a++)
        for (int bl = 0; bl <= 1; bl++, cases++)
          check_case (formats[f], 1280, 720, NULL, nets[n][0], nets[n][1], (Aspect) a, bl, cbs, 12, stream, &st);
  report ("1280x720, all combos", cases * 12, &st);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
}

/* The other slot sizes (one with odd sides): every format, network, aspect mode and filter, two combos per case,
 * rotating so that every combo runs with every format. */
static void
test_matrix_sizes (void)
{
  static const guint sizes[][2] = { {640, 360}, {333, 187} };
  static const guint nets[][2] = { {640, 640}, {320, 192} };
  hipStream_t stream;
  Stats st = { 0 };
  guint cases = 0, rot = 0;

  RDS_TEST_REQUIRE_GPU ();
  stream = new_stream ();
  for (guint s = 0; s < G_N_ELEMENTS (sizes); s++)
    for (guint f = 0; f < G_N_ELEMENTS (formats); f++)
      for (guint n = 0; n < G_N_ELEMENTS (nets); n++)
        for (int a = ASPECT_OFF; a <= ASPECT_SYMMETRIC; a++)
          for (int bl = 0; bl <= 1; bl++, cases++) {
            const Combo *cbs[2] = { &combos[rot % 12], &combos[(rot + 7) % 12] };

            rot++;
            check_case (formats[f], sizes[s][0], sizes[s][1], NULL, nets[n][0], nets[n][1], (Aspect) a, bl, cbs, 2,
                stream, &st);
          }
  report ("640x360 and 333x187", cases * 2, &st);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
}

/* Source rects, as an object's crop: of a 640x360 slot, odd offsets and sizes, 2x2 at an odd offset, a rect on the
 * right and bottom edges, a tall one on the left edge; of an odd 333x187 slot, a rect on its right and bottom edges
 * (the last, half chroma sample of a 4:2:0 source). */
static const struct
{
  guint sw, sh;
  RdsBufSurfTransformRect rect;
} crops[] = {
  {640, 360, {.left = 37,.top = 23,.width = 101,.height = 67}},
  {640, 360, {.left = 5,.top = 7,.width = 2,.height = 2}},
  {640, 360, {.left = 579,.top = 327,.width = 61,.height = 33}},
  {640, 360, {.left = 0,.top = 1,.width = 31,.height = 359}},
  {333, 187, {.left = 300,.top = 150,.width = 33,.height = 37}},
};

/* Max |diff| of the job for @rect (NULL: all) of a random @fmt sw x sh slot against rds_buf_surf_transform of that
 * rect (CROP_SRC) into a dw x dh RGBA (or GRAY8) surface, normalised; the padding is scale * (0 - offset). */
static double
diff_from_transform (RdsBufSurfaceColorFormat fmt, guint sw, guint sh, const RdsBufSurfTransformRect * rect,
    guint net_w, guint net_h, Aspect aspect, gboolean bilinear, const Combo * cb, guint32 seed, hipStream_t stream)
{
  RdsBufSurface *src = create_surf (fmt, sw, sh, 1), *conv;
  RdsPreprocessJob job = make_job_rect (src, 0, rect, net_w, net_h, aspect, bilinear, cb);
  gboolean gray = cb->color == RDS_PREPROCESS_GRAY;
  RdsBufSurfTransformRect src_rect = rect ? *rect : (RdsBufSurfTransformRect) { 0 };
  RdsBufSurfTransformParams tp = {
    .transform_flag = RDS_BUFSURF_TRANSFORM_FILTER | (rect ? RDS_BUFSURF_TRANSFORM_CROP_SRC : 0),
    .transform_filter = bilinear ? RdsBufSurfTransformInter_Bilinear : RdsBufSurfTransformInter_Nearest,
    .src_rect = rect ? &src_rect : NULL,
  };
  HostImage hc;
  double max_d = 0.0;
  float *t;

  fill_random (src, 0, seed);
  src->numFilled = 1;
  conv = create_surf (gray ? RDS_BUF_COLOR_FORMAT_GRAY8 : RDS_BUF_COLOR_FORMAT_RGBA, job.dw, job.dh, 1);
  g_assert_cmpint (rds_buf_surf_transform (src, conv, &tp), ==, RdsBufSurfTransformError_Success);
  snapshot (conv, 0, &hc);
  t = run_job (&job, 1, 0, NAN, stream);

  for (guint y = 0; y < job.out_h; y++)
    for (guint x = 0; x < job.out_w; x++)
      for (guint c = 0; c < job.channels; c++) {
        gboolean inside = x >= job.dl && x < job.dl + job.dw && y >= job.dt && y < job.dt + job.dh;
        double b = 0.0, d;

        if (inside) {
          const guint8 *px = hc.im.plane[0] + (gsize) (y - job.dt) * hc.im.pitch[0]
              + (gsize) (x - job.dl) * (gray ? 1 : 4);
          b = gray ? px[0] : px[cb->color == RDS_PREPROCESS_BGR ? 2 - c : c];
        }
        d = fabs ((double) t[tensor_index (&job, x, y, c)] - (double) job.scale * (b - (double) job.offsets[c]));
        if (isnan (d))
          d = G_MAXDOUBLE;
        max_d = MAX (max_d, d);
      }

  g_free (t);
  host_image_clear (&hc);
  g_assert_cmpint (rds_buf_surface_destroy (conv), ==, 0);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
  return max_d;
}

/* The property behind the reference: the scaled rect equals rds_buf_surf_transform of the slot into a dw x dh
 * RGBA (or GRAY8) surface, normalised. Same device code, so no rounding slack. */
static void
test_matches_transform (void)
{
  static const struct
  {
    RdsBufSurfaceColorFormat fmt;
    guint sw, sh, net_w, net_h;
    Aspect aspect;
  } cases[] = {
    {RDS_BUF_COLOR_FORMAT_NV12, 333, 187, 320, 192, ASPECT_SYMMETRIC},
    {RDS_BUF_COLOR_FORMAT_BGRx, 333, 187, 320, 192, ASPECT_SYMMETRIC},
    {RDS_BUF_COLOR_FORMAT_RGBA, 333, 187, 320, 192, ASPECT_SYMMETRIC},
    {RDS_BUF_COLOR_FORMAT_NV12, 1280, 720, 640, 640, ASPECT_TOP_LEFT},
    {RDS_BUF_COLOR_FORMAT_RGBA, 1280, 720, 640, 640, ASPECT_OFF},
  };
  hipStream_t stream;
  double max_d = 0.0;

  RDS_TEST_REQUIRE_GPU ();
  stream = new_stream ();
  for (guint f = 0; f < G_N_ELEMENTS (cases); f++)
    for (int bl = 0; bl <= 1; bl++)
      for (guint ci = 0; ci < 12; ci++) {       /* every colour, order and normalisation */
        max_d = MAX (max_d, diff_from_transform (cases[f].fmt, cases[f].sw, cases[f].sh, NULL, cases[f].net_w,
                cases[f].net_h, cases[f].aspect, bl, &combos[ci], 11 + f + bl, stream));
        g_assert_cmpfloat (max_d, <=, TOL);
      }
  g_test_message ("against rds_buf_surf_transform + normalisation: max |diff| %g", max_d);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
}

/* Source rects: the same property with src_rect, for NV12 and RGBA, every aspect mode and filter, into a square
 * and a wide network, three combos per case (rotating, so every combo runs with every format). */
static void
test_crop_matches_transform (void)
{
  static const RdsBufSurfaceColorFormat fmts[] = { RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_RGBA };
  static const guint nets[][2] = { {64, 64}, {96, 48} };
  hipStream_t stream;
  double max_d = 0.0;
  guint rot = 0;

  RDS_TEST_REQUIRE_GPU ();
  stream = new_stream ();
  for (guint f = 0; f < G_N_ELEMENTS (fmts); f++)
    for (guint r = 0; r < G_N_ELEMENTS (crops); r++)
      for (guint n = 0; n < G_N_ELEMENTS (nets); n++)
        for (int a = ASPECT_OFF; a <= ASPECT_SYMMETRIC; a++)
          for (int bl = 0; bl <= 1; bl++)
            for (guint k = 0; k < 3; k++) {
              const Combo *cb = &combos[rot++ % 12];
              const RdsBufSurfTransformRect *rc = &crops[r].rect;
              double d = diff_from_transform (fmts[f], crops[r].sw, crops[r].sh, rc, nets[n][0], nets[n][1],
                  (Aspect) a, bl, cb, 70 + r, stream);

              if (d > TOL)
                g_test_message ("%s %ux%u rect %u,%u %ux%u -> %ux%u %s %s %s %s scale %s: max diff %g",
                    fmt_name (fmts[f]), crops[r].sw, crops[r].sh, rc->left, rc->top, rc->width, rc->height,
                    nets[n][0], nets[n][1],
                    aspect_names[a], bl ? "bilinear" : "nearest", color_names[cb->color],
                    cb->nhwc ? "NHWC" : "NCHW", cb->norm ? "1/255" : "1", d);
              g_assert_cmpfloat (d, <=, TOL);
              max_d = MAX (max_d, d);
            }
  g_test_message ("source rects against rds_buf_surf_transform + normalisation: max |diff| %g", max_d);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
}

/* Source rects against the CPU reference: NV12 and RGBA, every aspect mode and filter, two combos per case. */
static void
test_crop_reference (void)
{
  static const RdsBufSurfaceColorFormat fmts[] = { RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_RGBA };
  static const guint nets[][2] = { {64, 64}, {96, 48} };
  hipStream_t stream;
  Stats st = { 0 };
  guint cases = 0, rot = 0;

  RDS_TEST_REQUIRE_GPU ();
  stream = new_stream ();
  for (guint f = 0; f < G_N_ELEMENTS (fmts); f++)
    for (guint r = 0; r < G_N_ELEMENTS (crops); r++)
      for (guint n = 0; n < G_N_ELEMENTS (nets); n++)
        for (int a = ASPECT_OFF; a <= ASPECT_SYMMETRIC; a++)
          for (int bl = 0; bl <= 1; bl++, cases++) {
            const Combo *cbs[2] = { &combos[rot % 12], &combos[(rot + 5) % 12] };

            rot++;
            check_case (fmts[f], crops[r].sw, crops[r].sh, &crops[r].rect, nets[n][0], nets[n][1], (Aspect) a, bl,
                cbs, 2, stream, &st);
          }
  report ("source rects of 640x360 and 333x187", cases * 2, &st);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
}

/* A batch of four: the launcher for image 1 writes all of image 1 and nothing of images 0, 2 and 3. */
static void
test_batch_isolation (void)
{
  const float sentinel = -12345.5f;
  hipStream_t stream;

  RDS_TEST_REQUIRE_GPU ();
  stream = new_stream ();
  for (guint ci = 0; ci < 12; ci += 3) {
    RdsBufSurface *batch = create_surf (RDS_BUF_COLOR_FORMAT_NV12, 640, 360, 4);
    RdsPreprocessJob job;
    float *t;
    gsize per;

    for (guint i = 0; i < 4; i++)
      fill_random (batch, i, 40 + i);
    /* slot 1 of the batch, as the element will pass it */
    job = make_job (batch, 1, 320, 192, ASPECT_SYMMETRIC, TRUE, &combos[ci]);
    per = image_floats (&job);
    t = run_job (&job, 4, 1, sentinel, stream);
    for (guint img = 0; img < 4; img++) {
      gsize touched = 0;

      for (gsize i = 0; i < per; i++)
        touched += t[img * per + i] != sentinel;
      if (img == 1)
        g_assert_cmpuint (touched, ==, per);
      else
        g_assert_cmpuint (touched, ==, 0);
    }
    g_free (t);
    g_assert_cmpint (rds_buf_surface_destroy (batch), ==, 0);
  }
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
}

/* Bad jobs are refused before anything is launched. */
static void
test_invalid (void)
{
  RdsBufSurface *src;
  RdsPreprocessJob good, job;
  float *dev = NULL;

  RDS_TEST_REQUIRE_GPU ();
  src = create_surf (RDS_BUF_COLOR_FORMAT_RGBA, 64, 48, 1);
  good = make_job (src, 0, 32, 32, ASPECT_SYMMETRIC, TRUE, &combos[0]);
  g_assert_cmpint (hipMalloc ((void **) &dev, image_floats (&good) * sizeof (float)), ==, hipSuccess);
  g_assert_cmpint (rds_infer_preprocess_launch (&good, dev, 0, NULL), ==, 0);
  g_assert_cmpint (rds_infer_preprocess_launch (&good, NULL, 0, NULL), ==, -1);
  g_assert_cmpint (rds_infer_preprocess_launch (NULL, dev, 0, NULL), ==, -1);

  job = good, job.src.data = NULL;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.channels = 1;       /* RGB needs 3 */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.color = RDS_PREPROCESS_GRAY;  /* GRAY needs 1 */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.color = 3;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.dw = 0;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.dl = job.out_w - job.dw + 1;  /* rect past the right edge */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.dt = job.out_h;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.src.width = 0;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.src.format = 0;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.src.pitch[0] = 4 * job.src.width - 1;       /* RGBA row doesn't fit */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.src.pitch[0] = 0;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  /* source rects: inside the slot and non-empty; sw 0 is the whole slot, with sx, sy and sh 0 too */
  job = good, job.sx = 63, job.sy = 47, job.sw = 1, job.sh = 1;
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, 0);
  job = good, job.sx = 1, job.sw = 64, job.sh = 48;     /* past the right edge */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.sy = 47, job.sw = 1, job.sh = 2;      /* past the bottom edge */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.sw = 10;      /* sh 0 */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.sh = 10;      /* sw 0 */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.sx = 3;       /* sw 0 */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  job = good, job.sx = 0xffffffffu, job.sw = 2, job.sh = 1;     /* sx + sw wraps */
  g_assert_cmpint (rds_infer_preprocess_launch (&job, dev, 0, NULL), ==, -1);
  /* 4:2:0 sources need their chroma planes */
  {
    RdsBufSurface *nv12 = create_surf (RDS_BUF_COLOR_FORMAT_NV12, 63, 47, 1);
    RdsBufSurface *i420 = create_surf (RDS_BUF_COLOR_FORMAT_YUV420, 63, 47, 1);
    RdsPreprocessJob j;

    j = make_job (nv12, 0, 32, 32, ASPECT_SYMMETRIC, TRUE, &combos[0]);
    g_assert_cmpint (rds_infer_preprocess_launch (&j, dev, 0, NULL), ==, 0);
    j.src.pitch[1] = 0;
    g_assert_cmpint (rds_infer_preprocess_launch (&j, dev, 0, NULL), ==, -1);
    j = make_job (nv12, 0, 32, 32, ASPECT_SYMMETRIC, TRUE, &combos[0]);
    j.src.pitch[1] = 2 * 32 - 1;        /* 32 interleaved UV pairs per row */
    g_assert_cmpint (rds_infer_preprocess_launch (&j, dev, 0, NULL), ==, -1);
    j = make_job (i420, 0, 32, 32, ASPECT_SYMMETRIC, TRUE, &combos[0]);
    g_assert_cmpint (rds_infer_preprocess_launch (&j, dev, 0, NULL), ==, 0);
    j.src.pitch[2] = 0;
    g_assert_cmpint (rds_infer_preprocess_launch (&j, dev, 0, NULL), ==, -1);
    g_assert_cmpint (hipDeviceSynchronize (), ==, hipSuccess);
    g_assert_cmpint (rds_buf_surface_destroy (nv12), ==, 0);
    g_assert_cmpint (rds_buf_surface_destroy (i420), ==, 0);
  }
  g_assert_cmpint (hipDeviceSynchronize (), ==, hipSuccess);

  /* a letterbox computed for another frame size is refused */
  {
    RdsInferLetterbox lb;

    rds_infer_letterbox (32, 32, 64, 50, TRUE, TRUE, &lb);
    job = good;
    g_assert_false (rds_infer_preprocess_job_set_source (&job, &src->surfaceList[0], NULL, &lb));
    g_assert_true (memcmp (&job, &good, sizeof (job)) == 0);
  }
  /* and one computed for the slot when the source is a rect of it */
  {
    RdsBufSurfTransformRect rect = {.left = 2,.top = 3,.width = 20,.height = 10 };
    RdsInferLetterbox lb;

    rds_infer_letterbox (32, 32, 64, 48, TRUE, TRUE, &lb);
    job = good;
    g_assert_false (rds_infer_preprocess_job_set_source (&job, &src->surfaceList[0], &rect, &lb));
    g_assert_true (memcmp (&job, &good, sizeof (job)) == 0);
    rds_infer_letterbox (32, 32, 20, 10, TRUE, TRUE, &lb);
    g_assert_true (rds_infer_preprocess_job_set_source (&job, &src->surfaceList[0], &rect, &lb));
    g_assert_cmpuint (job.sx, ==, 2);
    g_assert_cmpuint (job.sy, ==, 3);
    g_assert_cmpuint (job.sw, ==, 20);
    g_assert_cmpuint (job.sh, ==, 10);
  }
  g_assert_cmpint (hipFree (dev), ==, hipSuccess);
  g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
}

/* scaling-filter maps as rds_buf_surf_transform maps transform_filter. */
static void
test_filter_mapping (void)
{
  g_assert_false (rds_transform_filter_is_bilinear (RdsBufSurfTransformInter_Nearest));
  g_assert_true (rds_transform_filter_is_bilinear (RdsBufSurfTransformInter_Bilinear));
  g_assert_true (rds_transform_filter_is_bilinear (RdsBufSurfTransformInter_Algo1));
  g_assert_true (rds_transform_filter_is_bilinear (RdsBufSurfTransformInter_Algo2));
  g_assert_true (rds_transform_filter_is_bilinear (RdsBufSurfTransformInter_Algo3));
  g_assert_false (rds_transform_filter_is_bilinear (RdsBufSurfTransformInter_Algo4));
  g_assert_false (rds_transform_filter_is_bilinear (RdsBufSurfTransformInter_Default));
}

static int
cmp_float (const void *a, const void *b)
{
  float x = *(const float *) a, y = *(const float *) b;

  return x < y ? -1 : x > y;
}

/* A 4 x 1280x720 batch into 4 x 3x640x640 (letterboxed, bilinear, normalised, NCHW): the median of 50 timed runs
 * of the four launches, after warm-up. Reported, not asserted. */
static void
time_batch (RdsBufSurfaceColorFormat fmt)
{
  const guint runs = 50;
  RdsBufSurface *batch = create_surf (fmt, 1280, 720, 4);
  hipStream_t stream = new_stream ();
  hipEvent_t e0, e1;
  float *dev = NULL, ms[50];
  RdsPreprocessJob job[4];
  gsize per;

  for (guint i = 0; i < 4; i++) {
    fill_random (batch, i, 90 + i);
    job[i] = make_job (batch, i, 640, 640, ASPECT_SYMMETRIC, TRUE, &combos[1]);
  }
  per = image_floats (&job[0]);
  g_assert_cmpint (hipMalloc ((void **) &dev, 4 * per * sizeof (float)), ==, hipSuccess);
  g_assert_cmpint (hipEventCreate (&e0), ==, hipSuccess);
  g_assert_cmpint (hipEventCreate (&e1), ==, hipSuccess);
  for (guint r = 0; r < 5 + runs; r++) {
    g_assert_cmpint (hipEventRecord (e0, stream), ==, hipSuccess);
    for (guint i = 0; i < 4; i++)
      g_assert_cmpint (rds_infer_preprocess_launch (&job[i], dev, i, stream), ==, 0);
    g_assert_cmpint (hipEventRecord (e1, stream), ==, hipSuccess);
    g_assert_cmpint (hipEventSynchronize (e1), ==, hipSuccess);
    if (r >= 5)
      g_assert_cmpint (hipEventElapsedTime (&ms[r - 5], e0, e1), ==, hipSuccess);
  }
  qsort (ms, runs, sizeof (float), cmp_float);
  g_test_message ("4 x 1280x720 %s -> 4 x 3x640x640: median %.3f ms (min %.3f, max %.3f) over %u runs",
      fmt_name (fmt), ms[runs / 2], ms[0], ms[runs - 1], runs);

  g_assert_cmpint (hipEventDestroy (e0), ==, hipSuccess);
  g_assert_cmpint (hipEventDestroy (e1), ==, hipSuccess);
  g_assert_cmpint (hipFree (dev), ==, hipSuccess);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
  g_assert_cmpint (rds_buf_surface_destroy (batch), ==, 0);
}

static void
test_timing (void)
{
  RDS_TEST_REQUIRE_GPU ();
  time_batch (RDS_BUF_COLOR_FORMAT_NV12);
  time_batch (RDS_BUF_COLOR_FORMAT_RGBA);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  init_combos ();
  g_test_add_func ("/gpu/preprocess/filter-mapping", test_filter_mapping);
  g_test_add_func ("/gpu/preprocess/invalid", test_invalid);
  g_test_add_func ("/gpu/preprocess/matrix-720p", test_matrix_full);
  g_test_add_func ("/gpu/preprocess/matrix-sizes", test_matrix_sizes);
  g_test_add_func ("/gpu/preprocess/matches-transform", test_matches_transform);
  g_test_add_func ("/gpu/preprocess/crop-reference", test_crop_reference);
  g_test_add_func ("/gpu/preprocess/crop-matches-transform", test_crop_matches_transform);
  g_test_add_func ("/gpu/preprocess/batch-isolation", test_batch_isolation);
  g_test_add_func ("/gpu/preprocess/timing", test_timing);
  return g_test_run ();
}
