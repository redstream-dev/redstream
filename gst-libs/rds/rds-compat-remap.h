/* Redstream compat: map DeepStream element factory names to Redstream ones.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_COMPAT_REMAP_H
#define RDS_COMPAT_REMAP_H

#include <gst/gst.h>

G_BEGIN_DECLS

/* Returns the Redstream factory name for a DeepStream one, or @factory_name unchanged.
 * Remapping is disabled when the RDS_COMPAT_NO_REMAP environment variable is set. */
const gchar *rds_compat_map_factory_name (const gchar * factory_name);

/* Rewrites whole-token factory names in a gst-launch description. Returns a newly allocated string. */
gchar *rds_compat_rewrite_launch (const gchar * description);

GstElement *rds_compat_element_factory_make (const gchar * factoryname, const gchar * name);

GstElement *rds_compat_parse_launch (const gchar * pipeline_description, GError ** error);

G_END_DECLS

#endif /* RDS_COMPAT_REMAP_H */
