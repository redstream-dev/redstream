/* Markdown helpers for the docs lints (element_docs, element_compat_matrix and the config-key lints), so they all read
 * one Markdown dialect.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include <glib-object.h>
#include <string.h>

/* CommonMark code fences. A line of at most 3 spaces, then 3 or more '`' or '~' (a '`' fence's info
 * string has no '`') opens a fence; a line of at most 3 spaces, then at least as many of the same character and only
 * blanks after, closes it. Returns whether `line` is a fence line or inside a fence. `fence` is the open fence's
 * character (0: none) and `fence_len` its length; start both at 0. */
static inline gboolean
rds_md_fence_line (const char *line, char *fence, guint * fence_len)
{
  const char *p = line;
  guint n = 0;

  for (guint indent = 0; *p == ' ' && indent < 3; indent++)
    p++;
  if (*p == '`' || *p == '~')
    while (p[n] == *p)
      n++;
  if (*fence == 0) {
    if (n < 3 || (*p == '`' && strchr (p + n, '`') != NULL))
      return FALSE;
    *fence = *p;
    *fence_len = n;
    return TRUE;
  }
  if (n > 0 && *p == *fence && n >= *fence_len) {
    const char *q = p + n;

    while (*q == ' ' || *q == '\t' || *q == '\r')
      q++;
    if (*q == '\0')
      *fence = 0;
  }
  return TRUE;
}

/* The cells of a table row, stripped; '|' inside backticks or escaped as "\|" does not split. */
static inline gchar **
rds_md_split_cells (const gchar * row)
{
  GPtrArray *cells = g_ptr_array_new ();
  GString *cell = NULL;
  gboolean code = FALSE;

  for (const gchar * p = row; *p; p++) {
    if (*p == '\\' && p[1] == '|') {
      if (cell)
        g_string_append_c (cell, '|');
      p++;
      continue;
    }
    if (*p == '`')
      code = !code;
    if (*p == '|' && !code) {
      if (cell)
        g_ptr_array_add (cells, g_strstrip (g_string_free (cell, FALSE)));
      cell = g_string_new (NULL);
      continue;
    }
    if (cell)
      g_string_append_c (cell, *p);
  }
  /* what follows the last '|' is not a cell */
  if (cell)
    g_string_free (cell, TRUE);
  g_ptr_array_add (cells, NULL);
  return (gchar **) g_ptr_array_free (cells, FALSE);
}

/* The names in backticks in a first cell ("`a`, `b`"). */
static inline gchar **
rds_md_cell_names (const gchar * cell)
{
  GPtrArray *names = g_ptr_array_new ();
  const gchar *p = cell;

  while ((p = strchr (p, '`'))) {
    const gchar *close = strchr (p + 1, '`');

    if (!close)
      break;
    g_ptr_array_add (names, g_strndup (p + 1, close - p - 1));
    p = close + 1;
  }
  g_ptr_array_add (names, NULL);
  return (gchar **) g_ptr_array_free (names, FALSE);
}

/* A property every GStreamer element has, from GstObject, GstElement or GstBin (`name`, `parent`, `async-handling`,
 * `message-forward`): the pages and the matrix need no row for it, and may have one. The types are looked up by name,
 * so the config lints need no GStreamer. */
static inline gboolean
rds_md_is_common_property (GParamSpec * pspec)
{
  static const char *const owners[] = { "GstObject", "GstElement", "GstBin" };

  for (guint i = 0; i < G_N_ELEMENTS (owners); i++) {
    GType t = g_type_from_name (owners[i]);

    if (t != 0 && pspec->owner_type == t)
      return TRUE;
  }
  return FALSE;
}

/* A config page's key tables, for rds_md_read_key_rows. */
typedef struct
{
  const char *doc;              /* the page's file name, for messages */
  guint n_cells;                /* cells in a key row */
  gint group_cell;              /* the cell holding the row's group, or -1: the group of the section's heading */
  /* The group a heading starts (newly allocated; "" when group_cell holds it), or NULL when the heading is not a
   * key section. */
  gchar *(*heading_group) (const char *heading);
} RdsMdKeyTable;

/* The key rows ("| `key` | ... |") of the key sections in @text, into @rows ("group.key" -> the row's cells, a
 * gchar ** whose cells[0] is the key without its backticks). @rows frees its keys with g_free and its values with
 * g_strfreev. Cells are split as rds_md_split_cells splits them. A "#" or "##"
 * heading starts or ends a key section; "###" subheadings stay in it, and a "###" heading that would start one is an
 * error. Nothing inside code fences is read.
 * Returns the number of errors (malformed and duplicate rows, misplaced key sections), each with a message. */
static inline guint
rds_md_read_key_rows (const char *text, const RdsMdKeyTable * t, GHashTable * rows)
{
  gchar **lines = g_strsplit (text, "\n", -1), *group = NULL;
  gchar fence = 0;
  guint fence_len = 0, n_bad = 0;

  g_assert (t->group_cell < (gint) t->n_cells);
  for (guint i = 0; lines[i]; i++) {
    gchar **cells, *id;

    if (rds_md_fence_line (lines[i], &fence, &fence_len))
      continue;
    if (g_str_has_prefix (lines[i], "###")) {
      gchar *g = t->heading_group (lines[i]);

      if (g) {
        g_test_message ("%s:%u: %s: a key section must be a ## heading", t->doc, i + 1, lines[i]);
        n_bad++;
        g_free (g);
      }
      continue;
    }
    if (lines[i][0] == '#') {
      g_free (group);
      group = t->heading_group (lines[i]);
      continue;
    }
    if (!group || !g_str_has_prefix (lines[i], "| `"))
      continue;
    cells = rds_md_split_cells (lines[i]);
    if (g_strv_length (cells) != t->n_cells || !g_str_has_prefix (cells[0], "`")
        || !g_str_has_suffix (cells[0], "`")) {
      g_test_message ("%s:%u: %s: expected %u cells, the first a `key`", t->doc, i + 1, lines[i], t->n_cells);
      n_bad++;
      g_strfreev (cells);
      continue;
    }
    cells[0][strlen (cells[0]) - 1] = '\0';
    memmove (cells[0], cells[0] + 1, strlen (cells[0]));
    id = g_strconcat (t->group_cell >= 0 ? cells[t->group_cell] : group, ".", cells[0], NULL);
    if (g_hash_table_contains (rows, id)) {
      g_test_message ("%s:%u: %s: duplicate row", t->doc, i + 1, lines[i]);
      n_bad++;
      g_free (id);
      g_strfreev (cells);
      continue;
    }
    g_hash_table_insert (rows, id, cells);
  }
  g_free (group);
  g_strfreev (lines);
  return n_bad;
}
