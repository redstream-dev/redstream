/* rdsinfer's MIGraphX engine and its program cache, on the committed scale2 test models (output = 2 x input) in a
 * temporary directory; the ONNX reader on its own (cpu). The YOLO11n cases run only when the model has been fetched
 * (tests/models/fetch-yolo11n.sh); their programs are cached in the build directory, not next to the model.
 * Internal: links the plugin's static core library only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-infer-engine.h"
#include "rds-infer-onnx.h"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#define SCALE2 RDS_TEST_MODELS "/scale2.onnx"
#define SCALE2_B1 RDS_TEST_MODELS "/scale2-b1.onnx"
#define YOLO RDS_TEST_CACHE_MODELS "/yolo11n.onnx"
#define YOLO_REF_IN RDS_TEST_CACHE_MODELS "/yolo-parity-in.f32"
#define YOLO_REF_OUT RDS_TEST_CACHE_MODELS "/yolo-parity-out.f32"

static const guint dims_64x48[3] = { 3, 64, 48 };
/* The YOLO11n export has a symbolic height and width, so infer-dims are needed. */
static const guint dims_640[3] = { 3, 640, 640 };

#define HIP_OK(x) g_assert_cmpint ((x), ==, hipSuccess)

/* A fresh temporary directory holding a copy of @model as scale2.onnx. */
static gchar *
make_model_dir (const gchar * model, gchar ** onnx)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("rds-engine-XXXXXX", &err), *contents;
  gsize len;

  g_assert_no_error (err);
  g_assert_true (g_file_get_contents (model, &contents, &len, &err));
  g_assert_no_error (err);
  *onnx = g_build_filename (dir, "scale2.onnx", NULL);
  g_assert_true (g_file_set_contents (*onnx, contents, len, &err));
  g_assert_no_error (err);
  g_free (contents);
  return dir;
}

static void
remove_dir (const gchar * dir)
{
  GDir *d;
  const gchar *name;

  g_chmod (dir, 0755);
  d = g_dir_open (dir, 0, NULL);
  g_assert_nonnull (d);
  while ((name = g_dir_read_name (d))) {
    gchar *path = g_build_filename (dir, name, NULL);

    if (g_file_test (path, G_FILE_TEST_IS_DIR) && !g_file_test (path, G_FILE_TEST_IS_SYMLINK))
      remove_dir (path);
    else
      g_assert_cmpint (g_unlink (path), ==, 0);
    g_free (path);
  }
  g_dir_close (d);
  g_assert_cmpint (g_rmdir (dir), ==, 0);
}

static gboolean
dir_has_tmp (const gchar * dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *name;
  gboolean found = FALSE;

  g_assert_nonnull (d);
  while ((name = g_dir_read_name (d)))
    found |= g_str_has_suffix (name, ".tmp");
  g_dir_close (d);
  return found;
}

static gchar *
read_file (const gchar * path)
{
  GError *err = NULL;
  gchar *contents = NULL;

  g_assert_true (g_file_get_contents (path, &contents, NULL, &err));
  g_assert_no_error (err);
  return contents;
}

static void
write_file (const gchar * path, const gchar * contents, gssize len)
{
  GError *err = NULL;

  g_assert_true (g_file_set_contents (path, contents, len, &err));
  g_assert_no_error (err);
}

static RdsInferEngine *
open_engine (const gchar * onnx, const gchar * engine_file, guint batch, gboolean fp16, const guint * dims,
    gboolean force)
{
  RdsInferEngineOptions opts = { onnx, engine_file, batch, 0, fp16, dims, force };
  GError *err = NULL;
  RdsInferEngine *e = rds_infer_engine_open (&opts, &err);

  g_assert_no_error (err);
  g_assert_nonnull (e);
  if (rds_infer_engine_was_built (e))
    g_test_message ("built %s: %s", rds_infer_engine_get_program_path (e), rds_infer_engine_get_build_reason (e));
  return e;
}

static gfloat *
input_pattern (gsize n)
{
  gfloat *v = g_new (gfloat, n);

  for (gsize i = 0; i < n; i++)
    v[i] = (gfloat) ((gint) (i % 1001) - 500) * 0.37f;
  return v;
}

/* Runs @e on @input (host, batch * c * h * w floats) and returns output 0 (host). The output buffer is filled with
 * 0xFF bytes (NaN) first, so an output MIGraphX didn't write into our buffer can never compare equal. */
static gfloat *
run_engine (RdsInferEngine * e, guint batch, const gfloat * input, gsize * n_out)
{
  guint c, h, w, n_outputs = rds_infer_engine_get_n_outputs (e);
  gsize in_n, out_n[8];
  gfloat *d_in, *d_out[8], *out;
  hipStream_t stream;
  GError *err = NULL;

  g_assert_cmpuint (n_outputs, >=, 1);
  g_assert_cmpuint (n_outputs, <=, 8);
  rds_infer_engine_get_input (e, &c, &h, &w);
  in_n = (gsize) batch * c * h * w;
  HIP_OK (hipStreamCreateWithFlags (&stream, hipStreamNonBlocking));
  HIP_OK (hipMalloc ((void **) &d_in, in_n * sizeof (gfloat)));
  HIP_OK (hipMemcpyAsync (d_in, input, in_n * sizeof (gfloat), hipMemcpyHostToDevice, stream));
  for (guint i = 0; i < n_outputs; i++) {
    out_n[i] = batch * rds_infer_engine_get_output_elements (e, i);
    HIP_OK (hipMalloc ((void **) &d_out[i], out_n[i] * sizeof (gfloat)));
    HIP_OK (hipMemsetAsync (d_out[i], 0xFF, out_n[i] * sizeof (gfloat), stream));
  }
  g_assert_true (rds_infer_engine_run_async (e, d_in, d_out, stream, &err));
  g_assert_no_error (err);
  out = g_new (gfloat, out_n[0]);
  HIP_OK (hipMemcpyAsync (out, d_out[0], out_n[0] * sizeof (gfloat), hipMemcpyDeviceToHost, stream));
  HIP_OK (hipStreamSynchronize (stream));
  for (guint i = 0; i < n_outputs; i++)
    HIP_OK (hipFree (d_out[i]));
  HIP_OK (hipFree (d_in));
  HIP_OK (hipStreamDestroy (stream));
  *n_out = out_n[0];
  return out;
}

/* Runs scale2 on a pattern and checks output = 2 x input, within @tol (0: exactly). */
static void
check_scale2 (RdsInferEngine * e, guint batch, gfloat tol)
{
  guint c, h, w, dims[8], ndims;
  const gchar *name;
  gsize n, n_out;
  gfloat *in, *out;

  rds_infer_engine_get_input (e, &c, &h, &w);
  g_assert_cmpuint (c, ==, 3);
  g_assert_cmpuint (h, ==, 64);
  g_assert_cmpuint (w, ==, 48);
  g_assert_cmpstr (rds_infer_engine_get_input_name (e), ==, "images");
  g_assert_cmpuint (rds_infer_engine_get_n_outputs (e), ==, 1);
  rds_infer_engine_get_output (e, 0, &name, dims, &ndims);
  g_assert_cmpstr (name, ==, "output0");
  g_assert_cmpuint (ndims, ==, 3);
  g_assert_cmpuint (dims[0], ==, 3);
  g_assert_cmpuint (dims[1], ==, 64);
  g_assert_cmpuint (dims[2], ==, 48);
  g_assert_cmpuint (rds_infer_engine_get_output_elements (e, 0), ==, 3 * 64 * 48);

  n = (gsize) batch * c * h * w;
  in = input_pattern (n);
  out = run_engine (e, batch, in, &n_out);
  g_assert_cmpuint (n_out, ==, n);
  for (gsize i = 0; i < n; i++) {
    if (tol == 0.0f) {
      if (out[i] != 2.0f * in[i])
        g_error ("output[%" G_GSIZE_FORMAT "] = %g, expected %g", i, out[i], 2.0f * in[i]);
    } else if (!(fabsf (out[i] - 2.0f * in[i]) <= tol * fmaxf (1.0f, fabsf (2.0f * in[i])))) {
      g_error ("output[%" G_GSIZE_FORMAT "] = %g, expected %g (tolerance %g)", i, out[i], 2.0f * in[i], tol);
    }
  }
  g_free (in);
  g_free (out);
}

static gboolean
has_warning (RdsInferEngine * e, const gchar * needle)
{
  GPtrArray *w = rds_infer_engine_get_warnings (e);

  for (guint i = 0; i < w->len; i++)
    if (strstr (g_ptr_array_index (w, i), needle))
      return TRUE;
  return FALSE;
}

/* ----- the ONNX reader ----- */

static void
test_onnx_reader (void)
{
  GError *err = NULL;
  gchar *data;
  gsize len;
  RdsInferOnnxInfo *info;
  RdsInferOnnxValue *in, *out;

  g_assert_true (g_file_get_contents (SCALE2, &data, &len, &err));
  info = rds_infer_onnx_read_info ((const guint8 *) data, len, &err);
  g_assert_no_error (err);
  g_assert_cmpuint (info->inputs->len, ==, 1);      /* the "scale" initializer is not an input */
  in = g_ptr_array_index (info->inputs, 0);
  g_assert_cmpstr (in->name, ==, "images");
  g_assert_cmpint (in->elem_type, ==, RDS_INFER_ONNX_FLOAT);
  g_assert_true (in->has_shape);
  g_assert_cmpuint (in->n_dims, ==, 4);
  g_assert_cmpint (in->dims[0], ==, -1);
  g_assert_cmpstr (in->dim_params[0], ==, "batch");
  g_assert_cmpint (in->dims[1], ==, 3);
  g_assert_null (in->dim_params[1]);
  g_assert_cmpint (in->dims[2], ==, -1);
  g_assert_cmpstr (in->dim_params[2], ==, "height");
  g_assert_cmpstr (in->dim_params[3], ==, "width");
  g_assert_cmpuint (info->outputs->len, ==, 1);
  out = g_ptr_array_index (info->outputs, 0);
  g_assert_cmpstr (out->name, ==, "output0");
  rds_infer_onnx_info_free (info);
  g_free (data);

  g_assert_true (g_file_get_contents (SCALE2_B1, &data, &len, &err));
  info = rds_infer_onnx_read_info ((const guint8 *) data, len, &err);
  g_assert_no_error (err);
  in = g_ptr_array_index (info->inputs, 0);
  g_assert_cmpint (in->dims[0], ==, 1);
  g_assert_null (in->dim_params[0]);
  rds_infer_onnx_info_free (info);
  g_free (data);
}

/* Every prefix of a model and some garbage: an error or a result, never a crash or an overread (ASan). */
static void
test_onnx_reader_truncated (void)
{
  GError *err = NULL;
  gchar *data;
  gsize len, failed = 0;
  static const guint8 garbage[][6] = {
    { 0x3a, 0xff, 0xff, 0xff, 0xff, 0x0f },     /* graph, with a length beyond the data */
    { 0x3a, 0x80, 0x80, 0x80, 0x80, 0x80 },     /* an unterminated varint */
    { 0x3b, 0x00, 0x00, 0x00, 0x00, 0x00 },     /* field 7 as a (deprecated) group */
    { 0x0f, 0x00, 0x00, 0x00, 0x00, 0x00 },     /* wire type 7 */
  };

  g_assert_true (g_file_get_contents (SCALE2, &data, &len, &err));
  for (gsize n = 0; n < len; n++) {
    guint8 *copy = g_memdup2 (data, n);         /* exactly n bytes, so ASan sees any overread */
    RdsInferOnnxInfo *info = rds_infer_onnx_read_info (copy, n, &err);

    if (info) {
      rds_infer_onnx_info_free (info);
    } else {
      g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL);
      g_clear_error (&err);
      failed++;
    }
    g_free (copy);
  }
  g_test_message ("%" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " prefixes rejected", failed, len);
  g_assert_cmpuint (failed, >, 0);
  g_free (data);

  for (guint i = 0; i < G_N_ELEMENTS (garbage); i++) {
    g_assert_null (rds_infer_onnx_read_info (garbage[i], sizeof garbage[i], &err));
    g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL);
    g_test_message ("%s", err->message);
    g_clear_error (&err);
  }
}

static void
test_invalid_options (void)
{
  RdsInferEngineOptions opts = { SCALE2, NULL, 0, 0, FALSE, dims_64x48, FALSE };
  static const guint zero_dims[3] = { 3, 0, 48 };
  GError *err = NULL;

  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID);
  g_clear_error (&err);
  opts.batch_size = 1;
  opts.onnx_file = NULL;
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID);
  g_clear_error (&err);
  opts.onnx_file = SCALE2;
  opts.infer_dims = zero_dims;
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID);
  g_clear_error (&err);
}

/* ----- the engine on the GPU ----- */

/* Build, run, then load from the cache: same results, manifest untouched. */
static void
test_build_and_cache (void)
{
  gchar *onnx, *dir, *base, *manifest;
  RdsInferEngine *e;
  gint64 t0;

  RDS_TEST_REQUIRE_GPU ();
  dir = make_model_dir (SCALE2, &onnx);
  t0 = g_get_monotonic_time ();
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_test_message ("built in %.1f ms", (g_get_monotonic_time () - t0) / 1e3);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_nonnull (rds_infer_engine_get_build_reason (e));
  g_assert_cmpuint (rds_infer_engine_get_warnings (e)->len, ==, 0);
  base = g_path_get_basename (rds_infer_engine_get_program_path (e));
  g_test_message ("cache file %s", base);
  g_assert_true (g_str_has_prefix (base, "scale2_b2_gfx"));
  g_assert_true (g_str_has_suffix (base, "_fp32.mxr"));
  g_assert_true (g_str_has_prefix (rds_infer_engine_get_program_path (e), dir));
  g_assert_true (g_file_test (rds_infer_engine_get_program_path (e), G_FILE_TEST_IS_REGULAR));
  manifest = g_strconcat (rds_infer_engine_get_program_path (e), ".json", NULL);
  g_assert_cmpstr (rds_infer_engine_get_manifest_path (e), ==, manifest);
  g_free (manifest);
  manifest = read_file (rds_infer_engine_get_manifest_path (e));
  g_test_message ("manifest:\n%s", manifest);
  check_scale2 (e, 2, 0.0f);
  g_assert_false (dir_has_tmp (dir));
  rds_infer_engine_free (e);

  t0 = g_get_monotonic_time ();
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_test_message ("loaded in %.1f ms", (g_get_monotonic_time () - t0) / 1e3);
  g_assert_false (rds_infer_engine_was_built (e));
  g_assert_null (rds_infer_engine_get_build_reason (e));
  g_assert_true (g_str_has_suffix (rds_infer_engine_get_program_path (e), base));
  check_scale2 (e, 2, 0.0f);
  {
    gchar *again = read_file (rds_infer_engine_get_manifest_path (e));

    g_assert_cmpstr (again, ==, manifest);
    g_free (again);
  }
  rds_infer_engine_free (e);

  g_free (manifest);
  g_free (base);
  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

/* An edited manifest hash, a changed model, force_rebuild: each rebuilds, saying why. */
static void
test_invalidation (void)
{
  gchar *onnx, *dir, *manifest, *path, *hash;
  RdsInferEngine *e;
  gchar *contents;
  gsize len;

  RDS_TEST_REQUIRE_GPU ();
  dir = make_model_dir (SCALE2, &onnx);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  path = g_strdup (rds_infer_engine_get_manifest_path (e));
  rds_infer_engine_free (e);

  /* The manifest's hash edited: every hex digit replaced by '0'. */
  manifest = read_file (path);
  hash = strstr (manifest, "\"onnx_sha256\"");
  g_assert_nonnull (hash);
  hash = strchr (hash + strlen ("\"onnx_sha256\""), '"') + 1;
  for (guint i = 0; i < 64; i++)
    hash[i] = '0';
  write_file (path, manifest, -1);
  g_free (manifest);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_nonnull (strstr (rds_infer_engine_get_build_reason (e), "onnx_sha256"));
  check_scale2 (e, 2, 0.0f);
  rds_infer_engine_free (e);

  /* The model changed: a byte string appended as ModelProto.producer_version (field 3), still a valid model. */
  g_assert_true (g_file_get_contents (onnx, &contents, &len, NULL));
  contents = g_realloc (contents, len + 3);
  memcpy (contents + len, "\x1a\x01x", 3);
  write_file (onnx, contents, len + 3);
  g_free (contents);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_nonnull (strstr (rds_infer_engine_get_build_reason (e), "onnx_sha256"));
  rds_infer_engine_free (e);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_assert_false (rds_infer_engine_was_built (e));
  rds_infer_engine_free (e);

  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, TRUE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_nonnull (strstr (rds_infer_engine_get_build_reason (e), "forced"));
  check_scale2 (e, 2, 0.0f);
  rds_infer_engine_free (e);

  /* Another batch size is another program. */
  e = open_engine (onnx, NULL, 1, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_nonnull (strstr (rds_infer_engine_get_program_path (e), "scale2_b1_"));
  check_scale2 (e, 1, 0.0f);
  rds_infer_engine_free (e);

  g_free (path);
  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

static void
copy_file (const gchar * from, const gchar * to)
{
  gchar *contents;
  gsize len;

  g_assert_true (g_file_get_contents (from, &contents, &len, NULL));
  write_file (to, contents, len);
  g_free (contents);
}

static void
test_engine_file (void)
{
  gchar *onnx, *dir, *custom, *custom_json, *cache, *trt, *missing;
  RdsInferEngine *e;

  RDS_TEST_REQUIRE_GPU ();
  dir = make_model_dir (SCALE2, &onnx);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  cache = g_strdup (rds_infer_engine_get_program_path (e));
  custom = g_build_filename (dir, "custom.mxr", NULL);
  custom_json = g_strconcat (custom, ".json", NULL);
  copy_file (cache, custom);
  copy_file (rds_infer_engine_get_manifest_path (e), custom_json);
  rds_infer_engine_free (e);
  g_assert_cmpint (g_unlink (cache), ==, 0);
  {
    gchar *json = g_strconcat (cache, ".json", NULL);

    g_assert_cmpint (g_unlink (json), ==, 0);
    g_free (json);
  }

  /* A matching .mxr: loaded from there. */
  e = open_engine (onnx, custom, 2, FALSE, dims_64x48, FALSE);
  g_assert_false (rds_infer_engine_was_built (e));
  g_assert_cmpstr (rds_infer_engine_get_program_path (e), ==, custom);
  check_scale2 (e, 2, 0.0f);
  rds_infer_engine_free (e);
  g_assert_false (g_file_test (cache, G_FILE_TEST_EXISTS));

  /* A matching .mxr opened at another batch size doesn't match: built under the cache name for that size. */
  e = open_engine (onnx, custom, 1, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_nonnull (strstr (rds_infer_engine_get_build_reason (e), "batch_size"));
  g_assert_nonnull (strstr (rds_infer_engine_get_program_path (e), "scale2_b1_"));
  rds_infer_engine_free (e);

  /* A TensorRT engine: ignored with a warning, built from the ONNX file, saved under the cache name. */
  trt = g_build_filename (dir, "model.engine", NULL);
  write_file (trt, "not a program", -1);
  e = open_engine (onnx, trt, 2, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_true (has_warning (e, "TensorRT"));
  g_assert_cmpstr (rds_infer_engine_get_program_path (e), ==, cache);
  check_scale2 (e, 2, 0.0f);
  rds_infer_engine_free (e);
  /* ... and the next open finds that program (ours: the cache name is looked up after model-engine-file). */
  e = open_engine (onnx, trt, 2, FALSE, dims_64x48, FALSE);
  g_assert_false (rds_infer_engine_was_built (e));
  g_assert_true (has_warning (e, "TensorRT"));
  rds_infer_engine_free (e);
  g_assert_cmpint (g_unlink (cache), ==, 0);

  /* A missing file: built and saved under the cache name, the reason names the file. */
  missing = g_build_filename (dir, "missing.mxr", NULL);
  e = open_engine (onnx, missing, 2, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_nonnull (strstr (rds_infer_engine_get_build_reason (e), missing));
  g_assert_cmpstr (rds_infer_engine_get_program_path (e), ==, cache);
  g_assert_false (g_file_test (missing, G_FILE_TEST_EXISTS));
  rds_infer_engine_free (e);

  g_free (missing);
  g_free (trt);
  g_free (cache);
  g_free (custom_json);
  g_free (custom);
  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

/* A read-only model directory: the program goes to $XDG_CACHE_HOME/redstream/ (the test isolates XDG dirs) and
 * is found there the next time. */
static void
test_readonly_dir (void)
{
  gchar *onnx, *dir, *xdg;
  RdsInferEngine *e;

  RDS_TEST_REQUIRE_GPU ();
  dir = make_model_dir (SCALE2, &onnx);
  g_assert_cmpint (g_chmod (dir, 0555), ==, 0);
  if (access (dir, W_OK) == 0) {
    g_test_skip ("running as root: a 0555 directory is still writable");
    remove_dir (dir);
    g_free (dir);
    g_free (onnx);
    return;
  }
  xdg = g_build_filename (g_get_user_cache_dir (), "redstream", NULL);
  g_test_message ("cache dir %s", xdg);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_true (g_str_has_prefix (rds_infer_engine_get_program_path (e), xdg));
  g_assert_true (g_file_test (rds_infer_engine_get_manifest_path (e), G_FILE_TEST_IS_REGULAR));
  check_scale2 (e, 2, 0.0f);
  rds_infer_engine_free (e);

  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_assert_false (rds_infer_engine_was_built (e));
  g_assert_true (g_str_has_prefix (rds_infer_engine_get_program_path (e), xdg));
  rds_infer_engine_free (e);
  g_assert_false (dir_has_tmp (xdg));

  g_free (xdg);
  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

/* Stale temporary files (a crash before the rename) are ignored and overwritten; a program without a manifest (a
 * crash between the two renames) is rebuilt. */
static void
test_atomic_write (void)
{
  gchar *onnx, *dir, *cache, *tmp, *json_tmp;
  RdsInferEngine *e;

  RDS_TEST_REQUIRE_GPU ();
  dir = make_model_dir (SCALE2, &onnx);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  cache = g_strdup (rds_infer_engine_get_program_path (e));
  rds_infer_engine_free (e);
  g_assert_false (dir_has_tmp (dir));

  /* Crash before the renames: only the temporary files exist. */
  g_assert_cmpint (g_unlink (cache), ==, 0);
  {
    gchar *json = g_strconcat (cache, ".json", NULL);

    g_assert_cmpint (g_unlink (json), ==, 0);
    g_free (json);
  }
  tmp = g_strconcat (cache, ".tmp", NULL);
  json_tmp = g_strconcat (cache, ".json.tmp", NULL);
  write_file (tmp, "half a program", -1);
  write_file (json_tmp, "{\"format\":", -1);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  check_scale2 (e, 2, 0.0f);
  rds_infer_engine_free (e);
  g_assert_false (dir_has_tmp (dir));

  /* Crash between the renames: a program (here: garbage) without a manifest. */
  {
    gchar *json = g_strconcat (cache, ".json", NULL);

    g_assert_cmpint (g_unlink (json), ==, 0);
    write_file (cache, "garbage", -1);
    e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
    g_assert_true (rds_infer_engine_was_built (e));
    g_assert_nonnull (strstr (rds_infer_engine_get_build_reason (e), "manifest"));
    check_scale2 (e, 2, 0.0f);
    rds_infer_engine_free (e);
    g_assert_true (g_file_test (json, G_FILE_TEST_IS_REGULAR));
    g_free (json);
  }

  /* A matching manifest but a broken program: rebuilt. */
  write_file (cache, "garbage", -1);
  e = open_engine (onnx, NULL, 2, FALSE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  check_scale2 (e, 2, 0.0f);
  rds_infer_engine_free (e);

  g_free (json_tmp);
  g_free (tmp);
  g_free (cache);
  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

static void
test_fp16 (void)
{
  gchar *onnx, *dir;
  RdsInferEngine *e;

  RDS_TEST_REQUIRE_GPU ();
  dir = make_model_dir (SCALE2, &onnx);
  e = open_engine (onnx, NULL, 1, TRUE, dims_64x48, FALSE);
  g_assert_true (rds_infer_engine_was_built (e));
  g_assert_true (g_str_has_suffix (rds_infer_engine_get_program_path (e), "_fp16.mxr"));
  g_assert_nonnull (strstr (rds_infer_engine_get_program_path (e), "scale2_b1_"));
  check_scale2 (e, 1, 1e-2f);
  rds_infer_engine_free (e);
  e = open_engine (onnx, NULL, 1, TRUE, dims_64x48, FALSE);
  g_assert_false (rds_infer_engine_was_built (e));
  check_scale2 (e, 1, 1e-2f);
  rds_infer_engine_free (e);
  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

/* A model with its batch fixed at 1: fine at batch 1 (dims still per image), an error at batch 2. */
static void
test_fixed_batch (void)
{
  gchar *onnx, *dir;
  RdsInferEngineOptions opts = { NULL, NULL, 2, 0, FALSE, dims_64x48, FALSE };
  GError *err = NULL;
  RdsInferEngine *e;

  RDS_TEST_REQUIRE_GPU ();
  dir = make_model_dir (SCALE2_B1, &onnx);
  opts.onnx_file = onnx;
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL);
  g_test_message ("%s", err->message);
  g_assert_nonnull (strstr (err->message, "dynamic batch"));
  g_clear_error (&err);

  e = open_engine (onnx, NULL, 1, FALSE, dims_64x48, FALSE);
  check_scale2 (e, 1, 0.0f);
  rds_infer_engine_free (e);
  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

static void
test_errors (void)
{
  RdsInferEngineOptions opts = { RDS_TEST_MODELS "/missing.onnx", NULL, 1, 0, FALSE, dims_64x48, FALSE };
  static const guint wrong_c[3] = { 4, 64, 48 };
  GError *err = NULL;
  gchar *onnx, *dir;
  RdsInferEngine *e;
  float *d_buf;

  RDS_TEST_REQUIRE_GPU ();
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_FILE);
  g_test_message ("%s", err->message);
  g_clear_error (&err);

  dir = make_model_dir (SCALE2, &onnx);
  opts.onnx_file = onnx;
  opts.infer_dims = NULL;
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL);
  g_test_message ("%s", err->message);
  g_assert_nonnull (strstr (err->message, "height"));
  g_assert_nonnull (strstr (err->message, "infer-dims"));
  g_clear_error (&err);

  opts.infer_dims = wrong_c;
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL);
  g_test_message ("%s", err->message);
  g_clear_error (&err);

  opts.infer_dims = dims_64x48;
  opts.gpu_id = 99;
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_DEVICE);
  g_test_message ("%s", err->message);
  g_clear_error (&err);

  /* Not a model. */
  write_file (onnx, "this is not a protobuf", -1);
  opts.gpu_id = 0;
  g_assert_null (rds_infer_engine_open (&opts, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL);
  g_test_message ("%s", err->message);
  g_clear_error (&err);

  /* run_async refuses the null stream. */
  copy_file (SCALE2, onnx);
  e = open_engine (onnx, NULL, 1, FALSE, dims_64x48, FALSE);
  HIP_OK (hipMalloc ((void **) &d_buf, 3 * 64 * 48 * sizeof (float)));
  g_assert_false (rds_infer_engine_run_async (e, d_buf, &d_buf, NULL, &err));
  g_assert_error (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID);
  g_clear_error (&err);
  HIP_OK (hipFree (d_buf));
  rds_infer_engine_free (e);

  remove_dir (dir);
  g_free (dir);
  g_free (onnx);
}

/* ----- YOLO11n (when fetched) ----- */

/* The fetched model behind a symlink in the build directory, so its programs are cached there. */
static gchar *
yolo_link (void)
{
  gchar *dir = g_build_filename (RDS_TEST_BUILD_DIR, "infer-engine-yolo", NULL);
  gchar *link = g_build_filename (dir, "yolo11n.onnx", NULL);

  g_assert_cmpint (g_mkdir_with_parents (dir, 0755), ==, 0);
  if (!g_file_test (link, G_FILE_TEST_IS_SYMLINK))
    g_assert_cmpint (symlink (YOLO, link), ==, 0);
  g_free (dir);
  return link;
}

static void
test_yolo_smoke (void)
{
  gchar *link;
  RdsInferEngine *e;
  guint dims[8], ndims, c, h, w;
  const gchar *name;
  gint64 t0;
  gsize n, n_out;
  gfloat *in, *out;
  GRand *rand;

  RDS_TEST_REQUIRE_GPU ();
  if (!g_file_test (YOLO, G_FILE_TEST_IS_REGULAR)) {
    g_test_skip ("no " YOLO " (run tests/models/fetch-yolo11n.sh)");
    return;
  }
  link = yolo_link ();
  t0 = g_get_monotonic_time ();
  e = open_engine (link, NULL, 1, FALSE, dims_640, FALSE);
  g_test_message ("first open (%s): %.1f s", rds_infer_engine_was_built (e) ? "built" : "loaded",
      (g_get_monotonic_time () - t0) / 1e6);
  rds_infer_engine_free (e);
  t0 = g_get_monotonic_time ();
  e = open_engine (link, NULL, 1, FALSE, dims_640, FALSE);
  g_test_message ("second open (loaded): %.1f ms", (g_get_monotonic_time () - t0) / 1e3);
  g_assert_false (rds_infer_engine_was_built (e));

  rds_infer_engine_get_input (e, &c, &h, &w);
  g_assert_cmpuint (c, ==, 3);
  g_assert_cmpuint (h, ==, 640);
  g_assert_cmpuint (w, ==, 640);
  g_assert_cmpuint (rds_infer_engine_get_n_outputs (e), ==, 1);
  rds_infer_engine_get_output (e, 0, &name, dims, &ndims);
  g_assert_cmpstr (name, ==, "output0");
  g_assert_cmpuint (ndims, ==, 2);
  g_assert_cmpuint (dims[0], ==, 84);
  g_assert_cmpuint (dims[1], ==, 8400);

  n = (gsize) c * h * w;
  rand = g_rand_new_with_seed (5);
  in = g_new (gfloat, n);
  for (gsize i = 0; i < n; i++)
    in[i] = (gfloat) g_rand_double (rand);
  out = run_engine (e, 1, in, &n_out);
  g_assert_cmpuint (n_out, ==, 84 * 8400);
  for (gsize i = 0; i < n_out; i++)
    if (!isfinite (out[i]))
      g_error ("output[%" G_GSIZE_FORMAT "] = %g", i, out[i]);
  g_free (out);
  g_free (in);
  g_rand_free (rand);
  rds_infer_engine_free (e);
  g_free (link);
}

/* Against ONNX Runtime's output on a stored input (raw little-endian float32, 1x3x640x640 in, 1x84x8400 out). */
static void
test_yolo_parity (void)
{
  gchar *link, *in, *ref;
  gsize in_len, ref_len, n_out;
  RdsInferEngine *e;
  gfloat *out, max_diff = 0.0f;

  RDS_TEST_REQUIRE_GPU ();
  if (!g_file_test (YOLO, G_FILE_TEST_IS_REGULAR) || !g_file_test (YOLO_REF_IN, G_FILE_TEST_IS_REGULAR)
      || !g_file_test (YOLO_REF_OUT, G_FILE_TEST_IS_REGULAR)) {
    g_test_skip ("needs " YOLO ", " YOLO_REF_IN " and " YOLO_REF_OUT);
    return;
  }
  link = yolo_link ();
  e = open_engine (link, NULL, 1, FALSE, dims_640, FALSE);
  g_assert_true (g_file_get_contents (YOLO_REF_IN, &in, &in_len, NULL));
  g_assert_true (g_file_get_contents (YOLO_REF_OUT, &ref, &ref_len, NULL));
  g_assert_cmpuint (in_len, ==, 3 * 640 * 640 * sizeof (gfloat));
  g_assert_cmpuint (ref_len, ==, 84 * 8400 * sizeof (gfloat));
  out = run_engine (e, 1, (const gfloat *) in, &n_out);
  g_assert_cmpuint (n_out * sizeof (gfloat), ==, ref_len);
  for (gsize i = 0; i < n_out; i++) {
    gfloat d = fabsf (out[i] - ((const gfloat *) ref)[i]);

    if (!(d <= max_diff))
      max_diff = isnan (d) ? INFINITY : d;
  }
  g_test_message ("max |diff| against ONNX Runtime: %g", max_diff);
  g_assert_cmpfloat (max_diff, <=, 0.02f);
  g_free (out);
  g_free (ref);
  g_free (in);
  rds_infer_engine_free (e);
  g_free (link);
}

int
main (int argc, char **argv)
{
  /* G_TEST_OPTION_ISOLATE_DIRS points HOME at /dev/null, where MIOpen (convolutions, YOLO11n) can't create its
   * databases: keep them in the build directory. */
  g_setenv ("MIOPEN_USER_DB_PATH", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_setenv ("MIOPEN_CUSTOM_CACHE_DIR", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  g_test_add_func ("/cpu/infer-engine/onnx-reader", test_onnx_reader);
  g_test_add_func ("/cpu/infer-engine/onnx-reader-truncated", test_onnx_reader_truncated);
  g_test_add_func ("/cpu/infer-engine/invalid-options", test_invalid_options);
  g_test_add_func ("/gpu/infer-engine/build-and-cache", test_build_and_cache);
  g_test_add_func ("/gpu/infer-engine/invalidation", test_invalidation);
  g_test_add_func ("/gpu/infer-engine/engine-file", test_engine_file);
  g_test_add_func ("/gpu/infer-engine/readonly-dir", test_readonly_dir);
  g_test_add_func ("/gpu/infer-engine/atomic-write", test_atomic_write);
  g_test_add_func ("/gpu/infer-engine/fp16", test_fp16);
  g_test_add_func ("/gpu/infer-engine/fixed-batch", test_fixed_batch);
  g_test_add_func ("/gpu/infer-engine/errors", test_errors);
  g_test_add_func ("/gpu/infer-yolo/smoke", test_yolo_smoke);
  g_test_add_func ("/gpu/infer-yolo/parity", test_yolo_parity);
  return g_test_run ();
}
