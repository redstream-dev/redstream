/* Private declarations shared by the C side of libgstrds and its HIP kernel file (rds-transform-kernels.hip).
 * The shared part is plain C, no GLib and no HIP types (streams are void *), so both compilers can include it;
 * the C-only part at the end is hidden from the HIP compiler. Not installed, not exported.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TRANSFORM_PRIVATE_H
#define RDS_TRANSFORM_PRIVATE_H

#define RDS_TRANSFORM_INTERNAL __attribute__ ((visibility ("hidden")))

#ifdef __cplusplus
extern "C" {
#endif

/* Placeholder launcher: fills n bytes at the device pointer dev with value, asynchronously on stream
 * (a hipStream_t, NULL for the null stream). Returns 0 when the launch succeeded and -1 when it did not
 * (for instance when the library was built for a different GPU architecture). */
RDS_TRANSFORM_INTERNAL int rds_transform_kernel_smoke (void *dev, unsigned n, unsigned char value, void *stream);

#ifdef __cplusplus
}
#endif

#ifndef __HIP__
#include "rds-transform.h"

/* Checks the arguments of rds_buf_surf_transform () against the calling thread's session, in the documented
 * order, without touching pixels or HIP. Returns Success when the call may go ahead, else the error the
 * transform reports. Tests reach it through the static internal library. */
RDS_TRANSFORM_INTERNAL RdsBufSurfTransform_Error rds_transform_validate (const RdsBufSurface * src,
    const RdsBufSurface * dst, const RdsBufSurfTransformParams * transform_params);
#endif

#endif /* RDS_TRANSFORM_PRIVATE_H */
