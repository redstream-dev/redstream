/* Redstream: helpers shared by GstRdsEncBin and its two codecs (plugin-internal).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_ENC_BIN_PRIVATE_H
#define GST_RDS_ENC_BIN_PRIVATE_H

#include <glib-object.h>

/* Defines `GType func (void)`, registering the enum @type_name with the GEnumValues given (without the terminating
 * entry) the first time it is called. Precede it with a static prototype for a file-local type. */
#define RDS_ENC_DEFINE_ENUM(func, type_name, ...)                             \
GType                                                                         \
func (void)                                                                   \
{                                                                             \
  static gsize id = 0;                                                        \
  static const GEnumValue values[] = { __VA_ARGS__, {0, NULL, NULL} };        \
                                                                              \
  if (g_once_init_enter (&id))                                                \
    g_once_init_leave (&id, g_enum_register_static (type_name, values));      \
  return (GType) id;                                                          \
}

#endif /* GST_RDS_ENC_BIN_PRIVATE_H */
