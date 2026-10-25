/* rdstracker config parser: the YAML corpus (tests/check/tracker-configs), defaults, every key's type and range,
 * errors, warnings, the key table.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tracker-config.h"
#include "rds-keyfile-yaml.h"

#include <glib/gstdio.h>
#include <string.h>

static gchar *
corpus (const gchar * name)
{
  return g_build_filename (RDS_TRACKER_CONFIGS, name, NULL);
}

static void
print_warnings (RdsTrackerConfig * cfg)
{
  for (guint i = 0; i < cfg->warnings->len; i++)
    g_test_message ("warning: %s", (const gchar *) g_ptr_array_index (cfg->warnings, i));
}

static RdsTrackerConfig *
load_path_ok (const gchar * path)
{
  GError *err = NULL;
  RdsTrackerConfig *cfg = rds_tracker_config_load (path, &err);

  g_assert_no_error (err);
  g_assert_nonnull (cfg);
  print_warnings (cfg);
  return cfg;
}

static RdsTrackerConfig *
load_ok (const gchar * name)
{
  gchar *path = corpus (name);
  RdsTrackerConfig *cfg = load_path_ok (path);

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
  RdsTrackerConfig *cfg = rds_tracker_config_load (path, &err);
  const gchar *pos, *want;

  g_assert_null (cfg);
  g_assert_error (err, RDS_TRACKER_CONFIG_ERROR, code);
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

/* A one-off config: @text written to a fresh temp dir. Returns the path; tmp_cleanup removes the file and dir. */
static gchar *
tmp_config (const gchar * text)
{
  GError *err = NULL;
  gchar *dir = g_dir_make_tmp ("rds-tracker-config-XXXXXX", &err);
  gchar *path = g_build_filename (dir, "tracker.yml", NULL);

  g_assert_no_error (err);
  g_assert_true (g_file_set_contents (path, text, -1, NULL));
  g_free (dir);
  return path;
}

static void
tmp_cleanup (gchar * path)
{
  gchar *dir = g_path_get_dirname (path);

  g_unlink (path);
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

static void
text_fails (const gchar * text, gint code, ...)
{
  gchar *path = tmp_config (text);
  va_list ap;

  va_start (ap, code);
  assert_load_fails (path, code, ap);
  va_end (ap);
  tmp_cleanup (path);
}

static RdsTrackerConfig *
text_ok (const gchar * text)
{
  gchar *path = tmp_config (text);
  RdsTrackerConfig *cfg = load_path_ok (path);

  tmp_cleanup (path);
  return cfg;
}

static guint
count_warnings (RdsTrackerConfig * cfg, const gchar * needle)
{
  guint n = 0;

  for (guint i = 0; i < cfg->warnings->len; i++)
    if (strstr (g_ptr_array_index (cfg->warnings, i), needle))
      n++;
  return n;
}

/* The documented defaults (ours for usePrediction4Assoc, which is 0 for the IOU tracker that the defaults give). */
static void
assert_defaults (const RdsTrackerConfig * cfg)
{
  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_IOU);
  g_assert_cmpfloat (cfg->min_detector_confidence, ==, 0.0f);
  g_assert_true (cfg->check_class_match);
  g_assert_cmpuint (cfg->max_targets_per_stream, ==, 30);
  g_assert_cmpfloat (cfg->min_iou_diff4new_target, ==, 0.5f);
  g_assert_cmpuint (cfg->probation_age, ==, 5);
  g_assert_cmpuint (cfg->max_shadow_tracking_age, ==, 38);
  g_assert_cmpuint (cfg->early_termination_age, ==, 2);
  g_assert_false (cfg->use_unique_id);
  g_assert_cmpint (cfg->association_matcher_type, ==, 0);
  g_assert_cmpfloat (cfg->min_matching_score4overall, ==, 0.0f);
  g_assert_cmpfloat (cfg->min_matching_score4size_similarity, ==, 0.0f);
  g_assert_cmpfloat (cfg->min_matching_score4iou, ==, 0.0f);
  g_assert_cmpfloat (cfg->matching_score_weight4size_similarity, ==, 0.0f);
  g_assert_cmpfloat (cfg->matching_score_weight4iou, ==, 1.0f);
  g_assert_cmpfloat (cfg->tentative_detector_confidence, ==, 0.5f);
  g_assert_cmpfloat (cfg->min_matching_score4tentative_iou, ==, 0.0f);
  g_assert_false (cfg->use_prediction4assoc);
  g_assert_cmpint (cfg->state_estimator_type, ==, 0);
  g_assert_cmpfloat (cfg->process_noise_var4loc, ==, 2.0f);
  g_assert_cmpfloat (cfg->process_noise_var4size, ==, 1.0f);
  g_assert_cmpfloat (cfg->process_noise_var4vel, ==, 0.1f);
  g_assert_cmpfloat (cfg->measurement_noise_var4detector, ==, 4.0f);
  g_assert_cmpfloat (cfg->measurement_noise_var4tracker, ==, 16.0f);
  g_assert_cmpfloat (cfg->noise_weight_var4loc, ==, -0.1f);
  g_assert_cmpfloat (cfg->noise_weight_var4vel, ==, -0.1f);
  g_assert_false (cfg->use_aspect_ratio);
}

/* No file: every default, no warnings. */
static void
test_defaults (void)
{
  RdsTrackerConfig *cfg = load_path_ok (NULL);

  g_assert_null (cfg->config_path);
  assert_defaults (cfg);
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_tracker_config_free (cfg);

  /* an empty file and empty sections give the same */
  cfg = text_ok ("%YAML:1.0\n");
  assert_defaults (cfg);
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_tracker_config_free (cfg);
  cfg = text_ok ("BaseConfig:\nTargetManagement: ~\n");
  assert_defaults (cfg);
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_tracker_config_free (cfg);
}

static void
test_iou (void)
{
  RdsTrackerConfig *cfg = load_ok ("iou.yml");

  g_assert_nonnull (strstr (cfg->config_path, "iou.yml"));
  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_IOU);
  g_assert_cmpstr (rds_tracker_type_name (cfg->tracker_type), ==, "IOU");
  g_assert_cmpfloat (cfg->min_detector_confidence, ==, 0.2f);
  g_assert_false (cfg->check_class_match);      /* from DataAssociator */
  g_assert_cmpuint (cfg->max_targets_per_stream, ==, 64);
  g_assert_cmpfloat (cfg->min_iou_diff4new_target, ==, 0.4f);
  g_assert_cmpuint (cfg->probation_age, ==, 3);
  g_assert_cmpuint (cfg->max_shadow_tracking_age, ==, 10);
  g_assert_cmpuint (cfg->early_termination_age, ==, 1);
  g_assert_cmpfloat (cfg->min_matching_score4overall, ==, 0.1f);
  g_assert_cmpfloat (cfg->min_matching_score4size_similarity, ==, 0.2f);
  g_assert_cmpfloat (cfg->min_matching_score4iou, ==, 0.3f);
  g_assert_cmpfloat (cfg->matching_score_weight4size_similarity, ==, 0.25f);
  g_assert_cmpfloat (cfg->matching_score_weight4iou, ==, 0.75f);
  g_assert_false (cfg->use_prediction4assoc);   /* ours: 0 for IOU */
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_tracker_config_free (cfg);
}

/* Every supported key, set to other than its default. */
static void
test_nvsort (void)
{
  RdsTrackerConfig *cfg = load_ok ("nvsort.yml");

  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_NVSORT);
  g_assert_cmpstr (rds_tracker_type_name (cfg->tracker_type), ==, "NvSORT");
  g_assert_cmpfloat (cfg->min_detector_confidence, ==, 0.05f);
  g_assert_false (cfg->check_class_match);      /* from BaseConfig */
  g_assert_cmpuint (cfg->max_targets_per_stream, ==, 200);
  g_assert_cmpfloat (cfg->min_iou_diff4new_target, ==, 0.7f);
  g_assert_cmpuint (cfg->probation_age, ==, 4);
  g_assert_cmpuint (cfg->max_shadow_tracking_age, ==, 40);
  g_assert_cmpuint (cfg->early_termination_age, ==, 3);
  g_assert_true (cfg->use_unique_id);
  g_assert_cmpint (cfg->association_matcher_type, ==, 1);
  g_assert_cmpfloat (cfg->min_matching_score4overall, ==, 0.15f);
  g_assert_cmpfloat (cfg->min_matching_score4size_similarity, ==, 0.25f);
  g_assert_cmpfloat (cfg->min_matching_score4iou, ==, 0.35f);
  g_assert_cmpfloat (cfg->matching_score_weight4size_similarity, ==, 0.3f);
  g_assert_cmpfloat (cfg->matching_score_weight4iou, ==, 0.6f);
  g_assert_cmpfloat (cfg->tentative_detector_confidence, ==, 0.125f);
  g_assert_cmpfloat (cfg->min_matching_score4tentative_iou, ==, 0.45f);
  g_assert_false (cfg->use_prediction4assoc);   /* set: the NvSORT default (1) doesn't apply */
  g_assert_cmpint (cfg->state_estimator_type, ==, 2);
  g_assert_cmpfloat (cfg->process_noise_var4loc, ==, 3.5f);
  g_assert_cmpfloat (cfg->process_noise_var4size, ==, 1.5f);
  g_assert_cmpfloat (cfg->process_noise_var4vel, ==, 0.25f);
  g_assert_cmpfloat (cfg->measurement_noise_var4detector, ==, 5.0f);
  g_assert_cmpfloat (cfg->measurement_noise_var4tracker, ==, 12.0f);
  g_assert_cmpfloat (cfg->noise_weight_var4loc, ==, 0.08f);
  g_assert_cmpfloat (cfg->noise_weight_var4vel, ==, 0.005f);
  g_assert_true (cfg->use_aspect_ratio);
  /* minTrackerConfidence is accepted silently (no effect for IOU and NvSORT) */
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_tracker_config_free (cfg);
}

/* IOU only when both the state estimator and the matcher are 0; usePrediction4Assoc defaults to 1 for NvSORT. */
static void
test_tracker_type (void)
{
  RdsTrackerConfig *cfg;

  cfg = text_ok ("DataAssociator:\n  associationMatcherType: 1\n");
  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_NVSORT);
  g_assert_true (cfg->use_prediction4assoc);
  rds_tracker_config_free (cfg);
  cfg = text_ok ("StateEstimator:\n  stateEstimatorType: 1\n");
  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_NVSORT);
  g_assert_cmpint (cfg->state_estimator_type, ==, 1);
  g_assert_true (cfg->use_prediction4assoc);
  rds_tracker_config_free (cfg);
  cfg = text_ok ("StateEstimator:\n  stateEstimatorType: 0\nDataAssociator:\n  associationMatcherType: 0\n"
      "  usePrediction4Assoc: true\n");
  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_IOU);
  g_assert_true (cfg->use_prediction4assoc);
  rds_tracker_config_free (cfg);
}

/* The OpenCV-style "%YAML:1.0" line, in the shared reader and through the parser; line numbers still count it. */
static void
test_yaml_header (void)
{
  gchar *path = tmp_config ("%YAML:1.0\n---\nBaseConfig:\n  minDetectorConfidence: 0.5\n");
  GError *err = NULL;
  GKeyFile *kf = rds_keyfile_from_yaml (path, &err);
  gchar *v;
  RdsTrackerConfig *cfg;

  g_assert_no_error (err);
  v = g_key_file_get_string (kf, "BaseConfig", "minDetectorConfidence", NULL);
  g_assert_cmpstr (v, ==, "0.5");
  g_free (v);
  g_key_file_unref (kf);
  tmp_cleanup (path);

  /* without a document start, as tracker configs are usually written */
  cfg = text_ok ("%YAML:1.0\n# comment\nBaseConfig:\n  minDetectorConfidence: 0.5\n");
  g_assert_cmpfloat (cfg->min_detector_confidence, ==, 0.5f);
  rds_tracker_config_free (cfg);
  text_fails ("%YAML:1.0\nBaseConfig:\n  probationAge: [1\n", RDS_TRACKER_CONFIG_ERROR_PARSE, "4: ", NULL);
  /* a standard YAML directive still works with a document start */
  cfg = text_ok ("%YAML 1.1\n---\nBaseConfig:\n  minDetectorConfidence: 0.25\n");
  g_assert_cmpfloat (cfg->min_detector_confidence, ==, 0.25f);
  rds_tracker_config_free (cfg);
}

static void
test_unknown (void)
{
  RdsTrackerConfig *cfg = load_ok ("unknown.yml");

  g_assert_cmpuint (count_warnings (cfg, "BaseConfig.noSuchKey: unknown key"), ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "TargetManagement.anotherUnknownKey: unknown key"), ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "MysterySection: unknown section"), ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "someKey"), ==, 0);    /* one warning for the whole section */
  g_assert_cmpuint (cfg->warnings->len, ==, 3);
  g_assert_cmpfloat (cfg->min_detector_confidence, ==, 0.3f);
  rds_tracker_config_free (cfg);
}

/* VisualTracker and ReID: one warning naming both; their keys and the NvDCF/NvDeepSORT-only keys elsewhere are
 * type-checked, nothing more. */
static void
test_ignored_sections (void)
{
  RdsTrackerConfig *cfg = load_ok ("ignored-sections.yml");

  g_assert_cmpuint (cfg->warnings->len, ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "VisualTracker, ReID: "), ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "runs as NvSORT"), ==, 1);
  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_NVSORT);
  rds_tracker_config_free (cfg);

  /* the type-only check still finds bad values */
  text_fails ("VisualTracker:\n  featureImgSizeLevel: 9\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "VisualTracker.featureImgSizeLevel: 9: ", "at most 5", NULL);
  text_fails ("ReID:\n  inferDims: [128, x, 3]\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "ReID.inferDims: 128;x;3: ", "not an integer", NULL);
  text_fails ("PoseEstimator:\n  networkMode: 3\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "PoseEstimator.networkMode: 3: ", NULL);

  /* the model inference keys are valid under ReID and PoseEstimator, and nowhere else */
  cfg = text_ok ("PoseEstimator:\n  poseEstimatorType: 1\n  onnxFile: pose.onnx\nObjectModelProjection:\n"
      "  waistHeightRatio: 0.4\n");
  g_assert_cmpuint (cfg->warnings->len, ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "ObjectModelProjection, PoseEstimator: "), ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "runs as IOU"), ==, 1);
  rds_tracker_config_free (cfg);
  cfg = text_ok ("VisualTracker:\n  onnxFile: x.onnx\n");
  g_assert_cmpuint (count_warnings (cfg, "VisualTracker.onnxFile: unknown key"), ==, 1);
  rds_tracker_config_free (cfg);
}

static void
test_se_type3 (void)
{
  RdsTrackerConfig *cfg = load_ok ("se-type3.yml");

  g_assert_cmpint (cfg->state_estimator_type, ==, 2);
  g_assert_cmpint (cfg->tracker_type, ==, RDS_TRACKER_TYPE_NVSORT);
  g_assert_cmpuint (cfg->warnings->len, ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "StateEstimator.stateEstimatorType: 3: "), ==, 1);
  rds_tracker_config_free (cfg);
}

static void
test_check_class_match (void)
{
  RdsTrackerConfig *cfg;

  load_fails ("ccm-conflict.yml", RDS_TRACKER_CONFIG_ERROR_INVALID, "DataAssociator.checkClassMatch: 0: ",
      "BaseConfig.checkClassMatch", NULL);
  cfg = load_ok ("ccm-both.yml");
  g_assert_false (cfg->check_class_match);
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_tracker_config_free (cfg);

  cfg = load_ok ("deprecated.yml");
  g_assert_true (cfg->check_class_match);
  g_assert_cmpuint (cfg->warnings->len, ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "TrajectoryManagement.checkClassMatchForReAssoc: 1: "), ==, 1);
  g_assert_cmpuint (count_warnings (cfg, "deprecated"), ==, 1);
  rds_tracker_config_free (cfg);
  /* deprecated keys warn whatever their value */
  cfg = text_ok ("TrajectoryManagement:\n  checkClassMatchForReAssoc: 0\n");
  g_assert_cmpuint (cfg->warnings->len, ==, 1);
  rds_tracker_config_free (cfg);
}

static void
test_noop (void)
{
  const gchar *noop[] = { "BaseConfig.useBatchNumForFrameId: 1: ", "TargetManagement.preserveStreamUpdateOrder: 1: ",
    "TargetManagement.enableBboxUnClipping: 1: ", "TargetManagement.outputTerminatedTracks: 1: ",
    "TargetManagement.outputShadowTracks: 1: ", "TargetManagement.outputVisibility: 1: ",
    "TargetManagement.terminatedTrackFilename: dump/track: ", "TrajectoryManagement.enableReAssoc: 1: ",
  };
  RdsTrackerConfig *cfg = load_ok ("noop.yml");

  for (guint i = 0; i < G_N_ELEMENTS (noop); i++)
    if (count_warnings (cfg, noop[i]) != 1)
      g_error ("expected one warning for %s", noop[i]);
  /* the other re-association keys are covered by enableReAssoc's warning */
  g_assert_cmpuint (cfg->warnings->len, ==, G_N_ELEMENTS (noop));
  rds_tracker_config_free (cfg);

  cfg = load_ok ("noop-default.yml");
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  rds_tracker_config_free (cfg);
}

static void
test_errors (void)
{
  gchar *path = corpus ("no-such-config.yml");
  GError *err = NULL;

  g_assert_null (rds_tracker_config_load (path, &err));
  g_assert_error (err, RDS_TRACKER_CONFIG_ERROR, RDS_TRACKER_CONFIG_ERROR_FILE);
  g_assert_nonnull (strstr (err->message, path));
  g_clear_error (&err);
  g_free (path);

  load_fails ("malformed.yml", RDS_TRACKER_CONFIG_ERROR_PARSE, "4: ", NULL);
  text_fails ("BaseConfig:\n  minDetectorConfidence: high\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "BaseConfig.minDetectorConfidence: high: ", "not a number", NULL);
  text_fails ("BaseConfig:\n  checkClassMatch: maybe\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "BaseConfig.checkClassMatch: maybe: ", "not a boolean", NULL);
  text_fails ("TargetManagement:\n  probationAge: -1\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "TargetManagement.probationAge: -1: ", "at least 0", NULL);
  text_fails ("TargetManagement:\n  maxTargetsPerStream: 65536\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "TargetManagement.maxTargetsPerStream: 65536: ", "at most 65535", NULL);
  text_fails ("TargetManagement:\n  probationAge: 2.5\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "TargetManagement.probationAge: 2.5: ", "not an integer", NULL);
  text_fails ("DataAssociator:\n  minMatchingScore4Iou: 1.5\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "DataAssociator.minMatchingScore4Iou: 1.5: ", "at most 1", NULL);
  text_fails ("DataAssociator:\n  dataAssociatorType: 1\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "DataAssociator.dataAssociatorType: 1: ", "at most 0", NULL);
  text_fails ("StateEstimator:\n  processNoiseVar4Loc: nan\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "StateEstimator.processNoiseVar4Loc: nan: ", "not a finite number", NULL);
  text_fails ("StateEstimator:\n  stateEstimatorType: 4\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "StateEstimator.stateEstimatorType: 4: ", NULL);
  text_fails ("BaseConfig:\n  minDetectorConfidence:\n", RDS_TRACKER_CONFIG_ERROR_INVALID,
      "BaseConfig.minDetectorConfidence: : ", NULL);
  text_fails ("BaseConfig: 3\n", RDS_TRACKER_CONFIG_ERROR_PARSE, "1: ", NULL);
}

/* Every key in the table: its default parses back, a non-number is rejected for numbers and bools, and values just
 * outside its range are rejected with "FILE:SECTION.KEY: VALUE: ". */
typedef struct
{
  guint n_keys, n_status[4];
  GHashTable *names;            /* "Section.key" */
} KeyWalk;

static const gchar *
yaml_section (const RdsTrackerKeyDesc * key)
{
  /* the model inference keys: write them under ReID */
  return strchr (key->section, ',') ? "ReID" : key->section;
}

static void
walk_key (const RdsTrackerKeyDesc * key, gpointer user_data)
{
  KeyWalk *kw = user_data;
  const gchar *section = yaml_section (key);
  gboolean number = key->range != NULL, list = g_str_has_suffix (key->type, " list");

  kw->n_keys++;
  kw->n_status[key->status]++;
  g_hash_table_add (kw->names, g_strconcat (key->section, ".", key->name, NULL));
  g_assert_nonnull (key->trackers);
  if (key->status != RDS_TRACKER_KEY_SUPPORTED)
    g_assert_nonnull (key->note);

  if (key->default_value) {
    gchar *text = g_strdup_printf ("%s:\n  %s: %s\n", section, key->name, key->default_value);
    gchar *needle = g_strconcat (".", key->name, ": ", NULL);
    RdsTrackerConfig *cfg = text_ok (text);

    /* a default never warns about the key, except for deprecated keys (an ignored section warns about itself) */
    g_assert_cmpuint (count_warnings (cfg, needle), ==, key->status == RDS_TRACKER_KEY_DEPRECATED ? 1 : 0);
    rds_tracker_config_free (cfg);
    g_free (needle);
    g_free (text);
  }
  if (number || !strcmp (key->type, "bool")) {
    gchar *text = g_strdup_printf ("%s:\n  %s: %s\n", section, key->name, list ? "[abc]" : "abc");
    gchar *want = g_strdup_printf ("%s.%s: abc: ", section, key->name);

    text_fails (text, RDS_TRACKER_CONFIG_ERROR_INVALID, want, NULL);
    g_free (want);
    g_free (text);
  }
  if (number && strcmp (key->range, "any") != 0) {
    gchar **ends = g_strsplit (g_str_has_prefix (key->range, ">= ") ? key->range + 3 : key->range, "..", 2);
    gdouble bounds[2] = { g_ascii_strtod (ends[0], NULL) - 1, ends[1] ? g_ascii_strtod (ends[1], NULL) + 1 : 0 };

    for (guint b = 0; b < (ends[1] ? 2u : 1u); b++) {
      gchar num[G_ASCII_DTOSTR_BUF_SIZE];
      gchar *value = g_strdup (g_ascii_formatd (num, sizeof (num), "%g", bounds[b]));
      gchar *text = g_strdup_printf ("%s:\n  %s: %s%s%s\n", section, key->name, list ? "[" : "", value,
          list ? "]" : "");
      gchar *want = g_strdup_printf ("%s.%s: %s: ", section, key->name, value);

      text_fails (text, RDS_TRACKER_CONFIG_ERROR_INVALID, want, b == 0 ? "at least" : "at most", NULL);
      g_free (want);
      g_free (text);
      g_free (value);
    }
    g_strfreev (ends);
  }
}

/* Every key the public tracker documentation lists, by section. */
static const gchar *const documented[] = {
  "BaseConfig.minDetectorConfidence", "BaseConfig.checkClassMatch", "BaseConfig.useBatchNumForFrameId",
  "TargetManagement.preserveStreamUpdateOrder", "TargetManagement.maxTargetsPerStream",
  "TargetManagement.minIouDiff4NewTarget", "TargetManagement.enableBboxUnClipping", "TargetManagement.probationAge",
  "TargetManagement.maxShadowTrackingAge", "TargetManagement.earlyTerminationAge",
  "TargetManagement.outputTerminatedTracks", "TargetManagement.outputShadowTracks",
  "TargetManagement.outputVisibility", "TargetManagement.terminatedTrackFilename",
  "TargetManagement.minTrackerConfidence", "TargetManagement.searchRegionPaddingScale",
  "TrajectoryManagement.useUniqueID", "TrajectoryManagement.enableReAssoc",
  "TrajectoryManagement.minMatchingScore4Overall", "TrajectoryManagement.minTrackletMatchingScore",
  "TrajectoryManagement.minMatchingScore4ReidSimilarity", "TrajectoryManagement.matchingScoreWeight4TrackletSimilarity",
  "TrajectoryManagement.matchingScoreWeight4ReidSimilarity", "TrajectoryManagement.minTrajectoryLength4Projection",
  "TrajectoryManagement.prepLength4TrajectoryProjection", "TrajectoryManagement.trajectoryProjectionLength",
  "TrajectoryManagement.maxAngle4TrackletMatching", "TrajectoryManagement.minSpeedSimilarity4TrackletMatching",
  "TrajectoryManagement.minBboxSizeSimilarity4TrackletMatching",
  "TrajectoryManagement.maxTrackletMatchingTimeSearchRange",
  "TrajectoryManagement.trajectoryProjectionProcessNoiseScale",
  "TrajectoryManagement.trajectoryProjectionMeasurementNoiseScale",
  "TrajectoryManagement.trackletSpacialSearchRegionScale", "TrajectoryManagement.reidExtractionInterval",
  "TrajectoryManagement.checkClassMatchForReAssoc",
  "DataAssociator.dataAssociatorType", "DataAssociator.associationMatcherType", "DataAssociator.checkClassMatch",
  "DataAssociator.minMatchingScore4Overall", "DataAssociator.minMatchingScore4SizeSimilarity",
  "DataAssociator.minMatchingScore4Iou", "DataAssociator.matchingScoreWeight4SizeSimilarity",
  "DataAssociator.matchingScoreWeight4Iou", "DataAssociator.tentativeDetectorConfidence",
  "DataAssociator.minMatchingScore4TentativeIou", "DataAssociator.minMatchingScore4VisualSimilarity",
  "DataAssociator.matchingScoreWeight4VisualSimilarity", "DataAssociator.thresholdMahalanobis",
  "DataAssociator.minMatchingScore4ReidSimilarity", "DataAssociator.matchingScoreWeight4ReidSimilarity",
  "DataAssociator.matchingScoreWeight4ReIDSimilarity", "DataAssociator.usePrediction4Assoc",
  "StateEstimator.stateEstimatorType", "StateEstimator.processNoiseVar4Loc", "StateEstimator.processNoiseVar4Size",
  "StateEstimator.processNoiseVar4Vel", "StateEstimator.measurementNoiseVar4Detector",
  "StateEstimator.measurementNoiseVar4Tracker", "StateEstimator.noiseWeightVar4Loc",
  "StateEstimator.noiseWeightVar4Vel", "StateEstimator.useAspectRatio",
  "VisualTracker.visualTrackerType", "VisualTracker.useColorNames", "VisualTracker.useHog",
  "VisualTracker.featureImgSizeLevel", "VisualTracker.featureFocusOffsetFactor_y",
  "VisualTracker.useHighPrecisionFeature", "VisualTracker.filterLr", "VisualTracker.filterChannelWeightsLr",
  "VisualTracker.gaussianSigma", "VisualTracker.vpiBackend4DcfTracker",
  "ReID.reidType", "ReID.reidFeatureSize", "ReID.reidHistorySize", "ReID.addFeatureNormalization", "ReID.keepAspc",
  "ReID.outputReidTensor",
  "ReID, PoseEstimator.batchSize", "ReID, PoseEstimator.workspaceSize", "ReID, PoseEstimator.inferDims",
  "ReID, PoseEstimator.inputOrder", "ReID, PoseEstimator.colorFormat", "ReID, PoseEstimator.networkMode",
  "ReID, PoseEstimator.offsets", "ReID, PoseEstimator.netScaleFactor", "ReID, PoseEstimator.tltEncodedModel",
  "ReID, PoseEstimator.tltModelKey", "ReID, PoseEstimator.onnxFile", "ReID, PoseEstimator.modelEngineFile",
  "ReID, PoseEstimator.calibrationTableFile", "ReID, PoseEstimator.useVPICropScaler",
  "ObjectModelProjection.cameraModelFilepath", "ObjectModelProjection.waistHeightRatio",
  "ObjectModelProjection.outputFootLocation", "ObjectModelProjection.outputConvexHull",
  "ObjectModelProjection.maxConvexHullSize", "ObjectModelProjection.minPoseConfidence",
  "PoseEstimator.poseEstimatorType", "PoseEstimator.poseInferenceInterval", "PoseEstimator.operateOnClassIds",
};

static void
test_key_table (void)
{
  KeyWalk kw = { 0, {0}, g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL) };
  GError *err = NULL;

  g_assert_true (rds_tracker_config_check_table (&err));
  g_assert_no_error (err);
  rds_tracker_config_foreach_key (walk_key, &kw);
  for (guint i = 0; i < G_N_ELEMENTS (documented); i++)
    if (!g_hash_table_contains (kw.names, documented[i]))
      g_error ("%s is not in the key table", documented[i]);
  g_assert_cmpuint (kw.n_keys, ==, G_N_ELEMENTS (documented));
  g_test_message ("%u keys: %u supported, %u no-op, %u ignored, %u deprecated", kw.n_keys,
      kw.n_status[RDS_TRACKER_KEY_SUPPORTED], kw.n_status[RDS_TRACKER_KEY_NOOP],
      kw.n_status[RDS_TRACKER_KEY_IGNORED], kw.n_status[RDS_TRACKER_KEY_DEPRECATED]);
  g_hash_table_unref (kw.names);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/tracker-config/defaults", test_defaults);
  g_test_add_func ("/cpu/tracker-config/iou", test_iou);
  g_test_add_func ("/cpu/tracker-config/nvsort", test_nvsort);
  g_test_add_func ("/cpu/tracker-config/tracker-type", test_tracker_type);
  g_test_add_func ("/cpu/tracker-config/yaml-header", test_yaml_header);
  g_test_add_func ("/cpu/tracker-config/unknown", test_unknown);
  g_test_add_func ("/cpu/tracker-config/ignored-sections", test_ignored_sections);
  g_test_add_func ("/cpu/tracker-config/se-type3", test_se_type3);
  g_test_add_func ("/cpu/tracker-config/check-class-match", test_check_class_match);
  g_test_add_func ("/cpu/tracker-config/noop", test_noop);
  g_test_add_func ("/cpu/tracker-config/errors", test_errors);
  g_test_add_func ("/cpu/tracker-config/key-table", test_key_table);
  return g_test_run ();
}
