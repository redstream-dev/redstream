/* Redstream: RdsBufSurface allocation, CPU mapping and copies.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-buf-surface.h"
#include "rds-buf-surface-private.h"
#include "rds-hip-private.h"

#include <stdlib.h>
#include <string.h>

GST_DEBUG_CATEGORY_STATIC (rds_buf_surface_debug);
#define GST_CAT_DEFAULT rds_buf_surface_debug

typedef struct
{
  gpointer base;                /* one allocation for the whole batch */
  gsize stride;                 /* bytes between consecutive surfaces */
  guint8 *staging;              /* pinned mirror of base, for mapping device memory; lazily allocated */
} RdsBufSurfacePriv;

typedef struct
{
  guint32 wdiv, hdiv, bpp;
} PlaneDesc;

/* Called once per (surface, plane); returns FALSE on failure. */
typedef gboolean (*PlaneFunc) (RdsBufSurface * surf, guint index, guint plane, gpointer data);

static void
ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (rds_buf_surface_debug, "rdsbufsurface", 0, "Redstream buffer surfaces");
    g_once_init_leave (&initialized, 1);
  }
}

static guint
format_planes (RdsBufSurfaceColorFormat fmt, PlaneDesc pd[RDS_BUF_SURFACE_MAX_PLANES])
{
  switch (fmt) {
    case RDS_BUF_COLOR_FORMAT_GRAY8:
      pd[0] = (PlaneDesc) {1, 1, 1};
      return 1;
    case RDS_BUF_COLOR_FORMAT_YUV420:
      pd[0] = (PlaneDesc) {1, 1, 1};
      pd[1] = pd[2] = (PlaneDesc) {2, 2, 1};
      return 3;
    case RDS_BUF_COLOR_FORMAT_NV12:
      pd[0] = (PlaneDesc) {1, 1, 1};
      pd[1] = (PlaneDesc) {2, 2, 2};
      return 2;
    case RDS_BUF_COLOR_FORMAT_RGBA:
    case RDS_BUF_COLOR_FORMAT_BGRA:
    case RDS_BUF_COLOR_FORMAT_RGBx:
    case RDS_BUF_COLOR_FORMAT_BGRx:
      pd[0] = (PlaneDesc) {1, 1, 4};
      return 1;
    case RDS_BUF_COLOR_FORMAT_RGB:
    case RDS_BUF_COLOR_FORMAT_BGR:
      pd[0] = (PlaneDesc) {1, 1, 3};
      return 1;
    default:
      return 0;
  }
}

static gboolean
is_host_accessible (RdsBufSurfaceMemType t)
{
  return t == RDS_BUF_MEM_SYSTEM || t == RDS_BUF_MEM_HIP_PINNED || t == RDS_BUF_MEM_HIP_UNIFIED;
}

static gboolean
is_cpu_only (RdsBufSurfaceMemType t)
{
  return t == RDS_BUF_MEM_SYSTEM || t == RDS_BUF_MEM_HIP_PINNED;
}

gboolean
rds_buf_mem_type_is_supported (RdsBufSurfaceMemType t)
{
  switch (t) {
    case RDS_BUF_MEM_DEFAULT:
    case RDS_BUF_MEM_HIP_PINNED:
    case RDS_BUF_MEM_HIP_DEVICE:
    case RDS_BUF_MEM_HIP_UNIFIED:
    case RDS_BUF_MEM_SYSTEM:
      return TRUE;
    default:
      return FALSE;
  }
}

/* Hand-built surfaces can carry any memType; refuse the unsupported ones before touching memory or HIP. */
static gboolean
check_mem_type (const RdsBufSurface * surf)
{
  if (rds_buf_mem_type_is_supported (surf->memType))
    return TRUE;
  GST_ERROR ("surface %p has unsupported memory type %d", surf, surf->memType);
  return FALSE;
}

/* Makes @gpu the current HIP device and stores the previous one in @prev. Only for HIP memory. */
static gboolean
device_enter (guint32 gpu, int *prev)
{
  if (!RDS_HIP_CHECK (hipGetDevice (prev)))
    return FALSE;
  return *prev == (int) gpu || RDS_HIP_CHECK (hipSetDevice ((int) gpu));
}

/* Restores the device saved by a successful device_enter (). */
static void
device_leave (guint32 gpu, int prev)
{
  if (prev != (int) gpu)
    RDS_HIP_CHECK (hipSetDevice (prev));
}

/* Fills the geometry of one surface (everything except dataPtr and mappedAddr). */
static void
fill_params (RdsBufSurfaceParams * sp, const RdsBufSurfaceCreateParams * params,
    const PlaneDesc * pd, guint num_planes)
{
  RdsBufSurfacePlaneParams *pp = &sp->planeParams;
  guint32 offset = 0;

  sp->width = params->width;
  sp->height = params->height;
  sp->colorFormat = params->colorFormat;
  sp->layout = RDS_BUF_LAYOUT_PITCH;
  pp->num_planes = num_planes;

  for (guint p = 0; p < num_planes; p++) {
    pp->width[p] = (params->width + pd[p].wdiv - 1) / pd[p].wdiv;
    pp->height[p] = (params->height + pd[p].hdiv - 1) / pd[p].hdiv;
    pp->bytesPerPix[p] = pd[p].bpp;
    pp->pitch[p] = GST_ROUND_UP_N (pp->width[p] * pd[p].bpp, RDS_BUF_SURFACE_PITCH_ALIGN);
    pp->psize[p] = pp->pitch[p] * pp->height[p];
    pp->offset[p] = offset;
    offset += pp->psize[p];
  }

  sp->pitch = pp->pitch[0];
  sp->dataSize = MAX (offset, params->size);
}

static gpointer
mem_alloc (RdsBufSurfaceMemType t, gsize size)
{
  gpointer ptr = NULL;

  switch (t) {
    case RDS_BUF_MEM_SYSTEM:
      /* Not g_aligned_alloc0 (): it aborts on failure, and sizes come from configs. */
      if (posix_memalign (&ptr, RDS_BUF_SURFACE_PITCH_ALIGN, size) != 0)
        return NULL;
      memset (ptr, 0, size);
      return ptr;
    case RDS_BUF_MEM_HIP_PINNED:
      if (!RDS_HIP_CHECK (hipHostMalloc (&ptr, size, hipHostMallocDefault)))
        return NULL;
      return ptr;
    case RDS_BUF_MEM_DEFAULT:
    case RDS_BUF_MEM_HIP_DEVICE:
      if (!RDS_HIP_CHECK (hipMalloc (&ptr, size)))
        return NULL;
      return ptr;
    case RDS_BUF_MEM_HIP_UNIFIED:
      if (!RDS_HIP_CHECK (hipMallocManaged (&ptr, size, hipMemAttachGlobal)))
        return NULL;
      return ptr;
    default:
      return NULL;
  }
}

static void
mem_free (RdsBufSurfaceMemType t, gpointer ptr)
{
  if (ptr == NULL)
    return;

  switch (t) {
    case RDS_BUF_MEM_SYSTEM:
      free (ptr);
      break;
    case RDS_BUF_MEM_HIP_PINNED:
      RDS_HIP_CHECK (hipHostFree (ptr));
      break;
    case RDS_BUF_MEM_DEFAULT:
    case RDS_BUF_MEM_HIP_DEVICE:
    case RDS_BUF_MEM_HIP_UNIFIED:
      RDS_HIP_CHECK (hipFree (ptr));
      break;
    default:
      break;
  }
}

/* Expands index/plane -1 and validates every bound before calling @fn on anything. */
static int
for_each_plane (RdsBufSurface * surf, int index, int plane, PlaneFunc fn, gpointer data)
{
  guint first, last;

  if (surf->surfaceList == NULL || surf->batchSize == 0) {
    GST_ERROR ("surface has no surface list");
    return -1;
  }
  if (index < -1 || (index >= 0 && (guint) index >= surf->batchSize)) {
    GST_ERROR ("index %d out of range (batch size %u)", index, surf->batchSize);
    return -1;
  }
  first = index < 0 ? 0 : (guint) index;
  last = index < 0 ? surf->batchSize - 1 : (guint) index;

  for (guint i = first; i <= last; i++) {
    guint n = surf->surfaceList[i].planeParams.num_planes;

    if (n > RDS_BUF_SURFACE_MAX_PLANES || plane < -1 || (plane >= 0 && (guint) plane >= n)) {
      GST_ERROR ("plane %d out of range for surface %u (%u planes)", plane, i, n);
      return -1;
    }
  }

  for (guint i = first; i <= last; i++) {
    guint n = surf->surfaceList[i].planeParams.num_planes;
    guint pfirst = plane < 0 ? 0 : (guint) plane;
    guint plast = plane < 0 ? n : (guint) plane + 1;

    for (guint p = pfirst; p < plast; p++) {
      if (!fn (surf, i, p, data))
        return -1;
    }
  }
  return 0;
}

int
rds_buf_surface_create (RdsBufSurface ** surf, guint32 batchSize, const RdsBufSurfaceCreateParams * params)
{
  PlaneDesc pd[RDS_BUF_SURFACE_MAX_PLANES];
  RdsBufSurfaceParams proto = { 0 };
  RdsBufSurfaceParams *list = NULL;
  RdsBufSurfacePriv *priv;
  RdsBufSurface *s;
  gpointer base;
  guint num_planes;
  gsize stride, total;
  gboolean hip = params != NULL && params->memType != RDS_BUF_MEM_SYSTEM;
  int prev_dev = 0;

  g_return_val_if_fail (surf != NULL, -1);
  *surf = NULL;
  g_return_val_if_fail (params != NULL, -1);
  ensure_debug_category ();

  if (batchSize < 1) {
    GST_ERROR ("batch size must be at least 1");
    return -1;
  }
  if (params->layout != RDS_BUF_LAYOUT_PITCH) {
    GST_ERROR ("unsupported layout %d (only pitch-linear)", params->layout);
    return -1;
  }
  if (!rds_buf_mem_type_is_supported (params->memType)) {
    GST_ERROR ("unsupported memory type %d", params->memType);
    return -1;
  }
  num_planes = format_planes (params->colorFormat, pd);
  if (num_planes == 0) {
    GST_ERROR ("unsupported color format %d", params->colorFormat);
    return -1;
  }
  if (params->width < 1 || params->width > RDS_BUF_SURFACE_MAX_DIM
      || params->height < 1 || params->height > RDS_BUF_SURFACE_MAX_DIM) {
    GST_ERROR ("size %ux%u out of range (1..%u)", params->width, params->height, RDS_BUF_SURFACE_MAX_DIM);
    return -1;
  }

  fill_params (&proto, params, pd, num_planes);
  stride = GST_ROUND_UP_N ((gsize) proto.dataSize, RDS_BUF_SURFACE_PITCH_ALIGN);
  if (!g_size_checked_mul (&total, stride, batchSize) || total > G_MAXSSIZE) {
    GST_ERROR ("batch of %u surfaces of %" G_GSIZE_FORMAT " bytes is too large", batchSize, stride);
    return -1;
  }

  /* Only HIP memory selects a device: the system path never touches HIP. */
  if (hip && !device_enter (params->gpuId, &prev_dev))
    return -1;
  base = mem_alloc (params->memType, total);
  if (base != NULL) {
    list = g_try_new0 (RdsBufSurfaceParams, batchSize);
    if (list == NULL) {
      mem_free (params->memType, base);
      base = NULL;
    }
  }
  if (hip)
    device_leave (params->gpuId, prev_dev);
  if (base == NULL) {
    GST_ERROR ("failed to allocate %" G_GSIZE_FORMAT " bytes (memory type %d)", total, params->memType);
    return -1;
  }

  priv = g_new0 (RdsBufSurfacePriv, 1);
  priv->base = base;
  priv->stride = stride;

  s = g_new0 (RdsBufSurface, 1);
  s->gpuId = params->gpuId;
  s->batchSize = batchSize;
  s->numFilled = 0;
  s->isContiguous = TRUE;
  s->memType = params->memType;
  s->surfaceList = list;
  s->_priv = priv;
  for (guint32 i = 0; i < batchSize; i++) {
    s->surfaceList[i] = proto;
    s->surfaceList[i].dataPtr = (guint8 *) priv->base + i * stride;
  }

  GST_DEBUG ("created %ux%u format %d batch %u memory type %d, %u bytes per surface",
      params->width, params->height, params->colorFormat, batchSize, params->memType, proto.dataSize);
  *surf = s;
  return 0;
}

int
rds_buf_surface_destroy (RdsBufSurface * surf)
{
  RdsBufSurfacePriv *priv;
  gboolean hip, switched;
  int prev_dev = 0;

  g_return_val_if_fail (surf != NULL, -1);
  ensure_debug_category ();

  priv = surf->_priv;
  if (priv == NULL) {
    GST_ERROR ("surface %p was not created by rds_buf_surface_create", surf);
    return -1;
  }

  hip = surf->memType != RDS_BUF_MEM_SYSTEM || priv->staging != NULL;
  /* Free even if the device switch fails: the pointers are still valid on their own device. */
  switched = hip && device_enter (surf->gpuId, &prev_dev);
  if (priv->staging != NULL)
    RDS_HIP_CHECK (hipHostFree (priv->staging));
  mem_free (surf->memType, priv->base);
  if (switched)
    device_leave (surf->gpuId, prev_dev);
  g_free (priv);
  g_free (surf->surfaceList);
  g_free (surf);
  return 0;
}

static gboolean
map_plane (RdsBufSurface * surf, guint index, guint plane, G_GNUC_UNUSED gpointer data)
{
  RdsBufSurfaceParams *sp = &surf->surfaceList[index];
  RdsBufSurfacePriv *priv = surf->_priv;

  if (is_host_accessible (surf->memType)) {
    sp->mappedAddr.addr[plane] = (guint8 *) sp->dataPtr + sp->planeParams.offset[plane];
    return TRUE;
  }

  if (priv == NULL) {
    GST_ERROR ("cannot map device memory of a surface not created by rds_buf_surface_create");
    return FALSE;
  }
  if (priv->staging == NULL) {
    gsize size = priv->stride * surf->batchSize;
    gpointer staging = NULL;
    gboolean ok;
    int prev_dev;

    if (!device_enter (surf->gpuId, &prev_dev))
      return FALSE;
    ok = RDS_HIP_CHECK (hipHostMalloc (&staging, size, hipHostMallocDefault));
    device_leave (surf->gpuId, prev_dev);
    if (!ok)
      return FALSE;
    memset (staging, 0, size);
    priv->staging = staging;
  }
  sp->mappedAddr.addr[plane] = priv->staging + index * priv->stride + sp->planeParams.offset[plane];
  return TRUE;
}

int
rds_buf_surface_map (RdsBufSurface * surf, int index, int plane, RdsBufSurfaceMemMapFlags type)
{
  g_return_val_if_fail (surf != NULL, -1);
  ensure_debug_category ();

  if (type != RDS_BUF_MAP_READ && type != RDS_BUF_MAP_WRITE && type != RDS_BUF_MAP_READ_WRITE) {
    GST_ERROR ("invalid map flags %d", type);
    return -1;
  }
  if (!check_mem_type (surf))
    return -1;
  return for_each_plane (surf, index, plane, map_plane, NULL);
}

static gboolean
unmap_plane (RdsBufSurface * surf, guint index, guint plane, G_GNUC_UNUSED gpointer data)
{
  surf->surfaceList[index].mappedAddr.addr[plane] = NULL;
  return TRUE;
}

int
rds_buf_surface_unmap (RdsBufSurface * surf, int index, int plane)
{
  g_return_val_if_fail (surf != NULL, -1);
  ensure_debug_category ();

  return for_each_plane (surf, index, plane, unmap_plane, NULL);
}

/* Copies one plane between device memory and its staging mirror. The caller has made the device current. */
static gboolean
sync_plane (RdsBufSurface * surf, guint index, guint plane, gboolean to_cpu)
{
  RdsBufSurfaceParams *sp = &surf->surfaceList[index];
  RdsBufSurfacePlaneParams *pp = &sp->planeParams;
  RdsBufSurfacePriv *priv = surf->_priv;
  guint8 *dev = (guint8 *) sp->dataPtr + pp->offset[plane];
  guint8 *host;

  if (sp->mappedAddr.addr[plane] == NULL || priv == NULL || priv->staging == NULL) {
    GST_ERROR ("surface %u plane %u is not mapped", index, plane);
    return FALSE;
  }
  /* Recompute rather than trust mappedAddr, which callers can overwrite. */
  host = priv->staging + index * priv->stride + pp->offset[plane];
  if (to_cpu)
    return RDS_HIP_CHECK (hipMemcpy (host, dev, pp->psize[plane], hipMemcpyDeviceToHost));
  return RDS_HIP_CHECK (hipMemcpy (dev, host, pp->psize[plane], hipMemcpyHostToDevice));
}

static gboolean
sync_plane_to_cpu (RdsBufSurface * surf, guint index, guint plane, G_GNUC_UNUSED gpointer data)
{
  return sync_plane (surf, index, plane, TRUE);
}

static gboolean
sync_plane_to_device (RdsBufSurface * surf, guint index, guint plane, G_GNUC_UNUSED gpointer data)
{
  return sync_plane (surf, index, plane, FALSE);
}

/* Host-accessible memory needs no sync, but the bounds are still checked. */
static gboolean
sync_noop (G_GNUC_UNUSED RdsBufSurface * surf, G_GNUC_UNUSED guint index, G_GNUC_UNUSED guint plane,
    G_GNUC_UNUSED gpointer data)
{
  return TRUE;
}

static int
sync_surface (RdsBufSurface * surf, int index, int plane, PlaneFunc fn)
{
  int prev_dev, ret;

  if (!check_mem_type (surf))
    return -1;
  if (is_host_accessible (surf->memType))
    return for_each_plane (surf, index, plane, sync_noop, NULL);

  if (!device_enter (surf->gpuId, &prev_dev))
    return -1;
  ret = for_each_plane (surf, index, plane, fn, NULL);
  device_leave (surf->gpuId, prev_dev);
  return ret;
}

int
rds_buf_surface_sync_for_cpu (RdsBufSurface * surf, int index, int plane)
{
  g_return_val_if_fail (surf != NULL, -1);
  ensure_debug_category ();

  return sync_surface (surf, index, plane, sync_plane_to_cpu);
}

int
rds_buf_surface_sync_for_device (RdsBufSurface * surf, int index, int plane)
{
  g_return_val_if_fail (surf != NULL, -1);
  ensure_debug_category ();

  return sync_surface (surf, index, plane, sync_plane_to_device);
}

static gboolean
memset_plane (RdsBufSurface * surf, guint index, guint plane, gpointer data)
{
  RdsBufSurfaceParams *sp = &surf->surfaceList[index];
  guint8 *ptr = (guint8 *) sp->dataPtr + sp->planeParams.offset[plane];
  guint8 value = GPOINTER_TO_UINT (data);

  if (is_cpu_only (surf->memType)) {
    memset (ptr, value, sp->planeParams.psize[plane]);
    return TRUE;
  }
  return RDS_HIP_CHECK (hipMemset (ptr, value, sp->planeParams.psize[plane]));
}

int
rds_buf_surface_memset (RdsBufSurface * surf, int index, int plane, guint8 value)
{
  int prev_dev, ret;

  g_return_val_if_fail (surf != NULL, -1);
  ensure_debug_category ();

  if (!check_mem_type (surf))
    return -1;
  if (is_cpu_only (surf->memType))
    return for_each_plane (surf, index, plane, memset_plane, GUINT_TO_POINTER (value));

  if (!device_enter (surf->gpuId, &prev_dev))
    return -1;
  ret = for_each_plane (surf, index, plane, memset_plane, GUINT_TO_POINTER (value));
  /* Library operations are synchronous (ready-on-push). */
  if (ret == 0 && !RDS_HIP_CHECK (hipStreamSynchronize (NULL)))
    ret = -1;
  device_leave (surf->gpuId, prev_dev);
  return ret;
}

/* Checks that surface @i of @dst can receive surface @i of @src. */
static gboolean
copy_compatible (const RdsBufSurfaceParams * s, const RdsBufSurfaceParams * d, guint32 i)
{
  if (s->width != d->width || s->height != d->height || s->colorFormat != d->colorFormat
      || s->planeParams.num_planes != d->planeParams.num_planes
      || s->planeParams.num_planes > RDS_BUF_SURFACE_MAX_PLANES) {
    GST_ERROR ("surface %u mismatch: %ux%u format %d vs %ux%u format %d", i, s->width, s->height,
        s->colorFormat, d->width, d->height, d->colorFormat);
    return FALSE;
  }
  for (guint p = 0; p < s->planeParams.num_planes; p++) {
    gsize row_bytes = (gsize) s->planeParams.width[p] * s->planeParams.bytesPerPix[p];

    if (d->planeParams.pitch[p] < row_bytes || d->planeParams.height[p] < s->planeParams.height[p]) {
      GST_ERROR ("surface %u plane %u: destination pitch %u / height %u too small for %" G_GSIZE_FORMAT
          " bytes x %u rows", i, p, d->planeParams.pitch[p], d->planeParams.height[p], row_bytes,
          s->planeParams.height[p]);
      return FALSE;
    }
  }
  return TRUE;
}

/* Copies the pixel bytes of every plane of the first src->numFilled surfaces. */
static gboolean
copy_planes (const RdsBufSurface * src, RdsBufSurface * dst, gboolean host_copy)
{
  for (guint32 i = 0; i < src->numFilled; i++) {
    const RdsBufSurfaceParams *s = &src->surfaceList[i];
    RdsBufSurfaceParams *d = &dst->surfaceList[i];

    for (guint p = 0; p < s->planeParams.num_planes; p++) {
      const guint8 *sptr = (const guint8 *) s->dataPtr + s->planeParams.offset[p];
      guint8 *dptr = (guint8 *) d->dataPtr + d->planeParams.offset[p];
      gsize row_bytes = (gsize) s->planeParams.width[p] * s->planeParams.bytesPerPix[p];
      guint32 rows = s->planeParams.height[p];

      if (host_copy) {
        for (guint32 y = 0; y < rows; y++)
          memcpy (dptr + (gsize) y * d->planeParams.pitch[p], sptr + (gsize) y * s->planeParams.pitch[p],
              row_bytes);
      } else if (!RDS_HIP_CHECK (hipMemcpy2D (dptr, d->planeParams.pitch[p], sptr, s->planeParams.pitch[p],
                  row_bytes, rows, hipMemcpyDefault))) {
        return FALSE;
      }
    }
  }
  return TRUE;
}

int
rds_buf_surface_copy (const RdsBufSurface * src, RdsBufSurface * dst)
{
  guint32 gpu;
  int prev_dev;
  gboolean ok;

  g_return_val_if_fail (src != NULL, -1);
  g_return_val_if_fail (dst != NULL, -1);
  ensure_debug_category ();

  if (!check_mem_type (src) || !check_mem_type (dst))
    return -1;
  if (src->numFilled > src->batchSize || src->numFilled > dst->batchSize) {
    GST_ERROR ("cannot copy %u filled surfaces (source batch %u, destination batch %u)",
        src->numFilled, src->batchSize, dst->batchSize);
    return -1;
  }
  if (src == dst)
    return 0;
  if (src->numFilled > 0 && (src->surfaceList == NULL || dst->surfaceList == NULL)) {
    GST_ERROR ("surface has no surface list");
    return -1;
  }
  for (guint32 i = 0; i < src->numFilled; i++) {
    if (!copy_compatible (&src->surfaceList[i], &dst->surfaceList[i], i))
      return -1;
  }

  if (is_cpu_only (src->memType) && is_cpu_only (dst->memType)) {
    (void) copy_planes (src, dst, TRUE);       /* memcpy cannot fail */
  } else {
    /* Run on the GPU that owns the destination, or the source's when the destination is host memory. */
    gpu = is_cpu_only (dst->memType) ? src->gpuId : dst->gpuId;
    if (!device_enter (gpu, &prev_dev))
      return -1;
    /* Library operations are synchronous (ready-on-push). */
    ok = copy_planes (src, dst, FALSE) && RDS_HIP_CHECK (hipStreamSynchronize (NULL));
    device_leave (gpu, prev_dev);
    if (!ok)
      return -1;
  }

  dst->numFilled = src->numFilled;
  return 0;
}
