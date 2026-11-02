/* Redstream: rdsinfer's bbox and classifier parsers (plugin-internal, not installed): the custom parser loader, the
 * built-in YOLO parser, the built-in classifier parser and the classifier results they give. C-callable; the parser
 * ABI itself (rds/rds-infer-custom-impl.h) is C++.
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
 * class's pre-cluster threshold. Shapes are per image: a layer's inferDims have no batch dimension (the engine
 * strips it), so the model output [1, 84, 8400] arrives as [84, 8400]. */
#define RDS_INFER_PARSER_YOLO "RdsInferParseYolo"

/* The built-in classifier parser (network-type=1 without parse-classifier-func-name): for each output layer (FP32,
 * at least one value, any shape: its values are the classes), the raw argmax (no softmax: the value is the
 * probability as it is) with ties to the lowest class index (NaN never wins), kept if strictly above
 * classifier-threshold (DeepStream 8.0 behaviour). The label is that layer's line of the label file, the class's
 * entry in it, or "" when the line or the entry is missing (ours). No description. */
#define RDS_INFER_PARSER_CLASSIFIER "RdsInferParseClassifier"

/* One label of a classifier result: what becomes an RdsLabelInfo. */
typedef struct
{
  guint label_id;               /* the output layer (a custom parser's attributeIndex) */
  guint class_id;               /* the class (attributeValue) */
  guint num_classes;            /* the values of output layer label_id; 0 when there is no such layer (ours) */
  gfloat prob;                  /* attributeConfidence */
  gchar *label;                 /* owned, never NULL ("" for none) */
} RdsInferClassifierLabel;

/* What a classifier parser found for one object, as values (no metadata pointers): owned, freed whole with
 * rds_infer_classifier_result_free, deep-copied with rds_infer_classifier_result_copy. */
typedef struct
{
  GArray *labels;               /* RdsInferClassifierLabel, in the parser's order; clearing it frees the strings */
  gchar *desc;                  /* owned: a custom parser's descString, "" when it left it empty; NULL for the
                                 * built-in parser */
} RdsInferClassifierResult;

G_GNUC_INTERNAL RdsInferClassifierResult *rds_infer_classifier_result_new (void);
G_GNUC_INTERNAL RdsInferClassifierResult *rds_infer_classifier_result_copy (const RdsInferClassifierResult * result);
/* NULL is allowed. */
G_GNUC_INTERNAL void rds_infer_classifier_result_free (RdsInferClassifierResult * result);
/* Appends a label; @label is copied (NULL: ""). */
G_GNUC_INTERNAL void rds_infer_classifier_result_add (RdsInferClassifierResult * result, guint label_id,
    guint class_id, guint num_classes, gfloat prob, const gchar * label);

/* A parse function: of a loaded custom parser library, or built in; a bbox parser or a classifier parser. Not
 * thread-safe: call it from one thread at a time. Libraries are loaded with RTLD_NODELETE: rds_infer_parser_free
 * drops the handle, the library stays mapped. That includes a library that fails the ABI or symbol check: it stays
 * mapped, and its constructors have run. */
typedef struct _RdsInferParser RdsInferParser;

/* Loads @lib_path (RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE), checks that it has its own rds_infer_parser_abi_version
 * with this ABI version, and looks up @func_name. */
G_GNUC_INTERNAL RdsInferParser *rds_infer_parser_open (const gchar * lib_path, const gchar * func_name,
    GError ** error);
/* The built-in parser named @func_name (RDS_INFER_PARSER_YOLO); any other name is an error naming the built-in
 * ones. */
G_GNUC_INTERNAL RdsInferParser *rds_infer_parser_open_builtin (const gchar * func_name, GError ** error);
/* A classifier parser: as rds_infer_parser_open, @func_name an RdsInferClassifierParseCustomFunc. */
G_GNUC_INTERNAL RdsInferParser *rds_infer_parser_open_classifier (const gchar * lib_path, const gchar * func_name,
    GError ** error);
/* The built-in classifier parser (RDS_INFER_PARSER_CLASSIFIER). */
G_GNUC_INTERNAL RdsInferParser *rds_infer_parser_open_builtin_classifier (void);
/* Releases the library handle (the library stays loaded). NULL is allowed. */
G_GNUC_INTERNAL void rds_infer_parser_free (RdsInferParser * parser);
G_GNUC_INTERNAL const gchar *rds_infer_parser_get_func_name (const RdsInferParser * parser);

/* The detection params passed to every call: @num_classes entries in each threshold array. Zero classes and no
 * thresholds until set. */
G_GNUC_INTERNAL void rds_infer_parser_set_detection_params (RdsInferParser * parser, guint num_classes,
    const gfloat * pre_cluster_thresholds, const gfloat * post_cluster_thresholds);

/* A classifier parser's params, passed to (or used by) every call: classifier-threshold, and for the built-in
 * parser the label file's lines (RdsInferConfig.classifier_labels: one gchar ** per output layer; copied, NULL for
 * none). A threshold of 0 and no labels until set. */
G_GNUC_INTERNAL void rds_infer_parser_set_classifier_params (RdsInferParser * parser, gfloat threshold,
    const GPtrArray * labels);

/* Checks the network's @n_layers output layers once, before the first call: a built-in parser checks that it can
 * parse them, with the num_classes of rds_infer_parser_set_detection_params for the YOLO parser (the error names
 * the shapes it got and the ones it needs); a custom parser accepts anything. rds_infer_parser_parse and
 * rds_infer_parser_classify check a built-in parser's layers again on every call. */
G_GNUC_INTERNAL gboolean rds_infer_parser_check_layers (const RdsInferParser * parser,
    const RdsInferLayerInfo * layers, guint n_layers, GError ** error);

/* Calls the parse function on one image's @n_layers output layers and appends what it returns to @objects (a GArray
 * of RdsInferObjectDetectionInfo). On a false return or an exception, @objects is left as it was and @error says
 * which (with the exception's what() when it is a std::exception), or, for a built-in parser, which layer doesn't
 * fit (RDS_INFER_PARSER_ERROR_LAYERS). The objects are not validated: rds_infer_postprocess does that. */
G_GNUC_INTERNAL gboolean rds_infer_parser_parse (RdsInferParser * parser, const RdsInferLayerInfo * layers,
    guint n_layers, const RdsInferNetworkInfo * network_info, GArray * objects, GError ** error);

/* Calls a classifier parser on one image's @n_layers output layers and appends its labels to @result (and, for a
 * custom parser, sets its desc to its descString, also an empty one: DeepStream 8.0 behaviour). A custom
 * parser's attributes are taken as they are: attributeLabel NULL is "", and no threshold is applied (the parser
 * applies classifier-threshold itself); every attributeLabel is freed, also after a false return or an exception.
 * On failure @result is left as it was and @error says why, as for rds_infer_parser_parse. */
G_GNUC_INTERNAL gboolean rds_infer_parser_classify (RdsInferParser * parser, const RdsInferLayerInfo * layers,
    guint n_layers, const RdsInferNetworkInfo * network_info, RdsInferClassifierResult * result, GError ** error);

G_END_DECLS

#endif /* RDS_INFER_PARSE_H */
