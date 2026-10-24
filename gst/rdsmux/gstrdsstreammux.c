/* Redstream: rdsstreammux, batches frames from several sources into one memory:RDS buffer with an RdsBatchMeta.
 * Every frame is scaled (and colour-converted) into its own fixed-size slot of a pooled batch surface, as
 * DeepStream's default nvstreammux does. Property names and enum integers follow its public documentation.
 *
 * Batching: a GstAggregator. One aggregate () call builds one batch from at most one buffer per sink pad, visiting
 * the pads round robin (the pad visited first rotates every batch), and stops at batch-size frames.
 * - batched-push-timeout -1 (no timeout): the aggregator is not live, so aggregate () runs once every pad that is not
 *   at EOS has a buffer.
 * - batched-push-timeout >= 0: forced-live aggregation with the aggregator latency set to the timeout, and
 *   get_next_time returning the running time at which the batch's first buffer was seen. A partial batch is then
 *   pushed one timeout after its first buffer, wherever the buffer timestamps sit relative to the clock. (The
 *   aggregator's stock "output position" next time fires late when a file decodes faster than real time and at
 *   once when it decodes slower.)
 * Pool pressure: a live or forced-live aggregator pad queues at least two buffers plus a latency's worth, on top of
 * the one being batched; upstream pools must allow for that.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsstreammux.h"
#include "../rdsvideo/rds-video-util.h"

#include <gst/video/video.h>
#include <rds/rds.h>
/* after GLib/GStreamer: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_stream_mux_debug);
#define GST_CAT_DEFAULT gst_rds_stream_mux_debug

#define DEFAULT_BATCH_SIZE 1
#define DEFAULT_BATCHED_PUSH_TIMEOUT -1
#define DEFAULT_WIDTH 0
#define DEFAULT_HEIGHT 0
#define DEFAULT_ENABLE_PADDING FALSE
#define DEFAULT_GPU_ID 0
#define DEFAULT_MEMORY_TYPE 0
#define DEFAULT_BUFFER_POOL_SIZE 4
#define DEFAULT_COMPUTE_HW 0
#define DEFAULT_INTERPOLATION_METHOD RdsBufSurfTransformInter_Default
#define DEFAULT_NUM_SURFACES_PER_FRAME 1
#define DEFAULT_SORT_BATCH FALSE
#define DEFAULT_ATTACH_SYS_TS TRUE

#define MAX_BUFFER_POOL_SIZE 1024
#define COMPUTE_HW_VIC 2

enum
{
  PROP_0,
  PROP_BATCH_SIZE,
  PROP_BATCHED_PUSH_TIMEOUT,
  PROP_WIDTH,
  PROP_HEIGHT,
  PROP_ENABLE_PADDING,
  PROP_GPU_ID,
  PROP_NVBUF_MEMORY_TYPE,
  PROP_BUFFER_POOL_SIZE,
  PROP_COMPUTE_HW,
  PROP_INTERPOLATION_METHOD,
  PROP_NUM_SURFACES_PER_FRAME,
};

struct _GstRdsStreamMuxPad
{
  GstAggregatorPad parent;

  guint index;                  /* the N of sink_N: source_id and pad_index in the frame meta */

  /* streaming state (aggregator source thread, which also handles the serialized events) */
  GstVideoInfo info;
  gboolean have_info;
  gboolean is_rds;              /* memory:RDS input; otherwise system memory, uploaded through stage */
  RdsBufSurface *stage;         /* device copy of system-memory frames (created on first use) */
  gint frame_num;
};

struct _GstRdsStreamMux
{
  GstAggregator parent;

  /* properties (object lock) */
  guint batch_size;
  gint batched_push_timeout;    /* µs, -1 = none */
  guint width, height;
  gboolean enable_padding;
  guint gpu_id;
  gint memory_type;             /* RdsBufSurfaceMemType 0..3 */
  guint buffer_pool_size;
  gint compute_hw;              /* RdsBufSurfTransform_Compute */
  gint interpolation_method;    /* RdsBufSurfTransform_Inter */
  guint num_surfaces_per_frame;
  gboolean sort_batch;
  gboolean attach_sys_ts;

  /* streaming state (source thread) */
  hipStream_t stream;           /* created on the first batch, so negotiation errors come before any GPU work */
  guint stream_gpu;
  gboolean timeout_enabled;     /* fixed at start */
  GstClockTime batch_start;     /* running time the pending batch's first buffer was seen, or NONE */
  guint rr_next;                /* position (in pad-index order) of the pad visited first in the next batch */
  GstVideoInfo out_info;
  gboolean have_out_info;
  gint fps_n, fps_d;            /* the framerate of the negotiated output (object lock) */
};

#define RDS_STREAM_MUX_FORMATS "{ NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR, GRAY8 }"
#define RDS_STREAM_MUX_FIELDS \
  "format = (string) " RDS_STREAM_MUX_FORMATS ", width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], " \
  "framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink_%u", GST_PAD_SINK, GST_PAD_REQUEST,
    GST_STATIC_CAPS ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), " RDS_STREAM_MUX_FIELDS "; video/x-raw, "
        RDS_STREAM_MUX_FIELDS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), " RDS_STREAM_MUX_FIELDS));

/* Enum types of our own: rdsvideoconvert's live in another plugin, and both may be loaded in one process. The
 * integers are DeepStream's (configs use them); the nicks and descriptions are ours. */
#define GST_TYPE_RDS_STREAM_MUX_MEMORY_TYPE (gst_rds_stream_mux_memory_type_get_type ())
static GType
gst_rds_stream_mux_memory_type_get_type (void)
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
    GType t = g_enum_register_static ("GstRdsStreamMuxMemoryType", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

#define GST_TYPE_RDS_STREAM_MUX_INTERPOLATION (gst_rds_stream_mux_interpolation_get_type ())
static GType
gst_rds_stream_mux_interpolation_get_type (void)
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
    GType t = g_enum_register_static ("GstRdsStreamMuxInterpolationMethod", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

#define GST_TYPE_RDS_STREAM_MUX_COMPUTE_HW (gst_rds_stream_mux_compute_hw_get_type ())
static GType
gst_rds_stream_mux_compute_hw_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {0, "Platform default (the GPU)", "default"},
    {1, "GPU", "gpu"},
    {COMPUTE_HW_VIC, "NVIDIA Jetson VIC engine (not available: fails at start)", "vic"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsStreamMuxComputeHW", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

/* ---- pad ---- */

G_DEFINE_TYPE (GstRdsStreamMuxPad, gst_rds_stream_mux_pad, GST_TYPE_AGGREGATOR_PAD);

static void
pad_free_stage (GstRdsStreamMuxPad * pad)
{
  if (pad->stage != NULL)
    rds_buf_surface_destroy (pad->stage);
  pad->stage = NULL;
}

static void
gst_rds_stream_mux_pad_finalize (GObject * object)
{
  pad_free_stage (GST_RDS_STREAM_MUX_PAD (object));
  G_OBJECT_CLASS (gst_rds_stream_mux_pad_parent_class)->finalize (object);
}

static void
gst_rds_stream_mux_pad_class_init (GstRdsStreamMuxPadClass * klass)
{
  G_OBJECT_CLASS (klass)->finalize = gst_rds_stream_mux_pad_finalize;
}

static void
gst_rds_stream_mux_pad_init (GstRdsStreamMuxPad * pad)
{
  gst_video_info_init (&pad->info);
}

/* ---- element ---- */

G_DEFINE_TYPE (GstRdsStreamMux, gst_rds_stream_mux, GST_TYPE_AGGREGATOR);
GST_ELEMENT_REGISTER_DEFINE (rdsstreammux, "rdsstreammux", GST_RANK_NONE, GST_TYPE_RDS_STREAM_MUX);

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

static GstAggregatorPad *
gst_rds_stream_mux_create_new_pad (GstAggregator * agg, GstPadTemplate * templ, const gchar * req_name,
    const GstCaps * caps)
{
  GstAggregatorPad *pad = GST_AGGREGATOR_CLASS (gst_rds_stream_mux_parent_class)->create_new_pad (agg, templ,
      req_name, caps);

  if (pad != NULL) {
    const gchar *name = GST_PAD_NAME (pad);

    GST_RDS_STREAM_MUX_PAD (pad)->index = (guint) g_ascii_strtoull (name + strlen ("sink_"), NULL, 10);
  }
  return pad;
}

/* ---- caps ---- */

/* The output framerate: the inputs' when they all agree, else 0/1 (also before any input has caps). */
static void
inputs_framerate (GstRdsStreamMux * self, gint * fps_n, gint * fps_d)
{
  gboolean first = TRUE;

  *fps_n = 0;
  *fps_d = 1;
  GST_OBJECT_LOCK (self);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL; l = l->next) {
    GstRdsStreamMuxPad *pad = l->data;

    if (!pad->have_info)
      continue;
    if (first) {
      *fps_n = GST_VIDEO_INFO_FPS_N (&pad->info);
      *fps_d = GST_VIDEO_INFO_FPS_D (&pad->info);
      first = FALSE;
    } else if (gst_util_fraction_compare (*fps_n, *fps_d, GST_VIDEO_INFO_FPS_N (&pad->info),
            GST_VIDEO_INFO_FPS_D (&pad->info)) != 0) {
      *fps_n = 0;
      *fps_d = 1;
      break;
    }
  }
  GST_OBJECT_UNLOCK (self);
}

static GstFlowReturn
gst_rds_stream_mux_update_src_caps (GstAggregator * agg, GstCaps * caps, GstCaps ** ret)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GstCaps *ours;
  guint width, height;
  gint fps_n, fps_d;

  GST_OBJECT_LOCK (self);
  width = self->width;
  height = self->height;
  GST_OBJECT_UNLOCK (self);

  if (width == 0 || height == 0) {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION,
        ("rdsstreammux needs the width and height properties (the output batch size)"),
        ("width %u, height %u: the scaling mux scales every frame to width x height", width, height));
    return GST_FLOW_NOT_NEGOTIATED;
  }

  inputs_framerate (self, &fps_n, &fps_d);
  /* NV12 first: the format downstream gets unless it asks for another one */
  ours = gst_caps_from_string ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), format = (string) "
      RDS_STREAM_MUX_FORMATS ", pixel-aspect-ratio = (fraction) 1/1");
  gst_caps_set_simple (ours, "width", G_TYPE_INT, (gint) width, "height", G_TYPE_INT, (gint) height, "framerate",
      GST_TYPE_FRACTION, fps_n, fps_d, NULL);
  *ret = gst_caps_intersect_full (ours, caps, GST_CAPS_INTERSECT_FIRST);
  gst_caps_unref (ours);
  GST_DEBUG_OBJECT (self, "downstream %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT, caps, *ret);
  if (gst_caps_is_empty (*ret)) {
    gst_clear_caps (ret);
    return GST_FLOW_NOT_NEGOTIATED;
  }
  return GST_FLOW_OK;
}

static gboolean
gst_rds_stream_mux_negotiated_src_caps (GstAggregator * agg, GstCaps * caps)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GstVideoInfo info;

  if (!gst_video_info_from_caps (&info, caps)
      || rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&info)) == RDS_BUF_COLOR_FORMAT_INVALID)
    return FALSE;
  self->out_info = info;
  self->have_out_info = TRUE;
  GST_OBJECT_LOCK (self);
  self->fps_n = GST_VIDEO_INFO_FPS_N (&info);
  self->fps_d = GST_VIDEO_INFO_FPS_D (&info);
  GST_OBJECT_UNLOCK (self);
  return TRUE;
}

/* ---- allocation ---- */

static gboolean
gst_rds_stream_mux_decide_allocation (GstAggregator * agg, GstQuery * query)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GstBufferPool *pool;
  RdsBufSurfaceMemType mem_type;
  guint batch_size, gpu_id, pool_size;

  GST_OBJECT_LOCK (self);
  batch_size = self->batch_size;
  mem_type = (RdsBufSurfaceMemType) self->memory_type;
  gpu_id = self->gpu_id;
  pool_size = self->buffer_pool_size;
  GST_OBJECT_UNLOCK (self);

  /* minimum = maximum: a stalled downstream back-pressures the mux instead of growing the pool */
  pool = rds_decide_allocation (query, batch_size, mem_type, gpu_id, pool_size, pool_size);
  if (pool == NULL) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("Cannot set up the output batch pool"),
        ("batch-size %u, nvbuf-memory-type %d, gpu-id %u, buffer-pool-size %u", batch_size, mem_type, gpu_id,
            pool_size));
    return FALSE;
  }
  gst_object_unref (pool);
  return TRUE;
}

static gboolean
gst_rds_stream_mux_propose_allocation (GstAggregator * agg, G_GNUC_UNUSED GstAggregatorPad * pad,
    G_GNUC_UNUSED GstQuery * decide_query, GstQuery * query)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GstCaps *caps = NULL;
  guint gpu_id;

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps == NULL)
    return FALSE;
  if (rds_caps_has_memory_rds (caps)) {
    GST_OBJECT_LOCK (self);
    gpu_id = self->gpu_id;
    GST_OBJECT_UNLOCK (self);
    return rds_propose_allocation (query, 1, RDS_BUF_MEM_DEFAULT, gpu_id, 2);
  }
  /* system memory: frames are mapped with gst_video_frame_map, which honours GstVideoMeta */
  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
  return TRUE;
}

/* ---- events ---- */

static void
set_pool_flushing (GstRdsStreamMux * self, gboolean flushing)
{
  GstBufferPool *pool = gst_aggregator_get_buffer_pool (GST_AGGREGATOR (self));

  if (pool == NULL)
    return;
  if (gst_buffer_pool_is_active (pool))
    gst_buffer_pool_set_flushing (pool, flushing);
  gst_object_unref (pool);
}

static gboolean
gst_rds_stream_mux_sink_event (GstAggregator * agg, GstAggregatorPad * aggpad, GstEvent * event)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GstRdsStreamMuxPad *pad = GST_RDS_STREAM_MUX_PAD (aggpad);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:{
      GstCaps *caps;
      GstVideoInfo info;
      gint fps_n, fps_d, cur_n, cur_d;

      gst_event_parse_caps (event, &caps);
      if (!gst_video_info_from_caps (&info, caps)
          || rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&info))
          == RDS_BUF_COLOR_FORMAT_INVALID) {
        GST_ERROR_OBJECT (pad, "unsupported caps %" GST_PTR_FORMAT, caps);
        gst_event_unref (event);
        return FALSE;
      }
      /* the staging surface has the old geometry: recreated on the next frame that needs it */
      if (pad->stage != NULL && !gst_video_info_is_equal (&info, &pad->info))
        pad_free_stage (pad);
      GST_OBJECT_LOCK (self);
      pad->info = info;
      pad->have_info = TRUE;
      pad->is_rds = rds_caps_has_memory_rds (caps);
      cur_n = self->fps_n;
      cur_d = self->fps_d;
      GST_OBJECT_UNLOCK (self);
      GST_DEBUG_OBJECT (pad, "caps %" GST_PTR_FORMAT, caps);

      /* only the output framerate depends on the inputs */
      inputs_framerate (self, &fps_n, &fps_d);
      if (!self->have_out_info || gst_util_fraction_compare (fps_n, fps_d, cur_n, cur_d) != 0)
        gst_pad_mark_reconfigure (GST_AGGREGATOR_SRC_PAD (agg));
      break;
    }
    case GST_EVENT_FLUSH_START:
      /* a batch waiting for a pool buffer must give up */
      set_pool_flushing (self, TRUE);
      break;
    default:
      break;
  }
  return GST_AGGREGATOR_CLASS (gst_rds_stream_mux_parent_class)->sink_event (agg, aggpad, event);
}

static GstFlowReturn
gst_rds_stream_mux_flush (GstAggregator * agg)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);

  set_pool_flushing (self, FALSE);
  self->batch_start = GST_CLOCK_TIME_NONE;
  return GST_FLOW_OK;
}

/* ---- timing ---- */

/* With a timeout, the batch is due one aggregator latency (the timeout) after its first buffer was seen. Called
 * by the aggregator on the source thread before it waits; NONE (no buffer yet, or no timeout) waits for data. */
static GstClockTime
gst_rds_stream_mux_get_next_time (GstAggregator * agg)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  gboolean any = FALSE;

  if (!self->timeout_enabled)
    return GST_CLOCK_TIME_NONE;
  if (GST_CLOCK_TIME_IS_VALID (self->batch_start))
    return self->batch_start;

  GST_OBJECT_LOCK (self);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL && !any; l = l->next)
    any = gst_aggregator_pad_has_buffer (GST_AGGREGATOR_PAD (l->data));
  GST_OBJECT_UNLOCK (self);
  if (any)
    self->batch_start = gst_element_get_current_running_time (GST_ELEMENT (self));
  return self->batch_start;
}

/* ---- start / stop ---- */

/* Settings we cannot honour: checked at start, before any GPU work. */
static gboolean
check_settings (GstRdsStreamMux * self)
{
  gint compute_hw;
  guint num_surfaces;

  GST_OBJECT_LOCK (self);
  compute_hw = self->compute_hw;
  num_surfaces = self->num_surfaces_per_frame;
  GST_OBJECT_UNLOCK (self);

  if (compute_hw == COMPUTE_HW_VIC) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("compute-hw=VIC is not available"),
        ("only the GPU (compute-hw 0 or 1) is supported"));
    return FALSE;
  }
  if (num_surfaces != 1) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("num-surfaces-per-frame %u is not supported", num_surfaces),
        ("only one surface per frame is supported"));
    return FALSE;
  }
  return TRUE;
}

static gboolean
gst_rds_stream_mux_start (GstAggregator * agg)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  gint timeout;

  if (!check_settings (self))
    return FALSE;

  GST_OBJECT_LOCK (self);
  timeout = self->batched_push_timeout;
  GST_OBJECT_UNLOCK (self);

  self->timeout_enabled = timeout >= 0;
  self->batch_start = GST_CLOCK_TIME_NONE;
  self->rr_next = 0;
  self->have_out_info = FALSE;
  gst_aggregator_set_force_live (agg, self->timeout_enabled);
  if (self->timeout_enabled)
    g_object_set (self, "latency", (guint64) timeout * GST_USECOND, NULL);
  return TRUE;
}

static gboolean
gst_rds_stream_mux_stop (GstAggregator * agg)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);

  GST_OBJECT_LOCK (self);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL; l = l->next) {
    GstRdsStreamMuxPad *pad = l->data;

    pad_free_stage (pad);
    pad->frame_num = 0;
  }
  self->fps_n = 0;
  self->fps_d = 1;
  GST_OBJECT_UNLOCK (self);

  if (self->stream != NULL) {
    if (hipStreamDestroy (self->stream) != hipSuccess)
      GST_WARNING_OBJECT (self, "hipStreamDestroy failed");
    self->stream = NULL;
  }
  self->have_out_info = FALSE;
  return TRUE;
}

static GstStateChangeReturn
gst_rds_stream_mux_change_state (GstElement * element, GstStateChange transition)
{
  /* the source task may be waiting for a pool buffer held downstream: wake it before the aggregator stops it */
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
    set_pool_flushing (GST_RDS_STREAM_MUX (element), TRUE);
  return GST_ELEMENT_CLASS (gst_rds_stream_mux_parent_class)->change_state (element, transition);
}

/* ---- batching ---- */

static gboolean
ensure_stream (GstRdsStreamMux * self)
{
  int prev = 0;
  guint gpu_id;
  hipError_t err;

  if (self->stream != NULL)
    return TRUE;
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
ensure_stage (GstRdsStreamMux * self, GstRdsStreamMuxPad * pad)
{
  RdsBufSurfaceCreateParams cp = {
    .gpuId = self->stream_gpu,
    .width = GST_VIDEO_INFO_WIDTH (&pad->info),
    .height = GST_VIDEO_INFO_HEIGHT (&pad->info),
    .colorFormat = rds_buf_color_format_from_video_format (GST_VIDEO_INFO_FORMAT (&pad->info)),
    .layout = RDS_BUF_LAYOUT_PITCH,
    .memType = RDS_BUF_MEM_DEFAULT,
  };

  if (pad->stage != NULL)
    return TRUE;
  if (rds_buf_surface_create (&pad->stage, 1, &cp) != 0) {
    pad->stage = NULL;
    return FALSE;
  }
  return TRUE;
}

/* Black in slot @slot: Y 16, U/V 128 for YUV; every byte 0 for GRAY8 and RGB formats (as rdsvideoconvert). */
static gboolean
fill_slot_black (RdsBufSurface * batch, guint slot)
{
  RdsBufSurfaceParams *p = &batch->surfaceList[slot];
  gboolean yuv = p->colorFormat == RDS_BUF_COLOR_FORMAT_NV12 || p->colorFormat == RDS_BUF_COLOR_FORMAT_YUV420;

  for (guint plane = 0; plane < p->planeParams.num_planes; plane++) {
    if (rds_buf_surface_memset (batch, (int) slot, (int) plane, yuv ? (plane == 0 ? 16 : 128) : 0) != 0)
      return FALSE;
  }
  return TRUE;
}

/* enable-padding: the largest rect with the input's aspect ratio that fits the slot, at its top-left corner;
 * even-sized for 4:2:0 output. */
static RdsBufSurfTransformRect
padded_rect (guint in_w, guint in_h, guint out_w, guint out_h, gboolean even)
{
  RdsBufSurfTransformRect r = { 0, 0, out_w, out_h };

  if ((guint64) out_w * in_h <= (guint64) out_h * in_w)
    r.height = (guint32) (((guint64) in_h * out_w + in_w / 2) / in_w);
  else
    r.width = (guint32) (((guint64) in_w * out_h + in_h / 2) / in_h);
  if (even) {
    r.width = MAX (r.width & ~1u, MIN (2u, out_w));
    r.height = MAX (r.height & ~1u, MIN (2u, out_h));
  }
  r.width = CLAMP (r.width, 1, out_w);
  r.height = CLAMP (r.height, 1, out_h);
  return r;
}

typedef struct
{
  GstRdsStreamMuxPad *pad;
  GstBuffer *buf;
} MuxEntry;

static gint
compare_entries (gconstpointer a, gconstpointer b)
{
  const MuxEntry *ea = a, *eb = b;

  return ea->pad->index < eb->pad->index ? -1 : ea->pad->index > eb->pad->index;
}

static gint
compare_pads (gconstpointer a, gconstpointer b)
{
  const GstRdsStreamMuxPad *pa = a, *pb = b;

  return pa->index < pb->index ? -1 : pa->index > pb->index;
}

/* Scales @e's frame into slot @slot of @batch through a single-slot view of the batch. */
static gboolean
fill_slot (GstRdsStreamMux * self, MuxEntry * e, RdsBufSurface * batch, guint slot, gint filter,
    RdsBufSurfTransform_Compute compute, gboolean padding)
{
  GstRdsStreamMuxPad *pad = e->pad;
  RdsBufSurfTransformConfigParams session = {
    .compute_mode = compute,
    .gpu_id = (gint32) self->stream_gpu,
    .cuda_stream = self->stream,
  };
  RdsBufSurfTransformConfigParams prev_session;
  RdsBufSurfTransformParams params = { 0 };
  RdsBufSurfTransformRect dst_rect;
  RdsBufSurfTransform_Error err;
  RdsBufSurface view, *src;
  GstMapInfo map;
  gboolean mapped = FALSE, ok = FALSE;
  guint out_w = GST_VIDEO_INFO_WIDTH (&self->out_info), out_h = GST_VIDEO_INFO_HEIGHT (&self->out_info);

  if (!pad->have_info) {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("A frame arrived on %s before its caps", GST_PAD_NAME (pad)),
        (NULL));
    return FALSE;
  }

  /* Source: the memory:RDS surface itself, or the system frame uploaded into the pad's staging surface. */
  if (pad->is_rds) {
    if (rds_buffer_get_surface (e->buf) == NULL || !gst_buffer_map (e->buf, &map, GST_MAP_READ)) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map a memory:RDS input buffer on %s", GST_PAD_NAME (pad)),
          (NULL));
      return FALSE;
    }
    mapped = TRUE;
    src = (RdsBufSurface *) map.data;
    if (src->batchSize != 1 || src->numFilled != 1) {
      GST_ELEMENT_ERROR (self, STREAM, NOT_IMPLEMENTED, ("Batched input is not supported"),
          ("%s: batch size %u, %u filled; only single frames are supported", GST_PAD_NAME (pad), src->batchSize,
              src->numFilled));
      goto done;
    }
  } else {
    GstVideoFrame frame;
    gboolean copied;

    if (!ensure_stage (self, pad)) {
      GST_ELEMENT_ERROR (self, RESOURCE, NO_SPACE_LEFT, ("Cannot allocate a staging surface on GPU %u",
              self->stream_gpu), (NULL));
      return FALSE;
    }
    if (!gst_video_frame_map (&frame, &pad->info, e->buf, GST_MAP_READ)) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map a system-memory input frame on %s",
              GST_PAD_NAME (pad)), (NULL));
      return FALSE;
    }
    copied = rds_video_frame_copy_surface (&frame, pad->stage, TRUE);
    gst_video_frame_unmap (&frame);
    if (!copied) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot upload a frame to the GPU"), (NULL));
      return FALSE;
    }
    src = pad->stage;
  }

  /* Destination: a hand-built single-surface view of the slot (no library ownership: _priv NULL). */
  view = *batch;
  view.batchSize = 1;
  view.numFilled = 1;
  view.surfaceList = &batch->surfaceList[slot];
  view._priv = NULL;

  params.transform_flag = RDS_BUFSURF_TRANSFORM_FILTER;
  params.transform_filter = (RdsBufSurfTransform_Inter) filter;
  if (padding) {
    gboolean even = GST_VIDEO_INFO_FORMAT (&self->out_info) == GST_VIDEO_FORMAT_NV12
        || GST_VIDEO_INFO_FORMAT (&self->out_info) == GST_VIDEO_FORMAT_I420;

    dst_rect = padded_rect (src->surfaceList[0].width, src->surfaceList[0].height, out_w, out_h, even);
    if (dst_rect.width != out_w || dst_rect.height != out_h) {
      /* the transform leaves pixels outside the rect alone: black the slot first (synchronous memset) */
      if (!fill_slot_black (batch, slot)) {
        GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot clear a batch slot"), (NULL));
        goto done;
      }
      params.transform_flag |= RDS_BUFSURF_TRANSFORM_CROP_DST;
      params.dst_rect = &dst_rect;
    }
  }

  /* Session parameters are per thread and shared by everything on this thread: set ours before the call and
   * restore the previous one right after (our stream dies in stop ()). A failed set leaves it unchanged. */
  (void) rds_buf_surf_transform_get_session_params (&prev_session);
  err = rds_buf_surf_transform_set_session_params (&session);
  if (err == RdsBufSurfTransformError_Success) {
    err = rds_buf_surf_transform (src, &view, &params);
    if (rds_buf_surf_transform_set_session_params (&prev_session) != RdsBufSurfTransformError_Success)
      GST_WARNING_OBJECT (self, "cannot restore the thread's previous transform session");
  }
  if (err != RdsBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Scaling a frame into the batch failed"),
        ("%s: rds_buf_surf_transform: %s (%d)", GST_PAD_NAME (pad), transform_error_name (err), err));
    goto done;
  }
  /* The transform is synchronous: the slot is ready when the batch is pushed. */
  ok = TRUE;

done:
  if (mapped)
    gst_buffer_unmap (e->buf, &map);
  return ok;
}

static void
fill_frame_meta (GstRdsStreamMux * self, RdsBatchMeta * bm, MuxEntry * e, guint slot, gboolean attach_sys_ts)
{
  RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

  fm->source_id = e->pad->index;
  fm->pad_index = e->pad->index;
  fm->batch_id = slot;
  fm->frame_num = e->pad->frame_num++;
  fm->buf_pts = GST_BUFFER_PTS (e->buf);
  fm->ntp_timestamp = attach_sys_ts ? (guint64) g_get_real_time () * 1000 : 0;
  fm->source_frame_width = GST_VIDEO_INFO_WIDTH (&e->pad->info);
  fm->source_frame_height = GST_VIDEO_INFO_HEIGHT (&e->pad->info);
  fm->pipeline_width = GST_VIDEO_INFO_WIDTH (&self->out_info);
  fm->pipeline_height = GST_VIDEO_INFO_HEIGHT (&self->out_info);
  fm->num_surfaces_per_frame = 1;
  fm->surface_index = 0;
  rds_add_frame_meta_to_batch (bm, fm);
}

static GstFlowReturn
gst_rds_stream_mux_aggregate (GstAggregator * agg, gboolean timeout)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GPtrArray *pads;
  GArray *entries;
  GstBufferPool *pool = NULL;
  GstBuffer *outbuf = NULL;
  GstMapInfo map;
  RdsBufSurface *batch;
  RdsBatchMeta *bm;
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  guint batch_size, n;
  gint filter;
  RdsBufSurfTransform_Compute compute;
  gboolean padding, sort_batch, attach_sys_ts, all_eos = TRUE;
  GstFlowReturn ret = GST_FLOW_ERROR;

  GST_OBJECT_LOCK (self);
  batch_size = self->batch_size;
  filter = self->interpolation_method;
  compute = (RdsBufSurfTransform_Compute) self->compute_hw;
  padding = self->enable_padding;
  sort_batch = self->sort_batch;
  attach_sys_ts = self->attach_sys_ts;
  pads = g_ptr_array_new_with_free_func (gst_object_unref);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL; l = l->next)
    g_ptr_array_add (pads, gst_object_ref (l->data));
  GST_OBJECT_UNLOCK (self);
  g_ptr_array_sort_values (pads, compare_pads);
  self->batch_start = GST_CLOCK_TIME_NONE;

  /* Collect: at most one buffer per pad, round robin from rr_next, up to batch-size frames. */
  entries = g_array_new (FALSE, FALSE, sizeof (MuxEntry));
  n = pads->len;
  for (guint k = 0; k < n && entries->len < batch_size; k++) {
    GstRdsStreamMuxPad *pad = g_ptr_array_index (pads, (self->rr_next + k) % n);
    MuxEntry e = { pad, gst_aggregator_pad_pop_buffer (GST_AGGREGATOR_PAD (pad)) };

    if (e.buf != NULL)
      g_array_append_val (entries, e);
  }
  if (n > 0)
    self->rr_next = (self->rr_next + 1) % n;

  if (entries->len == 0) {
    for (guint i = 0; i < n; i++)
      all_eos &= gst_aggregator_pad_is_eos (g_ptr_array_index (pads, i));
    ret = n > 0 && all_eos ? GST_FLOW_EOS : GST_AGGREGATOR_FLOW_NEED_DATA;
    goto out;
  }
  if (sort_batch)
    g_array_sort (entries, compare_entries);
  GST_LOG_OBJECT (self, "batch of %u (timeout %d)", entries->len, timeout);

  if (!self->have_out_info) {
    ret = GST_FLOW_NOT_NEGOTIATED;
    goto out;
  }
  if (!ensure_stream (self))
    goto out;

  pool = gst_aggregator_get_buffer_pool (agg);
  if (pool == NULL) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("No output batch pool"), (NULL));
    goto out;
  }
  if (!gst_buffer_pool_is_active (pool) && !gst_buffer_pool_set_active (pool, TRUE)) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot activate the output batch pool"), (NULL));
    goto out;
  }
  /* blocks while every pool buffer is downstream (bounded pool) */
  ret = gst_buffer_pool_acquire_buffer (pool, &outbuf, NULL);
  if (ret != GST_FLOW_OK)
    goto out;
  ret = GST_FLOW_ERROR;
  /* a pool buffer is ours alone: the WRITE map does not copy */
  if (rds_buffer_get_surface (outbuf) == NULL || !gst_buffer_map (outbuf, &map, GST_MAP_WRITE)) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map an output batch"), (NULL));
    goto out;
  }
  batch = (RdsBufSurface *) map.data;
  if (batch->batchSize < entries->len) {
    gst_buffer_unmap (outbuf, &map);
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("The output batch is too small"),
        ("batch size %u, %u frames", batch->batchSize, entries->len));
    goto out;
  }

  bm = rds_create_batch_meta (batch_size);
  for (guint i = 0; i < entries->len; i++) {
    MuxEntry *e = &g_array_index (entries, MuxEntry, i);
    GstClockTime rt;

    if (!fill_slot (self, e, batch, i, filter, compute, padding)) {
      gst_buffer_unmap (outbuf, &map);
      rds_destroy_batch_meta (bm);
      goto out;
    }
    fill_frame_meta (self, bm, e, i, attach_sys_ts);
    GST_OBJECT_LOCK (e->pad);
    rt = gst_segment_to_running_time (&GST_AGGREGATOR_PAD (e->pad)->segment, GST_FORMAT_TIME,
        GST_BUFFER_PTS (e->buf));
    GST_OBJECT_UNLOCK (e->pad);
    if (GST_CLOCK_TIME_IS_VALID (rt) && (!GST_CLOCK_TIME_IS_VALID (pts) || rt < pts))
      pts = rt;
  }
  batch->numFilled = entries->len;
  gst_buffer_unmap (outbuf, &map);
  rds_buffer_add_batch_meta (outbuf, bm);

  /* the output segment is TIME from 0: running time is the output timestamp */
  GST_BUFFER_PTS (outbuf) = pts;
  GST_BUFFER_DTS (outbuf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (outbuf) = GST_CLOCK_TIME_NONE;
  if (GST_CLOCK_TIME_IS_VALID (pts)) {
    GST_OBJECT_LOCK (self);
    GST_AGGREGATOR_PAD (agg->srcpad)->segment.position = pts;
    GST_OBJECT_UNLOCK (self);
  }
  gst_aggregator_selected_samples (agg, pts, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, NULL);
  ret = gst_aggregator_finish_buffer (agg, outbuf);
  outbuf = NULL;

out:
  if (outbuf != NULL)
    gst_buffer_unref (outbuf);
  if (pool != NULL)
    gst_object_unref (pool);
  for (guint i = 0; i < entries->len; i++)
    gst_buffer_unref (g_array_index (entries, MuxEntry, i).buf);
  g_array_unref (entries);
  g_ptr_array_unref (pads);
  return ret;
}

/* ---- properties ---- */

/* Allocation and timing settings are read when streaming starts: refuse changes once streaming, including during
 * the READY -> PAUSED transition (object lock held, which also guards the state fields). */
static gboolean
check_mutable (GstRdsStreamMux * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_stream_mux_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_BATCH_SIZE:
      if (check_mutable (self, pspec))
        self->batch_size = g_value_get_uint (value);
      break;
    case PROP_BATCHED_PUSH_TIMEOUT:
      if (check_mutable (self, pspec))
        self->batched_push_timeout = g_value_get_int (value);
      break;
    case PROP_WIDTH:
      if (check_mutable (self, pspec))
        self->width = g_value_get_uint (value);
      break;
    case PROP_HEIGHT:
      if (check_mutable (self, pspec))
        self->height = g_value_get_uint (value);
      break;
    case PROP_ENABLE_PADDING:
      self->enable_padding = g_value_get_boolean (value);
      break;
    case PROP_GPU_ID:
      if (check_mutable (self, pspec))
        self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      if (check_mutable (self, pspec))
        self->memory_type = g_value_get_enum (value);
      break;
    case PROP_BUFFER_POOL_SIZE:
      if (check_mutable (self, pspec))
        self->buffer_pool_size = g_value_get_uint (value);
      break;
    case PROP_COMPUTE_HW:
      if (check_mutable (self, pspec))
        self->compute_hw = g_value_get_enum (value);
      break;
    case PROP_INTERPOLATION_METHOD:
      self->interpolation_method = g_value_get_enum (value);
      break;
    case PROP_NUM_SURFACES_PER_FRAME:
      if (check_mutable (self, pspec))
        self->num_surfaces_per_frame = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_stream_mux_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_BATCH_SIZE:
      g_value_set_uint (value, self->batch_size);
      break;
    case PROP_BATCHED_PUSH_TIMEOUT:
      g_value_set_int (value, self->batched_push_timeout);
      break;
    case PROP_WIDTH:
      g_value_set_uint (value, self->width);
      break;
    case PROP_HEIGHT:
      g_value_set_uint (value, self->height);
      break;
    case PROP_ENABLE_PADDING:
      g_value_set_boolean (value, self->enable_padding);
      break;
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      g_value_set_enum (value, self->memory_type);
      break;
    case PROP_BUFFER_POOL_SIZE:
      g_value_set_uint (value, self->buffer_pool_size);
      break;
    case PROP_COMPUTE_HW:
      g_value_set_enum (value, self->compute_hw);
      break;
    case PROP_INTERPOLATION_METHOD:
      g_value_set_enum (value, self->interpolation_method);
      break;
    case PROP_NUM_SURFACES_PER_FRAME:
      g_value_set_uint (value, self->num_surfaces_per_frame);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_stream_mux_class_init (GstRdsStreamMuxClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAggregatorClass *agg_class = GST_AGGREGATOR_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  GST_DEBUG_CATEGORY_INIT (gst_rds_stream_mux_debug, "rdsstreammux", 0, "Redstream stream batcher");

  gobject_class->set_property = gst_rds_stream_mux_set_property;
  gobject_class->get_property = gst_rds_stream_mux_get_property;

  g_object_class_install_property (gobject_class, PROP_BATCH_SIZE,
      g_param_spec_uint ("batch-size", "Batch size", "Maximum number of frames in a batch", 1, G_MAXUINT,
          DEFAULT_BATCH_SIZE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_BATCHED_PUSH_TIMEOUT,
      g_param_spec_int ("batched-push-timeout", "Batched push timeout",
          "Microseconds to wait after the first frame of a batch before pushing it incomplete; -1 waits for every "
          "active source", -1, G_MAXINT, DEFAULT_BATCHED_PUSH_TIMEOUT, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_WIDTH,
      g_param_spec_uint ("width", "Width", "Width of every frame in the batch (required)", 0,
          RDS_BUF_SURFACE_MAX_DIM, DEFAULT_WIDTH, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_HEIGHT,
      g_param_spec_uint ("height", "Height", "Height of every frame in the batch (required)", 0,
          RDS_BUF_SURFACE_MAX_DIM, DEFAULT_HEIGHT, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_ENABLE_PADDING,
      g_param_spec_boolean ("enable-padding", "Enable padding",
          "Keep the aspect ratio: scale to fit, place the frame at the top-left of its slot, fill the rest black",
          DEFAULT_ENABLE_PADDING, flags));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU id", "GPU used for scaling and the output batches", 0, G_MAXUINT,
          DEFAULT_GPU_ID, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_NVBUF_MEMORY_TYPE,
      g_param_spec_enum ("nvbuf-memory-type", "Memory type", "Memory type of the output batches",
          GST_TYPE_RDS_STREAM_MUX_MEMORY_TYPE, DEFAULT_MEMORY_TYPE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_BUFFER_POOL_SIZE,
      g_param_spec_uint ("buffer-pool-size", "Buffer pool size", "Number of batches in the output pool", 1,
          MAX_BUFFER_POOL_SIZE, DEFAULT_BUFFER_POOL_SIZE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_COMPUTE_HW,
      g_param_spec_enum ("compute-hw", "Compute hardware", "Engine that runs the scaling",
          GST_TYPE_RDS_STREAM_MUX_COMPUTE_HW, DEFAULT_COMPUTE_HW, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_INTERPOLATION_METHOD,
      g_param_spec_enum ("interpolation-method", "Interpolation method", "Filter used for scaling",
          GST_TYPE_RDS_STREAM_MUX_INTERPOLATION, DEFAULT_INTERPOLATION_METHOD, flags));
  g_object_class_install_property (gobject_class, PROP_NUM_SURFACES_PER_FRAME,
      g_param_spec_uint ("num-surfaces-per-frame", "Surfaces per frame",
          "Surfaces per frame (only 1 is supported; other values fail at start)", 1, 4,
          DEFAULT_NUM_SURFACES_PER_FRAME, flags | GST_PARAM_MUTABLE_READY));

  gst_element_class_add_static_pad_template_with_gtype (element_class, &sink_template,
      GST_TYPE_RDS_STREAM_MUX_PAD);
  gst_element_class_add_static_pad_template_with_gtype (element_class, &src_template, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_set_static_metadata (element_class, "Redstream stream muxer", "Codec/Muxer/Video",
      "Batches frames from several sources into one memory:RDS batch, scaling each into its slot",
      "Redstream contributors");

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_change_state);
  agg_class->create_new_pad = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_create_new_pad);
  agg_class->update_src_caps = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_update_src_caps);
  agg_class->negotiated_src_caps = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_negotiated_src_caps);
  agg_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_decide_allocation);
  agg_class->propose_allocation = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_propose_allocation);
  agg_class->sink_event = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_sink_event);
  agg_class->flush = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_flush);
  agg_class->get_next_time = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_get_next_time);
  agg_class->start = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_start);
  agg_class->stop = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_stop);
  agg_class->aggregate = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_aggregate);

  gst_type_mark_as_plugin_api (GST_TYPE_RDS_STREAM_MUX_PAD, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_STREAM_MUX_MEMORY_TYPE, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_STREAM_MUX_INTERPOLATION, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_STREAM_MUX_COMPUTE_HW, 0);
}

static void
gst_rds_stream_mux_init (GstRdsStreamMux * self)
{
  self->batch_size = DEFAULT_BATCH_SIZE;
  self->batched_push_timeout = DEFAULT_BATCHED_PUSH_TIMEOUT;
  self->width = DEFAULT_WIDTH;
  self->height = DEFAULT_HEIGHT;
  self->enable_padding = DEFAULT_ENABLE_PADDING;
  self->gpu_id = DEFAULT_GPU_ID;
  self->memory_type = DEFAULT_MEMORY_TYPE;
  self->buffer_pool_size = DEFAULT_BUFFER_POOL_SIZE;
  self->compute_hw = DEFAULT_COMPUTE_HW;
  self->interpolation_method = DEFAULT_INTERPOLATION_METHOD;
  self->num_surfaces_per_frame = DEFAULT_NUM_SURFACES_PER_FRAME;
  self->sort_batch = DEFAULT_SORT_BATCH;
  self->attach_sys_ts = DEFAULT_ATTACH_SYS_TS;
  self->batch_start = GST_CLOCK_TIME_NONE;
  self->fps_n = 0;
  self->fps_d = 1;
  gst_video_info_init (&self->out_info);
}
