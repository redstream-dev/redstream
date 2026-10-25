/* Redstream: value parsing shared by the config parsers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-config-parse.h"

#include <errno.h>
#include <math.h>
#include <string.h>

gboolean
rds_config_parse_bool (const gchar * s, gboolean * out)
{
  if (!strcmp (s, "1") || !g_ascii_strcasecmp (s, "true"))
    *out = TRUE;
  else if (!strcmp (s, "0") || !g_ascii_strcasecmp (s, "false"))
    *out = FALSE;
  else
    return FALSE;
  return TRUE;
}

const gchar *
rds_config_format_number (gchar * buf, gdouble v)
{
  return g_ascii_formatd (buf, G_ASCII_DTOSTR_BUF_SIZE, "%.10g", v);
}

gboolean
rds_config_parse_number (const gchar * s, gboolean is_int, gdouble min, gboolean min_exclusive, gdouble max,
    gdouble * out, gchar ** reason)
{
  gchar *end = NULL, buf[G_ASCII_DTOSTR_BUF_SIZE];
  gdouble v;

  errno = 0;
  if (is_int) {
    gint64 i = g_ascii_strtoll (s, &end, 10);

    if (!*s || *end) {
      *reason = g_strdup ("not an integer");
      return FALSE;
    }
    if (errno == ERANGE) {
      *reason = g_strdup ("out of range");
      return FALSE;
    }
    v = (gdouble) i;
  } else {
    v = g_ascii_strtod (s, &end);
    if (!*s || *end) {
      *reason = g_strdup ("not a number");
      return FALSE;
    }
    /* overflow (1e400 reads as inf) and underflow are range errors; only a written nan or inf is not finite.
     * Floats are stored as gfloat: values it can't hold (including ones that would become 0) are out of range. */
    if (errno == ERANGE || (isfinite (v) && (fabs (v) > G_MAXFLOAT || (v != 0 && fabs (v) < G_MINFLOAT)))) {
      *reason = g_strdup ("out of range for a 32-bit float");
      return FALSE;
    }
    if (!isfinite (v)) {
      *reason = g_strdup ("not a finite number");
      return FALSE;
    }
  }
  if (min_exclusive ? v <= min : v < min) {
    *reason = g_strdup_printf ("must be %s %s", min_exclusive ? "greater than" : "at least",
        rds_config_format_number (buf, min));
    return FALSE;
  }
  if (v > max) {
    *reason = g_strdup_printf ("must be at most %s", rds_config_format_number (buf, max));
    return FALSE;
  }
  *out = v;
  return TRUE;
}
