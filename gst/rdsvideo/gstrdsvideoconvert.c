/* Redstream: rdsvideoconvert, colour conversion and scaling of memory:RDS video with rds_buf_surf_transform.
 * Property names and enum integers follow DeepStream's public nvvideoconvert documentation.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsvideoconvert.h"
#include "rds-video-util.h"

#include <gst/video/video.h>
#include <rds/rds.h>
/* after GLib/GStreamer: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_video_convert_debug);
#define GST_CAT_DEFAULT gst_rds_video_convert_debug

#define DEFAULT_GPU_ID 0
#define DEFAULT_MEMORY_TYPE 0
#define DEFAULT_OUTPUT_BUFFERS 4
#define DEFAULT_DISABLE_PASSTHROUGH FALSE
#define DEFAULT_SILENT FALSE

enum
{
  PROP_0,
  PROP_GPU_ID,
  PROP_NVBUF_MEMORY_TYPE,
  PROP_OUTPUT_BUFFERS,
  PROP_DISABLE_PASSTHROUGH,
  PROP_SILENT,
};

struct _GstRdsVideoConvert
{
  GstBaseTransform parent;

  /* properties (object lock) */
  guint gpu_id;
  gint memory_type;             /* GstRdsVidConvBufMemoryType == RdsBufSurfaceMemType 0..3 */
  guint output_buffers;
  gboolean disable_passthrough;
  gboolean silent;

  /* streaming state */
  hipStream_t stream;
  guint stream_gpu;             /* the GPU the stream (and every transform) runs on */
  GstVideoInfo in_info;
  GstVideoInfo out_info;
  gboolean caps_equal;          /* negotiated input and output caps are equal (object lock) */
};

#define RDS_VIDEO_CONVERT_FORMATS "{ NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR, GRAY8 }"
#define RDS_VIDEO_CONVERT_FIELDS \
  "format = (string) " RDS_VIDEO_CONVERT_FORMATS ", width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], " \
  "framerate = (fraction) [ 0/1, 2147483647/1 ]"
#define RDS_VIDEO_CONVERT_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), " RDS_VIDEO_CONVERT_FIELDS "; video/x-raw, " RDS_VIDEO_CONVERT_FIELDS

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_VIDEO_CONVERT_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_VIDEO_CONVERT_CAPS));

#define GST_TYPE_RDS_VID_CONV_BUF_MEMORY_TYPE (gst_rds_vid_conv_buf_memory_type_get_type ())
static GType
gst_rds_vid_conv_buf_memory_type_get_type (void)
{
  static GType type = 0;
  /* Integers and nicks as in DeepStream (dGPU values); the integers are what configs use. */
  static const GEnumValue values[] = {
    {0, "Default memory for the platform (HIP device memory)", "nvbuf-mem-default"},
    {1, "Page-locked host memory", "nvbuf-mem-cuda-pinned"},
    {2, "HIP device memory", "nvbuf-mem-cuda-device"},
    {3, "HIP managed (unified) memory", "nvbuf-mem-cuda-unified"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsVidConvBufMemoryType", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

G_DEFINE_TYPE (GstRdsVideoConvert, gst_rds_video_convert, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdsvideoconvert, "rdsvideoconvert", GST_RANK_NONE, GST_TYPE_RDS_VIDEO_CONVERT);

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

/* ---- caps ---- */

static GstCaps *
gst_rds_video_convert_transform_caps (GstBaseTransform * trans, GstPadDirection direction, GstCaps * caps,
    GstCaps * filter)
{
  GstCaps *own = gst_caps_new_empty (), *other = gst_caps_new_empty (), *ret;

  /* Any format and size, in either memory: drop what we convert, keep the rest (framerate). Each structure's own
   * memory comes first, as in videoconvertscale, so passthrough and downstream's order win. */
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *s = gst_structure_copy (gst_caps_get_structure (caps, i));
    GstCapsFeatures *f = gst_caps_get_features (caps, i);
    gboolean is_rds = f != NULL && !gst_caps_features_is_any (f)
        && gst_caps_features_contains (f, RDS_CAPS_FEATURE_MEMORY_RDS);
    GstCaps **rds = is_rds ? &own : &other, **sys = is_rds ? &other : &own;

    gst_structure_remove_fields (s, "format", "width", "height", "pixel-aspect-ratio", "colorimetry",
        "chroma-site", NULL);
    *rds = gst_caps_merge_structure_full (*rds, gst_structure_copy (s),
        gst_caps_features_new_single_static_str (RDS_CAPS_FEATURE_MEMORY_RDS));
    *sys = gst_caps_merge_structure_full (*sys, s,
        gst_caps_features_new_single_static_str (GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY));
  }
  ret = gst_caps_merge (own, other);

  if (filter != NULL) {
    GstCaps *tmp = gst_caps_intersect_full (filter, ret, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (ret);
    ret = tmp;
  }
  GST_DEBUG_OBJECT (trans, "%s %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT,
      direction == GST_PAD_SINK ? "sink" : "src", caps, ret);
  return ret;
}

/* Sets @field from @in when @out lacks it, or fixates it to @in's string value when @out allows several. */
static void
take_field (GstStructure * out, const GstStructure * in, const gchar * field)
{
  const GValue *v = gst_structure_get_value (in, field);

  if (v == NULL)
    return;
  if (!gst_structure_has_field (out, field))
    gst_structure_set_value (out, field, v);
  else if (G_VALUE_HOLDS_STRING (v))
    gst_structure_fixate_field_string (out, field, g_value_get_string (v));
}

/* Same format, or both YUV 4:2:0: colorimetry and chroma siting carry over. */
static gboolean
same_colour_family (const gchar * a, const gchar * b)
{
  if (a == NULL || b == NULL)
    return FALSE;
  if (g_str_equal (a, b))
    return TRUE;
  return (g_str_equal (a, "NV12") || g_str_equal (a, "I420")) && (g_str_equal (b, "NV12")
      || g_str_equal (b, "I420"));
}

static GstCaps *
gst_rds_video_convert_fixate_caps (GstBaseTransform * trans, G_GNUC_UNUSED GstPadDirection direction,
    GstCaps * caps, GstCaps * othercaps)
{
  const GstStructure *in = gst_caps_get_structure (caps, 0);
  GstStructure *out;
  GstCaps *same;
  const gchar *format;
  gint v, n, d;

  /* The input caps themselves when the other side allows them: that gives passthrough. */
  same = gst_caps_intersect_full (othercaps, caps, GST_CAPS_INTERSECT_FIRST);
  if (!gst_caps_is_empty (same)) {
    gst_caps_unref (othercaps);
    same = gst_caps_fixate (same);
    GST_DEBUG_OBJECT (trans, "fixated to the input caps %" GST_PTR_FORMAT, same);
    return same;
  }
  gst_caps_unref (same);

  /* Otherwise a simplified videoscale-style fixation: the first allowed structure, as close to the input as it
   * allows. */
  othercaps = gst_caps_truncate (othercaps);
  othercaps = gst_caps_make_writable (othercaps);
  out = gst_caps_get_structure (othercaps, 0);

  if (gst_structure_get_int (in, "width", &v))
    gst_structure_fixate_field_nearest_int (out, "width", v);
  if (gst_structure_get_int (in, "height", &v))
    gst_structure_fixate_field_nearest_int (out, "height", v);
  format = gst_structure_get_string (in, "format");
  if (format != NULL)
    gst_structure_fixate_field_string (out, "format", format);
  if (gst_structure_get_fraction (in, "pixel-aspect-ratio", &n, &d)) {
    if (!gst_structure_has_field (out, "pixel-aspect-ratio"))
      gst_structure_set (out, "pixel-aspect-ratio", GST_TYPE_FRACTION, n, d, NULL);
    else
      gst_structure_fixate_field_nearest_fraction (out, "pixel-aspect-ratio", n, d);
  }
  if (gst_structure_has_field (out, "framerate") && gst_structure_get_fraction (in, "framerate", &n, &d))
    gst_structure_fixate_field_nearest_fraction (out, "framerate", n, d);
  if (same_colour_family (format, gst_structure_get_string (out, "format"))) {
    take_field (out, in, "colorimetry");
    take_field (out, in, "chroma-site");
  }

  othercaps = gst_caps_fixate (othercaps);
  GST_DEBUG_OBJECT (trans, "fixated to %" GST_PTR_FORMAT, othercaps);
  return othercaps;
}

static gboolean
gst_rds_video_convert_set_caps (GstBaseTransform * trans, GstCaps * incaps, GstCaps * outcaps)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);
  gboolean passthrough;

  if (!gst_video_info_from_caps (&self->in_info, incaps) || !gst_video_info_from_caps (&self->out_info, outcaps)) {
    GST_ERROR_OBJECT (self, "invalid caps %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT, incaps, outcaps);
    return FALSE;
  }
  if (rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&self->in_info))
      == RDS_BUF_COLOR_FORMAT_INVALID
      || rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&self->out_info))
      == RDS_BUF_COLOR_FORMAT_INVALID) {
    GST_ERROR_OBJECT (self, "unsupported format in %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT, incaps, outcaps);
    return FALSE;
  }

  GST_OBJECT_LOCK (self);
  self->caps_equal = gst_caps_is_equal (incaps, outcaps);
  passthrough = self->caps_equal && !self->disable_passthrough;
  GST_OBJECT_UNLOCK (self);
  gst_base_transform_set_passthrough (trans, passthrough);
  GST_DEBUG_OBJECT (self, "%" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT ", passthrough %d", incaps, outcaps,
      passthrough);
  return TRUE;
}

static gboolean
gst_rds_video_convert_get_unit_size (G_GNUC_UNUSED GstBaseTransform * trans, GstCaps * caps, gsize * size)
{
  GstVideoInfo info;

  if (rds_caps_has_memory_rds (caps)) {
    *size = sizeof (RdsBufSurface);
    return TRUE;
  }
  if (!gst_video_info_from_caps (&info, caps))
    return FALSE;
  *size = GST_VIDEO_INFO_SIZE (&info);
  return TRUE;
}

/* ---- allocation ---- */

static gboolean
gst_rds_video_convert_propose_allocation (GstBaseTransform * trans, GstQuery * decide_query, GstQuery * query)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);
  GstCaps *caps = NULL;
  guint gpu_id;

  /* passthrough: the base class forwards the query downstream */
  if (decide_query == NULL)
    return GST_BASE_TRANSFORM_CLASS (gst_rds_video_convert_parent_class)->propose_allocation (trans,
        decide_query, query);

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps != NULL && rds_caps_has_memory_rds (caps)) {
    GST_OBJECT_LOCK (self);
    gpu_id = self->gpu_id;
    GST_OBJECT_UNLOCK (self);
    return rds_propose_allocation (query, 1, RDS_BUF_MEM_DEFAULT, gpu_id, 2);
  }
  return GST_BASE_TRANSFORM_CLASS (gst_rds_video_convert_parent_class)->propose_allocation (trans,
      decide_query, query);
}

/* System-memory output: a video pool (reusing downstream's when offered), GstVideoMeta when downstream supports
 * it, then the base class for the allocator and params. */
static gboolean
decide_system_allocation (GstBaseTransform * trans, GstQuery * query)
{
  GstCaps *caps = NULL;
  GstBufferPool *pool = NULL;
  GstStructure *config;
  GstVideoInfo info;
  guint size, min = 0, max = 0;
  gboolean update;

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps == NULL || !gst_video_info_from_caps (&info, caps))
    return FALSE;

  size = GST_VIDEO_INFO_SIZE (&info);
  update = gst_query_get_n_allocation_pools (query) > 0;
  if (update) {
    guint offered = 0;

    gst_query_parse_nth_allocation_pool (query, 0, &pool, &offered, &min, &max);
    size = MAX (size, offered);
  }
  if (pool == NULL)
    pool = gst_video_buffer_pool_new ();

  config = gst_buffer_pool_get_config (pool);
  gst_buffer_pool_config_set_params (config, caps, size, min, max);
  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL))
    gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  if (!gst_buffer_pool_set_config (pool, config)) {
    config = gst_buffer_pool_get_config (pool);
    if (!gst_buffer_pool_config_validate_params (config, caps, size, min, max)
        || !gst_buffer_pool_set_config (pool, config)) {
      GST_ERROR_OBJECT (trans, "cannot configure the output pool");
      gst_object_unref (pool);
      return FALSE;
    }
  }
  if (update)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);
  gst_object_unref (pool);

  return GST_BASE_TRANSFORM_CLASS (gst_rds_video_convert_parent_class)->decide_allocation (trans, query);
}

static gboolean
gst_rds_video_convert_decide_allocation (GstBaseTransform * trans, GstQuery * query)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);
  GstCaps *caps = NULL;
  GstBufferPool *pool;
  RdsBufSurfaceMemType mem_type;
  guint gpu_id, output_buffers;

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps == NULL)
    return FALSE;
  if (!rds_caps_has_memory_rds (caps))
    return decide_system_allocation (trans, query);

  GST_OBJECT_LOCK (self);
  mem_type = (RdsBufSurfaceMemType) self->memory_type;
  gpu_id = self->gpu_id;
  output_buffers = self->output_buffers;
  GST_OBJECT_UNLOCK (self);

  /* writes the pool into the query, where the base class picks it up */
  pool = rds_decide_allocation (query, 1, mem_type, gpu_id, output_buffers, 0);
  if (pool == NULL) {
    GST_ERROR_OBJECT (self, "cannot set up a memory:RDS output pool for %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
  gst_object_unref (pool);
  return TRUE;
}

/* ---- metadata ---- */

static gboolean
gst_rds_video_convert_transform_meta (G_GNUC_UNUSED GstBaseTransform * trans, G_GNUC_UNUSED GstBuffer * outbuf,
    GstMeta * meta, G_GNUC_UNUSED GstBuffer * inbuf)
{
  GType api = meta->info->api;
  const gchar *const *tags = gst_meta_api_type_get_tags (api);

  /* The output buffer describes its own layout, and a crop meta has been applied by the conversion. */
  if (api == GST_VIDEO_META_API_TYPE || api == GST_VIDEO_CROP_META_API_TYPE)
    return FALSE;
  /* Untagged metas (RdsMeta among them) do not depend on the pixels' size, format or memory. So do metas tagged
   * only "video". */
  if (tags == NULL || tags[0] == NULL)
    return TRUE;
  return tags[1] == NULL && g_strcmp0 (tags[0], GST_META_TAG_VIDEO_STR) == 0;
}

/* ---- streaming ---- */

static gboolean
gst_rds_video_convert_start (GstBaseTransform * trans)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);
  int prev = 0;
  guint gpu_id;
  hipError_t err;

  GST_OBJECT_LOCK (self);
  gpu_id = self->gpu_id;
  GST_OBJECT_UNLOCK (self);

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
  return TRUE;
}

static gboolean
gst_rds_video_convert_stop (GstBaseTransform * trans)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);

  if (self->stream != NULL) {
    if (hipStreamDestroy (self->stream) != hipSuccess)
      GST_WARNING_OBJECT (self, "hipStreamDestroy failed");
    self->stream = NULL;
  }
  GST_OBJECT_LOCK (self);
  self->caps_equal = FALSE;
  GST_OBJECT_UNLOCK (self);
  return TRUE;
}

/* The input's crop meta, clamped to the frame, as a source rect. FALSE if there is none (or it is empty). */
static gboolean
crop_meta_rect (GstRdsVideoConvert * self, GstBuffer * inbuf, RdsBufSurfTransformRect * rect)
{
  GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta (inbuf);
  guint w = GST_VIDEO_INFO_WIDTH (&self->in_info), h = GST_VIDEO_INFO_HEIGHT (&self->in_info);

  if (crop == NULL || crop->x >= w || crop->y >= h)
    return FALSE;
  rect->left = crop->x;
  rect->top = crop->y;
  rect->width = MIN (crop->width, w - crop->x);
  rect->height = MIN (crop->height, h - crop->y);
  if (rect->left == 0 && rect->top == 0 && rect->width == w && rect->height == h)
    return FALSE;
  return rect->width > 0 && rect->height > 0;
}

static GstFlowReturn
gst_rds_video_convert_transform (GstBaseTransform * trans, GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);
  RdsBufSurfTransformConfigParams session = {
    .compute_mode = RdsBufSurfTransformCompute_Default,
    .gpu_id = (gint32) self->stream_gpu,
    .cuda_stream = self->stream,
  };
  RdsBufSurfTransformParams params = { 0 };
  RdsBufSurfTransformRect src_rect;
  RdsBufSurfTransform_Error err;
  GstMapInfo in_map, out_map;
  RdsBufSurface *src, *dst;
  GstFlowReturn ret = GST_FLOW_OK;

  if (rds_buffer_get_surface (inbuf) == NULL || rds_buffer_get_surface (outbuf) == NULL) {
    GST_ELEMENT_ERROR (self, STREAM, NOT_IMPLEMENTED,
        ("System-memory conversion is not implemented yet"), ("only memory:RDS to memory:RDS is supported"));
    return GST_FLOW_ERROR;
  }
  /* Mapping yields the surface; a WRITE map makes sure the output surface is ours alone. */
  if (!gst_buffer_map (inbuf, &in_map, GST_MAP_READ))
    goto map_failed;
  if (!gst_buffer_map (outbuf, &out_map, GST_MAP_WRITE)) {
    gst_buffer_unmap (inbuf, &in_map);
    goto map_failed;
  }
  src = (RdsBufSurface *) in_map.data;
  dst = (RdsBufSurface *) out_map.data;

  if (src->batchSize != 1 || dst->batchSize != 1) {
    GST_ELEMENT_ERROR (self, STREAM, NOT_IMPLEMENTED, ("Batched input arrives in Phase 4"),
        ("input batch size %u, output batch size %u; only 1 is supported", src->batchSize, dst->batchSize));
    ret = GST_FLOW_ERROR;
    goto done;
  }

  if (crop_meta_rect (self, inbuf, &src_rect)) {
    params.transform_flag |= RDS_BUFSURF_TRANSFORM_CROP_SRC;
    params.src_rect = &src_rect;
  }

  /* Session parameters are per thread and shared by every element on this streaming thread: set ours before
   * every call. */
  err = rds_buf_surf_transform_set_session_params (&session);
  if (err == RdsBufSurfTransformError_Success)
    err = rds_buf_surf_transform (src, dst, &params);
  if (err != RdsBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Video conversion failed"),
        ("rds_buf_surf_transform: %s (%d)", transform_error_name (err), err));
    ret = GST_FLOW_ERROR;
  }
  /* The transform is synchronous: the output is ready when it is pushed. */

done:
  gst_buffer_unmap (outbuf, &out_map);
  gst_buffer_unmap (inbuf, &in_map);
  return ret;

map_failed:
  GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map a memory:RDS buffer"), (NULL));
  return GST_FLOW_ERROR;
}

/* ---- properties ---- */

/* Allocation settings are read when the pools are set up: refuse changes once streaming, including during the
 * READY -> PAUSED transition (object lock held, which also guards the state fields). */
static gboolean
check_mutable (GstRdsVideoConvert * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_video_convert_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (object);
  GstBaseTransform *trans = GST_BASE_TRANSFORM (object);
  gboolean update_passthrough = FALSE, passthrough = FALSE;

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_GPU_ID:
      if (check_mutable (self, pspec))
        self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      if (check_mutable (self, pspec))
        self->memory_type = g_value_get_enum (value);
      break;
    case PROP_OUTPUT_BUFFERS:
      if (check_mutable (self, pspec))
        self->output_buffers = g_value_get_uint (value);
      break;
    case PROP_DISABLE_PASSTHROUGH:
      if (self->disable_passthrough != g_value_get_boolean (value)) {
        self->disable_passthrough = g_value_get_boolean (value);
        passthrough = self->caps_equal && !self->disable_passthrough;
        update_passthrough = TRUE;
      }
      break;
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);

  if (update_passthrough) {
    gboolean was = gst_base_transform_is_passthrough (trans);

    /* Narrow race, accepted: set_caps on the streaming thread may compute passthrough from new caps between our
     * unlock and set_passthrough below, and the last writer wins. It only matters if the caps change at that very
     * moment; the next caps or property change corrects it. */
    /* Same caps make the base class skip set_caps on renegotiation, so switch here; reconfiguring the source pad
     * then sets up (or drops) the output pool before the next buffer. */
    gst_base_transform_set_passthrough (trans, passthrough);
    gst_base_transform_reconfigure_src (trans);
    /* upstream's allocation (our proposal or downstream's) changes with passthrough */
    if (was != gst_base_transform_is_passthrough (trans))
      gst_pad_push_event (GST_BASE_TRANSFORM_SINK_PAD (trans), gst_event_new_reconfigure ());
  }
}

static void
gst_rds_video_convert_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      g_value_set_enum (value, self->memory_type);
      break;
    case PROP_OUTPUT_BUFFERS:
      g_value_set_uint (value, self->output_buffers);
      break;
    case PROP_DISABLE_PASSTHROUGH:
      g_value_set_boolean (value, self->disable_passthrough);
      break;
    case PROP_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_video_convert_class_init (GstRdsVideoConvertClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  GST_DEBUG_CATEGORY_INIT (gst_rds_video_convert_debug, "rdsvideoconvert", 0,
      "Redstream memory:RDS video converter");

  gobject_class->set_property = gst_rds_video_convert_set_property;
  gobject_class->get_property = gst_rds_video_convert_get_property;

  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU id", "GPU used for the conversion", 0, G_MAXUINT, DEFAULT_GPU_ID,
          flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_NVBUF_MEMORY_TYPE,
      g_param_spec_enum ("nvbuf-memory-type", "Memory type", "Memory type of memory:RDS output buffers",
          GST_TYPE_RDS_VID_CONV_BUF_MEMORY_TYPE, DEFAULT_MEMORY_TYPE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_OUTPUT_BUFFERS,
      g_param_spec_uint ("output-buffers", "Output buffers", "Minimum number of buffers in the output pool",
          1, G_MAXUINT, DEFAULT_OUTPUT_BUFFERS, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_DISABLE_PASSTHROUGH,
      g_param_spec_boolean ("disable-passthrough", "Disable passthrough",
          "Convert even when the input and output caps are the same", DEFAULT_DISABLE_PASSTHROUGH, flags));
  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Accepted for compatibility; has no effect", DEFAULT_SILENT,
          flags));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream video converter",
      "Filter/Converter/Video/Scaler", "Converts, scales and crops video in memory:RDS (HIP) or system memory",
      "Redstream contributors");

  trans_class->transform_caps = GST_DEBUG_FUNCPTR (gst_rds_video_convert_transform_caps);
  trans_class->fixate_caps = GST_DEBUG_FUNCPTR (gst_rds_video_convert_fixate_caps);
  trans_class->set_caps = GST_DEBUG_FUNCPTR (gst_rds_video_convert_set_caps);
  trans_class->get_unit_size = GST_DEBUG_FUNCPTR (gst_rds_video_convert_get_unit_size);
  trans_class->propose_allocation = GST_DEBUG_FUNCPTR (gst_rds_video_convert_propose_allocation);
  trans_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_rds_video_convert_decide_allocation);
  trans_class->transform_meta = GST_DEBUG_FUNCPTR (gst_rds_video_convert_transform_meta);
  trans_class->start = GST_DEBUG_FUNCPTR (gst_rds_video_convert_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_rds_video_convert_stop);
  trans_class->transform = GST_DEBUG_FUNCPTR (gst_rds_video_convert_transform);
  trans_class->passthrough_on_same_caps = FALSE;

  gst_type_mark_as_plugin_api (GST_TYPE_RDS_VID_CONV_BUF_MEMORY_TYPE, 0);
}

static void
gst_rds_video_convert_init (GstRdsVideoConvert * self)
{
  self->gpu_id = DEFAULT_GPU_ID;
  self->memory_type = DEFAULT_MEMORY_TYPE;
  self->output_buffers = DEFAULT_OUTPUT_BUFFERS;
  self->disable_passthrough = DEFAULT_DISABLE_PASSTHROUGH;
  self->silent = DEFAULT_SILENT;
  gst_video_info_init (&self->in_info);
  gst_video_info_init (&self->out_info);
}
