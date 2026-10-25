/* Redstream: the rdstracker configuration, parsed from a NvMultiObjectTracker-style YAML config (ll-config-file) into
 * a typed struct (plugin-internal, not installed). Section and key names, types, ranges and defaults follow
 * DeepStream's public tracker documentation; every key has one entry in the table in rds-tracker-config.c with its
 * section, type, range, default, status and the tracker types that use it.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TRACKER_CONFIG_H
#define RDS_TRACKER_CONFIG_H

#include <glib.h>

G_BEGIN_DECLS

#define RDS_TRACKER_CONFIG_ERROR (rds_tracker_config_error_quark ())
G_GNUC_INTERNAL GQuark rds_tracker_config_error_quark (void);

/* Error messages name the file, the key and its value: "FILE:SECTION.KEY: VALUE: reason". */
typedef enum
{
  RDS_TRACKER_CONFIG_ERROR_FILE,        /* the config file can't be read */
  RDS_TRACKER_CONFIG_ERROR_PARSE,       /* YAML syntax error (the message has the line) */
  RDS_TRACKER_CONFIG_ERROR_INVALID,     /* wrong type, out of range, conflicting keys */
} RdsTrackerConfigError;

/* The tracker a config runs as: IOU when stateEstimatorType and associationMatcherType are both 0, NvSORT
 * otherwise (NvDeepSORT and NvDCF configs run as NvSORT, without their visual tracker and Re-ID). */
typedef enum
{
  RDS_TRACKER_TYPE_IOU,
  RDS_TRACKER_TYPE_NVSORT,
} RdsTrackerType;

typedef struct
{
  gchar *config_path;                           /* the file as given to rds_tracker_config_load; NULL: defaults */
  RdsTrackerType tracker_type;
  /* BaseConfig */
  gfloat min_detector_confidence;
  gboolean check_class_match;                   /* from BaseConfig or DataAssociator */
  /* TargetManagement */
  guint max_targets_per_stream, probation_age, max_shadow_tracking_age, early_termination_age;
  gfloat min_iou_diff4new_target;
  /* TrajectoryManagement */
  gboolean use_unique_id;
  /* DataAssociator */
  gint association_matcher_type;                /* 0 greedy, 1 cascaded */
  gfloat min_matching_score4overall, min_matching_score4size_similarity, min_matching_score4iou;
  gfloat matching_score_weight4size_similarity, matching_score_weight4iou;
  gfloat tentative_detector_confidence, min_matching_score4tentative_iou;
  gboolean use_prediction4assoc;
  /* StateEstimator */
  gint state_estimator_type;                    /* 0 none, 1 simple, 2 regular (3 is read as 2) */
  gfloat process_noise_var4loc, process_noise_var4size, process_noise_var4vel;
  gfloat measurement_noise_var4detector, measurement_noise_var4tracker;
  gfloat noise_weight_var4loc, noise_weight_var4vel;       /* > 0: height-proportional noise, else unset */
  gboolean use_aspect_ratio;
  /* Warnings found while loading (unknown keys and sections, ignored sections, no-op keys set to other than their
   * default), one string each and at most one per key. The element posts them. */
  GPtrArray *warnings;
} RdsTrackerConfig;

/* Loads the YAML config @path (the OpenCV-style "%YAML:1.0" first line is accepted) and validates it. A NULL @path
 * gives every default. */
G_GNUC_INTERNAL RdsTrackerConfig *rds_tracker_config_load (const gchar * path, GError ** error);
G_GNUC_INTERNAL void rds_tracker_config_free (RdsTrackerConfig * cfg);
G_GNUC_INTERNAL const gchar *rds_tracker_type_name (RdsTrackerType type);

typedef enum
{
  RDS_TRACKER_KEY_SUPPORTED,
  RDS_TRACKER_KEY_NOOP,                 /* accepted and ignored, with a warning when set to other than its default */
  RDS_TRACKER_KEY_IGNORED,              /* used only by a module rdstracker doesn't have: type-checked, ignored */
  RDS_TRACKER_KEY_DEPRECATED,           /* an old name: accepted and ignored, with a warning whenever set */
} RdsTrackerKeyStatus;

/* One key of the table, as text (for the compat matrix). */
typedef struct
{
  const gchar *section;         /* "BaseConfig", ...; the model inference keys are "ReID, PoseEstimator" */
  const gchar *name;
  const gchar *type;            /* "bool", "int", "uint", "float", "string", "int list", "float list", "string list" */
  RdsTrackerKeyStatus status;
  const gchar *default_value;   /* as it would be written in the file; NULL: none (unset) */
  gboolean default_ours;        /* the documentation gives no default: the default is ours */
  const gchar *range;           /* "MIN..MAX", ">= MIN" or "any" (of each element for lists); NULL: not a number */
  const gchar *trackers;        /* the tracker types that use the key, e.g. "IOU, NvSORT, NvDeepSORT, NvDCF" */
  const gchar *note;            /* why a key is not supported, or how it is read; NULL: none */
} RdsTrackerKeyDesc;

/* @key and its strings are valid during the call only. */
typedef void (*RdsTrackerConfigKeyFunc) (const RdsTrackerKeyDesc * key, gpointer user_data);
/* Calls @func for every key in the table, in table order. */
G_GNUC_INTERNAL void rds_tracker_config_foreach_key (RdsTrackerConfigKeyFunc func, gpointer user_data);
/* Checks the key table itself (keys unique per section, fields, ranges, defaults, notes); FALSE with the first
 * mistake. */
G_GNUC_INTERNAL gboolean rds_tracker_config_check_table (GError ** error);

G_END_DECLS

#endif /* RDS_TRACKER_CONFIG_H */
