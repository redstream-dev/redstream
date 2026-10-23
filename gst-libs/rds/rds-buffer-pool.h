/* Redstream: buffer pool of memory:RDS buffers (one RdsBufSurface per buffer), and allocation-query helpers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_BUFFER_POOL_H
#define RDS_BUFFER_POOL_H

#include <rds/rds-memory.h>

G_BEGIN_DECLS

#define RDS_TYPE_BUFFER_POOL (rds_buffer_pool_get_type ())
RDS_API G_DECLARE_FINAL_TYPE (RdsBufferPool, rds_buffer_pool, RDS, BUFFER_POOL, GstBufferPool)

/* Returns a new reference. Configure with gst_buffer_pool_config_set_params() (video caps; the size is
 * overridden to sizeof (RdsBufSurface)) and rds_buffer_pool_config_set_surface_params(). set_config rejects
 * missing caps, unsupported formats, sizes outside 1..RDS_BUF_SURFACE_MAX_DIM, batch size 0 and unsupported
 * memory types; an invalid GPU id only fails at allocation. After a rejected set_config the pool keeps its
 * previous valid configuration (GstBufferPool behaviour).
 * Supports GST_BUFFER_POOL_OPTION_VIDEO_META (added to batch-size-1 buffers only). Its offsets and strides
 * describe the planes of the surface (possibly device memory), not the mapped data: mapping an RDS buffer yields
 * the RdsBufSurface struct, so never gst_video_frame_map() these buffers. */
RDS_API GstBufferPool *rds_buffer_pool_new (void);

/* Defaults when unset: batch_size 1, RDS_BUF_MEM_DEFAULT, GPU 0. */
RDS_API void rds_buffer_pool_config_set_surface_params (GstStructure * config, guint batch_size,
    RdsBufSurfaceMemType mem_type, guint gpu_id);
/* TRUE if all three were set; outputs are left untouched otherwise. Any output may be NULL. */
RDS_API gboolean rds_buffer_pool_config_get_surface_params (const GstStructure * config, guint * batch_size,
    RdsBufSurfaceMemType * mem_type, guint * gpu_id);

/* Downstream side (propose_allocation): always advertises GstVideoMeta and GstVideoCropMeta; when the query asks
 * for a pool and its caps carry memory:RDS, also offers a configured (inactive) RdsBufferPool. FALSE if the query
 * has no caps or the pool rejects the configuration. */
RDS_API gboolean rds_propose_allocation (GstQuery * query, guint batch_size, RdsBufSurfaceMemType mem_type,
    guint gpu_id, guint min_buffers);
/* Upstream side (decide_allocation): reuses the first inactive RdsBufferPool in the query whose surface params
 * match, otherwise creates one (also when reconfiguring the reused pool fails); merges min/max (max_buffers 0 = no
 * limit of ours); configures it, writes it back as the query's first pool (moving a matched entry rather than
 * duplicating it), and returns a new reference (inactive). The returned pool may be downstream's own pool,
 * reconfigured. NULL if the query has no caps, its caps lack memory:RDS, or configuration fails.
 * A subclass overriding decide_allocation should call this instead of chaining up; it does not filter the
 * query's metas or set allocation params. */
RDS_API GstBufferPool *rds_decide_allocation (GstQuery * query, guint batch_size,
    RdsBufSurfaceMemType mem_type, guint gpu_id, guint min_buffers, guint max_buffers);

G_END_DECLS

#endif /* RDS_BUFFER_POOL_H */
