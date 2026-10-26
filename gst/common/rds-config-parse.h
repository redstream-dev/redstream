/* Redstream: value parsing shared by the config parsers (plugin-internal, not installed).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_CONFIG_PARSE_H
#define RDS_CONFIG_PARSE_H

#include <glib.h>

G_BEGIN_DECLS

/* "1"/"0" or true/false in any case. */
G_GNUC_INTERNAL gboolean rds_config_parse_bool (const gchar * s, gboolean * out);

/* Parses @s as a whole number (@is_int, base 10) or a float stored as a 32-bit float, and checks it against @min
 * (exclusive with @min_exclusive) and @max. On failure @reason (g_free) says why: "not an integer", "not a number",
 * "not a finite number" (nan, inf), "out of range" (an integer that overflows 64 bits), "out of range for a 32-bit
 * float" (overflow or underflow), "must be at least / greater than / at most N". */
G_GNUC_INTERNAL gboolean rds_config_parse_number (const gchar * s, gboolean is_int, gdouble min,
    gboolean min_exclusive, gdouble max, gdouble * out, gchar ** reason);

/* Locale-independent "%.10g" into @buf (G_ASCII_DTOSTR_BUF_SIZE bytes); returns @buf. */
G_GNUC_INTERNAL const gchar *rds_config_format_number (gchar * buf, gdouble v);

G_END_DECLS

#endif /* RDS_CONFIG_PARSE_H */
