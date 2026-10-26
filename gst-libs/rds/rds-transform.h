/* Redstream: batched crop, scale and colour conversion between RdsBufSurfaces on the GPU. Shaped like DeepStream's
 * NvBufSurfTransform (names and enumerator values from NVIDIA's public API reference).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TRANSFORM_H
#define RDS_TRANSFORM_H

#include <rds/rds-buf-surface.h>

G_BEGIN_DECLS

typedef enum
{
  RdsBufSurfTransformCompute_Default = 0,
  RdsBufSurfTransformCompute_GPU,
  RdsBufSurfTransformCompute_VIC,               /* NVIDIA Jetson engine: rejected */
} RdsBufSurfTransform_Compute;

typedef enum
{
  RdsBufSurfTransform_None = 0,
  RdsBufSurfTransform_Rotate90,
  RdsBufSurfTransform_Rotate180,
  RdsBufSurfTransform_Rotate270,
  RdsBufSurfTransform_FlipX,
  RdsBufSurfTransform_FlipY,
  RdsBufSurfTransform_Transpose,
  RdsBufSurfTransform_InvTranspose,
} RdsBufSurfTransform_Flip;                     /* only None is supported */

typedef enum
{
  RdsBufSurfTransformInter_Nearest = 0,
  RdsBufSurfTransformInter_Bilinear,
  RdsBufSurfTransformInter_Algo1,               /* runs as bilinear */
  RdsBufSurfTransformInter_Algo2,               /* runs as bilinear */
  RdsBufSurfTransformInter_Algo3,               /* runs as bilinear */
  RdsBufSurfTransformInter_Algo4,               /* runs as nearest */
  RdsBufSurfTransformInter_Default,             /* nearest */
} RdsBufSurfTransform_Inter;

typedef enum
{
  RdsBufSurfTransformError_ROI_Error = -4,     /* a rect is empty or not inside its surface (never clamped) */
  RdsBufSurfTransformError_Invalid_Params = -3,
  RdsBufSurfTransformError_Execution_Error = -2,
  RdsBufSurfTransformError_Unsupported = -1,
  RdsBufSurfTransformError_Success = 0,
} RdsBufSurfTransform_Error;

typedef enum
{
  RDS_BUFSURF_TRANSFORM_CROP_SRC = 1,
  RDS_BUFSURF_TRANSFORM_CROP_DST = 1 << 1,
  RDS_BUFSURF_TRANSFORM_FILTER = 1 << 2,
  RDS_BUFSURF_TRANSFORM_FLIP = 1 << 3,
  RDS_BUFSURF_TRANSFORM_NORMALIZE = 1 << 4,     /* rejected */
  /* A 4:2:0 (NV12, YUV420) destination rect with an odd edge is aligned outward to even coordinates (clamped to
   * the surface). Without it such a rect is Invalid_Params; a right or bottom edge on the surface's own edge always
   * counts as even. Source rects may always be odd. */
  RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP = 1 << 5,
} RdsBufSurfTransform_Transform_Flag;

/* A rectangle in pixels of its surface: top-left corner (top, left) and size (width, height). It must be
 * non-empty and lie inside the surface, or the transform returns ROI_Error; it is never clamped. */
typedef struct
{
  guint32 top;
  guint32 left;
  guint32 width;
  guint32 height;
} RdsBufSurfTransformRect;

typedef struct
{
  guint32 transform_flag;                       /* RdsBufSurfTransform_Transform_Flag bits */
  RdsBufSurfTransform_Flip transform_flip;
  RdsBufSurfTransform_Inter transform_filter;
  RdsBufSurfTransformRect *src_rect;            /* numFilled entries, read when CROP_SRC is set */
  RdsBufSurfTransformRect *dst_rect;            /* numFilled entries, read when CROP_DST is set */
} RdsBufSurfTransformParams;

/* composite_flag bits: accepted and ignored (DeepStream 8.0 behaviour: the composite always runs and always uses
 * composite_filter). */
typedef enum
{
  RDS_BUFSURF_TRANSFORM_COMPOSITE = 1,
  RDS_BUFSURF_TRANSFORM_BLEND = 1 << 1,
  RDS_BUFSURF_TRANSFORM_COMPOSITE_FILTER = 1 << 2,
} RdsBufSurfTransform_Composite_Flag;

typedef struct
{
  guint32 composite_flag;                       /* RdsBufSurfTransform_Composite_Flag bits, ignored */
  guint32 input_buf_count;                      /* ignored: src->numFilled entries are composited */
  RdsBufSurfTransformRect *src_comp_rect;       /* src->numFilled entries, required */
  RdsBufSurfTransformRect *dst_comp_rect;       /* src->numFilled entries, required */
  RdsBufSurfTransform_Inter composite_filter;   /* always read */
  gpointer reserved[4];
} RdsBufSurfTransformCompositeParams;

typedef struct
{
  RdsBufSurfTransform_Compute compute_mode;
  gint32 gpu_id;
  gpointer cuda_stream;                         /* a hipStream_t; NULL = the default stream. Named as in
                                                 * DeepStream so ported assignments compile unchanged. */
} RdsBufSurfTransformConfigParams;

/* Session parameters are per thread: they apply to every transform called on that thread, by any caller, until
 * changed. A thread that never set them (or called set_default_session) gets GPU 0 on the default stream. */
RDS_API RdsBufSurfTransform_Error rds_buf_surf_transform_set_session_params (
    RdsBufSurfTransformConfigParams * config_params);
RDS_API RdsBufSurfTransform_Error rds_buf_surf_transform_get_session_params (
    RdsBufSurfTransformConfigParams * config_params);
RDS_API RdsBufSurfTransform_Error rds_buf_surf_transform_set_default_session (void);

/* Transforms the first src->numFilled surfaces into dst (crop, scale, colour conversion). Synchronous: returns
 * after the GPU work is complete. Both surfaces must be device-accessible (not RDS_BUF_MEM_SYSTEM), and device
 * memory (default, device) must be on the session's GPU. src and dst must not overlap (the same pixel memory on
 * both sides of an entry is Invalid_Params). Destination pixels outside dst_rect are not written.
 * dst->numFilled is set only on success; on error dst is not modified, apart from pixels if the GPU work itself
 * fails. The exact sampling and colour rules are documented in docs/compat-matrix.md. */
RDS_API RdsBufSurfTransform_Error rds_buf_surf_transform (RdsBufSurface * src, RdsBufSurface * dst,
    RdsBufSurfTransformParams * transform_params);

/* Composites the first src->numFilled surfaces into surface 0 of dst: entry i scales src_comp_rect[i] of source
 * surface i into dst_comp_rect[i], with the transform's sampling and colour rules (composite_filter as the filter).
 * Entries are drawn in order, later ones on top; destination pixels outside every dst rect are not written. Rects
 * reaching past their surface are clipped: a source rect is cut to its surface, a destination rect keeps its
 * scaling and only its part inside the surface is drawn; an empty rect, or one wholly outside, draws nothing. On a
 * 4:2:0 destination a rect's odd left or top edge is rounded up to even and an odd right or bottom edge (not on the
 * surface edge) rounded down. Synchronous, on the session's stream, with the same surface requirements as
 * rds_buf_surf_transform (dst only needs one surface); no source may share dst's pixel memory. dst->numFilled is
 * not changed. On error dst is not modified, apart from pixels if the GPU work itself fails. */
RDS_API RdsBufSurfTransform_Error rds_buf_surf_transform_composite (RdsBufSurface * src, RdsBufSurface * dst,
    RdsBufSurfTransformCompositeParams * composite_params);

G_END_DECLS

#endif /* RDS_TRANSFORM_H */
