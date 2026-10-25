/* Redstream: the rdsinfer configuration, parsed from an nvinfer-style config file (GLib key file or YAML) into a
 * typed struct (plugin-internal, not installed). Key names, types and groups follow DeepStream's public nvinfer
 * documentation; every key has one entry in the table in rds-infer-config.c with its type, range, default and
 * status (supported, accepted as a no-op with a warning, or rejected with an error).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_CONFIG_H
#define RDS_INFER_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

/* Bounds of the keys the element also has as properties (the property ranges use them). */
#define RDS_INFER_MAX_BATCH_SIZE 1024   /* nvinfer's documented maximum batch size */
#define RDS_INFER_MAX_INTERVAL 32       /* the documented range of the interval property */

#define RDS_INFER_CONFIG_ERROR (rds_infer_config_error_quark ())
G_GNUC_INTERNAL GQuark rds_infer_config_error_quark (void);

/* Error messages name the file, the key and its value: "FILE:KEY: VALUE: reason" (KEY is GROUP:KEY outside
 * [property]). */
typedef enum
{
  RDS_INFER_CONFIG_ERROR_FILE,          /* the config or label file can't be read */
  RDS_INFER_CONFIG_ERROR_PARSE,         /* syntax error (the message has the line where the parser knows it) */
  RDS_INFER_CONFIG_ERROR_INVALID,       /* wrong type, out of range, a missing mandatory key or group */
  RDS_INFER_CONFIG_ERROR_UNSUPPORTED,   /* a valid nvinfer key or value that rdsinfer does not support */
} RdsInferConfigError;

/* Detection parameters of one class: [class-attrs-all], overridden per key by [class-attrs-<id>]. */
typedef struct
{
  gfloat pre_cluster_threshold, post_cluster_threshold, nms_iou_threshold;
  gint topk;                                    /* -1: off */
  guint roi_top_offset, roi_bottom_offset, detected_min_w, detected_min_h, detected_max_w, detected_max_h;
} RdsInferClassAttrs;

typedef struct
{
  gchar *config_path;                           /* the file as given to rds_infer_config_load */
  /* Absolute, but symlinks are not resolved: for a symlinked config file it is the link's directory. Relative paths
   * in the file are resolved against it and normalised (g_canonicalize_filename). */
  gchar *config_dir;
  gchar *onnx_file, *model_engine_file, *labelfile_path, *custom_lib_path, *parse_bbox_func_name;
  guint batch_size, interval, gie_unique_id, num_detected_classes, gpu_id;
  gint network_mode, network_type, process_mode, cluster_mode, model_color_format, network_input_order,
      scaling_filter;
  gfloat net_scale_factor, offsets[4];
  guint n_offsets;                              /* 0: no offsets (all zero) */
  guint infer_dims[3];                          /* C, H, W */
  gboolean has_infer_dims;
  gboolean maintain_aspect_ratio, symmetric_padding, output_tensor_meta;
  gchar **output_blob_names;                    /* NULL-terminated, NULL when unset */
  GArray *filter_out_class_ids;                 /* guint, never NULL */
  RdsInferClassAttrs *class_attrs;              /* num_detected_classes entries, after inheritance */
  gchar **labels;                               /* NULL-terminated, num_detected_classes entries ("" if none) */
  /* Warnings found while loading (no-op and unknown keys, label count), one string each and at most one per key.
   * The element posts them, rds-compile prints them. */
  GPtrArray *warnings;
} RdsInferConfig;

/* Loads @path (YAML if it ends in .yml or .yaml, a key file otherwise), validates it and loads the labels. */
G_GNUC_INTERNAL RdsInferConfig *rds_infer_config_load (const gchar * path, GError ** error);

/* The supported-value and cross-key checks of rds_infer_config_load, for re-running after element properties
 * override fields. Overrides follow these rules:
 * - strings: g_free the old value, store a g_strdup'ed new one (NULL for unset). Paths from properties are used as
 *   given, not resolved against config_dir.
 * - filter_out_class_ids: replace its contents; it stays non-NULL.
 * - num_detected_classes, class_attrs and labels must not change (they were sized and loaded together).
 * - Ranges are checked only while parsing the file, not here: the element's GParamSpecs must carry the same bounds
 *   (batch-size 1..1024, interval 0..32, ...).
 * @overridden lists the config key names (not property names) whose values came from element properties, NULL
 * for none. Errors about those keys read "element property KEY: VALUE: reason" instead of "FILE:KEY: ...". */
G_GNUC_INTERNAL gboolean rds_infer_config_validate (const RdsInferConfig * cfg, const gchar * const *overridden,
    GError ** error);
G_GNUC_INTERNAL void rds_infer_config_free (RdsInferConfig * cfg);

typedef enum
{
  RDS_INFER_KEY_SUPPORTED,
  RDS_INFER_KEY_NOOP,                   /* accepted and ignored, with a warning */
  RDS_INFER_KEY_REJECTED,               /* an error whenever it is set */
} RdsInferKeyStatus;

/* One key of the table, as text (for the compat matrix). */
typedef struct
{
  const gchar *group;           /* "property" or "class-attrs" (both [class-attrs-all] and [class-attrs-<id>]) */
  const gchar *name;
  const gchar *type;            /* "bool", "int", "uint", "float", "string", "path", "float list", "dims",
                                 * "uint list", "int list", "string list" */
  RdsInferKeyStatus status;
  const gchar *default_value;   /* as it would be written in the file; NULL: none (unset or zero) */
  const gchar *range;           /* "MIN..MAX", ">= MIN" or "> MIN" (of each element for lists); NULL: not a number */
  const gchar *accepted;        /* the supported values, e.g. "2, 4", when not every value in range is; else NULL */
  const gchar *note;            /* why a key is a no-op or rejected, or why other values are rejected; NULL: none */
} RdsInferKeyDesc;

/* @key and its strings are valid during the call only. */
typedef void (*RdsInferConfigKeyFunc) (const RdsInferKeyDesc * key, gpointer user_data);
/* Calls @func for every key in the table, in table order. */
G_GNUC_INTERNAL void rds_infer_config_foreach_key (RdsInferConfigKeyFunc func, gpointer user_data);
/* Checks the key table itself (fields, ranges, accepted values, defaults, notes); FALSE with the first mistake. */
G_GNUC_INTERNAL gboolean rds_infer_config_check_table (GError ** error);

G_END_DECLS

#endif /* RDS_INFER_CONFIG_H */
