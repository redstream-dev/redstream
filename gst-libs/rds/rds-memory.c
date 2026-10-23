/* Redstream: memory:RDS allocator and memory.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-memory.h"

GST_DEBUG_CATEGORY_STATIC (rds_memory_debug);
#define GST_CAT_DEFAULT rds_memory_debug

typedef struct
{
  GstMemory mem;
  RdsBufSurface *surface;       /* owned */
} RdsMemory;

struct _RdsAllocator
{
  GstAllocator parent;
  guint gpu_id;
  RdsBufSurfaceMemType mem_type;
};

G_DEFINE_TYPE (RdsAllocator, rds_allocator, GST_TYPE_ALLOCATOR);

static const struct
{
  GstVideoFormat video;
  RdsBufSurfaceColorFormat rds;
} format_map[] = {
  {GST_VIDEO_FORMAT_GRAY8, RDS_BUF_COLOR_FORMAT_GRAY8},
  {GST_VIDEO_FORMAT_I420, RDS_BUF_COLOR_FORMAT_YUV420},
  {GST_VIDEO_FORMAT_NV12, RDS_BUF_COLOR_FORMAT_NV12},
  {GST_VIDEO_FORMAT_RGBA, RDS_BUF_COLOR_FORMAT_RGBA},
  {GST_VIDEO_FORMAT_BGRA, RDS_BUF_COLOR_FORMAT_BGRA},
  {GST_VIDEO_FORMAT_RGBx, RDS_BUF_COLOR_FORMAT_RGBx},
  {GST_VIDEO_FORMAT_BGRx, RDS_BUF_COLOR_FORMAT_BGRx},
  {GST_VIDEO_FORMAT_RGB, RDS_BUF_COLOR_FORMAT_RGB},
  {GST_VIDEO_FORMAT_BGR, RDS_BUF_COLOR_FORMAT_BGR},
};

static GstMemory *
rds_allocator_alloc (GstAllocator * allocator, G_GNUC_UNUSED gsize size,
    G_GNUC_UNUSED GstAllocationParams * params)
{
  GST_WARNING_OBJECT (allocator, "gst_allocator_alloc() is not supported, use rds_allocator_alloc_surface()");
  return NULL;
}

static void
rds_allocator_free (G_GNUC_UNUSED GstAllocator * allocator, GstMemory * mem)
{
  RdsMemory *m = (RdsMemory *) mem;

  rds_buf_surface_destroy (m->surface);
  g_free (m);
}

static gpointer
rds_memory_map (GstMemory * mem, G_GNUC_UNUSED gsize maxsize, G_GNUC_UNUSED GstMapFlags flags)
{
  return ((RdsMemory *) mem)->surface;
}

static void
rds_memory_unmap (G_GNUC_UNUSED GstMemory * mem)
{
}

static GstMemory *
rds_memory_copy (GstMemory * mem, gssize offset, gssize size)
{
  const RdsBufSurface *src = ((RdsMemory *) mem)->surface;
  const RdsBufSurfaceParams *sp = &src->surfaceList[0];
  RdsBufSurfaceCreateParams params = {
    .width = sp->width,
    .height = sp->height,
    .size = sp->dataSize,
    .colorFormat = sp->colorFormat,
    .layout = sp->layout,
  };
  GstMemory *copy;

  if (offset != 0 || (size != -1 && (gsize) size != mem->size)) {
    GST_WARNING_OBJECT (mem->allocator, "partial copies of RDS memory are not supported");
    return NULL;
  }

  copy = rds_allocator_alloc_surface (mem->allocator, src->batchSize, &params);
  if (copy == NULL)
    return NULL;

  if (rds_buf_surface_copy (src, ((RdsMemory *) copy)->surface) != 0) {
    gst_memory_unref (copy);
    return NULL;
  }
  return copy;
}

static GstMemory *
rds_memory_share (G_GNUC_UNUSED GstMemory * mem, G_GNUC_UNUSED gssize offset, G_GNUC_UNUSED gssize size)
{
  return NULL;
}

static void
rds_allocator_class_init (RdsAllocatorClass * klass)
{
  GstAllocatorClass *alloc_class = GST_ALLOCATOR_CLASS (klass);

  alloc_class->alloc = rds_allocator_alloc;
  alloc_class->free = rds_allocator_free;

  GST_DEBUG_CATEGORY_INIT (rds_memory_debug, "rdsmemory", 0, "Redstream memory:RDS allocator");
}

static void
rds_allocator_init (RdsAllocator * self)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (self);

  alloc->mem_type = RDS_MEMORY_TYPE;
  alloc->mem_map = rds_memory_map;
  alloc->mem_unmap = rds_memory_unmap;
  alloc->mem_copy = rds_memory_copy;
  alloc->mem_share = rds_memory_share;

  GST_OBJECT_FLAG_SET (self, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

GstAllocator *
rds_allocator_new (guint gpu_id, RdsBufSurfaceMemType mem_type)
{
  RdsAllocator *self = g_object_new (RDS_TYPE_ALLOCATOR, NULL);

  self->gpu_id = gpu_id;
  self->mem_type = mem_type;
  return GST_ALLOCATOR_CAST (gst_object_ref_sink (self));
}

GstMemory *
rds_allocator_alloc_surface (GstAllocator * allocator, guint32 batch_size,
    const RdsBufSurfaceCreateParams * params)
{
  RdsAllocator *self;
  RdsBufSurfaceCreateParams p;
  RdsBufSurface *surface = NULL;
  RdsMemory *m;

  g_return_val_if_fail (RDS_IS_ALLOCATOR (allocator), NULL);
  g_return_val_if_fail (params != NULL, NULL);

  self = RDS_ALLOCATOR (allocator);
  p = *params;
  p.gpuId = self->gpu_id;
  p.memType = self->mem_type;
  if (rds_buf_surface_create (&surface, batch_size, &p) != 0) {
    GST_ERROR_OBJECT (self, "failed to create a %ux%u surface batch of %u", p.width, p.height, batch_size);
    return NULL;
  }

  m = g_new0 (RdsMemory, 1);
  m->surface = surface;
  /* Not NO_SHARE: gst_buffer_copy() (and so gst_buffer_make_writable()) then refs the surface instead of
   * copying it. Sub-region shares are refused by mem_share. */
  gst_memory_init (GST_MEMORY_CAST (m), 0, allocator, NULL,
      sizeof (RdsBufSurface), 0, 0, sizeof (RdsBufSurface));
  return GST_MEMORY_CAST (m);
}

gboolean
rds_is_rds_memory (GstMemory * mem)
{
  return mem != NULL && gst_memory_is_type (mem, RDS_MEMORY_TYPE);
}

RdsBufSurface *
rds_memory_get_surface (GstMemory * mem)
{
  if (!rds_is_rds_memory (mem))
    return NULL;
  return ((RdsMemory *) mem)->surface;
}

RdsBufSurface *
rds_buffer_get_surface (GstBuffer * buffer)
{
  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);

  if (gst_buffer_n_memory (buffer) != 1)
    return NULL;
  return rds_memory_get_surface (gst_buffer_peek_memory (buffer, 0));
}

gboolean
rds_caps_has_memory_rds (const GstCaps * caps)
{
  g_return_val_if_fail (GST_IS_CAPS (caps), FALSE);

  if (gst_caps_get_size (caps) == 0)
    return FALSE;
  return gst_caps_features_contains (gst_caps_get_features (caps, 0), RDS_CAPS_FEATURE_MEMORY_RDS);
}

RdsBufSurfaceColorFormat
rds_buf_color_format_from_video_format (GstVideoFormat format)
{
  for (gsize i = 0; i < G_N_ELEMENTS (format_map); i++) {
    if (format_map[i].video == format)
      return format_map[i].rds;
  }
  return RDS_BUF_COLOR_FORMAT_INVALID;
}

GstVideoFormat
rds_video_format_from_buf_color_format (RdsBufSurfaceColorFormat format)
{
  for (gsize i = 0; i < G_N_ELEMENTS (format_map); i++) {
    if (format_map[i].rds == format)
      return format_map[i].video;
  }
  return GST_VIDEO_FORMAT_UNKNOWN;
}
