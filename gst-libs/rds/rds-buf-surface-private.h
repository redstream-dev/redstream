/* Private RdsBufSurface helpers shared inside libgstrds. Not installed, not exported.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_BUF_SURFACE_PRIVATE_H
#define RDS_BUF_SURFACE_PRIVATE_H

#include <gst/gst.h>
#include "rds-buf-surface.h"

G_BEGIN_DECLS

#define RDS_BUF_SURFACE_PRIV_OWNED 0x5244534fu    /* "RDSO" */
#define RDS_BUF_SURFACE_PRIV_VIEW 0x52445356u     /* "RDSV" */

/* What _priv points at. An owned surface (rds_buf_surface_create) owns base and its staging mirror. A view shows
 * one slot of another buffer's batch: base is NULL, dataPtr points into the parent's allocation, and CPU maps of
 * device memory use the owner's staging mirror (slot @index). The parent buffer ref is held by the view's RDS
 * memory (rds-memory.c), which also owns the view's RdsBufSurface, params and this struct; that ref keeps @owner
 * alive (a buffer with views is not writable, so its memory cannot be replaced). */
typedef struct _RdsBufSurfacePriv RdsBufSurfacePriv;
struct _RdsBufSurfacePriv
{
  guint32 magic;                /* RDS_BUF_SURFACE_PRIV_OWNED or RDS_BUF_SURFACE_PRIV_VIEW */
  gpointer base;                /* owned: one allocation for the whole batch */
  gsize stride;                 /* owned: bytes between consecutive surfaces */
  guint32 batch_size;           /* owned: surfaces in base (and in the staging mirror) */
  guint8 *staging;              /* owned: pinned mirror of base for mapping device memory; lazily allocated, set
                                 * atomically (views of one batch may map it from several threads) */
  GstBuffer *parent;            /* view: the buffer holding the batch (a ref) */
  RdsBufSurfacePriv *owner;     /* view: the batch surface's private struct (its staging mirror is shared) */
  guint index;                  /* view: the slot it shows */
};

/* TRUE if @surf is a view (its _priv is a view's). */
G_GNUC_INTERNAL gboolean rds_buf_surface_is_view (const RdsBufSurface * surf);
/* Fills @view as a single-surface view of slot @index of @src, with @params and @priv as its storage. @src may itself
 * be a view (index 0). Takes no references: the caller sets priv->parent. */
G_GNUC_INTERNAL void rds_buf_surface_init_view (RdsBufSurface * view, RdsBufSurfaceParams * params,
    RdsBufSurfacePriv * priv, const RdsBufSurface * src, guint index);
/* Invalidates a view before its memory frees it (rds_buf_surface_destroy refuses views). The view owns no HIP
 * resources: its staging mirror belongs to the batch. */
G_GNUC_INTERNAL void rds_buf_surface_release_view (RdsBufSurface * view);

/* TRUE for the memory types this library can allocate (not the Jetson-only ones, nor values outside the enum). */
G_GNUC_INTERNAL gboolean rds_buf_mem_type_is_supported (RdsBufSurfaceMemType mem_type);

/* Makes @gpu the current HIP device and stores the previous one in @prev. Only for HIP memory. */
G_GNUC_INTERNAL gboolean rds_device_enter (guint32 gpu, int *prev);
/* Restores the device saved by a successful rds_device_enter (). */
G_GNUC_INTERNAL void rds_device_leave (guint32 gpu, int prev);

G_END_DECLS

#endif /* RDS_BUF_SURFACE_PRIVATE_H */
