/* The custom bbox and classifier parser ABI (rds/rds-infer-custom-impl.h and its NvDsInfer* aliases), rdsinfer's
 * parser loader, against the test modules in infer-parser-fixture.cpp and infer-parser-noabi.c, and the built-in
 * classifier parser on fixed layer data.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds_compat_infer.h>

#include "rds-infer-parse.h"
#include "infer-parser-fixture.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <utility>
#include <dlfcn.h>

/* From infer_abi_noleak.cpp: the unprefixed names are free there. */
int infer_abi_noleak_sum (void);

/* ---- Layout: NVIDIA's documented field order, then ours ---- */

static_assert (RDS_INFER_MAX_DIMS == 8, "");
static_assert (sizeof (RdsInferDims) == (1 + RDS_INFER_MAX_DIMS + 1) * sizeof (unsigned int), "");
static_assert (offsetof (RdsInferDims, numDims) < offsetof (RdsInferDims, d), "");
static_assert (offsetof (RdsInferDims, d) < offsetof (RdsInferDims, numElements), "");
static_assert (offsetof (RdsInferDimsCHW, c) < offsetof (RdsInferDimsCHW, h), "");
static_assert (offsetof (RdsInferDimsCHW, h) < offsetof (RdsInferDimsCHW, w), "");
static_assert (offsetof (RdsInferLayerInfo, dataType) < offsetof (RdsInferLayerInfo, inferDims), "");
static_assert (offsetof (RdsInferLayerInfo, inferDims) < offsetof (RdsInferLayerInfo, bindingIndex), "");
static_assert (offsetof (RdsInferLayerInfo, bindingIndex) < offsetof (RdsInferLayerInfo, layerName), "");
static_assert (offsetof (RdsInferLayerInfo, layerName) < offsetof (RdsInferLayerInfo, buffer), "");
static_assert (offsetof (RdsInferLayerInfo, buffer) < offsetof (RdsInferLayerInfo, isInput), "");
static_assert (offsetof (RdsInferLayerInfo, isInput) < offsetof (RdsInferLayerInfo, deviceBuffer), "");
static_assert (offsetof (RdsInferLayerInfo, deviceBuffer) + sizeof (void *) == sizeof (RdsInferLayerInfo),
    "deviceBuffer is the last field");
static_assert (offsetof (RdsInferNetworkInfo, width) < offsetof (RdsInferNetworkInfo, height), "");
static_assert (offsetof (RdsInferNetworkInfo, height) < offsetof (RdsInferNetworkInfo, channels), "");
static_assert (offsetof (RdsInferObjectDetectionInfo, classId) == 0, "");
static_assert (offsetof (RdsInferObjectDetectionInfo, left) == 4, "");
static_assert (offsetof (RdsInferObjectDetectionInfo, top) == 8, "");
static_assert (offsetof (RdsInferObjectDetectionInfo, width) == 12, "");
static_assert (offsetof (RdsInferObjectDetectionInfo, height) == 16, "");
static_assert (offsetof (RdsInferObjectDetectionInfo, detectionConfidence) == 20, "");
static_assert (offsetof (RdsInferObjectDetectionInfo, rotation_angle) == 24, "");
static_assert (sizeof (RdsInferObjectDetectionInfo) == 7 * 4, "");
static_assert (RDS_INFER_DATA_FLOAT == 0 && RDS_INFER_DATA_HALF == 1 && RDS_INFER_DATA_INT8 == 2
    && RDS_INFER_DATA_INT32 == 3 && RDS_INFER_DATA_INT64 == 4, "");
/* The tensor meta: the documented fields in order, with the documented types. */
#define TM_ORDER(a, b) static_assert (offsetof (RdsInferTensorMeta, a) < offsetof (RdsInferTensorMeta, b), "")
TM_ORDER (unique_id, num_output_layers);
TM_ORDER (num_output_layers, output_layers_info);
TM_ORDER (output_layers_info, out_buf_ptrs_host);
TM_ORDER (out_buf_ptrs_host, out_buf_ptrs_dev);
TM_ORDER (out_buf_ptrs_dev, gpu_id);
TM_ORDER (gpu_id, priv_data);
TM_ORDER (priv_data, network_info);
TM_ORDER (network_info, maintain_aspect_ratio);
TM_ORDER (maintain_aspect_ratio, symmetric_padding);
#undef TM_ORDER
static_assert (offsetof (RdsInferTensorMeta, unique_id) == 0, "");
/* symmetric_padding is the last field (only tail padding after it) */
static_assert (sizeof (RdsInferTensorMeta) - offsetof (RdsInferTensorMeta, symmetric_padding) - sizeof (gboolean)
    < alignof (RdsInferTensorMeta), "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::unique_id), guint>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::num_output_layers), guint>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::output_layers_info), RdsInferLayerInfo *>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::out_buf_ptrs_host), void **>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::out_buf_ptrs_dev), void **>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::gpu_id), gint>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::priv_data), void *>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::network_info), RdsInferNetworkInfo>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::maintain_aspect_ratio), gboolean>::value, "");
static_assert (std::is_same<decltype (RdsInferTensorMeta::symmetric_padding), gboolean>::value, "");
static_assert (RDS_INFER_TENSOR_OUTPUT_META == 12, "");

/* ---- The DeepStream names are the same types ---- */

static_assert (NVDSINFER_MAX_DIMS == RDS_INFER_MAX_DIMS, "");
static_assert (std::is_same<NvDsInferDims, RdsInferDims>::value, "");
static_assert (std::is_same<NvDsInferDimsCHW, RdsInferDimsCHW>::value, "");
static_assert (std::is_same<NvDsInferDataType, RdsInferDataType>::value, "");
static_assert (std::is_same<NvDsInferLayerInfo, RdsInferLayerInfo>::value, "");
static_assert (std::is_same<NvDsInferNetworkInfo, RdsInferNetworkInfo>::value, "");
static_assert (std::is_same<NvDsInferParseDetectionParams, RdsInferParseDetectionParams>::value, "");
static_assert (std::is_same<NvDsInferObjectDetectionInfo, RdsInferObjectDetectionInfo>::value, "");
static_assert (std::is_same<NvDsInferParseObjectInfo, RdsInferObjectDetectionInfo>::value, "");
static_assert (std::is_same<RdsInferParseObjectInfo, RdsInferObjectDetectionInfo>::value, "");
static_assert (std::is_same<NvDsInferParseCustomFunc, RdsInferParseCustomFunc>::value, "");
static_assert (std::is_same<NvDsInferTensorMeta, RdsInferTensorMeta>::value, "");
static_assert (NVDSINFER_TENSOR_OUTPUT_META == RDS_INFER_TENSOR_OUTPUT_META, "");
static_assert (std::is_same<decltype (FLOAT), const RdsInferDataType>::value, "");
static_assert (FLOAT == RDS_INFER_DATA_FLOAT && HALF == RDS_INFER_DATA_HALF && INT8 == RDS_INFER_DATA_INT8
    && INT32 == RDS_INFER_DATA_INT32 && INT64 == RDS_INFER_DATA_INT64, "");

/* Both check macros accept a matching parser defined in this file. */
extern "C" bool AbiTestParse (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferObjectDetectionInfo> &objectList);

extern "C" bool
AbiTestParse (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams, std::vector<NvDsInferObjectDetectionInfo> &objectList)
{
  (void) networkInfo;
  (void) detectionParams;
  objectList.resize (outputLayersInfo.size ());
  return true;
}

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (AbiTestParse);
RDS_CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (AbiTestParse);

/* The classifier ABI: NvDsInferAttribute in its documented order, NVIDIA's spelling of the function type. */
static_assert (offsetof (RdsInferAttribute, attributeIndex) == 0, "");
static_assert (offsetof (RdsInferAttribute, attributeValue) == 4, "");
static_assert (offsetof (RdsInferAttribute, attributeConfidence) == 8, "");
static_assert (offsetof (RdsInferAttribute, attributeConfidence) < offsetof (RdsInferAttribute, attributeLabel), "");
static_assert (offsetof (RdsInferAttribute, attributeLabel) + sizeof (char *) == sizeof (RdsInferAttribute), "");
static_assert (std::is_same<decltype (RdsInferAttribute::attributeIndex), unsigned int>::value, "");
static_assert (std::is_same<decltype (RdsInferAttribute::attributeValue), unsigned int>::value, "");
static_assert (std::is_same<decltype (RdsInferAttribute::attributeConfidence), float>::value, "");
static_assert (std::is_same<decltype (RdsInferAttribute::attributeLabel), char *>::value, "");
static_assert (std::is_same<NvDsInferAttribute, RdsInferAttribute>::value, "");
static_assert (std::is_same<NvDsInferClassiferParseCustomFunc, RdsInferClassifierParseCustomFunc>::value, "");
static_assert (std::is_same<RdsInferClassifierParseCustomFunc, bool (*) (std::vector<RdsInferLayerInfo> const &,
        RdsInferNetworkInfo const &, float, std::vector<RdsInferAttribute> &, std::string &)>::value, "");

extern "C" bool AbiTestClassify (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);

extern "C" bool
AbiTestClassify (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    float classifierThreshold, std::vector<NvDsInferAttribute> &attrList, std::string &descString)
{
  (void) networkInfo;
  (void) classifierThreshold;
  attrList.resize (outputLayersInfo.size ());
  descString.clear ();
  return true;
}

CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (AbiTestClassify);
RDS_CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (AbiTestClassify);

static void
test_detection_params (void)
{
  RdsInferParseDetectionParams p;
  const char *base = reinterpret_cast<const char *> (&p);

  /* Documented order; not checkable with offsetof (the reference member makes the struct non-standard-layout). */
  g_assert_true (base + 0 == reinterpret_cast<const char *> (&p.numClassesConfigured));
  g_assert_true (reinterpret_cast<const char *> (&p.numClassesConfigured)
      < reinterpret_cast<const char *> (&p.perClassPreclusterThreshold));
  g_assert_true (reinterpret_cast<const char *> (&p.perClassPreclusterThreshold)
      < reinterpret_cast<const char *> (&p.perClassPostclusterThreshold));
  g_assert_cmpuint (p.numClassesConfigured, ==, 0);

  /* The deprecated name is a reference to the pre-cluster thresholds, also in copies. */
  p.numClassesConfigured = 2;
  p.perClassPreclusterThreshold = { 0.25f, 0.5f };
  p.perClassPostclusterThreshold = { 0.1f, 0.2f };
  g_assert_true (&p.perClassThreshold == &p.perClassPreclusterThreshold);
  g_assert_cmpfloat (p.perClassThreshold[1], ==, 0.5f);

  RdsInferParseDetectionParams copy (p);
  g_assert_true (&copy.perClassThreshold == &copy.perClassPreclusterThreshold);
  g_assert_cmpuint (copy.numClassesConfigured, ==, 2);
  g_assert_cmpfloat (copy.perClassThreshold[0], ==, 0.25f);
  g_assert_cmpfloat (copy.perClassPostclusterThreshold[1], ==, 0.2f);

  RdsInferParseDetectionParams assigned;
  assigned = p;
  g_assert_true (&assigned.perClassThreshold == &assigned.perClassPreclusterThreshold);
  g_assert_cmpuint (assigned.perClassThreshold.size (), ==, 2);

  RdsInferParseDetectionParams moved (std::move (copy));
  g_assert_true (&moved.perClassThreshold == &moved.perClassPreclusterThreshold);
  g_assert_cmpfloat (moved.perClassThreshold[1], ==, 0.5f);
  RdsInferParseDetectionParams move_assigned;
  move_assigned = std::move (moved);
  g_assert_true (&move_assigned.perClassThreshold == &move_assigned.perClassPreclusterThreshold);
  g_assert_cmpuint (move_assigned.perClassThreshold.size (), ==, 2);
}

/* DeepStream parsers compare the data type with the unprefixed names; this file builds with -Werror. */
static void
test_data_type_names (void)
{
  NvDsInferLayerInfo layer = {};
  NvDsInferDataType types[] = { FLOAT, HALF, INT8, INT32, INT64 };

  layer.dataType = FLOAT;
  g_assert_true (layer.dataType == FLOAT);
  g_assert_false (layer.dataType == HALF);
  switch (layer.dataType) {
    case FLOAT:
    case HALF:
    case INT8:
    case INT32:
    case INT64:
      break;
  }
  for (unsigned int i = 0; i < G_N_ELEMENTS (types); i++)
    g_assert_cmpint (types[i], ==, (int) i);
  layer.inferDims.numDims = 2;
  layer.inferDims.d[0] = 84;
  layer.inferDims.d[1] = 8400;
  layer.inferDims.numElements = 84 * 8400;
  g_assert_cmpuint (layer.inferDims.numElements, ==, layer.inferDims.d[0] * layer.inferDims.d[1]);
  g_assert_null (layer.deviceBuffer);
  g_assert_cmpint (infer_abi_noleak_sum (), ==, 15);
}

/* ---- Loader ---- */

static RdsInferParser *
open_ok (const char *path, const char *func)
{
  GError *err = NULL;
  RdsInferParser *parser = rds_infer_parser_open (path, func, &err);

  g_assert_no_error (err);
  g_assert_nonnull (parser);
  return parser;
}

static void
assert_open_fails (const char *path, const char *func, gint code, const char *const *substrings)
{
  GError *err = NULL;
  RdsInferParser *parser = rds_infer_parser_open (path, func, &err);

  g_assert_null (parser);
  g_assert_error (err, RDS_INFER_PARSER_ERROR, code);
  g_test_message ("%s", err->message);
  for (; *substrings; substrings++)
    g_assert_nonnull (strstr (err->message, *substrings));
  g_error_free (err);
}

/* @path appears once in the error from opening @path / @func (dlerror repeats it otherwise). */
static void
assert_path_once (const char *path, const char *func, const char *needle)
{
  GError *err = NULL;
  const char *first;

  g_assert_null (rds_infer_parser_open (path, func, &err));
  g_assert_nonnull (err);
  first = strstr (err->message, needle);
  g_assert_true (first == err->message);
  g_assert_null (strstr (first + 1, needle));
  g_error_free (err);
}

static gboolean
parse (RdsInferParser * parser, GArray * objects, GError ** err)
{
  float out[4] = { 0 };
  RdsInferLayerInfo layer = {};
  RdsInferNetworkInfo net = { 64, 64, 3 };

  layer.dataType = RDS_INFER_DATA_FLOAT;
  layer.inferDims.numDims = 1;
  layer.inferDims.d[0] = 4;
  layer.inferDims.numElements = 4;
  layer.layerName = "output0";
  layer.buffer = out;
  return rds_infer_parser_parse (parser, &layer, 1, &net, objects, err);
}

static void
test_fixture_boxes (void)
{
  RdsInferParser *parser = open_ok (RDS_TEST_PARSER_FIXTURE, "FixtureBoxes");
  GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  const float pre[] = { 0.2f, 0.3f }, post[] = { 0.4f, 0.5f };
  GError *err = NULL;

  g_assert_cmpstr (rds_infer_parser_get_func_name (parser), ==, "FixtureBoxes");
  rds_infer_parser_set_detection_params (parser, 2, pre, post);
  /* Twice: the second call appends again and doesn't see the first call's objects. */
  for (int round = 1; round <= 2; round++) {
    g_assert_true (parse (parser, objects, &err));
    g_assert_no_error (err);
    g_assert_cmpuint (objects->len, ==, round * RDS_TEST_FIXTURE_N_BOXES);
  }
  for (guint i = 0; i < objects->len; i++) {
    const RdsInferObjectDetectionInfo *o = &g_array_index (objects, RdsInferObjectDetectionInfo, i);
    const RdsTestFixtureBox *b = &rds_test_fixture_boxes[i % RDS_TEST_FIXTURE_N_BOXES];

    g_assert_cmpuint (o->classId, ==, b->class_id);
    g_assert_cmpfloat (o->left, ==, b->left);
    g_assert_cmpfloat (o->top, ==, b->top);
    g_assert_cmpfloat (o->width, ==, b->width);
    g_assert_cmpfloat (o->height, ==, b->height);
    g_assert_cmpfloat (o->detectionConfidence, ==, b->confidence);
    g_assert_cmpfloat (o->rotation_angle, ==, 0.0f);
  }
  g_array_unref (objects);
  rds_infer_parser_free (parser);
}

static void
test_missing_library (void)
{
  const char *want[] = { "/nonexistent/libparser.so: ", "No such file", NULL };

  assert_open_fails ("/nonexistent/libparser.so", "FixtureBoxes", RDS_INFER_PARSER_ERROR_OPEN, want);
  assert_path_once ("/nonexistent/libparser.so", "FixtureBoxes", "/nonexistent/libparser.so");
}

static void
test_wrong_symbol (void)
{
  const char *want[] = { RDS_TEST_PARSER_FIXTURE, "NoSuchParser", NULL };

  assert_open_fails (RDS_TEST_PARSER_FIXTURE, "NoSuchParser", RDS_INFER_PARSER_ERROR_SYMBOL, want);
  assert_path_once (RDS_TEST_PARSER_FIXTURE, "NoSuchParser", RDS_TEST_PARSER_FIXTURE);
}

/* The version symbol must be the module's own, not one of a library it depends on. */
static void
test_abi_from_dependency (void)
{
  const char *want[] = { RDS_TEST_PARSER_DEPABI, "has no exported rds_infer_parser_abi_version",
    "libinfer-parser-abihelper.so", NULL };

  assert_open_fails (RDS_TEST_PARSER_DEPABI, "NoAbiParse", RDS_INFER_PARSER_ERROR_ABI, want);
}

/* C++ including only rds_compat.h defines its own unprefixed names and exports no version symbol. */
static void
test_compat_only (void)
{
  void *handle = dlopen (RDS_TEST_COMPAT_ONLY, RTLD_NOW | RTLD_LOCAL);
  int (*sum) (void);

  g_assert_nonnull (handle);
  sum = reinterpret_cast<int (*)(void)> (dlsym (handle, "compat_only_sum"));
  g_assert_nonnull (reinterpret_cast<void *> (sum));
  g_assert_cmpint (sum (), ==, 1 + 2 + 3 + 4 + 5 + 0 + 8);
  g_assert_null (dlsym (handle, "rds_infer_parser_abi_version"));
  dlclose (handle);
}

/* This executable defines (and exports) the version symbol too: the check must look only in the module. */
static void
test_no_abi (void)
{
  const char *want[] = { RDS_TEST_PARSER_NOABI, "has no exported rds_infer_parser_abi_version",
    "rds/rds-infer-custom-impl.h", "rds/rds_compat_infer.h", "version script", NULL };

  g_assert_nonnull (dlsym (RTLD_DEFAULT, "rds_infer_parser_abi_version"));
  assert_open_fails (RDS_TEST_PARSER_NOABI, "NoAbiParse", RDS_INFER_PARSER_ERROR_ABI, want);
}

static void
test_bad_abi_version (void)
{
  gchar *expected = g_strdup_printf ("%u", RDS_INFER_PARSER_ABI_VERSION);
  const char *want[] = { RDS_TEST_PARSER_BADABI, "999", expected, NULL };

  assert_open_fails (RDS_TEST_PARSER_BADABI, "NoAbiParse", RDS_INFER_PARSER_ERROR_ABI, want);
  g_free (expected);
}

/* Built with -fvisibility=hidden: the version symbol is still exported (the header gives it default visibility),
 * so the version check passes and only the (hidden) parse function is missing. */
static void
test_hidden_visibility (void)
{
  const char *want[] = { RDS_TEST_PARSER_HIDDEN, "FixtureBoxes", NULL };
  void *handle = dlopen (RDS_TEST_PARSER_HIDDEN, RTLD_NOW | RTLD_LOCAL);
  unsigned int (*version) (void);

  g_assert_nonnull (handle);
  version = reinterpret_cast<unsigned int (*)(void)> (dlsym (handle, "rds_infer_parser_abi_version"));
  g_assert_nonnull (reinterpret_cast<void *> (version));
  g_assert_cmpuint (version (), ==, RDS_INFER_PARSER_ABI_VERSION);
  g_assert_null (dlsym (handle, "FixtureBoxes"));
  dlclose (handle);
  assert_open_fails (RDS_TEST_PARSER_HIDDEN, "FixtureBoxes", RDS_INFER_PARSER_ERROR_SYMBOL, want);

  /* A parse function marked visibility ("default") loads and works. */
  RdsInferParser *parser = open_ok (RDS_TEST_PARSER_HIDDEN, "FixtureVisible");
  GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  GError *err = NULL;

  g_assert_true (parse (parser, objects, &err));
  g_assert_no_error (err);
  g_assert_cmpuint (objects->len, ==, RDS_TEST_FIXTURE_N_BOXES);
  g_array_unref (objects);
  rds_infer_parser_free (parser);
}

static void
assert_parse_fails (const char *func, const char *const *substrings)
{
  RdsInferParser *parser = open_ok (RDS_TEST_PARSER_FIXTURE, func);
  GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  RdsInferObjectDetectionInfo keep = {};
  GError *err = NULL;

  keep.classId = 42;
  g_array_append_val (objects, keep);
  g_assert_false (parse (parser, objects, &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE);
  g_test_message ("%s", err->message);
  for (; *substrings; substrings++)
    g_assert_nonnull (strstr (err->message, *substrings));
  /* Nothing the failed call produced is kept; what was there before is. */
  g_assert_cmpuint (objects->len, ==, 1);
  g_assert_cmpuint (g_array_index (objects, RdsInferObjectDetectionInfo, 0).classId, ==, 42);
  g_clear_error (&err);
  /* The parser still works after a failure. */
  g_assert_false (parse (parser, objects, &err));
  g_clear_error (&err);
  g_array_unref (objects);
  rds_infer_parser_free (parser);
}

static void
test_throw (void)
{
  const char *want[] = { RDS_TEST_PARSER_FIXTURE, "FixtureThrow", RDS_TEST_FIXTURE_THROW_WHAT, NULL };

  assert_parse_fails ("FixtureThrow", want);
}

static void
test_fail (void)
{
  const char *want[] = { RDS_TEST_PARSER_FIXTURE, "FixtureFail", "returned false", NULL };

  assert_parse_fails ("FixtureFail", want);
}

static gboolean
module_loaded (const char *path)
{
  void *handle = dlopen (path, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);

  if (!handle)
    return FALSE;
  dlclose (handle);
  return TRUE;
}

/* Modules are loaded with RTLD_NODELETE: freeing the parser (or a failed open) releases the handle, but the module
 * stays mapped and can be opened again. */
static void
test_free_and_reopen (void)
{
  GError *err = NULL;
  RdsInferParser *parser;
  void *func;

  parser = open_ok (RDS_TEST_PARSER_FIXTURE, "FixtureBoxes");
  g_assert_true (module_loaded (RDS_TEST_PARSER_FIXTURE));
  func = dlsym (RTLD_DEFAULT, "FixtureBoxes");
  g_assert_null (func);         /* RTLD_LOCAL: not in the global scope */
  rds_infer_parser_free (parser);
  g_assert_true (module_loaded (RDS_TEST_PARSER_FIXTURE));

  parser = open_ok (RDS_TEST_PARSER_FIXTURE, "FixtureBoxes");
  rds_infer_parser_free (parser);

  g_assert_null (rds_infer_parser_open (RDS_TEST_PARSER_FIXTURE, "NoSuchParser", &err));
  g_clear_error (&err);
  g_assert_null (rds_infer_parser_open (RDS_TEST_PARSER_NOABI, "NoAbiParse", &err));
  g_clear_error (&err);
  rds_infer_parser_free (NULL);
}

/* ---- Classifier parsers ---- */

/* FP32 output layers over @data, one after the other: layer i has @sizes[i] values (dims [n], as the engine gives a
 * classifier's [batch, n] output per image). */
static std::vector<RdsInferLayerInfo>
cls_layers (float *data, std::initializer_list<unsigned int> sizes)
{
  std::vector<RdsInferLayerInfo> layers;
  unsigned int i = 0;

  for (unsigned int n : sizes) {
    RdsInferLayerInfo l = {};

    l.dataType = RDS_INFER_DATA_FLOAT;
    l.inferDims.numDims = 1;
    l.inferDims.d[0] = n;
    l.inferDims.numElements = n;
    l.bindingIndex = (int) i++;
    l.layerName = "output";
    l.buffer = data;
    data += n;
    layers.push_back (l);
  }
  return layers;
}

/* The label file's lines as the config holds them: one NULL-terminated vector per line, split at ';'. */
static GPtrArray *
label_lines (std::initializer_list<const char *> lines)
{
  GPtrArray *a = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);

  for (const char *line : lines)
    g_ptr_array_add (a, g_strsplit (line, ";", -1));
  return a;
}

static RdsInferParser *
builtin_classifier (gfloat threshold, std::initializer_list<const char *> lines)
{
  RdsInferParser *parser = rds_infer_parser_open_builtin_classifier ();
  GPtrArray *labels = label_lines (lines);

  g_assert_nonnull (parser);
  g_assert_cmpstr (rds_infer_parser_get_func_name (parser), ==, RDS_INFER_PARSER_CLASSIFIER);
  /* the parser keeps its own copy of the labels */
  rds_infer_parser_set_classifier_params (parser, threshold, labels);
  g_ptr_array_unref (labels);
  return parser;
}

/* Runs @parser on @layers (a 32x32 network); FALSE with @err as rds_infer_parser_classify. */
static gboolean
classify (RdsInferParser * parser, const std::vector<RdsInferLayerInfo> &layers, RdsInferClassifierResult * result,
    GError ** err)
{
  RdsInferNetworkInfo net = { 32, 32, 3 };

  return rds_infer_parser_classify (parser, layers.data (), layers.size (), &net, result, err);
}

static void
check_label (const RdsInferClassifierResult * r, guint i, guint label_id, guint class_id, guint num_classes,
    gfloat prob, const char *label)
{
  const RdsInferClassifierLabel *l;

  g_assert_cmpuint (r->labels->len, >, i);
  l = &g_array_index (r->labels, RdsInferClassifierLabel, i);
  g_assert_cmpuint (l->label_id, ==, label_id);
  g_assert_cmpuint (l->class_id, ==, class_id);
  g_assert_cmpuint (l->num_classes, ==, num_classes);
  g_assert_cmpfloat (l->prob, ==, prob);
  g_assert_cmpstr (l->label, ==, label);
}

/* The built-in parser: per output layer the raw argmax (no softmax), its value the probability, its label from that
 * layer's line of the label file; NaN never wins. */
static void
test_classifier_argmax (void)
{
  float data[] = { 0.1f, 0.7f, 0.2f, /**/ -3.0f, -1.0f, -2.0f, 5.0f, /**/ NAN, 0.25f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 3, 4, 2 });
  RdsInferParser *parser = builtin_classifier (-10.0f, { "a;b;c", "w;x;y;z", "n;m" });
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();
  GError *err = NULL;

  g_assert_true (rds_infer_parser_check_layers (parser, layers.data (), layers.size (), &err));
  g_assert_no_error (err);
  g_assert_true (classify (parser, layers, r, &err));
  g_assert_no_error (err);
  g_assert_cmpuint (r->labels->len, ==, 3);
  check_label (r, 0, 0, 1, 3, 0.7f, "b");
  check_label (r, 1, 1, 3, 4, 5.0f, "z");
  check_label (r, 2, 2, 1, 2, 0.25f, "m");
  /* the built-in parser has no description: the display text comes from the labels */
  g_assert_null (r->desc);
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);
}

/* Equal values: the lowest class index wins. */
static void
test_classifier_ties (void)
{
  float data[] = { 0.5f, 0.5f, 0.5f, /**/ 0.1f, 0.4f, 0.4f, 0.2f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 3, 4 });
  RdsInferParser *parser = builtin_classifier (0.0f, { "a;b;c", "w;x;y;z" });
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();

  g_assert_true (classify (parser, layers, r, NULL));
  g_assert_cmpuint (r->labels->len, ==, 2);
  check_label (r, 0, 0, 0, 3, 0.5f, "a");
  check_label (r, 1, 1, 1, 4, 0.4f, "x");
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);
}

/* A layer's best value must be strictly above classifier-threshold (DeepStream 8.0 behaviour); a layer below it is
 * left out, and with the default 0 an all-zero (or all-negative) layer gives nothing. */
static void
test_classifier_threshold (void)
{
  float data[] = { 0.5f, 0.1f, /**/ 0.1f, 0.50390625f, /**/ 0.0f, 0.0f };
  float zero[] = { 0.0f, 0.0f, 0.0f, /**/ -0.5f, -0.25f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 2, 2, 2 }), zeros = cls_layers (zero, { 3, 2 });
  RdsInferParser *parser = builtin_classifier (0.5f, { "a;b", "c;d", "e;f" });
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();

  g_assert_true (classify (parser, layers, r, NULL));
  g_assert_cmpuint (r->labels->len, ==, 1);
  check_label (r, 0, 1, 1, 2, 0.50390625f, "d");
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);

  parser = builtin_classifier (0.0f, { "a;b;c", "d;e" });
  r = rds_infer_classifier_result_new ();
  g_assert_true (classify (parser, zeros, r, NULL));
  g_assert_cmpuint (r->labels->len, ==, 0);
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);
}

/* An index past its line's labels (layers 0 and 1), a layer without a line (layer 2) and no label file at all give
 * an empty label (ours: DeepStream 8.0 drops the first and the last). */
static void
test_classifier_labels (void)
{
  float data[] = { 0.1f, 0.2f, 0.9f, /**/ 0.3f, 0.6f, /**/ 0.8f, 0.1f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 3, 2, 2 });
  RdsInferParser *parser = builtin_classifier (0.0f, { "a;b", " x " });
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();

  g_assert_true (classify (parser, layers, r, NULL));
  g_assert_cmpuint (r->labels->len, ==, 3);
  check_label (r, 0, 0, 2, 3, 0.9f, "");
  check_label (r, 1, 1, 1, 2, 0.6f, "");
  check_label (r, 2, 2, 0, 2, 0.8f, "");
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);

  parser = builtin_classifier (0.0f, { "a;b;c", " x ;y" });
  r = rds_infer_classifier_result_new ();
  g_assert_true (classify (parser, layers, r, NULL));
  check_label (r, 0, 0, 2, 3, 0.9f, "c");
  check_label (r, 1, 1, 1, 2, 0.6f, "y");
  check_label (r, 2, 2, 0, 2, 0.8f, "");
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);

  parser = builtin_classifier (0.0f, { });
  r = rds_infer_classifier_result_new ();
  g_assert_true (classify (parser, layers, r, NULL));
  g_assert_cmpuint (r->labels->len, ==, 3);
  check_label (r, 1, 1, 1, 2, 0.6f, "");
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);
}

/* The built-in parser needs at least one FP32 output layer with at least one value. */
static void
test_classifier_layers (void)
{
  float data[] = { 0.5f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 1 }), empty = cls_layers (data, { 0 });
  RdsInferParser *parser = builtin_classifier (0.0f, { "a" });
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();
  GError *err = NULL;

  g_assert_false (rds_infer_parser_check_layers (parser, NULL, 0, &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS);
  g_test_message ("%s", err->message);
  g_clear_error (&err);
  g_assert_false (rds_infer_parser_check_layers (parser, empty.data (), empty.size (), &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS);
  g_assert_nonnull (strstr (err->message, "no values"));
  g_clear_error (&err);
  layers[0].dataType = RDS_INFER_DATA_INT32;
  g_assert_false (rds_infer_parser_check_layers (parser, layers.data (), layers.size (), &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS);
  g_assert_nonnull (strstr (err->message, "INT32"));
  g_clear_error (&err);
  /* the same check on every call */
  g_assert_false (classify (parser, layers, r, &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS);
  g_clear_error (&err);
  g_assert_cmpuint (r->labels->len, ==, 0);
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);
}

static RdsInferParser *
open_classifier_ok (const char *func, gfloat threshold)
{
  GError *err = NULL;
  RdsInferParser *parser = rds_infer_parser_open_classifier (RDS_TEST_PARSER_FIXTURE, func, &err);

  g_assert_no_error (err);
  g_assert_nonnull (parser);
  g_assert_cmpstr (rds_infer_parser_get_func_name (parser), ==, func);
  rds_infer_parser_set_classifier_params (parser, threshold, NULL);
  return parser;
}

/* A custom classifier parser, called with the layers, the network and the threshold: its attributes become labels
 * as they are (no threshold of ours), with its strdup'ed labels (freed by the loader: ASan checks) and its
 * description; a NULL label is an empty one. Any layer checks are the parser's own. */
static void
test_classifier_custom (void)
{
  float data[] = { 0.5f, 0.1f, 0.2f, /**/ 0.25f, 0.0f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 3, 2 });
  RdsInferParser *parser = open_classifier_ok ("FixtureClassify", 0.6f);
  RdsInferClassifierResult *r = rds_infer_classifier_result_new (), *copy;
  GError *err = NULL;

  g_assert_true (rds_infer_parser_check_layers (parser, NULL, 0, &err));
  g_assert_true (classify (parser, layers, r, &err));
  g_assert_no_error (err);
  g_assert_cmpuint (r->labels->len, ==, 2);
  check_label (r, 0, 0, 128, 3, 0.3f, "attr0-32x32");
  check_label (r, 1, 1, 64, 2, 0.3f, "");
  g_assert_cmpstr (r->desc, ==, "desc2");

  /* a copy owns its strings */
  copy = rds_infer_classifier_result_copy (r);
  rds_infer_classifier_result_free (r);
  g_assert_cmpuint (copy->labels->len, ==, 2);
  check_label (copy, 0, 0, 128, 3, 0.3f, "attr0-32x32");
  check_label (copy, 1, 1, 64, 2, 0.3f, "");
  g_assert_cmpstr (copy->desc, ==, "desc2");
  rds_infer_classifier_result_free (copy);
  rds_infer_parser_free (parser);
}

/* A custom parser's empty descString is kept as an empty description (DeepStream 8.0 behaviour): desc is "", so the
 * display text gets no labels. An attribute whose index is past the layers has no class count (0). */
static void
test_classifier_custom_no_desc (void)
{
  float data[] = { 0.5f, 0.1f, 0.2f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 3 });
  RdsInferParser *parser = open_classifier_ok ("FixtureClassifyNoDesc", 0.6f);
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();

  g_assert_true (classify (parser, layers, r, NULL));
  g_assert_cmpuint (r->labels->len, ==, 1);
  check_label (r, 0, 0, 128, 3, 0.3f, "attr0-32x32");
  g_assert_cmpstr (r->desc, ==, "");
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);

  parser = open_classifier_ok ("FixtureClassifyPastLayers", 0.0f);
  r = rds_infer_classifier_result_new ();
  g_assert_true (classify (parser, layers, r, NULL));
  g_assert_cmpuint (r->labels->len, ==, 1);
  check_label (r, 0, 1, 5, 0, 0.5f, "past");
  g_assert_cmpstr (r->desc, ==, "past");
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);
}

/* The value type on its own: a NULL label is an empty one, a copy is deep. */
static void
test_classifier_result (void)
{
  RdsInferClassifierResult *r = rds_infer_classifier_result_new (), *copy;

  g_assert_cmpuint (r->labels->len, ==, 0);
  g_assert_null (r->desc);
  rds_infer_classifier_result_add (r, 3, 7, 0, 0.25f, NULL);
  rds_infer_classifier_result_add (r, 0, 1, 2, 0.75f, "two");
  copy = rds_infer_classifier_result_copy (r);
  rds_infer_classifier_result_free (r);
  check_label (copy, 0, 3, 7, 0, 0.25f, "");
  check_label (copy, 1, 0, 1, 2, 0.75f, "two");
  g_assert_null (copy->desc);
  rds_infer_classifier_result_free (copy);
}

static void
assert_classify_fails (const char *func, const char *const *substrings)
{
  float data[] = { 0.5f, 0.1f, /**/ 0.25f };
  std::vector<RdsInferLayerInfo> layers = cls_layers (data, { 2, 1 });
  RdsInferParser *parser = open_classifier_ok (func, 0.5f);
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();
  GError *err = NULL;

  g_assert_false (classify (parser, layers, r, &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE);
  g_test_message ("%s", err->message);
  for (; *substrings; substrings++)
    g_assert_nonnull (strstr (err->message, *substrings));
  g_clear_error (&err);
  /* nothing of the failed call is kept, and the labels it allocated are freed (ASan) */
  g_assert_cmpuint (r->labels->len, ==, 0);
  g_assert_null (r->desc);
  g_assert_false (classify (parser, layers, r, &err));
  g_clear_error (&err);
  rds_infer_classifier_result_free (r);
  rds_infer_parser_free (parser);
}

static void
test_classifier_fail (void)
{
  const char *want[] = { RDS_TEST_PARSER_FIXTURE, "FixtureClassifyFail", "returned false", NULL };

  assert_classify_fails ("FixtureClassifyFail", want);
}

static void
test_classifier_throw (void)
{
  const char *want[] = { RDS_TEST_PARSER_FIXTURE, "FixtureClassifyThrow", RDS_TEST_FIXTURE_THROW_WHAT, NULL };

  assert_classify_fails ("FixtureClassifyThrow", want);
}

static void
test_classifier_wrong_symbol (void)
{
  GError *err = NULL;

  g_assert_null (rds_infer_parser_open_classifier (RDS_TEST_PARSER_FIXTURE, "NoSuchClassifier", &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_SYMBOL);
  g_assert_nonnull (strstr (err->message, "NoSuchClassifier"));
  g_clear_error (&err);
  g_assert_null (rds_infer_parser_open_classifier (RDS_TEST_PARSER_NOABI, "NoAbiParse", &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_ABI);
  g_clear_error (&err);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/infer-abi/detection-params", test_detection_params);
  g_test_add_func ("/infer-abi/data-type-names", test_data_type_names);
  g_test_add_func ("/infer-parser/fixture-boxes", test_fixture_boxes);
  g_test_add_func ("/infer-parser/missing-library", test_missing_library);
  g_test_add_func ("/infer-parser/wrong-symbol", test_wrong_symbol);
  g_test_add_func ("/infer-parser/no-abi", test_no_abi);
  g_test_add_func ("/infer-parser/bad-abi-version", test_bad_abi_version);
  g_test_add_func ("/infer-parser/hidden-visibility", test_hidden_visibility);
  g_test_add_func ("/infer-parser/throw", test_throw);
  g_test_add_func ("/infer-parser/fail", test_fail);
  g_test_add_func ("/infer-parser/abi-from-dependency", test_abi_from_dependency);
  g_test_add_func ("/infer-parser/compat-only", test_compat_only);
  g_test_add_func ("/infer-parser/free-and-reopen", test_free_and_reopen);
  g_test_add_func ("/infer-parser/classifier-argmax", test_classifier_argmax);
  g_test_add_func ("/infer-parser/classifier-ties", test_classifier_ties);
  g_test_add_func ("/infer-parser/classifier-threshold", test_classifier_threshold);
  g_test_add_func ("/infer-parser/classifier-labels", test_classifier_labels);
  g_test_add_func ("/infer-parser/classifier-layers", test_classifier_layers);
  g_test_add_func ("/infer-parser/classifier-custom", test_classifier_custom);
  g_test_add_func ("/infer-parser/classifier-custom-no-desc", test_classifier_custom_no_desc);
  g_test_add_func ("/infer-parser/classifier-result", test_classifier_result);
  g_test_add_func ("/infer-parser/classifier-fail", test_classifier_fail);
  g_test_add_func ("/infer-parser/classifier-throw", test_classifier_throw);
  g_test_add_func ("/infer-parser/classifier-wrong-symbol", test_classifier_wrong_symbol);
  return g_test_run ();
}
