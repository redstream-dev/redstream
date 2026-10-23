/* Redstream: RdsBufSurface, a batch of pitch-linear frames in HIP device, pinned, unified or system memory.
 * Shaped like DeepStream's NvBufSurface (field names from NVIDIA's public API reference) so that code walking
 * surfaceList[]/planeParams ports by renaming.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_BUF_SURFACE_H
#define RDS_BUF_SURFACE_H

#include <glib.h>
#include <rds/rds-prelude.h>

G_BEGIN_DECLS

#define RDS_BUF_SURFACE_MAX_PLANES 4
/* Row pitch and surface spacing alignment, in bytes (matches hipMallocPitch on RDNA2). */
#define RDS_BUF_SURFACE_PITCH_ALIGN 256
/* Largest supported width or height; keeps all size arithmetic within 32 bits. */
#define RDS_BUF_SURFACE_MAX_DIM 16384

/* Values follow DeepStream's documented order: configs pass them as integers (nvbuf-memory-type=N). */
typedef enum
{
  RDS_BUF_MEM_DEFAULT = 0,          /* HIP device memory */
  RDS_BUF_MEM_HIP_PINNED = 1,       /* page-locked host memory (hipHostMalloc) */
  RDS_BUF_MEM_HIP_DEVICE = 2,       /* HIP device memory (hipMalloc) */
  RDS_BUF_MEM_HIP_UNIFIED = 3,      /* managed memory (hipMallocManaged) */
  RDS_BUF_MEM_SURFACE_ARRAY = 4,    /* Jetson only: rejected */
  RDS_BUF_MEM_HANDLE = 5,           /* Jetson only: rejected */
  RDS_BUF_MEM_SYSTEM = 6,           /* ordinary host memory */
} RdsBufSurfaceMemType;

typedef enum
{
  RDS_BUF_COLOR_FORMAT_INVALID = 0,
  RDS_BUF_COLOR_FORMAT_GRAY8,
  RDS_BUF_COLOR_FORMAT_YUV420,      /* I420: Y, U, V planes */
  RDS_BUF_COLOR_FORMAT_NV12,
  RDS_BUF_COLOR_FORMAT_RGBA,
  RDS_BUF_COLOR_FORMAT_BGRA,
  RDS_BUF_COLOR_FORMAT_RGBx,
  RDS_BUF_COLOR_FORMAT_BGRx,
  RDS_BUF_COLOR_FORMAT_RGB,
  RDS_BUF_COLOR_FORMAT_BGR,
  RDS_BUF_COLOR_FORMAT_LAST,
} RdsBufSurfaceColorFormat;

typedef enum
{
  RDS_BUF_LAYOUT_PITCH = 0,
  RDS_BUF_LAYOUT_BLOCK_LINEAR,      /* not supported */
} RdsBufSurfaceLayout;

typedef enum
{
  RDS_BUF_MAP_READ = 0,
  RDS_BUF_MAP_WRITE,
  RDS_BUF_MAP_READ_WRITE,
} RdsBufSurfaceMemMapFlags;

typedef struct
{
  guint32 num_planes;
  guint32 width[RDS_BUF_SURFACE_MAX_PLANES];        /* in plane units (NV12 UV: pairs) */
  guint32 height[RDS_BUF_SURFACE_MAX_PLANES];
  guint32 pitch[RDS_BUF_SURFACE_MAX_PLANES];        /* bytes per row */
  guint32 offset[RDS_BUF_SURFACE_MAX_PLANES];       /* bytes from dataPtr */
  guint32 psize[RDS_BUF_SURFACE_MAX_PLANES];        /* pitch * height */
  guint32 bytesPerPix[RDS_BUF_SURFACE_MAX_PLANES];
  gpointer _reserved[4 * RDS_BUF_SURFACE_MAX_PLANES];
} RdsBufSurfacePlaneParams;

typedef struct
{
  gpointer addr[RDS_BUF_SURFACE_MAX_PLANES];        /* CPU addresses, valid between map and unmap */
  gpointer _reserved[4];
} RdsBufSurfaceMappedAddr;

typedef struct
{
  guint32 width;
  guint32 height;
  guint32 pitch;                                    /* plane 0 pitch */
  RdsBufSurfaceColorFormat colorFormat;
  RdsBufSurfaceLayout layout;
  guint64 bufferDesc;                               /* unused; DeepStream keeps a DMABuf fd here */
  guint32 dataSize;                                 /* bytes, all planes */
  gpointer dataPtr;                                 /* device or host pointer, depending on memType */
  RdsBufSurfacePlaneParams planeParams;
  RdsBufSurfaceMappedAddr mappedAddr;
  gpointer _reserved[4];
} RdsBufSurfaceParams;

typedef struct
{
  guint32 gpuId;
  guint32 batchSize;
  guint32 numFilled;
  gboolean isContiguous;
  RdsBufSurfaceMemType memType;
  RdsBufSurfaceParams *surfaceList;
  gpointer _priv;                                   /* library-owned; NULL in hand-built surfaces */
  gpointer _reserved[3];
} RdsBufSurface;

typedef struct
{
  guint32 gpuId;
  guint32 width;
  guint32 height;
  guint32 size;                                     /* 0, or a minimum per-surface size in bytes */
  gboolean isContiguous;                            /* ignored: batches are always one allocation */
  RdsBufSurfaceColorFormat colorFormat;
  RdsBufSurfaceLayout layout;
  RdsBufSurfaceMemType memType;
} RdsBufSurfaceCreateParams;

/* All functions return 0 on success and -1 on failure (logged to the "rdsbufsurface" debug category).
 * index -1 means every surface in the batch; plane -1 means every plane of the surface. */
RDS_API int rds_buf_surface_create (RdsBufSurface ** surf, guint32 batchSize,
    const RdsBufSurfaceCreateParams * params);
RDS_API int rds_buf_surface_destroy (RdsBufSurface * surf);
RDS_API int rds_buf_surface_map (RdsBufSurface * surf, int index, int plane, RdsBufSurfaceMemMapFlags type);
RDS_API int rds_buf_surface_unmap (RdsBufSurface * surf, int index, int plane);
RDS_API int rds_buf_surface_sync_for_cpu (RdsBufSurface * surf, int index, int plane);
RDS_API int rds_buf_surface_sync_for_device (RdsBufSurface * surf, int index, int plane);
/* Copies the first src->numFilled surfaces row by row (padding untouched) and sets dst->numFilled.
 * Sizes and formats must match; memory types may differ. */
RDS_API int rds_buf_surface_copy (const RdsBufSurface * src, RdsBufSurface * dst);
/* Fills whole planes (pitch * height bytes, padding included). */
RDS_API int rds_buf_surface_memset (RdsBufSurface * surf, int index, int plane, guint8 value);

G_END_DECLS

#endif /* RDS_BUF_SURFACE_H */
