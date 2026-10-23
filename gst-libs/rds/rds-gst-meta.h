/* Redstream: the GstMeta that carries an RdsBatchMeta on a buffer.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_GST_META_H
#define RDS_GST_META_H

#include <gst/gst.h>
#include <rds/rds-prelude.h>
#include <rds/rds-meta.h>

G_BEGIN_DECLS

typedef struct
{
  GstMeta meta;
  gpointer meta_data;           /* RdsBatchMeta * when meta_type == RDS_BATCH_GST_META */
  gpointer user_data;
  RdsMetaType meta_type;
} RdsMeta;

RDS_API GType rds_meta_api_get_type (void);
#define RDS_META_API_TYPE (rds_meta_api_get_type ())
RDS_API const GstMetaInfo *rds_meta_get_info (void);

/* Attaches batch_meta to a writable buffer and takes ownership of it (it is destroyed with the meta). Copying the
 * buffer (gst_buffer_copy, gst_buffer_make_writable) deep-copies it; other transforms drop it. On failure (NULL
 * return) ownership stays with the caller. */
RDS_API RdsMeta *rds_buffer_add_batch_meta (GstBuffer * buffer, RdsBatchMeta * batch_meta);
/* The buffer's batch meta, or NULL. Transfer none. */
RDS_API RdsBatchMeta *gst_buffer_get_rds_batch_meta (GstBuffer * buffer);

G_END_DECLS

#endif /* RDS_GST_META_H */
