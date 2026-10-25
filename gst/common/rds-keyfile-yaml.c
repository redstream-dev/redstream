/* Redstream: reads a YAML config into a GKeyFile, so YAML and key-file configs go through the same typed parser.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-keyfile-yaml.h"

#include <string.h>
#include <yaml.h>

/* GKeyFile refuses (with a critical) group and key names it could not write back. */
static gboolean
valid_name (const gchar * s, gboolean is_group)
{
  if (!*s || !g_utf8_validate (s, -1, NULL))
    return FALSE;
  for (const gchar * p = s; *p; p++)
    if (*p == '[' || *p == ']' || g_ascii_iscntrl (*p) || (!is_group && *p == '='))
      return FALSE;
  return is_group || (!g_ascii_isspace (s[0]) && !g_ascii_isspace (s[strlen (s) - 1]));
}

static gboolean
fail (GError ** error, const gchar * path, const yaml_node_t * node, const gchar * what)
{
  g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE, "%s:%lu: %s", path,
      (gulong) node->start_mark.line + 1, what);
  return FALSE;
}

/* A plain (unquoted) empty, ~ or null scalar: YAML's null. */
static gboolean
is_null (const yaml_node_t * node)
{
  const gchar *v;

  if (node->type != YAML_SCALAR_NODE || node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE)
    return FALSE;
  v = (const gchar *) node->data.scalar.value;
  return !*v || !strcmp (v, "~") || !strcmp (v, "null") || !strcmp (v, "Null") || !strcmp (v, "NULL");
}

/* A scalar's text; null is "" (unset). */
static const gchar *
scalar (const yaml_node_t * node)
{
  return is_null (node) ? "" : (const gchar *) node->data.scalar.value;
}

static gboolean
add_group (GKeyFile * kf, yaml_document_t * doc, const gchar * path, const gchar * group, yaml_node_t * map,
    GError ** error)
{
  if (is_null (map))
    return TRUE;                /* "group:" or "group: ~", with nothing under it */
  if (map->type != YAML_MAPPING_NODE)
    return fail (error, path, map, "a group must be a mapping of keys to values");

  for (yaml_node_pair_t * pair = map->data.mapping.pairs.start; pair < map->data.mapping.pairs.top; pair++) {
    yaml_node_t *k = yaml_document_get_node (doc, pair->key), *v = yaml_document_get_node (doc, pair->value);

    if (k->type != YAML_SCALAR_NODE || !valid_name (scalar (k), FALSE))
      return fail (error, path, k, "a key must be a plain name (no '=', '[', ']' or control characters)");
    if (v->type == YAML_SCALAR_NODE) {
      if (!g_utf8_validate (scalar (v), -1, NULL))
        return fail (error, path, v, "a value must be valid UTF-8");
      g_key_file_set_string (kf, group, scalar (k), scalar (v));
    } else if (v->type == YAML_SEQUENCE_NODE) {
      GString *joined = g_string_new (NULL);

      for (yaml_node_item_t * it = v->data.sequence.items.start; it < v->data.sequence.items.top; it++) {
        yaml_node_t *item = yaml_document_get_node (doc, *it);

        if (item->type != YAML_SCALAR_NODE || !g_utf8_validate (scalar (item), -1, NULL)) {
          g_string_free (joined, TRUE);
          return fail (error, path, item, "a list must hold plain values");
        }
        if (strchr (scalar (item), ';')) {
          g_string_free (joined, TRUE);
          return fail (error, path, item, "a list item can't contain ';' (it separates the items)");
        }
        if (joined->len)
          g_string_append_c (joined, ';');
        g_string_append (joined, scalar (item));
      }
      g_key_file_set_string (kf, group, scalar (k), joined->str);
      g_string_free (joined, TRUE);
    } else {
      return fail (error, path, v, "a value must be a scalar or a list of scalars");
    }
  }
  return TRUE;
}

GKeyFile *
rds_keyfile_from_yaml (const gchar * path, GError ** error)
{
  gchar *text = NULL;
  gsize len = 0;
  yaml_parser_t parser;
  yaml_document_t doc;
  yaml_node_t *root;
  GKeyFile *kf;
  gboolean ok = TRUE;

  if (!g_file_get_contents (path, &text, &len, error))
    return NULL;
  yaml_parser_initialize (&parser);
  yaml_parser_set_input_string (&parser, (const guchar *) text, len);
  if (!yaml_parser_load (&parser, &doc)) {
    if (parser.context)
      g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE, "%s:%lu: %s (%s at line %lu)", path,
          (gulong) parser.problem_mark.line + 1, parser.problem, parser.context,
          (gulong) parser.context_mark.line + 1);
    else
      g_set_error (error, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE, "%s:%lu: %s", path,
          (gulong) parser.problem_mark.line + 1, parser.problem ? parser.problem : "invalid YAML");
    yaml_parser_delete (&parser);
    g_free (text);
    return NULL;
  }

  kf = g_key_file_new ();
  root = yaml_document_get_root_node (&doc);
  if (root && root->type != YAML_MAPPING_NODE) {
    ok = fail (error, path, root, "the top level must be a mapping of groups");
  } else if (root) {
    for (yaml_node_pair_t * pair = root->data.mapping.pairs.start; ok && pair < root->data.mapping.pairs.top;
        pair++) {
      yaml_node_t *k = yaml_document_get_node (&doc, pair->key);

      if (k->type != YAML_SCALAR_NODE || !valid_name (scalar (k), TRUE))
        ok = fail (error, path, k, "a group name must be a plain name (no '[', ']' or control characters)");
      else
        ok = add_group (kf, &doc, path, scalar (k), yaml_document_get_node (&doc, pair->value), error);
    }
  }
  yaml_document_delete (&doc);
  yaml_parser_delete (&parser);
  g_free (text);
  if (!ok)
    g_clear_pointer (&kf, g_key_file_unref);
  return kf;
}
