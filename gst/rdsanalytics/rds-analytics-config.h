/* Redstream: the rdsanalytics configuration, parsed from an nvdsanalytics-style GLib key file (config-file) into a
 * typed struct (plugin-internal, not installed). Every key has one entry in the table in rds-analytics-config.c with
 * its groups, type, range, default, status and note.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ANALYTICS_CONFIG_H
#define RDS_ANALYTICS_CONFIG_H

#include "rds-analytics-types.h"

G_BEGIN_DECLS

#define RDS_ANALYTICS_CONFIG_ERROR (rds_analytics_config_error_quark ())
G_GNUC_INTERNAL GQuark rds_analytics_config_error_quark (void);

/* Error messages name the file, the group and the key: "FILE:[GROUP] KEY: VALUE: reason", or "FILE:[GROUP]: reason"
 * for a group as a whole. */
typedef enum
{
  RDS_ANALYTICS_CONFIG_ERROR_FILE,      /* the config file can't be read */
  RDS_ANALYTICS_CONFIG_ERROR_PARSE,     /* key-file syntax error (the message has the line) */
  RDS_ANALYTICS_CONFIG_ERROR_INVALID,   /* a missing, empty, wrongly typed or out-of-range value, a bad rule */
} RdsAnalyticsConfigError;

typedef struct
{
  /* [property] */
  gboolean enable;
  guint config_width, config_height;
  guint osd_mode;                       /* 0..2 */
  guint display_font_size;              /* 0 in the file reads as 12 */
  guint obj_cnt_win_in_ms;
  gboolean display_obj_cnt;
  /* RdsAnalyticsStream *, one per pad_index that has a group, sorted by pad_index */
  GPtrArray *streams;
} RdsAnalyticsConfig;

/* Loads and validates the key file @path. Skipped groups and unknown keys in feature groups are logged with
 * g_message (domain "rdsanalytics"). */
G_GNUC_INTERNAL RdsAnalyticsConfig *rds_analytics_config_load (const gchar * path, GError ** error);
G_GNUC_INTERNAL void rds_analytics_config_free (RdsAnalyticsConfig * cfg);
/* The groups of @pad_index; NULL when it has none. */
G_GNUC_INTERNAL const RdsAnalyticsStream *rds_analytics_config_get_stream (const RdsAnalyticsConfig * cfg,
    guint pad_index);
/* "roi-filtering", "overcrowding", "line-crossing", "direction-detection" */
G_GNUC_INTERNAL const gchar *rds_analytics_feature_name (RdsAnalyticsFeature feature);

typedef enum
{
  RDS_ANALYTICS_KEY_SUPPORTED,
  RDS_ANALYTICS_KEY_NOOP,               /* accepted and ignored */
} RdsAnalyticsKeyStatus;

/* One key of the table, as text (for docs/elements/rdsanalytics-config.md). */
typedef struct
{
  const gchar *groups;          /* "property", or the features it is valid in: "roi-filtering, overcrowding" */
  const gchar *name;            /* a label key is written "roi-<label>" */
  const gchar *type;            /* "bool", "uint", "int list", "string", "polygon", "line", "vector" */
  RdsAnalyticsKeyStatus status;
  gboolean required;
  const gchar *default_value;   /* as it would be written in the file; NULL: none */
  gboolean default_ours;        /* DeepStream 8.0's default is unknown: the default is ours */
  const gchar *range;           /* "MIN..MAX" or ">= MIN" (of each element for lists); NULL: not a number */
  const gchar *note;            /* NULL: none */
} RdsAnalyticsKeyDesc;

/* @key and its strings are valid during the call only. */
typedef void (*RdsAnalyticsConfigKeyFunc) (const RdsAnalyticsKeyDesc * key, gpointer user_data);
/* Calls @func for every key in the table, in table order. */
G_GNUC_INTERNAL void rds_analytics_config_foreach_key (RdsAnalyticsConfigKeyFunc func, gpointer user_data);
/* Checks the key table itself (keys unique per group kind, fields, ranges, defaults, notes); FALSE with the first
 * mistake. */
G_GNUC_INTERNAL gboolean rds_analytics_config_check_table (GError ** error);

G_END_DECLS

#endif /* RDS_ANALYTICS_CONFIG_H */
