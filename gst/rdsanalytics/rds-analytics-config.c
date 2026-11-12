/* Redstream: the rdsanalytics config parser. One table lists every key nvdsanalytics reads with its groups, type,
 * range, default, status and the struct field it fills; it drives parsing and docs/elements/rdsanalytics-config.md.
 * The file is a GLib key file: [property], then [<feature>-stream-<n>] groups whose labelled rule keys (roi-<label>,
 * line-crossing-<label>, direction-<label>) are kept in file order.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-config.h"
#include "rds-config-parse.h"

#include <stdarg.h>
#include <string.h>

G_DEFINE_QUARK (rds-analytics-config-error-quark, rds_analytics_config_error)

/* Group kinds a key is valid in. */
enum
{
  GRP_PROPERTY = 1 << 0,
  GRP_ROI = 1 << 1,
  GRP_OC = 1 << 2,
  GRP_LC = 1 << 3,
  GRP_DIR = 1 << 4,
};
#define GRP_FEATURES (GRP_ROI | GRP_OC | GRP_LC | GRP_DIR)

static const struct
{
  const gchar *name;
  guint kind;
  const gchar *rule_key;        /* its rules' key prefix */
} features[] = {
  [RDS_ANALYTICS_ROI_FILTERING] = {"roi-filtering", GRP_ROI, "roi-"},
  [RDS_ANALYTICS_OVERCROWDING] = {"overcrowding", GRP_OC, "roi-"},
  [RDS_ANALYTICS_LINE_CROSSING] = {"line-crossing", GRP_LC, "line-crossing-"},
  [RDS_ANALYTICS_DIRECTION] = {"direction-detection", GRP_DIR, "direction-"},
};

typedef enum
{
  TYPE_BOOL,                    /* 0/1 or true/false */
  TYPE_UINT,
  TYPE_INT_LIST,                /* ';'-separated */
  TYPE_MODE,                    /* strict, balanced, loose */
  TYPE_STRING,                  /* any; not stored */
  /* the labelled rules: ';'-separated coordinates, integers >= 0 */
  TYPE_POLYGON,                 /* x;y pairs, >= 3 points */
  TYPE_LINE,                    /* 8 values */
  TYPE_VECTOR,                  /* 4 values */
} KeyType;

static const gchar *const type_names[] = {
  [TYPE_BOOL] = "bool",[TYPE_UINT] = "uint",[TYPE_INT_LIST] = "int list",[TYPE_MODE] = "string",
  [TYPE_STRING] = "string",[TYPE_POLYGON] = "polygon",[TYPE_LINE] = "line",[TYPE_VECTOR] = "vector",
};

typedef struct
{
  const gchar *name;            /* a rule key: its prefix followed by "<label>" */
  guint groups;
  KeyType type;
  RdsAnalyticsKeyStatus status;
  gboolean required;
  gdouble min, max;             /* numbers, and the elements of lists and rules */
  const gchar *def;             /* as written in the file; NULL: none */
  gboolean def_ours;            /* DeepStream 8.0's default is unknown */
  gssize field;                 /* offset + 1 into RdsAnalyticsConfig ([property]) or RdsAnalyticsGroup; 0: none */
  const gchar *note;
} KeyInfo;

#define P(f) (G_STRUCT_OFFSET (RdsAnalyticsConfig, f) + 1)
#define GF(f) (G_STRUCT_OFFSET (RdsAnalyticsGroup, f) + 1)
#define FIELD(type, base, k) G_STRUCT_MEMBER (type, base, (k)->field - 1)
#define SUP RDS_ANALYTICS_KEY_SUPPORTED
#define NOOP RDS_ANALYTICS_KEY_NOOP
#define COORD .min = 0,.max = G_MAXINT

/* Ranges and defaults are DeepStream 8.0 behaviour (observed; the docs give no defaults) unless marked ours. */
static const KeyInfo keys[] = {
  /* [property] */
  {.name = "enable",.groups = GRP_PROPERTY,.type = TYPE_BOOL,.status = SUP,.required = TRUE,.field = P (enable),
      .note = "0: pass-through, no meta"},
  {.name = "config-width",.groups = GRP_PROPERTY,.type = TYPE_UINT,.status = SUP,.required = TRUE,.min = 1,
        .max = G_MAXINT,.field = P (config_width),
      .note = "the frame size the rule coordinates are in; they are scaled to the mux output"},
  {.name = "config-height",.groups = GRP_PROPERTY,.type = TYPE_UINT,.status = SUP,.required = TRUE,.min = 1,
      .max = G_MAXINT,.field = P (config_height),.note = "as config-width"},
  {.name = "osd-mode",.groups = GRP_PROPERTY,.type = TYPE_UINT,.status = SUP,.min = 0,.max = 2,.def = "2",
      .field = P (osd_mode),.note = "0: no overlay, 1: the rules and their labels, 2: also the counts and statuses"},
  /* ours: no upper bound */
  {.name = "display-font-size",.groups = GRP_PROPERTY,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,
      .def = "12",.field = P (display_font_size),.note = "0 reads as 12"},
  {.name = "obj-cnt-win-in-ms",.groups = GRP_PROPERTY,.type = TYPE_UINT,.status = SUP,.min = 1,.max = 1e9,
        .def = "50",.def_ours = TRUE,.field = P (obj_cnt_win_in_ms),
      .note = "undocumented: the PTS window of objCnt (the per-class count's mode over the window); the default is "
        "ours, within the 33-100 ms measured on DeepStream 8.0"},
  {.name = "display-obj-cnt",.groups = GRP_PROPERTY,.type = TYPE_BOOL,.status = SUP,.def = "0",
      .field = P (display_obj_cnt),.note = "undocumented: 1 draws the per-class object counts, in every osd-mode"},

  /* the feature groups */
  /* ours: 8.0 configs always set it */
  {.name = "enable",.groups = GRP_FEATURES,.type = TYPE_BOOL,.status = SUP,.def = "1",.def_ours = TRUE,
      .field = GF (enable),.note = "0: the feature is off for the stream, and its labels are absent from the metas"},
  {.name = "class-id",.groups = GRP_FEATURES,.type = TYPE_INT_LIST,.status = SUP,.min = -1,.max = G_MAXINT,
        .field = GF (class_ids),
      .note = "the classes the group's rules apply to; unset, empty or a list holding -1: every class"},
  {.name = "inverse-roi",.groups = GRP_ROI,.type = TYPE_BOOL,.status = SUP,.def = "0",.field = GF (inverse_roi),
      .note = "1: the labels go on the objects outside each ROI, and those are counted"},
  {.name = "object-threshold",.groups = GRP_OC,.type = TYPE_UINT,.status = SUP,.min = 0,.max = G_MAXINT,.def = "0",
        .field = GF (object_threshold),
      .note = "overcrowded when the ROI's count is at least this and above 0"},
  {.name = "time-threshold",.groups = GRP_OC,.type = TYPE_UINT,.status = NOOP,.min = 0,.max = G_MAXINT,.def = "0",
      .field = GF (time_threshold),.note = "undocumented; accepted with no effect, as in DeepStream 8.0"},
  {.name = "mode",.groups = GRP_LC,.type = TYPE_MODE,.status = SUP,.def = "loose",.field = GF (mode),
      .note = "strict, balanced or loose; any other value reads as loose"},
  {.name = "mode",.groups = GRP_DIR,.type = TYPE_STRING,.status = NOOP,
      .note = "accepted and ignored, as in DeepStream 8.0"},
  {.name = "extended",.groups = GRP_LC,.type = TYPE_BOOL,.status = SUP,.def = "0",.field = GF (extended),
      .note = "0: the crossing must hit the segment, 1: the infinite line"},
  {.name = "roi-<label>",.groups = GRP_ROI | GRP_OC,.type = TYPE_POLYGON,.status = SUP, COORD,
      .note = "x1;y1;x2;y2;... in order around the polygon, at least 3 points; integers >= 0. A label used by both "
      "the roi-filtering and the overcrowding group of a stream is an error"},
  {.name = "line-crossing-<label>",.groups = GRP_LC,.type = TYPE_LINE,.status = SUP, COORD,
      .note = "8 integers >= 0: the direction vector (x1;y1;x2;y2), then the line's two ends"},
  {.name = "direction-<label>",.groups = GRP_DIR,.type = TYPE_VECTOR,.status = SUP, COORD,
      .note = "4 integers >= 0: the vector from (x1;y1) to (x2;y2)"},
};

static gboolean
is_rule_type (KeyType type)
{
  return type == TYPE_POLYGON || type == TYPE_LINE || type == TYPE_VECTOR;
}

static gboolean
is_number_type (KeyType type)
{
  return type == TYPE_UINT || type == TYPE_INT_LIST || is_rule_type (type);
}

/* A rule key's prefix length ("roi-<label>" -> 4); 0 for other keys. */
static gsize
rule_prefix_len (const KeyInfo * k)
{
  const gchar *p = strstr (k->name, "<label>");

  return p ? (gsize) (p - k->name) : 0;
}

static gboolean G_GNUC_PRINTF (6, 7)
key_error (GError ** error, const gchar * path, const gchar * group, const gchar * key, const gchar * value,
    const gchar * fmt, ...)
{
  gchar *reason;
  va_list ap;

  va_start (ap, fmt);
  reason = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_ANALYTICS_CONFIG_ERROR, RDS_ANALYTICS_CONFIG_ERROR_INVALID, "%s:[%s] %s: %s: %s", path,
      group, key, value, reason);
  g_free (reason);
  return FALSE;
}

static gboolean G_GNUC_PRINTF (4, 5)
group_error (GError ** error, const gchar * path, const gchar * group, const gchar * fmt, ...)
{
  gchar *reason;
  va_list ap;

  va_start (ap, fmt);
  reason = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_ANALYTICS_CONFIG_ERROR, RDS_ANALYTICS_CONFIG_ERROR_INVALID, "%s:[%s]: %s", path, group,
      reason);
  g_free (reason);
  return FALSE;
}

/* Parses the ';'-separated integers of @s (a trailing ';' is accepted, as GLib's list readers do) into @out, each
 * within @k's range. On failure @reason says which item and why. */
static gboolean
parse_ints (const KeyInfo * k, const gchar * s, GArray * out, gchar ** reason)
{
  gchar **items = g_strsplit (s, ";", -1);
  guint n = g_strv_length (items);
  gboolean ok = TRUE;

  /* "a;b;" is "a;b", and ";" an empty list */
  if (n > 1 && !*g_strstrip (items[n - 1]))
    n--;
  if (n == 1 && !*g_strstrip (items[0]))
    n = 0;
  for (guint i = 0; ok && i < n; i++) {
    gchar *item = g_strstrip (items[i]), *why = NULL;
    gdouble v;

    if (!*item) {
      *reason = g_strdup_printf ("item %u is empty", i + 1);
      ok = FALSE;
    } else if (!(ok = rds_config_parse_number (item, TRUE, k->min, FALSE, k->max, &v, &why))) {
      *reason = g_strdup_printf ("%s: %s", item, why);
      g_free (why);
    } else {
      gint iv = (gint) v;

      g_array_append_val (out, iv);
    }
  }
  g_strfreev (items);
  return ok;
}

/* Parses @raw (stripped, not empty) as @k's scalar or list type; with @base, stores it into k's field. On failure
 * @reason says why. */
static gboolean
parse_value (const KeyInfo * k, const gchar * raw, gpointer base, gchar ** reason)
{
  switch (k->type) {
    case TYPE_BOOL:{
      gboolean b = FALSE;

      if (!rds_config_parse_bool (raw, &b)) {
        *reason = g_strdup ("not a boolean (use 0/1 or true/false)");
        return FALSE;
      }
      if (base)
        FIELD (gboolean, base, k) = b;
      return TRUE;
    }
    case TYPE_UINT:{
      gdouble v;

      if (!rds_config_parse_number (raw, TRUE, k->min, FALSE, k->max, &v, reason))
        return FALSE;
      if (base)
        FIELD (guint, base, k) = (guint) v;
      return TRUE;
    }
    case TYPE_INT_LIST:{
      GArray *ids = g_array_new (FALSE, FALSE, sizeof (gint));
      gboolean all = FALSE;

      if (!parse_ints (k, raw, ids, reason)) {
        g_array_unref (ids);
        return FALSE;
      }
      /* class-id: -1 anywhere means every class (DeepStream 8.0 behaviour) */
      for (guint i = 0; i < ids->len; i++)
        all |= g_array_index (ids, gint, i) == -1;
      if (all || !ids->len || !base) {
        g_array_unref (ids);
        ids = NULL;
      }
      if (base) {
        g_clear_pointer (&FIELD (GArray *, base, k), g_array_unref);
        FIELD (GArray *, base, k) = ids;
      }
      return TRUE;
    }
    case TYPE_MODE:
      /* an unknown value is loose, silently (DeepStream 8.0 behaviour) */
      if (base)
        FIELD (RdsAnalyticsMode, base, k) = !strcmp (raw, "strict") ? RDS_ANALYTICS_MODE_STRICT
            : !strcmp (raw, "balanced") ? RDS_ANALYTICS_MODE_BALANCED : RDS_ANALYTICS_MODE_LOOSE;
      return TRUE;
    case TYPE_STRING:
      return TRUE;
    case TYPE_POLYGON:
    case TYPE_LINE:
    case TYPE_VECTOR:
      break;
  }
  g_assert_not_reached ();
  return FALSE;
}

/* Parses a rule value (stripped, not empty) into a new rule. Ours: the counts are checked (8.0 reads short lists
 * as garbage or zeros, and accepts ROIs of 1 or 2 points that never match). */
static RdsAnalyticsRule *
parse_rule (const KeyInfo * k, const gchar * label, const gchar * raw, gchar ** reason)
{
  GArray *v = g_array_new (FALSE, FALSE, sizeof (gint));
  RdsAnalyticsRule *rule = NULL;

  if (!parse_ints (k, raw, v, reason))
    goto out;
  if (k->type == TYPE_POLYGON && v->len % 2) {
    *reason = g_strdup_printf ("%u values: an odd number of values (x;y pairs)", v->len);
    goto out;
  }
  if (k->type == TYPE_POLYGON && v->len < 6) {
    *reason = g_strdup_printf ("%u points: a polygon needs at least 3 points", v->len / 2);
    goto out;
  }
  if (k->type == TYPE_LINE && v->len != 8) {
    *reason = g_strdup_printf ("%u values: a line crossing needs 8 values (the direction, then the line)", v->len);
    goto out;
  }
  if (k->type == TYPE_VECTOR && v->len != 4) {
    *reason = g_strdup_printf ("%u values: a direction needs 4 values (x1;y1;x2;y2)", v->len);
    goto out;
  }
  rule = g_new0 (RdsAnalyticsRule, 1);
  rule->label = g_strdup (label);
  rule->n_points = v->len / 2;
  rule->points = g_new (RdsAnalyticsPoint, rule->n_points);
  for (guint i = 0; i < rule->n_points; i++) {
    rule->points[i].x = g_array_index (v, gint, 2 * i);
    rule->points[i].y = g_array_index (v, gint, 2 * i + 1);
  }
out:
  g_array_unref (v);
  return rule;
}

static void
rule_free (RdsAnalyticsRule * rule)
{
  g_free (rule->label);
  g_free (rule->points);
  g_free (rule);
}

static void
group_free (RdsAnalyticsGroup * g)
{
  if (!g)
    return;
  g_clear_pointer (&g->class_ids, g_array_unref);
  g_ptr_array_unref (g->rules);
  g_free (g);
}

static void
stream_free (RdsAnalyticsStream * s)
{
  for (guint f = 0; f < RDS_ANALYTICS_N_FEATURES; f++)
    group_free (s->groups[f]);
  g_free (s);
}

/* Fills @base with the defaults of the keys of group kind @kind; rds_analytics_config_check_table makes sure every
 * default parses. */
static void
apply_defaults (gpointer base, guint kind)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    gchar *reason = NULL;

    if (!(keys[i].groups & kind) || !keys[i].field || !keys[i].def)
      continue;
    if (!parse_value (&keys[i], keys[i].def, base, &reason))
      g_error ("bad default for %s: %s", keys[i].name, reason);
  }
}

/* The table entry for @name in group kind @kind; NULL: an unknown key. */
static const KeyInfo *
find_key (guint kind, const gchar * name)
{
  /* a locale-suffixed key (KEY[locale]) is no key of ours */
  if (strchr (name, '['))
    return NULL;
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gsize plen = rule_prefix_len (k);

    if (!(k->groups & kind))
      continue;
    if (plen ? !strncmp (name, k->name, plen) : !strcmp (name, k->name))
      return k;
  }
  return NULL;
}

/* The raw value of @key, stripped (g_free). */
static gchar *
raw_value (GKeyFile * kf, const gchar * group, const gchar * key)
{
  gchar *raw = g_key_file_get_value (kf, group, key, NULL);

  return raw ? g_strstrip (raw) : g_strdup ("");
}

static gboolean
parse_property (GKeyFile * kf, const gchar * path, RdsAnalyticsConfig * cfg, GError ** error)
{
  /* unknown keys in [property] are ignored silently (DeepStream 8.0 behaviour) */
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gchar *raw, *reason = NULL;
    gboolean ok;

    if (!(k->groups & GRP_PROPERTY))
      continue;
    if (!g_key_file_has_key (kf, "property", k->name, NULL)) {
      if (k->required) {
        g_set_error (error, RDS_ANALYTICS_CONFIG_ERROR, RDS_ANALYTICS_CONFIG_ERROR_INVALID,
            "%s:[property] %s: missing (a required key)", path, k->name);
        return FALSE;
      }
      continue;
    }
    raw = raw_value (kf, "property", k->name);
    if (!*raw)
      ok = key_error (error, path, "property", k->name, raw, "empty value");
    else if (!(ok = parse_value (k, raw, cfg, &reason)))
      key_error (error, path, "property", k->name, raw, "%s", reason);
    g_free (reason);
    g_free (raw);
    if (!ok)
      return FALSE;
  }
  /* 0 reads as the default (DeepStream 8.0 behaviour) */
  if (!cfg->display_font_size)
    cfg->display_font_size = 12;
  return TRUE;
}

/* The feature whose labels share objInROIcnt with @f's: ROI filtering and overcrowding; RDS_ANALYTICS_N_FEATURES for
 * the others, whose labels are independent. */
static RdsAnalyticsFeature
label_sharing_feature (RdsAnalyticsFeature f)
{
  switch (f) {
    case RDS_ANALYTICS_ROI_FILTERING:
      return RDS_ANALYTICS_OVERCROWDING;
    case RDS_ANALYTICS_OVERCROWDING:
      return RDS_ANALYTICS_ROI_FILTERING;
    default:
      return RDS_ANALYTICS_N_FEATURES;
  }
}

static gboolean
group_has_label (const RdsAnalyticsGroup * g, const gchar * label)
{
  for (guint i = 0; g && i < g->rules->len; i++)
    if (strcmp (((const RdsAnalyticsRule *) g_ptr_array_index (g->rules, i))->label, label) == 0)
      return TRUE;
  return FALSE;
}

static gboolean
parse_feature_group (GKeyFile * kf, const gchar * path, const gchar * name, const RdsAnalyticsStream * s,
    RdsAnalyticsGroup * g, GError ** error)
{
  RdsAnalyticsFeature other = label_sharing_feature (g->feature);
  guint kind = features[g->feature].kind;
  gchar **names = g_key_file_get_keys (kf, name, NULL, NULL);
  GHashTable *labels = g_hash_table_new (g_str_hash, g_str_equal);
  gboolean ok = TRUE;

  /* in file order, so the rules keep the config's order */
  for (guint i = 0; ok && names && names[i]; i++) {
    const KeyInfo *k = find_key (kind, names[i]);
    gchar *raw, *reason = NULL;

    if (!k) {
      /* DeepStream 8.0 behaviour: logged, and the plugin carries on */
      g_message ("%s:[%s] %s: unknown key, ignored", path, name, names[i]);
      continue;
    }
    raw = raw_value (kf, name, names[i]);
    if (is_rule_type (k->type)) {
      const gchar *label = names[i] + rule_prefix_len (k);
      RdsAnalyticsRule *rule;

      /* ours: 8.0 tests both polygons of a duplicate label and draws one of them twice */
      if (!*label)
        ok = key_error (error, path, name, names[i], raw, "empty label");
      else if (!g_hash_table_add (labels, (gpointer) label))
        ok = key_error (error, path, name, names[i], raw, "duplicate label in this group");
      /* ours: the ROI-filtering and overcrowding labels of a stream are the keys of one map, objInROIcnt, where 8.0
       * lets one overwrite the other */
      else if (other != RDS_ANALYTICS_N_FEATURES && group_has_label (s->groups[other], label))
        ok = key_error (error, path, name, names[i], raw, "label already used by [%s-stream-%u]",
            features[other].name, s->pad_index);
      else if (!*raw)           /* ours: an empty rule crashes 8.0 */
        ok = key_error (error, path, name, names[i], raw, "empty value");
      else if (!(rule = parse_rule (k, label, raw, &reason)))
        ok = key_error (error, path, name, names[i], raw, "%s", reason);
      else
        g_ptr_array_add (g->rules, rule);
    } else if (!*raw) {
      /* an empty class-id means every class; an ignored string (mode in a direction group) may be empty */
      if (k->type == TYPE_INT_LIST)
        g_clear_pointer (&FIELD (GArray *, g, k), g_array_unref);
      else if (k->type != TYPE_STRING)
        ok = key_error (error, path, name, names[i], raw, "empty value");
    } else if (!parse_value (k, raw, k->field ? g : NULL, &reason)) {
      ok = key_error (error, path, name, names[i], raw, "%s", reason);
    }
    g_free (reason);
    g_free (raw);
  }
  /* a group without a rule is an error (DeepStream 8.0 behaviour) */
  if (ok && !g->rules->len)
    ok = group_error (error, path, name, "no %s<label> key: the group needs at least one rule",
        features[g->feature].rule_key);
  g_hash_table_unref (labels);
  g_strfreev (names);
  return ok;
}

static gint
compare_streams (gconstpointer a, gconstpointer b)
{
  const RdsAnalyticsStream *sa = *(RdsAnalyticsStream * const *) a, *sb = *(RdsAnalyticsStream * const *) b;

  return sa->pad_index < sb->pad_index ? -1 : sa->pad_index > sb->pad_index;
}

static RdsAnalyticsStream *
find_stream (const RdsAnalyticsConfig * cfg, guint pad_index)
{
  for (guint i = 0; i < cfg->streams->len; i++) {
    RdsAnalyticsStream *s = g_ptr_array_index (cfg->streams, i);

    if (s->pad_index == pad_index)
      return s;
  }
  return NULL;
}

static RdsAnalyticsStream *
get_stream (RdsAnalyticsConfig * cfg, guint pad_index)
{
  RdsAnalyticsStream *s = find_stream (cfg, pad_index);

  if (!s) {
    s = g_new0 (RdsAnalyticsStream, 1);
    s->pad_index = pad_index;
    g_ptr_array_add (cfg->streams, s);
  }
  return s;
}

static gboolean
is_digits (const gchar * s)
{
  if (!*s)
    return FALSE;
  for (; *s; s++)
    if (!g_ascii_isdigit (*s))
      return FALSE;
  return TRUE;
}

/* A feature group: [<feature>-stream-<n>]. */
static gboolean
parse_group (GKeyFile * kf, const gchar * path, const gchar * name, RdsAnalyticsConfig * cfg, GError ** error)
{
  for (guint f = 0; f < RDS_ANALYTICS_N_FEATURES; f++) {
    gsize len = strlen (features[f].name);
    const gchar *rest;
    guint64 n;
    RdsAnalyticsStream *s;
    RdsAnalyticsGroup *g;

    if (strncmp (name, features[f].name, len) != 0 || name[len] != '-')
      continue;
    rest = name + len + 1;
    if (is_digits (rest)) {
      /* the spelling without "stream", which the docs also use: skipped (DeepStream 8.0 behaviour) */
      g_message ("%s:[%s]: group ignored (the group name is [%s-stream-%s])", path, name, features[f].name, rest);
      return TRUE;
    }
    if (!g_str_has_prefix (rest, "stream-"))
      break;
    rest += strlen ("stream-");
    /* ours: a bad stream number is an error. Leading zeros are refused, so each stream has one spelling: GKeyFile
     * merges repeated headers of one spelling, never "-7" and "-007". */
    if (!is_digits (rest) || (rest[0] == '0' && rest[1]) || !g_ascii_string_to_unsigned (rest, 10, 0, G_MAXUINT, &n,
            NULL))
      return group_error (error, path, name, "%s: not a stream number (the frame meta's pad_index, without leading "
          "zeros)", rest);
    s = get_stream (cfg, (guint) n);
    /* cannot happen with one spelling per stream; an error rather than a dropped group if it ever does */
    if (s->groups[f])
      return group_error (error, path, name, "a second %s group for stream %u", features[f].name, (guint) n);
    g = g_new0 (RdsAnalyticsGroup, 1);
    g->feature = f;
    g->pad_index = (guint) n;
    g->rules = g_ptr_array_new_with_free_func ((GDestroyNotify) rule_free);
    apply_defaults (g, features[f].kind);
    s->groups[f] = g;
    return parse_feature_group (kf, path, name, s, g, error);
  }
  /* ours: logged too */
  g_message ("%s:[%s]: unknown group, ignored", path, name);
  return TRUE;
}

RdsAnalyticsConfig *
rds_analytics_config_load (const gchar * path, GError ** error)
{
  GKeyFile *kf;
  GError *err = NULL;
  RdsAnalyticsConfig *cfg;
  gchar **groups;
  gboolean ok = TRUE;

  g_return_val_if_fail (path != NULL, NULL);

  kf = g_key_file_new ();
  /* with the translations kept, a locale-suffixed key is seen (and logged as unknown), not silently dropped */
  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_KEEP_TRANSLATIONS, &err)) {
    g_set_error (error, RDS_ANALYTICS_CONFIG_ERROR, err->domain == G_KEY_FILE_ERROR
        && err->code != G_KEY_FILE_ERROR_NOT_FOUND ? RDS_ANALYTICS_CONFIG_ERROR_PARSE
        : RDS_ANALYTICS_CONFIG_ERROR_FILE, "%s: can't read the config: %s", path, err->message);
    g_error_free (err);
    g_key_file_unref (kf);
    return NULL;
  }

  cfg = g_new0 (RdsAnalyticsConfig, 1);
  cfg->streams = g_ptr_array_new_with_free_func ((GDestroyNotify) stream_free);
  apply_defaults (cfg, GRP_PROPERTY);
  if (!g_key_file_has_group (kf, "property"))
    ok = group_error (error, path, "property", "missing group (the config needs it)");
  else
    ok = parse_property (kf, path, cfg, error);

  groups = g_key_file_get_groups (kf, NULL);
  for (guint i = 0; ok && groups[i]; i++)
    if (strcmp (groups[i], "property") != 0)
      ok = parse_group (kf, path, groups[i], cfg, error);
  g_strfreev (groups);
  g_key_file_unref (kf);
  if (!ok) {
    rds_analytics_config_free (cfg);
    return NULL;
  }
  g_ptr_array_sort (cfg->streams, compare_streams);
  return cfg;
}

void
rds_analytics_config_free (RdsAnalyticsConfig * cfg)
{
  if (!cfg)
    return;
  g_ptr_array_unref (cfg->streams);
  g_free (cfg);
}

const RdsAnalyticsStream *
rds_analytics_config_get_stream (const RdsAnalyticsConfig * cfg, guint pad_index)
{
  g_return_val_if_fail (cfg != NULL, NULL);

  return find_stream (cfg, pad_index);
}

const gchar *
rds_analytics_feature_name (RdsAnalyticsFeature feature)
{
  g_return_val_if_fail (feature < RDS_ANALYTICS_N_FEATURES, NULL);
  return features[feature].name;
}

static gchar *
groups_text (guint groups)
{
  GString *str;

  if (groups & GRP_PROPERTY)
    return g_strdup ("property");
  str = g_string_new (NULL);
  for (guint f = 0; f < RDS_ANALYTICS_N_FEATURES; f++)
    if (groups & features[f].kind)
      g_string_append_printf (str, "%s%s", str->len ? ", " : "", features[f].name);
  return g_string_free (str, FALSE);
}

void
rds_analytics_config_foreach_key (RdsAnalyticsConfigKeyFunc func, gpointer user_data)
{
  g_return_if_fail (func != NULL);

  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gchar min[G_ASCII_DTOSTR_BUF_SIZE], max[G_ASCII_DTOSTR_BUF_SIZE], *range = NULL;
    gchar *groups = groups_text (k->groups);
    RdsAnalyticsKeyDesc desc = {
      .groups = groups,
      .name = k->name,
      .type = type_names[k->type],
      .status = k->status,
      .required = k->required,
      .default_value = k->def,
      .default_ours = k->def_ours,
      .note = k->note,
    };

    if (is_number_type (k->type)) {
      rds_config_format_number (min, k->min);
      rds_config_format_number (max, k->max);
      range = k->max == G_MAXINT ? g_strdup_printf (">= %s", min) : g_strdup_printf ("%s..%s", min, max);
    }
    desc.range = range;
    func (&desc, user_data);
    g_free (range);
    g_free (groups);
  }
}

static gboolean G_GNUC_PRINTF (3, 4)
table_error (GError ** error, const KeyInfo * k, const gchar * fmt, ...)
{
  gchar *msg, *groups = groups_text (k->groups);
  va_list ap;

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_ANALYTICS_CONFIG_ERROR, RDS_ANALYTICS_CONFIG_ERROR_INVALID, "key table: [%s] %s: %s",
      groups, k->name, msg);
  g_free (groups);
  g_free (msg);
  return FALSE;
}

gboolean
rds_analytics_config_check_table (GError ** error)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gboolean rule = is_rule_type (k->type);

    for (guint j = 0; j < i; j++)
      if ((keys[j].groups & k->groups) && !strcmp (keys[j].name, k->name))
        return table_error (error, k, "listed twice for one group kind");
    if (!k->groups || ((k->groups & GRP_PROPERTY) && k->groups != GRP_PROPERTY))
      return table_error (error, k, "a key is in [property] or in feature groups, not both");
    if (rule != (rule_prefix_len (k) > 0) || (rule && !g_str_has_suffix (k->name, "<label>")))
      return table_error (error, k, "rule keys, and only they, are named <prefix><label>");
    if (rule && (k->field || k->def || k->required || k->status != RDS_ANALYTICS_KEY_SUPPORTED))
      return table_error (error, k, "rule keys are supported, with no field, default or required flag");
    if (!rule && k->status == RDS_ANALYTICS_KEY_SUPPORTED && !k->field)
      return table_error (error, k, "supported keys need a field");
    if (k->type == TYPE_STRING && k->field)
      return table_error (error, k, "strings are not stored");
    if (k->status != RDS_ANALYTICS_KEY_SUPPORTED && !k->note)
      return table_error (error, k, "keys that are not supported need a note");
    if (k->required && (k->def || k->groups != GRP_PROPERTY))
      return table_error (error, k, "required keys are [property] keys with no default");
    if (k->def_ours && !k->def)
      return table_error (error, k, "a default marked ours needs a default");
    if (is_number_type (k->type) && k->min > k->max)
      return table_error (error, k, "a number needs min <= max");
    if (!is_number_type (k->type) && (k->min != 0 || k->max != 0))
      return table_error (error, k, "only numbers have a range");
    if (k->def) {
      gchar *reason = NULL;

      if (!parse_value (k, k->def, NULL, &reason)) {
        table_error (error, k, "default \"%s\": %s", k->def, reason);
        g_free (reason);
        return FALSE;
      }
    }
  }
  return TRUE;
}
