/* docs/compat-matrix.md has a property table for rdsmultistreamtiler, rdsosd, rdslogger, rdsvideosink and rdsurisrcbin:
 * one row per property the element has, and no row for a property it lacks (cpu).
 *
 * The properties checked are those g_object_class_list_properties returns, minus the ones GstObject, GstElement and
 * GstBin install (`name`, `parent`, `async-handling`, `message-forward`): every GStreamer element has them, and
 * DeepStream's tables mostly leave them out. A table may still have a row for one (nvurisrcbin's page documents
 * `async-handling` and `message-forward`). GstBaseTransform's `qos` is kept, since DeepStream 8.0's gst-inspect shows
 * it for the transforms.
 *
 * An element's table is the one under the "### `<element>` (" heading; its rows start with "| `" and the first cell
 * names one or more properties in backticks ("`x-clock-offset`, `y-clock-offset`").
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <string.h>

static const gchar *const elements[] = { "rdsmultistreamtiler", "rdsosd", "rdslogger", "rdsvideosink", "rdsurisrcbin" };

static gboolean
is_common_property (GParamSpec * pspec)
{
  return pspec->owner_type == GST_TYPE_OBJECT || pspec->owner_type == GST_TYPE_ELEMENT
      || pspec->owner_type == GST_TYPE_BIN;
}

/* The property names in the first cells of the table under the element's heading. */
static GHashTable *
matrix_properties (gchar ** lines, const gchar * element)
{
  GHashTable *names = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  gchar *heading = g_strdup_printf ("### `%s` (", element);
  gboolean in_table = FALSE;

  for (guint i = 0; lines[i]; i++) {
    const gchar *p, *end;

    if (lines[i][0] == '#') {
      if (in_table)
        break;
      in_table = g_str_has_prefix (lines[i], heading);
      continue;
    }
    if (!in_table || !g_str_has_prefix (lines[i], "| `"))
      continue;
    end = strchr (lines[i] + 1, '|');
    if (!end)
      continue;
    p = lines[i] + 1;
    while ((p = strchr (p, '`')) && p < end) {
      const gchar *close = strchr (p + 1, '`');

      if (!close || close > end)
        break;
      g_hash_table_add (names, g_strndup (p + 1, close - p - 1));
      p = close + 1;
    }
  }
  g_free (heading);
  return names;
}

/* The row a property needs, for the failure message: name, value type, default; status and notes left to fill. */
static gchar *
expected_row (GParamSpec * pspec)
{
  GValue v = G_VALUE_INIT;
  gchar *def, *row;

  g_value_init (&v, pspec->value_type);
  g_param_value_set_default (pspec, &v);
  def = (pspec->flags & G_PARAM_READABLE) ? g_strdup_value_contents (&v) : g_strdup ("");
  row = g_strdup_printf ("| `%s` | %s | `%s` | ... | ... |", pspec->name, g_type_name (pspec->value_type), def);
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

  for (guint e = 0; e < G_N_ELEMENTS (elements); e++) {
    GstElement *el = gst_element_factory_make (elements[e], NULL);
    GHashTable *rows;
    GParamSpec **props;
    guint n_props;
    GHashTableIter it;
    gpointer name;

    if (!el) {
      /* rdsosd is optional (-Dosd); the others are always built. */
      g_assert_cmpstr (elements[e], ==, "rdsosd");
      g_assert_false (RDS_HAVE_OSD);
      g_test_message ("%s is not built: its table is not checked", elements[e]);
      continue;
    }
    rows = matrix_properties (lines, elements[e]);
    if (g_hash_table_size (rows) == 0) {
      g_test_message ("compat matrix: no property table under \"### `%s` (\"", elements[e]);
      n_bad++;
    }
    props = g_object_class_list_properties (G_OBJECT_GET_CLASS (el), &n_props);
    for (guint i = 0; i < n_props; i++) {
      if (is_common_property (props[i])) {
        /* a row is allowed, not needed */
        g_hash_table_remove (rows, props[i]->name);
        continue;
      }
      if (!g_hash_table_remove (rows, props[i]->name)) {
        gchar *row = expected_row (props[i]);

        g_test_message ("compat matrix, %s properties: %s is missing; expected: %s", elements[e], props[i]->name, row);
        g_free (row);
        n_bad++;
      }
      n_checked++;
    }
    g_hash_table_iter_init (&it, rows);
    while (g_hash_table_iter_next (&it, &name, NULL)) {
      g_test_message ("compat matrix, %s properties: %s has a row but is not a property", elements[e],
          (const gchar *) name);
      n_bad++;
    }
    g_free (props);
    g_hash_table_unref (rows);
    gst_object_unref (el);
  }
  g_test_message ("%u properties checked", n_checked);
  g_assert_cmpuint (n_bad, ==, 0);
  g_strfreev (lines);
  g_free (text);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/elements/compat-matrix", test_compat_matrix);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
