/* Redstream: rds_buf_surf_transform argument validation, per-thread session parameters and dispatch.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-transform.h"
#include "rds-buf-surface-private.h"
#include "rds-hip-private.h"
#include "rds-transform-private.h"

GST_DEBUG_CATEGORY_STATIC (rds_transform_debug);
#define GST_CAT_DEFAULT rds_transform_debug

static void
ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (rds_transform_debug, "rdstransform", 0, "Redstream surface transform");
    g_once_init_leave (&initialized, 1);
  }
}

/* The calling thread's session; NULL until set_session_params () first succeeds on it. */
static GPrivate session_key = G_PRIVATE_INIT (g_free);

static const RdsBufSurfTransformConfigParams default_session = {
  .compute_mode = RdsBufSurfTransformCompute_Default,
  .gpu_id = 0,
  .cuda_stream = NULL,
};

static const RdsBufSurfTransformConfigParams *
current_session (void)
{
  const RdsBufSurfTransformConfigParams *s = g_private_get (&session_key);

  return s != NULL ? s : &default_session;
}

RdsBufSurfTransform_Error
rds_buf_surf_transform_set_session_params (RdsBufSurfTransformConfigParams * config_params)
{
  RdsBufSurfTransformConfigParams *s;
  int count = 0;

  g_return_val_if_fail (config_params != NULL, RdsBufSurfTransformError_Invalid_Params);
  ensure_debug_category ();

  switch (config_params->compute_mode) {
    case RdsBufSurfTransformCompute_Default:
    case RdsBufSurfTransformCompute_GPU:
      break;
    case RdsBufSurfTransformCompute_VIC:
      GST_ERROR ("compute mode VIC is not available (NVIDIA Jetson only)");
      return RdsBufSurfTransformError_Unsupported;
    default:
      GST_ERROR ("invalid compute mode %d", (gint) config_params->compute_mode);
      return RdsBufSurfTransformError_Invalid_Params;
  }
  if (config_params->gpu_id < 0) {
    GST_ERROR ("invalid gpu_id %d", config_params->gpu_id);
    return RdsBufSurfTransformError_Invalid_Params;
  }
  /* Without a visible device, store the value anyway: the transform reports Execution_Error later. */
  if (hipGetDeviceCount (&count) == hipSuccess && count > 0 && config_params->gpu_id >= count) {
    GST_ERROR ("gpu_id %d out of range (%d devices)", config_params->gpu_id, count);
    return RdsBufSurfTransformError_Invalid_Params;
  }

  s = g_private_get (&session_key);
  if (s == NULL) {
    s = g_new (RdsBufSurfTransformConfigParams, 1);
    g_private_set (&session_key, s);
  }
  *s = *config_params;
  return RdsBufSurfTransformError_Success;
}

RdsBufSurfTransform_Error
rds_buf_surf_transform_get_session_params (RdsBufSurfTransformConfigParams * config_params)
{
  g_return_val_if_fail (config_params != NULL, RdsBufSurfTransformError_Invalid_Params);

  *config_params = *current_session ();
  return RdsBufSurfTransformError_Success;
}

RdsBufSurfTransform_Error
rds_buf_surf_transform_set_default_session (void)
{
  /* g_private_replace frees the old value through the destroy notify. */
  g_private_replace (&session_key, NULL);
  return RdsBufSurfTransformError_Success;
}

static gboolean
is_device_accessible (RdsBufSurfaceMemType t)
{
  switch (t) {
    case RDS_BUF_MEM_DEFAULT:
    case RDS_BUF_MEM_HIP_DEVICE:
    case RDS_BUF_MEM_HIP_UNIFIED:
    case RDS_BUF_MEM_HIP_PINNED:
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
is_supported_format (RdsBufSurfaceColorFormat fmt)
{
  switch (fmt) {
    case RDS_BUF_COLOR_FORMAT_GRAY8:
    case RDS_BUF_COLOR_FORMAT_YUV420:
    case RDS_BUF_COLOR_FORMAT_NV12:
    case RDS_BUF_COLOR_FORMAT_RGBA:
    case RDS_BUF_COLOR_FORMAT_BGRA:
    case RDS_BUF_COLOR_FORMAT_RGBx:
    case RDS_BUF_COLOR_FORMAT_BGRx:
    case RDS_BUF_COLOR_FORMAT_RGB:
    case RDS_BUF_COLOR_FORMAT_BGR:
      return TRUE;
    default:
      return FALSE;
  }
}

static gboolean
is_yuv420 (RdsBufSurfaceColorFormat fmt)
{
  return fmt == RDS_BUF_COLOR_FORMAT_NV12 || fmt == RDS_BUF_COLOR_FORMAT_YUV420;
}

/* Rule 5: every filled surface of @surf has a supported format and pitch layout. */
static gboolean
check_formats (const RdsBufSurface * surf, guint n, const gchar * side)
{
  for (guint i = 0; i < n; i++) {
    const RdsBufSurfaceParams *p = &surf->surfaceList[i];

    if (!is_supported_format (p->colorFormat)) {
      GST_ERROR ("%s surface %u: unsupported colour format %d", side, i, (gint) p->colorFormat);
      return FALSE;
    }
    if (p->layout != RDS_BUF_LAYOUT_PITCH) {
      GST_ERROR ("%s surface %u: unsupported layout %d (only pitch-linear)", side, i, (gint) p->layout);
      return FALSE;
    }
  }
  return TRUE;
}

/* Rule 6: rects present, non-empty and inside their surfaces (64-bit sums, so wrap-around can't pass). */
static RdsBufSurfTransform_Error
check_rects (const RdsBufSurface * surf, const RdsBufSurfTransformRect * rects, guint n, const gchar * side)
{
  if (rects == NULL) {
    GST_ERROR ("%s crop flag set but the %s_rect array is NULL", side, side);
    return RdsBufSurfTransformError_Invalid_Params;
  }
  for (guint i = 0; i < n; i++) {
    const RdsBufSurfTransformRect *r = &rects[i];
    const RdsBufSurfaceParams *p = &surf->surfaceList[i];

    if (r->width == 0 || r->height == 0
        || (guint64) r->left + r->width > p->width || (guint64) r->top + r->height > p->height) {
      GST_ERROR ("%s rect %u (left %u, top %u, %ux%u) is empty or outside the %ux%u surface", side, i,
          r->left, r->top, r->width, r->height, p->width, p->height);
      return RdsBufSurfTransformError_ROI_Error;
    }
  }
  return RdsBufSurfTransformError_Success;
}

/* Rule 7: without ALLOW_ODD_CROP, every edge of a destination rect on a 4:2:0 surface must be even. A right or
 * bottom edge on the surface's own (odd) edge counts as even: the last chroma sample is a whole one there. */
static gboolean
check_even_dst_rects (const RdsBufSurface * dst, const RdsBufSurfTransformRect * rects, guint n)
{
  for (guint i = 0; i < n; i++) {
    const RdsBufSurfTransformRect *r = &rects[i];
    const RdsBufSurfaceParams *p = &dst->surfaceList[i];
    guint64 right = (guint64) r->left + r->width;
    guint64 bottom = (guint64) r->top + r->height;
    gboolean odd = (r->left & 1) || (r->top & 1) || ((right & 1) && right != p->width)
        || ((bottom & 1) && bottom != p->height);

    if (is_yuv420 (p->colorFormat) && odd) {
      GST_ERROR ("dst rect %u (left %u, top %u, %ux%u) is odd on a 4:2:0 surface and ALLOW_ODD_CROP is not set",
          i, r->left, r->top, r->width, r->height);
      return FALSE;
    }
  }
  return TRUE;
}

RdsBufSurfTransform_Error
rds_transform_validate (const RdsBufSurface * src, const RdsBufSurface * dst,
    const RdsBufSurfTransformParams * transform_params)
{
  const RdsBufSurfTransformConfigParams *session;
  const RdsBufSurfTransformParams *tp = transform_params;
  RdsBufSurfTransform_Error ret;
  guint n;

  /* 1. NULL arguments */
  g_return_val_if_fail (src != NULL, RdsBufSurfTransformError_Invalid_Params);
  g_return_val_if_fail (dst != NULL, RdsBufSurfTransformError_Invalid_Params);
  g_return_val_if_fail (transform_params != NULL, RdsBufSurfTransformError_Invalid_Params);
  ensure_debug_category ();
  session = current_session ();

  /* 2. batch counts */
  n = src->numFilled;
  if (n == 0 || n > src->batchSize || n > dst->batchSize || src->surfaceList == NULL || dst->surfaceList == NULL) {
    GST_ERROR ("invalid batch: src numFilled %u, src batchSize %u, dst batchSize %u, surface lists %p/%p",
        n, src->batchSize, dst->batchSize, src->surfaceList, dst->surfaceList);
    return RdsBufSurfTransformError_Invalid_Params;
  }

  /* 3. modes we don't implement */
  if (session->compute_mode == RdsBufSurfTransformCompute_VIC) {
    GST_ERROR ("compute mode VIC is not available (NVIDIA Jetson only)");
    return RdsBufSurfTransformError_Unsupported;
  }
  if (tp->transform_flag & RDS_BUFSURF_TRANSFORM_NORMALIZE) {
    GST_ERROR ("NORMALIZE is not supported");
    return RdsBufSurfTransformError_Unsupported;
  }
  if ((tp->transform_flag & RDS_BUFSURF_TRANSFORM_FLIP) && tp->transform_flip != RdsBufSurfTransform_None) {
    GST_ERROR ("flip %d is not supported (only None)", (gint) tp->transform_flip);
    return RdsBufSurfTransformError_Unsupported;
  }

  /* 4. memory types */
  if (!is_device_accessible (src->memType) || !is_device_accessible (dst->memType)) {
    GST_ERROR ("memory types src %d / dst %d: both must be device-accessible (default, device, pinned, unified)",
        (gint) src->memType, (gint) dst->memType);
    return RdsBufSurfTransformError_Unsupported;
  }

  /* 5. formats and layouts */
  if (!check_formats (src, n, "src") || !check_formats (dst, n, "dst"))
    return RdsBufSurfTransformError_Unsupported;

  /* 6. rects */
  if (tp->transform_flag & RDS_BUFSURF_TRANSFORM_CROP_SRC) {
    ret = check_rects (src, tp->src_rect, n, "src");
    if (ret != RdsBufSurfTransformError_Success)
      return ret;
  }
  if (tp->transform_flag & RDS_BUFSURF_TRANSFORM_CROP_DST) {
    ret = check_rects (dst, tp->dst_rect, n, "dst");
    if (ret != RdsBufSurfTransformError_Success)
      return ret;
  }

  /* 7. odd destination rects on 4:2:0 */
  if ((tp->transform_flag & RDS_BUFSURF_TRANSFORM_CROP_DST)
      && !(tp->transform_flag & RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP)
      && !check_even_dst_rects (dst, tp->dst_rect, n))
    return RdsBufSurfTransformError_Invalid_Params;

  /* 8. filter (only read when FILTER is set; otherwise Default) */
  if ((tp->transform_flag & RDS_BUFSURF_TRANSFORM_FILTER)
      && ((gint) tp->transform_filter < RdsBufSurfTransformInter_Nearest
          || (gint) tp->transform_filter > RdsBufSurfTransformInter_Default)) {
    GST_ERROR ("invalid filter %d", (gint) tp->transform_filter);
    return RdsBufSurfTransformError_Invalid_Params;
  }

  return RdsBufSurfTransformError_Success;
}

RdsBufSurfTransform_Error
rds_buf_surf_transform (RdsBufSurface * src, RdsBufSurface * dst, RdsBufSurfTransformParams * transform_params)
{
  const RdsBufSurfTransformConfigParams *session;
  RdsBufSurfTransform_Error ret;
  int prev_dev;

  ret = rds_transform_validate (src, dst, transform_params);
  if (ret != RdsBufSurfTransformError_Success)
    return ret;
  session = current_session ();

  if (!rds_device_enter ((guint32) session->gpu_id, &prev_dev)) {
    GST_ERROR ("cannot select GPU %d", session->gpu_id);
    return RdsBufSurfTransformError_Execution_Error;
  }
  /* The kernel launch, the stream sync and dst->numFilled come with the kernels. */
  GST_ERROR ("no kernel yet");
  ret = RdsBufSurfTransformError_Unsupported;
  rds_device_leave ((guint32) session->gpu_id, prev_dev);
  return ret;
}
