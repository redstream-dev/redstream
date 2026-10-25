/* The custom bbox parser ABI (rds/rds-infer-custom-impl.h and its NvDsInfer* aliases) and rdsinfer's parser
 * loader, against the test modules in infer-parser-fixture.cpp and infer-parser-noabi.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds_compat.h>

#include "rds-infer-parse.h"
#include "infer-parser-fixture.h"

#include <cstddef>
#include <cstring>
#include <type_traits>
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
  const char *want[] = { "/nonexistent/libparser.so", NULL };

  assert_open_fails ("/nonexistent/libparser.so", "FixtureBoxes", RDS_INFER_PARSER_ERROR_OPEN, want);
}

static void
test_wrong_symbol (void)
{
  const char *want[] = { RDS_TEST_PARSER_FIXTURE, "NoSuchParser", NULL };

  assert_open_fails (RDS_TEST_PARSER_FIXTURE, "NoSuchParser", RDS_INFER_PARSER_ERROR_SYMBOL, want);
}

/* This executable defines (and exports) the version symbol too: the check must look only in the module. */
static void
test_no_abi (void)
{
  const char *want[] = { RDS_TEST_PARSER_NOABI, "not built against", "rds-infer-custom-impl.h", NULL };

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

/* The loader keeps the module open while the parser lives, closes it on free and on every failed open. */
static void
test_closed_on_free (void)
{
  GError *err = NULL;
  RdsInferParser *parser;

  g_assert_false (module_loaded (RDS_TEST_PARSER_FIXTURE));
  parser = open_ok (RDS_TEST_PARSER_FIXTURE, "FixtureBoxes");
  g_assert_true (module_loaded (RDS_TEST_PARSER_FIXTURE));
  rds_infer_parser_free (parser);
  g_assert_false (module_loaded (RDS_TEST_PARSER_FIXTURE));

  g_assert_null (rds_infer_parser_open (RDS_TEST_PARSER_FIXTURE, "NoSuchParser", &err));
  g_clear_error (&err);
  g_assert_false (module_loaded (RDS_TEST_PARSER_FIXTURE));
  g_assert_null (rds_infer_parser_open (RDS_TEST_PARSER_NOABI, "NoAbiParse", &err));
  g_clear_error (&err);
  g_assert_false (module_loaded (RDS_TEST_PARSER_NOABI));
  rds_infer_parser_free (NULL);
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
  g_test_add_func ("/infer-parser/closed-on-free", test_closed_on_free);
  return g_test_run ();
}
