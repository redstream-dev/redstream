/* Private RdsBufSurface helpers shared inside libgstrds. Not installed, not exported.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_BUF_SURFACE_PRIVATE_H
#define RDS_BUF_SURFACE_PRIVATE_H

#include "rds-buf-surface.h"

G_BEGIN_DECLS

/* TRUE for the memory types this library can allocate (not the Jetson-only ones, nor values outside the enum). */
G_GNUC_INTERNAL gboolean rds_buf_mem_type_is_supported (RdsBufSurfaceMemType mem_type);

/* Makes @gpu the current HIP device and stores the previous one in @prev. Only for HIP memory. */
G_GNUC_INTERNAL gboolean rds_device_enter (guint32 gpu, int *prev);
/* Restores the device saved by a successful rds_device_enter (). */
G_GNUC_INTERNAL void rds_device_leave (guint32 gpu, int prev);

G_END_DECLS

#endif /* RDS_BUF_SURFACE_PRIVATE_H */
