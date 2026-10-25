/* Redstream: rdsinfer's bbox parsers (plugin-internal, not installed): the custom parser loader and the built-in
 * YOLO parser. C-callable; the parser ABI itself (rds/rds-infer-custom-impl.h) is C++.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_PARSE_H
#define RDS_INFER_PARSE_H

#include <glib.h>
#include <rds/rds-infer.h>

G_BEGIN_DECLS

#define RDS_INFER_PARSER_ERROR (rds_infer_parser_error_quark ())
G_GNUC_INTERNAL GQuark rds_infer_parser_error_quark (void);

/* Messages start with the library path ("built-in parser" for the built-in ones). */
typedef enum
{
  RDS_INFER_PARSER_ERROR_OPEN,          /* dlopen failed (the dlerror text follows) */
  RDS_INFER_PARSER_ERROR_ABI,           /* no rds_infer_parser_abi_version of its own, or a different version */
  RDS_INFER_PARSER_ERROR_SYMBOL,        /* the parse function is not in the library, or no such built-in parser */
  RDS_INFER_PARSER_ERROR_PARSE,         /* the parse function returned false or threw */
  RDS_INFER_PARSER_ERROR_LAYERS,        /* the output layers don't fit the built-in parser (the shape follows) */
} RdsInferParserError;

/* The built-in YOLO parser (parse-bbox-func-name without custom-lib-path): one FP32 output layer per image, of
 * shape [4 + C, A] (Ultralytics v8/11 detect export) or [A, 4 + C], where C is num-detected-classes and A the
 * number of anchors; the layout is the one whose dimension equals 4 + C ([4 + C, A] if both do). Rows 0-3 are the
 * box centre x, y, width and height in network-input pixels, rows 4.. the class scores (no objectness). Each anchor
 * gives at most one object: its best class (the lowest class id among equal scores), if that score is at least the
 * class's pre-cluster threshold. */
#define RDS_INFER_PARSER_YOLO "RdsInferParseYolo"

/* A parse function: of a loaded custom parser library, or built in. Not thread-safe: call it from one thread at a
 * time. Libraries are loaded with RTLD_NODELETE: rds_infer_parser_free drops the handle, the library stays mapped.
 * That includes a library that fails the ABI or symbol check: it stays mapped, and its constructors have run. */
typedef struct _RdsInferParser RdsInferParser;

/* Loads @lib_path (RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE), checks that it has its own rds_infer_parser_abi_version
 * with this ABI version, and looks up @func_name. */
G_GNUC_INTERNAL RdsInferParser *rds_infer_parser_open (const gchar * lib_path, const gchar * func_name,
    GError ** error);
/* The built-in parser named @func_name (RDS_INFER_PARSER_YOLO); any other name is an error naming the built-in
 * ones. */
G_GNUC_INTERNAL RdsInferParser *rds_infer_parser_open_builtin (const gchar * func_name, GError ** error);
/* Releases the library handle (the library stays loaded). NULL is allowed. */
G_GNUC_INTERNAL void rds_infer_parser_free (RdsInferParser * parser);
G_GNUC_INTERNAL const gchar *rds_infer_parser_get_func_name (const RdsInferParser * parser);

/* The detection params passed to every call: @num_classes entries in each threshold array. Zero classes and no
 * thresholds until set. */
G_GNUC_INTERNAL void rds_infer_parser_set_detection_params (RdsInferParser * parser, guint num_classes,
    const gfloat * pre_cluster_thresholds, const gfloat * post_cluster_thresholds);

/* Checks the network's @n_layers output layers once, before the first call: a built-in parser checks that it can
 * parse them, with the num_classes of rds_infer_parser_set_detection_params (the error names the shapes it got and
 * the ones it needs); a custom parser accepts anything. rds_infer_parser_parse checks a built-in parser's layers
 * again on every call. */
G_GNUC_INTERNAL gboolean rds_infer_parser_check_layers (const RdsInferParser * parser,
    const RdsInferLayerInfo * layers, guint n_layers, GError ** error);

/* Calls the parse function on one image's @n_layers output layers and appends what it returns to @objects (a GArray
 * of RdsInferObjectDetectionInfo). On a false return or an exception, @objects is left as it was and @error says
 * which (with the exception's what() when it is a std::exception), or, for a built-in parser, which layer doesn't
 * fit (RDS_INFER_PARSER_ERROR_LAYERS). The objects are not validated: rds_infer_postprocess does that. */
G_GNUC_INTERNAL gboolean rds_infer_parser_parse (RdsInferParser * parser, const RdsInferLayerInfo * layers,
    guint n_layers, const RdsInferNetworkInfo * network_info, GArray * objects, GError ** error);

G_END_DECLS

#endif /* RDS_INFER_PARSE_H */
