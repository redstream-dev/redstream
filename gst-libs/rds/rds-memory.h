/* Redstream: the memory:RDS caps feature, its GstAllocator, and GstMemory wrapping an RdsBufSurface.
 * Mapping an RDS memory yields the RdsBufSurface pointer, not pixels (as NVMM does in DeepStream).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_MEMORY_H
#define RDS_MEMORY_H

#include <gst/gst.h>
#include <gst/video/video.h>
#include <rds/rds-prelude.h>
#include <rds/rds-buf-surface.h>

G_BEGIN_DECLS

#define RDS_CAPS_FEATURE_MEMORY_RDS "memory:RDS"
#define RDS_MEMORY_TYPE "RdsMemory"

#define RDS_TYPE_ALLOCATOR (rds_allocator_get_type ())
RDS_API G_DECLARE_FINAL_TYPE (RdsAllocator, rds_allocator, RDS, ALLOCATOR, GstAllocator)

/* An allocator for one GPU and memory type. Returns a new reference. */
RDS_API GstAllocator *rds_allocator_new (guint gpu_id, RdsBufSurfaceMemType mem_type);
/* Allocates a batch surface; params->gpuId and params->memType are taken from the allocator.
 * gst_allocator_alloc() is not supported (it carries no geometry) and returns NULL.
 * Buffers copied with gst_buffer_copy() share the surface; gst_memory_copy() (and gst_buffer_copy_deep())
 * duplicate it. Sub-region shares, partial copies and resized memories are not supported (map and copy fail).
 * Copy-on-write: a GST_MAP_WRITE map of a shared RDS memory (a buffer made writable while another buffer still
 * refs the memory) makes GStreamer swap a private deep copy into the buffer, i.e. a full surface copy.
 * Copies, including that copy-on-write copy, duplicate only the first numFilled surfaces of the batch, so a
 * producer must set numFilled before pushing.
 * A buffer must hold exactly one RDS memory: mapping a buffer with several (gst_buffer_map merges them) yields
 * a system-memory copy of the RdsBufSurface structs whose pointers dangle once the merged memory is freed. */
RDS_API GstMemory *rds_allocator_alloc_surface (GstAllocator * allocator, guint32 batch_size,
    const RdsBufSurfaceCreateParams * params);

RDS_API gboolean rds_is_rds_memory (GstMemory * mem);
/* The surface inside an RDS memory, or NULL. Transfer none. */
RDS_API RdsBufSurface *rds_memory_get_surface (GstMemory * mem);
/* The surface of a buffer holding exactly one RDS memory, or NULL. Transfer none. */
RDS_API RdsBufSurface *rds_buffer_get_surface (GstBuffer * buffer);
/* TRUE if the first caps structure carries the memory:RDS feature. */
RDS_API gboolean rds_caps_has_memory_rds (const GstCaps * caps);

RDS_API RdsBufSurfaceColorFormat rds_buf_color_format_from_video_format (GstVideoFormat format);
RDS_API GstVideoFormat rds_video_format_from_buf_color_format (RdsBufSurfaceColorFormat format);

G_END_DECLS

#endif /* RDS_MEMORY_H */
