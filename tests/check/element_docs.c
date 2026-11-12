/* docs/elements/<page>.md, for every factory in rds-element-pages.h: the page has a row for each property, pad
 * template and signal of the element, and none for one it lacks; each property row has the property's type and
 * default (cpu). Also: every relative link in docs/, docs/elements/, README.md and AGENTS.md resolves.
 *
 * The page format:
 * - A table belongs to the "## " or "### " heading above it, up to the next heading of the same or a higher level,
 *   except the tables under a "### Only on `<factory>`" heading inside "## Properties", which belong to that factory
 *   only; the factory must be one that uses the page. Headings and tables inside code fences (``` or ~~~,
 *   CommonMark rules) are not read.
 * - Each table starts with the section's header row and a separator row; every other row has as many cells as the
 *   header, and starts with "| `". Cells are split on '|', except inside backticks or as "\|", so a table row must
 *   have no unpaired backticks. The first cell names one or more properties, pads or signals in backticks.
 * - "## Properties": | Property | Type | Default | Values | Changeable | Description |. The type is `int`, `uint`,
 *   `int64`, `uint64`, `long`, `ulong`, `float`, `double`, `boolean` or `string`; `enum` or `flags` then the GType
 *   name in backticks; otherwise the GType name in backticks. The default, in backticks: integers in decimal,
 *   floats with %g, `true`/`false`, a string in double quotes or `NULL`, an enum by its nick (by its number when it
 *   has none), flags by their nicks joined with '+' (`0`: none; bits no flag covers as "+0x..."). "—" (no
 *   backticks) for every other type, and for write-only properties. A Values cell that starts with a range "A–B"
 *   (numbers, an en dash, '-' or U+2212 for a minus) must start with the property's whole range, its minimum and
 *   maximum written as defaults are; narrower practical limits go in words after it. "N-M" with an ASCII hyphen
 *   fails; any other Values cell is not checked.
 *   GstObject's, GstElement's and GstBin's properties need no row (as in element_compat_matrix); a row for one is
 *   still checked. A page shared by two factories puts the rows they have in common there, and the others under
 *   "### Only on `<factory>`".
 * - "## Pads": | Pad | Direction | Presence | Caps |, a row per pad template (direction `sink`/`src`, presence
 *   `always`/`sometimes`/`request`).
 * - "## Pad properties": when a pad template's type is not GstPad, GstGhostPad or GstAggregatorPad, the properties
 *   that type adds, in a table like the properties' and under the same rules (GstObject's, GstPad's, GstProxyPad's,
 *   GstGhostPad's and GstAggregatorPad's need no row).
 * - "## Signals": | Signal | Arguments | Returns | Description |, a row for each signal of a GstRds* type the element
 *   derives from. A row for another signal the element has (GstElement's `pad-added`, say) is allowed.
 * Each failure is a test message naming the page (and line, for a row), with the row expected where there is one.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <string.h>

#include "rds-element-pages.h"

#define NO_DEFAULT "\xe2\x80\x94"     /* "—" */

/* ---- reporting ---- */

typedef struct
{
  const gchar *page;            /* file name in docs/elements/ */
  const gchar *factory;
  guint n_bad;
} Ctx;

/* One failure: "<page>:<line> (<factory>): ...", or without the line when it is 0. */
static void G_GNUC_PRINTF (3, 4)
fail_at (Ctx * ctx, guint line, const gchar * format, ...)
{
  va_list args;
  gchar *msg;

  va_start (args, format);
  msg = g_strdup_vprintf (format, args);
  va_end (args);
  if (line > 0)
    g_test_message ("%s:%u (%s): %s", ctx->page, line, ctx->factory, msg);
  else
    g_test_message ("%s (%s): %s", ctx->page, ctx->factory, msg);
  g_free (msg);
  ctx->n_bad++;
}

/* ---- page parsing ---- */

typedef struct
{
  gchar **lines;
  gboolean *in_fence;           /* per line: a fence line or inside a fence */
} Page;

/* A table row and its line number (from 1). */
typedef struct
{
  const gchar *text;
  guint line;
} Row;

static gboolean
read_page (const gchar * path, Page * page)
{
  gchar *text = NULL;
  gchar fence = 0;
  guint n, fence_len = 0;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return FALSE;
  page->lines = g_strsplit (text, "\n", -1);
  g_free (text);
  n = g_strv_length (page->lines);
  page->in_fence = g_new0 (gboolean, n + 1);
  for (guint i = 0; i < n; i++)
    page->in_fence[i] = rds_md_fence_line (page->lines[i], &fence, &fence_len);
  return TRUE;
}

static void
page_clear (Page * page)
{
  g_strfreev (page->lines);
  g_free (page->in_fence);
}

/* "## x" -> 2; 0 when the line is not a heading */
static guint
heading_level (const Page * page, guint i)
{
  const gchar *l = page->lines[i];
  guint n = 0;

  if (page->in_fence[i])
    return 0;
  while (l[n] == '#')
    n++;
  return n > 0 && l[n] == ' ' ? n : 0;
}

/* the heading's text, trailing blanks dropped, compared with `heading` ("## Properties") */
static gboolean
heading_is (const gchar * line, const gchar * heading)
{
  gsize n = strlen (line);

  while (n > 0 && g_ascii_isspace (line[n - 1]))
    n--;
  return n == strlen (heading) && strncmp (line, heading, n) == 0;
}

/* The cells of a table row, stripped; '|' inside backticks or escaped as "\|" does not split. */
static gchar **
split_cells (const gchar * row)
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
static gchar **
cell_names (const gchar * cell)
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

static const gchar *const prop_columns[] = { "Property", "Type", "Default", "Values", "Changeable", "Description",
  NULL
};
static const gchar *const pad_columns[] = { "Pad", "Direction", "Presence", "Caps", NULL };
static const gchar *const signal_columns[] = { "Signal", "Arguments", "Returns", "Description", NULL };

static gboolean
is_separator_row (const gchar * line)
{
  return line != NULL && strspn (line, "|-: \t\r") == strlen (line) && strchr (line, '-') != NULL;
}

/* One table of a section (Rows): the header row must be `columns`, then a separator; every other row has as many
 * cells and starts with "| `". Appends those rows to `rows`, and empties `table`. */
static void
take_table (Ctx * ctx, GArray * table, const gchar * heading, const gchar * const *columns, GArray * rows)
{
  guint n_cols = g_strv_length ((gchar **) columns);
  const Row *head;
  gchar **header;

  if (table->len == 0)
    return;
  head = &g_array_index (table, Row, 0);
  header = split_cells (head->text);
  if (!g_strv_equal ((const gchar * const *) header, columns)) {
    gchar *want = g_strjoinv (" | ", (gchar **) columns);

    fail_at (ctx, head->line, "a table under \"%s\" has the header \"%s\"; expected: | %s |", heading, head->text,
        want);
    g_free (want);
  }
  g_strfreev (header);
  if (table->len < 2 || !is_separator_row (g_array_index (table, Row, 1).text))
    fail_at (ctx, head->line, "a table under \"%s\" has no separator row after its header", heading);
  for (guint r = 2; r < table->len; r++) {
    const Row *row = &g_array_index (table, Row, r);
    gchar **cells = split_cells (row->text);
    guint n = g_strv_length (cells);

    g_strfreev (cells);
    if (n != n_cols)
      fail_at (ctx, row->line, "a row under \"%s\" has %u cells, the header %u: %s", heading, n, n_cols, row->text);
    else if (!g_str_has_prefix (row->text, "| `"))
      fail_at (ctx, row->line, "a row under \"%s\" names nothing in backticks: %s", heading, row->text);
    else
      g_array_append_val (rows, *row);
  }
  g_array_set_size (table, 0);
}

/* The table rows under the heading line `heading` (of level `level`), up to the next heading of a level <= `level`.
 * With `parent`, only a heading inside that level-2 section counts. In "## Properties", the rows under a nested
 * "Only on ..." heading are left out (they belong to one factory). Each table's shape is checked against `columns`.
 * Appends the Rows to `rows`; returns whether the heading was found. */
static gboolean
section_rows (Ctx * ctx, const Page * page, const gchar * parent, const gchar * heading, guint level,
    const gchar * const *columns, GArray * rows)
{
  gboolean found = FALSE, in = FALSE, skipping = FALSE, in_parent = parent == NULL;
  gboolean skip_only_on = g_strcmp0 (heading, "## Properties") == 0;
  GArray *table = g_array_new (FALSE, FALSE, sizeof (Row));
  guint skip_level = 0;

  for (guint i = 0; page->lines[i]; i++) {
    guint h = heading_level (page, i);
    gboolean table_line = !page->in_fence[i] && page->lines[i][0] == '|';

    if (!table_line || h > 0)
      take_table (ctx, table, heading, columns, rows);
    if (h > 0) {
      if (parent != NULL && h <= 2)
        in_parent = h == 2 && heading_is (page->lines[i], parent);
      if (in && h <= level) {
        in = FALSE;
        continue;
      }
      if (!in && h == level && in_parent && heading_is (page->lines[i], heading)) {
        in = found = TRUE;
        skipping = FALSE;
        continue;
      }
      if (in && skip_only_on) {
        if (skipping && h <= skip_level)
          skipping = FALSE;
        if (!skipping && g_str_has_prefix (page->lines[i] + h + 1, "Only on ")) {
          skipping = TRUE;
          skip_level = h;
        }
      }
      continue;
    }
    if (in && !skipping && table_line) {
      Row row = { page->lines[i], i + 1 };

      g_array_append_val (table, row);
    }
  }
  take_table (ctx, table, heading, columns, rows);
  g_array_unref (table);
  return found;
}

/* Every "### Only on ..." heading in "## Properties" names, in backticks, one factory whose page this is. */
static void
check_only_on_headings (Ctx * ctx, const Page * page)
{
  gboolean in_props = FALSE;

  for (guint i = 0; page->lines[i]; i++) {
    guint h = heading_level (page, i);
    gchar **names;
    gboolean ok = FALSE;

    if (h > 0 && h <= 2)
      in_props = h == 2 && heading_is (page->lines[i], "## Properties");
    if (!in_props || h != 3 || !g_str_has_prefix (page->lines[i] + h + 1, "Only on "))
      continue;
    names = cell_names (page->lines[i] + h + 1);
    for (guint e = 0; names[0] != NULL && names[1] == NULL && e < G_N_ELEMENTS (rds_element_pages); e++)
      ok |= strcmp (rds_element_pages[e].factory, names[0]) == 0 && strcmp (rds_element_pages[e].page,
          ctx->page) == 0;
    if (!ok)
      fail_at (ctx, i + 1, "\"%s\" names no factory that uses this page", page->lines[i]);
    g_strfreev (names);
  }
}

/* A section's rows by the names in their first cells. */
typedef struct
{
  GPtrArray *cells;             /* gchar **, one per row (owned) */
  GArray *lines;                /* guint, the line of each row */
  GHashTable *by_name;          /* name (owned) -> row index + 1 */
} Table;

static void
table_init (Ctx * ctx, Table * t, GArray * rows)
{
  t->cells = g_ptr_array_new_with_free_func ((GDestroyNotify) g_strfreev);
  t->lines = g_array_new (FALSE, FALSE, sizeof (guint));
  t->by_name = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  for (guint r = 0; r < rows->len; r++) {
    const Row *row = &g_array_index (rows, Row, r);
    gchar **cells = split_cells (row->text);
    gchar **names = cell_names (cells[0] ? cells[0] : "");

    g_ptr_array_add (t->cells, cells);
    g_array_append_val (t->lines, row->line);
    for (guint i = 0; names[i]; i++) {
      if (g_hash_table_contains (t->by_name, names[i])) {
        fail_at (ctx, row->line, "`%s` is named twice", names[i]);
        continue;
      }
      g_hash_table_insert (t->by_name, g_strdup (names[i]), GUINT_TO_POINTER (r + 1));
    }
    g_strfreev (names);
  }
}

/* The cells of the row naming `name`, and its line; NULL when none does. The name is taken out of the table. */
static gchar **
table_take (Table * t, const gchar * name, guint * line)
{
  guint idx = GPOINTER_TO_UINT (g_hash_table_lookup (t->by_name, name));

  if (idx == 0)
    return NULL;
  g_hash_table_remove (t->by_name, name);
  *line = g_array_index (t->lines, guint, idx - 1);
  return g_ptr_array_index (t->cells, idx - 1);
}

/* Every name left in the table: no such property, pad or signal. */
static void
table_report_leftovers (Ctx * ctx, Table * t, const gchar * what)
{
  GHashTableIter it;
  gpointer name, idx;

  g_hash_table_iter_init (&it, t->by_name);
  while (g_hash_table_iter_next (&it, &name, &idx))
    fail_at (ctx, g_array_index (t->lines, guint, GPOINTER_TO_UINT (idx) - 1), "`%s` has a row but is not a %s",
        (const gchar *) name, what);
}

static void
table_clear (Table * t)
{
  g_hash_table_unref (t->by_name);
  g_array_unref (t->lines);
  g_ptr_array_unref (t->cells);
}

static const gchar *
cell (gchar ** cells, guint i)
{
  for (guint k = 0; k < i; k++)
    if (cells[k] == NULL)
      return "";
  return cells[i] ? cells[i] : "";
}

/* ---- expected cells ---- */

static gchar *
type_cell (GParamSpec * pspec)
{
  GType t = pspec->value_type;

  switch (G_TYPE_FUNDAMENTAL (t)) {
    case G_TYPE_CHAR:
      return g_strdup ("char");
    case G_TYPE_UCHAR:
      return g_strdup ("uchar");
    case G_TYPE_BOOLEAN:
      return g_strdup ("boolean");
    case G_TYPE_INT:
      return g_strdup ("int");
    case G_TYPE_UINT:
      return g_strdup ("uint");
    case G_TYPE_LONG:
      return g_strdup ("long");
    case G_TYPE_ULONG:
      return g_strdup ("ulong");
    case G_TYPE_INT64:
      return g_strdup ("int64");
    case G_TYPE_UINT64:
      return g_strdup ("uint64");
    case G_TYPE_FLOAT:
      return g_strdup ("float");
    case G_TYPE_DOUBLE:
      return g_strdup ("double");
    case G_TYPE_STRING:
      return g_strdup ("string");
    case G_TYPE_ENUM:
      return g_strdup_printf ("enum `%s`", g_type_name (t));
    case G_TYPE_FLAGS:
      return g_strdup_printf ("flags `%s`", g_type_name (t));
    default:
      return g_strdup_printf ("`%s`", g_type_name (t));
  }
}

/* an enum value's nick, or its number when no value of the type has it */
static gchar *
enum_text (GType type, gint value)
{
  GEnumClass *klass = g_type_class_ref (type);
  GEnumValue *ev = g_enum_get_value (klass, value);
  gchar *s = ev ? g_strdup (ev->value_nick) : g_strdup_printf ("%d", value);

  g_type_class_unref (klass);
  return s;
}

static gint
flags_value_cmp (gconstpointer a, gconstpointer b)
{
  guint va = (*(GFlagsValue * const *) a)->value, vb = (*(GFlagsValue * const *) b)->value;

  return va < vb ? -1 : va > vb;
}

/* the nicks of the set flags in value order, joined with '+', then any bits no flag covers as "0x..."; "0" when none
 * is set */
static gchar *
flags_text (GType type, guint value)
{
  GFlagsClass *klass = g_type_class_ref (type);
  GPtrArray *sorted = g_ptr_array_new ();
  GString *s = g_string_new (NULL);
  guint left = value;

  for (guint i = 0; i < klass->n_values; i++)
    g_ptr_array_add (sorted, &klass->values[i]);
  g_ptr_array_sort (sorted, flags_value_cmp);
  for (guint i = 0; i < sorted->len; i++) {
    const GFlagsValue *v = g_ptr_array_index (sorted, i);

    if (v->value != 0 && (left & v->value) == v->value) {
      g_string_append_printf (s, "%s%s", s->len ? "+" : "", v->value_nick);
      left &= ~v->value;
    }
  }
  if (left != 0)
    g_string_append_printf (s, "%s0x%x", s->len ? "+" : "", left);
  if (s->len == 0)
    g_string_append_c (s, '0');
  g_ptr_array_unref (sorted);
  g_type_class_unref (klass);
  return g_string_free (s, FALSE);
}

static gchar *
default_cell (GParamSpec * pspec)
{
  GValue v = G_VALUE_INIT;
  gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
  gchar *s = NULL, *quoted;

  if (!(pspec->flags & G_PARAM_READABLE))
    return g_strdup (NO_DEFAULT);
  g_value_init (&v, pspec->value_type);
  g_param_value_set_default (pspec, &v);
  switch (G_TYPE_FUNDAMENTAL (pspec->value_type)) {
    case G_TYPE_CHAR:
      s = g_strdup_printf ("%d", g_value_get_schar (&v));
      break;
    case G_TYPE_UCHAR:
      s = g_strdup_printf ("%u", g_value_get_uchar (&v));
      break;
    case G_TYPE_BOOLEAN:
      s = g_strdup (g_value_get_boolean (&v) ? "true" : "false");
      break;
    case G_TYPE_INT:
      s = g_strdup_printf ("%d", g_value_get_int (&v));
      break;
    case G_TYPE_UINT:
      s = g_strdup_printf ("%u", g_value_get_uint (&v));
      break;
    case G_TYPE_LONG:
      s = g_strdup_printf ("%ld", g_value_get_long (&v));
      break;
    case G_TYPE_ULONG:
      s = g_strdup_printf ("%lu", g_value_get_ulong (&v));
      break;
    case G_TYPE_INT64:
      s = g_strdup_printf ("%" G_GINT64_FORMAT, g_value_get_int64 (&v));
      break;
    case G_TYPE_UINT64:
      s = g_strdup_printf ("%" G_GUINT64_FORMAT, g_value_get_uint64 (&v));
      break;
    case G_TYPE_FLOAT:
      s = g_strdup (g_ascii_formatd (buf, sizeof (buf), "%g", g_value_get_float (&v)));
      break;
    case G_TYPE_DOUBLE:
      s = g_strdup (g_ascii_formatd (buf, sizeof (buf), "%g", g_value_get_double (&v)));
      break;
    case G_TYPE_STRING:
      s = g_value_get_string (&v) ? g_strdup_printf ("\"%s\"", g_value_get_string (&v)) : g_strdup ("NULL");
      break;
    case G_TYPE_ENUM:
      s = enum_text (pspec->value_type, g_value_get_enum (&v));
      break;
    case G_TYPE_FLAGS:
      s = flags_text (pspec->value_type, g_value_get_flags (&v));
      break;
    default:                   /* boxed, object, pointer, ...: no default cell */
      break;
  }
  g_value_unset (&v);
  if (s == NULL)
    return g_strdup (NO_DEFAULT);
  quoted = g_strdup_printf ("`%s`", s);
  g_free (s);
  return quoted;
}

static gchar *
format_double (gdouble v)
{
  gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

  return g_strdup (g_ascii_formatd (buf, sizeof (buf), "%g", v));
}

/* The range of a numeric property, its bounds formatted as defaults are (decimal, %g for floats). FALSE for other
 * types. */
static gboolean
range_cells (GParamSpec * pspec, gchar ** lo, gchar ** hi)
{
  *lo = *hi = NULL;
  if (G_IS_PARAM_SPEC_CHAR (pspec)) {
    *lo = g_strdup_printf ("%d", G_PARAM_SPEC_CHAR (pspec)->minimum);
    *hi = g_strdup_printf ("%d", G_PARAM_SPEC_CHAR (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_UCHAR (pspec)) {
    *lo = g_strdup_printf ("%u", G_PARAM_SPEC_UCHAR (pspec)->minimum);
    *hi = g_strdup_printf ("%u", G_PARAM_SPEC_UCHAR (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_INT (pspec)) {
    *lo = g_strdup_printf ("%d", G_PARAM_SPEC_INT (pspec)->minimum);
    *hi = g_strdup_printf ("%d", G_PARAM_SPEC_INT (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_UINT (pspec)) {
    *lo = g_strdup_printf ("%u", G_PARAM_SPEC_UINT (pspec)->minimum);
    *hi = g_strdup_printf ("%u", G_PARAM_SPEC_UINT (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_LONG (pspec)) {
    *lo = g_strdup_printf ("%ld", G_PARAM_SPEC_LONG (pspec)->minimum);
    *hi = g_strdup_printf ("%ld", G_PARAM_SPEC_LONG (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_ULONG (pspec)) {
    *lo = g_strdup_printf ("%lu", G_PARAM_SPEC_ULONG (pspec)->minimum);
    *hi = g_strdup_printf ("%lu", G_PARAM_SPEC_ULONG (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_INT64 (pspec)) {
    *lo = g_strdup_printf ("%" G_GINT64_FORMAT, G_PARAM_SPEC_INT64 (pspec)->minimum);
    *hi = g_strdup_printf ("%" G_GINT64_FORMAT, G_PARAM_SPEC_INT64 (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_UINT64 (pspec)) {
    *lo = g_strdup_printf ("%" G_GUINT64_FORMAT, G_PARAM_SPEC_UINT64 (pspec)->minimum);
    *hi = g_strdup_printf ("%" G_GUINT64_FORMAT, G_PARAM_SPEC_UINT64 (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_FLOAT (pspec)) {
    *lo = format_double (G_PARAM_SPEC_FLOAT (pspec)->minimum);
    *hi = format_double (G_PARAM_SPEC_FLOAT (pspec)->maximum);
  } else if (G_IS_PARAM_SPEC_DOUBLE (pspec)) {
    *lo = format_double (G_PARAM_SPEC_DOUBLE (pspec)->minimum);
    *hi = format_double (G_PARAM_SPEC_DOUBLE (pspec)->maximum);
  }
  return *lo != NULL;
}

/* One number at *p: an optional sign ('-' or U+2212, returned as '-'), then digits, '.', 'e', 'E' and a sign after
 * an exponent mark. Advances *p; NULL when there are no digits. */
static gchar *
take_number (const gchar ** p)
{
  GString *n = g_string_new (NULL);
  const gchar *q = *p;
  gchar prev = '\0';
  gboolean digits = FALSE;

  if (*q == '-') {
    g_string_append_c (n, '-');
    q++;
  } else if (g_str_has_prefix (q, "\xe2\x88\x92")) {
    g_string_append_c (n, '-');
    q += 3;
  }
  for (; *q; prev = *q, q++) {
    if (g_ascii_isdigit (*q))
      digits = TRUE;
    else if (!(*q == '.' || *q == 'e' || *q == 'E' || ((*q == '+' || *q == '-') && (prev == 'e' || prev == 'E'))))
      break;
    g_string_append_c (n, *q);
  }
  if (!digits) {
    g_string_free (n, TRUE);
    return NULL;
  }
  *p = q;
  return g_string_free (n, FALSE);
}

typedef enum
{
  RANGE_NONE,                   /* the cell doesn't start with a range */
  RANGE_FOUND,
  RANGE_HYPHEN,                 /* "N-M": a range written with an ASCII hyphen */
} RangeParse;

/* A Values cell that starts with "A–B" (en dash, spaces allowed around it; '-' or U+2212 as the minus sign): its
 * bounds, with any U+2212 written as '-'. RANGE_HYPHEN for "N-M" (a digit on both sides of an ASCII hyphen). */
static RangeParse
parse_range (const gchar * cell, gchar ** lo, gchar ** hi)
{
  const gchar *p = cell;

  *lo = *hi = NULL;
  if ((*lo = take_number (&p)) == NULL)
    return RANGE_NONE;
  if (p[0] == '-' && g_ascii_isdigit (p[1]) && p > cell && g_ascii_isdigit (p[-1])) {
    g_clear_pointer (lo, g_free);
    return RANGE_HYPHEN;
  }
  while (*p == ' ')
    p++;
  if (g_str_has_prefix (p, "\xe2\x80\x93")) {
    p += 3;
    while (*p == ' ')
      p++;
    *hi = take_number (&p);
  }
  if (*hi == NULL)
    g_clear_pointer (lo, g_free);
  return *lo != NULL ? RANGE_FOUND : RANGE_NONE;
}

/* A bound as the pages write it: U+2212 for the minus sign. */
static gchar *
page_number (const gchar * n)
{
  return n[0] == '-' ? g_strconcat ("\xe2\x88\x92", n + 1, NULL) : g_strdup (n);
}

/* ---- checks ---- */

typedef gboolean (*SkipFunc) (GParamSpec * pspec);

static gboolean
skip_element_property (GParamSpec * pspec)
{
  return pspec->owner_type == GST_TYPE_OBJECT || pspec->owner_type == GST_TYPE_ELEMENT
      || pspec->owner_type == GST_TYPE_BIN;
}

static gboolean
skip_pad_property (GParamSpec * pspec)
{
  return pspec->owner_type == GST_TYPE_OBJECT || pspec->owner_type == GST_TYPE_PAD
      || pspec->owner_type == GST_TYPE_PROXY_PAD || pspec->owner_type == GST_TYPE_GHOST_PAD
      || pspec->owner_type == g_type_from_name ("GstAggregatorPad");
}

/* A Values cell that starts with a range "A–B" must give the property's whole range, its minimum and maximum
 * (narrower practical limits go in words after it); "N-M" with a hyphen fails. */
static void
check_range_cell (Ctx * ctx, guint line, const gchar * what, GParamSpec * pspec, const gchar * values)
{
  gchar *lo, *hi, *want_lo, *want_hi, *show_lo, *show_hi;
  RangeParse r;

  if (!range_cells (pspec, &want_lo, &want_hi))
    return;
  show_lo = page_number (want_lo);
  show_hi = page_number (want_hi);
  r = parse_range (values, &lo, &hi);
  if (r == RANGE_HYPHEN) {
    fail_at (ctx, line, "%s `%s` writes its range with a hyphen: use an en dash (\xe2\x80\x93) for a range; "
        "expected: | ... | %s\xe2\x80\x93%s | ...", what, pspec->name, show_lo, show_hi);
  } else if (r == RANGE_FOUND) {
    if (strcmp (lo, want_lo) != 0 || strcmp (hi, want_hi) != 0)
      fail_at (ctx, line, "%s `%s` has the range %s to %s; expected: | ... | %s\xe2\x80\x93%s | ...", what,
          pspec->name, lo, hi, show_lo, show_hi);
    g_free (lo);
    g_free (hi);
  }
  g_free (show_lo);
  g_free (show_hi);
  g_free (want_lo);
  g_free (want_hi);
}

/* The rows against the properties: each needed one has a row, each row names a property, and type and default
 * match. Returns the number of properties checked. */
static guint
check_props (Ctx * ctx, const gchar * what, GPtrArray * pspecs, GArray * rows, SkipFunc skip)
{
  Table t;
  guint n_checked = 0;

  table_init (ctx, &t, rows);
  for (guint i = 0; i < pspecs->len; i++) {
    GParamSpec *pspec = g_ptr_array_index (pspecs, i);
    guint line = 0;
    gchar **cells = table_take (&t, pspec->name, &line);
    gchar *type = type_cell (pspec), *def = default_cell (pspec);

    if (cells == NULL && !skip (pspec)) {
      fail_at (ctx, 0, "%s `%s` has no row; expected: | `%s` | %s | %s | ... | ... | ... |", what, pspec->name,
          pspec->name, type, def);
    } else if (cells != NULL && (strcmp (cell (cells, 1), type) != 0 || strcmp (cell (cells, 2), def) != 0)) {
      fail_at (ctx, line, "%s `%s` has type \"%s\", default \"%s\"; expected: | `%s` | %s | %s | ...", what,
          pspec->name, cell (cells, 1), cell (cells, 2), pspec->name, type, def);
    } else if (cells != NULL) {
      check_range_cell (ctx, line, what, pspec, cell (cells, 3));
    }
    if (cells != NULL || !skip (pspec))
      n_checked++;
    g_free (type);
    g_free (def);
  }
  table_report_leftovers (ctx, &t, what);
  table_clear (&t);
  return n_checked;
}

static const gchar *
presence_name (GstPadPresence p)
{
  switch (p) {
    case GST_PAD_ALWAYS:
      return "always";
    case GST_PAD_SOMETIMES:
      return "sometimes";
    default:
      return "request";
  }
}

static gboolean
is_plain_pad_type (GType t)
{
  return t == G_TYPE_NONE || t == GST_TYPE_PAD || t == GST_TYPE_GHOST_PAD
      || t == g_type_from_name ("GstAggregatorPad");
}

/* The pad rows, then the pad-property rows of the templates' own pad types. Returns the pads checked. */
static guint
check_pads (Ctx * ctx, const Page * page, GstElementClass * klass)
{
  GArray *rows = g_array_new (FALSE, FALSE, sizeof (Row)), *prop_rows = g_array_new (FALSE, FALSE, sizeof (Row));
  GArray *pad_types = g_array_new (FALSE, FALSE, sizeof (GType));
  GPtrArray *pspecs = g_ptr_array_new ();
  GPtrArray *pad_classes = g_ptr_array_new_with_free_func (g_type_class_unref);
  Table t;
  guint n_pads = 0;

  section_rows (ctx, page, NULL, "## Pads", 2, pad_columns, rows);
  table_init (ctx, &t, rows);
  for (GList * l = gst_element_class_get_pad_template_list (klass); l; l = l->next) {
    GstPadTemplate *templ = l->data;
    const gchar *name = GST_PAD_TEMPLATE_NAME_TEMPLATE (templ);
    const gchar *dir = GST_PAD_TEMPLATE_DIRECTION (templ) == GST_PAD_SRC ? "src" : "sink";
    const gchar *presence = presence_name (GST_PAD_TEMPLATE_PRESENCE (templ));
    guint line = 0;
    gchar **cells = table_take (&t, name, &line);
    GType pad_type = GST_PAD_TEMPLATE_GTYPE (templ);
    gboolean seen = FALSE;

    n_pads++;
    if (cells == NULL) {
      fail_at (ctx, 0, "pad template `%s` has no row; expected: | `%s` | %s | %s | ... |", name, name, dir,
          presence);
    } else if (strcmp (cell (cells, 1), dir) != 0 || strcmp (cell (cells, 2), presence) != 0) {
      fail_at (ctx, line, "pad `%s` has direction \"%s\", presence \"%s\"; expected: | `%s` | %s | %s | ... |", name,
          cell (cells, 1), cell (cells, 2), name, dir, presence);
    }

    if (is_plain_pad_type (pad_type))
      continue;
    for (guint i = 0; i < pad_types->len; i++)
      seen |= g_array_index (pad_types, GType, i) == pad_type;
    if (!seen) {
      /* the class holds the pspecs: kept until they are checked */
      GObjectClass *pad_class = g_type_class_ref (pad_type);
      guint n;
      GParamSpec **props = g_object_class_list_properties (pad_class, &n);

      g_ptr_array_add (pad_classes, pad_class);
      g_array_append_val (pad_types, pad_type);
      for (guint i = 0; i < n; i++) {
        gboolean dup = FALSE;

        for (guint k = 0; k < pspecs->len; k++)
          dup |= strcmp (((GParamSpec *) g_ptr_array_index (pspecs, k))->name, props[i]->name) == 0;
        if (!dup)
          g_ptr_array_add (pspecs, props[i]);
      }
      g_free (props);
    }
  }
  table_report_leftovers (ctx, &t, "pad template");
  table_clear (&t);

  /* with no pad type of its own, a "Pad properties" table names properties that don't exist: still reported */
  section_rows (ctx, page, NULL, "## Pad properties", 2, prop_columns, prop_rows);
  check_props (ctx, "pad property", pspecs, prop_rows, skip_pad_property);

  g_ptr_array_unref (pspecs);
  g_ptr_array_unref (pad_classes);
  g_array_unref (pad_types);
  g_array_unref (prop_rows);
  g_array_unref (rows);
  return n_pads;
}

/* The signals of one type (an instance type or an interface): those of a GstRds* type need a row; a row for another
 * is allowed. Returns the signals checked. */
static guint
check_type_signals (Ctx * ctx, GType ty, Table * t)
{
  guint n_ids, n_checked = 0;
  guint *ids = g_signal_list_ids (ty, &n_ids);

  for (guint i = 0; i < n_ids; i++) {
    GSignalQuery q;
    guint line = 0;
    gboolean has_row;

    g_signal_query (ids[i], &q);
    has_row = table_take (t, q.signal_name, &line) != NULL;
    if (has_row || g_str_has_prefix (g_type_name (q.itype), "GstRds"))
      n_checked++;
    if (!has_row && g_str_has_prefix (g_type_name (q.itype), "GstRds"))
      fail_at (ctx, 0, "signal `%s` has no row; expected: | `%s` | ... | ... | ... |", q.signal_name,
          q.signal_name);
  }
  g_free (ids);
  return n_checked;
}

/* Every signal of a GstRds* type the element derives from has a row; a row for any other signal of the element's
 * types and interfaces is allowed; a row naming no signal of them fails. */
static guint
check_signals (Ctx * ctx, const Page * page, GType type)
{
  GArray *rows = g_array_new (FALSE, FALSE, sizeof (Row));
  Table t;
  guint n_signals = 0, n_ifaces;
  GType *ifaces;

  section_rows (ctx, page, NULL, "## Signals", 2, signal_columns, rows);
  table_init (ctx, &t, rows);
  for (GType ty = type; ty != 0; ty = g_type_parent (ty))
    n_signals += check_type_signals (ctx, ty, &t);
  /* the leaf type's interfaces include those its parents implement */
  ifaces = g_type_interfaces (type, &n_ifaces);
  for (guint i = 0; i < n_ifaces; i++)
    n_signals += check_type_signals (ctx, ifaces[i], &t);
  g_free (ifaces);
  table_report_leftovers (ctx, &t, "signal");
  table_clear (&t);
  g_array_unref (rows);
  return n_signals;
}

/* whether no earlier entry of the table has this page (a page shared by two factories is read once for checks of
 * the page itself) */
static gboolean
first_entry_of_page (guint e)
{
  for (guint k = 0; k < e; k++)
    if (strcmp (rds_element_pages[k].page, rds_element_pages[e].page) == 0)
      return FALSE;
  return TRUE;
}

static void
test_pages (void)
{
  guint n_bad = 0;

  for (guint e = 0; e < G_N_ELEMENTS (rds_element_pages); e++) {
    const RdsElementPage *ep = &rds_element_pages[e];
    Ctx ctx = { ep->page, ep->factory, 0 };
    gchar *path = g_build_filename (RDS_DOCS_ELEMENTS, ep->page, NULL);
    gchar *only = g_strdup_printf ("### Only on `%s`", ep->factory);
    GstElement *el = gst_element_factory_make (ep->factory, NULL);
    GArray *rows;
    GPtrArray *pspecs;
    GParamSpec **props;
    guint n_props, n_checked, n_pads, n_signals;
    Page page;

    if (el == NULL) {
      if (!ep->optional)
        fail_at (&ctx, 0, "%s is not built", ep->factory);
      else
        g_test_message ("%s is not built: its page is not checked", ep->factory);
      goto next;
    }
    if (!read_page (path, &page)) {
      fail_at (&ctx, 0, "cannot read %s", path);
      gst_object_unref (el);
      goto next;
    }

    if (first_entry_of_page (e))
      check_only_on_headings (&ctx, &page);
    rows = g_array_new (FALSE, FALSE, sizeof (Row));
    if (!section_rows (&ctx, &page, NULL, "## Properties", 2, prop_columns, rows))
      fail_at (&ctx, 0, "no \"## Properties\" heading");
    section_rows (&ctx, &page, "## Properties", only, 3, prop_columns, rows);
    pspecs = g_ptr_array_new ();
    props = g_object_class_list_properties (G_OBJECT_GET_CLASS (el), &n_props);
    for (guint i = 0; i < n_props; i++)
      g_ptr_array_add (pspecs, props[i]);
    n_checked = check_props (&ctx, "property", pspecs, rows, skip_element_property);
    n_pads = check_pads (&ctx, &page, GST_ELEMENT_GET_CLASS (el));
    n_signals = check_signals (&ctx, &page, G_OBJECT_TYPE (el));
    g_test_message ("%s (%s): %u properties, %u pads, %u signals checked, %u failures", ep->page, ep->factory,
        n_checked, n_pads, n_signals, ctx.n_bad);

    g_free (props);
    g_ptr_array_unref (pspecs);
    g_array_unref (rows);
    page_clear (&page);
    gst_object_unref (el);
  next:
    n_bad += ctx.n_bad;
    g_free (only);
    g_free (path);
  }
  g_assert_cmpuint (n_bad, ==, 0);
}

/* ---- links ---- */

/* One link target: "<path>" or "path" with an optional title; NULL when it is not a relative file path (empty,
 * in-page, or with a "scheme:" prefix, a ':' before any '/'). A #anchor is dropped. */
static gchar *
link_path (const gchar * raw)
{
  gchar *target;
  gchar *cut, *colon;

  while (g_ascii_isspace (*raw))
    raw++;
  if (*raw == '<') {
    const gchar *close = strchr (raw + 1, '>');

    target = close ? g_strndup (raw + 1, close - raw - 1) : g_strdup (raw + 1);
  } else {
    target = g_strdup (raw);
    if ((cut = strpbrk (target, " \t")))
      *cut = '\0';             /* a title: ](x.md "title") */
  }
  if ((cut = strchr (target, '#')))
    *cut = '\0';
  colon = strchr (target, ':');
  cut = strchr (target, '/');
  if (target[0] == '\0' || (colon != NULL && (cut == NULL || colon < cut))) {
    g_free (target);
    return NULL;
  }
  return target;
}

static guint
check_target (const gchar * path, const gchar * dir, guint line, const gchar * raw, guint * n_links)
{
  gchar *target = link_path (raw), *full;
  guint n_bad = 0;

  if (target == NULL)
    return 0;
  full = g_build_filename (dir, target, NULL);
  (*n_links)++;
  if (!g_file_test (full, G_FILE_TEST_EXISTS)) {
    g_test_message ("%s:%u: link target %s does not exist", path, line, target);
    n_bad = 1;
  }
  g_free (full);
  g_free (target);
  return n_bad;
}

/* Relative link targets in a Markdown file resolve, from the file's directory: inline links "](target)", with the
 * target in angle brackets or not, and reference definitions "[label]: target" (not footnotes, "[^label]:"). Links
 * in fences and in inline code are not read; links with a scheme (http:, mailto:, ...) and in-page (#) links are
 * skipped, and a target's #anchor is dropped. */
static guint
check_links_in (const gchar * path, guint * n_links)
{
  Page page;
  gchar *dir = g_path_get_dirname (path);
  guint n_bad = 0;

  g_assert_true (read_page (path, &page));
  for (guint i = 0; page.lines[i]; i++) {
    const gchar *l = page.lines[i];
    gboolean code = FALSE;

    if (page.in_fence[i])
      continue;

    /* a reference definition: up to 3 spaces, "[label]:", the target */
    for (guint indent = 0; *l == ' ' && indent < 3; indent++)
      l++;
    if (*l == '[') {
      const gchar *close = strchr (l, ']');

      if (close != NULL && close[1] == ':' && memchr (l, '`', close - l) == NULL) {
        if (l[1] != '^')
          n_bad += check_target (path, dir, i + 1, close + 2, n_links);
        continue;
      }
    }

    for (const gchar * p = page.lines[i]; *p; p++) {
      const gchar *end;
      gchar *raw;

      if (*p == '`')
        code = !code;
      if (code || p[0] != ']' || p[1] != '(')
        continue;
      end = p[2] == '<' && strchr (p + 3, '>') ? strchr (strchr (p + 3, '>'), ')') : strchr (p + 2, ')');
      if (end == NULL)
        continue;
      raw = g_strndup (p + 2, end - p - 2);
      n_bad += check_target (path, dir, i + 1, raw, n_links);
      g_free (raw);
      p = end;
    }
  }
  page_clear (&page);
  g_free (dir);
  return n_bad;
}

static guint
check_links_in_dir (const gchar * dir_path, guint * n_files, guint * n_links)
{
  GDir *dir = g_dir_open (dir_path, 0, NULL);
  const gchar *name;
  guint n_bad = 0;

  g_assert_nonnull (dir);
  while ((name = g_dir_read_name (dir))) {
    gchar *path;

    if (!g_str_has_suffix (name, ".md"))
      continue;
    path = g_build_filename (dir_path, name, NULL);
    n_bad += check_links_in (path, n_links);
    (*n_files)++;
    g_free (path);
  }
  g_dir_close (dir);
  return n_bad;
}

static void
test_links (void)
{
  guint n_bad = 0, n_files = 0, n_links = 0;

  n_bad += check_links_in_dir (RDS_DOCS, &n_files, &n_links);
  n_bad += check_links_in_dir (RDS_DOCS_ELEMENTS, &n_files, &n_links);
  /* the top-level files that link into docs/ */
  for (guint i = 0; i < 2; i++) {
    gchar *path = g_build_filename (RDS_SOURCE_ROOT, i == 0 ? "README.md" : "AGENTS.md", NULL);

    n_bad += check_links_in (path, &n_links);
    n_files++;
    g_free (path);
  }
  g_test_message ("%u relative links in %u files checked", n_links, n_files);
  g_assert_cmpuint (n_bad, ==, 0);
}

/* ---- the cell formatting on its own ---- */

static void
check_cells (GParamSpec * pspec, const gchar * type, const gchar * def)
{
  gchar *t, *d;

  g_param_spec_ref_sink (pspec);
  t = type_cell (pspec);
  d = default_cell (pspec);
  g_assert_cmpstr (t, ==, type);
  g_assert_cmpstr (d, ==, def);
  g_free (t);
  g_free (d);
  g_param_spec_unref (pspec);
}

static void
check_range (GParamSpec * pspec, const gchar * lo, const gchar * hi)
{
  gchar *l = NULL, *h = NULL;
  gboolean ok;

  g_param_spec_ref_sink (pspec);
  ok = range_cells (pspec, &l, &h);
  g_assert_true (ok == (lo != NULL));
  g_assert_cmpstr (l, ==, lo);
  g_assert_cmpstr (h, ==, hi);
  g_free (l);
  g_free (h);
  g_param_spec_unref (pspec);
}

static void
check_parse (const gchar * cell, const gchar * lo, const gchar * hi)
{
  gchar *l = NULL, *h = NULL;
  RangeParse r = parse_range (cell, &l, &h);

  g_assert_cmpint (r, ==, lo != NULL ? RANGE_FOUND : RANGE_NONE);
  g_assert_cmpstr (l, ==, lo);
  g_assert_cmpstr (h, ==, hi);
  g_free (l);
  g_free (h);
}

static void
test_cells (void)
{
  const GParamFlags rw = G_PARAM_READWRITE;
  gchar *s;

  check_cells (g_param_spec_double ("d", NULL, NULL, -1, 1, 0.25, rw), "double", "`0.25`");
  check_cells (g_param_spec_float ("f", NULL, NULL, 0, 1e9, 1e7, rw), "float", "`1e+07`");
  check_cells (g_param_spec_uint64 ("u", NULL, NULL, 0, G_MAXUINT64, G_MAXUINT64, rw), "uint64",
      "`18446744073709551615`");
  check_cells (g_param_spec_int64 ("i", NULL, NULL, G_MININT64, 0, G_MININT64, rw), "int64",
      "`-9223372036854775808`");
  check_cells (g_param_spec_char ("c", NULL, NULL, -128, 127, -5, rw), "char", "`-5`");
  check_cells (g_param_spec_uchar ("uc", NULL, NULL, 0, 255, 200, rw), "uchar", "`200`");
  check_cells (g_param_spec_string ("s", NULL, NULL, "a b", rw), "string", "`\"a b\"`");
  check_cells (g_param_spec_string ("sn", NULL, NULL, NULL, rw), "string", "`NULL`");
  check_cells (g_param_spec_boolean ("b", NULL, NULL, FALSE, rw), "boolean", "`false`");
  check_cells (g_param_spec_enum ("e", NULL, NULL, GST_TYPE_STATE, GST_STATE_PAUSED, rw), "enum `GstState`",
      "`paused`");
  check_cells (g_param_spec_flags ("fl", NULL, NULL, GST_TYPE_BUFFER_FLAGS,
          GST_BUFFER_FLAG_DISCONT | GST_BUFFER_FLAG_LIVE, rw), "flags `GstBufferFlags`", "`live+discont`");
  check_cells (g_param_spec_flags ("f0", NULL, NULL, GST_TYPE_BUFFER_FLAGS, 0, rw), "flags `GstBufferFlags`",
      "`0`");
  check_cells (g_param_spec_boxed ("caps", NULL, NULL, GST_TYPE_CAPS, rw), "`GstCaps`", NO_DEFAULT);
  check_cells (g_param_spec_object ("o", NULL, NULL, GST_TYPE_ELEMENT, rw), "`GstElement`", NO_DEFAULT);
  check_cells (g_param_spec_int ("w", NULL, NULL, 0, 10, 3, G_PARAM_WRITABLE), "int", NO_DEFAULT);

  check_range (g_param_spec_uint ("r1", NULL, NULL, 1, G_MAXUINT, 1, rw), "1", "4294967295");
  check_range (g_param_spec_int ("r2", NULL, NULL, G_MININT, G_MAXINT, 0, rw), "-2147483648", "2147483647");
  check_range (g_param_spec_int64 ("r3", NULL, NULL, -1, G_MAXINT64, 0, rw), "-1", "9223372036854775807");
  check_range (g_param_spec_uint64 ("r4", NULL, NULL, 0, G_MAXUINT64, 0, rw), "0", "18446744073709551615");
  check_range (g_param_spec_float ("r5", NULL, NULL, 0, 1, 0, rw), "0", "1");
  check_range (g_param_spec_double ("r6", NULL, NULL, -0.5, 1e9, 0, rw), "-0.5", "1e+09");
  check_range (g_param_spec_boolean ("r7", NULL, NULL, FALSE, rw), NULL, NULL);
  check_range (g_param_spec_string ("r8", NULL, NULL, NULL, rw), NULL, NULL);
#define EN_DASH "\xe2\x80\x93"
#define MINUS "\xe2\x88\x92"
  check_parse ("0" EN_DASH "4294967295", "0", "4294967295");
  check_parse (MINUS "1" EN_DASH "30, more", "-1", "30");
  check_parse ("-2147483648 " EN_DASH " 2147483647", "-2147483648", "2147483647");
  check_parse ("0" EN_DASH "1e+09", "0", "1e+09");
  check_parse ("the uint64 range", NULL, NULL);
  check_parse ("`0` `fast`, `1` `exact`", NULL, NULL);
  check_parse ("-1", NULL, NULL);
  check_parse ("1e-5" EN_DASH "2", "1e-5", "2");
  check_parse ("+1" EN_DASH "2", NULL, NULL);       /* a leading '+' is no number: nothing read before the cell */
  {
    gchar *l = NULL, *h = NULL;

    /* a hyphen between digits is a range written wrongly: reported, not skipped */
    g_assert_cmpint (parse_range ("0-30", &l, &h), ==, RANGE_HYPHEN);
    g_assert_cmpint (parse_range ("-1-30", &l, &h), ==, RANGE_HYPHEN);
    g_assert_null (l);
    g_assert_null (h);
  }
#undef EN_DASH
#undef MINUS
  check_parse ("", NULL, NULL);

  /* values a GParamSpec refuses as a default, but a class could still produce */
  s = enum_text (GST_TYPE_STATE, 42);
  g_assert_cmpstr (s, ==, "42");
  g_free (s);
  s = flags_text (GST_TYPE_BUFFER_FLAGS, GST_BUFFER_FLAG_LIVE | (1u << 31));
  g_assert_cmpstr (s, ==, "live+0x80000000");
  g_free (s);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/elements/doc-cells", test_cells);
  g_test_add_func ("/cpu/elements/docs", test_pages);
  g_test_add_func ("/cpu/elements/doc-links", test_links);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
