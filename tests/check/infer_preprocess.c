/* rdsinfer's preprocessing kernel on the GPU, compared with the CPU reference in infer-preprocess-ref.h and with
 * rds_buf_surf_transform followed by normalisation. Skips without a ROCm device. Links the static internal
 * libgstrds (surfaces, the transform) and the plugin's kernel object; no plugin is loaded.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include "infer-preprocess-ref.h"
#include "rds-infer-preprocess.h"

#include <string.h>

/* The kernel rounds sampled values to bytes like the converter, in float: a value this close to a rounding
 * boundary may round either way (bilinear positions and weights carry float error, up to ~3e-4 of a step between
 * neighbours up to 255 apart). Nearest positions are exact; only the YUV conversion is in float there. */
#define DELTA_NEAREST 0.002
#define DELTA_BILINEAR 0.08
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

/* A job for slot @slot of @surf into a net_w x net_h input, with the geometry from rds_infer_letterbox. */
static RdsPreprocessJob
make_job (RdsBufSurface * surf, guint slot, guint net_w, guint net_h, Aspect aspect, gboolean bilinear,
    const Combo * cb)
{
  RdsPreprocessJob job;
  RdsInferLetterbox lb;
  const RdsBufSurfaceParams *p = &surf->surfaceList[slot];

  memset (&job, 0, sizeof (job));
  rds_infer_letterbox (net_w, net_h, p->width, p->height, aspect != ASPECT_OFF, aspect == ASPECT_SYMMETRIC, &lb);
  g_assert_true (rds_infer_preprocess_job_set_source (&job, p, &lb));
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

  rs->rgb = g_new (double, 3 * n);
  rs->gray = g_new (double, n);
  for (guint y = 0; y < job->dh; y++)
    for (guint x = 0; x < job->dw; x++) {
      gsize i = (gsize) y * job->dw + x;

      ref_pre_sample (src, job->dl + x, job->dt + y, job->dl, job->dt, job->dw, job->dh, job->bilinear,
          &rs->rgb[3 * i], &rs->gray[i]);
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

/* Checks image @out (one image of the tensor) against the reference samples. */
static void
check_image (const RdsPreprocessJob * job, const float *out, const RefSamples * rs, Stats * st)
{
  double delta = job->bilinear ? DELTA_BILINEAR : DELTA_NEAREST;

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

/* One source format and size into one network, aspect mode and filter, for the given output combos. */
static void
check_case (RdsBufSurfaceColorFormat fmt, guint sw, guint sh, guint net_w, guint net_h, Aspect aspect,
    gboolean bilinear, const Combo * const *cbs, guint n_cbs, hipStream_t stream, Stats * total)
{
  RdsBufSurface *src = create_surf (fmt, sw, sh, 1);
  HostImage hsrc;
  RefSamples rs;
  RdsPreprocessJob job0 = make_job (src, 0, net_w, net_h, aspect, bilinear, cbs[0]);
  Stats st = { 0 };

  fill_random (src, 0, 3 + fmt * 7 + sw + sh * 3);
  snapshot (src, 0, &hsrc);
  ref_samples_compute (&hsrc.im, &job0, &rs);

  for (guint i = 0; i < n_cbs; i++) {
    RdsPreprocessJob job = make_job (src, 0, net_w, net_h, aspect, bilinear, cbs[i]);
    /* image 1 of 2, so the image offset is exercised */
    float *t = run_job (&job, 2, 1, NAN, stream);

    check_image (&job, t + image_floats (&job), &rs, &st);
    g_free (t);
    if (st.max_diff > TOL)
      g_test_message ("%s %ux%u -> %ux%u %s %s %s %s scale %s: max diff %g", fmt_name (fmt), sw, sh, net_w, net_h,
          aspect_names[aspect], bilinear ? "bilinear" : "nearest", color_names[cbs[i]->color],
          cbs[i]->nhwc ? "NHWC" : "NCHW", cbs[i]->norm ? "1/255" : "1", st.max_diff);
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
          check_case (formats[f], 1280, 720, nets[n][0], nets[n][1], (Aspect) a, bl, cbs, 12, stream, &st);
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
            check_case (formats[f], sizes[s][0], sizes[s][1], nets[n][0], nets[n][1], (Aspect) a, bl, cbs, 2,
                stream, &st);
          }
  report ("640x360 and 333x187", cases * 2, &st);
  g_assert_cmpint (hipStreamDestroy (stream), ==, hipSuccess);
}

/* The property behind the reference: the scaled rect equals rds_buf_surf_transform of the slot into a dw x dh
 * RGBA (or GRAY8) surface, normalised; the padding is scale * (0 - offset). Same device code, so no rounding
 * slack. */
static void
test_matches_transform (void)
{
  static const RdsBufSurfaceColorFormat srcs[] = { RDS_BUF_COLOR_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_BGRx };
  hipStream_t stream;
  double max_d = 0.0;

  RDS_TEST_REQUIRE_GPU ();
  stream = new_stream ();
  for (guint f = 0; f < G_N_ELEMENTS (srcs); f++) {
    for (int bl = 0; bl <= 1; bl++) {
      for (guint ci = 0; ci < 12; ci += 2) {
        const Combo *cb = &combos[ci];  /* every colour and order, normalised */
        RdsBufSurface *src = create_surf (srcs[f], 333, 187, 1), *conv;
        RdsPreprocessJob job = make_job (src, 0, 320, 192, ASPECT_SYMMETRIC, bl, cb);
        gboolean gray = cb->color == RDS_PREPROCESS_GRAY;
        RdsBufSurfTransformParams tp = {
          .transform_flag = RDS_BUFSURF_TRANSFORM_FILTER,
          .transform_filter = bl ? RdsBufSurfTransformInter_Bilinear : RdsBufSurfTransformInter_Nearest,
        };
        HostImage hc;
        float *t;

        fill_random (src, 0, 11 + f + bl);
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
              d = fabs ((double) t[tensor_index (&job, x, y, c)]
                  - (double) job.scale * (b - (double) job.offsets[c]));
              if (isnan (d))
                d = G_MAXDOUBLE;
              max_d = MAX (max_d, d);
            }
        g_assert_cmpfloat (max_d, <=, TOL);

        g_free (t);
        host_image_clear (&hc);
        g_assert_cmpint (rds_buf_surface_destroy (conv), ==, 0);
        g_assert_cmpint (rds_buf_surface_destroy (src), ==, 0);
      }
    }
  }
  g_test_message ("against rds_buf_surf_transform + normalisation: max |diff| %g", max_d);
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
  g_assert_cmpint (hipDeviceSynchronize (), ==, hipSuccess);

  /* a letterbox computed for another frame size is refused */
  {
    RdsInferLetterbox lb;

    rds_infer_letterbox (32, 32, 64, 50, TRUE, TRUE, &lb);
    job = good;
    g_assert_false (rds_infer_preprocess_job_set_source (&job, &src->surfaceList[0], &lb));
    g_assert_true (memcmp (&job, &good, sizeof (job)) == 0);
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
  g_test_add_func ("/gpu/preprocess/batch-isolation", test_batch_isolation);
  g_test_add_func ("/gpu/preprocess/timing", test_timing);
  return g_test_run ();
}
