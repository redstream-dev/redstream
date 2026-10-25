/* Redstream: rdsinfer's custom bbox parser loader. No C++ exception leaves this file.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define RDS_INFER_NO_PARSER_ABI_SYMBOL
#include <rds/rds-infer-custom-impl.h>

#include "rds-infer-parse.h"

#include <dlfcn.h>
#include <exception>

G_DEFINE_QUARK (rds-infer-parser-error-quark, rds_infer_parser_error)

struct _RdsInferParser
{
  void *handle;
  gchar *lib_path, *func_name;
  RdsInferParseCustomFunc func;
  RdsInferParseDetectionParams params;
  /* Reused between calls. */
  std::vector<RdsInferLayerInfo> layers;
  std::vector<RdsInferObjectDetectionInfo> objects;
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

static RdsInferParser *
parser_open (const gchar * lib_path, const gchar * func_name, GError ** error)
{
  RdsInferParser *parser = new RdsInferParser ();
  AbiVersionFunc version_func;
  unsigned int version;
  const char *err;

  parser->lib_path = g_strdup (lib_path);
  parser->func_name = g_strdup (func_name);
  parser->handle = dlopen (lib_path, RTLD_NOW | RTLD_LOCAL);
  if (!parser->handle) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_OPEN,
        "%s: can't load the custom parser library: %s", lib_path, dlerror ());
    goto fail;
  }

  /* dlsym on the handle searches the library and its dependencies only, never the loading process. */
  version_func = reinterpret_cast<AbiVersionFunc> (dlsym (parser->handle, "rds_infer_parser_abi_version"));
  if (!version_func) {
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_ABI,
        "%s: the custom parser library was not built against Redstream's parser header "
        "(rds/rds-infer-custom-impl.h, through rds/rds_compat.h for DeepStream parsers): it has no "
        "rds_infer_parser_abi_version", lib_path);
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
  parser->func = reinterpret_cast<RdsInferParseCustomFunc> (dlsym (parser->handle, func_name));
  if (!parser->func) {
    err = dlerror ();
    g_set_error (error, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_SYMBOL,
        "%s: no parse function %s in the custom parser library (it must be extern \"C\" and exported): %s",
        lib_path, func_name, err ? err : "the symbol is NULL");
    goto fail;
  }
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
    return parser_open (lib_path, func_name, error);
  }
  catch (const std::exception & e) {
    g_error ("rdsinfer: %s: loading the custom parser failed: %s", lib_path, e.what ());
  }
  catch (...) {
    g_error ("rdsinfer: %s: loading the custom parser failed", lib_path);
  }
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
  return parser->func_name;
}

void
rds_infer_parser_set_detection_params (RdsInferParser * parser, guint num_classes,
    const gfloat * pre_cluster_thresholds, const gfloat * post_cluster_thresholds)
{
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

  g_return_val_if_fail (parser != NULL, FALSE);
  g_return_val_if_fail (n_layers == 0 || layers != NULL, FALSE);
  g_return_val_if_fail (network_info != NULL, FALSE);
  g_return_val_if_fail (objects != NULL && g_array_get_element_size (objects) ==
      sizeof (RdsInferObjectDetectionInfo), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

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
