/* Markdown helpers for the docs lints (element_docs, element_compat_matrix and the config-key lints).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include <string.h>

/* CommonMark code fences, for both lints. A line of at most 3 spaces, then 3 or more '`' or '~' (a '`' fence's info
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
 * g_strfreev. Cells are split on every '|', also inside backticks, so a key table's cells hold none. A "#" or "##"
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
    /* "| a | b |" splits into "", " a ", " b ", "" */
    cells = g_strsplit (lines[i] + 1, "|", -1);
    for (guint c = 0; cells[c]; c++)
      g_strstrip (cells[c]);
    if (g_strv_length (cells) != t->n_cells + 1 || !g_str_has_prefix (cells[0], "`")
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
