/* Redstream: buffer pool of memory:RDS buffers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-buffer-pool.h"
#include "rds-buf-surface-private.h"

GST_DEBUG_CATEGORY_STATIC (rds_buffer_pool_debug);
#define GST_CAT_DEFAULT rds_buffer_pool_debug

struct _RdsBufferPool
{
  GstBufferPool parent;
  GstAllocator *allocator;
  GstVideoInfo info;
  RdsBufSurfaceCreateParams params;
  guint batch_size;
  gboolean add_video_meta;
};

G_DEFINE_TYPE (RdsBufferPool, rds_buffer_pool, GST_TYPE_BUFFER_POOL);

#define CONFIG_BATCH_SIZE "rds-batch-size"
#define CONFIG_MEM_TYPE "rds-mem-type"
#define CONFIG_GPU_ID "rds-gpu-id"

/* The allocation helpers can log before any pool exists, so the category is created lazily. */
static void
ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (rds_buffer_pool_debug, "rdsbufferpool", 0, "Redstream memory:RDS buffer pool");
    g_once_init_leave (&initialized, 1);
  }
}

/* D3: single frames are always filled; batches are filled by the producer (the muxer). */
static guint32
initial_num_filled (RdsBufferPool * self)
{
  return self->batch_size == 1 ? 1 : 0;
}

static const gchar **
rds_buffer_pool_get_options (G_GNUC_UNUSED GstBufferPool * pool)
{
  static const gchar *options[] = { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };

  return options;
}

static gboolean
rds_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  RdsBufferPool *self = RDS_BUFFER_POOL (pool);
  GstCaps *caps = NULL;
  guint size, min, max;
  guint batch_size = 1, gpu_id = 0;
  RdsBufSurfaceMemType mem_type = RDS_BUF_MEM_DEFAULT;
  RdsBufSurfaceColorFormat color_format;
  GstVideoInfo info;

  if (!gst_buffer_pool_config_get_params (config, &caps, &size, &min, &max) || caps == NULL) {
    GST_WARNING_OBJECT (self, "config has no caps");
    return FALSE;
  }
  if (!gst_video_info_from_caps (&info, caps)) {
    GST_WARNING_OBJECT (self, "invalid video caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
  color_format = rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&info));
  if (color_format == RDS_BUF_COLOR_FORMAT_INVALID) {
    GST_WARNING_OBJECT (self, "unsupported format %s", GST_VIDEO_INFO_NAME (&info));
    return FALSE;
  }

  if (GST_VIDEO_INFO_WIDTH (&info) < 1 || GST_VIDEO_INFO_WIDTH (&info) > RDS_BUF_SURFACE_MAX_DIM
      || GST_VIDEO_INFO_HEIGHT (&info) < 1 || GST_VIDEO_INFO_HEIGHT (&info) > RDS_BUF_SURFACE_MAX_DIM) {
    GST_WARNING_OBJECT (self, "size %dx%d out of range (1..%u)", GST_VIDEO_INFO_WIDTH (&info),
        GST_VIDEO_INFO_HEIGHT (&info), RDS_BUF_SURFACE_MAX_DIM);
    return FALSE;
  }

  if (!rds_buffer_pool_config_get_surface_params (config, &batch_size, &mem_type, &gpu_id)
      && (gst_structure_has_field (config, CONFIG_BATCH_SIZE) || gst_structure_has_field (config, CONFIG_MEM_TYPE)
          || gst_structure_has_field (config, CONFIG_GPU_ID)))
    GST_DEBUG_OBJECT (self, "config sets only some surface parameters (or with wrong types); using the defaults");
  if (batch_size == 0) {
    GST_WARNING_OBJECT (self, "batch size must be at least 1");
    return FALSE;
  }
  if (!rds_buf_mem_type_is_supported (mem_type)) {
    GST_WARNING_OBJECT (self, "unsupported memory type %d", mem_type);
    return FALSE;
  }

  self->info = info;
  self->batch_size = batch_size;
  self->params = (RdsBufSurfaceCreateParams) {
    .gpuId = gpu_id,
    .width = GST_VIDEO_INFO_WIDTH (&self->info),
    .height = GST_VIDEO_INFO_HEIGHT (&self->info),
    .colorFormat = color_format,
    .layout = RDS_BUF_LAYOUT_PITCH,
    .memType = mem_type,
  };
  gst_clear_object (&self->allocator);
  self->allocator = rds_allocator_new (gpu_id, mem_type);
  self->add_video_meta = gst_buffer_pool_config_has_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);

  gst_buffer_pool_config_set_params (config, caps, sizeof (RdsBufSurface), min, max);
  gst_buffer_pool_config_set_allocator (config, self->allocator, NULL);

  GST_DEBUG_OBJECT (self, "%ux%u %s, batch %u, memory type %d, GPU %u", self->params.width, self->params.height,
      GST_VIDEO_INFO_NAME (&self->info), batch_size, mem_type, gpu_id);

  return GST_BUFFER_POOL_CLASS (rds_buffer_pool_parent_class)->set_config (pool, config);
}

static GstFlowReturn
rds_buffer_pool_alloc_buffer (GstBufferPool * pool, GstBuffer ** buffer,
    G_GNUC_UNUSED GstBufferPoolAcquireParams * params)
{
  RdsBufferPool *self = RDS_BUFFER_POOL (pool);
  GstMemory *mem;
  RdsBufSurface *surf;
  GstBuffer *buf;

  mem = rds_allocator_alloc_surface (self->allocator, self->batch_size, &self->params);
  if (mem == NULL) {
    GST_ERROR_OBJECT (self, "failed to allocate a surface");
    return GST_FLOW_ERROR;
  }
  surf = rds_memory_get_surface (mem);
  surf->numFilled = initial_num_filled (self);

  buf = gst_buffer_new ();
  gst_buffer_append_memory (buf, mem);

  if (self->add_video_meta && self->batch_size == 1) {
    const RdsBufSurfacePlaneParams *pp = &surf->surfaceList[0].planeParams;
    gsize offset[GST_VIDEO_MAX_PLANES] = { 0, };
    gint stride[GST_VIDEO_MAX_PLANES] = { 0, };

    for (guint p = 0; p < pp->num_planes && p < GST_VIDEO_MAX_PLANES; p++) {
      offset[p] = pp->offset[p];
      stride[p] = pp->pitch[p];
    }
    gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_INFO_FORMAT (&self->info),
        GST_VIDEO_INFO_WIDTH (&self->info), GST_VIDEO_INFO_HEIGHT (&self->info), pp->num_planes, offset, stride);
  }

  *buffer = buf;
  return GST_FLOW_OK;
}

static void
rds_buffer_pool_reset_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  RdsBufferPool *self = RDS_BUFFER_POOL (pool);
  RdsBufSurface *surf;

  GST_BUFFER_POOL_CLASS (rds_buffer_pool_parent_class)->reset_buffer (pool, buffer);

  /* A surface still shared with another buffer is not ours to reset; the base class then drops this buffer. */
  if (!gst_buffer_is_all_memory_writable (buffer))
    return;
  surf = rds_buffer_get_surface (buffer);
  if (surf != NULL)
    surf->numFilled = initial_num_filled (self);
}

static void
rds_buffer_pool_finalize (GObject * object)
{
  RdsBufferPool *self = RDS_BUFFER_POOL (object);

  gst_clear_object (&self->allocator);

  G_OBJECT_CLASS (rds_buffer_pool_parent_class)->finalize (object);
}

static void
rds_buffer_pool_class_init (RdsBufferPoolClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBufferPoolClass *pool_class = GST_BUFFER_POOL_CLASS (klass);

  gobject_class->finalize = rds_buffer_pool_finalize;
  pool_class->get_options = rds_buffer_pool_get_options;
  pool_class->set_config = rds_buffer_pool_set_config;
  pool_class->alloc_buffer = rds_buffer_pool_alloc_buffer;
  pool_class->reset_buffer = rds_buffer_pool_reset_buffer;

  ensure_debug_category ();
}

static void
rds_buffer_pool_init (RdsBufferPool * self)
{
  self->batch_size = 1;
}

GstBufferPool *
rds_buffer_pool_new (void)
{
  return GST_BUFFER_POOL_CAST (gst_object_ref_sink (g_object_new (RDS_TYPE_BUFFER_POOL, NULL)));
}

void
rds_buffer_pool_config_set_surface_params (GstStructure * config, guint batch_size,
    RdsBufSurfaceMemType mem_type, guint gpu_id)
{
  g_return_if_fail (config != NULL);

  gst_structure_set (config, CONFIG_BATCH_SIZE, G_TYPE_UINT, batch_size, CONFIG_MEM_TYPE, G_TYPE_INT,
      (gint) mem_type, CONFIG_GPU_ID, G_TYPE_UINT, gpu_id, NULL);
}

gboolean
rds_buffer_pool_config_get_surface_params (const GstStructure * config, guint * batch_size,
    RdsBufSurfaceMemType * mem_type, guint * gpu_id)
{
  guint b, g;
  gint m;

  g_return_val_if_fail (config != NULL, FALSE);

  if (!gst_structure_get_uint (config, CONFIG_BATCH_SIZE, &b) || !gst_structure_get_int (config, CONFIG_MEM_TYPE, &m)
      || !gst_structure_get_uint (config, CONFIG_GPU_ID, &g))
    return FALSE;

  if (batch_size)
    *batch_size = b;
  if (mem_type)
    *mem_type = (RdsBufSurfaceMemType) m;
  if (gpu_id)
    *gpu_id = g;
  return TRUE;
}

static gboolean
configure_pool (GstBufferPool * pool, GstCaps * caps, guint min, guint max, guint batch_size,
    RdsBufSurfaceMemType mem_type, guint gpu_id, gboolean video_meta)
{
  GstStructure *config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, caps, sizeof (RdsBufSurface), min, max);
  rds_buffer_pool_config_set_surface_params (config, batch_size, mem_type, gpu_id);
  if (video_meta)
    gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  return gst_buffer_pool_set_config (pool, config);
}

static void
add_meta_once (GstQuery * query, GType api)
{
  if (!gst_query_find_allocation_meta (query, api, NULL))
    gst_query_add_allocation_meta (query, api, NULL);
}

gboolean
rds_propose_allocation (GstQuery * query, guint batch_size, RdsBufSurfaceMemType mem_type, guint gpu_id,
    guint min_buffers)
{
  GstCaps *caps = NULL;
  gboolean need_pool = FALSE;
  GstBufferPool *pool;

  g_return_val_if_fail (GST_IS_QUERY (query) && GST_QUERY_TYPE (query) == GST_QUERY_ALLOCATION, FALSE);
  ensure_debug_category ();

  gst_query_parse_allocation (query, &caps, &need_pool);
  if (caps == NULL) {
    GST_DEBUG ("allocation query without caps");
    return FALSE;
  }

  add_meta_once (query, GST_VIDEO_META_API_TYPE);
  add_meta_once (query, GST_VIDEO_CROP_META_API_TYPE);

  if (!need_pool || !rds_caps_has_memory_rds (caps))
    return TRUE;

  pool = rds_buffer_pool_new ();
  if (!configure_pool (pool, caps, min_buffers, 0, batch_size, mem_type, gpu_id, TRUE)) {
    GST_WARNING_OBJECT (pool, "cannot configure a pool for %" GST_PTR_FORMAT, caps);
    gst_object_unref (pool);
    return FALSE;
  }
  gst_query_add_allocation_pool (query, pool, sizeof (RdsBufSurface), min_buffers, 0);
  gst_object_unref (pool);
  return TRUE;
}

/* An inactive RdsBufferPool configured with exactly these surface params. */
static gboolean
pool_matches (GstBufferPool * pool, guint batch_size, RdsBufSurfaceMemType mem_type, guint gpu_id)
{
  GstStructure *config;
  guint b, g;
  RdsBufSurfaceMemType m;
  gboolean ok;

  if (pool == NULL || !RDS_IS_BUFFER_POOL (pool) || gst_buffer_pool_is_active (pool))
    return FALSE;
  config = gst_buffer_pool_get_config (pool);
  ok = rds_buffer_pool_config_get_surface_params (config, &b, &m, &g)
      && b == batch_size && m == mem_type && g == gpu_id;
  gst_structure_free (config);
  return ok;
}

GstBufferPool *
rds_decide_allocation (GstQuery * query, guint batch_size, RdsBufSurfaceMemType mem_type, guint gpu_id,
    guint min_buffers, guint max_buffers)
{
  GstCaps *caps = NULL;
  GstBufferPool *pool = NULL;
  guint n_pools, min = 0, max = 0;

  g_return_val_if_fail (GST_IS_QUERY (query) && GST_QUERY_TYPE (query) == GST_QUERY_ALLOCATION, NULL);
  ensure_debug_category ();

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps == NULL) {
    GST_DEBUG ("allocation query without caps");
    return NULL;
  }

  n_pools = gst_query_get_n_allocation_pools (query);
  for (guint i = 0; i < n_pools && pool == NULL; i++) {
    GstBufferPool *candidate = NULL;
    guint cmin, cmax;

    gst_query_parse_nth_allocation_pool (query, i, &candidate, NULL, &cmin, &cmax);
    if (pool_matches (candidate, batch_size, mem_type, gpu_id)) {
      pool = candidate;
      min = cmin;
      max = cmax;
    } else if (candidate != NULL) {
      gst_object_unref (candidate);
    }
  }
  if (pool == NULL) {
    /* Downstream's buffer counts still apply to whatever pool upstream uses. */
    if (n_pools > 0)
      gst_query_parse_nth_allocation_pool (query, 0, NULL, NULL, &min, &max);
    pool = rds_buffer_pool_new ();
  }

  min = MAX (min, min_buffers);
  if (max_buffers != 0 && (max == 0 || max > max_buffers))
    max = max_buffers;
  if (max != 0 && max < min)
    max = min;

  if (!configure_pool (pool, caps, min, max, batch_size, mem_type, gpu_id,
          gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL))) {
    GST_WARNING_OBJECT (pool, "cannot configure a pool for %" GST_PTR_FORMAT, caps);
    gst_object_unref (pool);
    return NULL;
  }

  if (n_pools > 0)
    gst_query_set_nth_allocation_pool (query, 0, pool, sizeof (RdsBufSurface), min, max);
  else
    gst_query_add_allocation_pool (query, pool, sizeof (RdsBufSurface), min, max);
  return pool;
}
