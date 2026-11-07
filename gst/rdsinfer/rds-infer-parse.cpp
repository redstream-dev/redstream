/* Redstream: rdsinfer's custom parser loader (bbox and classifier parsers), the built-in YOLO parser, the built-in
 * classifier parser and the classifier results. No C++ exception leaves this file.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define RDS_INFER_NO_PARSER_ABI_SYMBOL
#include <rds/rds-infer-custom-impl.h>

#include "rds-infer-parse.h"

#include <dlfcn.h>
#include <link.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <exception>
#include <string>
#include <type_traits>

G_DEFINE_QUARK (rds-infer-parser-error-quark, rds_infer_parser_error)

struct _RdsInferParser
{
  void *handle;                 /* NULL for a built-in parser */
  gchar *lib_path, *func_name;  /* lib_path is "built-in parser" for one */
  gboolean builtin;
  gboolean classifier;          /* classify_func, else func */
  RdsInferParseCustomFunc func;
  RdsInferClassifierParseCustomFunc classify_func;
  RdsInferParseDetectionParams params;
  /* Classifiers: classifier-threshold, and the label file's lines (one vector of labels per output layer). */
  float threshold;
  std::vector<std::vector<std::string>> labels;
  /* Reused between calls. */
  std::vector<RdsInferLayerInfo> layers;
  std::vector<RdsInferObjectDetectionInfo> objects;
  std::vector<RdsInferAttribute> attrs;
  std::string desc;
};

static void
parser_free (RdsInferParser * parser)
{
  if (parser->handle)
    dlclose (parser->handle);
  g_free (parser->lib_path);
  g_free (parser->func_name);
  delete parser;
}

typedef unsigned int (*AbiVersionFunc) (void);

/* dlerror () text without a leading "<path>: ", which the messages already start with. */
static const char *
strip_path (const char *dl_err, const gchar * path)
{
  gsize len = strlen (path);

  if (!dl_err)
    return "unknown error";
  if (strncmp (dl_err, path, len) == 0 && strncmp (dl_err + len, ": ", 2) == 0)
    return dl_err + len + 2;
  return dl_err;
}

static RdsInferParser *
parser_open (const gchar * lib_path, const gchar * func_name, bool classifier, GError ** error)
{
  RdsInferParser *parser = new RdsInferParser ();
  AbiVersionFunc version_func;
  void *func;
  struct link_map *module_map = NULL, *symbol_map = NULL;
  Dl_info info;
  unsigned int version;
  const char *err;

  parser->lib_path = g_strdup (lib_path);
  parser->func_name = g_strdup (func_name);
  /* Never unloaded (as GStreamer never unloads plugins): a parser with HIP code objects, GTypes or threads can
   * crash on dlclose. Freeing the parser drops the reference only. */
  parser->handle = dlopen (lib_path, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
  if (!parser->handle) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_OPEN,
        "%s: can't load the custom parser library: %s", lib_path, strip_path (dlerror (), lib_path));
    goto fail;
  }

  /* dlsym on the handle searches the library and its dependencies (never the loading process); the symbol must be
   * the library's own. */
  version_func = reinterpret_cast<AbiVersionFunc> (dlsym (parser->handle, "rds_infer_parser_abi_version"));
  if (version_func && (dlinfo (parser->handle, RTLD_DI_LINKMAP, &module_map) != 0
          || !dladdr1 (reinterpret_cast<void *> (version_func), &info, reinterpret_cast<void **> (&symbol_map),
              RTLD_DL_LINKMAP) || !module_map || !symbol_map)) {
    err = dlerror ();
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_ABI,
        "%s: can't check that rds_infer_parser_abi_version is the library's own: looking up where it is defined "
        "failed: %s", lib_path, err ? strip_path (err, lib_path) : "unknown error");
    goto fail;
  }
  if (version_func && symbol_map != module_map) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_ABI,
        "%s has no exported rds_infer_parser_abi_version of its own (the one found is in %s, which it loads): "
        "build it against rds/rds-infer-custom-impl.h (rds/rds_compat_infer.h for DeepStream parsers)",
        lib_path, symbol_map->l_name && symbol_map->l_name[0] ? symbol_map->l_name : "the executable");
    goto fail;
  }
  if (!version_func) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_ABI,
        "%s has no exported rds_infer_parser_abi_version: build it against rds/rds-infer-custom-impl.h "
        "(rds/rds_compat_infer.h for DeepStream parsers); if it uses a version script or --exclude-libs, "
        "export that symbol", lib_path);
    goto fail;
  }
  version = version_func ();
  if (version != RDS_INFER_PARSER_ABI_VERSION) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_ABI,
        "%s: the custom parser library has parser ABI version %u, this rdsinfer needs version %u: rebuild it "
        "against this Redstream's headers", lib_path, version, RDS_INFER_PARSER_ABI_VERSION);
    goto fail;
  }

  dlerror ();
  func = dlsym (parser->handle, func_name);
  if (!func) {
    err = dlerror ();
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_SYMBOL,
        "%s: no parse function %s in the custom parser library (it must be extern \"C\" and exported): %s",
        lib_path, func_name, err ? strip_path (err, lib_path) : "the symbol is NULL");
    goto fail;
  }
  parser->classifier = classifier;
  if (classifier)
    parser->classify_func = reinterpret_cast<RdsInferClassifierParseCustomFunc> (func);
  else
    parser->func = reinterpret_cast<RdsInferParseCustomFunc> (func);
  return parser;

fail:
  parser_free (parser);
  return NULL;
}

RdsInferParser *
rds_infer_parser_open (const gchar * lib_path, const gchar * func_name, GError ** error)
{
  g_return_val_if_fail (lib_path != NULL, NULL);
  g_return_val_if_fail (func_name != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  try {
    return parser_open (lib_path, func_name, false, error);
  }
  catch (const std::exception & e) {
    g_error ("rdsinfer: %s: loading the custom parser failed: %s", lib_path, e.what ());
  }
  catch (...) {
    g_error ("rdsinfer: %s: loading the custom parser failed", lib_path);
  }
}

RdsInferParser *
rds_infer_parser_open_classifier (const gchar * lib_path, const gchar * func_name, GError ** error)
{
  g_return_val_if_fail (lib_path != NULL, NULL);
  g_return_val_if_fail (func_name != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  try {
    return parser_open (lib_path, func_name, true, error);
  }
  catch (const std::exception & e) {
    g_error ("rdsinfer: %s: loading the custom classifier parser failed: %s", lib_path, e.what ());
  }
  catch (...) {
    g_error ("rdsinfer: %s: loading the custom classifier parser failed", lib_path);
  }
}

/* ---- The built-in YOLO parser ---- */

#define BUILTIN_PATH "built-in parser"

static const char *
data_type_name (RdsInferDataType type)
{
  switch (type) {
    case RDS_INFER_DATA_FLOAT:
      return "FP32";
    case RDS_INFER_DATA_HALF:
      return "FP16";
    case RDS_INFER_DATA_INT8:
      return "INT8";
    case RDS_INFER_DATA_INT32:
      return "INT32";
    case RDS_INFER_DATA_INT64:
      return "INT64";
  }
  return "of an unknown data type";
}

/* "[d0, d1, ...]" */
static gchar *
dims_string (const RdsInferDims & dims)
{
  GString *s = g_string_new ("[");

  for (unsigned int i = 0; i < MIN (dims.numDims, (unsigned int) RDS_INFER_MAX_DIMS); i++)
    g_string_append_printf (s, "%s%u", i ? ", " : "", dims.d[i]);
  g_string_append_c (s, ']');
  return g_string_free (s, FALSE);
}

/* Where a YOLO output is: score row r of anchor a is at data[r * row_stride + a * anchor_stride]. */
struct YoloLayout
{
  unsigned int n_anchors, row_stride, anchor_stride;
};

/* Checks that @layers are one FP32 output of shape [4 + @num_classes, A] or [A, 4 + @num_classes], and with
 * @need_buffer that it has a host buffer. */
static gboolean
yolo_layout (const RdsInferLayerInfo * layers, guint n_layers, unsigned int num_classes, bool need_buffer,
    YoloLayout * out, GError ** error)
{
  const unsigned int rows = 4 + num_classes;
  const RdsInferLayerInfo *l = layers;
  const RdsInferDims *d;
  const char *name;
  gchar *what = NULL, *shape;

  if (num_classes == 0) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS,
        BUILTIN_PATH ": " RDS_INFER_PARSER_YOLO " needs at least one class (num-detected-classes 0: the detection "
        "params are not set)");
    return FALSE;
  }
  if (n_layers != 1) {
    what = g_strdup_printf ("the network has %u output layers", n_layers);
    goto fail;
  }
  name = l->layerName ? l->layerName : "(unnamed)";
  if (l->isInput) {
    what = g_strdup_printf ("layer \"%s\" is an input layer", name);
    goto fail;
  }
  if (l->dataType != RDS_INFER_DATA_FLOAT) {
    what = g_strdup_printf ("output layer \"%s\" is %s", name, data_type_name (l->dataType));
    goto fail;
  }
  d = &l->inferDims;
  if (d->numDims != 2 || d->d[0] == 0 || d->d[1] == 0 || (d->d[0] != rows && d->d[1] != rows)) {
    shape = dims_string (*d);
    what = g_strdup_printf ("output layer \"%s\" has shape %s", name, shape);
    g_free (shape);
    goto fail;
  }
  if ((guint64) d->d[0] * d->d[1] != d->numElements) {
    shape = dims_string (*d);
    what = g_strdup_printf ("output layer \"%s\" has shape %s but %u elements", name, shape, d->numElements);
    g_free (shape);
    goto fail;
  }
  if (need_buffer && !l->buffer) {
    what = g_strdup_printf ("output layer \"%s\" has no host buffer", name);
    goto fail;
  }
  /* [4 + C, A] wins when both dimensions are 4 + C (ours): it is what the Ultralytics export produces. */
  if (d->d[0] == rows) {
    out->n_anchors = d->d[1];
    out->row_stride = d->d[1];
    out->anchor_stride = 1;
  } else {
    out->n_anchors = d->d[0];
    out->row_stride = 1;
    out->anchor_stride = rows;
  }
  return TRUE;

fail:
  g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS,
      BUILTIN_PATH ": " RDS_INFER_PARSER_YOLO " needs one FP32 output layer of shape [%u, anchors] or "
      "[anchors, %u] (4 + num-detected-classes %u); %s", rows, rows, num_classes, what);
  g_free (what);
  return FALSE;
}

/* RdsInferParseYolo: see RDS_INFER_PARSER_YOLO. */
static bool
parse_yolo (std::vector<RdsInferLayerInfo> const &outputLayersInfo, RdsInferNetworkInfo const &networkInfo,
    RdsInferParseDetectionParams const &detectionParams, std::vector<RdsInferObjectDetectionInfo> &objectList)
{
  const unsigned int n_classes = detectionParams.numClassesConfigured;
  const float *thresholds = detectionParams.perClassPreclusterThreshold.data ();
  const float *data;
  YoloLayout l;

  (void) networkInfo;
  if (!yolo_layout (outputLayersInfo.data (), outputLayersInfo.size (), n_classes, true, &l, NULL)
      || detectionParams.perClassPreclusterThreshold.size () < n_classes)
    return false;
  data = static_cast<const float *> (outputLayersInfo[0].buffer);
  for (unsigned int a = 0; a < l.n_anchors; a++) {
    const float *p = data + (gsize) a * l.anchor_stride;
    const float *scores = p + (gsize) 4 * l.row_stride;
    unsigned int best_class = 0;
    float best = -INFINITY;
    float w, h;

    /* Strictly greater: the lowest class id among equal scores; NaN scores never win. */
    for (unsigned int c = 0; c < n_classes; c++) {
      float s = scores[(gsize) c * l.row_stride];

      if (s > best) {
        best = s;
        best_class = c;
      }
    }
    /* At least the threshold (the documented parser contract); a NaN score fails it. */
    if (!(best >= thresholds[best_class]))
      continue;
    w = p[(gsize) 2 * l.row_stride];
    h = p[(gsize) 3 * l.row_stride];
    objectList.push_back ({ best_class, p[0] - w / 2, p[l.row_stride] - h / 2, w, h, best, 0.0f });
  }
  return true;
}

static_assert (std::is_same<decltype (&parse_yolo), RdsInferParseCustomFunc>::value,
    "the built-in parser is an RdsInferParseCustomFunc");

RdsInferParser *
rds_infer_parser_open_builtin (const gchar * func_name, GError ** error)
{
  RdsInferParser *parser;

  g_return_val_if_fail (func_name != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (strcmp (func_name, RDS_INFER_PARSER_YOLO) != 0) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_SYMBOL,
        BUILTIN_PATH ": no built-in parse function %s (the built-in one is " RDS_INFER_PARSER_YOLO "); for a "
        "custom parser, set custom-lib-path to its library", func_name);
    return NULL;
  }
  try {
    parser = new RdsInferParser ();
  }
  catch (const std::exception & e) {
    g_error ("rdsinfer: creating the built-in parser failed: %s", e.what ());
  }
  parser->builtin = TRUE;
  parser->lib_path = g_strdup (BUILTIN_PATH);
  parser->func_name = g_strdup (func_name);
  parser->func = parse_yolo;
  return parser;
}

/* ---- The built-in classifier parser ---- */

/* Checks that @layers are at least one FP32 output layer with at least one value each, and with @need_buffer that
 * they have host buffers. */
static gboolean
classifier_layers (const RdsInferLayerInfo * layers, guint n_layers, bool need_buffer, GError ** error)
{
  gchar *what = NULL;

  if (n_layers == 0)
    what = g_strdup ("the network has no output layers");
  for (guint i = 0; !what && i < n_layers; i++) {
    const RdsInferLayerInfo *l = &layers[i];
    const char *name = l->layerName ? l->layerName : "(unnamed)";

    if (l->isInput)
      what = g_strdup_printf ("layer \"%s\" is an input layer", name);
    else if (l->dataType != RDS_INFER_DATA_FLOAT)
      what = g_strdup_printf ("output layer \"%s\" is %s", name, data_type_name (l->dataType));
    else if (l->inferDims.numElements == 0)
      what = g_strdup_printf ("output layer \"%s\" has no values", name);
    else if (need_buffer && !l->buffer)
      what = g_strdup_printf ("output layer \"%s\" has no host buffer", name);
  }
  if (!what)
    return TRUE;
  g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS,
      BUILTIN_PATH ": " RDS_INFER_PARSER_CLASSIFIER " needs FP32 output layers of at least one value each; %s", what);
  g_free (what);
  return FALSE;
}

/* RdsInferParseClassifier: see RDS_INFER_PARSER_CLASSIFIER. The labels are left NULL: the loader takes them from the
 * label file. Called only through rds_infer_parser_classify, which has checked the layers (classifier_layers). */
static bool
parse_classifier (std::vector<RdsInferLayerInfo> const &outputLayersInfo, RdsInferNetworkInfo const &networkInfo,
    float classifierThreshold, std::vector<RdsInferAttribute> &attrList, std::string &descString)
{
  (void) networkInfo;
  (void) descString;
  for (unsigned int i = 0; i < outputLayersInfo.size (); i++) {
    const RdsInferLayerInfo &l = outputLayersInfo[i];
    const float *values = static_cast<const float *> (l.buffer);
    unsigned int best_class = 0;
    float best = -INFINITY;

    /* Strictly greater: the lowest class among equal values; NaN never wins. */
    for (unsigned int c = 0; c < l.inferDims.numElements; c++) {
      if (values[c] > best) {
        best = values[c];
        best_class = c;
      }
    }
    /* Strictly above the threshold (DeepStream 8.0 behaviour). */
    if (best > classifierThreshold)
      attrList.push_back ({ i, best_class, best, nullptr });
  }
  return true;
}

static_assert (std::is_same<decltype (&parse_classifier), RdsInferClassifierParseCustomFunc>::value,
    "the built-in classifier parser is an RdsInferClassifierParseCustomFunc");

RdsInferParser *
rds_infer_parser_open_builtin_classifier (void)
{
  RdsInferParser *parser;

  try {
    parser = new RdsInferParser ();
  }
  catch (const std::exception & e) {
    g_error ("rdsinfer: creating the built-in classifier parser failed: %s", e.what ());
  }
  parser->builtin = TRUE;
  parser->classifier = TRUE;
  parser->lib_path = g_strdup (BUILTIN_PATH);
  parser->func_name = g_strdup (RDS_INFER_PARSER_CLASSIFIER);
  parser->classify_func = parse_classifier;
  return parser;
}

gboolean
rds_infer_parser_check_layers (const RdsInferParser * parser, const RdsInferLayerInfo * layers, guint n_layers,
    GError ** error)
{
  YoloLayout l;

  g_return_val_if_fail (parser != NULL, FALSE);
  g_return_val_if_fail (n_layers == 0 || layers != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!parser->builtin)
    return TRUE;
  if (parser->classifier)
    return classifier_layers (layers, n_layers, false, error);
  return yolo_layout (layers, n_layers, parser->params.numClassesConfigured, false, &l, error);
}

void
rds_infer_parser_free (RdsInferParser * parser)
{
  if (parser)
    parser_free (parser);
}

const gchar *
rds_infer_parser_get_func_name (const RdsInferParser * parser)
{
  g_return_val_if_fail (parser != NULL, NULL);

  return parser->func_name;
}

void
rds_infer_parser_set_detection_params (RdsInferParser * parser, guint num_classes,
    const gfloat * pre_cluster_thresholds, const gfloat * post_cluster_thresholds)
{
  g_return_if_fail (parser != NULL);
  g_return_if_fail (num_classes == 0 || (pre_cluster_thresholds && post_cluster_thresholds));

  /* Only std::bad_alloc can be thrown here; GLib aborts on out-of-memory too. */
  try {
    parser->params.numClassesConfigured = num_classes;
    parser->params.perClassPreclusterThreshold.assign (pre_cluster_thresholds,
        pre_cluster_thresholds + num_classes);
    parser->params.perClassPostclusterThreshold.assign (post_cluster_thresholds,
        post_cluster_thresholds + num_classes);
  }
  catch (const std::exception & e) {
    g_error ("rdsinfer: setting the parser's detection params failed: %s", e.what ());
  }
}

gboolean
rds_infer_parser_parse (RdsInferParser * parser, const RdsInferLayerInfo * layers, guint n_layers,
    const RdsInferNetworkInfo * network_info, GArray * objects, GError ** error)
{
  bool ok;

  g_return_val_if_fail (parser != NULL && !parser->classifier, FALSE);
  g_return_val_if_fail (n_layers == 0 || layers != NULL, FALSE);
  g_return_val_if_fail (network_info != NULL, FALSE);
  g_return_val_if_fail (objects != NULL && g_array_get_element_size (objects) ==
      sizeof (RdsInferObjectDetectionInfo), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (parser->builtin) {
    YoloLayout l;

    if (!yolo_layout (layers, n_layers, parser->params.numClassesConfigured, true, &l, error))
      return FALSE;
  }
  try {
    parser->layers.assign (layers, layers + n_layers);
    parser->objects.clear ();
    ok = parser->func (parser->layers, *network_info, parser->params, parser->objects);
  }
  catch (const std::exception & e) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE,
        "%s: the parse function %s threw an exception: %s", parser->lib_path, parser->func_name, e.what ());
    return FALSE;
  }
  catch (...) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE,
        "%s: the parse function %s threw an exception (not a std::exception)", parser->lib_path,
        parser->func_name);
    return FALSE;
  }
  if (!ok) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE,
        "%s: the parse function %s returned false", parser->lib_path, parser->func_name);
    return FALSE;
  }
  if (!parser->objects.empty ())
    g_array_append_vals (objects, parser->objects.data (), parser->objects.size ());
  return TRUE;
}

void
rds_infer_parser_set_classifier_params (RdsInferParser * parser, gfloat threshold, const GPtrArray * labels)
{
  g_return_if_fail (parser != NULL && parser->classifier);

  /* Only std::bad_alloc can be thrown here; GLib aborts on out-of-memory too. */
  try {
    parser->threshold = threshold;
    parser->labels.clear ();
    for (guint i = 0; labels && i < labels->len; i++) {
      const gchar *const *line = static_cast<const gchar * const *> (g_ptr_array_index (labels, i));

      parser->labels.emplace_back ();
      for (; line && *line; line++)
        parser->labels.back ().emplace_back (*line);
    }
  }
  catch (const std::exception & e) {
    g_error ("rdsinfer: setting the classifier parser's params failed: %s", e.what ());
  }
}

/* The parser's attribute labels are its strdup'ed memory, ours to free. */
static void
free_attr_labels (RdsInferParser * parser)
{
  for (RdsInferAttribute & a : parser->attrs) {
    free (a.attributeLabel);
    a.attributeLabel = nullptr;
  }
}

gboolean
rds_infer_parser_classify (RdsInferParser * parser, const RdsInferLayerInfo * layers, guint n_layers,
    const RdsInferNetworkInfo * network_info, RdsInferClassifierResult * result, GError ** error)
{
  bool ok;

  g_return_val_if_fail (parser != NULL && parser->classifier, FALSE);
  g_return_val_if_fail (n_layers == 0 || layers != NULL, FALSE);
  g_return_val_if_fail (network_info != NULL, FALSE);
  g_return_val_if_fail (result != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (parser->builtin && !classifier_layers (layers, n_layers, true, error))
    return FALSE;
  try {
    parser->layers.assign (layers, layers + n_layers);
    parser->attrs.clear ();
    parser->desc.clear ();
    ok = parser->classify_func (parser->layers, *network_info, parser->threshold, parser->attrs, parser->desc);
  }
  catch (const std::exception & e) {
    free_attr_labels (parser);
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE,
        "%s: the parse function %s threw an exception: %s", parser->lib_path, parser->func_name, e.what ());
    return FALSE;
  }
  catch (...) {
    free_attr_labels (parser);
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE,
        "%s: the parse function %s threw an exception (not a std::exception)", parser->lib_path,
        parser->func_name);
    return FALSE;
  }
  if (!ok) {
    free_attr_labels (parser);
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_PARSE,
        "%s: the parse function %s returned false", parser->lib_path, parser->func_name);
    return FALSE;
  }
  for (const RdsInferAttribute & a : parser->attrs) {
    guint num_classes = a.attributeIndex < n_layers ? layers[a.attributeIndex].inferDims.numElements : 0;
    const char *label = a.attributeLabel;

    /* The built-in parser's labels come from the label file: "" past a line's labels or without a line (ours). */
    if (parser->builtin) {
      label = "";
      if (a.attributeIndex < parser->labels.size () && a.attributeValue < parser->labels[a.attributeIndex].size ())
        label = parser->labels[a.attributeIndex][a.attributeValue].c_str ();
    }
    rds_infer_classifier_result_add (result, a.attributeIndex, a.attributeValue, num_classes,
        a.attributeConfidence, label);
  }
  free_attr_labels (parser);
  /* a custom parser always has a description, also an empty one (DeepStream 8.0 behaviour): the display text then
   * gets no labels (and, empty, stays unchanged: ours) */
  if (!parser->builtin) {
    g_free (result->desc);
    result->desc = g_strdup (parser->desc.c_str ());
  }
  return TRUE;
}

/* ---- Classifier results ---- */

static void
clear_classifier_label (gpointer data)
{
  g_free (static_cast<RdsInferClassifierLabel *> (data)->label);
}

RdsInferClassifierResult *
rds_infer_classifier_result_new (void)
{
  RdsInferClassifierResult *result = g_new0 (RdsInferClassifierResult, 1);

  result->labels = g_array_new (FALSE, FALSE, sizeof (RdsInferClassifierLabel));
  g_array_set_clear_func (result->labels, clear_classifier_label);
  return result;
}

RdsInferClassifierResult *
rds_infer_classifier_result_copy (const RdsInferClassifierResult * result)
{
  RdsInferClassifierResult *copy;

  g_return_val_if_fail (result != NULL, NULL);

  copy = rds_infer_classifier_result_new ();
  for (guint i = 0; i < result->labels->len; i++) {
    const RdsInferClassifierLabel *l = &g_array_index (result->labels, RdsInferClassifierLabel, i);

    rds_infer_classifier_result_add (copy, l->label_id, l->class_id, l->num_classes, l->prob, l->label);
  }
  copy->desc = g_strdup (result->desc);
  return copy;
}

void
rds_infer_classifier_result_free (RdsInferClassifierResult * result)
{
  if (!result)
    return;
  g_array_unref (result->labels);
  g_free (result->desc);
  g_free (result);
}

void
rds_infer_classifier_result_add (RdsInferClassifierResult * result, guint label_id, guint class_id,
    guint num_classes, gfloat prob, const gchar * label)
{
  RdsInferClassifierLabel l;

  g_return_if_fail (result != NULL);

  l.label_id = label_id;
  l.class_id = class_id;
  l.num_classes = num_classes;
  l.prob = prob;
  l.label = g_strdup (label ? label : "");
  g_array_append_val (result->labels, l);
}
