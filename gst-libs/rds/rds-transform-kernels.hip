/* Redstream: HIP kernels for libgstrds, compiled by amdclang++ and linked into the C library as an object.
 * C-compatible C++ only: no exceptions, no STL. Entry points are declared in rds-transform-private.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <hip/hip_runtime.h>

#include "rds-transform-private.h"

#define RDS_SMOKE_BLOCK 256

__global__ static void
rds_smoke_fill_kernel (unsigned char *dst, unsigned n, unsigned char value)
{
  unsigned i = blockIdx.x * blockDim.x + threadIdx.x;

  if (i < n)
    dst[i] = value;
}

extern "C" int
rds_transform_kernel_smoke (void *dev, unsigned n, unsigned char value, void *stream)
{
  unsigned blocks = (n + RDS_SMOKE_BLOCK - 1) / RDS_SMOKE_BLOCK;

  if (dev == NULL)
    return -1;
  if (n == 0)
    return 0;

  (void) hipGetLastError ();    /* clear a stale error from an earlier call */
  hipLaunchKernelGGL (rds_smoke_fill_kernel, dim3 (blocks), dim3 (RDS_SMOKE_BLOCK), 0, (hipStream_t) stream,
      (unsigned char *) dev, n, value);
  return hipGetLastError () == hipSuccess ? 0 : -1;
}
