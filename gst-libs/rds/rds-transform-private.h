/* Private declarations shared by the C side of libgstrds and its HIP kernel file (rds-transform-kernels.hip).
 * Plain C, no GLib and no HIP types (streams are void *), so both compilers can include it.
 * Not installed, not exported.
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

#endif /* RDS_TRANSFORM_PRIVATE_H */
