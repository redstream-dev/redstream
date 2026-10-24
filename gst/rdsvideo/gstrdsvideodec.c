/* Redstream: rdsvideodec, H.264 decode with rocDecode into pooled memory:RDS NV12 surfaces (or system memory when
 * downstream cannot take memory:RDS). Property names follow DeepStream's public nvv4l2decoder documentation.
 *
 * Flow: each input frame (one access unit) goes to the rocDecode parser as one packet whose timestamp is the frame's
 * system_frame_number. The parser calls back synchronously from rocDecParseVideoData: on_sequence (create the
 * decoder, set the output state), on_decode (submit a picture) and on_display (in display order: copy the cropped
 * picture into an output buffer, synchronize, finish the frame found by that timestamp).
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
/* Decode surfaces on top of the stream's minimum: headroom for pictures held for display (spike 0a). */
#define EXTRA_DECODE_SURFACES 4
/* Output pool: minimum buffers in flight downstream. */
#define OUTPUT_MIN_BUFFERS 4

enum
{
  PROP_0,
  PROP_GPU_ID,
};

struct _GstRdsVideoDec
{
  GstVideoDecoder parent;

  /* properties (object lock) */
  guint gpu_id;

  /* streaming state (stream lock) */
  guint stream_gpu;             /* the GPU of the stream, the decoder and the output pool, fixed at start */
  RdsBufSurfaceMemType mem_type;        /* output surfaces' memory type */
  hipStream_t stream;
  GstVideoCodecState *input_state;
  rocDecVideoCodec codec;
  RocdecVideoParser parser;     /* created lazily on the first frame */
  rocDecDecoderHandle decoder;  /* created by the first sequence callback */
  RocdecVideoFormat format;     /* the sequence the decoder was created for */
  guint num_surfaces;
  guint out_width, out_height;  /* display size: the decoder's output, already cropped */
  gboolean output_rds;          /* negotiated memory:RDS output */
  /* results of the callbacks during one rocDecParseVideoData call */
  GstFlowReturn cb_flow;        /* non-OK: stop and return it (an error was already posted if needed) */
  gboolean cb_decode_error;     /* a picture failed to decode or to come out */
};

#define RDS_VIDEO_DEC_SINK_CAPS \
  "video/x-h264, stream-format = (string) byte-stream, alignment = (string) au, parsed = (boolean) true"
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
  self->num_surfaces = 0;
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
      && self->num_surfaces >= (guint) fmt->min_num_decode_surfaces + EXTRA_DECODE_SURFACES;
}

/* rocDecode quirk (rocDecode 1.10, ROCm 10.1, Mesa VA on RDNA2): the first rocDecGetVideoFrame of a decode surface
 * maps a picture of zeros even after its decode completed; every later mapping of that surface is correct. Mapping
 * each surface once before anything is decoded into it avoids the bad frame. These calls report an error (there is
 * no decoded picture yet), but the mapping they set up is kept. */
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
  guint surfaces = (guint) fmt->min_num_decode_surfaces + EXTRA_DECODE_SURFACES;
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
  self->num_surfaces = surfaces;
  premap_surfaces (self);
  GST_INFO_OBJECT (self, "decoder: coded %ux%u, display %ux%u, %u decode surfaces", fmt->coded_width,
      fmt->coded_height, width, height, surfaces);
  return TRUE;
}

/* New output caps for a @width x @height display size: memory:RDS when downstream takes it, else system memory. */
static gboolean
set_output_state (GstRdsVideoDec * self, guint width, guint height)
{
  GstVideoDecoder *dec = GST_VIDEO_DECODER (self);
  GstVideoCodecState *state;
  GstCaps *rds_caps, *peer;
  gboolean rds;

  state = gst_video_decoder_set_output_state (dec, GST_VIDEO_FORMAT_NV12, width, height, self->input_state);
  if (state == NULL)
    return FALSE;
  rds_caps = gst_video_info_to_caps (&state->info);
  gst_caps_set_features_simple (rds_caps, gst_caps_features_new_static_str (RDS_CAPS_FEATURE_MEMORY_RDS, NULL));
  peer = gst_pad_peer_query_caps (GST_VIDEO_DECODER_SRC_PAD (dec), rds_caps);
  rds = !gst_caps_is_empty (peer);
  gst_caps_unref (peer);
  if (rds) {
    state->caps = rds_caps;
  } else {
    gst_caps_unref (rds_caps);
    state->caps = gst_video_info_to_caps (&state->info);
  }
  GST_DEBUG_OBJECT (self, "output caps %" GST_PTR_FORMAT, state->caps);
  gst_video_codec_state_unref (state);
  self->out_width = width;
  self->out_height = height;
  return gst_video_decoder_negotiate (dec);
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

  /* A new or changed sequence. TODO (resolution changes): rocDecReconfigureDecoder when the new coded size fits. */
  destroy_decoder (self);
  if (!create_decoder (self, fmt, width, height)) {
    self->cb_flow = GST_FLOW_ERROR;
    return 0;
  }
  if (width != self->out_width || height != self->out_height
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
  st = rocDecDecodeFrame (self->decoder, pic);
  if (st != ROCDEC_SUCCESS) {
    GST_WARNING_OBJECT (self, "rocDecDecodeFrame: %s", rocDecGetErrorName (st));
    self->cb_decode_error = TRUE;
    return 0;
  }
  return 1;
}

/* Copies the decoded picture into the output buffer on the element's stream and waits for it (ready-on-push). Row
 * bytes: Y width; UV 2 * ceil(width / 2), over ceil(height / 2) rows. */
static gboolean
copy_picture (GstRdsVideoDec * self, GstBuffer * outbuf, void *planes[3], const uint32_t pitch[3])
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

  if (self->output_rds) {
    RdsBufSurfaceParams *p;

    if (rds_buffer_get_surface (outbuf) == NULL || !gst_buffer_map (outbuf, &map, GST_MAP_WRITE)) {
      GST_ERROR_OBJECT (self, "output buffer is not a mappable memory:RDS buffer");
      return FALSE;
    }
    surf = (RdsBufSurface *) map.data;
    p = &surf->surfaceList[0];
    if (p->colorFormat != RDS_BUF_COLOR_FORMAT_NV12 || p->width != w || p->height != h) {
      GST_ERROR_OBJECT (self, "output surface is %ux%u format %d, expected %ux%u NV12", p->width, p->height,
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
      GST_ERROR_OBJECT (self, "cannot map the output frame");
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
    surf->numFilled = 1;
    gst_buffer_unmap (outbuf, &map);
  } else {
    gst_video_frame_unmap (&frame);
  }
  if (err != hipSuccess) {
    GST_ERROR_OBJECT (self, "copying the decoded picture failed: %s", hipGetErrorString (err));
    return FALSE;
  }
  return TRUE;
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

  ret = gst_video_decoder_allocate_output_frame (dec, frame);
  if (ret != GST_FLOW_OK) {
    GST_DEBUG_OBJECT (self, "cannot allocate an output frame: %s", gst_flow_get_name (ret));
    self->cb_flow = ret;
    gst_video_decoder_release_frame (dec, frame);
    return 1;
  }
  if (!copy_picture (self, frame->output_buffer, planes, pitch)) {
    self->cb_decode_error = TRUE;
    gst_video_decoder_drop_frame (dec, frame);
    return 1;
  }
  ret = gst_video_decoder_finish_frame (dec, frame);
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

/* Sends an end-of-stream packet so every pending picture is displayed, then drops the parser (recreated lazily on
 * the next frame; the decoder is kept for a matching sequence). */
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
  hipError_t err;

  GST_OBJECT_LOCK (self);
  gpu_id = self->gpu_id;
  GST_OBJECT_UNLOCK (self);

  if (hipGetDeviceCount (&n) != hipSuccess || gpu_id >= (guint) n) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("GPU %u is not available", gpu_id),
        ("%d ROCm device(s) visible", n));
    return FALSE;
  }
  if (hipGetDevice (&prev) != hipSuccess || hipSetDevice ((int) gpu_id) != hipSuccess) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("GPU %u is not available", gpu_id), (NULL));
    return FALSE;
  }
  err = hipStreamCreateWithFlags (&self->stream, hipStreamDefault);
  (void) hipSetDevice (prev);
  if (err != hipSuccess) {
    self->stream = NULL;
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot create a HIP stream on GPU %u", gpu_id),
        ("hipStreamCreateWithFlags: %s", hipGetErrorString (err)));
    return FALSE;
  }
  self->stream_gpu = gpu_id;
  self->out_width = self->out_height = 0;
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
  self->output_rds = FALSE;
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
  } else {
    GST_ERROR_OBJECT (self, "unsupported caps %" GST_PTR_FORMAT, state->caps);
    return FALSE;
  }
  if (self->parser != NULL && codec != self->codec) {
    /* a new codec needs a new parser; pending pictures of the old stream are shown first */
    (void) drain_parser (self);
  }
  self->codec = codec;
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
  pkt.flags = ROCDEC_PKT_TIMESTAMP;
  /* on_display finds the frame again by this number (the parser hands it back in display order) */
  pkt.pts = frame->system_frame_number;
  ret = parse_packet (self, &pkt);
  gst_buffer_unmap (frame->input_buffer, &map);
  /* the frame stays in the base class's list until on_display finishes it */
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
  self->output_rds = rds_caps_has_memory_rds (caps);
  if (!self->output_rds)
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

  gobject_class->set_property = gst_rds_video_dec_set_property;
  gobject_class->get_property = gst_rds_video_dec_get_property;

  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU ID", "ROCm device to decode on", 0, G_MAXUINT, DEFAULT_GPU_ID, flags));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream video decoder",
      "Codec/Decoder/Video/Hardware", "Decodes H.264 with rocDecode into memory:RDS (HIP) or system memory",
      "Redstream contributors");

  dec_class->start = GST_DEBUG_FUNCPTR (gst_rds_video_dec_start);
  dec_class->stop = GST_DEBUG_FUNCPTR (gst_rds_video_dec_stop);
  dec_class->set_format = GST_DEBUG_FUNCPTR (gst_rds_video_dec_set_format);
  dec_class->handle_frame = GST_DEBUG_FUNCPTR (gst_rds_video_dec_handle_frame);
  dec_class->drain = GST_DEBUG_FUNCPTR (gst_rds_video_dec_drain);
  dec_class->finish = GST_DEBUG_FUNCPTR (gst_rds_video_dec_finish);
  dec_class->flush = GST_DEBUG_FUNCPTR (gst_rds_video_dec_flush);
  dec_class->decide_allocation = GST_DEBUG_FUNCPTR (gst_rds_video_dec_decide_allocation);
}

static void
gst_rds_video_dec_init (GstRdsVideoDec * self)
{
  self->gpu_id = DEFAULT_GPU_ID;
  self->mem_type = RDS_BUF_MEM_HIP_DEVICE;
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);
}
