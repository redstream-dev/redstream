/* Redstream: buffer pool of memory:RDS buffers (one RdsBufSurface per buffer), and allocation-query helpers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_BUFFER_POOL_H
#define RDS_BUFFER_POOL_H

#include <rds/rds-memory.h>

G_BEGIN_DECLS

#define RDS_TYPE_BUFFER_POOL (rds_buffer_pool_get_type ())
RDS_API G_DECLARE_FINAL_TYPE (RdsBufferPool, rds_buffer_pool, RDS, BUFFER_POOL, GstBufferPool)

/* Returns a new reference. Configure with gst_buffer_pool_config_set_params() (video caps; the size is
 * overridden to sizeof (RdsBufSurface)) and rds_buffer_pool_config_set_surface_params(). Supports
 * GST_BUFFER_POOL_OPTION_VIDEO_META (added to batch-size-1 buffers only). */
RDS_API GstBufferPool *rds_buffer_pool_new (void);

/* Defaults when unset: batch_size 1, RDS_BUF_MEM_DEFAULT, GPU 0. */
RDS_API void rds_buffer_pool_config_set_surface_params (GstStructure * config, guint batch_size,
    RdsBufSurfaceMemType mem_type, guint gpu_id);
/* TRUE if all three were set; outputs are left untouched otherwise. Any output may be NULL. */
RDS_API gboolean rds_buffer_pool_config_get_surface_params (const GstStructure * config, guint * batch_size,
    RdsBufSurfaceMemType * mem_type, guint * gpu_id);

G_END_DECLS

#endif /* RDS_BUFFER_POOL_H */
