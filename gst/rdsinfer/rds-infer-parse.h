/* Redstream: rdsinfer's custom bbox parser loader (plugin-internal, not installed). C-callable; the parser ABI
 * itself (rds/rds-infer-custom-impl.h) is C++.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_PARSE_H
#define RDS_INFER_PARSE_H

#include <glib.h>
#include <rds/rds-infer.h>

G_BEGIN_DECLS

#define RDS_INFER_PARSER_ERROR (rds_infer_parser_error_quark ())
G_GNUC_INTERNAL GQuark rds_infer_parser_error_quark (void);

/* Messages start with the library path. */
typedef enum
{
  RDS_INFER_PARSER_ERROR_OPEN,          /* dlopen failed (the dlerror text follows) */
  RDS_INFER_PARSER_ERROR_ABI,           /* no rds_infer_parser_abi_version of its own, or a different version */
  RDS_INFER_PARSER_ERROR_SYMBOL,        /* the parse function is not in the library */
  RDS_INFER_PARSER_ERROR_PARSE,         /* the parse function returned false or threw */
} RdsInferParserError;

/* A parse function of a loaded custom parser library. Not thread-safe: call it from one thread at a time. Libraries
 * are loaded with RTLD_NODELETE: rds_infer_parser_free drops the handle, the library stays mapped. */
typedef struct _RdsInferParser RdsInferParser;

/* Loads @lib_path (RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE), checks that it has its own rds_infer_parser_abi_version
 * with this ABI version, and looks up @func_name. */
G_GNUC_INTERNAL RdsInferParser *rds_infer_parser_open (const gchar * lib_path, const gchar * func_name,
    GError ** error);
/* Releases the library handle (the library stays loaded). NULL is allowed. */
G_GNUC_INTERNAL void rds_infer_parser_free (RdsInferParser * parser);
G_GNUC_INTERNAL const gchar *rds_infer_parser_get_func_name (const RdsInferParser * parser);

/* The detection params passed to every call: @num_classes entries in each threshold array. Zero classes and no
 * thresholds until set. */
G_GNUC_INTERNAL void rds_infer_parser_set_detection_params (RdsInferParser * parser, guint num_classes,
    const gfloat * pre_cluster_thresholds, const gfloat * post_cluster_thresholds);

/* Calls the parse function on one image's @n_layers output layers and appends what it returns to @objects (a GArray
 * of RdsInferObjectDetectionInfo). On a false return or an exception, @objects is left as it was and @error says
 * which (with the exception's what() when it is a std::exception). */
G_GNUC_INTERNAL gboolean rds_infer_parser_parse (RdsInferParser * parser, const RdsInferLayerInfo * layers,
    guint n_layers, const RdsInferNetworkInfo * network_info, GArray * objects, GError ** error);

G_END_DECLS

#endif /* RDS_INFER_PARSE_H */
