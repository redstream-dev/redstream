/* Redstream: rdsvideodec, H.264/H.265 decode with rocDecode into pooled memory:RDS NV12 surfaces (or system memory when
 * downstream cannot take memory:RDS). Property names follow DeepStream's public nvv4l2decoder documentation.
 *
 * Flow: each input frame (one access unit) goes to the rocDecode parser as one packet whose timestamp is the frame's
 * system_frame_number. The parser calls back synchronously from rocDecParseVideoData: on_sequence (create the
 * decoder, set the output state), on_decode (submit a picture) and on_display (in display order: copy the cropped
 * picture into an output buffer, synchronize, finish the frame found by that timestamp).
 *
 * Lifecycle: a new sequence with another geometry reconfigures the decoder in place when the new coded size fits
 * the one it was created for, else recreates it; either way the output caps follow. flush (seek) destroys the
 * parser and the decoder; drain and finish display every pending picture (an end-of-stream packet), release the
 * frames the parser never displayed and destroy the parser. Both come back lazily with the next frame.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsvideodec.h"

#include <gst/video/video.h>
#include <rds/rds.h>
/* after GLib/GStreamer: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>
#include <rocdecode/rocdecode.h>
#include <rocdecode/rocparser.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_video_dec_debug);
#define GST_CAT_DEFAULT gst_rds_video_dec_debug

#define DEFAULT_GPU_ID 0
#define DEFAULT_CUDADEC_MEMTYPE GST_RDS_CUDADEC_MEMTYPE_DEVICE
#define DEFAULT_NUM_EXTRA_SURFACES 0
#define MAX_NUM_EXTRA_SURFACES 24
#define DEFAULT_DROP_FRAME_INTERVAL 0
#define MAX_DROP_FRAME_INTERVAL 30
#define DEFAULT_SKIP_FRAMES GST_RDS_SKIP_FRAMES_DECODE_ALL
#define DEFAULT_LOW_LATENCY_MODE FALSE
/* Decode surfaces on top of the stream's minimum: headroom for pictures held for display (spikes/rocdec-hip). */
#define EXTRA_DECODE_SURFACES 4
/* Output pool: minimum buffers in flight downstream. */
#define OUTPUT_MIN_BUFFERS 4

enum
{
  PROP_0,
  PROP_GPU_ID,
  PROP_CUDADEC_MEMTYPE,
  PROP_NUM_EXTRA_SURFACES,
  PROP_DROP_FRAME_INTERVAL,
  PROP_SKIP_FRAMES,
  PROP_LOW_LATENCY_MODE,
};

/* Integers and nicks of DeepStream's nvv4l2decoder enums (public plugin documentation). */
typedef enum
{
  GST_RDS_CUDADEC_MEMTYPE_DEVICE = 0,
  GST_RDS_CUDADEC_MEMTYPE_PINNED = 1,
  GST_RDS_CUDADEC_MEMTYPE_UNIFIED = 2,
} GstRdsCudaDecMemType;

typedef enum
{
  GST_RDS_SKIP_FRAMES_DECODE_ALL = 0,
  GST_RDS_SKIP_FRAMES_DECODE_NON_REF = 1,
  GST_RDS_SKIP_FRAMES_DECODE_KEY = 2,
} GstRdsSkipFrames;

#define GST_TYPE_RDS_CUDADEC_MEMTYPE (gst_rds_cudadec_memtype_get_type ())
static GType
gst_rds_cudadec_memtype_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {GST_RDS_CUDADEC_MEMTYPE_DEVICE, "HIP device memory", "memtype_device"},
    {GST_RDS_CUDADEC_MEMTYPE_PINNED, "Pinned host memory", "memtype_pinned"},
    {GST_RDS_CUDADEC_MEMTYPE_UNIFIED, "HIP unified (managed) memory", "memtype_unified"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type))
    g_once_init_leave (&type, g_enum_register_static ("GstRdsCudaDecMemType", values));
  return type;
}

#define GST_TYPE_RDS_SKIP_FRAMES (gst_rds_skip_frames_get_type ())
static GType
gst_rds_skip_frames_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {GST_RDS_SKIP_FRAMES_DECODE_ALL, "Decode every frame", "decode_all"},
    {GST_RDS_SKIP_FRAMES_DECODE_NON_REF, "Decode non-reference frames (Jetson-only on DeepStream: decodes every "
          "frame here)", "decode_non_ref"},
    {GST_RDS_SKIP_FRAMES_DECODE_KEY, "Decode key frames only", "decode_key"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type))
    g_once_init_leave (&type, g_enum_register_static ("GstRdsSkipFrames", values));
  return type;
}

struct _GstRdsVideoDec
{
  GstVideoDecoder parent;

  /* properties (object lock) */
  guint gpu_id;
  GstRdsCudaDecMemType cudadec_memtype;
  guint num_extra_surfaces;
  guint drop_frame_interval;
  GstRdsSkipFrames skip_frames;
  gboolean low_latency_mode;

  /* streaming state (stream lock) */
  guint stream_gpu;             /* the GPU of the stream, the decoder and the output pool, fixed at start */
  RdsBufSurfaceMemType mem_type;        /* output surfaces' memory type, fixed at start */
  guint extra_surfaces;         /* num-extra-surfaces, fixed at start */
  hipStream_t stream;
  GstVideoCodecState *input_state;
  rocDecVideoCodec codec;
  RocdecVideoParser parser;     /* created lazily on the first frame */
  rocDecDecoderHandle decoder;  /* created by the first sequence callback */
  RocdecVideoFormat format;     /* the sequence the decoder decodes now */
  guint max_width, max_height;  /* the coded size the decoder was created for: reconfiguring may not exceed it */
  guint num_surfaces;
  guint out_width, out_height;  /* display size: the decoder's output, already cropped */
  gboolean output_dirty;        /* the input caps changed (e.g. framerate): renew the output state */
  gboolean finishing;           /* inside finish_frame: the output buffer is filled, its memory may not change */
  guint64 display_count;        /* pictures displayed since start or flush (drop-frame-interval) */
  GstRdsSkipFrames last_skip_frames;    /* skip-frames as handle_frame last saw it */
  gboolean wait_sync;           /* a delta frame was skipped: drop delta frames until the next key frame */
  /* results of the callbacks during one rocDecParseVideoData call */
  GstFlowReturn cb_flow;        /* non-OK: stop and return it (an error was already posted if needed) */
  gboolean cb_decode_error;     /* a picture failed to decode or to come out */
  gboolean cb_decode_failed;    /* rocDecDecodeFrame failed (the parser then never displays that picture) */
};

#define RDS_VIDEO_DEC_SINK_CAPS \
  "video/x-h264, stream-format = (string) byte-stream, alignment = (string) au, parsed = (boolean) true; " \
  "video/x-h265, stream-format = (string) byte-stream, alignment = (string) au, parsed = (boolean) true"
#define RDS_VIDEO_DEC_SRC_FIELDS \
  "format = (string) NV12, width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], " \
  "framerate = (fraction) [ 0/1, 2147483647/1 ]"
#define RDS_VIDEO_DEC_SRC_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), " RDS_VIDEO_DEC_SRC_FIELDS "; video/x-raw, " RDS_VIDEO_DEC_SRC_FIELDS

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_VIDEO_DEC_SINK_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_VIDEO_DEC_SRC_CAPS));

G_DEFINE_TYPE (GstRdsVideoDec, gst_rds_video_dec, GST_TYPE_VIDEO_DECODER);
/* Above vah264dec/vah265dec (GST_RANK_PRIMARY + 1), so autoplugging picks rocDecode, as nvv4l2decoder on DeepStream. */
GST_ELEMENT_REGISTER_DEFINE (rdsvideodec, "rdsvideodec", GST_RANK_PRIMARY + 10, GST_TYPE_RDS_VIDEO_DEC);

/* ---- properties ---- */

static void
gst_rds_video_dec_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_GPU_ID:
      self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_CUDADEC_MEMTYPE:
      self->cudadec_memtype = g_value_get_enum (value);
      break;
    case PROP_NUM_EXTRA_SURFACES:
      self->num_extra_surfaces = g_value_get_uint (value);
      break;
    case PROP_DROP_FRAME_INTERVAL:
      self->drop_frame_interval = g_value_get_uint (value);
      break;
    case PROP_SKIP_FRAMES:
      self->skip_frames = g_value_get_enum (value);
      if (self->skip_frames == GST_RDS_SKIP_FRAMES_DECODE_NON_REF && GST_STATE (self) > GST_STATE_READY)
        GST_WARNING_OBJECT (self, "skip-frames=decode_non_ref is Jetson-only on DeepStream; decoding every frame");
      break;
    case PROP_LOW_LATENCY_MODE:
      self->low_latency_mode = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_video_dec_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_CUDADEC_MEMTYPE:
      g_value_set_enum (value, self->cudadec_memtype);
      break;
    case PROP_NUM_EXTRA_SURFACES:
      g_value_set_uint (value, self->num_extra_surfaces);
      break;
    case PROP_DROP_FRAME_INTERVAL:
      g_value_set_uint (value, self->drop_frame_interval);
      break;
    case PROP_SKIP_FRAMES:
      g_value_set_enum (value, self->skip_frames);
      break;
    case PROP_LOW_LATENCY_MODE:
      g_value_set_boolean (value, self->low_latency_mode);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

/* ---- rocDecode objects ---- */

static void
destroy_parser (GstRdsVideoDec * self)
{
  if (self->parser != NULL) {
    rocDecStatus st = rocDecDestroyVideoParser (self->parser);
    if (st != ROCDEC_SUCCESS)
      GST_WARNING_OBJECT (self, "rocDecDestroyVideoParser: %s", rocDecGetErrorName (st));
    self->parser = NULL;
  }
}

static void
destroy_decoder (GstRdsVideoDec * self)
{
  if (self->decoder != NULL) {
    rocDecStatus st = rocDecDestroyDecoder (self->decoder);
    if (st != ROCDEC_SUCCESS)
      GST_WARNING_OBJECT (self, "rocDecDestroyDecoder: %s", rocDecGetErrorName (st));
    self->decoder = NULL;
  }
  memset (&self->format, 0, sizeof (self->format));
  self->max_width = self->max_height = 0;
  self->num_surfaces = 0;
}

static guint
decode_surfaces_for (GstRdsVideoDec * self, const RocdecVideoFormat * fmt)
{
  return (guint) fmt->min_num_decode_surfaces + EXTRA_DECODE_SURFACES + self->extra_surfaces;
}

/* The decoder can keep decoding @fmt if it was created for the same coded and display geometry. */
static gboolean
decoder_matches (GstRdsVideoDec * self, const RocdecVideoFormat * fmt)
{
  const RocdecVideoFormat *cur = &self->format;

  return self->decoder != NULL && cur->codec == fmt->codec && cur->coded_width == fmt->coded_width
      && cur->coded_height == fmt->coded_height && cur->chroma_format == fmt->chroma_format
      && cur->bit_depth_luma_minus8 == fmt->bit_depth_luma_minus8
      && cur->display_area.left == fmt->display_area.left && cur->display_area.top == fmt->display_area.top
      && cur->display_area.right == fmt->display_area.right
      && cur->display_area.bottom == fmt->display_area.bottom
      && self->num_surfaces >= decode_surfaces_for (self, fmt);
}

/* Works around lost first pictures on small surfaces (seen with rocDecode 1.10 on Mesa 25.2 radeonsi). rocDecode
 * exports a VA surface to HIP lazily, on the first rocDecGetVideoFrame of that surface. Mesa's amdgpu winsys
 * slab-suballocates buffers of 1 MiB or less, and exporting a suballocated texture (vaExportSurfaceHandle ->
 * si_texture_get_handle) moves it into a buffer of its own without its content. So for NV12 surfaces of at most
 * 1 MiB (e.g. 320x240), the picture decoded before the first export comes out as zeros; later pictures in that
 * surface are fine. Mapping every surface once before anything is decoded forces the export (and the move) up
 * front. These calls report an error, since nothing is decoded yet, but the export they trigger is kept. For larger
 * surfaces the export moves nothing and this is harmless. The proper fix is for rocDecode to create its surfaces
 * with VA_SURFACE_ATTRIB_USAGE_HINT_EXPORT. rocDecReconfigureDecoder recreates the surfaces, so this runs again
 * after it. */
static void
premap_surfaces (GstRdsVideoDec * self)
{
  for (guint i = 0; i < self->num_surfaces; i++) {
    void *planes[3] = { NULL, NULL, NULL };
    uint32_t pitch[3] = { 0, 0, 0 };
    RocdecProcParams proc;
    rocDecStatus st;

    memset (&proc, 0, sizeof (proc));
    proc.progressive_frame = 1;
    st = rocDecGetVideoFrame (self->decoder, (int) i, planes, pitch, &proc);
    GST_LOG_OBJECT (self, "pre-mapped surface %u: %s, %p", i, rocDecGetErrorName (st), planes[0]);
  }
}

static gboolean
create_decoder (GstRdsVideoDec * self, const RocdecVideoFormat * fmt, guint width, guint height)
{
  RocDecoderCreateInfo ci;
  guint surfaces = decode_surfaces_for (self, fmt);
  rocDecStatus st;

  memset (&ci, 0, sizeof (ci));
  ci.device_id = (uint8_t) self->stream_gpu;
  ci.width = ci.max_width = fmt->coded_width;
  ci.height = ci.max_height = fmt->coded_height;
  ci.num_decode_surfaces = surfaces;
  ci.codec_type = fmt->codec;
  ci.chroma_format = fmt->chroma_format;
  ci.bit_depth_minus_8 = fmt->bit_depth_luma_minus8;
  /* the decoder crops: its output is the display area */
  ci.display_rect.left = (int16_t) fmt->display_area.left;
  ci.display_rect.top = (int16_t) fmt->display_area.top;
  ci.display_rect.right = (int16_t) fmt->display_area.right;
  ci.display_rect.bottom = (int16_t) fmt->display_area.bottom;
  ci.output_format = rocDecVideoSurfaceFormat_NV12;
  ci.target_width = width;
  ci.target_height = height;
  ci.num_output_surfaces = 1;
  st = rocDecCreateDecoder (&self->decoder, &ci);
  if (st != ROCDEC_SUCCESS) {
    self->decoder = NULL;
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot create a rocDecode decoder"),
        ("rocDecCreateDecoder (%ux%u coded, %ux%u display, %u surfaces, GPU %u): %s", fmt->coded_width,
            fmt->coded_height, width, height, surfaces, self->stream_gpu, rocDecGetErrorName (st)));
    return FALSE;
  }
  self->format = *fmt;
  self->max_width = fmt->coded_width;
  self->max_height = fmt->coded_height;
  self->num_surfaces = surfaces;
  premap_surfaces (self);
  GST_INFO_OBJECT (self, "decoder: coded %ux%u, display %ux%u, %u decode surfaces", fmt->coded_width,
      fmt->coded_height, width, height, surfaces);
  return TRUE;
}

/* Switches the existing decoder to the new sequence @fmt in place. FALSE if it can't (other codec, chroma format
 * or bit depth, a larger coded size than at creation, or rocDecode refuses): the caller recreates it. */
static gboolean
reconfigure_decoder (GstRdsVideoDec * self, const RocdecVideoFormat * fmt, guint width, guint height)
{
  RocdecReconfigureDecoderInfo ri;
  guint surfaces = decode_surfaces_for (self, fmt);
  rocDecStatus st;

  if (self->decoder == NULL || self->format.codec != fmt->codec || self->format.chroma_format != fmt->chroma_format
      || self->format.bit_depth_luma_minus8 != fmt->bit_depth_luma_minus8
      || fmt->coded_width > self->max_width || fmt->coded_height > self->max_height)
    return FALSE;

  memset (&ri, 0, sizeof (ri));
  ri.width = fmt->coded_width;
  ri.height = fmt->coded_height;
  ri.target_width = width;
  ri.target_height = height;
  ri.num_decode_surfaces = surfaces;
  ri.bit_depth_minus_8 = fmt->bit_depth_luma_minus8;
  ri.display_rect.left = (int16_t) fmt->display_area.left;
  ri.display_rect.top = (int16_t) fmt->display_area.top;
  ri.display_rect.right = (int16_t) fmt->display_area.right;
  ri.display_rect.bottom = (int16_t) fmt->display_area.bottom;
  st = rocDecReconfigureDecoder (self->decoder, &ri);
  if (st != ROCDEC_SUCCESS) {
    GST_INFO_OBJECT (self, "rocDecReconfigureDecoder (%ux%u coded): %s, recreating the decoder", fmt->coded_width,
        fmt->coded_height, rocDecGetErrorName (st));
    return FALSE;
  }
  self->format = *fmt;
  self->num_surfaces = surfaces;
  /* the surfaces are new: export them before anything is decoded into them */
  premap_surfaces (self);
  GST_INFO_OBJECT (self, "decoder reconfigured: coded %ux%u, display %ux%u, %u decode surfaces",
      fmt->coded_width, fmt->coded_height, width, height, surfaces);
  return TRUE;
}

/* negotiate vfunc: picks the output memory again on every negotiation (memory:RDS when downstream takes it, else
 * system memory), so a RECONFIGURE from a new downstream peer can move the output between the two. That happens
 * under decodebin: it exposes and links the decoded pad only after the first caps, so the first choice was made
 * against an unlinked pad that accepts anything. */
static gboolean
gst_rds_video_dec_negotiate (GstVideoDecoder * dec)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (dec);
  GstVideoDecoderClass *parent = GST_VIDEO_DECODER_CLASS (gst_rds_video_dec_parent_class);
  GstVideoCodecState *state;
  GstCaps *rds_caps, *peer, *prev_caps, *prev_allocation_caps;
  gboolean rds, ok;

  if (self->finishing) {
    /* finish_frame saw a RECONFIGURE after on_display checked for one: the buffer it is about to push was filled
     * for the current caps, so keep them, and let the next allocation choose again. Safe because only a
     * RECONFIGURE can bring us here: the base class's output_state_changed is never set at this point, since
     * set_output_state always negotiates right away and a failure there never reaches finish_frame. */
    GST_DEBUG_OBJECT (self, "RECONFIGURE while finishing a frame: renegotiating with the next frame");
    gst_pad_mark_reconfigure (GST_VIDEO_DECODER_SRC_PAD (dec));
    return TRUE;
  }
  state = gst_video_decoder_get_output_state (dec);
  if (state == NULL)
    return parent->negotiate (dec);

  rds_caps = gst_video_info_to_caps (&state->info);
  gst_caps_set_features_simple (rds_caps, gst_caps_features_new_static_str (RDS_CAPS_FEATURE_MEMORY_RDS, NULL));
  peer = gst_pad_peer_query_caps (GST_VIDEO_DECODER_SRC_PAD (dec), rds_caps);
  rds = !gst_caps_is_empty (peer);
  gst_caps_unref (peer);
  /* A peer may list memory:RDS in its caps and still refuse the fixed caps. Ask, because pushing caps never says:
   * gst_pad_set_caps reports success for a refused sticky event, and only the first buffer fails (not-negotiated).
   * An unlinked pad can't answer; the RECONFIGURE that comes with the link makes us choose again. */
  if (rds && gst_pad_is_linked (GST_VIDEO_DECODER_SRC_PAD (dec)))
    rds = gst_pad_peer_query_accept_caps (GST_VIDEO_DECODER_SRC_PAD (dec), rds_caps);
  /* The state's caps are rewritten in place, without the object lock, as the base class does in its own negotiate:
   * every writer of the output state holds the stream lock, which all negotiation runs under. */
  prev_caps = g_steal_pointer (&state->caps);
  prev_allocation_caps = g_steal_pointer (&state->allocation_caps);
  if (rds)
    state->caps = rds_caps;
  else
    gst_caps_unref (rds_caps);  /* the base class derives system-memory caps from the info */
  ok = parent->negotiate (dec);
  if (!ok && rds) {
    /* downstream accepted memory:RDS but the allocation failed (no output pool): fall back to system memory */
    GST_INFO_OBJECT (dec, "memory:RDS output did not negotiate, retrying with system memory");
    gst_clear_caps (&state->caps);
    gst_clear_caps (&state->allocation_caps);
    ok = parent->negotiate (dec);
  }
  GST_DEBUG_OBJECT (dec, "output caps %" GST_PTR_FORMAT ": %s", state->caps, ok ? "negotiated" : "failed");
  if (ok) {
    gst_clear_caps (&prev_caps);
    gst_clear_caps (&prev_allocation_caps);
  } else {
    /* Restore the caps the output state had before this attempt: buffers in flight were filled for them. They are
     * usually the pad's current caps too, but not always: if the memory:RDS caps event went through, its pool
     * failed, and the system-memory retry failed at its pool step as well, the pad already holds the newer caps.
     * A later successful negotiation sets the pad's caps again. */
    gst_clear_caps (&state->caps);
    gst_clear_caps (&state->allocation_caps);
    state->caps = prev_caps;
    state->allocation_caps = prev_allocation_caps;
  }
  gst_video_codec_state_unref (state);
  return ok;
}

/* New output state for a @width x @height display size, negotiated right away. */
static gboolean
set_output_state (GstRdsVideoDec * self, guint width, guint height)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  GstVideoCodecState *state;
  gboolean ok;

  state = gst_video_decoder_set_output_state (dec, GST_VIDEO_FORMAT_NV12, width, height, self->input_state);
  if (state == NULL)
    return FALSE;
  gst_video_codec_state_unref (state);
  self->out_width = width;
  self->out_height = height;
  self->output_dirty = FALSE;
  /* Pushing the caps can get the pad linked to a new peer (decodebin exposes it then), which sends RECONFIGURE:
   * the allocation of the first output frame then negotiates again, before any buffer is filled. */
  ok = gst_video_decoder_negotiate (dec);
  return ok;
}

/* TRUE if the current output caps carry memory:RDS (so newly allocated output buffers do too). */
static gboolean
output_is_rds (GstRdsVideoDec * self)
{
  GstVideoCodecState *state = gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  gboolean rds = state != NULL && state->caps != NULL && rds_caps_has_memory_rds (state->caps);

  if (state != NULL)
    gst_video_codec_state_unref (state);
  return rds;
}

/* ---- parser callbacks (called from rocDecParseVideoData, under the stream lock) ---- */

static int ROCDECAPI
on_sequence (void *user_data, RocdecVideoFormat * fmt)
{
  GstRdsVideoDec *self = user_data;
  guint width = (guint) (fmt->display_area.right - fmt->display_area.left);
  guint height = (guint) (fmt->display_area.bottom - fmt->display_area.top);

  GST_DEBUG_OBJECT (self, "sequence: codec %d, coded %ux%u, display %ux%u at %d,%d, chroma %d, bit depth %u, "
      "min %u decode surfaces", fmt->codec, fmt->coded_width, fmt->coded_height, width, height,
      fmt->display_area.left, fmt->display_area.top, fmt->chroma_format, fmt->bit_depth_luma_minus8 + 8,
      fmt->min_num_decode_surfaces);

  if (fmt->bit_depth_luma_minus8 != 0 || fmt->bit_depth_chroma_minus8 != 0) {
    GST_ELEMENT_ERROR (self, STREAM, DECODE, ("Unsupported bit depth %u (only 8-bit video is supported)",
            fmt->bit_depth_luma_minus8 + 8), (NULL));
    self->cb_flow = GST_FLOW_ERROR;
    return 0;
  }
  if (fmt->chroma_format != rocDecVideoChromaFormat_420) {
    GST_ELEMENT_ERROR (self, STREAM, DECODE, ("Unsupported chroma format %d (only 4:2:0 is supported)",
            fmt->chroma_format), (NULL));
    self->cb_flow = GST_FLOW_ERROR;
    return 0;
  }
  if (width == 0 || height == 0 || fmt->display_area.left < 0 || fmt->display_area.top < 0) {
    GST_ELEMENT_ERROR (self, STREAM, DECODE, ("Invalid display area"), ("%d,%d - %d,%d",
            fmt->display_area.left, fmt->display_area.top, fmt->display_area.right, fmt->display_area.bottom));
    self->cb_flow = GST_FLOW_ERROR;
    return 0;
  }

  if (decoder_matches (self, fmt))
    return (int) self->num_surfaces;

  /* A new or changed sequence. The parser has displayed the pictures of the previous one by now. */
  if (!reconfigure_decoder (self, fmt, width, height)) {
    destroy_decoder (self);
    if (!create_decoder (self, fmt, width, height)) {
      self->cb_flow = GST_FLOW_ERROR;
      return 0;
    }
  }
  if (width != self->out_width || height != self->out_height || self->output_dirty
      || !gst_pad_has_current_caps (GST_VIDEO_DECODER_SRC_PAD (self))) {
    if (!set_output_state (self, width, height)) {
      GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("Cannot negotiate %ux%u NV12 output", width, height),
          (NULL));
      self->cb_flow = GST_FLOW_NOT_NEGOTIATED;
      return 0;
    }
  }
  return (int) self->num_surfaces;
}

static int ROCDECAPI
on_decode (void *user_data, RocdecPicParams * pic)
{
  GstRdsVideoDec *self = user_data;
  rocDecStatus st;

  if (self->decoder == NULL)
    return 0;
  GST_LOG_OBJECT (self, "decoding picture %d", pic->curr_pic_idx);
  st = rocDecDecodeFrame (self->decoder, pic);
  if (st != ROCDEC_SUCCESS) {
    GST_WARNING_OBJECT (self, "rocDecDecodeFrame: %s", rocDecGetErrorName (st));
    self->cb_decode_error = TRUE;
    self->cb_decode_failed = TRUE;
    return 0;
  }
  return 1;
}

/* Copies the decoded picture into the output buffer on the element's stream and waits for it (ready-on-push). Row
 * bytes: Y width; UV 2 * ceil(width / 2), over ceil(height / 2) rows. On failure, *@detail says why (free it). */
static gboolean
copy_picture (GstRdsVideoDec * self, GstBuffer * outbuf, void *planes[3], const uint32_t pitch[3], gchar ** detail)
{
  guint w = self->out_width, h = self->out_height;
  gsize row_bytes[2] = { w, 2 * ((w + 1) / 2) }, rows[2] = { h, (h + 1) / 2 };
  const void *src[2] = { planes[0], planes[1] };
  gsize src_pitch[2] = { pitch[0], pitch[1] ? pitch[1] : pitch[0] };
  guint8 *dst[2];
  gsize dst_pitch[2];
  GstMapInfo map;
  GstVideoFrame frame;
  RdsBufSurface *surf = NULL;
  hipError_t err = hipSuccess;

  /* the buffer's memory decides, not the caps: they may have moved on since the buffer was allocated */
  if (rds_buffer_get_surface (outbuf) != NULL) {
    RdsBufSurfaceParams *p;

    if (!gst_buffer_map (outbuf, &map, GST_MAP_WRITE)) {
      *detail = g_strdup ("the output buffer is not a mappable memory:RDS buffer");
      return FALSE;
    }
    surf = (RdsBufSurface *) map.data;
    p = &surf->surfaceList[0];
    if (p->colorFormat != RDS_BUF_COLOR_FORMAT_NV12 || p->width != w || p->height != h) {
      *detail = g_strdup_printf ("the output surface is %ux%u format %d, expected %ux%u NV12", p->width, p->height,
          p->colorFormat, w, h);
      gst_buffer_unmap (outbuf, &map);
      return FALSE;
    }
    for (guint i = 0; i < 2; i++) {
      dst[i] = (guint8 *) p->dataPtr + p->planeParams.offset[i];
      dst_pitch[i] = p->planeParams.pitch[i];
    }
  } else {
    GstVideoCodecState *state = gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
    gboolean ok = gst_video_frame_map (&frame, &state->info, outbuf, GST_MAP_WRITE);

    gst_video_codec_state_unref (state);
    if (!ok) {
      *detail = g_strdup ("cannot map the output frame");
      return FALSE;
    }
    for (guint i = 0; i < 2; i++) {
      dst[i] = GST_VIDEO_FRAME_PLANE_DATA (&frame, i);
      dst_pitch[i] = GST_VIDEO_FRAME_PLANE_STRIDE (&frame, i);
    }
  }

  for (guint i = 0; i < 2 && err == hipSuccess; i++)
    err = hipMemcpy2DAsync (dst[i], dst_pitch[i], src[i], src_pitch[i], row_bytes[i], rows[i], hipMemcpyDefault,
        self->stream);
  if (err == hipSuccess)
    err = hipStreamSynchronize (self->stream);
  else
    (void) hipStreamSynchronize (self->stream);

  if (surf != NULL) {
    /* the producer states what it filled, whatever the pool initialised */
    surf->numFilled = 1;
    gst_buffer_unmap (outbuf, &map);
  } else {
    gst_video_frame_unmap (&frame);
  }
  if (err != hipSuccess) {
    *detail = g_strdup_printf ("hipMemcpy2DAsync/hipStreamSynchronize: %s", hipGetErrorString (err));
    return FALSE;
  }
  return TRUE;
}

/* Allocates @frame's output buffer and copies the decoded picture into it; posts an error for fatal failures. */
static GstFlowReturn
fill_output_frame (GstRdsVideoDec * self, GstVideoCodecFrame * frame, void *planes[3], const uint32_t pitch[3])
{
  GstFlowReturn ret;
  gchar *detail = NULL;

  ret = gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER (self), frame);
  if (ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "cannot allocate an output frame: %s", gst_flow_get_name (ret));
    if (ret == GST_FLOW_ERROR)
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot allocate an output buffer"), (NULL));
    return ret;
  }
  if (!copy_picture (self, frame->output_buffer, planes, pitch, &detail)) {
    /* Not a bitstream problem (those stay tolerated, as GST_VIDEO_DECODER_ERROR): the GPU, the output pool or the
     * negotiated memory is broken, and every later frame would fail the same way. Fatal. */
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot copy the decoded picture into the output buffer"),
        ("%s", detail));
    g_free (detail);
    return GST_FLOW_ERROR;
  }
  return GST_FLOW_OK;
}

static int ROCDECAPI
on_display (void *user_data, RocdecParserDispInfo * info)
{
  GstRdsVideoDec *self = user_data;
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  GstVideoCodecFrame *frame;
  RocdecProcParams proc;
  void *planes[3] = { NULL, NULL, NULL };
  uint32_t pitch[3] = { 0, 0, 0 };
  rocDecStatus st;
  GstFlowReturn ret;
  guint64 index;
  guint interval;

  frame = gst_video_decoder_get_frame (dec, (int) info->pts);
  if (frame == NULL) {
    GST_WARNING_OBJECT (self, "no pending frame %" G_GUINT64_FORMAT " for picture %d", (guint64) info->pts,
        info->picture_index);
    return 1;
  }
  if (self->cb_flow != GST_FLOW_OK) {
    /* downstream already refused a frame in this call: don't push more */
    gst_video_decoder_release_frame (dec, frame);
    return 1;
  }

  GST_OBJECT_LOCK (self);
  interval = self->drop_frame_interval;
  GST_OBJECT_UNLOCK (self);
  index = self->display_count++;
  if (interval > 1 && index % interval != 0) {
    /* drop-frame-interval: only every interval-th displayed picture goes out. Configured drops use drop_frame (not
     * release_frame) so the base class counts them as handled output (had_output_data: no "no valid frames" error at
     * EOS when everything was dropped); the cost is a QoS message per dropped frame. */
    GST_LOG_OBJECT (self, "drop-frame-interval %u: dropping displayed picture %" G_GUINT64_FORMAT, interval, index);
    gst_video_decoder_drop_frame (dec, frame);
    return 1;
  }

  if (self->output_dirty && self->out_width > 0 && !set_output_state (self, self->out_width, self->out_height)) {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("Cannot negotiate %ux%u NV12 output", self->out_width,
            self->out_height), (NULL));
    self->cb_flow = GST_FLOW_NOT_NEGOTIATED;
    gst_video_decoder_release_frame (dec, frame);
    return 1;
  }

  memset (&proc, 0, sizeof (proc));
  proc.progressive_frame = info->progressive_frame;
  proc.top_field_first = info->top_field_first;
  st = rocDecGetVideoFrame (self->decoder, info->picture_index, planes, pitch, &proc);
  if (st != ROCDEC_SUCCESS || planes[0] == NULL || planes[1] == NULL) {
    GST_WARNING_OBJECT (self, "rocDecGetVideoFrame (picture %d): %s", info->picture_index,
        rocDecGetErrorName (st));
    self->cb_decode_error = TRUE;
    gst_video_decoder_drop_frame (dec, frame);
    return 1;
  }

  ret = fill_output_frame (self, frame, planes, pitch);
  if (ret == GST_FLOW_OK && gst_pad_check_reconfigure (GST_VIDEO_DECODER_SRC_PAD (dec))) {
    /* A RECONFIGURE arrived after the allocation: negotiate now rather than in finish_frame, which would push this
     * buffer with caps chosen after it was filled. If the memory type changed, fill a new buffer. */
    if (!gst_video_decoder_negotiate (dec))
      ret = GST_PAD_IS_FLUSHING (GST_VIDEO_DECODER_SRC_PAD (dec)) ? GST_FLOW_FLUSHING : GST_FLOW_NOT_NEGOTIATED;
    else if ((rds_buffer_get_surface (frame->output_buffer) != NULL) != output_is_rds (self)) {
      GST_DEBUG_OBJECT (self, "output memory changed by a RECONFIGURE: refilling the frame");
      gst_clear_buffer (&frame->output_buffer);
      ret = fill_output_frame (self, frame, planes, pitch);
    }
  }
  if (ret != GST_FLOW_OK) {
    self->cb_flow = ret;
    gst_video_decoder_release_frame (dec, frame);
    return 1;
  }
  self->finishing = TRUE;
  ret = gst_video_decoder_finish_frame (dec, frame);
  self->finishing = FALSE;
  if (ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "finish_frame: %s", gst_flow_get_name (ret));
    self->cb_flow = ret;
  }
  return 1;
}

static gboolean
create_parser (GstRdsVideoDec * self)
{
  RocdecParserParams pp;
  rocDecStatus st;

  memset (&pp, 0, sizeof (pp));
  pp.codec_type = self->codec;
  pp.max_num_decode_surfaces = 1;       /* raised by on_sequence's return value */
  pp.max_display_delay = 0;
  pp.user_data = self;
  pp.pfn_sequence_callback = on_sequence;
  pp.pfn_decode_picture = on_decode;
  pp.pfn_display_picture = on_display;
  st = rocDecCreateVideoParser (&self->parser, &pp);
  if (st != ROCDEC_SUCCESS) {
    self->parser = NULL;
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot create a rocDecode parser"),
        ("rocDecCreateVideoParser: %s", rocDecGetErrorName (st)));
    return FALSE;
  }
  return TRUE;
}

/* Feeds one packet to the parser and turns what the callbacks reported into a flow return. */
static GstFlowReturn
parse_packet (GstRdsVideoDec * self, RocdecSourceDataPacket * pkt)
{
  GstFlowReturn ret = GST_FLOW_OK;
  rocDecStatus st;

  self->cb_flow = GST_FLOW_OK;
  self->cb_decode_error = FALSE;
  self->cb_decode_failed = FALSE;
  st = rocDecParseVideoData (self->parser, pkt);
  if (self->cb_flow != GST_FLOW_OK)
    return self->cb_flow;
  if (st != ROCDEC_SUCCESS || self->cb_decode_error) {
    /* tolerate isolated errors, as other decoders do */
    GST_VIDEO_DECODER_ERROR (self, 1, STREAM, DECODE, ("Decoding failed"),
        ("rocDecParseVideoData: %s%s", rocDecGetErrorName (st),
            self->cb_decode_error ? ", a picture failed to decode" : ""), ret);
  }
  return ret;
}

/* Releases the frames still pending in the base class: once the parser is drained, it will never display them.
 * A backstop that no test reaches: frames whose decode fails are already dropped in handle_frame, and h264parse /
 * h265parse drop slices that arrive before any parameter set. */
static void
release_pending_frames (GstRdsVideoDec * self)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  GList *frames = gst_video_decoder_get_frames (dec);

  for (GList * l = frames; l != NULL; l = l->next) {
    GstVideoCodecFrame *frame = l->data;

    GST_DEBUG_OBJECT (self, "frame %u was never displayed, releasing it", frame->system_frame_number);
    gst_video_decoder_release_frame (dec, gst_video_codec_frame_ref (frame));
  }
  g_list_free_full (frames, (GDestroyNotify) gst_video_codec_frame_unref);
}

/* Sends an end-of-stream packet so every pending picture is displayed, releases the frames that were not, then
 * drops the parser (recreated lazily on the next frame; the decoder is kept for a matching sequence). */
static GstFlowReturn
drain_parser (GstRdsVideoDec * self)
{
  RocdecSourceDataPacket pkt;
  GstFlowReturn ret;

  if (self->parser == NULL)
    return GST_FLOW_OK;
  memset (&pkt, 0, sizeof (pkt));
  pkt.flags = ROCDEC_PKT_ENDOFSTREAM;
  ret = parse_packet (self, &pkt);
  release_pending_frames (self);
  destroy_parser (self);
  return ret;
}

/* ---- GstVideoDecoder ---- */

static gboolean
gst_rds_video_dec_start (GstVideoDecoder * dec)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (dec);
  int n = 0, prev = 0;
  guint gpu_id;
  GstRdsCudaDecMemType memtype;
  GstRdsSkipFrames skip;
  hipError_t err;

  GST_OBJECT_LOCK (self);
  gpu_id = self->gpu_id;
  memtype = self->cudadec_memtype;
  self->extra_surfaces = self->num_extra_surfaces;
  skip = self->skip_frames;
  GST_OBJECT_UNLOCK (self);

  switch (memtype) {
    case GST_RDS_CUDADEC_MEMTYPE_PINNED:
      self->mem_type = RDS_BUF_MEM_HIP_PINNED;
      break;
    case GST_RDS_CUDADEC_MEMTYPE_UNIFIED:
      self->mem_type = RDS_BUF_MEM_HIP_UNIFIED;
      break;
    case GST_RDS_CUDADEC_MEMTYPE_DEVICE:
    default:
      self->mem_type = RDS_BUF_MEM_HIP_DEVICE;
      break;
  }
  if (skip == GST_RDS_SKIP_FRAMES_DECODE_NON_REF)
    GST_WARNING_OBJECT (self, "skip-frames=decode_non_ref is Jetson-only on DeepStream; decoding every frame");

  if (hipGetDeviceCount (&n) != hipSuccess || gpu_id >= (guint) n) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("GPU %u is not available", gpu_id),
        ("%d ROCm device(s) visible", n));
    return FALSE;
  }
  if (hipGetDevice (&prev) != hipSuccess || hipSetDevice ((int) gpu_id) != hipSuccess) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("GPU %u is not available", gpu_id), (NULL));
    return FALSE;
  }
  /* Non-blocking, as in rdsvideoconvert: decoders in one process must not serialize against the legacy null
   * stream. The output copies synchronize this stream before the frame is finished (ready-on-push). */
  err = hipStreamCreateWithFlags (&self->stream, hipStreamNonBlocking);
  (void) hipSetDevice (prev);
  if (err != hipSuccess) {
    self->stream = NULL;
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot create a HIP stream on GPU %u", gpu_id),
        ("hipStreamCreateWithFlags: %s", hipGetErrorString (err)));
    return FALSE;
  }
  self->stream_gpu = gpu_id;
  self->out_width = self->out_height = 0;
  self->output_dirty = FALSE;
  self->display_count = 0;
  self->last_skip_frames = skip;
  self->wait_sync = FALSE;
  return TRUE;
}

static gboolean
gst_rds_video_dec_stop (GstVideoDecoder * dec)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (dec);

  destroy_parser (self);
  destroy_decoder (self);
  if (self->stream != NULL) {
    if (hipStreamDestroy (self->stream) != hipSuccess)
      GST_WARNING_OBJECT (self, "hipStreamDestroy failed");
    self->stream = NULL;
  }
  g_clear_pointer (&self->input_state, gst_video_codec_state_unref);
  self->out_width = self->out_height = 0;
  return TRUE;
}

static gboolean
gst_rds_video_dec_set_format (GstVideoDecoder * dec, GstVideoCodecState * state)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (dec);
  GstStructure *s = gst_caps_get_structure (state->caps, 0);
  rocDecVideoCodec codec;

  if (gst_structure_has_name (s, "video/x-h264")) {
    codec = rocDecVideoCodec_AVC;
  } else if (gst_structure_has_name (s, "video/x-h265")) {
    codec = rocDecVideoCodec_HEVC;
  } else {
    GST_ERROR_OBJECT (self, "unsupported caps %" GST_PTR_FORMAT, state->caps);
    return FALSE;
  }
  if (self->parser != NULL && codec != self->codec) {
    /* a new codec needs a new parser; pending pictures of the old stream are shown first */
    (void) drain_parser (self);
  }
  self->codec = codec;
  if (self->input_state != NULL && self->out_width > 0
      && (GST_VIDEO_INFO_FPS_N (&self->input_state->info) != GST_VIDEO_INFO_FPS_N (&state->info)
          || GST_VIDEO_INFO_FPS_D (&self->input_state->info) != GST_VIDEO_INFO_FPS_D (&state->info)
          || GST_VIDEO_INFO_PAR_N (&self->input_state->info) != GST_VIDEO_INFO_PAR_N (&state->info)
          || GST_VIDEO_INFO_PAR_D (&self->input_state->info) != GST_VIDEO_INFO_PAR_D (&state->info))) {
    /* the output caps take framerate and pixel aspect ratio from the input: renew them with the next picture
     * (a size change renews them anyway, in on_sequence) */
    GST_DEBUG_OBJECT (self, "input framerate or pixel aspect ratio changed");
    self->output_dirty = TRUE;
  }
  g_clear_pointer (&self->input_state, gst_video_codec_state_unref);
  self->input_state = gst_video_codec_state_ref (state);
  return TRUE;
}

static GstFlowReturn
gst_rds_video_dec_handle_frame (GstVideoDecoder * dec, GstVideoCodecFrame * frame)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (dec);
  RocdecSourceDataPacket pkt;
  GstMapInfo map;
  GstFlowReturn ret;
  GstRdsSkipFrames skip;

  GST_OBJECT_LOCK (self);
  skip = self->skip_frames;
  GST_OBJECT_UNLOCK (self);
  if (skip != self->last_skip_frames) {
    /* leaving decode_key: the next delta frames may reference pictures that were skipped */
    if (self->last_skip_frames == GST_RDS_SKIP_FRAMES_DECODE_KEY)
      self->wait_sync = TRUE;
    self->last_skip_frames = skip;
  }
  if (GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame)) {
    self->wait_sync = FALSE;
  } else if (skip == GST_RDS_SKIP_FRAMES_DECODE_KEY || self->wait_sync) {
    /* decode_key: delta frames never reach the parser. After a skipped delta frame, later delta frames (even with
     * decode_all again) would decode against missing references, which rocDecode does not report: wait for the next
     * key frame. */
    GST_LOG_OBJECT (self, "skip-frames: dropping delta frame %u%s", frame->system_frame_number,
        skip == GST_RDS_SKIP_FRAMES_DECODE_KEY ? "" : " (waiting for a key frame)");
    self->wait_sync = TRUE;
    return gst_video_decoder_drop_frame (dec, frame);
  }

  if (self->parser == NULL && !create_parser (self)) {
    gst_video_decoder_drop_frame (dec, frame);
    return GST_FLOW_ERROR;
  }
  if (!gst_buffer_map (frame->input_buffer, &map, GST_MAP_READ)) {
    GST_ELEMENT_ERROR (self, STREAM, DECODE, ("Cannot map the input buffer"), (NULL));
    gst_video_decoder_drop_frame (dec, frame);
    return GST_FLOW_ERROR;
  }
  memset (&pkt, 0, sizeof (pkt));
  pkt.payload = map.data;
  pkt.payload_size = (uint32_t) map.size;
  /* one whole access unit per packet: END_OF_PICTURE lets the parser decode it within this call */
  pkt.flags = ROCDEC_PKT_TIMESTAMP | ROCDEC_PKT_ENDOFPICTURE;
  if (GST_BUFFER_FLAG_IS_SET (frame->input_buffer, GST_BUFFER_FLAG_DISCONT))
    pkt.flags |= ROCDEC_PKT_DISCONTINUITY;
  /* on_display finds the frame again by this number (the parser hands it back in display order) */
  pkt.pts = frame->system_frame_number;
  GST_LOG_OBJECT (self, "parsing frame %u (%" G_GSIZE_FORMAT " bytes)", frame->system_frame_number, map.size);
  ret = parse_packet (self, &pkt);
  gst_buffer_unmap (frame->input_buffer, &map);
  if (self->cb_decode_failed) {
    /* Packets carry ROCDEC_PKT_ENDOFPICTURE, so the parser decodes a packet's picture within the same call: the
     * picture that failed is this frame's (e.g. a P-frame whose references are missing at a mid-GOP start). The
     * parser never displays it: drop it now rather than leaving it pending until the next drain. */
    GstVideoCodecFrame *pending = gst_video_decoder_get_frame (dec, (int) frame->system_frame_number);

    if (pending != NULL) {
      GST_DEBUG_OBJECT (self, "frame %u failed to decode, dropping it", frame->system_frame_number);
      gst_video_decoder_drop_frame (dec, pending);
    }
  }
  /* otherwise the frame stays in the base class's list until on_display finishes it */
  gst_video_codec_frame_unref (frame);
  return ret;
}

static GstFlowReturn
gst_rds_video_dec_drain (GstVideoDecoder * dec)
{
  return drain_parser (GST_RDS_VIDEO_DEC (dec));
}

static GstFlowReturn
gst_rds_video_dec_finish (GstVideoDecoder * dec)
{
  return drain_parser (GST_RDS_VIDEO_DEC (dec));
}

static gboolean
gst_rds_video_dec_flush (GstVideoDecoder * dec)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (dec);

  /* drop stale reference pictures: both are recreated by the next frame (the base class drops pending frames) */
  destroy_parser (self);
  destroy_decoder (self);
  self->display_count = 0;
  return TRUE;
}

static gboolean
gst_rds_video_dec_decide_allocation (GstVideoDecoder * dec, GstQuery * query)
{
  GstRdsVideoDec *self = GST_RDS_VIDEO_DEC (dec);
  GstCaps *caps = NULL;
  GstBufferPool *pool;

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps == NULL)
    return FALSE;
  if (!rds_caps_has_memory_rds (caps))
    return GST_VIDEO_DECODER_CLASS (gst_rds_video_dec_parent_class)->decide_allocation (dec, query);

  /* writes the pool into the query, where the base class picks it up */
  pool = rds_decide_allocation (query, 1, self->mem_type, self->stream_gpu, OUTPUT_MIN_BUFFERS, 0);
  if (pool == NULL) {
    GST_ERROR_OBJECT (self, "cannot set up a memory:RDS output pool for %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
  gst_object_unref (pool);
  return TRUE;
}

static void
gst_rds_video_dec_class_init (GstRdsVideoDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *dec_class = GST_VIDEO_DECODER_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY;

  GST_DEBUG_CATEGORY_INIT (gst_rds_video_dec_debug, "rdsvideodec", 0, "Redstream rocDecode video decoder");
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_CUDADEC_MEMTYPE, 0);
  gst_type_mark_as_plugin_api (GST_TYPE_RDS_SKIP_FRAMES, 0);

  gobject_class->set_property = gst_rds_video_dec_set_property;
  gobject_class->get_property = gst_rds_video_dec_get_property;

  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU ID", "ROCm device to decode on", 0, G_MAXUINT, DEFAULT_GPU_ID, flags));
  g_object_class_install_property (gobject_class, PROP_CUDADEC_MEMTYPE,
      g_param_spec_enum ("cudadec-memtype", "Output memory type",
          "Memory of the memory:RDS output surfaces (DeepStream's default is unified; ours is device memory)",
          GST_TYPE_RDS_CUDADEC_MEMTYPE, DEFAULT_CUDADEC_MEMTYPE, flags));
  g_object_class_install_property (gobject_class, PROP_NUM_EXTRA_SURFACES,
      g_param_spec_uint ("num-extra-surfaces", "Extra decode surfaces",
          "Decode surfaces on top of what the stream needs", 0, MAX_NUM_EXTRA_SURFACES,
          DEFAULT_NUM_EXTRA_SURFACES, flags));
  g_object_class_install_property (gobject_class, PROP_DROP_FRAME_INTERVAL,
      g_param_spec_uint ("drop-frame-interval", "Drop frame interval",
          "Output only every N-th decoded frame (0 or 1: every frame)", 0, MAX_DROP_FRAME_INTERVAL,
          DEFAULT_DROP_FRAME_INTERVAL, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property (gobject_class, PROP_SKIP_FRAMES,
      g_param_spec_enum ("skip-frames", "Skip frames", "Which frames to decode", GST_TYPE_RDS_SKIP_FRAMES,
          DEFAULT_SKIP_FRAMES, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));
  g_object_class_install_property (gobject_class, PROP_LOW_LATENCY_MODE,
      g_param_spec_boolean ("low-latency-mode", "Low latency mode",
          "Accepted for compatibility: pictures are always output as soon as the stream order allows",
          DEFAULT_LOW_LATENCY_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream video decoder",
      "Codec/Decoder/Video/Hardware", "Decodes H.264 and H.265 with rocDecode into memory:RDS (HIP) or system memory",
      "Redstream contributors");

  dec_class->start = GST_DEBUG_FUNCPTR (gst_rds_video_dec_start);
  dec_class->stop = GST_DEBUG_FUNCPTR (gst_rds_video_dec_stop);
  dec_class->set_format = GST_DEBUG_FUNCPTR (gst_rds_video_dec_set_format);
  dec_class->handle_frame = GST_DEBUG_FUNCPTR (gst_rds_video_dec_handle_frame);
  dec_class->drain = GST_DEBUG_FUNCPTR (gst_rds_video_dec_drain);
  dec_class->finish = GST_DEBUG_FUNCPTR (gst_rds_video_dec_finish);
  dec_class->flush = GST_DEBUG_FUNCPTR (gst_rds_video_dec_flush);
  dec_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_rds_video_dec_decide_allocation);
  dec_class->negotiate = GST_DEBUG_FUNCPTR (gst_rds_video_dec_negotiate);
}

static void
gst_rds_video_dec_init (GstRdsVideoDec * self)
{
  self->gpu_id = DEFAULT_GPU_ID;
  self->cudadec_memtype = DEFAULT_CUDADEC_MEMTYPE;
  self->num_extra_surfaces = DEFAULT_NUM_EXTRA_SURFACES;
  self->drop_frame_interval = DEFAULT_DROP_FRAME_INTERVAL;
  self->skip_frames = DEFAULT_SKIP_FRAMES;
  self->low_latency_mode = DEFAULT_LOW_LATENCY_MODE;
  self->mem_type = RDS_BUF_MEM_HIP_DEVICE;
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
}
