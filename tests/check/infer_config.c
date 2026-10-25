/* rdsinfer config parser: key file and YAML corpus (tests/check/infer-configs), defaults, errors, warnings, labels.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-config.h"
#include "rds-keyfile-yaml.h"

#include <glib/gstdio.h>
#include <string.h>

static gchar *
corpus (const gchar * name)
{
  return g_build_filename (RDS_INFER_CONFIGS, name, NULL);
}

static RdsInferConfig *
load_ok (const gchar * name)
{
  gchar *path = corpus (name);
  GError *err = NULL;
  RdsInferConfig *cfg = rds_infer_config_load (path, &err);

  g_assert_no_error (err);
  g_assert_nonnull (cfg);
  g_free (path);
  return cfg;
}

/* Loads @path, which must fail with @code; the message must contain "<path>:" followed by each of the
 * NULL-terminated substrings in @ap, in order. */
static void
assert_load_fails (const gchar * path, gint code, va_list ap)
{
  gchar *prefix = g_strconcat (path, ":", NULL);
  GError *err = NULL;
  RdsInferConfig *cfg = rds_infer_config_load (path, &err);
  const gchar *pos, *want;

  g_assert_null (cfg);
  g_assert_error (err, RDS_INFER_CONFIG_ERROR, code);
  g_test_message ("%s", err->message);
  pos = strstr (err->message, prefix);
  g_assert_nonnull (pos);
  pos += strlen (prefix);
  while ((want = va_arg (ap, const gchar *))) {
    const gchar *found = strstr (pos, want);

    if (!found)
      g_error ("\"%s\" not found after the file name in \"%s\"", want, err->message);
    pos = found + strlen (want);
  }
  g_error_free (err);
  g_free (prefix);
}

/* A corpus file that must fail: see assert_load_fails. */
static void
load_fails (const gchar * name, gint code, ...)
{
  gchar *path = corpus (name);
  va_list ap;

  va_start (ap, code);
  assert_load_fails (path, code, ap);
  va_end (ap);
  g_free (path);
}

/* A one-off config: @text written to a fresh temp dir as @name (with @labels as labels.txt unless NULL). Returns
 * the config path; tmp_cleanup removes both files and the dir. */
static gchar *
tmp_config (const gchar * name, const gchar * text, const gchar * labels)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("rds-infer-config-XXXXXX", &err);
  gchar *path = g_build_filename (dir, name, NULL);

  g_assert_no_error (err);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
  if (labels) {
    gchar *lbl = g_build_filename (dir, "labels.txt", NULL);

    g_assert_true (g_file_set_contents (lbl, labels, -1, NULL));
    g_free (lbl);
  }
  g_free (dir);
  return path;
}

static void
tmp_cleanup (gchar * path)
{
  gchar *dir = g_path_get_dirname (path), *lbl = g_build_filename (dir, "labels.txt", NULL);

  g_unlink (lbl);
  g_unlink (path);
  g_rmdir (dir);
  g_free (lbl);
  g_free (dir);
  g_free (path);
}

/* A one-off config that must fail: see assert_load_fails. */
static void
text_fails (const gchar * name, const gchar * text, gint code, ...)
{
  gchar *path = tmp_config (name, text, NULL);
  va_list ap;

  va_start (ap, code);
  assert_load_fails (path, code, ap);
  va_end (ap);
  tmp_cleanup (path);
}

#define DETECTOR_TXT "[property]\nonnx-file=m.onnx\nnum-detected-classes=2\nparse-bbox-func-name=RdsInferParseYolo\n"
#define DETECTOR_YML "property:\n  onnx-file: m.onnx\n  num-detected-classes: 2\n" \
  "  parse-bbox-func-name: RdsInferParseYolo\n"

static guint
count_warnings (RdsInferConfig * cfg, const gchar * needle)
{
  guint n = 0;

  for (guint i = 0; i < cfg->warnings->len; i++)
    if (strstr (g_ptr_array_index (cfg->warnings, i), needle))
      n++;
  return n;
}

static void
assert_attrs_equal (const RdsInferClassAttrs * a, const RdsInferClassAttrs * b)
{
  g_assert_cmpfloat (a->pre_cluster_threshold, ==, b->pre_cluster_threshold);
  g_assert_cmpfloat (a->post_cluster_threshold, ==, b->post_cluster_threshold);
  g_assert_cmpfloat (a->nms_iou_threshold, ==, b->nms_iou_threshold);
  g_assert_cmpint (a->topk, ==, b->topk);
  g_assert_cmpuint (a->roi_top_offset, ==, b->roi_top_offset);
  g_assert_cmpuint (a->roi_bottom_offset, ==, b->roi_bottom_offset);
  g_assert_cmpuint (a->detected_min_w, ==, b->detected_min_w);
  g_assert_cmpuint (a->detected_min_h, ==, b->detected_min_h);
  g_assert_cmpuint (a->detected_max_w, ==, b->detected_max_w);
  g_assert_cmpuint (a->detected_max_h, ==, b->detected_max_h);
}

static void
assert_strv_equal (gchar ** a, gchar ** b)
{
  g_assert_true ((a == NULL) == (b == NULL));
  if (a)
    g_assert_true (g_strv_equal ((const gchar * const *) a, (const gchar * const *) b));
}

static void
test_txt_yaml_equal (void)
{
  RdsInferConfig *t = load_ok ("minimal.txt"), *y = load_ok ("minimal.yml");

  g_assert_cmpstr (t->config_dir, ==, y->config_dir);
  g_assert_cmpstr (t->onnx_file, ==, y->onnx_file);
  g_assert_cmpstr (t->model_engine_file, ==, y->model_engine_file);
  g_assert_cmpstr (t->labelfile_path, ==, y->labelfile_path);
  g_assert_cmpstr (t->custom_lib_path, ==, y->custom_lib_path);
  g_assert_cmpstr (t->parse_bbox_func_name, ==, y->parse_bbox_func_name);
  g_assert_cmpuint (t->batch_size, ==, y->batch_size);
  g_assert_cmpuint (t->interval, ==, y->interval);
  g_assert_cmpuint (t->gie_unique_id, ==, y->gie_unique_id);
  g_assert_cmpuint (t->num_detected_classes, ==, y->num_detected_classes);
  g_assert_cmpuint (t->gpu_id, ==, y->gpu_id);
  g_assert_cmpint (t->network_mode, ==, y->network_mode);
  g_assert_cmpint (t->network_type, ==, y->network_type);
  g_assert_cmpint (t->process_mode, ==, y->process_mode);
  g_assert_cmpint (t->cluster_mode, ==, y->cluster_mode);
  g_assert_cmpint (t->model_color_format, ==, y->model_color_format);
  g_assert_cmpint (t->network_input_order, ==, y->network_input_order);
  g_assert_cmpint (t->scaling_filter, ==, y->scaling_filter);
  g_assert_cmpfloat (t->net_scale_factor, ==, y->net_scale_factor);
  g_assert_cmpuint (t->n_offsets, ==, y->n_offsets);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpfloat (t->offsets[i], ==, y->offsets[i]);
  g_assert_cmpint (t->has_infer_dims, ==, y->has_infer_dims);
  for (guint i = 0; i < 3; i++)
    g_assert_cmpuint (t->infer_dims[i], ==, y->infer_dims[i]);
  g_assert_cmpint (t->maintain_aspect_ratio, ==, y->maintain_aspect_ratio);
  g_assert_cmpint (t->symmetric_padding, ==, y->symmetric_padding);
  g_assert_cmpint (t->output_tensor_meta, ==, y->output_tensor_meta);
  assert_strv_equal (t->output_blob_names, y->output_blob_names);
  g_assert_cmpuint (t->filter_out_class_ids->len, ==, y->filter_out_class_ids->len);
  for (guint i = 0; i < t->filter_out_class_ids->len; i++)
    g_assert_cmpuint (g_array_index (t->filter_out_class_ids, guint, i), ==,
        g_array_index (y->filter_out_class_ids, guint, i));
  for (guint i = 0; i < t->num_detected_classes; i++)
    assert_attrs_equal (&t->class_attrs[i], &y->class_attrs[i]);
  assert_strv_equal (t->labels, y->labels);
  g_assert_cmpuint (t->warnings->len, ==, 0);
  g_assert_cmpuint (y->warnings->len, ==, 0);

  /* and the values are the ones written */
  g_assert_cmpuint (t->batch_size, ==, 2);
  g_assert_cmpuint (t->interval, ==, 1);
  g_assert_cmpuint (t->gie_unique_id, ==, 3);
  g_assert_cmpuint (t->num_detected_classes, ==, 3);
  g_assert_cmpint (t->network_mode, ==, 2);
  g_assert_cmpint (t->model_color_format, ==, 1);
  g_assert_cmpfloat_with_epsilon (t->net_scale_factor, 1.0f / 255.0f, 1e-9);
  g_assert_cmpuint (t->n_offsets, ==, 3);
  g_assert_cmpfloat (t->offsets[0], ==, 1.5f);
  g_assert_cmpfloat (t->offsets[2], ==, 3.5f);
  g_assert_true (t->has_infer_dims);
  g_assert_cmpuint (t->infer_dims[0], ==, 3);
  g_assert_cmpuint (t->infer_dims[1], ==, 640);
  g_assert_cmpuint (t->infer_dims[2], ==, 640);
  g_assert_true (t->maintain_aspect_ratio);
  g_assert_true (t->symmetric_padding);
  g_assert_false (t->output_tensor_meta);
  g_assert_cmpstr (t->parse_bbox_func_name, ==, "RdsInferParseYolo");
  g_assert_cmpuint (t->filter_out_class_ids->len, ==, 2);
  g_assert_cmpuint (g_array_index (t->filter_out_class_ids, guint, 1), ==, 2);
  g_assert_cmpuint (g_strv_length (t->output_blob_names), ==, 1);
  g_assert_cmpstr (t->output_blob_names[0], ==, "output0");
  g_assert_cmpfloat (t->class_attrs[0].pre_cluster_threshold, ==, 0.3f);
  g_assert_cmpfloat (t->class_attrs[0].post_cluster_threshold, ==, 0.0f);
  g_assert_cmpfloat (t->class_attrs[1].post_cluster_threshold, ==, 0.6f);
  g_assert_cmpfloat (t->class_attrs[1].nms_iou_threshold, ==, 0.5f);
  g_assert_cmpint (t->class_attrs[2].topk, ==, 100);
  g_assert_cmpuint (g_strv_length (t->labels), ==, 3);
  g_assert_cmpstr (t->labels[0], ==, "person");
  g_assert_cmpstr (t->labels[2], ==, "bicycle");

  rds_infer_config_free (t);
  rds_infer_config_free (y);
}

static void
test_defaults (void)
{
  RdsInferConfig *cfg = load_ok ("defaults.txt");

  g_assert_cmpuint (cfg->batch_size, ==, 1);
  g_assert_cmpuint (cfg->interval, ==, 0);
  g_assert_cmpuint (cfg->gie_unique_id, ==, 1);
  g_assert_cmpuint (cfg->gpu_id, ==, 0);
  g_assert_cmpfloat (cfg->net_scale_factor, ==, 1.0f);
  g_assert_cmpuint (cfg->n_offsets, ==, 0);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpfloat (cfg->offsets[i], ==, 0.0f);
  g_assert_cmpint (cfg->model_color_format, ==, 0);
  g_assert_cmpint (cfg->network_input_order, ==, 0);
  g_assert_cmpint (cfg->network_mode, ==, 0);
  g_assert_cmpint (cfg->network_type, ==, 0);
  g_assert_cmpint (cfg->process_mode, ==, 1);
  g_assert_cmpint (cfg->cluster_mode, ==, 2);
  g_assert_false (cfg->maintain_aspect_ratio);
  g_assert_false (cfg->symmetric_padding);
  g_assert_false (cfg->output_tensor_meta);
  g_assert_cmpint (cfg->scaling_filter, ==, 1);
  g_assert_false (cfg->has_infer_dims);
  g_assert_null (cfg->model_engine_file);
  g_assert_null (cfg->labelfile_path);
  g_assert_null (cfg->custom_lib_path);
  g_assert_null (cfg->output_blob_names);
  g_assert_nonnull (cfg->filter_out_class_ids);
  g_assert_cmpuint (cfg->filter_out_class_ids->len, ==, 0);
  g_assert_cmpuint (cfg->num_detected_classes, ==, 2);
  for (guint i = 0; i < 2; i++) {
    RdsInferClassAttrs *a = &cfg->class_attrs[i];

    g_assert_cmpfloat (a->pre_cluster_threshold, ==, 0.25f);
    g_assert_cmpfloat (a->post_cluster_threshold, ==, 0.0f);
    g_assert_cmpfloat (a->nms_iou_threshold, ==, 0.45f);
    g_assert_cmpint (a->topk, ==, -1);
    g_assert_cmpuint (a->roi_top_offset, ==, 0);
    g_assert_cmpuint (a->roi_bottom_offset, ==, 0);
    g_assert_cmpuint (a->detected_min_w, ==, 0);
    g_assert_cmpuint (a->detected_min_h, ==, 0);
    g_assert_cmpuint (a->detected_max_w, ==, 0);
    g_assert_cmpuint (a->detected_max_h, ==, 0);
  }
  /* no label file: empty labels */
  g_assert_cmpuint (g_strv_length (cfg->labels), ==, 2);
  g_assert_cmpstr (cfg->labels[0], ==, "");
  g_assert_cmpstr (cfg->labels[1], ==, "");
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_infer_config_free (cfg);
}

static void
test_offsets (void)
{
  const gchar *files[] = { "offsets.txt", "offsets.yml" };
  RdsInferConfig *cfg;

  for (guint f = 0; f < G_N_ELEMENTS (files); f++) {
    cfg = load_ok (files[f]);
    g_assert_cmpuint (cfg->n_offsets, ==, 3);
    g_assert_cmpfloat (cfg->offsets[0], ==, 77.5f);
    g_assert_cmpfloat (cfg->offsets[1], ==, 21.2f);
    g_assert_cmpfloat (cfg->offsets[2], ==, 11.8f);
    g_assert_cmpfloat (cfg->offsets[3], ==, 0.0f);
    rds_infer_config_free (cfg);
  }
  cfg = load_ok ("offsets-gray.txt");
  g_assert_cmpuint (cfg->n_offsets, ==, 1);
  g_assert_cmpfloat (cfg->offsets[0], ==, 128.0f);
  rds_infer_config_free (cfg);
  load_fails ("offsets-count.txt", RDS_INFER_CONFIG_ERROR_INVALID, "offsets: ", "77.5;21.2", ": ", "3", NULL);
}

static void
test_class_attrs (void)
{
  RdsInferConfig *cfg = load_ok ("class-attrs.txt");

  g_assert_cmpuint (cfg->num_detected_classes, ==, 4);
  for (guint i = 0; i < 4; i++) {
    RdsInferClassAttrs *a = &cfg->class_attrs[i];

    g_assert_cmpfloat (a->pre_cluster_threshold, ==, i == 3 ? 0.9f : 0.4f);
    g_assert_cmpfloat (a->nms_iou_threshold, ==, i == 2 ? 0.3f : 0.6f);
    g_assert_cmpint (a->topk, ==, 20);
    g_assert_cmpuint (a->roi_top_offset, ==, 10);
    g_assert_cmpuint (a->detected_max_w, ==, 500);
    /* not in any group: the default */
    g_assert_cmpfloat (a->post_cluster_threshold, ==, 0.0f);
  }
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_infer_config_free (cfg);
  load_fails ("class-attrs-range.txt", RDS_INFER_CONFIG_ERROR_INVALID, "class-attrs-80", "80", NULL);
}

static void
test_relative_paths (void)
{
  const gchar *files[] = { "sub/relative-paths.txt", "sub/relative-paths.yml" };
  gchar *dir = g_build_filename (RDS_INFER_CONFIGS, "sub", NULL);
  gchar *abs_dir = g_canonicalize_filename (dir, NULL);
  gchar *onnx = g_build_filename (abs_dir, "models", "model.onnx", NULL);
  gchar *labels = g_canonicalize_filename ("../labels-3.txt", abs_dir);

  for (guint f = 0; f < G_N_ELEMENTS (files); f++) {
    RdsInferConfig *cfg = load_ok (files[f]);

    g_assert_cmpstr (cfg->config_dir, ==, abs_dir);
    g_assert_cmpstr (cfg->onnx_file, ==, onnx);
    g_assert_cmpstr (cfg->labelfile_path, ==, labels);
    g_assert_cmpstr (cfg->model_engine_file, ==, "/opt/models/model_b1.mxr");
    g_assert_cmpstr (cfg->custom_lib_path, ==, "/usr/lib/libmyparser.so");
    g_assert_cmpstr (cfg->labels[1], ==, "car");
    rds_infer_config_free (cfg);
  }
  g_free (labels);
  g_free (onnx);
  g_free (abs_dir);
  g_free (dir);
}

static void
test_rejected (void)
{
  const struct
  {
    const gchar *file, *key, *value;
  } cases[] = {
    {"rejected-process-mode.txt", "process-mode", "2"},
    {"rejected-network-type.txt", "network-type", "1"},
    {"rejected-cluster-mode.txt", "cluster-mode", "0"},
    {"rejected-network-mode.txt", "network-mode", "1"},
    {"rejected-engine-create-func-name.txt", "engine-create-func-name", "MyEngine"},
    {"rejected-mean-file.txt", "mean-file", "mean.ppm"},
    {"rejected-scaling-compute-hw.txt", "scaling-compute-hw", "2"},
    {"rejected-enable-dbscan.txt", "enable-dbscan", "1"},
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
    gchar *kv = g_strdup_printf ("%s: %s: ", cases[i].key, cases[i].value);

    load_fails (cases[i].file, RDS_INFER_CONFIG_ERROR_UNSUPPORTED, kv, NULL);
    g_free (kv);
  }
  /* the error names the built-in parser */
  load_fails ("rejected-no-parser.txt", RDS_INFER_CONFIG_ERROR_UNSUPPORTED, "parse-bbox-func-name: ",
      "RdsInferParseYolo", NULL);
}

static void
test_required (void)
{
  RdsInferConfig *cfg;

  load_fails ("no-classes.txt", RDS_INFER_CONFIG_ERROR_INVALID, "num-detected-classes: ", NULL);
  load_fails ("no-model.txt", RDS_INFER_CONFIG_ERROR_INVALID, "onnx-file: ", NULL);
  /* no post-processing: classes and parser are optional */
  cfg = load_ok ("tensor-only.txt");
  g_assert_cmpint (cfg->network_type, ==, 100);
  g_assert_true (cfg->output_tensor_meta);
  g_assert_cmpuint (cfg->num_detected_classes, ==, 0);
  g_assert_nonnull (cfg->labels);
  g_assert_null (cfg->labels[0]);
  rds_infer_config_free (cfg);
}

static void
test_noop_warnings (void)
{
  const gchar *noop[] = { "gpu-id", "workspace-size", "force-implicit-batch-dim", "enable-dla", "use-dla-core",
    "tensor-meta-pool-size", "scaling-compute-hw", "is-classifier", "crop-objects-to-roi-boundary", "eps",
    "group-threshold", "minBoxes", "dbscan-min-score"
  };
  RdsInferConfig *cfg = load_ok ("noop.txt");

  for (guint i = 0; i < cfg->warnings->len; i++)
    g_test_message ("warning: %s", (const gchar *) g_ptr_array_index (cfg->warnings, i));
  for (guint i = 0; i < G_N_ELEMENTS (noop); i++) {
    gchar *needle = g_strdup_printf ("%s: ", noop[i]);

    if (count_warnings (cfg, needle) != 1)
      g_error ("expected one warning for %s", noop[i]);
    g_free (needle);
  }
  g_assert_cmpuint (count_warnings (cfg, "no-such-key"), ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "extra"), ==, 1);
  g_assert_cmpuint (cfg->warnings->len, ==, G_N_ELEMENTS (noop) + 2);
  /* no-ops are not applied: gpu-id comes from the element property only */
  g_assert_cmpuint (cfg->gpu_id, ==, 0);
  rds_infer_config_free (cfg);
}

static void
test_wrong_types (void)
{
  load_fails ("bad-int.txt", RDS_INFER_CONFIG_ERROR_INVALID, "batch-size: abc: ", NULL);
  load_fails ("bad-float.txt", RDS_INFER_CONFIG_ERROR_INVALID, "net-scale-factor: -1: ", NULL);
  load_fails ("bad-bool.txt", RDS_INFER_CONFIG_ERROR_INVALID, "maintain-aspect-ratio: maybe: ", NULL);
}

static void
test_missing (void)
{
  gchar *path = corpus ("no-such-config.txt");
  GError *err = NULL;

  g_assert_null (rds_infer_config_load (path, &err));
  g_assert_error (err, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_FILE);
  g_assert_nonnull (strstr (err->message, path));
  g_clear_error (&err);
  g_free (path);

  load_fails ("missing-property.txt", RDS_INFER_CONFIG_ERROR_INVALID, " no [property] group", NULL);
  load_fails ("missing-labels.txt", RDS_INFER_CONFIG_ERROR_FILE, "labelfile-path: ", "no-such-labels.txt", NULL);
}

static void
test_malformed_yaml (void)
{
  load_fails ("malformed.yml", RDS_INFER_CONFIG_ERROR_PARSE, "6: ", "line 5", NULL);
  load_fails ("nested.yml", RDS_INFER_CONFIG_ERROR_PARSE, "6: ", NULL);
  load_fails ("ctrl-group.yml", RDS_INFER_CONFIG_ERROR_PARSE, "2: ", NULL);
  text_fails ("pgie.yml", DETECTOR_YML "  \"x\\x01y\": 1\n", RDS_INFER_CONFIG_ERROR_PARSE, "5: ", NULL);
  load_fails ("semicolon-item.yml", RDS_INFER_CONFIG_ERROR_PARSE, "4: ", "';'", NULL);
}

/* Plain ~, null, Null, NULL and empty values are unset, for keys and for groups; quoted ones are strings. */
static void
test_yaml_null (void)
{
  RdsInferConfig *cfg = load_ok ("yaml-null.yml");
  gchar *tilde = g_build_filename (cfg->config_dir, "~", NULL);

  g_assert_null (cfg->model_engine_file);
  g_assert_null (cfg->labelfile_path);
  g_assert_null (cfg->custom_lib_path);
  g_assert_null (cfg->output_blob_names);
  g_assert_cmpstr (cfg->onnx_file, ==, tilde);  /* quoted "~" */
  g_assert_cmpstr (cfg->labels[0], ==, "");
  g_assert_cmpfloat (cfg->class_attrs[0].pre_cluster_threshold, ==, 0.25f);
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  g_free (tilde);
  rds_infer_config_free (cfg);
}

/* Numbers: trailing text, inf and nan, out-of-range integers and floats. */
static void
test_numbers (void)
{
  text_fails ("pgie.txt", DETECTOR_TXT "batch-size=12abc\n", RDS_INFER_CONFIG_ERROR_INVALID, "batch-size: 12abc: ",
      "not an integer", NULL);
  text_fails ("pgie.txt", DETECTOR_TXT "batch-size=99999999999999999999\n", RDS_INFER_CONFIG_ERROR_INVALID,
      "batch-size: 99999999999999999999: ", "out of range", NULL);
  text_fails ("pgie.txt", DETECTOR_TXT "net-scale-factor=inf\n", RDS_INFER_CONFIG_ERROR_INVALID,
      "net-scale-factor: inf: ", "not a finite number", NULL);
  text_fails ("pgie.txt", DETECTOR_TXT "net-scale-factor=nan\n", RDS_INFER_CONFIG_ERROR_INVALID,
      "net-scale-factor: nan: ", "not a finite number", NULL);
  text_fails ("pgie.txt", DETECTOR_TXT "net-scale-factor=1e-310\n", RDS_INFER_CONFIG_ERROR_INVALID,
      "net-scale-factor: 1e-310: ", "out of range", NULL);
  text_fails ("pgie.txt", DETECTOR_TXT "net-scale-factor=1e39\n", RDS_INFER_CONFIG_ERROR_INVALID,
      "net-scale-factor: 1e39: ", "out of range", NULL);
  text_fails ("pgie.txt", DETECTOR_TXT "offsets=1;1e-50;2\n", RDS_INFER_CONFIG_ERROR_INVALID,
      "offsets: 1;1e-50;2: ", "out of range", NULL);
}

/* Odd key-file values: a backslash (invalid key-file escape) shows the raw text; an empty rejected key is unset;
 * class ids are written without leading zeros. */
static void
test_keyfile_values (void)
{
  RdsInferConfig *cfg;

  load_fails ("backslash.txt", RDS_INFER_CONFIG_ERROR_INVALID, "onnx-file: C:\\models\\yolo.onnx: ", NULL);
  cfg = load_ok ("empty-rejected.txt");
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_infer_config_free (cfg);
  load_fails ("class-attrs-zero.txt", RDS_INFER_CONFIG_ERROR_INVALID, "class-attrs-00: ", NULL);
}

/* The YAML reader on its own: groups, scalars, sequences. */
static void
test_yaml_keyfile (void)
{
  gchar *path = corpus ("minimal.yml");
  GError *err = NULL;
  GKeyFile *kf = rds_keyfile_from_yaml (path, &err);
  gchar *v;

  g_assert_no_error (err);
  g_assert_true (g_key_file_has_group (kf, "property"));
  g_assert_true (g_key_file_has_group (kf, "class-attrs-1"));
  v = g_key_file_get_string (kf, "property", "offsets", NULL);
  g_assert_cmpstr (v, ==, "1.5;2.5;3.5");
  g_free (v);
  v = g_key_file_get_string (kf, "property", "filter-out-class-ids", NULL);
  g_assert_cmpstr (v, ==, "0;2");
  g_free (v);
  v = g_key_file_get_string (kf, "property", "symmetric-padding", NULL);
  g_assert_cmpstr (v, ==, "true");
  g_free (v);
  g_key_file_unref (kf);
  g_free (path);

  path = corpus ("no-such-config.yml");
  g_assert_null (rds_keyfile_from_yaml (path, &err));
  g_assert_error (err, G_FILE_ERROR, G_FILE_ERROR_NOENT);
  g_clear_error (&err);
  g_free (path);
}

static gchar *
labels_config_text (guint num_classes)
{
  return g_strdup_printf ("[property]\nonnx-file=m.onnx\nnum-detected-classes=%u\n"
      "parse-bbox-func-name=RdsInferParseYolo\nlabelfile-path=labels.txt\n", num_classes);
}

/* Writes @labels (raw bytes) and a config using it into a fresh temp dir, then loads it. */
static RdsInferConfig *
load_with_labels (const gchar * labels, guint num_classes)
{
  gchar *text = labels_config_text (num_classes), *path = tmp_config ("pgie.txt", text, labels);
  GError *err = NULL;
  RdsInferConfig *cfg = rds_infer_config_load (path, &err);

  g_assert_no_error (err);
  g_assert_nonnull (cfg);
  tmp_cleanup (path);
  g_free (text);
  return cfg;
}

static void
labels_fail (const gchar * labels, guint num_classes, ...)
{
  gchar *text = labels_config_text (num_classes), *path = tmp_config ("pgie.txt", text, labels);
  va_list ap;

  va_start (ap, num_classes);
  assert_load_fails (path, RDS_INFER_CONFIG_ERROR_INVALID, ap);
  va_end (ap);
  tmp_cleanup (path);
  g_free (text);
}

static void
assert_labels (const gchar * file, guint n, const gchar * const *want, guint n_warnings)
{
  RdsInferConfig *cfg = load_with_labels (file, n);

  g_assert_cmpuint (g_strv_length (cfg->labels), ==, n);
  for (guint i = 0; i < n; i++)
    g_assert_cmpstr (cfg->labels[i], ==, want[i]);
  g_assert_cmpuint (cfg->warnings->len, ==, n_warnings);
  rds_infer_config_free (cfg);
}

static void
test_labels (void)
{
  const gchar *abc[] = { "person", "car", "traffic light" };
  const gchar *short_labels[] = { "person", "car", "", "" };
  gchar *long_label = g_strnfill (200, 'x');
  gchar *long_file = g_strdup_printf ("a\n%s\nc\n", long_label);
  const gchar *with_long[] = { "a", long_label, "c" };

  assert_labels ("person\ncar\ntraffic light\n", 3, abc, 0);
  assert_labels ("person\ncar\ntraffic light", 3, abc, 0);   /* no final newline */
  assert_labels ("person;car;traffic light\n", 3, abc, 0);   /* one ';'-separated line */
  assert_labels ("person; car ;traffic light;\n", 3, abc, 0);        /* whitespace, trailing ';' */
  assert_labels ("person\r\ncar\r\ntraffic light\r\n", 3, abc, 0);   /* CRLF */
  assert_labels ("person\ncar\ntraffic light\n\n\n  \n", 3, abc, 0); /* trailing blank lines */
  assert_labels ("  person \n\tcar\ntraffic light  \n", 3, abc, 0);  /* trimmed */
  assert_labels ("person\ncar\n", 4, short_labels, 1);       /* fewer: warning, empty labels */
  assert_labels (long_file, 3, with_long, 0);                 /* kept whole */
  assert_labels ("person\ncar\ntraffic light\nextra\n", 3, abc, 1);  /* more: warning, extra ignored */
  /* labels go into metadata and messages: invalid UTF-8 is an error naming the file and line */
  labels_fail ("person\ncar\nbad \xff label\n", 3, "labelfile-path: ", "labels.txt:3: ", "UTF-8", NULL);
  g_free (long_file);
  g_free (long_label);
}

/* Element properties override fields after loading; validate re-checks them. */
static void
test_validate (void)
{
  RdsInferConfig *cfg = load_ok ("defaults.txt");
  GError *err = NULL;

  const gchar *overridden[] = { "process-mode", NULL };
  gchar *prefix = g_strconcat (cfg->config_path, ":process-mode: 2: ", NULL);

  g_assert_true (rds_infer_config_validate (cfg, NULL, &err));
  g_assert_no_error (err);
  cfg->process_mode = 2;
  /* from the file: FILE:KEY */
  g_assert_false (rds_infer_config_validate (cfg, NULL, &err));
  g_assert_error (err, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_UNSUPPORTED);
  g_assert_true (g_str_has_prefix (err->message, prefix));
  g_clear_error (&err);
  /* from an element property: named as such */
  g_assert_false (rds_infer_config_validate (cfg, overridden, &err));
  g_assert_error (err, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_UNSUPPORTED);
  g_assert_true (g_str_has_prefix (err->message, "element property process-mode: 2: "));
  g_clear_error (&err);
  cfg->process_mode = 1;
  cfg->batch_size = 4;
  cfg->interval = 3;
  g_assert_true (rds_infer_config_validate (cfg, overridden, &err));
  g_free (prefix);
  rds_infer_config_free (cfg);
}

static void
collect_key (const RdsInferKeyDesc * key, gpointer user_data)
{
  g_assert_nonnull (key->type);
  g_assert_true (key->status == RDS_INFER_KEY_SUPPORTED || key->note);
  g_hash_table_insert (user_data, g_strconcat (key->group, "/", key->name, NULL), GINT_TO_POINTER (key->status + 1));
  if (!strcmp (key->name, "batch-size")) {
    g_assert_cmpstr (key->type, ==, "uint");
    g_assert_cmpstr (key->default_value, ==, "1");
    g_assert_cmpstr (key->range, ==, "1..1024");
    g_assert_null (key->accepted);
  } else if (!strcmp (key->name, "net-scale-factor")) {
    g_assert_cmpstr (key->type, ==, "float");
    g_assert_cmpstr (key->range, ==, "> 0");
  } else if (!strcmp (key->name, "cluster-mode")) {
    g_assert_cmpstr (key->range, ==, "0..4");
    g_assert_cmpstr (key->accepted, ==, "2, 4");
  } else if (!strcmp (key->name, "roi-top-offset")) {
    /* unsigned, capped below G_MAXUINT: the real bound */
    g_assert_cmpstr (key->range, ==, "0..2147483647");
  } else if (!strcmp (key->name, "gpu-id")) {
    g_assert_cmpstr (key->range, ==, ">= 0");
  } else if (!strcmp (key->name, "topk")) {
    g_assert_cmpstr (key->range, ==, ">= -1");
  } else if (!strcmp (key->name, "onnx-file")) {
    g_assert_cmpstr (key->type, ==, "path");
    g_assert_null (key->default_value);
    g_assert_null (key->range);
  }
}

static void
test_key_table (void)
{
  GHashTable *keys = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  GError *err = NULL;

  /* the table is consistent: fields, ranges, accepted values, defaults */
  g_assert_true (rds_infer_config_check_table (&err));
  g_assert_no_error (err);
  rds_infer_config_foreach_key (collect_key, keys);
#define STATUS(k) (GPOINTER_TO_INT (g_hash_table_lookup (keys, k)) - 1)
  g_assert_cmpint (STATUS ("property/onnx-file"), ==, RDS_INFER_KEY_SUPPORTED);
  g_assert_cmpint (STATUS ("property/num-detected-classes"), ==, RDS_INFER_KEY_SUPPORTED);
  g_assert_cmpint (STATUS ("property/workspace-size"), ==, RDS_INFER_KEY_NOOP);
  g_assert_cmpint (STATUS ("property/mean-file"), ==, RDS_INFER_KEY_REJECTED);
  g_assert_cmpint (STATUS ("class-attrs/nms-iou-threshold"), ==, RDS_INFER_KEY_SUPPORTED);
  g_assert_cmpint (STATUS ("class-attrs/eps"), ==, RDS_INFER_KEY_NOOP);
  g_assert_cmpint (STATUS ("property/no-such-key"), ==, -1);
#undef STATUS
  g_hash_table_unref (keys);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/infer-config/txt-yaml-equal", test_txt_yaml_equal);
  g_test_add_func ("/cpu/infer-config/defaults", test_defaults);
  g_test_add_func ("/cpu/infer-config/offsets", test_offsets);
  g_test_add_func ("/cpu/infer-config/class-attrs", test_class_attrs);
  g_test_add_func ("/cpu/infer-config/relative-paths", test_relative_paths);
  g_test_add_func ("/cpu/infer-config/rejected", test_rejected);
  g_test_add_func ("/cpu/infer-config/required", test_required);
  g_test_add_func ("/cpu/infer-config/noop-warnings", test_noop_warnings);
  g_test_add_func ("/cpu/infer-config/wrong-types", test_wrong_types);
  g_test_add_func ("/cpu/infer-config/missing", test_missing);
  g_test_add_func ("/cpu/infer-config/malformed-yaml", test_malformed_yaml);
  g_test_add_func ("/cpu/infer-config/yaml-keyfile", test_yaml_keyfile);
  g_test_add_func ("/cpu/infer-config/yaml-null", test_yaml_null);
  g_test_add_func ("/cpu/infer-config/numbers", test_numbers);
  g_test_add_func ("/cpu/infer-config/keyfile-values", test_keyfile_values);
  g_test_add_func ("/cpu/infer-config/labels", test_labels);
  g_test_add_func ("/cpu/infer-config/validate", test_validate);
  g_test_add_func ("/cpu/infer-config/key-table", test_key_table);
  return g_test_run ();
}
