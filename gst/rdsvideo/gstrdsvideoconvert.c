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
#define DEFAULT_INTERPOLATION_METHOD RdsBufSurfTransformInter_Default
#define DEFAULT_COMPUTE_HW 0
#define DEFAULT_FLIP_METHOD 0
#define DEFAULT_ALLOW_ODD_CROP TRUE
#define DEFAULT_CONTIGUOUS_BUFFERS FALSE
#define DEFAULT_COPY_HW 1
#define DEFAULT_BL_OUTPUT FALSE

#define COMPUTE_HW_VIC 2

enum
{
  PROP_0,
  PROP_GPU_ID,
  PROP_NVBUF_MEMORY_TYPE,
  PROP_OUTPUT_BUFFERS,
  PROP_DISABLE_PASSTHROUGH,
  PROP_SILENT,
  PROP_SRC_CROP,
  PROP_DEST_CROP,
  PROP_INTERPOLATION_METHOD,
  PROP_COMPUTE_HW,
  PROP_FLIP_METHOD,
  PROP_ALLOW_ODD_CROP,
  PROP_CONTIGUOUS_BUFFERS,
  PROP_COPY_HW,
  PROP_BL_OUTPUT,
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
  gchar *src_crop_str;          /* as set; "" = whole frame */
  gchar *dest_crop_str;
  RdsBufSurfTransformRect src_crop;     /* parsed; all zero = whole frame */
  RdsBufSurfTransformRect dest_crop;
  gint interpolation_method;    /* RdsBufSurfTransform_Inter */
  gint compute_hw;              /* GstRdsComputeHW == RdsBufSurfTransform_Compute */
  gint flip_method;
  gboolean allow_odd_crop;
  gboolean contiguous_buffers;  /* no-op: batches are always contiguous */
  gint copy_hw;                 /* no-op: copies run on the GPU */
  gboolean bl_output;           /* no-op: Jetson only */

  /* streaming state */
  hipStream_t stream;
  guint stream_gpu;             /* the GPU the stream (and every transform) runs on */
  GstVideoInfo in_info;
  GstVideoInfo out_info;
  gboolean caps_equal;          /* negotiated input and output caps are equal (object lock) */
  gboolean in_rds, out_rds;     /* negotiated memory:RDS on each side */
  RdsBufSurface *in_stage;      /* device copies of system-memory frames (created on first use) */
  RdsBufSurface *out_stage;
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

/* The integers below are DeepStream's (configs use them); the nicks and descriptions are our own. */
#define GST_TYPE_RDS_INTERPOLATION_METHOD (gst_rds_interpolation_method_get_type ())
static GType
gst_rds_interpolation_method_get_type (void)
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
    GType t = g_enum_register_static ("GstRdsInterpolationMethod", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

#define GST_TYPE_RDS_COMPUTE_HW (gst_rds_compute_hw_get_type ())
static GType
gst_rds_compute_hw_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {0, "Platform default (the GPU)", "default"},
    {1, "GPU", "gpu"},
    {COMPUTE_HW_VIC, "NVIDIA Jetson VIC engine (not available: fails at start)", "vic"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsComputeHW", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

#define GST_TYPE_RDS_VIDEO_FLIP_METHOD (gst_rds_video_flip_method_get_type ())
static GType
gst_rds_video_flip_method_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {0, "No rotation or flip", "none"},
    {1, "Rotate 90 degrees counter-clockwise (not supported yet)", "counterclockwise"},
    {2, "Rotate 180 degrees (not supported yet)", "rotate-180"},
    {3, "Rotate 90 degrees clockwise (not supported yet)", "clockwise"},
    {4, "Mirror left to right (not supported yet)", "horizontal-flip"},
    {5, "Mirror across the upper-right/lower-left diagonal (not supported yet)", "upper-right-diagonal"},
    {6, "Mirror top to bottom (not supported yet)", "vertical-flip"},
    {7, "Mirror across the upper-left/lower-right diagonal (not supported yet)", "upper-left-diagonal"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsVideoFlipMethod", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

#define GST_TYPE_RDS_COPY_HW_TYPE (gst_rds_copy_hw_type_get_type ())
static GType
gst_rds_copy_hw_type_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {1, "GPU", "gpu"},
    {2, "NVIDIA Jetson VIC engine (copies still run on the GPU)", "vic"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsCopyHWType", values);
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

static gboolean
rect_is_set (const RdsBufSurfTransformRect * r)
{
  return (r->left | r->top | r->width | r->height) != 0;
}

/* Object lock held. Passthrough needs equal caps and nothing that changes the pixels. */
static gboolean
want_passthrough (GstRdsVideoConvert * self)
{
  return self->caps_equal && !self->disable_passthrough && !rect_is_set (&self->src_crop)
      && !rect_is_set (&self->dest_crop) && self->flip_method == 0;
}

static void
free_stages (GstRdsVideoConvert * self)
{
  if (self->in_stage != NULL)
    rds_buf_surface_destroy (self->in_stage);
  if (self->out_stage != NULL)
    rds_buf_surface_destroy (self->out_stage);
  self->in_stage = self->out_stage = NULL;
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
  GstVideoInfo in_info, out_info;
  gboolean passthrough;

  if (!gst_video_info_from_caps (&in_info, incaps) || !gst_video_info_from_caps (&out_info, outcaps)) {
    GST_ERROR_OBJECT (self, "invalid caps %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT, incaps, outcaps);
    return FALSE;
  }
  if (rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&in_info)) == RDS_BUF_COLOR_FORMAT_INVALID
      || rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&out_info))
      == RDS_BUF_COLOR_FORMAT_INVALID) {
    GST_ERROR_OBJECT (self, "unsupported format in %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT, incaps, outcaps);
    return FALSE;
  }
  /* staging surfaces have the old geometry: recreated on the next frame that needs them */
  if (self->in_stage != NULL && !gst_video_info_is_equal (&in_info, &self->in_info)) {
    rds_buf_surface_destroy (self->in_stage);
    self->in_stage = NULL;
  }
  if (self->out_stage != NULL && !gst_video_info_is_equal (&out_info, &self->out_info)) {
    rds_buf_surface_destroy (self->out_stage);
    self->out_stage = NULL;
  }
  self->in_info = in_info;
  self->out_info = out_info;
  self->in_rds = rds_caps_has_memory_rds (incaps);
  self->out_rds = rds_caps_has_memory_rds (outcaps);

  GST_OBJECT_LOCK (self);
  self->caps_equal = gst_caps_is_equal (incaps, outcaps);
  passthrough = want_passthrough (self);
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

/* Settings we cannot honour: checked at start and again per frame (they may change while playing). */
static gboolean
check_settings (GstRdsVideoConvert * self)
{
  gint compute_hw, flip_method;

  GST_OBJECT_LOCK (self);
  compute_hw = self->compute_hw;
  flip_method = self->flip_method;
  GST_OBJECT_UNLOCK (self);

  if (compute_hw == COMPUTE_HW_VIC) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("compute-hw=VIC is not available"),
        ("only the GPU (compute-hw 0 or 1) is supported"));
    return FALSE;
  }
  if (flip_method != 0) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("flip-method %d is not supported yet", flip_method),
        ("only flip-method 0 (no rotation or flip) is supported"));
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_rds_video_convert_start (GstBaseTransform * trans)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);
  int prev = 0;
  guint gpu_id;
  hipError_t err;

  if (!check_settings (self))
    return FALSE;

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

  free_stages (self);
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

/* The part of the input frame the conversion reads before src-crop: the input's crop meta clamped to the frame,
 * else the whole frame. TRUE if a crop meta narrowed it. */
static gboolean
input_base_rect (GstRdsVideoConvert * self, GstBuffer * inbuf, RdsBufSurfTransformRect * rect)
{
  GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta (inbuf);
  guint w = GST_VIDEO_INFO_WIDTH (&self->in_info), h = GST_VIDEO_INFO_HEIGHT (&self->in_info);

  rect->left = rect->top = 0;
  rect->width = w;
  rect->height = h;
  if (crop == NULL)
    return FALSE;
  if (crop->x >= w || crop->y >= h || crop->width == 0 || crop->height == 0) {
    GST_DEBUG_OBJECT (self, "ignoring crop meta %ux%u at %u,%u: nothing of it lies in the %ux%u frame",
        crop->width, crop->height, crop->x, crop->y, w, h);
    return FALSE;
  }
  rect->left = crop->x;
  rect->top = crop->y;
  rect->width = MIN (crop->width, w - crop->x);
  rect->height = MIN (crop->height, h - crop->y);
  return rect->left != 0 || rect->top != 0 || rect->width != w || rect->height != h;
}

/* @crop (relative to @base) clamped into @base, as nvvideoconvert documents for out-of-range crops. */
static RdsBufSurfTransformRect
clamp_rect (const RdsBufSurfTransformRect * base, const RdsBufSurfTransformRect * crop)
{
  RdsBufSurfTransformRect r;
  guint32 left = MIN (crop->left, base->width), top = MIN (crop->top, base->height);

  r.left = base->left + left;
  r.top = base->top + top;
  r.width = MIN (crop->width, base->width - left);
  r.height = MIN (crop->height, base->height - top);
  return r;
}

/* The black around dest-crop: Y 16, U/V 128 for YUV; every byte 0 for GRAY8 and RGB formats (alpha too). */
static gboolean
fill_black (RdsBufSurface * surf)
{
  RdsBufSurfaceParams *p = &surf->surfaceList[0];
  gboolean yuv = p->colorFormat == RDS_BUF_COLOR_FORMAT_NV12 || p->colorFormat == RDS_BUF_COLOR_FORMAT_YUV420;

  for (guint plane = 0; plane < p->planeParams.num_planes; plane++) {
    guint8 v = yuv ? (plane == 0 ? 16 : 128) : 0;

    if (rds_buf_surface_memset (surf, 0, (int) plane, v) != 0)
      return FALSE;
  }
  return TRUE;
}

/* A device surface with @info's geometry for staging system-memory frames. */
static gboolean
ensure_stage (GstRdsVideoConvert * self, RdsBufSurface ** stage, const GstVideoInfo * info)
{
  RdsBufSurfaceCreateParams cp = {
    .gpuId = self->stream_gpu,
    .width = GST_VIDEO_INFO_WIDTH (info),
    .height = GST_VIDEO_INFO_HEIGHT (info),
    .colorFormat = rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (info)),
    .layout = RDS_BUF_LAYOUT_PITCH,
    .memType = RDS_BUF_MEM_DEFAULT,
  };

  if (*stage != NULL)
    return TRUE;
  if (rds_buf_surface_create (stage, 1, &cp) != 0) {
    *stage = NULL;
    return FALSE;
  }
  return TRUE;
}

static GstFlowReturn
gst_rds_video_convert_transform (GstBaseTransform * trans, GstBuffer * inbuf, GstBuffer * outbuf)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (trans);
  RdsBufSurfTransformConfigParams session = {
    .gpu_id = (gint32) self->stream_gpu,
    .cuda_stream = self->stream,
  };
  RdsBufSurfTransformConfigParams prev_session;
  RdsBufSurfTransformParams params = { 0 };
  RdsBufSurfTransformRect src_crop, dest_crop, base, src_rect, dst_rect;
  RdsBufSurfTransform_Error err;
  GstMapInfo in_map, out_map;
  GstVideoFrame frame;
  RdsBufSurface *src, *dst;
  gboolean in_mapped = FALSE, out_mapped = FALSE, allow_odd_crop, meta_crop;
  gint filter;
  GstFlowReturn ret = GST_FLOW_ERROR;

  if (!check_settings (self))
    return GST_FLOW_ERROR;
  GST_OBJECT_LOCK (self);
  src_crop = self->src_crop;
  dest_crop = self->dest_crop;
  filter = self->interpolation_method;
  allow_odd_crop = self->allow_odd_crop;
  session.compute_mode = (RdsBufSurfTransform_Compute) self->compute_hw;
  GST_OBJECT_UNLOCK (self);

  /* Rects first: nvvideoconvert clamps crops to the frame. src-crop is relative to the input's crop meta. */
  meta_crop = input_base_rect (self, inbuf, &base);
  src_rect = rect_is_set (&src_crop) ? clamp_rect (&base, &src_crop) : base;
  if (rect_is_set (&dest_crop)) {
    RdsBufSurfTransformRect frame_rect = { 0, 0, GST_VIDEO_INFO_WIDTH (&self->out_info),
      GST_VIDEO_INFO_HEIGHT (&self->out_info)
    };
    dst_rect = clamp_rect (&frame_rect, &dest_crop);
  } else {
    dst_rect = (RdsBufSurfTransformRect) {
    0, 0, GST_VIDEO_INFO_WIDTH (&self->out_info), GST_VIDEO_INFO_HEIGHT (&self->out_info)};
  }
  if (src_rect.width == 0 || src_rect.height == 0 || dst_rect.width == 0 || dst_rect.height == 0) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("The crop rectangle is empty after clamping it to the frame"),
        ("source %ux%u at %u,%u, destination %ux%u at %u,%u", src_rect.width, src_rect.height, src_rect.left,
            src_rect.top, dst_rect.width, dst_rect.height, dst_rect.left, dst_rect.top));
    return GST_FLOW_ERROR;
  }

  /* Source: the memory:RDS surface itself, or the system frame uploaded into the staging surface. */
  if (self->in_rds) {
    if (rds_buffer_get_surface (inbuf) == NULL || !gst_buffer_map (inbuf, &in_map, GST_MAP_READ))
      goto map_failed;
    in_mapped = TRUE;
    src = (RdsBufSurface *) in_map.data;
  } else {
    gboolean ok;

    if (!ensure_stage (self, &self->in_stage, &self->in_info))
      goto stage_failed;
    if (!gst_video_frame_map (&frame, &self->in_info, inbuf, GST_MAP_READ))
      goto map_failed;
    ok = rds_video_frame_copy_surface (&frame, self->in_stage, TRUE);
    gst_video_frame_unmap (&frame);
    if (!ok)
      goto copy_failed;
    src = self->in_stage;
  }

  /* Destination: the output surface (a WRITE map makes it ours alone), or the staging surface downloaded after. */
  if (self->out_rds) {
    if (rds_buffer_get_surface (outbuf) == NULL || !gst_buffer_map (outbuf, &out_map, GST_MAP_WRITE))
      goto map_failed;
    out_mapped = TRUE;
    dst = (RdsBufSurface *) out_map.data;
  } else {
    if (!ensure_stage (self, &self->out_stage, &self->out_info))
      goto stage_failed;
    dst = self->out_stage;
  }

  if (src->batchSize != 1 || dst->batchSize != 1) {
    GST_ELEMENT_ERROR (self, STREAM, NOT_IMPLEMENTED, ("Batched input (batch size above 1) is not supported yet"),
        ("input batch size %u, output batch size %u; only 1 is supported", src->batchSize, dst->batchSize));
    goto done;
  }

  params.transform_flag = RDS_BUFSURF_TRANSFORM_FILTER;
  params.transform_filter = (RdsBufSurfTransform_Inter) filter;
  if (allow_odd_crop)
    params.transform_flag |= RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP;
  if (meta_crop || rect_is_set (&src_crop)) {
    params.transform_flag |= RDS_BUFSURF_TRANSFORM_CROP_SRC;
    params.src_rect = &src_rect;
  }
  if (dst_rect.left != 0 || dst_rect.top != 0 || dst_rect.width != (guint32) GST_VIDEO_INFO_WIDTH (&self->out_info)
      || dst_rect.height != (guint32) GST_VIDEO_INFO_HEIGHT (&self->out_info)) {
    params.transform_flag |= RDS_BUFSURF_TRANSFORM_CROP_DST;
    params.dst_rect = &dst_rect;
    /* the transform leaves pixels outside the rect alone: black them first (synchronous memset) */
    if (!fill_black (dst)) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot clear the output frame"), (NULL));
      goto done;
    }
  }

  /* Session parameters are per thread and shared by every element on this streaming thread: set ours before
   * every call and restore the previous one right after, so later callers on this thread never inherit our
   * stream (it is destroyed in stop ()). A failed set leaves the session unchanged. */
  (void) rds_buf_surf_transform_get_session_params (&prev_session);
  err = rds_buf_surf_transform_set_session_params (&session);
  if (err == RdsBufSurfTransformError_Success) {
    err = rds_buf_surf_transform (src, dst, &params);
    if (rds_buf_surf_transform_set_session_params (&prev_session) != RdsBufSurfTransformError_Success)
      GST_WARNING_OBJECT (self, "cannot restore the thread's previous transform session");
  }
  if (err != RdsBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Video conversion failed"),
        ("rds_buf_surf_transform: %s (%d)", transform_error_name (err), err));
    goto done;
  }
  /* The transform is synchronous: the output is ready when it is pushed (and the download below sees it). */

  if (!self->out_rds) {
    gboolean ok;

    if (!gst_video_frame_map (&frame, &self->out_info, outbuf, GST_MAP_WRITE))
      goto map_failed;
    ok = rds_video_frame_copy_surface (&frame, self->out_stage, FALSE);
    gst_video_frame_unmap (&frame);
    if (!ok)
      goto copy_failed;
  }
  ret = GST_FLOW_OK;

done:
  if (out_mapped)
    gst_buffer_unmap (outbuf, &out_map);
  if (in_mapped)
    gst_buffer_unmap (inbuf, &in_map);
  return ret;

map_failed:
  GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map a buffer"), ("memory:RDS input %d, output %d",
          self->in_rds, self->out_rds));
  goto done;
stage_failed:
  GST_ELEMENT_ERROR (self, RESOURCE, NO_SPACE_LEFT, ("Cannot allocate a staging surface on GPU %u",
          self->stream_gpu), (NULL));
  goto done;
copy_failed:
  GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot copy a frame between system and device memory"), (NULL));
  goto done;
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
      self->disable_passthrough = g_value_get_boolean (value);
      update_passthrough = TRUE;
      break;
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    case PROP_SRC_CROP:
    case PROP_DEST_CROP:{
      const gchar *str = g_value_get_string (value);
      RdsBufSurfTransformRect rect;
      gchar **slot = prop_id == PROP_SRC_CROP ? &self->src_crop_str : &self->dest_crop_str;

      if (!rds_video_parse_rect (str, &rect)) {
        g_warning ("%s: invalid %s \"%s\" (expected left:top:width:height); keeping \"%s\"",
            GST_OBJECT_NAME (self), pspec->name, str, *slot);
        break;
      }
      g_free (*slot);
      *slot = g_strdup (str != NULL ? str : "");
      if (prop_id == PROP_SRC_CROP)
        self->src_crop = rect;
      else
        self->dest_crop = rect;
      update_passthrough = TRUE;
      break;
    }
    case PROP_INTERPOLATION_METHOD:
      self->interpolation_method = g_value_get_enum (value);
      break;
    case PROP_COMPUTE_HW:
      self->compute_hw = g_value_get_enum (value);
      break;
    case PROP_FLIP_METHOD:
      self->flip_method = g_value_get_enum (value);
      update_passthrough = TRUE;
      break;
    case PROP_ALLOW_ODD_CROP:
      self->allow_odd_crop = g_value_get_boolean (value);
      break;
    case PROP_CONTIGUOUS_BUFFERS:
      self->contiguous_buffers = g_value_get_boolean (value);
      break;
    case PROP_COPY_HW:
      self->copy_hw = g_value_get_enum (value);
      break;
    case PROP_BL_OUTPUT:
      self->bl_output = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  if (update_passthrough)
    passthrough = want_passthrough (self);
  GST_OBJECT_UNLOCK (self);

  if (update_passthrough && passthrough != gst_base_transform_is_passthrough (trans)) {
    /* Narrow race, accepted: set_caps on the streaming thread may compute passthrough from new caps between our
     * unlock and set_passthrough below, and the last writer wins. It only matters if the caps change at that very
     * moment; the next caps or property change corrects it. */
    /* Same caps make the base class skip set_caps on renegotiation, so switch here; reconfiguring the source pad
     * then sets up (or drops) the output pool before the next buffer. */
    gst_base_transform_set_passthrough (trans, passthrough);
    gst_base_transform_reconfigure_src (trans);
    /* upstream's allocation (our proposal or downstream's) changes with passthrough */
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
    case PROP_SRC_CROP:
      g_value_set_string (value, self->src_crop_str);
      break;
    case PROP_DEST_CROP:
      g_value_set_string (value, self->dest_crop_str);
      break;
    case PROP_INTERPOLATION_METHOD:
      g_value_set_enum (value, self->interpolation_method);
      break;
    case PROP_COMPUTE_HW:
      g_value_set_enum (value, self->compute_hw);
      break;
    case PROP_FLIP_METHOD:
      g_value_set_enum (value, self->flip_method);
      break;
    case PROP_ALLOW_ODD_CROP:
      g_value_set_boolean (value, self->allow_odd_crop);
      break;
    case PROP_CONTIGUOUS_BUFFERS:
      g_value_set_boolean (value, self->contiguous_buffers);
      break;
    case PROP_COPY_HW:
      g_value_set_enum (value, self->copy_hw);
      break;
    case PROP_BL_OUTPUT:
      g_value_set_boolean (value, self->bl_output);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_video_convert_finalize (GObject * object)
{
  GstRdsVideoConvert *self = GST_RDS_VIDEO_CONVERT (object);

  g_free (self->src_crop_str);
  g_free (self->dest_crop_str);
  G_OBJECT_CLASS (gst_rds_video_convert_parent_class)->finalize (object);
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
  gobject_class->finalize = gst_rds_video_convert_finalize;

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
  g_object_class_install_property (gobject_class, PROP_SRC_CROP,
      g_param_spec_string ("src-crop", "Source crop",
          "Part of the input to convert, as left:top:width:height; clamped to the frame and, when the input has a "
          "crop meta, relative to that crop. Empty: everything", "", flags));
  g_object_class_install_property (gobject_class, PROP_DEST_CROP,
      g_param_spec_string ("dest-crop", "Destination crop",
          "Where the picture goes in the output, as left:top:width:height; clamped to the frame, the rest is "
          "filled with black. Empty: the whole frame", "", flags));
  g_object_class_install_property (gobject_class, PROP_INTERPOLATION_METHOD,
      g_param_spec_enum ("interpolation-method", "Interpolation method", "Filter used for scaling",
          GST_TYPE_RDS_INTERPOLATION_METHOD, DEFAULT_INTERPOLATION_METHOD, flags));
  g_object_class_install_property (gobject_class, PROP_COMPUTE_HW,
      g_param_spec_enum ("compute-hw", "Compute hardware", "Engine that runs the conversion",
          GST_TYPE_RDS_COMPUTE_HW, DEFAULT_COMPUTE_HW, flags));
  g_object_class_install_property (gobject_class, PROP_FLIP_METHOD,
      g_param_spec_enum ("flip-method", "Flip method", "Rotation or mirroring (only 0 is supported yet)",
          GST_TYPE_RDS_VIDEO_FLIP_METHOD, DEFAULT_FLIP_METHOD, flags));
  g_object_class_install_property (gobject_class, PROP_ALLOW_ODD_CROP,
      g_param_spec_boolean ("allow-odd-crop", "Allow odd crop",
          "Accept odd destination crops on 4:2:0 output by widening them to even coordinates",
          DEFAULT_ALLOW_ODD_CROP, flags));
  g_object_class_install_property (gobject_class, PROP_CONTIGUOUS_BUFFERS,
      g_param_spec_boolean ("contiguous-buffers", "Contiguous buffers",
          "Accepted for compatibility; batches are always contiguous", DEFAULT_CONTIGUOUS_BUFFERS, flags));
  g_object_class_install_property (gobject_class, PROP_COPY_HW,
      g_param_spec_enum ("copy-hw", "Copy hardware", "Accepted for compatibility; copies run on the GPU",
          GST_TYPE_RDS_COPY_HW_TYPE, DEFAULT_COPY_HW, flags));
  g_object_class_install_property (gobject_class, PROP_BL_OUTPUT,
      g_param_spec_boolean ("bl-output", "Block-linear output",
          "Accepted for compatibility (Jetson only); output is always pitch-linear", DEFAULT_BL_OUTPUT, flags));

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
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_INTERPOLATION_METHOD, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_COMPUTE_HW, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_VIDEO_FLIP_METHOD, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_COPY_HW_TYPE, 0);
}

static void
gst_rds_video_convert_init (GstRdsVideoConvert * self)
{
  self->gpu_id = DEFAULT_GPU_ID;
  self->memory_type = DEFAULT_MEMORY_TYPE;
  self->output_buffers = DEFAULT_OUTPUT_BUFFERS;
  self->disable_passthrough = DEFAULT_DISABLE_PASSTHROUGH;
  self->silent = DEFAULT_SILENT;
  self->src_crop_str = g_strdup ("");
  self->dest_crop_str = g_strdup ("");
  self->interpolation_method = DEFAULT_INTERPOLATION_METHOD;
  self->compute_hw = DEFAULT_COMPUTE_HW;
  self->flip_method = DEFAULT_FLIP_METHOD;
  self->allow_odd_crop = DEFAULT_ALLOW_ODD_CROP;
  self->contiguous_buffers = DEFAULT_CONTIGUOUS_BUFFERS;
  self->copy_hw = DEFAULT_COPY_HW;
  self->bl_output = DEFAULT_BL_OUTPUT;
  gst_video_info_init (&self->in_info);
  gst_video_info_init (&self->out_info);
}
