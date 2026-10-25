/* Redstream: reads a YAML config into a GKeyFile (plugin-internal, not installed).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_KEYFILE_YAML_H
#define RDS_KEYFILE_YAML_H

#include <glib.h>

G_BEGIN_DECLS

/* The top-level mapping's keys become groups; each group is a mapping of key to a scalar or a sequence of scalars.
 * Scalars become strings as written; sequences are joined with ';' (the key-file list separator). Anything nested
 * deeper is an error. Errors are G_FILE_ERROR (can't read @path) or G_KEY_FILE_ERROR_PARSE, with "PATH:LINE: "
 * in front of the message, like g_key_file_load_from_file. Only the first document is read. */
G_GNUC_INTERNAL GKeyFile *rds_keyfile_from_yaml (const gchar * path, GError ** error);

G_END_DECLS

#endif /* RDS_KEYFILE_YAML_H */
