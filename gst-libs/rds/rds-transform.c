/* Redstream: rds_buf_surf_transform argument validation, per-thread session parameters and dispatch.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-transform.h"
#include "rds-buf-surface-private.h"
#include "rds-hip-private.h"
#include "rds-transform-private.h"

#include <string.h>

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

/* Rule 4b */
static gboolean
check_gpu (const RdsBufSurface * surf, const RdsBufSurfTransformConfigParams * session, const gchar * side)
{
  if ((surf->memType == RDS_BUF_MEM_DEFAULT || surf->memType == RDS_BUF_MEM_HIP_DEVICE)
      && surf->gpuId != (guint32) session->gpu_id) {
    GST_ERROR ("%s surface is device memory on GPU %u but the session uses GPU %d", side, surf->gpuId,
        session->gpu_id);
    return FALSE;
  }
  return TRUE;
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

/* The plane count of a supported format, and the minimum row bytes and rows of plane @pl at @w x @h. */
static guint
expected_planes (RdsBufSurfaceColorFormat fmt)
{
  switch (fmt) {
    case RDS_BUF_COLOR_FORMAT_YUV420:
      return 3;
    case RDS_BUF_COLOR_FORMAT_NV12:
      return 2;
    default:
      return 1;
  }
}

static void
expected_plane_size (RdsBufSurfaceColorFormat fmt, guint pl, guint32 w, guint32 h, guint64 * row_bytes,
    guint64 * rows)
{
  guint64 cw = ((guint64) w + 1) / 2, ch = ((guint64) h + 1) / 2;

  switch (fmt) {
    case RDS_BUF_COLOR_FORMAT_YUV420:
      *row_bytes = pl == 0 ? w : cw;
      *rows = pl == 0 ? h : ch;
      return;
    case RDS_BUF_COLOR_FORMAT_NV12:
      *row_bytes = pl == 0 ? w : cw * 2;
      *rows = pl == 0 ? h : ch;
      return;
    case RDS_BUF_COLOR_FORMAT_GRAY8:
      *row_bytes = w;
      break;
    case RDS_BUF_COLOR_FORMAT_RGB:
    case RDS_BUF_COLOR_FORMAT_BGR:
      *row_bytes = (guint64) w * 3;
      break;
    default:                   /* 4 bytes per pixel */
      *row_bytes = (guint64) w * 4;
      break;
  }
  *rows = h;
}

/* Rule 5b: sizes within RDS_BUF_SURFACE_MAX_DIM (the kernel's index arithmetic is 32-bit and sized for it), and
 * plane geometry that holds the format: the right plane count, and every plane's pitch and height large enough
 * for its rows. Surfaces from rds_buf_surface_create () always pass; this catches hand-built ones. */
static gboolean
check_geometry (const RdsBufSurface * surf, guint n, const gchar * side)
{
  for (guint i = 0; i < n; i++) {
    const RdsBufSurfaceParams *p = &surf->surfaceList[i];
    const RdsBufSurfacePlaneParams *pp = &p->planeParams;

    if (p->width == 0 || p->height == 0 || p->width > RDS_BUF_SURFACE_MAX_DIM
        || p->height > RDS_BUF_SURFACE_MAX_DIM) {
      GST_ERROR ("%s surface %u: size %ux%u is empty or larger than %u", side, i, p->width, p->height,
          RDS_BUF_SURFACE_MAX_DIM);
      return FALSE;
    }
    if (pp->num_planes != expected_planes (p->colorFormat)) {
      GST_ERROR ("%s surface %u: %u planes, the format has %u", side, i, pp->num_planes,
          expected_planes (p->colorFormat));
      return FALSE;
    }
    for (guint pl = 0; pl < pp->num_planes; pl++) {
      guint64 row_bytes, rows;

      expected_plane_size (p->colorFormat, pl, p->width, p->height, &row_bytes, &rows);
      if (pp->pitch[pl] < row_bytes || pp->height[pl] < rows) {
        GST_ERROR ("%s surface %u plane %u: pitch %u / height %u too small for %" G_GUINT64_FORMAT
            " bytes x %" G_GUINT64_FORMAT " rows", side, i, pl, pp->pitch[pl], pp->height[pl], row_bytes, rows);
        return FALSE;
      }
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

/* Rules 4 to 5b for the first @n_src surfaces of @src and the first @n_dst of @dst. */
static RdsBufSurfTransform_Error
check_surfaces (const RdsBufSurface * src, guint n_src, const RdsBufSurface * dst, guint n_dst,
    const RdsBufSurfTransformConfigParams * session)
{
  /* 4. memory types */
  if (!is_device_accessible (src->memType) || !is_device_accessible (dst->memType)) {
    GST_ERROR ("memory types src %d / dst %d: both must be device-accessible (default, device, pinned, unified)",
        (gint) src->memType, (gint) dst->memType);
    return RdsBufSurfTransformError_Unsupported;
  }

  /* 4b. device memory must be on the session's GPU (pinned and unified memory is reachable from any GPU) */
  if (!check_gpu (src, session, "src") || !check_gpu (dst, session, "dst"))
    return RdsBufSurfTransformError_Invalid_Params;

  /* 5. formats and layouts */
  if (!check_formats (src, n_src, "src") || !check_formats (dst, n_dst, "dst"))
    return RdsBufSurfTransformError_Unsupported;

  /* 5b. sizes and plane geometry */
  if (!check_geometry (src, n_src, "src") || !check_geometry (dst, n_dst, "dst"))
    return RdsBufSurfTransformError_Invalid_Params;
  return RdsBufSurfTransformError_Success;
}

static gboolean
is_valid_filter (RdsBufSurfTransform_Inter filter)
{
  return (gint) filter >= RdsBufSurfTransformInter_Nearest && (gint) filter <= RdsBufSurfTransformInter_Default;
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

  /* 4. - 5b. */
  ret = check_surfaces (src, n, dst, n, session);
  if (ret != RdsBufSurfTransformError_Success)
    return ret;

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
  if ((tp->transform_flag & RDS_BUFSURF_TRANSFORM_FILTER) && !is_valid_filter (tp->transform_filter)) {
    GST_ERROR ("invalid filter %d", (gint) tp->transform_filter);
    return RdsBufSurfTransformError_Invalid_Params;
  }

  /* 9. src and dst must not overlap: the cheap check is the same pixel memory on both sides of an entry (NULL
   * pointers are rejected by the transform itself) */
  for (guint i = 0; i < n; i++) {
    gpointer sp = src->surfaceList[i].dataPtr;

    if (sp != NULL && sp == dst->surfaceList[i].dataPtr) {
      GST_ERROR ("surface %u: src and dst share the same pixel memory %p (they must not overlap)", i, sp);
      return RdsBufSurfTransformError_Invalid_Params;
    }
  }

  return RdsBufSurfTransformError_Success;
}

G_STATIC_ASSERT ((gint) RDS_KFMT_GRAY8 == (gint) RDS_BUF_COLOR_FORMAT_GRAY8);
G_STATIC_ASSERT ((gint) RDS_KFMT_YUV420 == (gint) RDS_BUF_COLOR_FORMAT_YUV420);
G_STATIC_ASSERT ((gint) RDS_KFMT_NV12 == (gint) RDS_BUF_COLOR_FORMAT_NV12);
G_STATIC_ASSERT ((gint) RDS_KFMT_RGBA == (gint) RDS_BUF_COLOR_FORMAT_RGBA);
G_STATIC_ASSERT ((gint) RDS_KFMT_BGRA == (gint) RDS_BUF_COLOR_FORMAT_BGRA);
G_STATIC_ASSERT ((gint) RDS_KFMT_RGBx == (gint) RDS_BUF_COLOR_FORMAT_RGBx);
G_STATIC_ASSERT ((gint) RDS_KFMT_BGRx == (gint) RDS_BUF_COLOR_FORMAT_BGRx);
G_STATIC_ASSERT ((gint) RDS_KFMT_RGB == (gint) RDS_BUF_COLOR_FORMAT_RGB);
G_STATIC_ASSERT ((gint) RDS_KFMT_BGR == (gint) RDS_BUF_COLOR_FORMAT_BGR);

static gint force_kernel = 0;

void
rds_transform_force_kernel (gboolean force)
{
  g_atomic_int_set (&force_kernel, force ? 1 : 0);
}

/* The filter mapping (rds_transform_filter_is_bilinear). */
static gboolean
use_bilinear (RdsBufSurfTransform_Inter filter)
{
  static gint warned = 0;

  if (filter >= RdsBufSurfTransformInter_Algo1 && filter <= RdsBufSurfTransformInter_Algo3
      && g_atomic_int_compare_and_exchange (&warned, 0, 1))
    GST_WARNING ("filters Algo1-Algo3 run as bilinear on this implementation");
  return rds_transform_filter_is_bilinear (filter);
}

/* Builds the job for surface @i: rects default to the whole surface; a 4:2:0 destination rect is aligned outward
 * to even coordinates, clamped to the surface (ALLOW_ODD_CROP; validation already refused odd rects without it). */
static void
build_job (const RdsBufSurface * src, const RdsBufSurface * dst, const RdsBufSurfTransformParams * tp, guint i,
    gboolean bilinear, RdsKernelJob * job)
{
  const RdsBufSurfaceParams *sp = &src->surfaceList[i], *dp = &dst->surfaceList[i];

  memset (job, 0, sizeof (*job));
  rds_kernel_surface_init (&job->src, sp);
  rds_kernel_surface_init (&job->dst, dp);
  job->bilinear = bilinear;

  if (tp->transform_flag & RDS_BUFSURF_TRANSFORM_CROP_SRC) {
    const RdsBufSurfTransformRect *r = &tp->src_rect[i];
    job->sl = r->left, job->st = r->top, job->sw = r->width, job->sh = r->height;
  } else {
    job->sl = 0, job->st = 0, job->sw = sp->width, job->sh = sp->height;
  }
  if (tp->transform_flag & RDS_BUFSURF_TRANSFORM_CROP_DST) {
    const RdsBufSurfTransformRect *r = &tp->dst_rect[i];
    job->dl = r->left, job->dt = r->top, job->dw = r->width, job->dh = r->height;
  } else {
    job->dl = 0, job->dt = 0, job->dw = dp->width, job->dh = dp->height;
  }
  if (is_yuv420 (dp->colorFormat)) {
    guint right = MIN (GST_ROUND_UP_2 (job->dl + job->dw), dp->width);
    guint bottom = MIN (GST_ROUND_UP_2 (job->dt + job->dh), dp->height);

    job->dl &= ~1u;
    job->dt &= ~1u;
    job->dw = right - job->dl;
    job->dh = bottom - job->dt;
  }
  job->cl = job->dl, job->ct = job->dt, job->cw = job->dw, job->ch = job->dh;
}

/* A same-format, same-size job whose result is a plain copy of the source rect: hipMemcpy2DAsync per plane gives
 * the bytes the kernel would write. Not for x formats (the kernel writes 255 there), and on 4:2:0 only from an
 * even source corner, so the chroma rect is a whole-sample copy too. */
static gboolean
is_plain_copy (const RdsKernelJob * job)
{
  if (g_atomic_int_get (&force_kernel) || job->src.format != job->dst.format
      || job->sw != job->dw || job->sh != job->dh
      || job->cl != job->dl || job->ct != job->dt || job->cw != job->dw || job->ch != job->dh)
    return FALSE;
  switch (job->dst.format) {
    case RDS_BUF_COLOR_FORMAT_RGBx:
    case RDS_BUF_COLOR_FORMAT_BGRx:
      return FALSE;
    case RDS_BUF_COLOR_FORMAT_NV12:
    case RDS_BUF_COLOR_FORMAT_YUV420:
      return (job->sl & 1) == 0 && (job->st & 1) == 0;
    default:
      return TRUE;
  }
}

static gboolean
copy_rect (const RdsKernelJob * job, hipStream_t stream)
{
  RdsBufSurfaceColorFormat fmt = (RdsBufSurfaceColorFormat) job->dst.format;

  for (guint p = 0; p < expected_planes (fmt); p++) {
    guint div = p == 0 ? 1 : 2;     /* only 4:2:0 formats have more than one plane */
    guint64 bpp, rows;
    const guint8 *s;
    guint8 *d;

    /* bytes per pixel from the format, not from the (unchecked) bytesPerPix of a hand-built surface */
    expected_plane_size (fmt, p, 1, 1, &bpp, &rows);
    s = job->src.data + job->src.offset[p] + (gsize) (job->st / div) * job->src.pitch[p]
        + (gsize) (job->sl / div) * bpp;
    d = job->dst.data + job->dst.offset[p] + (gsize) (job->dt / div) * job->dst.pitch[p]
        + (gsize) (job->dl / div) * bpp;

    if (!RDS_HIP_CHECK (hipMemcpy2DAsync (d, job->dst.pitch[p], s, job->src.pitch[p],
                (gsize) ((job->dw + div - 1) / div) * bpp, (job->dh + div - 1) / div, hipMemcpyDefault, stream)))
      return FALSE;
  }
  return TRUE;
}

/* Runs @n jobs in order on the session's stream and GPU and waits for them. */
static RdsBufSurfTransform_Error
run_jobs (const RdsKernelJob * jobs, guint n, const RdsBufSurfTransformConfigParams * session)
{
  RdsBufSurfTransform_Error ret = RdsBufSurfTransformError_Success;
  hipStream_t stream = (hipStream_t) session->cuda_stream;
  int prev_dev;

  if (!rds_device_enter ((guint32) session->gpu_id, &prev_dev)) {
    GST_ERROR ("cannot select GPU %d", session->gpu_id);
    return RdsBufSurfTransformError_Execution_Error;
  }
  for (guint i = 0; i < n && ret == RdsBufSurfTransformError_Success; i++) {
    if (is_plain_copy (&jobs[i])) {
      if (!copy_rect (&jobs[i], stream))
        ret = RdsBufSurfTransformError_Execution_Error;
    } else if (rds_transform_launch (&jobs[i], stream) != 0) {
      GST_ERROR ("kernel launch failed for job %u (library built for another GPU architecture?)", i);
      ret = RdsBufSurfTransformError_Execution_Error;
    }
  }
  /* Synchronize even after a failed launch, so no work is left running on the caller's surfaces. */
  if (!RDS_HIP_CHECK (hipStreamSynchronize (stream)))
    ret = RdsBufSurfTransformError_Execution_Error;
  rds_device_leave ((guint32) session->gpu_id, prev_dev);
  return ret;
}

RdsBufSurfTransform_Error
rds_buf_surf_transform (RdsBufSurface * src, RdsBufSurface * dst, RdsBufSurfTransformParams * transform_params)
{
  RdsBufSurfTransform_Error ret;
  RdsKernelJob *jobs;
  gboolean bilinear;

  ret = rds_transform_validate (src, dst, transform_params);
  if (ret != RdsBufSurfTransformError_Success)
    return ret;

  for (guint i = 0; i < src->numFilled; i++) {
    if (src->surfaceList[i].dataPtr == NULL || dst->surfaceList[i].dataPtr == NULL) {
      GST_ERROR ("surface %u has no pixel memory (dataPtr is NULL)", i);
      return RdsBufSurfTransformError_Invalid_Params;
    }
  }
  /* no FILTER flag runs as nearest */
  bilinear = (transform_params->transform_flag & RDS_BUFSURF_TRANSFORM_FILTER)
      && use_bilinear (transform_params->transform_filter);

  jobs = g_new (RdsKernelJob, src->numFilled);
  for (guint i = 0; i < src->numFilled; i++)
    build_job (src, dst, transform_params, i, bilinear, &jobs[i]);
  ret = run_jobs (jobs, src->numFilled, current_session ());
  g_free (jobs);

  if (ret == RdsBufSurfTransformError_Success)
    dst->numFilled = src->numFilled;
  return ret;
}

/* Checks the arguments of rds_buf_surf_transform_composite (), like rds_transform_validate () does for the
 * transform, plus the NULL pixel pointers. */
static RdsBufSurfTransform_Error
composite_validate (const RdsBufSurface * src, const RdsBufSurface * dst,
    const RdsBufSurfTransformCompositeParams * cp)
{
  const RdsBufSurfTransformConfigParams *session;
  RdsBufSurfTransform_Error ret;
  gpointer dp;
  guint n;

  g_return_val_if_fail (src != NULL, RdsBufSurfTransformError_Invalid_Params);
  g_return_val_if_fail (dst != NULL, RdsBufSurfTransformError_Invalid_Params);
  g_return_val_if_fail (cp != NULL, RdsBufSurfTransformError_Invalid_Params);
  ensure_debug_category ();
  session = current_session ();

  /* both rect arrays are required (DeepStream 8.0 behaviour) */
  if (cp->src_comp_rect == NULL || cp->dst_comp_rect == NULL) {
    GST_ERROR ("composite rect arrays src %p / dst %p: both are required", cp->src_comp_rect, cp->dst_comp_rect);
    return RdsBufSurfTransformError_Invalid_Params;
  }

  /* src->numFilled decides the entry count; input_buf_count is ignored (DeepStream 8.0 behaviour) */
  n = src->numFilled;
  if (n == 0 || n > src->batchSize || dst->batchSize == 0 || src->surfaceList == NULL || dst->surfaceList == NULL) {
    GST_ERROR ("invalid batch: src numFilled %u, src batchSize %u, dst batchSize %u, surface lists %p/%p",
        n, src->batchSize, dst->batchSize, src->surfaceList, dst->surfaceList);
    return RdsBufSurfTransformError_Invalid_Params;
  }
  if (session->compute_mode == RdsBufSurfTransformCompute_VIC) {
    GST_ERROR ("compute mode VIC is not available (NVIDIA Jetson only)");
    return RdsBufSurfTransformError_Unsupported;
  }
  ret = check_surfaces (src, n, dst, 1, session);
  if (ret != RdsBufSurfTransformError_Success)
    return ret;

  /* Rects are clipped, but their sizes must fit the kernel's arithmetic (ours). */
  for (guint i = 0; i < n; i++) {
    const RdsBufSurfTransformRect *s = &cp->src_comp_rect[i], *d = &cp->dst_comp_rect[i];

    if (s->width > RDS_BUF_SURFACE_MAX_DIM || s->height > RDS_BUF_SURFACE_MAX_DIM
        || d->width > RDS_BUF_SURFACE_MAX_DIM || d->height > RDS_BUF_SURFACE_MAX_DIM) {
      GST_ERROR ("composite entry %u: src rect %ux%u or dst rect %ux%u larger than %u", i, s->width, s->height,
          d->width, d->height, RDS_BUF_SURFACE_MAX_DIM);
      return RdsBufSurfTransformError_ROI_Error;
    }
  }

  if (!is_valid_filter (cp->composite_filter)) {
    GST_ERROR ("invalid composite filter %d", (gint) cp->composite_filter);
    return RdsBufSurfTransformError_Invalid_Params;
  }

  dp = dst->surfaceList[0].dataPtr;
  if (dp == NULL) {
    GST_ERROR ("dst surface 0 has no pixel memory (dataPtr is NULL)");
    return RdsBufSurfTransformError_Invalid_Params;
  }
  for (guint i = 0; i < n; i++) {
    gpointer sp = src->surfaceList[i].dataPtr;

    if (sp == NULL || sp == dp) {
      GST_ERROR ("src surface %u: pixel memory %p is NULL or the destination's", i, sp);
      return RdsBufSurfTransformError_Invalid_Params;
    }
  }
  return RdsBufSurfTransformError_Success;
}

/* Builds composite entry @i into surface 0 of @dst. Returns FALSE when the entry draws nothing: an empty source or
 * destination rect after clipping. The source rect is cut to its surface (ours); the destination rect keeps its
 * size, so the scaling is that of the whole rect, and only the window inside the surface is written. On 4:2:0 the
 * rect is aligned inward: left and top rounded up to even, an odd right or bottom edge rounded down unless it is
 * the surface edge (DeepStream 8.0 behaviour for the origin). */
static gboolean
build_composite_job (const RdsBufSurface * src, const RdsBufSurface * dst,
    const RdsBufSurfTransformCompositeParams * cp, guint i, gboolean bilinear, RdsKernelJob * job)
{
  const RdsBufSurfaceParams *sp = &src->surfaceList[i], *dp = &dst->surfaceList[0];
  const RdsBufSurfTransformRect *s = &cp->src_comp_rect[i], *d = &cp->dst_comp_rect[i];
  guint64 sl = MIN (s->left, sp->width), sr = MIN ((guint64) s->left + s->width, sp->width);
  guint64 st = MIN (s->top, sp->height), sb = MIN ((guint64) s->top + s->height, sp->height);
  guint64 dl = d->left, dr = (guint64) d->left + d->width;
  guint64 dt = d->top, db = (guint64) d->top + d->height;

  if (is_yuv420 (dp->colorFormat)) {
    dl = (dl + 1) & ~G_GUINT64_CONSTANT (1);
    dt = (dt + 1) & ~G_GUINT64_CONSTANT (1);
    if ((dr & 1) && dr != dp->width)
      dr &= ~G_GUINT64_CONSTANT (1);
    if ((db & 1) && db != dp->height)
      db &= ~G_GUINT64_CONSTANT (1);
  }
  if (sr <= sl || sb <= st || dr <= dl || db <= dt || dl >= dp->width || dt >= dp->height)
    return FALSE;

  memset (job, 0, sizeof (*job));
  rds_kernel_surface_init (&job->src, sp);
  rds_kernel_surface_init (&job->dst, dp);
  job->bilinear = bilinear;
  job->sl = sl, job->st = st, job->sw = sr - sl, job->sh = sb - st;
  job->dl = dl, job->dt = dt, job->dw = dr - dl, job->dh = db - dt;
  job->cl = dl, job->ct = dt, job->cw = MIN (dr, dp->width) - dl, job->ch = MIN (db, dp->height) - dt;
  return TRUE;
}

RdsBufSurfTransform_Error
rds_buf_surf_transform_composite (RdsBufSurface * src, RdsBufSurface * dst,
    RdsBufSurfTransformCompositeParams * composite_params)
{
  RdsBufSurfTransform_Error ret;
  RdsKernelJob *jobs;
  gboolean bilinear;
  guint n = 0;

  ret = composite_validate (src, dst, composite_params);
  if (ret != RdsBufSurfTransformError_Success)
    return ret;

  /* composite_flag is ignored: the filter is always read (DeepStream 8.0 behaviour) */
  bilinear = use_bilinear (composite_params->composite_filter);
  jobs = g_new (RdsKernelJob, src->numFilled);
  for (guint i = 0; i < src->numFilled; i++)
    if (build_composite_job (src, dst, composite_params, i, bilinear, &jobs[n]))
      n++;
  if (n > 0)
    ret = run_jobs (jobs, n, current_session ());
  g_free (jobs);
  return ret;
}
