/* Redstream: rdsinfer's device switch, the calling thread's GPU for a scope (internal; includes HIP).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_DEVICE_H
#define RDS_INFER_DEVICE_H

#include <glib.h>
/* after GLib: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>

/* Makes @gpu_id the calling thread's device and stores the previous one in *@prev. FALSE (nothing changed) if either
 * HIP call fails; after TRUE, rds_infer_device_restore (*@prev) switches back. */
static inline gboolean
rds_infer_device_enter (guint gpu_id, int *prev)
{
  return hipGetDevice (prev) == hipSuccess && hipSetDevice ((int) gpu_id) == hipSuccess;
}

static inline void
rds_infer_device_restore (int prev)
{
  (void) hipSetDevice (prev);
}

#endif /* RDS_INFER_DEVICE_H */
