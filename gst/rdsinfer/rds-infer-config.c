/* Redstream: the rdsinfer config parser. One table lists every nvinfer config key with its type, range, default
 * and status; it drives parsing, the supported-value checks and the warnings (and the compat matrix is written from
 * it). YAML files are read into a GKeyFile first, so both formats go through the same typed parser.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-config.h"
#include "rds-keyfile-yaml.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <string.h>

G_DEFINE_QUARK (rds-infer-config-error-quark, rds_infer_config_error)

#define MAX_CLASSES 65536       /* ours: bounds the per-class arrays */
#define MAX_OFFSETS 4           /* nvinfer's documented maximum channel count */
G_STATIC_ASSERT (MAX_OFFSETS >= 3);     /* parse_value's vals[] also holds infer-dims */
#define PROPERTY_GROUP "property"
#define CLASS_ATTRS_ALL "class-attrs-all"
#define CLASS_ATTRS_PREFIX "class-attrs-"

typedef enum
{
  GROUP_PROPERTY,
  GROUP_CLASS_ATTRS,
} KeyGroup;

typedef enum
{
  TYPE_BOOL,                    /* 0/1 or true/false */
  TYPE_INT,
  TYPE_UINT,
  TYPE_FLOAT,
  TYPE_STRING,
  TYPE_PATH,                    /* a string, relative to the config file's directory */
  TYPE_OFFSETS,                 /* ';' list of up to MAX_OFFSETS floats: offsets, n_offsets */
  TYPE_DIMS,                    /* C;H;W: infer_dims, has_infer_dims */
  TYPE_UINT_LIST,               /* ';' list into a GArray of guint */
  TYPE_INT_LIST,                /* ';' list, checked only (no field) */
  TYPE_STRING_LIST,             /* ';' list into a gchar ** */
} KeyType;

static const gchar *const type_names[] = {
  [TYPE_BOOL] = "bool",[TYPE_INT] = "int",[TYPE_UINT] = "uint",[TYPE_FLOAT] = "float",[TYPE_STRING] = "string",
  [TYPE_PATH] = "path",[TYPE_OFFSETS] = "float list",[TYPE_DIMS] = "dims",[TYPE_UINT_LIST] = "uint list",
  [TYPE_INT_LIST] = "int list",[TYPE_STRING_LIST] = "string list",
};

typedef struct
{
  const gchar *name;
  KeyGroup group;
  KeyType type;
  RdsInferKeyStatus status;
  gdouble min, max;             /* numbers, and the elements of number lists */
  gboolean min_exclusive;
  const gchar *def;             /* default, parsed like a value from the file; NULL: zero / unset */
  gssize field;                 /* offset + 1 into RdsInferConfig (property) or RdsInferClassAttrs; 0: not stored */
  gint accept[2];               /* with n_accept > 0, the only supported values; others are rejected */
  guint n_accept;
  const gchar *note;            /* why the key is a no-op or rejected, or why other values are rejected */
} KeyInfo;

#define P(f) (G_STRUCT_OFFSET (RdsInferConfig, f) + 1)
#define C(f) (G_STRUCT_OFFSET (RdsInferClassAttrs, f) + 1)
#define FIELD(type, base, k) G_STRUCT_MEMBER (type, base, (k)->field - 1)
#define PROP GROUP_PROPERTY
#define CLS GROUP_CLASS_ATTRS
#define SUP RDS_INFER_KEY_SUPPORTED
#define NOOP RDS_INFER_KEY_NOOP
#define REJ RDS_INFER_KEY_REJECTED

#define NOTE_TRT "a TensorRT setting; MIGraphX has no equivalent"
#define NOTE_DLA "DLA is not available on AMD GPUs"
#define NOTE_SECONDARY "used in secondary mode (process-mode=2) only, which is not supported yet"
#define NOTE_CLASSIFIER "classifiers are not supported yet"
#define NOTE_SEGMENTATION "segmentation models are not supported yet"
#define NOTE_CLUSTER "used only by GroupRectangles and DBSCAN clustering, which are not supported"

/* Defaults are ours (the nvinfer documentation gives none). Ranges are the documented ones, plus our bounds on
 * counts. Keys are processed in table order, so a later entry for the same field wins within a group
 * (pre-cluster-threshold over its legacy alias threshold). */
static const KeyInfo keys[] = {
  /* [property]: model and input */
  {.name = "onnx-file",.group = PROP,.type = TYPE_PATH,.status = SUP,.field = P (onnx_file)},
  {.name = "model-engine-file",.group = PROP,.type = TYPE_PATH,.status = SUP,.field = P (model_engine_file)},
  {.name = "labelfile-path",.group = PROP,.type = TYPE_PATH,.status = SUP,.field = P (labelfile_path)},
  {.name = "infer-dims",.group = PROP,.type = TYPE_DIMS,.status = SUP,.min = 1,.max = G_MAXINT,
      .field = P (infer_dims)},
  {.name = "output-blob-names",.group = PROP,.type = TYPE_STRING_LIST,.status = SUP,
      .field = P (output_blob_names)},
  {.name = "batch-size",.group = PROP,.type = TYPE_UINT,.status = SUP,.min = 1,.max = RDS_INFER_MAX_BATCH_SIZE,
      .def = "1",.field = P (batch_size)},
  {.name = "network-mode",.group = PROP,.type = TYPE_INT,.status = SUP,.min = 0,.max = 3,.def = "0",
        .field = P (network_mode),.accept = {0, 2},.n_accept = 2,
      .note = "only FP32 (0) and FP16 (2) are supported; INT8 (1) and BEST (3) are not"},
  {.name = "network-type",.group = PROP,.type = TYPE_INT,.status = SUP,.min = 0,.max = 100,.def = "0",
        .field = P (network_type),.accept = {0, 100},.n_accept = 2,
      .note = "only detectors (0) and no post-processing (100) are supported; "
        "classifiers and segmentation models are not supported yet"},
  {.name = "process-mode",.group = PROP,.type = TYPE_INT,.status = SUP,.min = 1,.max = 2,.def = "1",
        .field = P (process_mode),.accept = {1},.n_accept = 1,
      .note = "only primary mode (1) is supported; secondary mode (2) is not supported yet"},
  {.name = "gie-unique-id",.group = PROP,.type = TYPE_UINT,.status = SUP,.min = 1,.max = G_MAXUINT,.def = "1",
      .field = P (gie_unique_id)},
  {.name = "interval",.group = PROP,.type = TYPE_UINT,.status = SUP,.min = 0,.max = RDS_INFER_MAX_INTERVAL,
      .def = "0",.field = P (interval)},
  {.name = "output-tensor-meta",.group = PROP,.type = TYPE_BOOL,.status = SUP,.def = "0",
      .field = P (output_tensor_meta)},
  /* preprocessing */
  {.name = "net-scale-factor",.group = PROP,.type = TYPE_FLOAT,.status = SUP,.min = 0,.max = G_MAXFLOAT,
      .min_exclusive = TRUE,.def = "1.0",.field = P (net_scale_factor)},
  {.name = "offsets",.group = PROP,.type = TYPE_OFFSETS,.status = SUP,.min = 0,.max = G_MAXFLOAT,
      .field = P (offsets)},
  {.name = "model-color-format",.group = PROP,.type = TYPE_INT,.status = SUP,.min = 0,.max = 2,.def = "0",
      .field = P (model_color_format)},
  {.name = "network-input-order",.group = PROP,.type = TYPE_INT,.status = SUP,.min = 0,.max = 1,.def = "0",
      .field = P (network_input_order)},
  {.name = "maintain-aspect-ratio",.group = PROP,.type = TYPE_BOOL,.status = SUP,.def = "0",
      .field = P (maintain_aspect_ratio)},
  {.name = "symmetric-padding",.group = PROP,.type = TYPE_BOOL,.status = SUP,.def = "0",
      .field = P (symmetric_padding)},
  {.name = "scaling-filter",.group = PROP,.type = TYPE_INT,.status = SUP,.min = 0,.max = 6,.def = "1",
      .field = P (scaling_filter)},
  /* detector post-processing */
  {.name = "num-detected-classes",.group = PROP,.type = TYPE_UINT,.status = SUP,.min = 1,.max = MAX_CLASSES,
      .field = P (num_detected_classes)},
  {.name = "cluster-mode",.group = PROP,.type = TYPE_INT,.status = SUP,.min = 0,.max = 4,.def = "2",
        .field = P (cluster_mode),.accept = {2, 4},.n_accept = 2,
      .note = "only NMS (2) and no clustering (4) are supported"},
  {.name = "parse-bbox-func-name",.group = PROP,.type = TYPE_STRING,.status = SUP,
      .field = P (parse_bbox_func_name)},
  {.name = "custom-lib-path",.group = PROP,.type = TYPE_PATH,.status = SUP,.field = P (custom_lib_path)},
  {.name = "filter-out-class-ids",.group = PROP,.type = TYPE_UINT_LIST,.status = SUP,.min = 0,.max = G_MAXUINT,
      .field = P (filter_out_class_ids)},

  /* [property]: accepted, without effect */
  {.name = "gpu-id",.group = PROP,.type = TYPE_UINT,.status = NOOP,.min = 0,.max = G_MAXUINT,
      .note = "the GPU is chosen with the element's gpu-id property"},
  {.name = "workspace-size",.group = PROP,.type = TYPE_UINT,.status = NOOP,.min = 0,.max = G_MAXUINT,
      .note = NOTE_TRT},
  {.name = "force-implicit-batch-dim",.group = PROP,.type = TYPE_BOOL,.status = NOOP,
      .note = NOTE_TRT},
  {.name = "enable-dla",.group = PROP,.type = TYPE_BOOL,.status = NOOP,.accept = {0},
      .n_accept = 1,.note = NOTE_DLA},
  {.name = "use-dla-core",.group = PROP,.type = TYPE_INT,.status = NOOP,.min = 0,.max = G_MAXINT,
      .note = NOTE_DLA},
  {.name = "tensor-meta-pool-size",.group = PROP,.type = TYPE_UINT,.status = NOOP,.min = 1,.max = G_MAXUINT,
      .note = "tensor output buffers are allocated as needed"},
  {.name = "scaling-compute-hw",.group = PROP,.type = TYPE_INT,.status = NOOP,.min = 0,.max = 2,
        .accept = {0, 1},.n_accept = 2,
      .note = "frames are always scaled on the GPU; VIC (2) is a Jetson unit"},
  {.name = "crop-objects-to-roi-boundary",.group = PROP,.type = TYPE_BOOL,.status = NOOP,
      .note = "it applies to preprocessed-tensor ROIs, which are not supported yet"},
  {.name = "is-classifier",.group = PROP,.type = TYPE_BOOL,.status = NOOP,.accept = {0},
      .n_accept = 1,.note = "legacy key; " NOTE_CLASSIFIER},
  {.name = "enable-dbscan",.group = PROP,.type = TYPE_BOOL,.status = NOOP,.accept = {0},
      .n_accept = 1,.note = "deprecated; DBSCAN clustering is not supported, use cluster-mode=2 (NMS)"},
  {.name = "int8-calib-file",.group = PROP,.type = TYPE_STRING,.status = NOOP,
      .note = "used only for INT8 (network-mode=1), which is not supported"},
  {.name = "output-instance-mask",.group = PROP,.type = TYPE_BOOL,.status = NOOP,.accept = {0},
      .n_accept = 1,.note = "instance segmentation is not supported yet"},
  {.name = "input-tensor-from-meta",.group = PROP,.type = TYPE_BOOL,.status = NOOP,
      .accept = {0},.n_accept = 1,.note = "preprocessed tensor input is not supported yet"},
  {.name = "operate-on-gie-id",.group = PROP,.type = TYPE_INT,.status = NOOP,.min = -1,.max = G_MAXINT,
      .note = NOTE_SECONDARY},
  {.name = "operate-on-class-ids",.group = PROP,.type = TYPE_INT_LIST,.status = NOOP,.min = -1,.max = G_MAXINT,
      .note = NOTE_SECONDARY},
  {.name = "input-object-min-width",.group = PROP,.type = TYPE_UINT,.status = NOOP,.min = 0,.max = G_MAXUINT,
      .note = NOTE_SECONDARY},
  {.name = "input-object-min-height",.group = PROP,.type = TYPE_UINT,.status = NOOP,.min = 0,.max = G_MAXUINT,
      .note = NOTE_SECONDARY},
  {.name = "input-object-max-width",.group = PROP,.type = TYPE_UINT,.status = NOOP,.min = 0,.max = G_MAXUINT,
      .note = NOTE_SECONDARY},
  {.name = "input-object-max-height",.group = PROP,.type = TYPE_UINT,.status = NOOP,.min = 0,.max = G_MAXUINT,
      .note = NOTE_SECONDARY},

  /* [property]: rejected */
  {.name = "model-file",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "Caffe models are not supported; use onnx-file"},
  {.name = "proto-file",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "Caffe models are not supported; use onnx-file"},
  {.name = "mean-file",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "mean image files are not supported; use offsets"},
  {.name = "tlt-encoded-model",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "encoded TAO models are not supported; export the model to ONNX"},
  {.name = "tlt-model-key",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "encoded TAO models are not supported; export the model to ONNX"},
  {.name = "uff-input-dims",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "UFF models are not supported; use onnx-file and infer-dims"},
  {.name = "uff-input-blob-name",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "UFF models are not supported; use onnx-file"},
  {.name = "uff-input-order",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "UFF models are not supported; use onnx-file and network-input-order"},
  {.name = "engine-create-func-name",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "custom TensorRT engine builders are not supported; use onnx-file"},
  {.name = "custom-network-config",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "custom TensorRT engine builders are not supported; use onnx-file"},
  {.name = "layer-device-precision",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "per-layer precision is a TensorRT setting; use network-mode"},
  {.name = "output-io-formats",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "I/O formats are a TensorRT setting; inputs and outputs are always FP32"},
  {.name = "parse-bbox-instance-mask-func-name",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = "instance segmentation is not supported yet"},
  {.name = "parse-classifier-func-name",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_CLASSIFIER},
  {.name = "classifier-threshold",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_CLASSIFIER},
  {.name = "classifier-type",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_CLASSIFIER},
  {.name = "classifier-async-mode",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_CLASSIFIER},
  {.name = "secondary-reinfer-interval",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_SECONDARY},
  {.name = "parse-segmentation-func-name",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_SEGMENTATION},
  {.name = "segmentation-threshold",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_SEGMENTATION},
  {.name = "segmentation-output-order",.group = PROP,.type = TYPE_STRING,.status = REJ,
      .note = NOTE_SEGMENTATION},

  /* [class-attrs-all] and [class-attrs-<id>] */
  {.name = "threshold",.group = CLS,.type = TYPE_FLOAT,.status = SUP,.min = 0,.max = G_MAXFLOAT,
      .field = C (pre_cluster_threshold),.note = "legacy name of pre-cluster-threshold, which wins if both are set"},
  {.name = "pre-cluster-threshold",.group = CLS,.type = TYPE_FLOAT,.status = SUP,.min = 0,.max = G_MAXFLOAT,
      .def = "0.25",.field = C (pre_cluster_threshold)},
  {.name = "post-cluster-threshold",.group = CLS,.type = TYPE_FLOAT,.status = SUP,.min = 0,.max = G_MAXFLOAT,
      .def = "0.0",.field = C (post_cluster_threshold)},
  {.name = "nms-iou-threshold",.group = CLS,.type = TYPE_FLOAT,.status = SUP,.min = 0,.max = G_MAXFLOAT,
      .def = "0.45",.field = C (nms_iou_threshold)},
  {.name = "topk",.group = CLS,.type = TYPE_INT,.status = SUP,.min = -1,.max = G_MAXINT,.def = "-1",
      .field = C (topk)},
  {.name = "roi-top-offset",.group = CLS,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,.def = "0",
      .field = C (roi_top_offset)},
  {.name = "roi-bottom-offset",.group = CLS,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,.def = "0",
      .field = C (roi_bottom_offset)},
  {.name = "detected-min-w",.group = CLS,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,.def = "0",
      .field = C (detected_min_w)},
  {.name = "detected-min-h",.group = CLS,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,.def = "0",
      .field = C (detected_min_h)},
  {.name = "detected-max-w",.group = CLS,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,.def = "0",
      .field = C (detected_max_w)},
  {.name = "detected-max-h",.group = CLS,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,.def = "0",
      .field = C (detected_max_h)},
  {.name = "eps",.group = CLS,.type = TYPE_FLOAT,.status = NOOP,.min = 0,.max = G_MAXFLOAT,
      .note = NOTE_CLUSTER},
  {.name = "group-threshold",.group = CLS,.type = TYPE_INT,.status = NOOP,.min = 0,.max = G_MAXINT,
      .note = NOTE_CLUSTER},
  {.name = "minBoxes",.group = CLS,.type = TYPE_INT,.status = NOOP,.min = 0,.max = G_MAXINT,
      .note = NOTE_CLUSTER},
  {.name = "dbscan-min-score",.group = CLS,.type = TYPE_FLOAT,.status = NOOP,.min = 0,.max = G_MAXFLOAT,
      .note = NOTE_CLUSTER},
};

typedef struct
{
  const gchar *path;
  RdsInferConfig *cfg;
  GHashTable *warned;           /* key names already warned about */
} LoadCtx;

/* "KEY" in [property], "GROUP:KEY" elsewhere. */
static gchar *
qualify (const gchar * group, const gchar * key)
{
  return group && strcmp (group, PROPERTY_GROUP) != 0 ? g_strconcat (group, ":", key, NULL) : g_strdup (key);
}

static gboolean G_GNUC_PRINTF (7, 8)
key_error (GError ** error, gint code, const gchar * path, const gchar * group, const gchar * key,
    const gchar * value, const gchar * fmt, ...)
{
  gchar *qkey = qualify (group, key), *reason;
  va_list ap;

  va_start (ap, fmt);
  reason = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_INFER_CONFIG_ERROR, code, "%s:%s: %s: %s", path, qkey, value, reason);
  g_free (reason);
  g_free (qkey);
  return FALSE;
}

/* At most one warning per key name and load. */
static void G_GNUC_PRINTF (4, 5)
warn_once (LoadCtx * ctx, const gchar * group, const gchar * key, const gchar * fmt, ...)
{
  gchar *qkey, *msg;
  va_list ap;

  if (!g_hash_table_add (ctx->warned, g_strdup (key)))
    return;
  qkey = qualify (group, key);
  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_ptr_array_add (ctx->cfg->warnings, g_strdup_printf ("%s:%s: %s", ctx->path, qkey, msg));
  g_free (msg);
  g_free (qkey);
}

/* Locale-independent %g. */
static const gchar *
fmt_num (gchar * buf, gdouble v)
{
  return g_ascii_formatd (buf, G_ASCII_DTOSTR_BUF_SIZE, "%.10g", v);
}

static gboolean
is_number_type (KeyType type)
{
  return type == TYPE_INT || type == TYPE_UINT || type == TYPE_FLOAT || type == TYPE_OFFSETS || type == TYPE_DIMS
      || type == TYPE_UINT_LIST || type == TYPE_INT_LIST;
}

static gboolean
is_integer_type (KeyType type)
{
  return type == TYPE_INT || type == TYPE_UINT || type == TYPE_DIMS || type == TYPE_UINT_LIST
      || type == TYPE_INT_LIST;
}

/* Parses one number of @k (a scalar or a list element) and checks its range; on failure @reason says why. */
static gboolean
parse_number (const KeyInfo * k, const gchar * s, gdouble * out, gchar ** reason)
{
  gchar *end = NULL, buf[G_ASCII_DTOSTR_BUF_SIZE];
  gdouble v;

  errno = 0;
  if (is_integer_type (k->type)) {
    gint64 i = g_ascii_strtoll (s, &end, 10);

    if (!*s || *end) {
      *reason = g_strdup ("not an integer");
      return FALSE;
    }
    if (errno == ERANGE) {
      *reason = g_strdup ("out of range");
      return FALSE;
    }
    v = (gdouble) i;
  } else {
    v = g_ascii_strtod (s, &end);
    if (!*s || *end) {
      *reason = g_strdup ("not a number");
      return FALSE;
    }
    if (!isfinite (v)) {
      *reason = g_strdup ("not a finite number");
      return FALSE;
    }
    /* floats are stored as gfloat: values it can't hold (including ones that would become 0) are out of range */
    if (errno == ERANGE || fabs (v) > G_MAXFLOAT || (v != 0 && fabs (v) < G_MINFLOAT)) {
      *reason = g_strdup ("out of range for a 32-bit float");
      return FALSE;
    }
  }
  if (k->min_exclusive ? v <= k->min : v < k->min) {
    *reason = g_strdup_printf ("must be %s %s", k->min_exclusive ? "greater than" : "at least", fmt_num (buf, k->min));
    return FALSE;
  }
  if (v > k->max) {
    *reason = g_strdup_printf ("must be at most %s", fmt_num (buf, k->max));
    return FALSE;
  }
  *out = v;
  return TRUE;
}

/* Splits a ';' list into trimmed elements. A trailing ';' is allowed; an empty element elsewhere is not. */
static gchar **
split_list (const gchar * s, gchar ** reason)
{
  gchar **items = g_strsplit (s, ";", -1);
  guint n = g_strv_length (items);

  for (guint i = 0; i < n; i++)
    g_strstrip (items[i]);
  if (n > 0 && !*items[n - 1]) {
    g_free (items[n - 1]);
    items[--n] = NULL;
  }
  for (guint i = 0; i < n; i++) {
    if (!*items[i]) {
      *reason = g_strdup ("empty list element");
      g_strfreev (items);
      return NULL;
    }
  }
  return items;
}

static gboolean
parse_bool (const gchar * s, gboolean * out)
{
  if (!strcmp (s, "1") || !g_ascii_strcasecmp (s, "true"))
    *out = TRUE;
  else if (!strcmp (s, "0") || !g_ascii_strcasecmp (s, "false"))
    *out = FALSE;
  else
    return FALSE;
  return TRUE;
}

/* Parses @raw as @k's type. With @base, stores it into k's field (paths resolved against
 * @config_dir); without, only checks it. @num receives the value of a scalar bool or number (for the accepted-value
 * checks). On failure @reason says why. */
static gboolean
parse_value (const KeyInfo * k, const gchar * raw, gpointer base, const gchar * config_dir, gdouble * num,
    gchar ** reason)
{
  gchar *s = g_strstrip (g_strdup (raw)), **items = NULL;
  gboolean ok = TRUE;
  gdouble v = 0;
  guint n;

  switch (k->type) {
    case TYPE_BOOL:{
      gboolean b = FALSE;

      if (!(ok = parse_bool (s, &b)))
        *reason = g_strdup ("not a boolean (use 0/1 or true/false)");
      else if (base)
        FIELD (gboolean, base, k) = b;
      v = b;
      break;
    }
    case TYPE_INT:
    case TYPE_UINT:
    case TYPE_FLOAT:
      if (!(ok = parse_number (k, s, &v, reason)) || !base)
        break;
      if (k->type == TYPE_INT)
        FIELD (gint, base, k) = (gint) v;
      else if (k->type == TYPE_UINT)
        FIELD (guint, base, k) = (guint) v;
      else
        FIELD (gfloat, base, k) = (gfloat) v;
      break;
    case TYPE_STRING:
    case TYPE_PATH:
      if (base) {
        gchar **field = &FIELD (gchar *, base, k);

        g_free (*field);
        if (!*s)
          *field = NULL;        /* an empty value is the same as no value */
        else if (k->type == TYPE_PATH && !g_path_is_absolute (s))
          *field = g_canonicalize_filename (s, config_dir);
        else
          *field = g_strdup (s);
      }
      break;
    case TYPE_OFFSETS:
    case TYPE_DIMS:
    case TYPE_UINT_LIST:
    case TYPE_INT_LIST:{
      gdouble vals[MAX_OFFSETS] = { 0 };
      GArray *ids = NULL;

      if (!(ok = (items = split_list (s, reason)) != NULL))
        break;
      n = g_strv_length (items);
      if (k->type == TYPE_OFFSETS && n > MAX_OFFSETS) {
        *reason = g_strdup_printf ("at most %d values", MAX_OFFSETS);
        ok = FALSE;
        break;
      }
      if (k->type == TYPE_DIMS && n != 3) {
        *reason = g_strdup ("must be channels;height;width");
        ok = FALSE;
        break;
      }
      if (k->type == TYPE_UINT_LIST)
        ids = g_array_new (FALSE, FALSE, sizeof (guint));
      for (guint i = 0; ok && i < n; i++) {
        ok = parse_number (k, items[i], &v, reason);
        if (ok && ids) {
          guint id = (guint) v;

          g_array_append_val (ids, id);
        } else if (ok && i < MAX_OFFSETS) {
          vals[i] = v;
        }
      }
      if (ok && base && k->type == TYPE_OFFSETS) {
        RdsInferConfig *cfg = base;

        memset (cfg->offsets, 0, sizeof (cfg->offsets));
        for (guint i = 0; i < n; i++)
          cfg->offsets[i] = (gfloat) vals[i];
        cfg->n_offsets = n;
      } else if (ok && base && k->type == TYPE_DIMS) {
        RdsInferConfig *cfg = base;

        for (guint i = 0; i < 3; i++)
          cfg->infer_dims[i] = (guint) vals[i];
        cfg->has_infer_dims = TRUE;
      } else if (ok && base && ids) {
        GArray **field = &FIELD (GArray *, base, k);

        g_array_unref (*field);
        *field = g_steal_pointer (&ids);
      }
      if (ids)
        g_array_unref (ids);
      break;
    }
    case TYPE_STRING_LIST:
      if (!(ok = (items = split_list (s, reason)) != NULL) || !base)
        break;
      g_strfreev (FIELD (gchar **, base, k));
      FIELD (gchar **, base, k) = items[0] ? g_steal_pointer (&items) : NULL;
      break;
  }
  g_strfreev (items);
  g_free (s);
  *num = v;
  return ok;
}

static gboolean
accepted (const KeyInfo * k, gdouble v)
{
  if (k->n_accept == 0)
    return TRUE;
  for (guint i = 0; i < k->n_accept; i++)
    if (v == k->accept[i])
      return TRUE;
  return FALSE;
}

static const KeyInfo *
find_key (KeyGroup group, const gchar * name)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++)
    if (keys[i].group == group && !strcmp (keys[i].name, name))
      return &keys[i];
  return NULL;
}

/* rds_infer_config_check_table (run by the tests) makes sure every default parses. */
static void
apply_defaults (KeyGroup group, gpointer base)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    gchar *reason = NULL;
    gdouble v;

    if (keys[i].group != group || !keys[i].def)
      continue;
    if (!parse_value (&keys[i], keys[i].def, base, NULL, &v, &reason))
      g_error ("bad default for %s: %s", keys[i].name, reason);
  }
}

/* Parses the keys of @group (a key-file group of kind @kind) into @base: known keys in table order, then a warning
 * for each unknown one. */
static gboolean
parse_group (LoadCtx * ctx, GKeyFile * kf, const gchar * group, KeyGroup kind, gpointer base, GError ** error)
{
  gchar **names;

  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gchar *raw, *reason = NULL;
    GError *err = NULL;
    gdouble v;

    if (k->group != kind || !g_key_file_has_key (kf, group, k->name, NULL))
      continue;
    if (!(raw = g_key_file_get_string (kf, group, k->name, &err))) {
      gchar *text = g_key_file_get_value (kf, group, k->name, NULL);

      key_error (error, RDS_INFER_CONFIG_ERROR_INVALID, ctx->path, group, k->name, text ? text : "?", "%s",
          err->message);
      g_free (text);
      g_error_free (err);
      return FALSE;
    }
    g_strstrip (raw);
    if (k->status == RDS_INFER_KEY_REJECTED && !*raw) {
      g_free (raw);             /* empty: unset, as for strings */
      continue;
    }
    if (k->status == RDS_INFER_KEY_REJECTED) {
      key_error (error, RDS_INFER_CONFIG_ERROR_UNSUPPORTED, ctx->path, group, k->name, raw, "%s", k->note);
      g_free (raw);
      return FALSE;
    }
    if (!parse_value (k, raw, k->status == RDS_INFER_KEY_SUPPORTED ? base : NULL, ctx->cfg->config_dir, &v,
            &reason)) {
      key_error (error, RDS_INFER_CONFIG_ERROR_INVALID, ctx->path, group, k->name, raw, "%s", reason);
      g_free (reason);
      g_free (raw);
      return FALSE;
    }
    if (k->status == RDS_INFER_KEY_NOOP) {
      if (!accepted (k, v)) {
        key_error (error, RDS_INFER_CONFIG_ERROR_UNSUPPORTED, ctx->path, group, k->name, raw, "%s", k->note);
        g_free (raw);
        return FALSE;
      }
      warn_once (ctx, group, k->name, "%s: ignored: %s", raw, k->note);
    }
    g_free (raw);
  }

  names = g_key_file_get_keys (kf, group, NULL, NULL);
  for (guint i = 0; names && names[i]; i++)
    if (!find_key (kind, names[i]))
      warn_once (ctx, group, names[i], "unknown key, ignored");
  g_strfreev (names);
  return TRUE;
}

static guint
input_channels (const RdsInferConfig * cfg)
{
  if (cfg->has_infer_dims)
    return cfg->infer_dims[0];
  return cfg->model_color_format == 2 ? 1 : 3;
}

/* A validate error: "FILE:KEY: VALUE: reason", or "element property KEY: VALUE: reason" for a key in
 * @overridden. */
static gboolean G_GNUC_PRINTF (7, 8)
validate_error (GError ** error, gint code, const RdsInferConfig * cfg, const gchar * const *overridden,
    const gchar * key, const gchar * value, const gchar * fmt, ...)
{
  gchar *reason;
  va_list ap;

  va_start (ap, fmt);
  reason = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  if (overridden && g_strv_contains (overridden, key))
    g_set_error (error, RDS_INFER_CONFIG_ERROR, code, "element property %s: %s: %s", key, value, reason);
  else
    key_error (error, code, cfg->config_path, NULL, key, value, "%s", reason);
  g_free (reason);
  return FALSE;
}

gboolean
rds_infer_config_validate (const RdsInferConfig * cfg, const gchar * const *overridden, GError ** error)
{
  g_return_val_if_fail (cfg != NULL, FALSE);

  /* supported keys of which only some values are supported (rds_infer_config_check_table: all [property] ints) */
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gint v;
    gchar *value;

    if (k->group != GROUP_PROPERTY || k->status != RDS_INFER_KEY_SUPPORTED || k->n_accept == 0
        || k->type != TYPE_INT)
      continue;
    v = FIELD (gint, cfg, k);
    if (accepted (k, v))
      continue;
    value = g_strdup_printf ("%d", v);
    validate_error (error, RDS_INFER_CONFIG_ERROR_UNSUPPORTED, cfg, overridden, k->name, value, "%s", k->note);
    g_free (value);
    return FALSE;
  }

  if (!cfg->onnx_file && !cfg->model_engine_file)
    return validate_error (error, RDS_INFER_CONFIG_ERROR_INVALID, cfg, overridden, "onnx-file", "(unset)",
        "set onnx-file or model-engine-file");
  if (cfg->network_type == 0 && cfg->num_detected_classes == 0)
    return validate_error (error, RDS_INFER_CONFIG_ERROR_INVALID, cfg, overridden, "num-detected-classes",
        "(unset)", "required for a detector (network-type=0)");
  if (cfg->network_type == 0 && !cfg->parse_bbox_func_name)
    return validate_error (error, RDS_INFER_CONFIG_ERROR_UNSUPPORTED, cfg, overridden, "parse-bbox-func-name",
        "(unset)",
        "nvinfer's internal resnet parser is not available; set it to RdsInferParseYolo (the built-in YOLO "
        "parser) or to a parser in custom-lib-path");
  if (cfg->n_offsets > 0 && cfg->n_offsets != input_channels (cfg)) {
    GString *value = g_string_new (NULL);

    for (guint i = 0; i < cfg->n_offsets; i++) {
      gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

      g_string_append_printf (value, "%s%s", i ? ";" : "", g_ascii_formatd (buf, sizeof (buf), "%g",
              cfg->offsets[i]));
    }
    validate_error (error, RDS_INFER_CONFIG_ERROR_INVALID, cfg, overridden, "offsets", value->str,
        "%u values for a %u-channel input (infer-dims or model-color-format)", cfg->n_offsets,
        input_channels (cfg));
    g_string_free (value, TRUE);
    return FALSE;
  }
  return TRUE;
}

/* Label file: one label per line, or all of them on one ';'-separated line. Whitespace around labels, a CR before
 * the newline and trailing blank lines are ignored. */
static gboolean
load_labels (LoadCtx * ctx, GError ** error)
{
  RdsInferConfig *cfg = ctx->cfg;
  guint n = cfg->num_detected_classes, n_labels;
  gchar *text = NULL, *start, **lines, **labels;
  GError *err = NULL;

  cfg->labels = g_new0 (gchar *, n + 1);
  if (!cfg->labelfile_path || n == 0) {
    for (guint i = 0; i < n; i++)
      cfg->labels[i] = g_strdup ("");
    return TRUE;
  }
  if (!g_file_get_contents (cfg->labelfile_path, &text, NULL, &err)) {
    key_error (error, RDS_INFER_CONFIG_ERROR_FILE, ctx->path, NULL, "labelfile-path", cfg->labelfile_path, "%s",
        err->message);
    g_error_free (err);
    return FALSE;
  }
  start = g_str_has_prefix (text, "\xEF\xBB\xBF") ? text + 3 : text;        /* UTF-8 byte order mark */
  lines = g_strsplit (start, "\n", -1);
  n_labels = g_strv_length (lines);
  /* labels end up in metadata, messages and JSON */
  for (guint i = 0; i < n_labels; i++) {
    if (!g_utf8_validate (lines[i], -1, NULL)) {
      gchar *where = g_strdup_printf ("%s:%u", cfg->labelfile_path, i + 1);

      key_error (error, RDS_INFER_CONFIG_ERROR_INVALID, ctx->path, NULL, "labelfile-path", where,
          "not valid UTF-8");
      g_free (where);
      g_strfreev (lines);
      g_free (text);
      return FALSE;
    }
  }
  for (guint i = 0; i < n_labels; i++)
    g_strstrip (lines[i]);
  while (n_labels > 0 && !*lines[n_labels - 1]) {
    g_free (lines[n_labels - 1]);
    lines[--n_labels] = NULL;
  }
  if (n_labels == 1 && strchr (lines[0], ';')) {
    labels = g_strsplit (lines[0], ";", -1);
    g_strfreev (lines);
    n_labels = g_strv_length (labels);
    for (guint i = 0; i < n_labels; i++)
      g_strstrip (labels[i]);
    if (n_labels > 0 && !*labels[n_labels - 1]) {
      g_free (labels[n_labels - 1]);
      labels[--n_labels] = NULL;
    }
  } else {
    labels = lines;
  }

  for (guint i = 0; i < n; i++)
    cfg->labels[i] = g_strdup (i < n_labels ? labels[i] : "");
  if (n_labels < n)
    warn_once (ctx, NULL, "labelfile-path", "%s: %u labels for %u classes, the rest have empty labels",
        cfg->labelfile_path, n_labels, n);
  else if (n_labels > n)
    warn_once (ctx, NULL, "labelfile-path", "%s: %u labels for %u classes, the extra labels are ignored",
        cfg->labelfile_path, n_labels, n);
  g_strfreev (labels);
  g_free (text);
  return TRUE;
}

static GKeyFile *
read_config (const gchar * path, GError ** error)
{
  GKeyFile *kf;
  GError *err = NULL;

  if (g_str_has_suffix (path, ".yml") || g_str_has_suffix (path, ".yaml")) {
    kf = rds_keyfile_from_yaml (path, &err);
  } else {
    kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, &err))
      g_clear_pointer (&kf, g_key_file_unref);
  }
  if (kf)
    return kf;
  if (err->domain == G_FILE_ERROR)
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_FILE, "%s: can't read the config: %s", path,
        err->message);
  else if (g_str_has_prefix (err->message, path))       /* the YAML reader names the file and line already */
    g_set_error_literal (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_PARSE, err->message);
  else
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_PARSE, "%s: %s", path, err->message);
  g_error_free (err);
  return NULL;
}

static gboolean
parse_class_groups (LoadCtx * ctx, GKeyFile * kf, GError ** error)
{
  RdsInferConfig *cfg = ctx->cfg;
  RdsInferClassAttrs all = { 0 };
  gchar **groups;
  gboolean ok = TRUE;

  apply_defaults (GROUP_CLASS_ATTRS, &all);
  if (g_key_file_has_group (kf, CLASS_ATTRS_ALL) && !parse_group (ctx, kf, CLASS_ATTRS_ALL, GROUP_CLASS_ATTRS,
          &all, error))
    return FALSE;
  cfg->class_attrs = g_new0 (RdsInferClassAttrs, MAX (cfg->num_detected_classes, 1));
  for (guint i = 0; i < cfg->num_detected_classes; i++)
    cfg->class_attrs[i] = all;

  /* each [class-attrs-<id>] starts from [class-attrs-all] and overrides the keys it sets */
  groups = g_key_file_get_groups (kf, NULL);
  for (guint i = 0; ok && groups[i]; i++) {
    const gchar *g = groups[i];
    guint64 id;

    if (!strcmp (g, PROPERTY_GROUP) || !strcmp (g, CLASS_ATTRS_ALL))
      continue;
    if (!g_str_has_prefix (g, CLASS_ATTRS_PREFIX) || !g_ascii_string_to_unsigned (g + strlen (CLASS_ATTRS_PREFIX),
            10, 0, G_MAXUINT, &id, NULL)) {
      gchar *bracketed = g_strdup_printf ("[%s]", g);

      warn_once (ctx, NULL, bracketed, "unknown group, ignored");
      g_free (bracketed);
      continue;
    }
    if (g[strlen (CLASS_ATTRS_PREFIX)] == '0' && g[strlen (CLASS_ATTRS_PREFIX) + 1]) {
      ok = key_error (error, RDS_INFER_CONFIG_ERROR_INVALID, ctx->path, NULL, g, g + strlen (CLASS_ATTRS_PREFIX),
          "write class ids without leading zeros");
      break;
    }
    if (id >= cfg->num_detected_classes) {
      gchar *value = g_strdup_printf ("%" G_GUINT64_FORMAT, id);

      ok = key_error (error, RDS_INFER_CONFIG_ERROR_INVALID, ctx->path, NULL, g, value,
          "no such class: class ids go from 0 to num-detected-classes - 1 (num-detected-classes is %u)",
          cfg->num_detected_classes);
      g_free (value);
      break;
    }
    ok = parse_group (ctx, kf, g, GROUP_CLASS_ATTRS, &cfg->class_attrs[id], error);
  }
  g_strfreev (groups);
  return ok;
}

RdsInferConfig *
rds_infer_config_load (const gchar * path, GError ** error)
{
  RdsInferConfig *cfg;
  GKeyFile *kf;
  LoadCtx ctx;
  gchar *abs_path;
  gboolean ok;

  g_return_val_if_fail (path != NULL, NULL);

  if (!(kf = read_config (path, error)))
    return NULL;

  cfg = g_new0 (RdsInferConfig, 1);
  cfg->config_path = g_strdup (path);
  abs_path = g_canonicalize_filename (path, NULL);
  cfg->config_dir = g_path_get_dirname (abs_path);
  g_free (abs_path);
  cfg->filter_out_class_ids = g_array_new (FALSE, FALSE, sizeof (guint));
  cfg->warnings = g_ptr_array_new_with_free_func (g_free);
  apply_defaults (GROUP_PROPERTY, cfg);
  ctx.path = path;
  ctx.cfg = cfg;
  ctx.warned = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  if (!g_key_file_has_group (kf, PROPERTY_GROUP)) {
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID,
        "%s: no [property] group (it is mandatory)", path);
    ok = FALSE;
  } else {
    ok = parse_group (&ctx, kf, PROPERTY_GROUP, GROUP_PROPERTY, cfg, error)
        && parse_class_groups (&ctx, kf, error)
        && rds_infer_config_validate (cfg, NULL, error)
        && load_labels (&ctx, error);
  }
  g_hash_table_unref (ctx.warned);
  g_key_file_unref (kf);
  if (!ok)
    g_clear_pointer (&cfg, rds_infer_config_free);
  return cfg;
}

void
rds_infer_config_free (RdsInferConfig * cfg)
{
  if (!cfg)
    return;
  g_free (cfg->config_path);
  g_free (cfg->config_dir);
  g_free (cfg->onnx_file);
  g_free (cfg->model_engine_file);
  g_free (cfg->labelfile_path);
  g_free (cfg->custom_lib_path);
  g_free (cfg->parse_bbox_func_name);
  g_strfreev (cfg->output_blob_names);
  if (cfg->filter_out_class_ids)
    g_array_unref (cfg->filter_out_class_ids);
  g_free (cfg->class_attrs);
  g_strfreev (cfg->labels);
  if (cfg->warnings)
    g_ptr_array_unref (cfg->warnings);
  g_free (cfg);
}

static gboolean
is_type_max (const KeyInfo * k)
{
  if (k->type == TYPE_FLOAT || k->type == TYPE_OFFSETS)
    return k->max == G_MAXFLOAT;
  if (k->type == TYPE_UINT || k->type == TYPE_UINT_LIST || k->type == TYPE_DIMS)
    return k->max == G_MAXUINT;
  return k->max == G_MAXINT;
}

void
rds_infer_config_foreach_key (RdsInferConfigKeyFunc func, gpointer user_data)
{
  g_return_if_fail (func != NULL);

  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gchar min[G_ASCII_DTOSTR_BUF_SIZE], max[G_ASCII_DTOSTR_BUF_SIZE], *range = NULL, *accept = NULL;
    RdsInferKeyDesc desc = {
      .group = k->group == GROUP_PROPERTY ? PROPERTY_GROUP : "class-attrs",
      .name = k->name,
      .type = type_names[k->type],
      .status = k->status,
      .default_value = k->def,
      .note = k->note,
    };

    if (is_number_type (k->type)) {
      fmt_num (min, k->min);
      fmt_num (max, k->max);
      if (k->min_exclusive)
        range = g_strdup_printf ("> %s", min);
      else if (is_type_max (k))
        range = g_strdup_printf (">= %s", min);
      else
        range = g_strdup_printf ("%s..%s", min, max);
    }
    if (k->n_accept > 0) {
      GString *str = g_string_new (NULL);

      for (guint a = 0; a < k->n_accept; a++)
        g_string_append_printf (str, "%s%d", a ? ", " : "", k->accept[a]);
      accept = g_string_free (str, FALSE);
    }
    desc.range = range;
    desc.accepted = accept;
    func (&desc, user_data);
    g_free (accept);
    g_free (range);
  }
}

static gboolean G_GNUC_PRINTF (3, 4)
table_error (GError ** error, const KeyInfo * k, const gchar * fmt, ...)
{
  gchar *msg;
  va_list ap;

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID, "key table: %s: %s", k->name, msg);
  g_free (msg);
  return FALSE;
}

gboolean
rds_infer_config_check_table (GError ** error)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];

    for (guint j = 0; j < i; j++)
      if (keys[j].group == k->group && !strcmp (keys[j].name, k->name))
        return table_error (error, k, "listed twice");
    if ((k->status == RDS_INFER_KEY_SUPPORTED) != (k->field > 0))
      return table_error (error, k, "supported keys need a field, and only they have one");
    if (k->status != RDS_INFER_KEY_SUPPORTED && !k->note)
      return table_error (error, k, "no-op and rejected keys need a note");
    if (is_number_type (k->type) && (k->max == 0 || k->min > k->max))
      return table_error (error, k, "a number needs min <= max and a max");
    if (!is_number_type (k->type) && (k->min != 0 || k->max != 0 || k->min_exclusive))
      return table_error (error, k, "only numbers have a range");
    if (k->n_accept > G_N_ELEMENTS (k->accept))
      return table_error (error, k, "n_accept is larger than accept[]");
    if (k->n_accept > 0 && !k->note)
      return table_error (error, k, "accepted values need a note");
    if (k->n_accept > 0 && k->status == RDS_INFER_KEY_SUPPORTED && (k->type != TYPE_INT
            || k->group != GROUP_PROPERTY))
      return table_error (error, k, "accepted values of supported keys are for [property] ints only");
    if (k->n_accept > 0 && k->status == RDS_INFER_KEY_NOOP && k->type != TYPE_INT && k->type != TYPE_BOOL)
      return table_error (error, k, "accepted values of no-op keys are for ints and bools only");
    if (k->n_accept > 0 && k->status == RDS_INFER_KEY_REJECTED)
      return table_error (error, k, "rejected keys have no accepted values");
    if (k->def) {
      union
      {
        RdsInferConfig cfg;
        RdsInferClassAttrs attrs;
      } scratch;
      gchar *reason = NULL;
      gdouble v;
      gboolean ok;

      memset (&scratch, 0, sizeof (scratch));
      if (k->type == TYPE_UINT_LIST || k->type == TYPE_STRING_LIST || k->type == TYPE_STRING
          || k->type == TYPE_PATH)
        return table_error (error, k, "defaults are for scalars and number lists only");
      ok = parse_value (k, k->def, k->field > 0 ? &scratch : NULL, NULL, &v, &reason);
      if (!ok || !accepted (k, v)) {
        table_error (error, k, "default \"%s\": %s", k->def, reason ? reason : "not an accepted value");
        g_free (reason);
        return FALSE;
      }
    }
  }
  return TRUE;
}
