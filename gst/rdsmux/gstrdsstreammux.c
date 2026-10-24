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
 *   The upstream latency of a live source is taken off again: the timer counts from the batch's first frame only.
 * - sync-inputs: forced-live as well, but get_next_time is the running time of the earliest queued frame. A partial
 *   batch then goes out once the clock reaches that frame's running time plus the latency (batched-push-timeout, or
 *   0, plus max-latency and the upstream latency). A frame already due goes out at once with whatever else is
 *   queued; one ahead of the clock waits for the others until its time. As in every mode, a batch with a frame from
 *   every source goes out as soon as it is complete: inputs are not held back to the clock.
 *   The timer of a partial batch starts when its first frame arrived (recorded per buffer by a sink pad probe),
 *   also when that frame queued up while the previous batch was still waiting.
 * Pool pressure: a live or forced-live aggregator pad queues at least two buffers plus a latency's worth, on top of
 * the one being batched; upstream pools must allow for that. Downstream, every demuxed view keeps its whole batch
 * alive and sinks keep their last buffer, so each demux branch can pin a different batch: unless buffer-pool-size
 * is set, the output pool holds batch-size + 2 batches (at least 4).
 *
 * Stream events (rds-event.h) go downstream in order with the batches:
 * - pad-added before a source's first event or frame, stream-start / stream-segment from its STREAM_START /
 *   SEGMENT, stream-eos from its EOS, stream-reset before its first event or frame after a flush. The aggregator
 *   queues serialized events with the buffers and hands them to sink_event on the source thread once the buffers
 *   in front of them have been taken, so a source's stream-eos follows the batch with its last frame and precedes
 *   every later batch.
 * - pad-deleted when a pad is released: right away from the releasing thread, or, while a batch is being built
 *   (it may hold the pad's last frame), after that batch.
 * Events wait in a queue until the output caps are negotiated, so they never come before the mux's own
 * stream-start, caps and segment.
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
#define DEFAULT_LIVE_SOURCE FALSE
#define DEFAULT_MAX_LATENCY 0
#define DEFAULT_SYNC_INPUTS FALSE
#define DEFAULT_DROP_PIPELINE_EOS FALSE
#define DEFAULT_FRAME_NUM_RESET_ON_EOS FALSE
#define DEFAULT_FRAME_NUM_RESET_ON_STREAM_RESET FALSE
#define DEFAULT_FRAME_DURATION G_MAXUINT64       /* -1 as documented: none */
#define DEFAULT_CACHE_BUFFER_TIMEOUT -1
#define DEFAULT_SYNC_INPUTS_NTP 0
#define DEFAULT_FAILSAFE_FLUSH_COUNT 10

/* with drop-pipeline-eos and every source ended, aggregate () waits this long at most for something to change */
#define IDLE_WAIT_US (100 * 1000)

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
  PROP_SORT_BATCH,
  PROP_ATTACH_SYS_TS,
  PROP_LIVE_SOURCE,
  PROP_MAX_LATENCY,
  PROP_SYNC_INPUTS,
  PROP_DROP_PIPELINE_EOS,
  PROP_FRAME_NUM_RESET_ON_EOS,
  PROP_FRAME_NUM_RESET_ON_STREAM_RESET,
  /* accepted, no effect */
  PROP_FRAME_DURATION,
  PROP_ALIGN_FIRST_BUFFER,
  PROP_ALIGN_INPUTS,
  PROP_CACHE_BUFFER,
  PROP_CACHE_BUFFER_TIMEOUT,
  PROP_EXTRACT_SEI_SIM_TIME,
  PROP_EXTRACT_SEI_TYPE5_DATA,
  PROP_DROP_BACKWARD_SEI,
  PROP_SYNC_INPUTS_NTP,
  PROP_FAILSAFE_FLUSH_COUNT,
  PROP_ASYNC_PROCESS,
};

typedef struct
{
  GstBuffer *buf;               /* identity only (no ref): matched when the buffer is popped */
  GstClockTime time;            /* running time at arrival, or NONE (no clock yet) */
} Arrival;

/* GstRdsStreamMuxPad.state: whether pad-added went downstream, and whether the pad was released */
enum
{
  PAD_STATE_NEW,
  PAD_STATE_ADDED,
  PAD_STATE_RELEASED,           /* after pad-added: pad-deleted is due */
  PAD_STATE_RELEASED_NEW,       /* never announced: its frames are dropped, no pad-deleted */
};

struct _GstRdsStreamMuxPad
{
  GstAggregatorPad parent;

  guint index;                  /* the N of sink_N: source_id and pad_index in the frame meta */
  guint32 flush_seqnum;         /* seqnum of the last FLUSH_START (object lock of the element). Kept after
                                 * FLUSH_STOP, as GstAggregator keeps its own: in a seek-style flush one pad may
                                 * finish START + STOP before another pad's START, and that START still makes the
                                 * flush element-wide. */

  /* streaming state (aggregator source thread, which also handles the serialized events) */
  GstVideoInfo info;
  gboolean have_info;
  gboolean is_rds;              /* memory:RDS input; otherwise system memory, uploaded through stage */
  RdsBufSurface *stage;         /* device copy of system-memory frames (created on first use) */
  gint frame_num;

  gint state;                   /* PAD_STATE_* (atomic) */

  /* arrival running time of every queued buffer, oldest first (arrivals_lock): the batch timer starts at the
   * arrival of the oldest frame still queued, not when the source thread first notices it */
  GMutex arrivals_lock;
  GQueue arrivals;              /* Arrival *, filled by a buffer probe, taken when the buffer is popped */
  gint reset_pending;           /* flushed since: rds-stream-reset is due before its next event or frame (atomic) */
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
  gboolean buffer_pool_size_set;        /* set by the user: honoured as is */
  gint compute_hw;              /* RdsBufSurfTransform_Compute */
  gint interpolation_method;    /* RdsBufSurfTransform_Inter */
  guint num_surfaces_per_frame;
  gboolean sort_batch;
  gboolean attach_sys_ts;
  gboolean live_source;
  guint max_latency;            /* ns */
  gboolean sync_inputs;
  gboolean drop_pipeline_eos;
  gboolean frame_num_reset_on_eos;
  gboolean frame_num_reset_on_stream_reset;
  /* accepted, no effect (stored so they read back) */
  guint64 frame_duration;
  gboolean align_first_buffer, align_inputs, cache_buffer, extract_sei_sim_time, extract_sei_type5_data;
  gboolean drop_backward_sei, async_process;
  gint cache_buffer_timeout, sync_inputs_ntp;
  guint failsafe_flush_count;
  gboolean noop_logged;

  /* downstream stream events (events_lock) */
  GMutex events_lock;
  GQueue pending;               /* GstEvent *, oldest first: the source thread's */
  GQueue deferred;              /* from other threads while in_flight: they go after the batch or event in hand */
  gboolean in_flight;           /* the source thread is handling a batch or a source's event */
  gboolean can_push;            /* output caps negotiated */
  GRecMutex push_lock;          /* serializes our event pushes across threads (taken before events_lock). Recursive:
                                 * a probe releasing a pad from inside our push re-enters send_event. The source
                                 * thread only holds it while in_flight is set, so another thread holding it is
                                 * never waiting for the source thread (it pushes directly only when !in_flight).
                                 * A downstream element whose serialized-event handler waits on the releasing
                                 * thread could still deadlock against a direct pad-deleted push. */

  /* wakes aggregate () waiting with every source ended under drop-pipeline-eos (wait_lock) */
  GMutex wait_lock;
  GCond wait_cond;
  guint wake_seq;

  /* streaming state (source thread) */
  hipStream_t stream;           /* created on the first batch, so negotiation errors come before any GPU work */
  guint stream_gpu;
  gboolean timeout_enabled;     /* fixed at start */
  gboolean sync_mode;           /* sync-inputs, fixed at start */
  GstClockTime upstream_latency;        /* the live upstream's share of the aggregator latency */
  GstClockTime batch_start;     /* running time the source thread first saw a queued buffer with no arrival time
                                 * since the last batch, or NONE */
  guint rr_next;                /* position (in pad-index order) of the pad visited first in the next batch */
  GstClockTime last_pts;        /* of the last batch pushed: batch timestamps never go backwards */
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

static void wake_idle (GstRdsStreamMux * self);
static void send_event (GstRdsStreamMux * self, GstEvent * event);

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
pad_clear_arrivals (GstRdsStreamMuxPad * pad)
{
  g_mutex_lock (&pad->arrivals_lock);
  g_queue_clear_full (&pad->arrivals, g_free);
  g_mutex_unlock (&pad->arrivals_lock);
}

/* Upstream thread, before the buffer is queued: records its arrival time. */
static GstPadProbeReturn
pad_arrival_probe (GstPad * gpad, GstPadProbeInfo * info, G_GNUC_UNUSED gpointer user_data)
{
  GstRdsStreamMuxPad *pad = GST_RDS_STREAM_MUX_PAD (gpad);
  GstElement *element = GST_PAD_PARENT (gpad);
  Arrival *a = g_new (Arrival, 1);

  a->buf = GST_PAD_PROBE_INFO_BUFFER (info);
  a->time = element != NULL ? gst_element_get_current_running_time (element) : GST_CLOCK_TIME_NONE;
  g_mutex_lock (&pad->arrivals_lock);
  g_queue_push_tail (&pad->arrivals, a);
  g_mutex_unlock (&pad->arrivals_lock);
  return GST_PAD_PROBE_OK;
}

/* Source thread: @buf was popped. Drops its record and any older ones (buffers that left the queue unpopped).
 * A buffer with no record (a GAP buffer the aggregator made from an event) leaves the records alone. */
static void
pad_take_arrival (GstRdsStreamMuxPad * pad, GstBuffer * buf)
{
  gboolean found = FALSE;

  g_mutex_lock (&pad->arrivals_lock);
  for (GList * l = pad->arrivals.head; l != NULL && !found; l = l->next)
    found = ((Arrival *) l->data)->buf == buf;
  while (found) {
    Arrival *a = g_queue_pop_head (&pad->arrivals);

    found = a->buf != buf;
    g_free (a);
  }
  g_mutex_unlock (&pad->arrivals_lock);
}

static void
gst_rds_stream_mux_pad_finalize (GObject * object)
{
  GstRdsStreamMuxPad *pad = GST_RDS_STREAM_MUX_PAD (object);

  pad_free_stage (pad);
  g_queue_clear_full (&pad->arrivals, g_free);
  g_mutex_clear (&pad->arrivals_lock);
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
  g_mutex_init (&pad->arrivals_lock);
  g_queue_init (&pad->arrivals);
  gst_pad_add_probe (GST_PAD (pad), GST_PAD_PROBE_TYPE_BUFFER, pad_arrival_probe, NULL, NULL);
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

static GstPad *
gst_rds_stream_mux_request_new_pad (GstElement * element, GstPadTemplate * templ, const gchar * name,
    const GstCaps * caps)
{
  GstPad *pad = GST_ELEMENT_CLASS (gst_rds_stream_mux_parent_class)->request_new_pad (element, templ, name, caps);

  /* with drop-pipeline-eos, a mux whose sources all ended waits for a new one. Its pad-added goes downstream
   * before its first event or frame, from the source thread (as DeepStream documents: on a new source's data). */
  if (pad != NULL)
    wake_idle (GST_RDS_STREAM_MUX (element));
  return pad;
}

/* pad-deleted, once the pad can give no more frames: after the batch being built (which may hold its last frame),
 * or right away. Only for a source that was announced. */
static void
gst_rds_stream_mux_release_pad (GstElement * element, GstPad * pad)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (element);
  GstRdsStreamMuxPad *mpad = GST_RDS_STREAM_MUX_PAD (pad);
  guint index = mpad->index;
  gint prev;

  do {
    prev = g_atomic_int_get (&mpad->state);
  } while (!g_atomic_int_compare_and_exchange (&mpad->state, prev,
          prev == PAD_STATE_ADDED ? PAD_STATE_RELEASED : PAD_STATE_RELEASED_NEW));

  GST_ELEMENT_CLASS (gst_rds_stream_mux_parent_class)->release_pad (element, pad);
  if (prev == PAD_STATE_ADDED)
    send_event (self, rds_event_new_pad_deleted (index));
  wake_idle (self);
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
  g_mutex_lock (&self->events_lock);
  self->can_push = TRUE;
  g_mutex_unlock (&self->events_lock);
  GST_OBJECT_LOCK (self);
  self->fps_n = GST_VIDEO_INFO_FPS_N (&info);
  self->fps_d = GST_VIDEO_INFO_FPS_D (&info);
  GST_OBJECT_UNLOCK (self);
  return TRUE;
}

/* ---- allocation ---- */

/* The output pool size (object lock held). Every demuxed view keeps its whole batch alive, and sinks keep their last
 * buffer (appsink always, basesink with enable-last-sample), so each demux branch can pin a different batch: with
 * buffer-pool-size left at its default the pool holds batch-size + 2 batches (at least 4). A size set explicitly is
 * honoured (start () warns when it is batch-size or less). */
static guint
effective_pool_size (GstRdsStreamMux * self)
{
  if (self->buffer_pool_size_set)
    return self->buffer_pool_size;
  return (guint) MIN ((guint64) MAX_BUFFER_POOL_SIZE, MAX ((guint64) DEFAULT_BUFFER_POOL_SIZE,
          (guint64) self->batch_size + 2));
}

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
  pool_size = effective_pool_size (self);
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

/* ---- stream events ----
 *
 * The source thread queues its events (pending) and pushes them, in order and after the mux's mandatory events,
 * once the output caps are negotiated: before a batch, and when it is done with a batch or a source's event. While
 * it handles one (in_flight), events from other threads (pad-deleted) wait in deferred and follow it: the batch in
 * hand may hold the released pad's last frame. push_lock orders a direct push from another thread against the
 * source thread's pushes, so a pad-deleted cannot overtake the pad-added of a pad requested after it. */

static void
wake_idle (GstRdsStreamMux * self)
{
  g_mutex_lock (&self->wait_lock);
  self->wake_seq++;
  g_cond_broadcast (&self->wait_cond);
  g_mutex_unlock (&self->wait_lock);
}

static void
queue_event (GstRdsStreamMux * self, GstEvent * event)
{
  g_mutex_lock (&self->events_lock);
  g_queue_push_tail (&self->pending, event);
  g_mutex_unlock (&self->events_lock);
}

static void
events_enter (GstRdsStreamMux * self)
{
  g_mutex_lock (&self->events_lock);
  self->in_flight = TRUE;
  g_mutex_unlock (&self->events_lock);
}

/* Source thread: pushes its queued events (when the caps allow). With @leave (done with the batch or event in hand),
 * the deferred events from other threads follow, and in_flight ends once both queues are empty. */
static void
events_push (GstRdsStreamMux * self, gboolean leave)
{
  g_rec_mutex_lock (&self->push_lock);
  for (;;) {
    GstEvent *event = NULL;

    g_mutex_lock (&self->events_lock);
    if (leave)
      while (!g_queue_is_empty (&self->deferred))
        g_queue_push_tail (&self->pending, g_queue_pop_head (&self->deferred));
    if (self->can_push)
      event = g_queue_pop_head (&self->pending);
    if (event == NULL && leave)
      self->in_flight = FALSE;
    g_mutex_unlock (&self->events_lock);
    if (event == NULL)
      break;
    GST_LOG_OBJECT (self, "pushing %" GST_PTR_FORMAT, event);
    if (!gst_aggregator_push_src_event (GST_AGGREGATOR (self), event))
      GST_DEBUG_OBJECT (self, "a stream event was not handled downstream");
  }
  g_rec_mutex_unlock (&self->push_lock);
}

/* Any other thread: pushes @event now, unless the source thread is handling a batch or event or events are waiting
 * (then it follows them). */
static void
send_event (GstRdsStreamMux * self, GstEvent * event)
{
  gboolean now;

  g_rec_mutex_lock (&self->push_lock);
  g_mutex_lock (&self->events_lock);
  now = !self->in_flight && self->can_push && g_queue_is_empty (&self->pending)
      && g_queue_is_empty (&self->deferred);
  if (!now)
    g_queue_push_tail (&self->deferred, event);
  g_mutex_unlock (&self->events_lock);
  if (now && !gst_pad_push_event (GST_AGGREGATOR_SRC_PAD (self), event))
    GST_DEBUG_OBJECT (self, "a stream event was not handled downstream");
  g_rec_mutex_unlock (&self->push_lock);
}

/* Source thread: the per-source events due before anything else from @pad: pad-added (once) and stream-reset after
 * a flush (restarting the frame numbers with frame-num-reset-on-stream-reset). FALSE for a pad released before it
 * was announced: nothing of it goes downstream. */
static gboolean
pad_announce (GstRdsStreamMux * self, GstRdsStreamMuxPad * pad)
{
  gboolean reset_frames;

  if (g_atomic_int_compare_and_exchange (&pad->state, PAD_STATE_NEW, PAD_STATE_ADDED))
    queue_event (self, rds_event_new_pad_added (pad->index));
  if (g_atomic_int_get (&pad->state) == PAD_STATE_RELEASED_NEW)
    return FALSE;
  if (g_atomic_int_compare_and_exchange (&pad->reset_pending, TRUE, FALSE)) {
    GST_OBJECT_LOCK (self);
    reset_frames = self->frame_num_reset_on_stream_reset;
    GST_OBJECT_UNLOCK (self);
    queue_event (self, rds_event_new_stream_reset (pad->index));
    if (reset_frames)
      pad->frame_num = 0;
  }
  return TRUE;
}

/* Source thread: a source's serialized event becomes its stream event. */
static void
pad_stream_event (GstRdsStreamMux * self, GstRdsStreamMuxPad * pad, GstEvent * event)
{
  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_STREAM_START:{
      const gchar *stream_id = NULL;

      gst_event_parse_stream_start (event, &stream_id);
      queue_event (self, rds_event_new_stream_start (pad->index, stream_id));
      break;
    }
    case GST_EVENT_SEGMENT:{
      const GstSegment *segment;

      gst_event_parse_segment (event, &segment);
      queue_event (self, rds_event_new_stream_segment (pad->index, segment));
      break;
    }
    case GST_EVENT_EOS:{
      gboolean reset_frames;

      GST_OBJECT_LOCK (self);
      reset_frames = self->frame_num_reset_on_eos;
      GST_OBJECT_UNLOCK (self);
      queue_event (self, rds_event_new_stream_eos (pad->index));
      if (reset_frames)
        pad->frame_num = 0;
      break;
    }
    default:
      break;
  }
}

static gboolean
gst_rds_stream_mux_sink_event (GstAggregator * agg, GstAggregatorPad * aggpad, GstEvent * event)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GstRdsStreamMuxPad *pad = GST_RDS_STREAM_MUX_PAD (aggpad);
  GstEventType type = GST_EVENT_TYPE (event);
  /* serialized events but FLUSH_STOP were queued by the aggregator: this is the source thread, in stream order */
  gboolean queued = GST_EVENT_IS_SERIALIZED (event) && type != GST_EVENT_FLUSH_STOP;
  gboolean ret;

  if (queued) {
    events_enter (self);
    /* a released pad may have sent pad-deleted already (directly, from the releasing thread): nothing after it */
    if (pad_announce (self, pad) && g_atomic_int_get (&pad->state) == PAD_STATE_ADDED)
      pad_stream_event (self, pad, event);
  }

  switch (type) {
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
        ret = FALSE;
        goto done;
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
    case GST_EVENT_FLUSH_START:{
      guint32 seqnum = gst_event_get_seqnum (event);
      gboolean all = TRUE;

      /* Only an element-wide flush (the same FLUSH_START on every sink pad, as GstAggregator counts it) stops the
       * source task, and only that one ends with flush () clearing the pool's flushing state again. A flush of
       * one source (a reconnect, a single-source seek) leaves batching running and the pool alone. */
      GST_OBJECT_LOCK (self);
      pad->flush_seqnum = seqnum;
      for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL && all; l = l->next)
        all = GST_RDS_STREAM_MUX_PAD (l->data)->flush_seqnum == seqnum;
      GST_OBJECT_UNLOCK (self);
      /* a batch waiting for a pool buffer must give up so the source task can stop */
      if (all)
        set_pool_flushing (self, TRUE);
      wake_idle (self);
      break;
    }
    default:
      break;
  }
  ret = GST_AGGREGATOR_CLASS (gst_rds_stream_mux_parent_class)->sink_event (agg, aggpad, event);

  if (type == GST_EVENT_FLUSH_START || type == GST_EVENT_FLUSH_STOP)
    pad_clear_arrivals (pad);
  if (type == GST_EVENT_FLUSH_STOP) {
    /* the source's next event or frame is preceded by rds-stream-reset; the pad is no longer at EOS */
    g_atomic_int_set (&pad->reset_pending, TRUE);
    wake_idle (self);
  }
done:
  if (queued)
    events_push (self, TRUE);
  return ret;
}

static GstFlowReturn
gst_rds_stream_mux_sink_event_pre_queue (GstAggregator * agg, GstAggregatorPad * aggpad, GstEvent * event)
{
  GstFlowReturn ret = GST_AGGREGATOR_CLASS (gst_rds_stream_mux_parent_class)->sink_event_pre_queue (agg, aggpad,
      event);

  /* a STREAM_START after EOS revives a source */
  wake_idle (GST_RDS_STREAM_MUX (agg));
  return ret;
}

static GstFlowReturn
gst_rds_stream_mux_flush (GstAggregator * agg)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);

  set_pool_flushing (self, FALSE);
  self->batch_start = GST_CLOCK_TIME_NONE;
  self->last_pts = GST_CLOCK_TIME_NONE;
  return GST_FLOW_OK;
}

/* ---- timing ---- */

/* sync-inputs: the running time of the earliest frame queued on any pad, or NONE. */
static GstClockTime
earliest_queued (GstRdsStreamMux * self)
{
  GstClockTime earliest = GST_CLOCK_TIME_NONE;
  GPtrArray *pads = g_ptr_array_new_with_free_func (gst_object_unref);

  GST_OBJECT_LOCK (self);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL; l = l->next)
    g_ptr_array_add (pads, gst_object_ref (l->data));
  GST_OBJECT_UNLOCK (self);
  for (guint i = 0; i < pads->len; i++) {
    GstAggregatorPad *pad = g_ptr_array_index (pads, i);
    GstBuffer *buf = gst_aggregator_pad_peek_buffer (pad);
    GstClockTime rt;

    if (buf == NULL)
      continue;
    GST_OBJECT_LOCK (pad);
    rt = gst_segment_to_running_time (&pad->segment, GST_FORMAT_TIME, GST_BUFFER_PTS (buf));
    GST_OBJECT_UNLOCK (pad);
    gst_buffer_unref (buf);
    if (GST_CLOCK_TIME_IS_VALID (rt) && (!GST_CLOCK_TIME_IS_VALID (earliest) || rt < earliest))
      earliest = rt;
  }
  g_ptr_array_unref (pads);
  return earliest;
}

/* Called by the aggregator on the source thread before it waits; NONE waits for data. The aggregator waits until
 * the clock reaches the returned running time plus its whole latency, the upstream's included.
 * - sync-inputs: the earliest queued frame's running time.
 * - batched-push-timeout: the arrival running time of the oldest frame still queued (the next batch's first frame),
 *   minus the upstream latency (that frame has arrived already): the batch is due one timeout (plus max-latency)
 *   after it arrived, also when it queued up while the previous batch was waiting. A queued buffer without an
 *   arrival time (no clock when it came, or a GAP buffer) counts from when the source thread first saw it. */
static GstClockTime
gst_rds_stream_mux_get_next_time (GstAggregator * agg)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GstClockTime start = GST_CLOCK_TIME_NONE;
  gboolean unknown = FALSE;

  if (self->sync_mode)
    return earliest_queued (self);
  if (!self->timeout_enabled)
    return GST_CLOCK_TIME_NONE;

  GST_OBJECT_LOCK (self);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL; l = l->next) {
    GstRdsStreamMuxPad *pad = l->data;
    Arrival *a;

    if (!gst_aggregator_pad_has_buffer (GST_AGGREGATOR_PAD (pad)))
      continue;
    g_mutex_lock (&pad->arrivals_lock);
    a = g_queue_peek_head (&pad->arrivals);
    if (a != NULL && GST_CLOCK_TIME_IS_VALID (a->time)) {
      if (!GST_CLOCK_TIME_IS_VALID (start) || a->time < start)
        start = a->time;
    } else {
      unknown = TRUE;
    }
    g_mutex_unlock (&pad->arrivals_lock);
  }
  GST_OBJECT_UNLOCK (self);

  if (unknown) {
    if (!GST_CLOCK_TIME_IS_VALID (self->batch_start))
      self->batch_start = gst_element_get_current_running_time (GST_ELEMENT (self));
    if (GST_CLOCK_TIME_IS_VALID (self->batch_start) && (!GST_CLOCK_TIME_IS_VALID (start)
            || self->batch_start < start))
      start = self->batch_start;
  }
  if (!GST_CLOCK_TIME_IS_VALID (start))
    return GST_CLOCK_TIME_NONE;
  /* (in the first upstream-latency of running time this clamps at 0: the batch is due later than the timeout) */
  return start - MIN (start, self->upstream_latency);
}

/* Source thread, no aggregator lock held: the upstream's share of the aggregator latency, for get_next_time. */
static void
update_upstream_latency (GstRdsStreamMux * self)
{
  GstClockTime total, own;
  gint timeout;
  guint max_latency;

  if (!self->timeout_enabled || self->sync_mode)
    return;
  GST_OBJECT_LOCK (self);
  timeout = self->batched_push_timeout;
  max_latency = self->max_latency;
  GST_OBJECT_UNLOCK (self);
  own = (GstClockTime) MAX (timeout, 0) * GST_USECOND + max_latency;
  total = gst_aggregator_get_latency (GST_AGGREGATOR (self));
  self->upstream_latency = GST_CLOCK_TIME_IS_VALID (total) && total > own ? total - own : 0;
}

/* ---- start / stop ---- */

/* Forgets queued stream events and which pads were announced: a restart announces every source again. */
static void
events_reset (GstRdsStreamMux * self)
{
  g_mutex_lock (&self->events_lock);
  g_queue_clear_full (&self->pending, (GDestroyNotify) gst_event_unref);
  g_queue_clear_full (&self->deferred, (GDestroyNotify) gst_event_unref);
  self->in_flight = FALSE;
  self->can_push = FALSE;
  g_mutex_unlock (&self->events_lock);

  GST_OBJECT_LOCK (self);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL; l = l->next) {
    GstRdsStreamMuxPad *pad = l->data;

    g_atomic_int_compare_and_exchange (&pad->state, PAD_STATE_ADDED, PAD_STATE_NEW);
    g_atomic_int_set (&pad->reset_pending, FALSE);
  }
  GST_OBJECT_UNLOCK (self);
}

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
  gboolean sync_inputs, small_pool;
  guint max_latency, pool_size, batch_size;

  if (!check_settings (self))
    return FALSE;

  GST_OBJECT_LOCK (self);
  timeout = self->batched_push_timeout;
  sync_inputs = self->sync_inputs;
  max_latency = self->max_latency;
  pool_size = self->buffer_pool_size;
  small_pool = self->buffer_pool_size_set && self->buffer_pool_size <= self->batch_size;
  batch_size = self->batch_size;
  GST_OBJECT_UNLOCK (self);

  if (small_pool)
    GST_WARNING_OBJECT (self, "buffer-pool-size %u is not larger than batch-size %u: every demuxed frame keeps its "
        "whole batch alive and sinks keep their last buffer, so %u or more downstream branches can hold every "
        "batch and stall the mux. Leave buffer-pool-size unset (batch-size + 2) or make it larger",
        pool_size, batch_size, pool_size);

  self->timeout_enabled = timeout >= 0;
  self->sync_mode = sync_inputs;
  self->upstream_latency = 0;
  self->batch_start = GST_CLOCK_TIME_NONE;
  self->rr_next = 0;
  self->last_pts = GST_CLOCK_TIME_NONE;
  self->have_out_info = FALSE;
  events_reset (self);
  /* batched-push-timeout owns the aggregator's "latency" property: set on every start (the timeout, or 0 without
   * one), so a value set directly, or left over from an earlier run with a timeout, never applies. max-latency is
   * the subclass latency: reported, and waited for in live mode. */
  gst_aggregator_set_force_live (agg, self->timeout_enabled || self->sync_mode);
  g_object_set (self, "latency", self->timeout_enabled ? (guint64) timeout * GST_USECOND : (guint64) 0, NULL);
  gst_aggregator_set_latency (agg, max_latency, max_latency);
  return TRUE;
}

static gboolean
gst_rds_stream_mux_stop (GstAggregator * agg)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (agg);
  GPtrArray *stages = g_ptr_array_new ();

  /* the staging surfaces are freed outside the object lock (freeing synchronizes with the GPU) */
  GST_OBJECT_LOCK (self);
  for (GList * l = GST_ELEMENT (self)->sinkpads; l != NULL; l = l->next) {
    GstRdsStreamMuxPad *pad = l->data;

    if (pad->stage != NULL)
      g_ptr_array_add (stages, pad->stage);
    pad->stage = NULL;
    pad->frame_num = 0;
    pad_clear_arrivals (pad);
  }
  self->fps_n = 0;
  self->fps_d = 1;
  GST_OBJECT_UNLOCK (self);
  for (guint i = 0; i < stages->len; i++)
    rds_buf_surface_destroy (g_ptr_array_index (stages, i));
  g_ptr_array_unref (stages);

  if (self->stream != NULL) {
    if (hipStreamDestroy (self->stream) != hipSuccess)
      GST_WARNING_OBJECT (self, "hipStreamDestroy failed");
    self->stream = NULL;
  }
  self->have_out_info = FALSE;
  events_reset (self);
  return TRUE;
}

static GstStateChangeReturn
gst_rds_stream_mux_change_state (GstElement * element, GstStateChange transition)
{
  /* the source task may be waiting for a pool buffer held downstream: wake it before the aggregator stops it */
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    set_pool_flushing (GST_RDS_STREAM_MUX (element), TRUE);
    wake_idle (GST_RDS_STREAM_MUX (element));
  }
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
  GstClockTime running_time;    /* of the PTS, taken when popped: a flush may reset the pad's segment later */
  RdsBufSurfTransformRect src_rect;     /* the part of the frame that goes into the slot (set by fill_slot) */
  gboolean cropped;             /* src_rect is narrower than the frame (a crop meta) */
} MuxEntry;

/* The part of the source surface (@w x @h) the mux reads: the buffer's crop meta clamped to the surface, else the
 * whole surface. The surface, not the caps: an element cropping in passthrough (videocrop) announces the cropped
 * size in its caps but forwards the full surface with a crop meta. */
static void
entry_set_src_rect (GstRdsStreamMux * self, MuxEntry * e, guint w, guint h)
{
  GstVideoCropMeta *crop = gst_buffer_get_video_crop_meta (e->buf);

  e->src_rect = (RdsBufSurfTransformRect) {.left = 0,.top = 0,.width = w,.height = h };
  e->cropped = FALSE;
  if (crop == NULL)
    return;
  if (crop->x >= w || crop->y >= h || crop->width == 0 || crop->height == 0) {
    GST_DEBUG_OBJECT (self, "ignoring crop meta %ux%u at %u,%u: nothing of it lies in the %ux%u frame",
        crop->width, crop->height, crop->x, crop->y, w, h);
    return;
  }
  e->src_rect.left = crop->x;
  e->src_rect.top = crop->y;
  e->src_rect.width = MIN (crop->width, w - crop->x);
  e->src_rect.height = MIN (crop->height, h - crop->y);
  e->cropped = e->src_rect.left != 0 || e->src_rect.top != 0 || e->src_rect.width != w || e->src_rect.height != h;
}

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
  entry_set_src_rect (self, e, src->surfaceList[0].width, src->surfaceList[0].height);

  /* Destination: a hand-built single-surface view of the slot (no library ownership: _priv NULL). */
  view = *batch;
  view.batchSize = 1;
  view.numFilled = 1;
  view.surfaceList = &batch->surfaceList[slot];
  view._priv = NULL;

  params.transform_flag = RDS_BUFSURF_TRANSFORM_FILTER;
  params.transform_filter = (RdsBufSurfTransform_Inter) filter;
  if (e->cropped) {
    params.transform_flag |= RDS_BUFSURF_TRANSFORM_CROP_SRC;
    params.src_rect = &e->src_rect;
  }
  if (padding) {
    gboolean even = GST_VIDEO_INFO_FORMAT (&self->out_info) == GST_VIDEO_FORMAT_NV12
        || GST_VIDEO_INFO_FORMAT (&self->out_info) == GST_VIDEO_FORMAT_I420;

    dst_rect = padded_rect (e->src_rect.width, e->src_rect.height, out_w, out_h, even);
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
  /* the size of what was scaled into the slot (the crop, when the frame carries one) */
  fm->source_frame_width = e->src_rect.width;
  fm->source_frame_height = e->src_rect.height;
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
  gboolean padding, sort_batch, attach_sys_ts, live_source, drop_eos, all_eos = TRUE;
  GstFlowReturn ret = GST_FLOW_ERROR;
  guint wake_seq;

  /* events from other threads (pad-deleted) wait until this batch is out: it may hold the pad's last frame */
  events_enter (self);
  update_upstream_latency (self);
  g_mutex_lock (&self->wait_lock);
  wake_seq = self->wake_seq;
  g_mutex_unlock (&self->wait_lock);

  GST_OBJECT_LOCK (self);
  batch_size = self->batch_size;
  filter = self->interpolation_method;
  compute = (RdsBufSurfTransform_Compute) self->compute_hw;
  padding = self->enable_padding;
  sort_batch = self->sort_batch;
  attach_sys_ts = self->attach_sys_ts;
  live_source = self->live_source;
  drop_eos = self->drop_pipeline_eos;
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
    MuxEntry e = {.pad = pad,.buf = gst_aggregator_pad_pop_buffer (GST_AGGREGATOR_PAD (pad)) };

    if (e.buf != NULL) {
      pad_take_arrival (pad, e.buf);
      GST_OBJECT_LOCK (pad);
      e.running_time = gst_segment_to_running_time (&GST_AGGREGATOR_PAD (pad)->segment, GST_FORMAT_TIME,
          GST_BUFFER_PTS (e.buf));
      GST_OBJECT_UNLOCK (pad);
      /* a source with no event since it was added or flushed; a frame of a pad released unannounced is dropped */
      if (pad_announce (self, pad))
        g_array_append_val (entries, e);
      else
        gst_buffer_unref (e.buf);
    }
  }
  if (n > 0)
    self->rr_next = (self->rr_next + 1) % n;

  if (entries->len == 0) {
    for (guint i = 0; i < n; i++)
      all_eos &= gst_aggregator_pad_is_eos (g_ptr_array_index (pads, i));
    ret = n > 0 && all_eos ? GST_FLOW_EOS : GST_AGGREGATOR_FLOW_NEED_DATA;
    if (ret == GST_FLOW_EOS && drop_eos) {
      /* No EOS downstream: the aggregator would pause its task, and the pad stays EOS. Wait instead (returning at
       * once would spin: the aggregator calls aggregate () again right away while every pad is at EOS) until a
       * source is added, revived (STREAM_START) or flushed, or the element stops. Bounded, so a missed wake-up
       * costs one period at most. */
      gint64 end = g_get_monotonic_time () + IDLE_WAIT_US;

      events_push (self, TRUE);
      GST_LOG_OBJECT (self, "every source ended, drop-pipeline-eos: waiting");
      g_mutex_lock (&self->wait_lock);
      while (self->wake_seq == wake_seq)
        if (!g_cond_wait_until (&self->wait_cond, &self->wait_lock, end))
          break;
      g_mutex_unlock (&self->wait_lock);
      ret = GST_AGGREGATOR_FLOW_NEED_DATA;
    }
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
  if (ret == GST_FLOW_FLUSHING) {
    /* An element-wide flush (or a stop) woke the wait: drop the frames. Not FLUSHING: the aggregator may not have
     * marked itself flushing yet (we set the pool flushing before chaining FLUSH_START up), and would then set
     * every sink pad flushing for good, including one whose FLUSH_STOP already came. */
    GST_DEBUG_OBJECT (self, "pool flushing: dropping a batch of %u", entries->len);
    ret = GST_AGGREGATOR_FLOW_NEED_DATA;
    goto out;
  }
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
    GstClockTime rt = e->running_time;

    if (!fill_slot (self, e, batch, i, filter, compute, padding)) {
      gst_buffer_unmap (outbuf, &map);
      rds_destroy_batch_meta (bm);
      goto out;
    }
    fill_frame_meta (self, bm, e, i, attach_sys_ts);
    /* live-source: the last copied frame's timestamp (as documented); else the earliest frame's */
    if (live_source)
      pts = rt;
    else if (GST_CLOCK_TIME_IS_VALID (rt) && (!GST_CLOCK_TIME_IS_VALID (pts) || rt < pts))
      pts = rt;
  }
  batch->numFilled = entries->len;
  gst_buffer_unmap (outbuf, &map);
  rds_buffer_add_batch_meta (outbuf, bm);

  /* The output segment is TIME from 0: running time is the output timestamp. Sources joining late or with other
   * segments can make the earliest frame older than the previous batch: keep batch timestamps monotonic. */
  if (GST_CLOCK_TIME_IS_VALID (pts) && GST_CLOCK_TIME_IS_VALID (self->last_pts) && pts < self->last_pts)
    pts = self->last_pts;
  if (GST_CLOCK_TIME_IS_VALID (pts))
    self->last_pts = pts;
  GST_BUFFER_PTS (outbuf) = pts;
  GST_BUFFER_DTS (outbuf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (outbuf) = GST_CLOCK_TIME_NONE;
  if (GST_CLOCK_TIME_IS_VALID (pts)) {
    GST_OBJECT_LOCK (self);
    GST_AGGREGATOR_PAD (agg->srcpad)->segment.position = pts;
    GST_OBJECT_UNLOCK (self);
  }
  gst_aggregator_selected_samples (agg, pts, GST_CLOCK_TIME_NONE, GST_CLOCK_TIME_NONE, NULL);
  /* the sources' events from before this batch go first */
  events_push (self, FALSE);
  ret = gst_aggregator_finish_buffer (agg, outbuf);
  outbuf = NULL;

out:
  /* the remaining events (all of them on an early return), then events from other threads go straight out again;
   * on EOS, before the aggregator's EOS */
  events_push (self, TRUE);
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
  gboolean noop = FALSE, log_noop;

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
      if (check_mutable (self, pspec)) {
        self->buffer_pool_size = g_value_get_uint (value);
        self->buffer_pool_size_set = TRUE;
      }
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
    case PROP_SORT_BATCH:
      self->sort_batch = g_value_get_boolean (value);
      break;
    case PROP_ATTACH_SYS_TS:
      self->attach_sys_ts = g_value_get_boolean (value);
      break;
    case PROP_LIVE_SOURCE:
      self->live_source = g_value_get_boolean (value);
      break;
    case PROP_MAX_LATENCY:
      if (check_mutable (self, pspec))
        self->max_latency = g_value_get_uint (value);
      break;
    case PROP_SYNC_INPUTS:
      if (check_mutable (self, pspec))
        self->sync_inputs = g_value_get_boolean (value);
      break;
    case PROP_DROP_PIPELINE_EOS:
      self->drop_pipeline_eos = g_value_get_boolean (value);
      break;
    case PROP_FRAME_NUM_RESET_ON_EOS:
      self->frame_num_reset_on_eos = g_value_get_boolean (value);
      break;
    case PROP_FRAME_NUM_RESET_ON_STREAM_RESET:
      self->frame_num_reset_on_stream_reset = g_value_get_boolean (value);
      break;
    case PROP_FRAME_DURATION:
      self->frame_duration = g_value_get_uint64 (value);
      noop = TRUE;
      break;
    case PROP_ALIGN_FIRST_BUFFER:
      self->align_first_buffer = g_value_get_boolean (value);
      noop = TRUE;
      break;
    case PROP_ALIGN_INPUTS:
      self->align_inputs = g_value_get_boolean (value);
      noop = TRUE;
      break;
    case PROP_CACHE_BUFFER:
      self->cache_buffer = g_value_get_boolean (value);
      noop = TRUE;
      break;
    case PROP_CACHE_BUFFER_TIMEOUT:
      self->cache_buffer_timeout = g_value_get_int (value);
      noop = TRUE;
      break;
    case PROP_EXTRACT_SEI_SIM_TIME:
      self->extract_sei_sim_time = g_value_get_boolean (value);
      noop = TRUE;
      break;
    case PROP_EXTRACT_SEI_TYPE5_DATA:
      self->extract_sei_type5_data = g_value_get_boolean (value);
      noop = TRUE;
      break;
    case PROP_DROP_BACKWARD_SEI:
      self->drop_backward_sei = g_value_get_boolean (value);
      noop = TRUE;
      break;
    case PROP_SYNC_INPUTS_NTP:
      self->sync_inputs_ntp = g_value_get_int (value);
      noop = TRUE;
      break;
    case PROP_FAILSAFE_FLUSH_COUNT:
      self->failsafe_flush_count = g_value_get_uint (value);
      noop = TRUE;
      break;
    case PROP_ASYNC_PROCESS:
      self->async_process = g_value_get_boolean (value);
      noop = TRUE;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  log_noop = noop && !self->noop_logged;
  if (log_noop)
    self->noop_logged = TRUE;
  GST_OBJECT_UNLOCK (self);
  /* outside the object lock: logging an object takes it */
  if (log_noop)
    GST_INFO_OBJECT (self, "property '%s' is accepted for compatibility but has no effect (logged once: other "
        "such properties are not reported)", pspec->name);
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
    case PROP_SORT_BATCH:
      g_value_set_boolean (value, self->sort_batch);
      break;
    case PROP_ATTACH_SYS_TS:
      g_value_set_boolean (value, self->attach_sys_ts);
      break;
    case PROP_LIVE_SOURCE:
      g_value_set_boolean (value, self->live_source);
      break;
    case PROP_MAX_LATENCY:
      g_value_set_uint (value, self->max_latency);
      break;
    case PROP_SYNC_INPUTS:
      g_value_set_boolean (value, self->sync_inputs);
      break;
    case PROP_DROP_PIPELINE_EOS:
      g_value_set_boolean (value, self->drop_pipeline_eos);
      break;
    case PROP_FRAME_NUM_RESET_ON_EOS:
      g_value_set_boolean (value, self->frame_num_reset_on_eos);
      break;
    case PROP_FRAME_NUM_RESET_ON_STREAM_RESET:
      g_value_set_boolean (value, self->frame_num_reset_on_stream_reset);
      break;
    case PROP_FRAME_DURATION:
      g_value_set_uint64 (value, self->frame_duration);
      break;
    case PROP_ALIGN_FIRST_BUFFER:
      g_value_set_boolean (value, self->align_first_buffer);
      break;
    case PROP_ALIGN_INPUTS:
      g_value_set_boolean (value, self->align_inputs);
      break;
    case PROP_CACHE_BUFFER:
      g_value_set_boolean (value, self->cache_buffer);
      break;
    case PROP_CACHE_BUFFER_TIMEOUT:
      g_value_set_int (value, self->cache_buffer_timeout);
      break;
    case PROP_EXTRACT_SEI_SIM_TIME:
      g_value_set_boolean (value, self->extract_sei_sim_time);
      break;
    case PROP_EXTRACT_SEI_TYPE5_DATA:
      g_value_set_boolean (value, self->extract_sei_type5_data);
      break;
    case PROP_DROP_BACKWARD_SEI:
      g_value_set_boolean (value, self->drop_backward_sei);
      break;
    case PROP_SYNC_INPUTS_NTP:
      g_value_set_int (value, self->sync_inputs_ntp);
      break;
    case PROP_FAILSAFE_FLUSH_COUNT:
      g_value_set_uint (value, self->failsafe_flush_count);
      break;
    case PROP_ASYNC_PROCESS:
      g_value_set_boolean (value, self->async_process);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_stream_mux_finalize (GObject * object)
{
  GstRdsStreamMux *self = GST_RDS_STREAM_MUX (object);

  g_queue_clear_full (&self->pending, (GDestroyNotify) gst_event_unref);
  g_queue_clear_full (&self->deferred, (GDestroyNotify) gst_event_unref);
  g_mutex_clear (&self->events_lock);
  g_rec_mutex_clear (&self->push_lock);
  g_cond_clear (&self->wait_cond);
  g_mutex_clear (&self->wait_lock);
  G_OBJECT_CLASS (gst_rds_stream_mux_parent_class)->finalize (object);
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
      g_param_spec_uint ("buffer-pool-size", "Buffer pool size",
          "Number of batches in the output pool (not set: batch-size + 2, at least 4)", 1,
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
  g_object_class_install_property (gobject_class, PROP_SORT_BATCH,
      g_param_spec_boolean ("sort-batch", "Sort batch", "Order the frames of a batch by sink pad index",
          DEFAULT_SORT_BATCH, flags | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property (gobject_class, PROP_ATTACH_SYS_TS,
      g_param_spec_boolean ("attach-sys-ts", "Attach system timestamp",
          "Set each frame's ntp_timestamp to the system (wall-clock) time in ns; otherwise 0", DEFAULT_ATTACH_SYS_TS,
          flags | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property (gobject_class, PROP_LIVE_SOURCE,
      g_param_spec_boolean ("live-source", "Live source",
          "The sources are live: a batch takes the timestamp of the last frame copied into it (otherwise the "
          "earliest frame's)", DEFAULT_LIVE_SOURCE, flags | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property (gobject_class, PROP_MAX_LATENCY,
      g_param_spec_uint ("max-latency", "Maximum latency",
          "Additional latency in live mode (ns), reported downstream and waited for before a partial batch", 0,
          G_MAXUINT, DEFAULT_MAX_LATENCY, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_SYNC_INPUTS,
      g_param_spec_boolean ("sync-inputs", "Synchronize inputs",
          "Push a partial batch once the clock reaches its earliest frame's running time (plus the latency), "
          "instead of waiting for every source", DEFAULT_SYNC_INPUTS, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_DROP_PIPELINE_EOS,
      g_param_spec_boolean ("drop-pipeline-eos", "Drop pipeline EOS",
          "Do not send EOS downstream when every source has ended (the per-source stream EOS events still go)",
          DEFAULT_DROP_PIPELINE_EOS, flags | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property (gobject_class, PROP_FRAME_NUM_RESET_ON_EOS,
      g_param_spec_boolean ("frame-num-reset-on-eos", "Reset frame numbers on EOS",
          "Restart a source's frame_num from 0 after its EOS", DEFAULT_FRAME_NUM_RESET_ON_EOS,
          flags | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property (gobject_class, PROP_FRAME_NUM_RESET_ON_STREAM_RESET,
      g_param_spec_boolean ("frame-num-reset-on-stream-reset", "Reset frame numbers on stream reset",
          "Restart a source's frame_num from 0 after it is flushed (stream reset)",
          DEFAULT_FRAME_NUM_RESET_ON_STREAM_RESET, flags | GST_PARAM_MUTABLE_PLAYING));

  /* Accepted so that configurations and code setting them keep working; they have no effect. */
  g_object_class_install_property (gobject_class, PROP_FRAME_DURATION,
      g_param_spec_uint64 ("frame-duration", "Frame duration", "Accepted, no effect", 0, G_MAXUINT64,
          DEFAULT_FRAME_DURATION, flags));
  g_object_class_install_property (gobject_class, PROP_ALIGN_FIRST_BUFFER,
      g_param_spec_boolean ("align-first-buffer", "Align first buffer", "Accepted, no effect", FALSE, flags));
  g_object_class_install_property (gobject_class, PROP_ALIGN_INPUTS,
      g_param_spec_boolean ("align-inputs", "Align inputs", "Accepted, no effect", FALSE, flags));
  g_object_class_install_property (gobject_class, PROP_CACHE_BUFFER,
      g_param_spec_boolean ("cache-buffer", "Cache buffer", "Accepted, no effect", FALSE, flags));
  g_object_class_install_property (gobject_class, PROP_CACHE_BUFFER_TIMEOUT,
      g_param_spec_int ("cache-buffer-timeout", "Cache buffer timeout", "Accepted, no effect", -1, G_MAXINT,
          DEFAULT_CACHE_BUFFER_TIMEOUT, flags));
  g_object_class_install_property (gobject_class, PROP_EXTRACT_SEI_SIM_TIME,
      g_param_spec_boolean ("extract-sei-sim-time", "Extract SEI simulation time", "Accepted, no effect", FALSE,
          flags));
  g_object_class_install_property (gobject_class, PROP_EXTRACT_SEI_TYPE5_DATA,
      g_param_spec_boolean ("extract-sei-type5-data", "Extract SEI type 5 data", "Accepted, no effect", FALSE,
          flags));
  g_object_class_install_property (gobject_class, PROP_DROP_BACKWARD_SEI,
      g_param_spec_boolean ("drop-backward-sei", "Drop backward SEI", "Accepted, no effect", FALSE, flags));
  g_object_class_install_property (gobject_class, PROP_SYNC_INPUTS_NTP,
      g_param_spec_int ("sync-inputs-ntp", "Synchronize inputs by NTP", "Accepted, no effect", 0, G_MAXINT,
          DEFAULT_SYNC_INPUTS_NTP, flags));
  g_object_class_install_property (gobject_class, PROP_FAILSAFE_FLUSH_COUNT,
      g_param_spec_uint ("failsafe-flush-count", "Failsafe flush count", "Accepted, no effect", 1,
          MAX_BUFFER_POOL_SIZE, DEFAULT_FAILSAFE_FLUSH_COUNT, flags));
  g_object_class_install_property (gobject_class, PROP_ASYNC_PROCESS,
      g_param_spec_boolean ("async-process", "Asynchronous processing", "Accepted, no effect", FALSE, flags));

  gst_element_class_add_static_pad_template_with_gtype (element_class, &sink_template,
      GST_TYPE_RDS_STREAM_MUX_PAD);
  gst_element_class_add_static_pad_template_with_gtype (element_class, &src_template, GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_set_static_metadata (element_class, "Redstream stream muxer", "Codec/Muxer/Video",
      "Batches frames from several sources into one memory:RDS batch, scaling each into its slot",
      "Redstream contributors");

  gobject_class->finalize = gst_rds_stream_mux_finalize;
  element_class->change_state = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_change_state);
  element_class->request_new_pad = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_request_new_pad);
  element_class->release_pad = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_release_pad);
  agg_class->create_new_pad = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_create_new_pad);
  agg_class->update_src_caps = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_update_src_caps);
  agg_class->negotiated_src_caps = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_negotiated_src_caps);
  agg_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_decide_allocation);
  agg_class->propose_allocation = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_propose_allocation);
  agg_class->sink_event = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_sink_event);
  agg_class->sink_event_pre_queue = GST_DEBUG_FUNCPTR (gst_rds_stream_mux_sink_event_pre_queue);
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
  self->live_source = DEFAULT_LIVE_SOURCE;
  self->max_latency = DEFAULT_MAX_LATENCY;
  self->sync_inputs = DEFAULT_SYNC_INPUTS;
  self->drop_pipeline_eos = DEFAULT_DROP_PIPELINE_EOS;
  self->frame_num_reset_on_eos = DEFAULT_FRAME_NUM_RESET_ON_EOS;
  self->frame_num_reset_on_stream_reset = DEFAULT_FRAME_NUM_RESET_ON_STREAM_RESET;
  self->frame_duration = DEFAULT_FRAME_DURATION;
  self->cache_buffer_timeout = DEFAULT_CACHE_BUFFER_TIMEOUT;
  self->sync_inputs_ntp = DEFAULT_SYNC_INPUTS_NTP;
  self->failsafe_flush_count = DEFAULT_FAILSAFE_FLUSH_COUNT;
  g_mutex_init (&self->events_lock);
  g_queue_init (&self->pending);
  g_queue_init (&self->deferred);
  g_rec_mutex_init (&self->push_lock);
  g_mutex_init (&self->wait_lock);
  g_cond_init (&self->wait_cond);
  self->batch_start = GST_CLOCK_TIME_NONE;
  self->last_pts = GST_CLOCK_TIME_NONE;
  self->fps_n = 0;
  self->fps_d = 1;
  gst_video_info_init (&self->out_info);
}
