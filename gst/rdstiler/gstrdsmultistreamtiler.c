/* Redstream: rdsmultistreamtiler, composites the frames of a memory:RDS batch into one tiled frame with
 * rds_buf_surf_transform_composite. Property names, ranges and defaults follow DeepStream's nvmultistreamtiler;
 * behaviour the docs leave open matches DeepStream 8.0 ("DeepStream 8.0 behaviour" below) except where marked
 * "ours". Every such choice has a row in docs/compat-matrix.md.
 *
 * Pixels: the element keeps a canvas, the last tiled grid. Each batch composites its frames into their cells
 * (frame meta pad_index -> cell, pixels from the slot at batch_id), so a source missing from a batch keeps its last
 * pixels until its rds-stream-eos clears the cell; the output buffer gets a copy of the canvas. With show-source the
 * chosen frame is composited straight into the output instead (the canvas is still updated).
 *
 * Meta: the output carries one fresh frame meta holding a copy of every object and display meta of the batch,
 * scaled into the cells. Each source's last frame meta is kept too (one copy per pad), so an absent source's meta
 * repeats with its pixels.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsmultistreamtiler.h"
#include "rds-tiler-layout.h"

#include <gst/video/video.h>
#include <rds/rds.h>
#include <string.h>
/* after GLib/GStreamer: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_multi_stream_tiler_debug);
#define GST_CAT_DEFAULT gst_rds_multi_stream_tiler_debug

/* defaults as gst-inspect shows them for DeepStream 8.0 */
#define DEFAULT_ROWS 1
#define DEFAULT_COLUMNS 1
#define DEFAULT_WIDTH 1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_SHOW_SOURCE -1
#define DEFAULT_GPU_ID 0
#define DEFAULT_MEMORY_TYPE RDS_BUF_MEM_HIP_DEVICE
#define DEFAULT_COMPUTE_HW 0
#define DEFAULT_INTERPOLATION_METHOD RdsBufSurfTransformInter_Default
#define DEFAULT_SQUARE_SEQ_GRID FALSE
#define DEFAULT_BUFFER_POOL_SIZE 5
#define MIN_BUFFER_POOL_SIZE 4
#define MAX_BUFFER_POOL_SIZE 10
#define MIN_OUTPUT_SIZE 16

#define COMPUTE_HW_VIC 2
/* the black source composited into cells to clear them */
#define BLACK_SIZE 16

enum
{
  PROP_0,
  PROP_ROWS,
  PROP_COLUMNS,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_SHOW_SOURCE,
  PROP_GPU_ID,
  PROP_NVBUF_MEMORY_TYPE,
  PROP_COMPUTE_HW,
  PROP_INTERPOLATION_METHOD,
  PROP_SQUARE_SEQ_GRID,
  PROP_BUFFER_POOL_SIZE,
  PROP_CUSTOM_TILE_CONFIG,
};

/* What the element keeps of one pad (source). */
typedef struct
{
  gboolean cached;              /* its last pixels are in the canvas and its last meta in @meta */
  gboolean clear;               /* ended: its cell is cleared on the next batch */
  RdsBatchMeta *meta;           /* holds one frame: a copy of the pad's last frame meta (created on first use) */
  guint frame_width, frame_height;      /* the size of that frame's slot */
} PadState;

struct _GstRdsMultiStreamTiler
{
  GstBaseTransform parent;

  /* properties (object lock) */
  guint rows;
  guint columns;
  guint width;
  guint height;
  gint show_source;
  guint gpu_id;
  gint memory_type;
  gint compute_hw;
  gint interpolation_method;
  gboolean square_seq_grid;
  guint buffer_pool_size;
  gpointer custom_tile_config;  /* reserved: stored, no effect */
  gboolean custom_tile_config_warned;

  /* streaming state (streaming thread, or stopped) */
  hipStream_t stream;
  guint stream_gpu;
  GstVideoInfo out_info;
  RdsBufSurfaceColorFormat format;
  guint n_pads;                 /* highest pad index seen + 1 */
  GPtrArray *pads;              /* PadState, by pad index, n_pads long */
  RdsTilerLayout layout;        /* the canvas's layout, valid when canvas != NULL */
  RdsBufSurface *canvas;
  RdsBufSurface *black;
  gboolean out_of_grid_warned;
  GArray *pending_pads;         /* guint: pads added before the caps, grown into at set_caps */
};

#define RDS_TILER_FORMATS "{ NV12, RGBA }"
#define RDS_TILER_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), format = (string) " RDS_TILER_FORMATS ", " \
  "width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_TILER_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_TILER_CAPS));

/* Enum integers and nicks as in DeepStream (configs use the integers); descriptions are our own. The type names
 * are the tiler's own: other plugins register theirs, and a GType name can be registered once per process. */
#define GST_TYPE_RDS_TILER_MEMORY_TYPE (gst_rds_tiler_memory_type_get_type ())
static GType
gst_rds_tiler_memory_type_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {0, "Default memory for the platform (HIP device memory)", "nvbuf-mem-default"},
    {1, "Page-locked host memory", "nvbuf-mem-cuda-pinned"},
    {2, "HIP device memory", "nvbuf-mem-cuda-device"},
    {3, "HIP managed (unified) memory", "nvbuf-mem-cuda-unified"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsTilerMemoryType", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

#define GST_TYPE_RDS_TILER_INTERPOLATION_METHOD (gst_rds_tiler_interpolation_method_get_type ())
static GType
gst_rds_tiler_interpolation_method_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {RdsBufSurfTransformInter_Nearest, "Nearest neighbour", "nearest"},
    {RdsBufSurfTransformInter_Bilinear, "Bilinear", "bilinear"},
    {RdsBufSurfTransformInter_Algo1, "Algorithm 1 (bilinear here)", "algo-1"},
    {RdsBufSurfTransformInter_Algo2, "Algorithm 2 (bilinear here)", "algo-2"},
    {RdsBufSurfTransformInter_Algo3, "Algorithm 3 (bilinear here)", "algo-3"},
    {RdsBufSurfTransformInter_Algo4, "Algorithm 4 (nearest here)", "algo-4"},
    {RdsBufSurfTransformInter_Default, "Platform default (nearest)", "default"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsTilerInterpolationMethod", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

#define GST_TYPE_RDS_TILER_COMPUTE_HW (gst_rds_tiler_compute_hw_get_type ())
static GType
gst_rds_tiler_compute_hw_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {0, "Platform default (the GPU)", "default"},
    {1, "GPU", "gpu"},
    {COMPUTE_HW_VIC, "NVIDIA Jetson VIC engine (not available: fails at start)", "vic"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsTilerComputeHW", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

G_DEFINE_TYPE (GstRdsMultiStreamTiler, gst_rds_multi_stream_tiler, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdsmultistreamtiler, "rdsmultistreamtiler", GST_RANK_NONE,
    GST_TYPE_RDS_MULTI_STREAM_TILER);

/* ---- per-pad state ---- */

static void
pad_state_free (gpointer data)
{
  PadState *ps = data;

  if (ps->meta != NULL)
    rds_destroy_batch_meta (ps->meta);
  g_free (ps);
}

static PadState *
pad_state (GstRdsMultiStreamTiler * self, guint pad)
{
  return pad < self->pads->len ? g_ptr_array_index (self->pads, pad) : NULL;
}

/* Drops the pad's cached meta (the batch meta stays for reuse). */
static void
pad_state_forget (PadState * ps)
{
  if (ps->meta != NULL)
    while (ps->meta->frame_meta_list != NULL)
      rds_remove_frame_meta_from_batch (ps->meta, ps->meta->frame_meta_list->data);
  ps->cached = FALSE;
}

/* The pad's cached frame meta, or NULL. */
static RdsFrameMeta *
pad_state_frame (PadState * ps)
{
  return ps->cached && ps->meta != NULL && ps->meta->frame_meta_list != NULL ? ps->meta->frame_meta_list->data :
      NULL;
}

/* The layout for @n pads on the negotiated output, or FALSE when its cells would vanish. */
static gboolean
layout_for (GstRdsMultiStreamTiler * self, guint n, RdsTilerLayout * layout)
{
  guint rows, columns;
  gboolean square;

  GST_OBJECT_LOCK (self);
  rows = self->rows;
  columns = self->columns;
  square = self->square_seq_grid;
  GST_OBJECT_UNLOCK (self);
  return rds_tiler_layout_init (layout, rows, columns, square, n, GST_VIDEO_INFO_WIDTH (&self->out_info),
      GST_VIDEO_INFO_HEIGHT (&self->out_info));
}

/* Grows n_pads to hold @pad: n is the highest pad index seen + 1, from rds-pad-added events (DeepStream 8.0
 * behaviour) and from frame metas (ours: DeepStream 8.0 fails on a pad index only a frame meta has). A pad the grid
 * cannot hold (its cells would vanish) is refused with one warning (ours: DeepStream 8.0 fails with a fatal composite
 * error). FALSE if refused. Before the caps (no output size yet) the pad waits in pending_pads until set_caps. */
static gboolean
grow_for_pad (GstRdsMultiStreamTiler * self, guint pad)
{
  RdsTilerLayout l;

  if (pad < self->n_pads)
    return TRUE;
  if (GST_VIDEO_INFO_WIDTH (&self->out_info) == 0) {
    g_array_append_val (self->pending_pads, pad);
    return TRUE;
  }
  if (pad == G_MAXUINT || !layout_for (self, pad + 1, &l)) {
    if (!self->out_of_grid_warned) {
      self->out_of_grid_warned = TRUE;
      GST_ELEMENT_WARNING (self, STREAM, FAILED, ("Source pad %u does not fit in the tiled output; its frames are "
              "skipped", pad), ("a grid for %u pads on %dx%d would have empty cells (logged once)", pad + 1,
              GST_VIDEO_INFO_WIDTH (&self->out_info), GST_VIDEO_INFO_HEIGHT (&self->out_info)));
    }
    return FALSE;
  }
  while (self->pads->len <= pad)
    g_ptr_array_add (self->pads, g_new0 (PadState, 1));
  self->n_pads = pad + 1;
  return TRUE;
}

/* ---- GPU helpers ---- */

static const gchar *
transform_error_name (RdsBufSurfTransform_Error err)
{
  switch (err) {
    case RdsBufSurfTransformError_ROI_Error:
      return "ROI_Error";
    case RdsBufSurfTransformError_Invalid_Params:
      return "Invalid_Params";
    case RdsBufSurfTransformError_Execution_Error:
      return "Execution_Error";
    case RdsBufSurfTransformError_Unsupported:
      return "Unsupported";
    case RdsBufSurfTransformError_Success:
      return "Success";
  }
  return "unknown error";
}

/* Composites with our stream set in this thread's transform session, restoring the previous session after (the
 * session is per thread and the streaming thread is shared with other elements). */
static gboolean
composite (GstRdsMultiStreamTiler * self, RdsBufSurface * src, RdsBufSurface * dst,
    RdsBufSurfTransformRect * src_rects, RdsBufSurfTransformRect * dst_rects, RdsBufSurfTransform_Inter filter)
{
  RdsBufSurfTransformConfigParams session = {
    .compute_mode = RdsBufSurfTransformCompute_GPU,
    .gpu_id = (gint32) self->stream_gpu,
    .cuda_stream = self->stream,
  };
  RdsBufSurfTransformConfigParams prev;
  RdsBufSurfTransformCompositeParams cp = {
    .src_comp_rect = src_rects,
    .dst_comp_rect = dst_rects,
    .composite_filter = filter,
  };
  RdsBufSurfTransform_Error err;

  (void) rds_buf_surf_transform_get_session_params (&prev);
  err = rds_buf_surf_transform_set_session_params (&session);
  if (err == RdsBufSurfTransformError_Success) {
    err = rds_buf_surf_transform_composite (src, dst, &cp);
    if (rds_buf_surf_transform_set_session_params (&prev) != RdsBufSurfTransformError_Success)
      GST_WARNING_OBJECT (self, "cannot restore the thread's previous transform session");
  }
  if (err != RdsBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Tiling failed"),
        ("rds_buf_surf_transform_composite: %s (%d)", transform_error_name (err), err));
    return FALSE;
  }
  return TRUE;
}

/* Black: Y 16, U/V 128 for NV12; every byte 0 for RGBA (alpha too, as rdsvideoconvert's dest-crop fill). */
static gboolean
fill_black (RdsBufSurface * surf)
{
  RdsBufSurfaceParams *p = &surf->surfaceList[0];
  gboolean yuv = p->colorFormat == RDS_BUF_COLOR_FORMAT_NV12;

  for (guint plane = 0; plane < p->planeParams.num_planes; plane++)
    if (rds_buf_surface_memset (surf, 0, (int) plane, yuv ? (plane == 0 ? 16 : 128) : 0) != 0)
      return FALSE;
  return TRUE;
}

/* A black device surface of the output format, @w x @h, or NULL. */
static RdsBufSurface *
new_black_surface (GstRdsMultiStreamTiler * self, guint w, guint h)
{
  RdsBufSurfaceCreateParams cp = {
    .gpuId = self->stream_gpu,
    .width = w,
    .height = h,
    .colorFormat = self->format,
    .layout = RDS_BUF_LAYOUT_PITCH,
    .memType = RDS_BUF_MEM_DEFAULT,
  };
  RdsBufSurface *s = NULL;

  if (rds_buf_surface_create (&s, 1, &cp) != 0)
    return NULL;
  s->numFilled = 1;
  if (!fill_black (s)) {
    rds_buf_surface_destroy (s);
    return NULL;
  }
  return s;
}

/* A hand-built surface whose @n entries all show surface 0 of @s (for composites with one source drawn many times).
 * @params must hold @n entries. */
static void
repeat_surface (const RdsBufSurface * s, guint n, RdsBufSurfaceParams * params, RdsBufSurface * out)
{
  for (guint i = 0; i < n; i++)
    params[i] = s->surfaceList[0];
  *out = (RdsBufSurface) {
  .gpuId = s->gpuId,.batchSize = n,.numFilled = n,.isContiguous = FALSE,.memType = s->memType,.surfaceList =
        params};
}

static RdsBufSurfTransformRect
to_transform_rect (const RdsTilerRect * r)
{
  return (RdsBufSurfTransformRect) {
  .top = r->top,.left = r->left,.width = r->width,.height = r->height};
}

/* Clears the cells of @pads (n of them) in the canvas. */
static gboolean
clear_cells (GstRdsMultiStreamTiler * self, const guint * pads, guint n)
{
  RdsBufSurfaceParams *params;
  RdsBufSurfTransformRect *src_rects, *dst_rects;
  RdsBufSurface src;
  guint k = 0;
  gboolean ok = TRUE;

  if (n == 0)
    return TRUE;
  params = g_new (RdsBufSurfaceParams, n);
  src_rects = g_new (RdsBufSurfTransformRect, n);
  dst_rects = g_new (RdsBufSurfTransformRect, n);
  for (guint i = 0; i < n; i++) {
    RdsTilerRect cell;

    if (!rds_tiler_layout_cell (&self->layout, pads[i], &cell))
      continue;
    src_rects[k] = (RdsBufSurfTransformRect) {
    0, 0, BLACK_SIZE, BLACK_SIZE};
    dst_rects[k++] = to_transform_rect (&cell);
  }
  if (k > 0) {
    repeat_surface (self->black, k, params, &src);
    ok = composite (self, &src, self->canvas, src_rects, dst_rects, RdsBufSurfTransformInter_Nearest);
  }
  g_free (dst_rects);
  g_free (src_rects);
  g_free (params);
  return ok;
}

/* Makes the canvas match the layout for n_pads on the negotiated output: a new black canvas when the layout (or the
 * output size or format) changed, with every cached pad's last pixels moved from its old cell to its new one, so
 * an absent source survives a grid change. */
static gboolean
ensure_canvas (GstRdsMultiStreamTiler * self)
{
  RdsTilerLayout l;
  RdsBufSurface *canvas, *old = self->canvas;
  RdsBufSurfaceParams *params;
  RdsBufSurfTransformRect *src_rects, *dst_rects;
  RdsBufSurface src;
  guint k = 0;
  gboolean ok = TRUE;

  /* n_pads only grows to sizes that fit (grow_for_pad), and n 0 or 1 always fits a 16 x 16 output */
  if (!layout_for (self, self->n_pads, &l)) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot lay out %u sources on %dx%d", self->n_pads,
            GST_VIDEO_INFO_WIDTH (&self->out_info), GST_VIDEO_INFO_HEIGHT (&self->out_info)), (NULL));
    return FALSE;
  }
  if (old != NULL && memcmp (&l, &self->layout, sizeof l) == 0 && old->surfaceList[0].colorFormat == self->format)
    return TRUE;

  canvas = new_black_surface (self, l.width, l.height);
  if (canvas == NULL) {
    GST_ELEMENT_ERROR (self, RESOURCE, NO_SPACE_LEFT, ("Cannot allocate a %ux%u tiling surface on GPU %u",
            l.width, l.height, self->stream_gpu), (NULL));
    return FALSE;
  }
  GST_DEBUG_OBJECT (self, "layout %ux%u, cells %ux%u, output %ux%u", l.rows, l.columns, l.cell_width,
      l.cell_height, l.width, l.height);

  if (old != NULL) {
    params = g_new (RdsBufSurfaceParams, self->n_pads);
    src_rects = g_new (RdsBufSurfTransformRect, self->n_pads);
    dst_rects = g_new (RdsBufSurfTransformRect, self->n_pads);
    for (guint pad = 0; pad < self->n_pads; pad++) {
      PadState *ps = pad_state (self, pad);
      RdsTilerRect from, to;

      if (ps == NULL || !ps->cached || !rds_tiler_layout_cell (&self->layout, pad, &from)
          || !rds_tiler_layout_cell (&l, pad, &to))
        continue;
      src_rects[k] = to_transform_rect (&from);
      dst_rects[k++] = to_transform_rect (&to);
    }
    if (k > 0) {
      repeat_surface (old, k, params, &src);
      ok = composite (self, &src, canvas, src_rects, dst_rects, RdsBufSurfTransformInter_Nearest);
    }
    g_free (dst_rects);
    g_free (src_rects);
    g_free (params);
    rds_buf_surface_destroy (old);
  }
  self->canvas = canvas;
  self->layout = l;
  return ok;
}

static void
drop_canvas (GstRdsMultiStreamTiler * self)
{
  if (self->canvas != NULL)
    rds_buf_surface_destroy (self->canvas);
  self->canvas = NULL;
}

/* Forgets every pad's pixels and meta (flush, stop). */
static void
reset_pads (GstRdsMultiStreamTiler * self)
{
  for (guint i = 0; i < self->pads->len; i++) {
    PadState *ps = g_ptr_array_index (self->pads, i);

    pad_state_forget (ps);
    ps->clear = FALSE;
  }
  drop_canvas (self);
}

/* ---- caps ---- */

static GstCaps *
gst_rds_multi_stream_tiler_transform_caps (GstBaseTransform * trans, GstPadDirection direction, GstCaps * caps,
    GstCaps * filter)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (trans);
  GstCaps *ret = gst_caps_new_empty (), *tmp, *templ;
  guint width, height;

  GST_OBJECT_LOCK (self);
  width = MIN (rds_tiler_round_output_size (self->width), (guint) G_MAXINT);
  height = MIN (rds_tiler_round_output_size (self->height), (guint) G_MAXINT);
  GST_OBJECT_UNLOCK (self);

  /* Same format and framerate on both sides (DeepStream 8.0 behaviour: no conversion); the output size is
   * width x height rounded up to a multiple of 4, the input any size. */
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *s = gst_structure_copy (gst_caps_get_structure (caps, i));

    gst_structure_remove_fields (s, "pixel-aspect-ratio", "batch-size", "num-surfaces-per-frame", NULL);
    if (direction == GST_PAD_SINK)
      gst_structure_set (s, "width", G_TYPE_INT, (gint) width, "height", G_TYPE_INT, (gint) height, NULL);
    else
      gst_structure_set (s, "width", GST_TYPE_INT_RANGE, 1, RDS_BUF_SURFACE_MAX_DIM, "height", GST_TYPE_INT_RANGE,
          1, RDS_BUF_SURFACE_MAX_DIM, NULL);
    ret = gst_caps_merge_structure_full (ret, s,
        gst_caps_features_new_single_static_str (RDS_CAPS_FEATURE_MEMORY_RDS));
  }
  templ = gst_pad_get_pad_template_caps (direction == GST_PAD_SINK ? GST_BASE_TRANSFORM_SRC_PAD (trans) :
      GST_BASE_TRANSFORM_SINK_PAD (trans));
  tmp = gst_caps_intersect (ret, templ);
  gst_caps_unref (templ);
  gst_caps_unref (ret);
  ret = tmp;
  if (filter != NULL) {
    tmp = gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = tmp;
  }
  GST_DEBUG_OBJECT (trans, "%s %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT,
      direction == GST_PAD_SINK ? "sink" : "src", caps, ret);
  return ret;
}

static gboolean
gst_rds_multi_stream_tiler_set_caps (GstBaseTransform * trans, GstCaps * incaps, GstCaps * outcaps)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (trans);
  GstVideoInfo out_info;
  RdsBufSurfaceColorFormat format;

  if (!gst_video_info_from_caps (&out_info, outcaps)) {
    GST_ERROR_OBJECT (self, "invalid caps %" GST_PTR_FORMAT, outcaps);
    return FALSE;
  }
  format = rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&out_info));
  if (format != RDS_BUF_COLOR_FORMAT_NV12 && format != RDS_BUF_COLOR_FORMAT_RGBA) {
    GST_ERROR_OBJECT (self, "unsupported format in %" GST_PTR_FORMAT, outcaps);
    return FALSE;
  }
  /* a new size or format replaces the canvas on the next batch (cached pixels move into it) */
  self->out_info = out_info;
  self->format = format;
  if (self->black != NULL && self->black->surfaceList[0].colorFormat != format) {
    rds_buf_surface_destroy (self->black);
    self->black = NULL;
  }
  for (guint i = 0; i < self->pending_pads->len; i++)
    (void) grow_for_pad (self, g_array_index (self->pending_pads, guint, i));
  g_array_set_size (self->pending_pads, 0);
  GST_DEBUG_OBJECT (self, "%" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT, incaps, outcaps);
  return TRUE;
}

static gboolean
gst_rds_multi_stream_tiler_get_unit_size (G_GNUC_UNUSED GstBaseTransform * trans, G_GNUC_UNUSED GstCaps * caps,
    gsize * size)
{
  *size = sizeof (RdsBufSurface);
  return TRUE;
}

/* ---- allocation ---- */

static gboolean
gst_rds_multi_stream_tiler_decide_allocation (GstBaseTransform * trans, GstQuery * query)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (trans);
  GstBufferPool *pool;
  RdsBufSurfaceMemType mem_type;
  guint gpu_id, pool_size;

  GST_OBJECT_LOCK (self);
  mem_type = (RdsBufSurfaceMemType) self->memory_type;
  gpu_id = self->gpu_id;
  pool_size = self->buffer_pool_size;
  GST_OBJECT_UNLOCK (self);

  /* our own pool of buffer-pool-size single surfaces; writes it into the query, where the base class picks it up */
  pool = rds_decide_allocation (query, 1, mem_type, gpu_id, pool_size, pool_size);
  if (pool == NULL) {
    GST_ERROR_OBJECT (self, "cannot set up a memory:RDS output pool");
    return FALSE;
  }
  gst_object_unref (pool);
  return TRUE;
}

/* ---- metadata ---- */

static gboolean
gst_rds_multi_stream_tiler_transform_meta (G_GNUC_UNUSED GstBaseTransform * trans,
    G_GNUC_UNUSED GstBuffer * outbuf, GstMeta * meta, G_GNUC_UNUSED GstBuffer * inbuf)
{
  GType api = meta->info->api;
  const gchar *const *tags = gst_meta_api_type_get_tags (api);

  /* the batch meta is replaced by the tiled one; the output describes its own layout */
  if (api == RDS_META_API_TYPE || api == GST_VIDEO_META_API_TYPE || api == GST_VIDEO_CROP_META_API_TYPE)
    return FALSE;
  if (tags == NULL || tags[0] == NULL)
    return TRUE;
  return tags[1] == NULL && g_strcmp0 (tags[0], GST_META_TAG_VIDEO_STR) == 0;
}

/* Appends copies of @src's objects and display metas to @dst, scaled from @frame_w x @frame_h into @rect. */
static void
add_scaled_meta (RdsFrameMeta * src, RdsFrameMeta * dst, guint frame_w, guint frame_h, const RdsTilerRect * rect)
{
  GList *last_obj = g_list_last (dst->obj_meta_list), *last_disp = g_list_last (dst->display_meta_list);
  RdsTilerScale scale;

  rds_tiler_scale_init (&scale, frame_w, frame_h, rect);
  rds_copy_obj_meta_list (src->obj_meta_list, dst);
  rds_copy_display_meta_list (src->display_meta_list, dst);
  for (GList * l = last_obj != NULL ? last_obj->next : dst->obj_meta_list; l != NULL; l = l->next)
    rds_tiler_scale_object (&scale, l->data);
  for (GList * l = last_disp != NULL ? last_disp->next : dst->display_meta_list; l != NULL; l = l->next)
    rds_tiler_scale_display (&scale, l->data);
}

/* ---- events ---- */

/* An ended or removed pad: its cell goes black and its meta stops repeating (DeepStream 8.0 behaviour). */
static void
end_pad (GstRdsMultiStreamTiler * self, guint pad)
{
  PadState *ps = pad_state (self, pad);

  if (ps == NULL)
    return;
  pad_state_forget (ps);
  ps->clear = TRUE;
}

static gboolean
gst_rds_multi_stream_tiler_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (trans);
  guint id;

  /* serialized: on the streaming thread, between batches. Every event goes downstream. */
  switch ((RdsEventType) GST_EVENT_TYPE (event)) {
    case RDS_EVENT_PAD_ADDED:
      if (rds_event_parse_pad_added (event, &id))
        (void) grow_for_pad (self, id);
      break;
    case RDS_EVENT_STREAM_EOS:
      if (rds_event_parse_stream_eos (event, &id))
        end_pad (self, id);
      break;
    case RDS_EVENT_PAD_DELETED:
      if (rds_event_parse_pad_deleted (event, &id))
        end_pad (self, id);
      break;
    case RDS_EVENT_STREAM_SEGMENT:
    case RDS_EVENT_STREAM_RESET:
    case RDS_EVENT_STREAM_START:
      break;
    default:
      if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP)
        reset_pads (self);
      break;
  }
  return GST_BASE_TRANSFORM_CLASS (gst_rds_multi_stream_tiler_parent_class)->sink_event (trans, event);
}

/* ---- streaming ---- */

static gboolean
gst_rds_multi_stream_tiler_start (GstBaseTransform * trans)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (trans);
  int prev = 0;
  guint gpu_id;
  gint compute_hw;
  hipError_t err;

  GST_OBJECT_LOCK (self);
  gpu_id = self->gpu_id;
  compute_hw = self->compute_hw;
  GST_OBJECT_UNLOCK (self);

  if (compute_hw == COMPUTE_HW_VIC) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("compute-hw=VIC is not available"),
        ("only the GPU (compute-hw 0 or 1) is supported"));
    return FALSE;
  }
  if (hipGetDevice (&prev) != hipSuccess || hipSetDevice ((int) gpu_id) != hipSuccess) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("GPU %u is not available", gpu_id), (NULL));
    return FALSE;
  }
  err = hipStreamCreateWithFlags (&self->stream, hipStreamNonBlocking);
  (void) hipSetDevice (prev);
  if (err != hipSuccess) {
    self->stream = NULL;
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot create a HIP stream on GPU %u", gpu_id),
        ("hipStreamCreateWithFlags: %s", hipGetErrorString (err)));
    return FALSE;
  }
  self->stream_gpu = gpu_id;
  self->n_pads = 0;
  self->out_of_grid_warned = FALSE;
  return TRUE;
}

static gboolean
gst_rds_multi_stream_tiler_stop (GstBaseTransform * trans)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (trans);

  drop_canvas (self);
  if (self->black != NULL)
    rds_buf_surface_destroy (self->black);
  self->black = NULL;
  g_ptr_array_set_size (self->pads, 0);
  g_array_set_size (self->pending_pads, 0);
  gst_video_info_init (&self->out_info);
  self->n_pads = 0;
  if (self->stream != NULL) {
    if (hipStreamDestroy (self->stream) != hipSuccess)
      GST_WARNING_OBJECT (self, "hipStreamDestroy failed");
    self->stream = NULL;
  }
  return TRUE;
}

/* One frame of the batch that has a cell: its pad, its slot and its frame meta. */
typedef struct
{
  guint pad;
  guint slot;
  RdsFrameMeta *meta;
} Entry;

static GstFlowReturn
gst_rds_multi_stream_tiler_transform (GstBaseTransform * trans, GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (trans);
  RdsBatchMeta *in_bm = gst_buffer_get_rds_batch_meta (inbuf), *out_bm;
  RdsFrameMeta *out_fm;
  GstMapInfo in_map, out_map;
  RdsBufSurface *in = NULL, *out, src;
  RdsBufSurfaceParams *params = NULL;
  RdsBufSurfTransformRect *src_rects = NULL, *dst_rects = NULL;
  GArray *entries = g_array_new (FALSE, FALSE, sizeof (Entry));
  GArray *cleared = g_array_new (FALSE, FALSE, sizeof (guint));
  gboolean in_mapped = FALSE, out_mapped = FALSE, empty;
  gint show_source;
  RdsBufSurfTransform_Inter filter;
  GstFlowReturn ret = GST_FLOW_ERROR;
  guint n_slots = 0, k;

  GST_OBJECT_LOCK (self);
  show_source = self->show_source;
  filter = (RdsBufSurfTransform_Inter) self->interpolation_method;
  GST_OBJECT_UNLOCK (self);

  if (rds_buffer_get_surface (inbuf) == NULL || !gst_buffer_map (inbuf, &in_map, GST_MAP_READ))
    goto map_failed;
  in_mapped = TRUE;
  in = (RdsBufSurface *) in_map.data;
  n_slots = MIN (in->numFilled, in->batchSize);
  if (n_slots > 0 && in->memType == RDS_BUF_MEM_SYSTEM) {
    GST_ELEMENT_ERROR (self, STREAM, NOT_IMPLEMENTED, ("System-memory batches are not supported"),
        ("the input batch must be in device-accessible memory:RDS memory (device, pinned or unified)"));
    goto done;
  }
  if (rds_buffer_get_surface (outbuf) == NULL || !gst_buffer_map (outbuf, &out_map, GST_MAP_WRITE))
    goto map_failed;
  out_mapped = TRUE;
  out = (RdsBufSurface *) out_map.data;

  if (self->black == NULL && (self->black = new_black_surface (self, BLACK_SIZE, BLACK_SIZE)) == NULL) {
    GST_ELEMENT_ERROR (self, RESOURCE, NO_SPACE_LEFT, ("Cannot allocate a surface on GPU %u", self->stream_gpu),
        (NULL));
    goto done;
  }

  /* The frames that have a cell; the grid grows for new pads first. */
  if (in_bm != NULL)
    rds_acquire_meta_lock (in_bm);
  empty = in_bm == NULL || in_bm->frame_meta_list == NULL;
  for (GList * l = in_bm != NULL ? in_bm->frame_meta_list : NULL; l != NULL; l = l->next) {
    RdsFrameMeta *fm = l->data;
    Entry e = {.pad = fm->pad_index,.slot = fm->batch_id,.meta = fm };

    if (e.slot >= n_slots) {
      GST_WARNING_OBJECT (self, "frame of pad %u has batch_id %u, outside the %u frames of the batch: skipped",
          e.pad, e.slot, n_slots);
      continue;
    }
    if (grow_for_pad (self, e.pad))
      g_array_append_val (entries, e);
  }
  if (!ensure_canvas (self))
    goto unlock;

  /* Ended pads' cells go black (unless the pad is back in this batch: it is drawn over anyway). */
  for (guint pad = 0; pad < self->n_pads; pad++) {
    PadState *ps = pad_state (self, pad);

    if (ps->clear) {
      g_array_append_val (cleared, pad);
      ps->clear = FALSE;
    }
  }
  if (!clear_cells (self, (const guint *) cleared->data, cleared->len))
    goto unlock;

  /* This batch's frames into their cells of the canvas, in batch order (a pad twice: the later one on top). */
  if (entries->len > 0) {
    params = g_new (RdsBufSurfaceParams, entries->len);
    src_rects = g_new (RdsBufSurfTransformRect, entries->len);
    dst_rects = g_new (RdsBufSurfTransformRect, entries->len);
    for (k = 0; k < entries->len; k++) {
      Entry *e = &g_array_index (entries, Entry, k);
      RdsTilerRect cell;

      (void) rds_tiler_layout_cell (&self->layout, e->pad, &cell);
      params[k] = in->surfaceList[e->slot];
      src_rects[k] = (RdsBufSurfTransformRect) {
      0, 0, params[k].width, params[k].height};
      dst_rects[k] = to_transform_rect (&cell);
    }
    src = (RdsBufSurface) {
    .gpuId = in->gpuId,.batchSize = entries->len,.numFilled = entries->len,.memType = in->memType,.surfaceList =
          params};
    if (!composite (self, &src, self->canvas, src_rects, dst_rects, filter))
      goto unlock;
  }

  /* Each pad's last meta, for when it is absent. */
  for (k = 0; k < entries->len; k++) {
    Entry *e = &g_array_index (entries, Entry, k);
    PadState *ps = pad_state (self, e->pad);

    if (ps->meta == NULL)
      ps->meta = rds_create_batch_meta (1);
    pad_state_forget (ps);
    if (rds_copy_frame_meta_to_batch (e->meta, ps->meta) != NULL) {
      ps->cached = TRUE;
      ps->frame_width = in->surfaceList[e->slot].width;
      ps->frame_height = in->surfaceList[e->slot].height;
    }
  }
  if (in_bm != NULL) {
    rds_release_meta_lock (in_bm);
    in_bm = NULL;
  }

  /* The output: one fresh frame meta (DeepStream 8.0 behaviour: all of its ids, sizes and timestamps 0). */
  out_bm = rds_create_batch_meta (1);
  out_fm = rds_acquire_frame_meta_from_pool (out_bm);
  rds_add_frame_meta_to_batch (out_bm, out_fm);
  rds_buffer_add_batch_meta (outbuf, out_bm);

  if (show_source < 0 && empty) {
    /* An empty batch clears the output: DeepStream's docs say the muxer sends one to clear the tiler's output (8.0
     * crashes on it). Black with the empty frame meta; the caches stay for the next batch (ours). */
    if (!fill_black (out)) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot clear the output frame"), (NULL));
      goto done;
    }
  } else if (show_source < 0) {
    /* the grid: the canvas, with every cached pad's meta in its cell (pad order: ours) */
    if (rds_buf_surface_copy (self->canvas, out) != 0) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot copy the tiled frame"), (NULL));
      goto done;
    }
    for (guint pad = 0; pad < self->n_pads; pad++) {
      PadState *ps = pad_state (self, pad);
      RdsFrameMeta *fm = pad_state_frame (ps);
      RdsTilerRect cell;

      if (fm != NULL && rds_tiler_layout_cell (&self->layout, pad, &cell))
        add_scaled_meta (fm, out_fm, ps->frame_width, ps->frame_height, &cell);
    }
  } else if ((guint64) show_source >= (guint64) self->layout.rows * self->layout.columns) {
    /* DeepStream 8.0 behaviour: an element error, checked on every batch */
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("show-source %d is not a cell of the %ux%u grid", show_source,
            self->layout.rows, self->layout.columns), ("show-source must be below rows x columns (%u)",
            self->layout.rows * self->layout.columns));
    goto done;
  } else {
    /* one pad stretched to the whole output with only its meta (DeepStream 8.0 behaviour). Black and no meta when
     * it has no frame in this batch: DeepStream 8.0 behaviour for a pad that never had one or has ended; ours for a
     * live pad missing from this batch (the grid repeats such a pad instead). */
    RdsTilerRect all = { 0, 0, GST_VIDEO_INFO_WIDTH (&self->out_info), GST_VIDEO_INFO_HEIGHT (&self->out_info) };
    Entry *shown = NULL;

    for (k = 0; k < entries->len; k++)
      if (g_array_index (entries, Entry, k).pad == (guint) show_source)
        shown = &g_array_index (entries, Entry, k);
    if (shown == NULL) {
      if (!fill_black (out)) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot clear the output frame"), (NULL));
        goto done;
      }
    } else {
      PadState *ps = pad_state (self, shown->pad);
      RdsBufSurfaceParams one = in->surfaceList[shown->slot];
      RdsBufSurfTransformRect src_rect = { 0, 0, one.width, one.height }, dst_rect = to_transform_rect (&all);

      src = (RdsBufSurface) {
      .gpuId = in->gpuId,.batchSize = 1,.numFilled = 1,.memType = in->memType,.surfaceList = &one};
      if (!composite (self, &src, out, &src_rect, &dst_rect, filter))
        goto done;
      if (pad_state_frame (ps) != NULL)
        add_scaled_meta (pad_state_frame (ps), out_fm, ps->frame_width, ps->frame_height, &all);
    }
  }
  out->numFilled = 1;
  /* composites, copies and memsets are synchronous: the output is ready when it is pushed */
  ret = GST_FLOW_OK;
  goto done;

unlock:
  if (in_bm != NULL)
    rds_release_meta_lock (in_bm);
done:
  g_free (dst_rects);
  g_free (src_rects);
  g_free (params);
  g_array_unref (cleared);
  g_array_unref (entries);
  if (out_mapped)
    gst_buffer_unmap (outbuf, &out_map);
  if (in_mapped)
    gst_buffer_unmap (inbuf, &in_map);
  return ret;

map_failed:
  GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map a buffer"), ("input and output must be memory:RDS"));
  goto done;
}

/* ---- properties ---- */

/* Allocation settings are read when the pool is set up: refuse changes once streaming (object lock held). */
static gboolean
check_mutable (GstRdsMultiStreamTiler * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_multi_stream_tiler_set_property (GObject * object, guint prop_id, const GValue * value,
    GParamSpec * pspec)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (object);
  gboolean reconfigure = FALSE, warn_custom = FALSE;

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_ROWS:
      self->rows = g_value_get_uint (value);
      break;
    case PROP_COLUMNS:
      self->columns = g_value_get_uint (value);
      break;
    case PROP_WIDTH:
      self->width = g_value_get_uint (value);
      reconfigure = TRUE;
      break;
    case PROP_HEIGHT:
      self->height = g_value_get_uint (value);
      reconfigure = TRUE;
      break;
    case PROP_SHOW_SOURCE:
      self->show_source = g_value_get_int (value);
      break;
    case PROP_GPU_ID:
      if (check_mutable (self, pspec))
        self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      if (check_mutable (self, pspec))
        self->memory_type = g_value_get_enum (value);
      break;
    case PROP_COMPUTE_HW:
      self->compute_hw = g_value_get_enum (value);
      break;
    case PROP_INTERPOLATION_METHOD:
      self->interpolation_method = g_value_get_enum (value);
      break;
    case PROP_SQUARE_SEQ_GRID:
      self->square_seq_grid = g_value_get_boolean (value);
      break;
    case PROP_BUFFER_POOL_SIZE:
      if (check_mutable (self, pspec))
        self->buffer_pool_size = g_value_get_uint (value);
      break;
    case PROP_CUSTOM_TILE_CONFIG:
      self->custom_tile_config = g_value_get_pointer (value);
      warn_custom = self->custom_tile_config != NULL && !self->custom_tile_config_warned;
      if (warn_custom)
        self->custom_tile_config_warned = TRUE;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);

  if (warn_custom)
    g_warning ("%s: custom-tile-config is reserved (as in DeepStream) and has no effect", GST_OBJECT_NAME (self));
  /* the output size is in the caps: renegotiate */
  if (reconfigure)
    gst_base_transform_reconfigure_src (GST_BASE_TRANSFORM (self));
}

static void
gst_rds_multi_stream_tiler_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_ROWS:
      g_value_set_uint (value, self->rows);
      break;
    case PROP_COLUMNS:
      g_value_set_uint (value, self->columns);
      break;
    case PROP_WIDTH:
      g_value_set_uint (value, self->width);
      break;
    case PROP_HEIGHT:
      g_value_set_uint (value, self->height);
      break;
    case PROP_SHOW_SOURCE:
      g_value_set_int (value, self->show_source);
      break;
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      g_value_set_enum (value, self->memory_type);
      break;
    case PROP_COMPUTE_HW:
      g_value_set_enum (value, self->compute_hw);
      break;
    case PROP_INTERPOLATION_METHOD:
      g_value_set_enum (value, self->interpolation_method);
      break;
    case PROP_SQUARE_SEQ_GRID:
      g_value_set_boolean (value, self->square_seq_grid);
      break;
    case PROP_BUFFER_POOL_SIZE:
      g_value_set_uint (value, self->buffer_pool_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_multi_stream_tiler_finalize (GObject * object)
{
  GstRdsMultiStreamTiler *self = GST_RDS_MULTI_STREAM_TILER (object);

  g_ptr_array_unref (self->pads);
  g_array_unref (self->pending_pads);
  G_OBJECT_CLASS (gst_rds_multi_stream_tiler_parent_class)->finalize (object);
}

static void
gst_rds_multi_stream_tiler_init (GstRdsMultiStreamTiler * self)
{
  self->rows = DEFAULT_ROWS;
  self->columns = DEFAULT_COLUMNS;
  self->width = DEFAULT_WIDTH;
  self->height = DEFAULT_HEIGHT;
  self->show_source = DEFAULT_SHOW_SOURCE;
  self->gpu_id = DEFAULT_GPU_ID;
  self->memory_type = DEFAULT_MEMORY_TYPE;
  self->compute_hw = DEFAULT_COMPUTE_HW;
  self->interpolation_method = DEFAULT_INTERPOLATION_METHOD;
  self->square_seq_grid = DEFAULT_SQUARE_SEQ_GRID;
  self->buffer_pool_size = DEFAULT_BUFFER_POOL_SIZE;
  self->pads = g_ptr_array_new_with_free_func (pad_state_free);
  self->pending_pads = g_array_new (FALSE, FALSE, sizeof (guint));
  gst_video_info_init (&self->out_info);
}

static void
gst_rds_multi_stream_tiler_class_init (GstRdsMultiStreamTilerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  GST_DEBUG_CATEGORY_INIT (gst_rds_multi_stream_tiler_debug, "rdsmultistreamtiler", 0,
      "Redstream multi-stream tiler");

  gobject_class->set_property = gst_rds_multi_stream_tiler_set_property;
  gobject_class->get_property = gst_rds_multi_stream_tiler_get_property;
  gobject_class->finalize = gst_rds_multi_stream_tiler_finalize;

  g_object_class_install_property (gobject_class, PROP_ROWS,
      g_param_spec_uint ("rows", "Rows", "Rows of the tiled output (more when the sources do not fit)", 1,
          G_MAXUINT, DEFAULT_ROWS, flags));
  g_object_class_install_property (gobject_class, PROP_COLUMNS,
      g_param_spec_uint ("columns", "Columns", "Columns of the tiled output (more when the sources do not fit)", 1,
          G_MAXUINT, DEFAULT_COLUMNS, flags));
  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_uint ("width", "Width", "Width of the tiled output in pixels (rounded up to a multiple of 4)",
          MIN_OUTPUT_SIZE, G_MAXUINT, DEFAULT_WIDTH, flags));
  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_uint ("height", "Height", "Height of the tiled output in pixels (rounded up to a multiple of 4)",
          MIN_OUTPUT_SIZE, G_MAXUINT, DEFAULT_HEIGHT, flags));
  g_object_class_install_property (gobject_class, PROP_SHOW_SOURCE,
      g_param_spec_int ("show-source", "Show source",
          "Pad index of the one source to scale to the whole output; -1 tiles all the sources", -1, G_MAXINT,
          DEFAULT_SHOW_SOURCE, flags));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU id", "GPU used for tiling", 0, G_MAXUINT, DEFAULT_GPU_ID,
          flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_NVBUF_MEMORY_TYPE,
      g_param_spec_enum ("nvbuf-memory-type", "Memory type", "Memory type of the output buffers",
          GST_TYPE_RDS_TILER_MEMORY_TYPE, DEFAULT_MEMORY_TYPE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_COMPUTE_HW,
      g_param_spec_enum ("compute-hw", "Compute hardware", "Engine that scales the frames",
          GST_TYPE_RDS_TILER_COMPUTE_HW, DEFAULT_COMPUTE_HW, flags));
  g_object_class_install_property (gobject_class, PROP_INTERPOLATION_METHOD,
      g_param_spec_enum ("interpolation-method", "Interpolation method", "Filter used for scaling",
          GST_TYPE_RDS_TILER_INTERPOLATION_METHOD, DEFAULT_INTERPOLATION_METHOD, flags));
  g_object_class_install_property (gobject_class, PROP_SQUARE_SEQ_GRID,
      g_param_spec_boolean ("square-seq-grid", "Square grid",
          "A square grid sized for the number of sources; rows and columns are ignored", DEFAULT_SQUARE_SEQ_GRID,
          flags));
  g_object_class_install_property (gobject_class, PROP_BUFFER_POOL_SIZE,
      g_param_spec_uint ("buffer-pool-size", "Buffer pool size", "Buffers in the output pool",
          MIN_BUFFER_POOL_SIZE, MAX_BUFFER_POOL_SIZE, DEFAULT_BUFFER_POOL_SIZE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_CUSTOM_TILE_CONFIG,
      g_param_spec_pointer ("custom-tile-config", "Custom tile config",
          "Reserved: accepted for compatibility, no effect", G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream multi-stream tiler",
      "Filter/Video/Compositor", "Tiles the frames of a memory:RDS batch into one 2D grid frame",
      "Redstream contributors");

  trans_class->transform_caps = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_transform_caps);
  trans_class->set_caps = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_set_caps);
  trans_class->get_unit_size = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_get_unit_size);
  trans_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_decide_allocation);
  trans_class->transform_meta = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_transform_meta);
  trans_class->sink_event = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_sink_event);
  trans_class->start = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_stop);
  trans_class->transform = GST_DEBUG_FUNCPTR (gst_rds_multi_stream_tiler_transform);
  trans_class->passthrough_on_same_caps = FALSE;

  gst_type_mark_as_plugin_api (GST_TYPE_RDS_TILER_MEMORY_TYPE, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_TILER_INTERPOLATION_METHOD, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_TILER_COMPUTE_HW, 0);
}
