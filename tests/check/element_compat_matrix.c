/* docs/compat-matrix.md has a status table (| Property | Status | Difference |) for every factory in
 * rds-element-pages.h: one row per property the element has, and no row for a property it lacks (cpu). A factory
 * the build leaves out (rdsosd, rdsinfer or rdstracker; rds-element-pages.h) is skipped with a message.
 *
 * The properties checked are those g_object_class_list_properties returns, minus two kinds, for which a row is
 * allowed but not needed:
 * - the ones GstObject, GstElement and GstBin install (`name`, `parent`, `async-handling`, `message-forward`): every
 *   GStreamer element has them, and DeepStream's tables mostly leave them out (nvurisrcbin's page documents
 *   `async-handling` and `message-forward`);
 * - the ones GStreamer's base classes install (GstVideoDecoder, GstBaseTransform, GstAggregator, GstBaseSink,
 *   GstBaseSrc): they are on the element pages. GstBaseTransform's `qos` is kept, since DeepStream 8.0's gst-inspect
 *   shows it for the transforms.
 *
 * An element's table is the one under the "### `<element>` (" heading; its rows start with "| `" and the first cell
 * names one or more properties in backticks ("`x-clock-offset`, `y-clock-offset`"). Each section also links to the
 * element's page, "](elements/<page>)": the lines from its heading to the next heading, outside code fences.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <string.h>

#include "rds-element-pages.h"

/* A property of one of GStreamer's base classes, documented on the element pages; GstBaseTransform's `qos` is not
 * one. The types are looked up by name, so the test needs no GStreamer library beyond the core. */
static gboolean
is_base_class_property (GParamSpec * pspec)
{
  static const gchar *const bases[] = { "GstVideoDecoder", "GstBaseTransform", "GstAggregator", "GstBaseSink",
    "GstBaseSrc"
  };

  if (strcmp (pspec->name, "qos") == 0 && pspec->owner_type == g_type_from_name ("GstBaseTransform"))
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS (bases); i++) {
    GType t = g_type_from_name (bases[i]);

    if (t != 0 && pspec->owner_type == t)
      return TRUE;
  }
  return FALSE;
}

/* The property names in the first cells of the status table under the element's heading, outside code fences. The
 * table starts with the header row | Property | Status | Difference | and a separator; every row has three cells and
 * starts with "| `". A malformed row, a name in two rows and a table without the header count in *n_bad, with a
 * message. */
static GHashTable *
matrix_properties (gchar ** lines, const gchar * element, guint * n_bad)
{
  GHashTable *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gchar *heading = g_strdup_printf ("### `%s` (", element);
  gboolean in_section = FALSE, header = FALSE;
  gchar fence = 0;
  guint fence_len = 0;

  for (guint i = 0; lines[i]; i++) {
    gchar **cells, **row_names;

    if (rds_md_fence_line (lines[i], &fence, &fence_len))
      continue;
    if (lines[i][0] == '#') {
      if (in_section)
        break;
      in_section = g_str_has_prefix (lines[i], heading);
      continue;
    }
    if (!in_section || lines[i][0] != '|')
      continue;
    cells = rds_md_split_cells (lines[i]);
    if (!g_str_has_prefix (lines[i], "| `")) {
      if (g_strv_length (cells) == 3 && strcmp (cells[0], "Property") == 0 && strcmp (cells[1], "Status") == 0
          && strcmp (cells[2], "Difference") == 0)
        header = TRUE;
      else if (strspn (lines[i], "|-: \t") != strlen (lines[i])) {
        g_test_message ("compat matrix:%u: %s: expected the header | Property | Status | Difference |", i + 1,
            lines[i]);
        (*n_bad)++;
      }
      g_strfreev (cells);
      continue;
    }
    if (!header || g_strv_length (cells) != 3) {
      g_test_message ("compat matrix:%u: %s: %s", i + 1, lines[i], header ? "expected 3 cells" :
          "a row before the header | Property | Status | Difference |");
      (*n_bad)++;
      g_strfreev (cells);
      continue;
    }
    row_names = rds_md_cell_names (cells[0]);
    for (guint n = 0; row_names[n]; n++)
      if (!g_hash_table_add (names, g_strdup (row_names[n]))) {
        g_test_message ("compat matrix:%u: %s has a second row", i + 1, row_names[n]);
        (*n_bad)++;
      }
    g_strfreev (row_names);
    g_strfreev (cells);
  }
  g_free (heading);
  return names;
}

/* Whether the section under the "### `<element>` (" heading links to `page` in docs/elements/. */
static gboolean
section_links_page (gchar ** lines, const gchar * element, const gchar * page)
{
  gchar *heading = g_strdup_printf ("### `%s` (", element);
  gchar *link = g_strdup_printf ("](elements/%s)", page);
  gboolean in_section = FALSE, found = FALSE;
  gchar fence = 0;
  guint fence_len = 0;

  for (guint i = 0; lines[i] && !found; i++) {
    if (rds_md_fence_line (lines[i], &fence, &fence_len))
      continue;
    if (lines[i][0] == '#') {
      if (in_section)
        break;
      in_section = g_str_has_prefix (lines[i], heading);
      continue;
    }
    found = in_section && strstr (lines[i], link) != NULL;
  }
  g_free (link);
  g_free (heading);
  return found;
}

/* The row a property needs, for the failure message: the slim status row, with the property's type and default as a
 * hint (the page has them). */
static gchar *
expected_row (GParamSpec * pspec)
{
  GValue v = G_VALUE_INIT;
  gchar *def, *row;

  g_value_init (&v, pspec->value_type);
  g_param_value_set_default (pspec, &v);
  def = (pspec->flags & G_PARAM_READABLE) ? g_strdup_value_contents (&v) : g_strdup ("");
  row = g_strdup_printf ("| `%s` | <status> | <difference> | (%s, default %s)", pspec->name,
      g_type_name (pspec->value_type), def);
  g_free (def);
  g_value_unset (&v);
  return row;
}

static void
test_compat_matrix (void)
{
  gchar *text = NULL, **lines;
  GError *err = NULL;
  guint n_bad = 0, n_checked = 0;

  g_file_get_contents (RDS_COMPAT_MATRIX, &text, NULL, &err);
  g_assert_no_error (err);
  lines = g_strsplit (text, "\n", -1);

  for (guint e = 0; e < G_N_ELEMENTS (rds_element_pages); e++) {
    const gchar *factory = rds_element_pages[e].factory;
    GstElement *el = gst_element_factory_make (factory, NULL);
    GHashTable *rows;
    GParamSpec **props;
    guint n_props;
    GHashTableIter it;
    gpointer name;

    if (!rds_element_pages[e].configured) {
      g_test_message ("%s is not in this build: its table is not checked", factory);
      if (el)
        gst_object_unref (el);
      continue;
    }
    if (!el) {
      g_test_message ("compat matrix: %s is in this build but does not load", factory);
      n_bad++;
      continue;
    }
    rows = matrix_properties (lines, factory, &n_bad);
    if (g_hash_table_size (rows) == 0) {
      g_test_message ("compat matrix: no property table under \"### `%s` (\"", factory);
      n_bad++;
    }
    props = g_object_class_list_properties (G_OBJECT_GET_CLASS (el), &n_props);
    for (guint i = 0; i < n_props; i++) {
      if (rds_md_is_common_property (props[i]) || is_base_class_property (props[i])) {
        /* a row is allowed, not needed */
        g_hash_table_remove (rows, props[i]->name);
        continue;
      }
      if (!g_hash_table_remove (rows, props[i]->name)) {
        gchar *row = expected_row (props[i]);

        g_test_message ("compat matrix, %s properties: %s is missing; expected: %s", factory, props[i]->name, row);
        g_free (row);
        n_bad++;
      }
      n_checked++;
    }
    g_hash_table_iter_init (&it, rows);
    while (g_hash_table_iter_next (&it, &name, NULL)) {
      g_test_message ("compat matrix, %s properties: %s has a row but is not a property", factory,
          (const gchar *) name);
      n_bad++;
    }
    g_free (props);
    g_hash_table_unref (rows);
    gst_object_unref (el);
  }
  for (guint e = 0; e < G_N_ELEMENTS (rds_element_pages); e++) {
    if (!section_links_page (lines, rds_element_pages[e].factory, rds_element_pages[e].page)) {
      g_test_message ("compat matrix: the \"### `%s` (\" section has no link to elements/%s",
          rds_element_pages[e].factory, rds_element_pages[e].page);
      n_bad++;
    }
  }
  g_test_message ("%u properties checked, %u page links", n_checked, (guint) G_N_ELEMENTS (rds_element_pages));
  g_assert_cmpuint (n_bad, ==, 0);
  g_strfreev (lines);
  g_free (text);
}

/* The table reader: fences skipped, the header and the cell count checked, a name in two rows flagged. */
static void
test_matrix_parse (void)
{
  const gchar *doc =
      "### `x` (`nvx`) properties\n"
      "| Property | Status | Difference |\n|---|---|---|\n"
      "| `a`, `b` | supported | |\n"
      "```text\n| `fenced` | supported | |\n```\n"
      "| `c` | supported | `p|q` is one cell |\n"
      "### `y` (`nvy`) properties\n"
      "| Property | Status | Notes |\n|---|---|---|\n"
      "| `a` | supported | |\n"
      "### `z` (`nvz`) properties\n"
      "| Property | Status | Difference |\n|---|---|---|\n"
      "| `a` | supported |\n"
      "| `b` | supported | |\n"
      "| `b` | supported | |\n";
  gchar **lines = g_strsplit (doc, "\n", -1);
  guint n_bad = 0;
  GHashTable *names = matrix_properties (lines, "x", &n_bad);

  g_assert_cmpuint (n_bad, ==, 0);
  g_assert_cmpuint (g_hash_table_size (names), ==, 3);
  g_assert_false (g_hash_table_contains (names, "fenced"));
  g_hash_table_unref (names);
  names = matrix_properties (lines, "y", &n_bad);
  g_assert_cmpuint (n_bad, ==, 2);      /* the wrong header, then a row before the right one */
  g_hash_table_unref (names);
  n_bad = 0;
  names = matrix_properties (lines, "z", &n_bad);
  g_assert_cmpuint (n_bad, ==, 2);      /* two cells, then `b` twice */
  g_hash_table_unref (names);
  g_strfreev (lines);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/elements/compat-matrix-parse", test_matrix_parse);
  g_test_add_func ("/cpu/elements/compat-matrix", test_compat_matrix);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
