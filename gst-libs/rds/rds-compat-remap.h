/* Redstream compat: map DeepStream element factory names to Redstream ones.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_COMPAT_REMAP_H
#define RDS_COMPAT_REMAP_H

#include <gst/gst.h>
#include <rds/rds-prelude.h>

G_BEGIN_DECLS

/* Returns the Redstream factory name for a DeepStream one, or @factory_name unchanged.
 * Remapping is disabled when the RDS_COMPAT_NO_REMAP environment variable is set to any
 * value (even "0" or the empty string). */
RDS_API
const gchar *rds_compat_map_factory_name (const gchar * factory_name);

/* Rewrites whole-token factory names in a gst-launch description. Returns a newly allocated
 * string, or NULL if @description is NULL. A token is delimited by whitespace, '!' and the
 * start/end of the string. Quoted values and elements that come directly after '(' or ','
 * are not rewritten. RDS_COMPAT_NO_REMAP disables rewriting, as for
 * rds_compat_map_factory_name(). */
RDS_API
gchar *rds_compat_rewrite_launch (const gchar * description);

RDS_API
GstElement *rds_compat_element_factory_make (const gchar * factoryname, const gchar * name);

RDS_API
GstElement *rds_compat_parse_launch (const gchar * pipeline_description, GError ** error);

G_END_DECLS

#endif /* RDS_COMPAT_REMAP_H */
