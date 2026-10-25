/* Private declarations shared by the C side of libgstrds and its HIP kernel file (rds-transform-kernels.hip).
 * The shared part is plain C, no GLib and no HIP types (streams are void *), so both compilers can include it;
 * the C-only part at the end is hidden from the HIP compiler. The device helpers the kernels share are in
 * rds-kernel-common.h. Not installed, not exported.
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

/* RdsBufSurfaceColorFormat values, repeated for the HIP side (which doesn't include GLib); rds-transform.c checks
 * that they match. */
enum
{
  RDS_KFMT_GRAY8 = 1,
  RDS_KFMT_YUV420 = 2,
  RDS_KFMT_NV12 = 3,
  RDS_KFMT_RGBA = 4,
  RDS_KFMT_BGRA = 5,
  RDS_KFMT_RGBx = 6,
  RDS_KFMT_BGRx = 7,
  RDS_KFMT_RGB = 8,
  RDS_KFMT_BGR = 9,
};

/* One surface of a transform job. data is the device-accessible base of the surface (its dataPtr); plane p starts
 * at data + offset[p] and has rows of pitch[p] bytes. format is an RdsBufSurfaceColorFormat. */
typedef struct
{
  unsigned char *data;
  unsigned width, height;
  unsigned pitch[3], offset[3];
  int format;
} RdsKernelSurface;

/* One surface's transform: source rect (sl, st, sw, sh) into destination rect (dl, dt, dw, dh), both non-empty
 * and inside their surfaces. For a 4:2:0 destination the rect is already aligned: left and top even, right and
 * bottom even or on the surface edge. */
typedef struct
{
  RdsKernelSurface src, dst;
  unsigned sl, st, sw, sh;
  unsigned dl, dt, dw, dh;
  int bilinear;                 /* 0 nearest, 1 bilinear (after the filter mapping) */
} RdsKernelJob;

/* Launches one surface's transform asynchronously on stream (a hipStream_t, NULL for the null stream) on the
 * current device. Returns 0, or -1 if the launch failed. */
RDS_TRANSFORM_INTERNAL int rds_transform_launch (const RdsKernelJob * job, void *stream);

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

/* Test hook: TRUE makes same-format copies go through the kernel instead of hipMemcpy2DAsync, so tests can check
 * that both give the same bytes. Process-wide; FALSE restores the default. */
RDS_TRANSFORM_INTERNAL void rds_transform_force_kernel (gboolean force);

/* The filter mapping of rds_buf_surf_transform (with the FILTER flag set): Bilinear and Algo1-3 run as bilinear,
 * everything else as nearest. Inline, so code outside libgstrds (rdsinfer's preprocessing) maps the same way. */
static inline gboolean
rds_transform_filter_is_bilinear (RdsBufSurfTransform_Inter filter)
{
  switch (filter) {
    case RdsBufSurfTransformInter_Bilinear:
    case RdsBufSurfTransformInter_Algo1:
    case RdsBufSurfTransformInter_Algo2:
    case RdsBufSurfTransformInter_Algo3:
      return TRUE;
    default:
      return FALSE;
  }
}

/* The kernel view of one surface of a batch: its dataPtr, size, format and up to three planes (unused ones 0). */
static inline void
rds_kernel_surface_init (RdsKernelSurface * k, const RdsBufSurfaceParams * p)
{
  k->data = (unsigned char *) p->dataPtr;
  k->width = p->width;
  k->height = p->height;
  k->format = p->colorFormat;
  for (guint i = 0; i < 3; i++) {
    gboolean has = i < p->planeParams.num_planes;

    k->pitch[i] = has ? p->planeParams.pitch[i] : 0;
    k->offset[i] = has ? p->planeParams.offset[i] : 0;
  }
}
#endif

#endif /* RDS_TRANSFORM_PRIVATE_H */
