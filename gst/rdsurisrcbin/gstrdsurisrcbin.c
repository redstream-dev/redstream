/* Redstream: rdsurisrcbin, a source bin for file and RTSP URIs, standing in for DeepStream's nvurisrcbin.
 *
 * Pads: one sometimes src template, vsrc_%u, for NV12 frames (memory:RDS, as rdsvideodec outputs them, or system
 * memory). As in DeepStream 8.0 behaviour, a new bin has no pad: the ghost pad vsrc_0 comes when the first video
 * stream is decoded (with its caps already set) and stays across file loops. It goes from PAUSED to READY (ours:
 * DeepStream 8.0 removes it at NULL). Audio is ignored: there is no asrc_%u template.
 *
 * URIs other than rtsp*:// are decoded by a uridecodebin3 (made going from NULL to READY), with only the first video
 * stream selected. Its decoded pad becomes vsrc_0's target, directly when the decoder can output memory:RDS
 * (rdsvideodec, which uridecodebin3 autoplugs), else through rdsvideoconvert to memory:RDS NV12. Any further pad goes
 * to a fakesink. Pad probes on the decoded pad (frames) and on vsrc_0's target (events) implement, as in DeepStream
 * 8.0 behaviour:
 *
 *   - file-loop on a file:// URI: no EOS, flush, second SEGMENT or STREAM_START downstream at the end of the file;
 *     the SEGMENT is open-ended, PTS go on from the end of the previous pass (DTS too: ours, DeepStream 8.0 offsets
 *     PTS only), and each pass's first buffer is DISCONT. A seek from downstream goes through; an EOS the application
 *     sends ends the loop (ours)
 *   - drop-frame-interval N: frames 0, N, 2N, ... of the decoded stream, counted across loops (0 and 1: every frame),
 *     dropped before any conversion
 *
 * rtsp*:// URIs (rtsp://, rtsps://, ...) go through an RTSP session (rds-urisrc-session: rtspsrc and the depayloader
 * in a bin of their own, made from READY to PAUSED and stopped from PAUSED to READY), then h264parse or h265parse
 * (config-interval -1) and rdsvideodec, made for the first session's codec, to vsrc_0, which is added at the decoder's
 * first caps:
 *
 *   - only H.264 and H.265 video streams are set up (rtspsrc's select-stream); another video codec (JPEG, say) is an
 *     error naming its encoding-name. Other streams are refused while disable-audio is TRUE, else discarded
 *   - rtspsrc gets latency, drop-on-latency, udp-buffer-size and protocols from select-rtp-protocol (below), and a
 *     100 ms teardown-timeout (ours)
 *   - the bin is live: PAUSED returns NO_PREROLL
 *   - drop-frame-interval applies as for files; file-loop does not (a GST_WARNING log at start)
 *   - without reconnection (rtsp-reconnect-interval 0, the default), an rtspsrc ERROR reaches the bus as it is, and the
 *     end of the stream (a server's EOS, or a closed TCP connection, which rtspsrc takes as one) is EOS on vsrc_0
 *   - an ERROR or WARNING from a session being stopped, or from the current one while the bin stops, is dropped: it is
 *     about the teardown
 *
 * Reconnection (rds-urisrc-reconnect decides when; the attempts run on GStreamer's thread pool):
 *
 *   - triggers, with rtsp-reconnect-interval above 0: no buffer out of the session for the interval (DeepStream 8.0
 *     behaviour: a watchdog on the system clock, ticking every second while PLAYING; reason "timeout"); and, at once
 *     (ours), an ERROR from the session or its EOS, which is dropped (reasons "error", "eos"). While reconnection is on,
 *     the session's ERRORs are posted as WARNINGs (DeepStream 8.0 behaviour), and so are the bin's own about a
 *     session it can't use (no video, or a codec it can't decode), each a failed
 *     attempt (ours)
 *   - the application's EOS ends the stream whatever the source's state (ours): no attempt after it, the session goes
 *     and the EOS goes through the parser and decoder to vsrc_0 (with no vsrc_0 yet, the attempts just stop)
 *   - an attempt replaces the session (the old one stopped with its state locked) and posts a WARNING (STREAM, FAILED;
 *     ours on every attempt: DeepStream 8.0 posts one for the first two attempts only) and the element message
 *     rds-reconnect (source-id int, attempt int, reason string; ours). It fails on an error, or without a buffer within
 *     the interval; the next one follows an interval after it started. The attempt count is consecutive failures, reset
 *     by a buffer
 *   - rtsp-reconnect-attempts N >= 0 (negative: unlimited): when attempt N fails (at once with N 0), a WARNING (ours:
 *     DeepStream 8.0 posts the message alone), then the element message attempt-exceeded (source-id uint, -1 as
 *     G_MAXUINT; attempt_exceeded boolean TRUE, as in DeepStream 8.0), then (ours) EOS on vsrc_0, so a muxer ends the
 *     source. Before vsrc_0 exists, attempt-exceeded and then an ERROR (RESOURCE, OPEN_READ; ours). No attempt after it
 *   - downstream (ours; DeepStream 8.0 restarts the running time): a reconnection is no new stream. The new session's
 *     STREAM_START and SEGMENT stop at the decoder's sink pad; the parser's src pad drops delta units until a key frame
 *     (marked DISCONT), and the leading pictures of an open GOP after it; the running time and PTS go on, as rtspsrc
 *     timestamps in the pipeline's running time. Before the first frame of the new session, once frames went out
 *     before, FLUSH_START and FLUSH_STOP (reset-time FALSE) leave vsrc_0, then the SEGMENT again (rdsstreammux sends
 *     one rds-stream-reset for the source); and the element message rds-reconnected (source-id int). A picture of the
 *     old session the decoder still held goes out before that. Failed attempts send nothing downstream
 *   - a new session at another size: new CAPS on vsrc_0 with its first frame, as in DeepStream 8.0 behaviour
 *     (rdsvideodec reconfigures or recreates its decoder), the reconnection otherwise as above. In another codec (H.264
 *     to H.265, say; ours: DeepStream 8.0 fails every attempt): a new parser in front of the same rdsvideodec, which
 *     switches its own parser to the new caps; vsrc_0 stays, and the reconnection is as above
 *   - the start: when the first connection fails, the bin retries every init-rtsp-reconnect-interval s if above 0
 *     (ours: it does nothing in DeepStream 8.0), else every rtsp-reconnect-interval s if above 0 (DeepStream 8.0
 *     behaviour), else the ERROR goes up. Retries post the same messages; vsrc_0 comes with the first frames
 *   - PAUSED to READY stops the watchdog and waits for an attempt under way; rtspsrc's 100 ms teardown-timeout keeps
 *     stopping a hanging session short
 *
 * The properties are nvurisrcbin's, with the names, types, ranges, defaults and NULL-or-READY flags of DeepStream 8.0
 * behaviour, where those differ from NVIDIA's nvurisrcbin pages (docs/compat-matrix.md lists each one and its
 * status). Going from NULL to READY checks the settings:
 *
 *   - uri: set, a valid URI, and a scheme some source element handles; else an error naming the property
 *   - smart-record: anything but smart-rec-disable is an error (smart record is not implemented)
 *   - type=rtsp needs an rtsp URI; else an error naming the property (ours: DeepStream 8.0 accepts the URI and plays
 *     it)
 *   - file-loop on a URI that is not file://: a GST_WARNING log, and the bin starts (and plays the URI once)
 *
 * source-id goes into the reconnection messages; gpu-id is stored and read back, and does nothing else yet.
 * The DeepStream properties with no counterpart here (decoder surfaces and memory, smart record's settings, IPC,
 * SEI, buffering of the internal queue, ...) are accepted no-ops: stored and read back, and a non-default value logs
 * a warning. message-forward and async-handling are GstBin's; nvurisrcbin has async-handling TRUE by default, so it
 * starts TRUE here.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsurisrcbin.h"
#include "rds-urisrc-reconnect.h"
#include "rds-urisrc-session.h"

#include <gst/rtsp/gstrtsptransport.h>
#include <rds/rds.h>

/* shared with rds-urisrc-session.c and rds-urisrc-reconnect.c */
GST_DEBUG_CATEGORY (gst_rds_uri_src_bin_debug);
#define GST_CAT_DEFAULT gst_rds_uri_src_bin_debug

/* ---- enums, with the values and nicks of DeepStream 8.0's nvurisrcbin ---- */

typedef enum
{
  GST_RDS_URI_SRC_BIN_TYPE_AUTO = 0,
  GST_RDS_URI_SRC_BIN_TYPE_URI = 1,
  GST_RDS_URI_SRC_BIN_TYPE_RTSP = 2,
} GstRdsUriSrcBinType;

/* RtpProtocol. When the bin opens RTSP URIs, the value maps to rtspsrc's protocols (GstRTSPLowerTrans): 1 UDP,
 * 2 UDP_MCAST, 4 TCP, 7 UDP | UDP_MCAST | TCP, 10 HTTP | TCP, 20 TLS | TCP; 0 leaves rtspsrc's default. 1, 2 and 4
 * are the GstRTSPLowerTrans flags themselves; 10 and 20 are not (HTTP is 16, TLS 32). */
typedef enum
{
  GST_RDS_URI_SRC_BIN_RTP_UNKNOWN = 0,
  GST_RDS_URI_SRC_BIN_RTP_UDP = 1,
  GST_RDS_URI_SRC_BIN_RTP_UDP_MCAST = 2,
  GST_RDS_URI_SRC_BIN_RTP_TCP = 4,
  GST_RDS_URI_SRC_BIN_RTP_UDP_UDP_MCAST_TCP = 7,
  GST_RDS_URI_SRC_BIN_RTP_HTTP = 10,
  GST_RDS_URI_SRC_BIN_RTP_TLS = 20,
} GstRdsUriSrcBinRtpProtocol;

G_STATIC_ASSERT ((gint) GST_RDS_URI_SRC_BIN_RTP_UDP == (gint) GST_RTSP_LOWER_TRANS_UDP);
G_STATIC_ASSERT ((gint) GST_RDS_URI_SRC_BIN_RTP_UDP_MCAST == (gint) GST_RTSP_LOWER_TRANS_UDP_MCAST);
G_STATIC_ASSERT ((gint) GST_RDS_URI_SRC_BIN_RTP_TCP == (gint) GST_RTSP_LOWER_TRANS_TCP);

typedef enum
{
  GST_RDS_URI_SRC_BIN_SMART_REC_DISABLE = 0,
  GST_RDS_URI_SRC_BIN_SMART_REC_CLOUD = 1,
  GST_RDS_URI_SRC_BIN_SMART_REC_MULTI = 2,
} GstRdsUriSrcBinSmartRecord;

#define DEFINE_ENUM_TYPE(func, name, ...)                                                  \
  static GType                                                                             \
  func (void)                                                                              \
  {                                                                                        \
    static GType type = 0;                                                                 \
    static const GEnumValue values[] = { __VA_ARGS__, {0, NULL, NULL} };                   \
                                                                                           \
    if (g_once_init_enter (&type))                                                         \
      g_once_init_leave (&type, g_enum_register_static (name, values));                    \
    return type;                                                                           \
  }

DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_type_get_type, "GstRdsUriSrcBinType",
    {GST_RDS_URI_SRC_BIN_TYPE_AUTO, "Select the source type from the URI scheme", "auto"},
    {GST_RDS_URI_SRC_BIN_TYPE_URI, "Any URI GStreamer has a source for", "uri"},
    {GST_RDS_URI_SRC_BIN_TYPE_RTSP, "RTSP", "rtsp"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_rtp_protocol_get_type, "GstRdsUriSrcBinRtpProtocol",
    {GST_RDS_URI_SRC_BIN_RTP_UNKNOWN, "rtspsrc's default", "rtp-unknown"},
    {GST_RDS_URI_SRC_BIN_RTP_UDP, "UDP", "rtp-udp"},
    {GST_RDS_URI_SRC_BIN_RTP_UDP_MCAST, "UDP multicast", "rtp-udp-mcast"},
    {GST_RDS_URI_SRC_BIN_RTP_TCP, "TCP (interleaved in the RTSP connection)", "rtp-tcp"},
    {GST_RDS_URI_SRC_BIN_RTP_UDP_UDP_MCAST_TCP, "UDP, UDP multicast or TCP", "rtp-udp-udpmcast-tcp"},
    {GST_RDS_URI_SRC_BIN_RTP_HTTP, "RTSP tunnelled over HTTP", "rtp-http"},
    {GST_RDS_URI_SRC_BIN_RTP_TLS, "RTSP over TLS", "rtp-tls"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_smart_record_get_type, "GstRdsUriSrcBinSmartRecord",
    {GST_RDS_URI_SRC_BIN_SMART_REC_DISABLE, "Disable smart record", "smart-rec-disable"},
    {GST_RDS_URI_SRC_BIN_SMART_REC_CLOUD, "Trigger smart record through cloud messages only", "smart-rec-cloud"},
    {GST_RDS_URI_SRC_BIN_SMART_REC_MULTI, "Trigger smart record through cloud and local events", "smart-rec-multi"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_smart_rec_container_get_type, "GstRdsUriSrcBinSmartRecContainer",
    {0, "MP4 container", "smart-rec-mp4"},
    {1, "MKV container", "smart-rec-mkv"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_smart_rec_mode_get_type, "GstRdsUriSrcBinSmartRecMode",
    {0, "Record audio and video if available", "smart-rec-mode-av"},
    {1, "Record video only if available", "smart-rec-mode-video"},
    {2, "Record audio only if available", "smart-rec-mode-audio"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_cudadec_memtype_get_type, "GstRdsUriSrcBinCudaDecMemType",
    {0, "Device memory", "memtype_device"},
    {1, "Pinned host memory", "memtype_pinned"},
    {2, "Unified memory", "memtype_unified"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_skip_frames_get_type, "GstRdsUriSrcBinSkipFrames",
    {0, "Decode all frames", "decode_all"},
    {1, "Decode non-reference frames", "decode_non_ref"},
    {2, "Decode key frames", "decode_key"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_buffer_mode_get_type, "GstRdsUriSrcBinBufferMode",
    {0, "Only use RTP timestamps", "unknown"},
    {1, "Slave receiver to sender clock", "slave"},
    {2, "Low/high watermark buffering", "buffer"},
    {3, "Choose the mode from whether the stream is live", "auto"},
    {4, "Synchronized sender and receiver clocks", "synced"});
DEFINE_ENUM_TYPE (gst_rds_uri_src_bin_leaky_get_type, "GstRdsUriSrcBinLeaky",
    {0, "Not leaky", "no-leak"},
    {1, "Leaky on upstream (new buffers)", "upstream-leak"},
    {2, "Leaky on downstream (old buffers)", "downstream-leak"});
#undef DEFINE_ENUM_TYPE

/* ---- properties ---- */

#define DEFAULT_LATENCY 100
#define DEFAULT_DROP_ON_LATENCY TRUE
#define DEFAULT_UDP_BUFFER_SIZE 524288
#define DEFAULT_RTP_PROTOCOL GST_RDS_URI_SRC_BIN_RTP_UDP_UDP_MCAST_TCP
#define DEFAULT_RECONNECT_INTERVAL 0
#define DEFAULT_RECONNECT_ATTEMPTS -1
#define DEFAULT_SOURCE_ID -1
#define DEFAULT_DISABLE_AUDIO TRUE

enum
{
  PROP_0,
  PROP_URI,
  PROP_TYPE,
  PROP_FILE_LOOP,
  PROP_LATENCY,
  PROP_DROP_ON_LATENCY,
  PROP_SELECT_RTP_PROTOCOL,
  PROP_UDP_BUFFER_SIZE,
  PROP_RTSP_RECONNECT_INTERVAL,
  PROP_RTSP_RECONNECT_ATTEMPTS,
  PROP_INIT_RTSP_RECONNECT_INTERVAL,
  PROP_DROP_FRAME_INTERVAL,
  PROP_SOURCE_ID,
  PROP_GPU_ID,
  PROP_DISABLE_AUDIO,
  PROP_SMART_RECORD,
  PROP_SMART_REC_STATUS,
  PROP_NOOP_FIRST,
};

/* nvurisrcbin's that have no counterpart here: stored; a non-default value logs a warning */
enum
{
  NOOP_NUM_EXTRA_SURFACES,
  NOOP_CUDADEC_MEMTYPE,
  NOOP_DEC_SKIP_FRAMES,
  NOOP_LOW_LATENCY_MODE,
  NOOP_SMART_REC_DIR_PATH,
  NOOP_SMART_REC_FILE_PREFIX,
  NOOP_SMART_REC_VIDEO_CACHE,
  NOOP_SMART_REC_CACHE,
  NOOP_SMART_REC_CONTAINER,
  NOOP_SMART_REC_MODE,
  NOOP_SMART_REC_DEFAULT_DURATION,
  NOOP_IPC_BUFFER_TIMESTAMP_COPY,
  NOOP_IPC_CONNECTION_ATTEMPTS,
  NOOP_IPC_CONNECTION_INTERVAL,
  NOOP_IPC_SOCKET_PATH,
  NOOP_IPC_FRAME_COPY,
  NOOP_EXTRACT_SEI_TYPE5_DATA,
  NOOP_SEI_UUID,
  NOOP_SIMULATE_FPS_INTERVAL_MS,
  NOOP_BUFFER_MODE,
  NOOP_LEAKY,
  NOOP_MAX_SIZE_BUFFERS,
  NOOP_DISABLE_PASSTHROUGH,
  N_NOOPS,
};

static GParamSpec *noop_specs[N_NOOPS];

struct _GstRdsUriSrcBin
{
  GstBin parent;

  /* properties (object lock) */
  gchar *uri;
  GstRdsUriSrcBinType type;
  gboolean file_loop;
  guint latency;
  gboolean drop_on_latency;
  GstRdsUriSrcBinRtpProtocol rtp_protocol;
  guint udp_buffer_size;
  guint reconnect_interval;
  gint reconnect_attempts;
  guint init_reconnect_interval;
  guint drop_frame_interval;
  gint source_id;
  guint gpu_id;
  gboolean disable_audio;
  GstRdsUriSrcBinSmartRecord smart_record;
  GValue noops[N_NOOPS];

  /* children (object lock; made and removed by state changes and pad-added) */
  GstElement *decodebin;        /* uridecodebin3, from NULL to READY; NULL for RTSP URIs */
  gboolean is_file;             /* the uri is file:// (fixed while not NULL) */
  gboolean is_rtsp;             /* the uri is rtsp*:// (fixed while not NULL) */
  RdsUriSrcSession *session;    /* RTSP: the current session, from READY to PAUSED */
  RdsUriSrcSession *retiring;   /* RTSP: the session being stopped, until it is out of the bin */
  RdsUriSrcCodec codec;         /* RTSP: what the parser and decoder were made for (NONE: not made yet) */
  GstElement *parser;           /* RTSP: the parser sessions link to */
  GList *dynamic;               /* the elements pad-added made (rdsvideoconvert, capsfilter, fakesinks; for RTSP the
                                 * parser and rdsvideodec, which outlive sessions) */
  GstPad *decoded;              /* the decoded video pad (frames: decoded_probe) */
  GstPad *target;               /* vsrc_0's target: the decoded pad, or the end of its conversion (events:
                                 * target_probe) */
  gulong decoded_probe_id, target_probe_id;
  GstPad *vsrc;                 /* vsrc_0 (RTSP: made with the decoder, added to the bin at its first caps) */
  gboolean have_video;          /* a decoded video pad was taken for vsrc_0 (RTSP: vsrc_0 was added) */
  gboolean stopping;            /* from PAUSED to READY: no new loop */
  RdsUriSrcReconnect *reconnect;        /* RTSP: when to replace the session (made once, its own lock) */

  /* the stream on vsrc_0 (object lock; reset going from READY to PAUSED) */
  gboolean seen_stream_start, seen_segment;
  gboolean looping;             /* from the EOS to the next pass's first buffer */
  guint32 loop_seqnum;          /* the latest loop seek's: its flushes and segment stay inside */
  gboolean loop_failed;         /* the loop seek failed: let the EOS through */
  gboolean eos_requested;       /* the application sent an EOS: let it through */
  gboolean eos_out;             /* an EOS went out of vsrc_0's target */
  gboolean resync;              /* the next timestamped buffer starts a new pass */
  gboolean discont;             /* the next buffer out is the first of a pass */
  GstClockTimeDiff offset;      /* added to PTS and DTS */
  GstClockTime end;             /* the latest end of a decoded frame (offset applied) */
  guint64 frame;                /* decoded frames seen, for drop-frame-interval */
  /* RTSP reconnection */
  gboolean dec_stream_start, dec_segment;       /* the decoder had the run's first STREAM_START, SEGMENT */
  gboolean gate;                /* a new session: delta units are dropped until a key frame */
  guint gate_dropped;
  GstClockTime gate_pts;        /* that key frame's PTS: decoded frames before it are the old session's */
  gboolean gate_leading;        /* after the key frame: delta units before it in PTS are dropped too */
  gboolean reset_pending;       /* a new session after frames went out: flush downstream before the next frame */
  gboolean reconnected_pending; /* an attempt was made: rds-reconnected at the next frame */
};

#define VSRC_FIELDS "format = (string) NV12, width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], " \
    "framerate = (fraction) [ 0/1, 2147483647/1 ]"

/* the format rdsvideodec outputs, in memory:RDS or system memory */
static GstStaticPadTemplate vsrc_template = GST_STATIC_PAD_TEMPLATE ("vsrc_%u", GST_PAD_SRC, GST_PAD_SOMETIMES,
    GST_STATIC_CAPS ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), " VSRC_FIELDS "; video/x-raw, " VSRC_FIELDS));

G_DEFINE_TYPE (GstRdsUriSrcBin, gst_rds_uri_src_bin, GST_TYPE_BIN);
GST_ELEMENT_REGISTER_DEFINE (rdsurisrcbin, "rdsurisrcbin", GST_RANK_NONE, GST_TYPE_RDS_URI_SRC_BIN);

/* ---- start ---- */

/* Whether some element is a URI source for @scheme. (gst_uri_protocol_is_supported does the same, but GStreamer 1.28
 * leaks a reference to every factory it finds.) */
static gboolean
have_source_for (const gchar * scheme)
{
  GList *factories = gst_element_factory_list_get_elements (GST_ELEMENT_FACTORY_TYPE_ANY, GST_RANK_NONE);
  gboolean found = FALSE;

  for (GList * l = factories; l != NULL && !found; l = l->next) {
    const gchar *const *protocols = gst_element_factory_get_uri_protocols (l->data);

    if (gst_element_factory_get_uri_type (l->data) != GST_URI_SRC || protocols == NULL)
      continue;
    for (guint i = 0; protocols[i] != NULL && !found; i++)
      found = g_ascii_strcasecmp (protocols[i], scheme) == 0;
  }
  gst_plugin_feature_list_free (factories);
  return found;
}

/* The settings NULL to READY checks; FALSE after posting an error naming the property. */
static gboolean
check_settings (GstRdsUriSrcBin * self)
{
  gchar *uri, *scheme = NULL;
  GstRdsUriSrcBinType type;
  GstRdsUriSrcBinSmartRecord smart_record;
  gboolean file_loop, ok = FALSE;

  GST_OBJECT_LOCK (self);
  uri = g_strdup (self->uri);
  type = self->type;
  smart_record = self->smart_record;
  file_loop = self->file_loop;
  GST_OBJECT_UNLOCK (self);

  /* the messages name the scheme, never the whole uri: it may hold credentials */
  if (uri == NULL || *uri == '\0') {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("No URI: set the uri property"), (NULL));
    goto done;
  }
  if (!gst_uri_is_valid (uri)) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND,
        ("The uri property is not a valid URI: it needs a scheme, as in file:///path or rtsp://host/path"), (NULL));
    goto done;
  }
  scheme = gst_uri_get_protocol (uri);
  if (!have_source_for (scheme)) {
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN,
        ("No source element handles the scheme '%s' of the uri property", scheme), (NULL));
    goto done;
  }
  if (smart_record != GST_RDS_URI_SRC_BIN_SMART_REC_DISABLE) {
    GST_ELEMENT_ERROR (self, CORE, NOT_IMPLEMENTED, ("Smart record is not supported: set smart-record=0"), (NULL));
    goto done;
  }
  if (type == GST_RDS_URI_SRC_BIN_TYPE_RTSP && !g_str_has_prefix (scheme, "rtsp")) {
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS,
        ("type=rtsp needs an rtsp:// URI, not %s://: set type=auto", scheme), (NULL));
    goto done;
  }
  if (file_loop && g_strcmp0 (scheme, "file") != 0)
    GST_WARNING_OBJECT (self, "file-loop applies to file:// URIs only; ignored for %s://", scheme);
  ok = TRUE;

done:
  g_free (scheme);
  g_free (uri);
  return ok;
}

/* ---- streaming ---- */

/* From a thread of GStreamer's pool, never the streaming thread: seeks the file back to its start. The seek goes
 * upstream from vsrc_0's target, as a sink's would. */
static void
loop_seek (GstElement * element, G_GNUC_UNUSED gpointer user_data)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (element);
  GstPad *target = NULL, *vsrc = NULL;
  GstEvent *seek = gst_event_new_seek (1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, GST_SEEK_TYPE_SET, 0,
      GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  gboolean stopping, eos = FALSE;

  GST_OBJECT_LOCK (self);
  /* not when stopping, or when a seek from downstream came first (it reset looping) */
  if (!self->stopping && self->looping && self->target != NULL) {
    vsrc = gst_object_ref (self->vsrc);
    /* the application's EOS came after the end of the file: it ends the loop here (once: it may have gone out
     * already) */
    if (self->eos_requested) {
      self->looping = FALSE;
      eos = !self->eos_out;
      self->eos_out = TRUE;
    } else {
      target = gst_object_ref (self->target);
      self->loop_seqnum = gst_event_get_seqnum (seek);
    }
  }
  GST_OBJECT_UNLOCK (self);
  if (eos) {
    /* as below: the streaming thread stopped at the end of the file */
    GST_DEBUG_OBJECT (self, "end of file after the application's EOS: no loop");
    gst_pad_push_event (vsrc, gst_event_new_eos ());
  }
  if (target == NULL) {
    /* not seeking */
    gst_clear_object (&vsrc);
    gst_event_unref (seek);
    return;
  }

  GST_DEBUG_OBJECT (self, "end of file: seeking back to the start");
  if (!gst_pad_send_event (target, seek)) {
    /* The stream ends here after all: the EOS the probe kept from vsrc_0 (the target is already EOS). Pushing it
     * from this pool thread is safe: the streaming thread stopped at the end of the file, and nothing restarts it
     * since the seek failed. */
    GST_OBJECT_LOCK (self);
    stopping = self->stopping;
    self->looping = FALSE;
    self->loop_failed = TRUE;
    self->eos_out = !stopping;
    GST_OBJECT_UNLOCK (self);
    if (!stopping) {
      GST_ELEMENT_WARNING (self, STREAM, FAILED, ("Could not seek back to the start of the file for file-loop"),
          (NULL));
      gst_pad_push_event (vsrc, gst_event_new_eos ());
    }
  }
  gst_object_unref (vsrc);
  gst_object_unref (target);
}

/* Events, on vsrc_0's target, so nothing it drops leaves the bin (decoded_probe, upstream of the conversion, does the
 * frames).
 *
 * file-loop: the probe drops the EOS at the end of the file and, from another thread (gst_element_call_async), sends a
 * flushing seek to 0 upstream. That seek's FLUSH_START, FLUSH_STOP and SEGMENT (told apart by its seqnum) stay
 * inside: they reach and reset the demuxer, decoder and converter, and the next pass's timestamps are shifted to
 * follow the previous pass. The flushes and SEGMENT of a seek from downstream go through. (A non-flushing segment
 * seek would need no timestamp rewriting, but it needs segment seeks from every demuxer and parser; a flushing seek
 * to 0 is what any seekable source does, MP4, AVI and raw H.264 alike.) */
static GstPadProbeReturn
target_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  gboolean loop = FALSE, drop = FALSE, foreign;

  GST_LOG_OBJECT (self, "event %" GST_PTR_FORMAT, ev);
  GST_OBJECT_LOCK (self);
  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_STREAM_START:
      /* one for the run */
      drop = self->seen_stream_start;
      self->seen_stream_start = TRUE;
      break;
    case GST_EVENT_FLUSH_START:
    case GST_EVENT_FLUSH_STOP:
      /* the loop seek's stay inside; others (a seek from downstream) go through */
      foreign = gst_event_get_seqnum (ev) != self->loop_seqnum;
      drop = !foreign;
      /* a seek from downstream after the application's EOS plays the file again, looping (ours) */
      if (foreign && GST_EVENT_TYPE (ev) == GST_EVENT_FLUSH_STOP)
        self->eos_requested = self->eos_out = FALSE;
      break;
    case GST_EVENT_SEGMENT:
      if (gst_event_get_seqnum (ev) == self->loop_seqnum) {
        /* the new pass's: the open-ended segment downstream already covers it */
        self->looping = drop = TRUE;
        break;
      }
      if (self->seen_segment) {
        /* a seek from downstream: its timestamps as they come, and no loop under way */
        self->looping = self->resync = FALSE;
        self->offset = 0;
        self->end = GST_CLOCK_TIME_NONE;
      }
      self->seen_segment = TRUE;
      if (self->file_loop && self->is_file) {
        GstSegment segment;
        GstEvent *open;

        /* open-ended, as the stream may never end */
        gst_event_copy_segment (ev, &segment);
        segment.stop = GST_CLOCK_TIME_NONE;
        open = gst_event_new_segment (&segment);
        gst_event_set_seqnum (open, gst_event_get_seqnum (ev));
        gst_event_unref (ev);
        GST_PAD_PROBE_INFO_DATA (info) = ev = open;
      }
      break;
    case GST_EVENT_EOS:
      /* the end of the file, unless the application asked for the EOS (ours) */
      if (self->file_loop && self->is_file && !self->stopping && !self->loop_failed && !self->eos_requested)
        self->looping = loop = drop = TRUE;
      else
        self->eos_out = TRUE;
      break;
    default:
      break;
  }
  GST_OBJECT_UNLOCK (self);
  if (loop)
    gst_element_call_async (GST_ELEMENT (self), loop_seek, NULL, NULL);
  if (drop) {
    /* HANDLED, not DROP: a dropped sticky event stays pending on the pad and is sent again with the next data */
    gst_event_unref (ev);
    return GST_PAD_PROBE_HANDLED;
  }
  return GST_PAD_PROBE_OK;
}

/* RTSP (ours): before the first frame of a new session, once frames went out, a flush out of vsrc_0 that keeps the
 * running time (FLUSH_STOP's reset-time FALSE): downstream drops what it holds of the old session, and rdsstreammux
 * sends rds-stream-reset for the source. Then the SEGMENT again, as FLUSH_STOP took it off the pad. On the streaming
 * thread, in order with the frames. (DeepStream 8.0 restarts the running time instead.) */
static void
reset_downstream (GstRdsUriSrcBin * self, GstPad * vsrc)
{
  GstEvent *segment = gst_pad_get_sticky_event (vsrc, GST_EVENT_SEGMENT, 0);
  guint32 seqnum = gst_util_seqnum_next ();
  GstEvent *ev;

  GST_INFO_OBJECT (self, "first frame from the new RTSP session: flushing downstream");
  ev = gst_event_new_flush_start ();
  gst_event_set_seqnum (ev, seqnum);
  gst_pad_push_event (vsrc, ev);
  ev = gst_event_new_flush_stop (FALSE);
  gst_event_set_seqnum (ev, seqnum);
  gst_pad_push_event (vsrc, ev);
  /* FLUSH_STOP took the TAG event off the pad too; it isn't pushed again, as nothing needs it */
  if (segment != NULL)
    gst_pad_push_event (vsrc, segment);
}

/* Frames, on the decoded pad: drop-frame-interval there, so dropped frames are never converted, and file-loop's
 * timestamps from every decoded frame, so the next pass starts after the last frame decoded, kept or not. RTSP: the
 * first frame after a reconnection attempt posts rds-reconnected, after reset_downstream when frames went out
 * before. */
static GstPadProbeReturn
decoded_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  GstClockTime pts = GST_BUFFER_PTS (buf), dts = GST_BUFFER_DTS (buf);
  GstClockTime ts = GST_CLOCK_TIME_IS_VALID (pts) ? pts : dts;
  GstClockTimeDiff offset;
  gboolean keep, discont = FALSE, reconnected = FALSE;
  GstPad *reset = NULL;
  gint source_id = 0;

  GST_LOG_OBJECT (self, "buffer %" GST_PTR_FORMAT, buf);
  GST_OBJECT_LOCK (self);
  if (self->looping) {
    /* the first frame of a new pass */
    self->looping = FALSE;
    self->resync = self->discont = TRUE;
  }
  /* A new pass starts where the previous one ended, from its first timestamped frame: decoders output in
   * presentation order, so that is its lowest PTS; a frame with only a DTS resyncs on that. */
  if (self->resync && GST_CLOCK_TIME_IS_VALID (ts)) {
    if (GST_CLOCK_TIME_IS_VALID (self->end))
      self->offset = GST_CLOCK_DIFF (ts, self->end);
    self->resync = FALSE;
  }
  offset = self->offset;
  if (GST_CLOCK_TIME_IS_VALID (ts)) {
    GstClockTime end = MAX ((GstClockTimeDiff) ts + offset, 0);

    if (GST_BUFFER_DURATION_IS_VALID (buf))
      end += GST_BUFFER_DURATION (buf);
    if (!GST_CLOCK_TIME_IS_VALID (self->end) || end > self->end)
      self->end = end;
  }
  /* frames 0, N, 2N, ... counted across file loops; DISCONT goes on a pass's first frame kept */
  keep = self->drop_frame_interval <= 1 || self->frame % self->drop_frame_interval == 0;
  self->frame++;
  if (keep) {
    discont = self->discont;
    self->discont = FALSE;
    /* not before the new session's first key frame: the decoder may still hold a picture of the old one, which the
     * key frame pushes out first */
    reconnected = self->reconnected_pending && !self->gate && (!GST_CLOCK_TIME_IS_VALID (ts)
        || !GST_CLOCK_TIME_IS_VALID (self->gate_pts) || ts >= self->gate_pts);
    if (reconnected) {
      if (self->reset_pending && self->vsrc != NULL)
        reset = gst_object_ref (self->vsrc);
      self->reconnected_pending = self->reset_pending = FALSE;
    }
    source_id = self->source_id;
  }
  GST_OBJECT_UNLOCK (self);

  if (!keep)
    return GST_PAD_PROBE_DROP;
  if (reconnected) {
    if (reset != NULL) {
      reset_downstream (self, reset);
      gst_object_unref (reset);
      discont = TRUE;
    }
    GST_INFO_OBJECT (self, "reconnected to the RTSP source");
    gst_element_post_message (GST_ELEMENT (self), gst_message_new_element (GST_OBJECT (self),
            gst_structure_new ("rds-reconnected", "source-id", G_TYPE_INT, source_id, NULL)));
  }
  if (offset != 0 || discont) {
    buf = gst_buffer_make_writable (buf);
    if (offset != 0 && GST_CLOCK_TIME_IS_VALID (pts))
      GST_BUFFER_PTS (buf) = MAX ((GstClockTimeDiff) pts + offset, 0);
    if (offset != 0 && GST_CLOCK_TIME_IS_VALID (dts))
      GST_BUFFER_DTS (buf) = MAX ((GstClockTimeDiff) dts + offset, 0);
    if (discont)
      GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
    GST_PAD_PROBE_INFO_DATA (info) = buf;
  }
  return GST_PAD_PROBE_OK;
}


/* RTSP: the first CAPS on vsrc_0 (stored on the pad before probes run, so the application's pad-added sees the
 * current caps) adds the pad to the bin. Linked from pad-added, inside this probe, the pad sends its STREAM_START
 * and then this CAPS downstream (GStreamer pushes the sticky events again, in order, after a relink in a probe). */
static GstPadProbeReturn
expose_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  gboolean expose;

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  GST_OBJECT_LOCK (self);
  expose = !self->stopping && self->vsrc == pad && !self->have_video;
  if (expose)
    self->have_video = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (expose) {
    GST_DEBUG_OBJECT (self, "adding vsrc_0 for %" GST_PTR_FORMAT, GST_PAD_PROBE_INFO_EVENT (info));
    gst_element_add_pad (GST_ELEMENT (self), pad);
  }
  return GST_PAD_PROBE_REMOVE;
}

/* ---- children ---- */

/* Adds @element to the bin, in its state, and to the dynamic elements removed from PAUSED to READY. */
static void
add_dynamic (GstRdsUriSrcBin * self, GstElement * element)
{
  GST_OBJECT_LOCK (self);
  self->dynamic = g_list_prepend (self->dynamic, gst_object_ref (element));
  GST_OBJECT_UNLOCK (self);
  gst_bin_add (GST_BIN (self), element);
}

/* A pad that is not vsrc_0's: to a fakesink. It is not a sink of this bin (no SINK flag, so the bin neither waits for
 * it nor posts its EOS) and never prerolls. */
static void
discard_pad (GstRdsUriSrcBin * self, GstPad * pad)
{
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);
  GstPad *sinkpad;

  if (sink == NULL)
    return;
  GST_DEBUG_OBJECT (self, "discarding %s:%s", GST_DEBUG_PAD_NAME (pad));
  g_object_set (sink, "sync", FALSE, "async", FALSE, NULL);
  GST_OBJECT_FLAG_UNSET (sink, GST_ELEMENT_FLAG_SINK);
  add_dynamic (self, sink);
  sinkpad = gst_element_get_static_pad (sink, "sink");
  gst_pad_link (pad, sinkpad);
  gst_object_unref (sinkpad);
  gst_element_sync_state_with_parent (sink);
}

static gboolean
pad_is_video (GstPad * pad)
{
  GstStream *stream = gst_pad_get_stream (pad);
  GstCaps *caps;
  gboolean video;

  if (stream != NULL) {
    video = (gst_stream_get_stream_type (stream) & GST_STREAM_TYPE_VIDEO) != 0;
    gst_object_unref (stream);
    return video;
  }
  caps = gst_pad_get_current_caps (pad);
  if (caps == NULL)
    caps = gst_pad_query_caps (pad, NULL);
  video = !gst_caps_is_empty (caps) && !gst_caps_is_any (caps)
      && g_str_has_prefix (gst_structure_get_name (gst_caps_get_structure (caps, 0)), "video/");
  gst_caps_unref (caps);
  return video;
}

/* uridecodebin3's decoded pads (on a streaming thread): the first video pad becomes vsrc_0's target, through
 * rdsvideoconvert when the decoder can't output memory:RDS. */
static void
on_pad_added (G_GNUC_UNUSED GstElement * decodebin, GstPad * pad, GstRdsUriSrcBin * self)
{
  GstPadTemplate *templ;
  GstPad *target, *vsrc;
  GstCaps *caps;
  gboolean take, rds;
  gulong decoded_id, target_id;

  take = pad_is_video (pad);
  GST_OBJECT_LOCK (self);
  take = take && !self->have_video && !self->stopping;
  if (take)
    self->have_video = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (!take) {
    discard_pad (self, pad);
    return;
  }

  caps = gst_pad_query_caps (pad, NULL);
  rds = rds_caps_has_memory_rds (caps);
  GST_DEBUG_OBJECT (self, "decoded pad %s:%s, caps %" GST_PTR_FORMAT, GST_DEBUG_PAD_NAME (pad), caps);
  gst_caps_unref (caps);
  templ = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (self), "vsrc_%u");
  decoded_id = gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, decoded_probe, self, NULL);
  if (rds) {
    target = gst_object_ref (pad);
  } else {
    GstElement *convert = gst_element_factory_make ("rdsvideoconvert", NULL);
    GstElement *filter = gst_element_factory_make ("capsfilter", NULL);
    GstPad *sinkpad;

    if (convert == NULL || filter == NULL) {
      gst_clear_object (&convert);
      gst_clear_object (&filter);
      gst_pad_remove_probe (pad, decoded_id);
      GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("rdsvideoconvert is missing"), (NULL));
      return;
    }
    /* frames on the GPU, as nvurisrcbin's are */
    caps = gst_caps_from_string ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), format = (string) NV12");
    g_object_set (filter, "caps", caps, NULL);
    gst_caps_unref (caps);
    add_dynamic (self, convert);
    add_dynamic (self, filter);
    sinkpad = gst_element_get_static_pad (convert, "sink");
    if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK || !gst_element_link (convert, filter)) {
      gst_object_unref (sinkpad);
      gst_pad_remove_probe (pad, decoded_id);
      GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("Could not link the decoder to rdsvideoconvert"), (NULL));
      return;
    }
    gst_object_unref (sinkpad);
    gst_element_sync_state_with_parent (filter);
    gst_element_sync_state_with_parent (convert);
    target = gst_element_get_static_pad (filter, "src");
  }

  target_id = gst_pad_add_probe (target, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
      target_probe, self, NULL);
  vsrc = gst_ghost_pad_new_from_template ("vsrc_0", target, templ);
  GST_OBJECT_LOCK (self);
  self->decoded = gst_object_ref (pad);
  self->decoded_probe_id = decoded_id;
  self->target = target;
  self->target_probe_id = target_id;
  self->vsrc = gst_object_ref (vsrc);
  GST_OBJECT_UNLOCK (self);
  gst_pad_set_active (vsrc, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), vsrc);
}

/* decodebin3's stream collection (posted on the thread that found it, before any stream is exposed): selects the
 * first video stream only, so audio and other streams are never decoded. */
static void
select_video (GstRdsUriSrcBin * self, GstMessage * msg)
{
  GstElementFactory *f = gst_element_get_factory (GST_ELEMENT (GST_MESSAGE_SRC (msg)));
  GstStreamCollection *collection = NULL;
  GList *ids = NULL;

  if (f == NULL || g_strcmp0 (GST_OBJECT_NAME (f), "decodebin3") != 0)
    return;
  gst_message_parse_stream_collection (msg, &collection);
  for (guint i = 0; i < gst_stream_collection_get_size (collection) && ids == NULL; i++) {
    GstStream *stream = gst_stream_collection_get_stream (collection, i);

    if (gst_stream_get_stream_type (stream) & GST_STREAM_TYPE_VIDEO)
      ids = g_list_append (ids, (gpointer) gst_stream_get_stream_id (stream));
  }
  if (ids != NULL) {
    /* to the decodebin3 that posted the collection: the element whose streams the ids name */
    GST_DEBUG_OBJECT (self, "selecting %s", (const gchar *) ids->data);
    gst_element_send_event (GST_ELEMENT (GST_MESSAGE_SRC (msg)), gst_event_new_select_streams (ids));
  }
  g_list_free (ids);
  gst_object_unref (collection);
}

typedef enum
{
  FROM_ELSEWHERE,
  FROM_SESSION,                 /* the current RTSP session */
  FROM_RETIRING,                /* the RTSP session being stopped */
} MessageOrigin;

/* Which RTSP session @msg comes from, if any; FROM_SESSION puts the current session in *@session (no ref: a token for
 * the reconnection). */
static MessageOrigin
message_origin (GstRdsUriSrcBin * self, GstMessage * msg, gpointer * session_out)
{
  GstObject *session = NULL, *retiring = NULL;
  MessageOrigin origin = FROM_ELSEWHERE;

  GST_OBJECT_LOCK (self);
  if (self->session != NULL)
    session = gst_object_ref (GST_OBJECT (self->session));
  if (self->retiring != NULL)
    retiring = gst_object_ref (GST_OBJECT (self->retiring));
  GST_OBJECT_UNLOCK (self);
  /* unlocked: walking up from an element outside the sessions takes this bin's lock */
  if (session != NULL && gst_object_has_as_ancestor (GST_MESSAGE_SRC (msg), session)) {
    origin = FROM_SESSION;
    *session_out = session;
  } else if (retiring != NULL && gst_object_has_as_ancestor (GST_MESSAGE_SRC (msg), retiring))
    origin = FROM_RETIRING;
  gst_clear_object (&session);
  gst_clear_object (&retiring);
  return origin;
}

static void
gst_rds_uri_src_bin_handle_message (GstBin * bin, GstMessage * msg)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (bin);
  gboolean failure = GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE (msg) == GST_MESSAGE_WARNING;
  gpointer session = NULL, lost = NULL;
  gboolean stopping;

  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_STREAM_COLLECTION && GST_IS_ELEMENT (GST_MESSAGE_SRC (msg)))
    select_video (self, msg);
  if (failure) {
    switch (message_origin (self, msg, &session)) {
      case FROM_SESSION:
        GST_OBJECT_LOCK (self);
        stopping = self->stopping;
        GST_OBJECT_UNLOCK (self);
        if (stopping) {
          /* about the teardown */
          GST_DEBUG_OBJECT (self, "dropped, from the RTSP session while stopping: %" GST_PTR_FORMAT, msg);
          gst_message_unref (msg);
          return;
        }
        GST_INFO_OBJECT (self, "from the RTSP session: %" GST_PTR_FORMAT, msg);
        if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR && rds_urisrc_reconnect_active (self->reconnect)) {
          GError *err = NULL;
          gchar *dbg = NULL;
          GstMessage *warning;

          /* DeepStream 8.0 behaviour: while reconnecting, the session's errors are warnings (without reconnection
           * they go up as they are) */
          gst_message_parse_error (msg, &err, &dbg);
          warning = gst_message_new_warning (GST_MESSAGE_SRC (msg), err, dbg);
          gst_message_set_seqnum (warning, gst_message_get_seqnum (msg));
          g_clear_error (&err);
          g_free (dbg);
          gst_message_unref (msg);
          msg = warning;
          lost = session;
        }
        break;
      case FROM_RETIRING:
        /* about the teardown of a session that is going anyway */
        GST_DEBUG_OBJECT (self, "dropped, from the RTSP session being stopped: %" GST_PTR_FORMAT, msg);
        gst_message_unref (msg);
        return;
      case FROM_ELSEWHERE:
        break;
    }
  }
  GST_BIN_CLASS (gst_rds_uri_src_bin_parent_class)->handle_message (bin, msg);
  if (lost != NULL)
    rds_urisrc_reconnect_lost (self->reconnect, lost, RDS_URISRC_RECONNECT_ERROR);
}

/* NULL to READY, after check_settings: a uridecodebin3 for any URI but RTSP. FALSE after posting an error. */
static gboolean
make_children (GstRdsUriSrcBin * self)
{
  GstElement *decodebin;
  gchar *uri, *scheme;
  gboolean rtsp;

  GST_OBJECT_LOCK (self);
  uri = g_strdup (self->uri);
  GST_OBJECT_UNLOCK (self);
  scheme = gst_uri_get_protocol (uri);
  rtsp = g_str_has_prefix (scheme, "rtsp");
  GST_OBJECT_LOCK (self);
  self->is_file = g_strcmp0 (scheme, "file") == 0;
  self->is_rtsp = rtsp;
  GST_OBJECT_UNLOCK (self);
  g_free (scheme);
  if (rtsp) {
    /* the session comes from READY to PAUSED, the decoder with its first stream */
    g_free (uri);
    return TRUE;
  }

  decodebin = gst_element_factory_make ("uridecodebin3", NULL);
  if (decodebin == NULL) {
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("uridecodebin3 is missing"), (NULL));
    g_free (uri);
    return FALSE;
  }
  g_object_set (decodebin, "uri", uri, NULL);
  g_free (uri);
  g_signal_connect (decodebin, "pad-added", G_CALLBACK (on_pad_added), self);
  GST_OBJECT_LOCK (self);
  self->decodebin = gst_object_ref (decodebin);
  GST_OBJECT_UNLOCK (self);
  gst_bin_add (GST_BIN (self), decodebin);
  return TRUE;
}

static void
remove_element (GstRdsUriSrcBin * self, GstElement * element)
{
  gst_element_set_state (element, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (self), element);
  gst_object_unref (element);
}

/* ---- RTSP ---- */

/* At the decoder's sink pad, which outlives sessions and parsers (ours): a new session's STREAM_START and SEGMENT are
 * dropped after the run's first ones, so a reconnection starts no new stream downstream. The parser in front gets
 * them (a new parser, for another codec, needs them). The running time goes on without help: a live rtspsrc added to
 * the running pipeline timestamps in its running time, as the first one did. */
static GstPadProbeReturn
decoder_sink_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  gboolean *seen, drop;

  switch (GST_EVENT_TYPE (ev)) {
    case GST_EVENT_STREAM_START:
      seen = &self->dec_stream_start;
      break;
    case GST_EVENT_SEGMENT:
      seen = &self->dec_segment;
      break;
    default:
      return GST_PAD_PROBE_OK;
  }
  GST_OBJECT_LOCK (self);
  drop = *seen;
  *seen = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (!drop)
    return GST_PAD_PROBE_OK;
  GST_DEBUG_OBJECT (self, "dropped, from a new RTSP session: %" GST_PTR_FORMAT, ev);
  /* HANDLED, not DROP: a dropped sticky event stays pending on the pad */
  gst_event_unref (ev);
  return GST_PAD_PROBE_HANDLED;
}

/* At the parser's src pad (ours): after a reconnection, the delta units before the first key frame are dropped, so
 * the decoder never gets a picture whose references were the old session's, and that key frame goes on DISCONT. The
 * parser sets the flags, and its config-interval -1 puts the parameter sets in front of every key frame. A key frame
 * may open a GOP whose leading pictures (B or RASL, after it in decode order and before it in PTS) refer to pictures
 * before it: h264parse takes any I picture for a key frame, h265parse a CRA. Those are dropped too, up to the first
 * picture after the key frame in PTS. */
static GstPadProbeReturn
gate_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
  GstClockTime pts = GST_BUFFER_PTS (buf);
  guint dropped;

  GST_OBJECT_LOCK (self);
  if (self->gate_leading) {
    gboolean leading = GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT) && GST_CLOCK_TIME_IS_VALID (pts)
        && GST_CLOCK_TIME_IS_VALID (self->gate_pts) && pts < self->gate_pts;

    /* over once a picture after the key frame passes (or one whose order can't be told) */
    if (!leading && (!GST_CLOCK_TIME_IS_VALID (pts) || !GST_CLOCK_TIME_IS_VALID (self->gate_pts)
            || pts > self->gate_pts))
      self->gate_leading = FALSE;
    GST_OBJECT_UNLOCK (self);
    if (leading) {
      GST_DEBUG_OBJECT (self, "dropped a leading picture of the first key frame: %" GST_PTR_FORMAT, buf);
      return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
  }
  if (!self->gate) {
    GST_OBJECT_UNLOCK (self);
    return GST_PAD_PROBE_OK;
  }
  if (GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT)) {
    self->gate_dropped++;
    GST_OBJECT_UNLOCK (self);
    return GST_PAD_PROBE_DROP;
  }
  self->gate = FALSE;
  self->gate_pts = pts;
  self->gate_leading = TRUE;
  dropped = self->gate_dropped;
  GST_OBJECT_UNLOCK (self);
  GST_INFO_OBJECT (self, "first key frame from the new RTSP session: dropped %u delta units before it", dropped);
  buf = gst_buffer_make_writable (buf);
  GST_BUFFER_FLAG_SET (buf, GST_BUFFER_FLAG_DISCONT);
  GST_PAD_PROBE_INFO_DATA (info) = buf;
  return GST_PAD_PROBE_OK;
}

static const gchar *
parser_name (RdsUriSrcCodec codec)
{
  return codec == RDS_URISRC_CODEC_H264 ? "h264parse" : "h265parse";
}

/* RTSP: the parser for @codec, in the bin (in NULL), with the key-frame gate on its src pad; NULL when it is
 * missing. */
static GstElement *
make_parser (GstRdsUriSrcBin * self, RdsUriSrcCodec codec)
{
  GstElement *parse = gst_element_factory_make (parser_name (codec), NULL);
  GstPad *pad;

  if (parse == NULL)
    return NULL;
  /* SPS and PPS (and VPS) with every key frame, so the decoder can start on any of them */
  g_object_set (parse, "config-interval", -1, NULL);
  pad = gst_element_get_static_pad (parse, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, gate_probe, self, NULL);
  gst_object_unref (pad);
  add_dynamic (self, parse);
  return parse;
}

/* RTSP: the parser and rdsvideodec for @codec (on the session's streaming thread), with vsrc_0 on the decoder's src
 * pad, not yet in the bin; NULL after posting an error, else the parser (a ref). */
static GstElement *
make_decoder (GstRdsUriSrcBin * self, RdsUriSrcCodec codec)
{
  GstElement *parse, *dec = gst_element_factory_make ("rdsvideodec", NULL);
  GstPadTemplate *templ = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (self), "vsrc_%u");
  GstPad *target, *vsrc, *pad;
  gulong decoded_id, target_id;

  if (dec == NULL) {
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("rdsvideodec is missing"), (NULL));
    return NULL;
  }
  if ((parse = make_parser (self, codec)) == NULL) {
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("%s is missing", parser_name (codec)), (NULL));
    gst_object_unref (dec);
    return NULL;
  }
  add_dynamic (self, dec);
  if (!gst_element_link (parse, dec)) {
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("Could not link %s to rdsvideodec", parser_name (codec)), (NULL));
    /* both go, so the next session makes a pair of its own and finds no other */
    GST_OBJECT_LOCK (self);
    self->dynamic = g_list_remove (g_list_remove (self->dynamic, parse), dec);
    GST_OBJECT_UNLOCK (self);
    remove_element (self, parse);
    remove_element (self, dec);
    return NULL;
  }
  pad = gst_element_get_static_pad (dec, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, decoder_sink_probe, self, NULL);
  gst_object_unref (pad);

  /* the decoder's output is memory:RDS NV12 already: it is vsrc_0's target, with both probes */
  target = gst_element_get_static_pad (dec, "src");
  decoded_id = gst_pad_add_probe (target, GST_PAD_PROBE_TYPE_BUFFER, decoded_probe, self, NULL);
  target_id = gst_pad_add_probe (target, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
      target_probe, self, NULL);
  vsrc = gst_ghost_pad_new_from_template ("vsrc_0", target, templ);
  gst_pad_add_probe (vsrc, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, expose_probe, self, NULL);
  gst_pad_set_active (vsrc, TRUE);
  GST_OBJECT_LOCK (self);
  self->decoded = gst_object_ref (target);
  self->decoded_probe_id = decoded_id;
  self->target = target;
  self->target_probe_id = target_id;
  self->vsrc = gst_object_ref_sink (vsrc);
  self->parser = gst_object_ref (parse);
  GST_OBJECT_UNLOCK (self);
  gst_element_sync_state_with_parent (dec);
  gst_element_sync_state_with_parent (parse);
  return gst_object_ref (parse);
}

/* A session the bin can't use (its codec): an ERROR (STREAM, CODEC_NOT_FOUND) saying @text, or while reconnection is
 * on a WARNING and a failed attempt, as for the session's own errors. */
static void
session_failed (GstRdsUriSrcBin * self, RdsUriSrcSession * session, const gchar * text)
{
  gboolean stopping;

  GST_OBJECT_LOCK (self);
  stopping = self->stopping;
  GST_OBJECT_UNLOCK (self);
  if (stopping) {
    /* about a session going anyway */
    GST_DEBUG_OBJECT (self, "while stopping: %s", text);
    return;
  }
  if (!rds_urisrc_reconnect_active (self->reconnect)) {
    GST_ELEMENT_ERROR (self, STREAM, CODEC_NOT_FOUND, ("%s", text), (NULL));
    return;
  }
  GST_ELEMENT_WARNING (self, STREAM, CODEC_NOT_FOUND, ("%s", text), (NULL));
  rds_urisrc_reconnect_lost (self->reconnect, session, RDS_URISRC_RECONNECT_ERROR);
}

/* Buffers leaving a session: the reconnection's watchdog counts from the last one. */
static GstPadProbeReturn
session_buffer_probe (GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;

  rds_urisrc_reconnect_buffer (self->reconnect, GST_PAD_PARENT (pad));
  return GST_PAD_PROBE_OK;
}

/* RTSP (ours): a session in another codec than the last (H.264 to H.265, say). A parser for it replaces the old one
 * in front of the same decoder: rdsvideodec takes the new caps and makes a rocDecode parser for them, after showing
 * the pictures of the old stream it still held. vsrc_0 stays, and the reconnection goes on as any other (the key-frame
 * gate, one flush, the SEGMENT again). The old parser is idle: its session is gone. On the new session's thread;
 * safe against stopping, as PAUSED to READY stops the session (joining its threads, this one too) before remove_pad
 * takes the parser and decoder away. NULL after session_failed, with no parser left (the next session makes one for
 * its codec), else the new parser (a ref). */
static GstElement *
swap_parser (GstRdsUriSrcBin * self, RdsUriSrcSession * session, RdsUriSrcCodec codec)
{
  GstElement *parse = make_parser (self, codec), *old, *dec;
  GstPad *src, *decpad;
  gchar *text;

  GST_OBJECT_LOCK (self);
  old = g_steal_pointer (&self->parser);
  self->codec = RDS_URISRC_CODEC_NONE;
  dec = gst_object_ref (GST_PAD_PARENT (self->target));
  GST_OBJECT_UNLOCK (self);
  GST_INFO_OBJECT (self, "the RTSP stream is %s now: a new parser", rds_urisrc_codec_name (codec));
  decpad = gst_element_get_static_pad (dec, "sink");
  gst_object_unref (dec);
  if (old != NULL) {
    GST_OBJECT_LOCK (self);
    self->dynamic = g_list_remove (self->dynamic, old);
    GST_OBJECT_UNLOCK (self);
    src = gst_element_get_static_pad (old, "src");
    gst_pad_unlink (src, decpad);
    gst_object_unref (src);
    /* the list's ref, then self->parser's */
    remove_element (self, old);
    gst_object_unref (old);
  }
  if (parse == NULL) {
    text = g_strdup_printf ("%s is missing: the RTSP stream changed to %s", parser_name (codec),
        rds_urisrc_codec_name (codec));
  } else {
    src = gst_element_get_static_pad (parse, "src");
    text = gst_pad_link (src, decpad) == GST_PAD_LINK_OK ? NULL :
        g_strdup_printf ("Could not link %s to rdsvideodec", parser_name (codec));
    gst_object_unref (src);
  }
  gst_object_unref (decpad);
  if (text != NULL) {
    if (parse != NULL) {
      GST_OBJECT_LOCK (self);
      self->dynamic = g_list_remove (self->dynamic, parse);
      GST_OBJECT_UNLOCK (self);
      remove_element (self, parse);
    }
    session_failed (self, session, text);
    g_free (text);
    return NULL;
  }
  gst_element_sync_state_with_parent (parse);
  GST_OBJECT_LOCK (self);
  self->parser = gst_object_ref (parse);
  self->codec = codec;
  GST_OBJECT_UNLOCK (self);
  return gst_object_ref (parse);
}

/* The session's video pad (its streaming thread): to the parser, made with the decoder for the first session's
 * codec. The decoder outlives sessions; the parser too, unless a later session is in another codec (swap_parser). */
static void
on_session_video (RdsUriSrcSession * session, GstPad * pad, RdsUriSrcCodec codec, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  gboolean have_decoder;
  GstElement *parse = NULL;
  GstPad *sinkpad;

  GST_OBJECT_LOCK (self);
  if (session != self->session || self->stopping) {
    /* a session being stopped */
    GST_OBJECT_UNLOCK (self);
    GST_DEBUG_OBJECT (self, "video from a session that is not the current one: ignored");
    return;
  }
  have_decoder = self->target != NULL;
  if (!have_decoder)
    self->codec = codec;
  else if (self->parser != NULL && self->codec == codec)
    parse = gst_object_ref (self->parser);
  GST_OBJECT_UNLOCK (self);
  if (!have_decoder && (parse = make_decoder (self, codec)) == NULL) {
    /* no decoder for it: the next session tries again */
    GST_OBJECT_LOCK (self);
    self->codec = RDS_URISRC_CODEC_NONE;
    GST_OBJECT_UNLOCK (self);
    return;
  }
  if (parse == NULL && (parse = swap_parser (self, session, codec)) == NULL)
    return;
  sinkpad = gst_element_get_static_pad (parse, "sink");
  GST_DEBUG_OBJECT (self, "%s stream from the RTSP session", rds_urisrc_codec_name (codec));
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST, session_buffer_probe, self,
      NULL);
  if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK)
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("Could not link the RTSP stream to the parser"), (NULL));
  gst_object_unref (sinkpad);
  gst_object_unref (parse);
}

/* The session found no video stream it can give the decoder (rtspsrc's threads). */
static void
on_session_no_video (RdsUriSrcSession * session, const gchar * encoding, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  gchar *text;

  /* the encoding-name only: the URI may hold credentials */
  if (encoding != NULL)
    text = g_strdup_printf ("The RTSP source's video is %s (its encoding-name), which cannot be decoded: only H.264 "
        "and H.265 are supported. Select the camera's H.264 or H.265 stream (profile)", encoding);
  else
    text = g_strdup ("The RTSP source has no video stream");
  session_failed (self, session, text);
  g_free (text);
}

/* An EOS leaving the session (its streaming thread): the server ended the stream, or rtspsrc took a closed
 * connection as its end. With no reconnection, it goes on to vsrc_0 as the end of the stream; with reconnection it
 * is dropped and a new session replaces this one (ours: at once). (The application's EOS is the bin's to send: see
 * gst_rds_uri_src_bin_send_event.) */
static gboolean
on_session_eos (RdsUriSrcSession * session, gpointer user_data)
{
  GstRdsUriSrcBin *self = user_data;
  gboolean requested;

  GST_OBJECT_LOCK (self);
  requested = self->eos_requested;
  GST_OBJECT_UNLOCK (self);
  if (requested || !rds_urisrc_reconnect_active (self->reconnect)) {
    GST_DEBUG_OBJECT (self, "EOS from the RTSP session: passed on");
    return TRUE;
  }
  GST_DEBUG_OBJECT (self, "EOS from the RTSP session: dropped, reconnecting");
  rds_urisrc_reconnect_lost (self->reconnect, session, RDS_URISRC_RECONNECT_EOS);
  return FALSE;
}

/* A new RTSP session from the properties, added to the bin (in NULL: the caller sets its state). FALSE after posting
 * an error. */
static gboolean
add_session (GstRdsUriSrcBin * self)
{
  static const RdsUriSrcSessionCallbacks callbacks = { on_session_video, on_session_no_video, on_session_eos };
  RdsUriSrcSessionSettings settings;
  RdsUriSrcSession *session;
  gchar *uri;

  GST_OBJECT_LOCK (self);
  uri = g_strdup (self->uri);
  settings.uri = uri;
  settings.latency = self->latency;
  settings.drop_on_latency = self->drop_on_latency;
  settings.udp_buffer_size = self->udp_buffer_size;
  settings.disable_audio = self->disable_audio;
  /* GstRTSPLowerTrans: 1, 2 and 4 are its flags; 10 and 20 are HTTP or TLS (16, 32) over TCP */
  switch (self->rtp_protocol) {
    case GST_RDS_URI_SRC_BIN_RTP_HTTP:
      settings.protocols = GST_RTSP_LOWER_TRANS_HTTP | GST_RTSP_LOWER_TRANS_TCP;
      break;
    case GST_RDS_URI_SRC_BIN_RTP_TLS:
      settings.protocols = GST_RTSP_LOWER_TRANS_TLS | GST_RTSP_LOWER_TRANS_TCP;
      break;
    default:
      settings.protocols = self->rtp_protocol;
      break;
  }
  GST_OBJECT_UNLOCK (self);
  session = rds_urisrc_session_new (&settings, &callbacks, self);
  g_free (uri);
  if (session == NULL) {
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("rtspsrc is missing"), (NULL));
    return FALSE;
  }
  rds_urisrc_reconnect_set_session (self->reconnect, session);
  GST_OBJECT_LOCK (self);
  self->session = gst_object_ref (session);
  GST_OBJECT_UNLOCK (self);
  gst_bin_add (GST_BIN (self), GST_ELEMENT (session));
  return TRUE;
}

/* The current RTSP session goes (rds_urisrc_session_stop: NULL, unlinked from the parser, removed). Until it is out of
 * the bin it is the retiring session, so its late messages are still told apart; its state is locked first, so a
 * change of the bin's state meanwhile can't start it again. Not on a session's thread. */
static void
remove_session (GstRdsUriSrcBin * self)
{
  RdsUriSrcSession *session;

  GST_OBJECT_LOCK (self);
  session = g_steal_pointer (&self->session);
  /* only when there is one: a second caller must not clear the first one's */
  if (session != NULL)
    self->retiring = session;
  GST_OBJECT_UNLOCK (self);
  if (session == NULL)
    return;
  gst_element_set_locked_state (GST_ELEMENT (session), TRUE);
  rds_urisrc_session_stop (session);
  GST_OBJECT_LOCK (self);
  if (self->retiring == session)
    self->retiring = NULL;
  GST_OBJECT_UNLOCK (self);
  gst_object_unref (session);
}

/* ---- RTSP reconnection (rds-urisrc-reconnect decides when; GStreamer's thread pool) ---- */

static const gchar *
reason_text (RdsUriSrcReconnectReason reason)
{
  switch (reason) {
    case RDS_URISRC_RECONNECT_ERROR:
      return "the stream failed";
    case RDS_URISRC_RECONNECT_EOS:
      return "the stream ended";
    default:
      return "no data within the reconnect interval";
  }
}

/* An attempt: a WARNING (ours: one per attempt; DeepStream 8.0 posts one for the first two attempts only) and
 * rds-reconnect (ours). */
static void
reconnect_announce (GstElement * element, guint attempt, RdsUriSrcReconnectReason reason)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (element);
  gint source_id;

  GST_OBJECT_LOCK (self);
  source_id = self->source_id;
  GST_OBJECT_UNLOCK (self);
  GST_ELEMENT_WARNING (self, STREAM, FAILED, ("Reconnecting to the RTSP source, attempt %u: %s", attempt,
          reason_text (reason)), (NULL));
  gst_element_post_message (element, gst_message_new_element (GST_OBJECT (self), gst_structure_new ("rds-reconnect",
              "source-id", G_TYPE_INT, source_id, "attempt", G_TYPE_INT, (gint) attempt, "reason", G_TYPE_STRING,
              rds_urisrc_reconnect_reason_name (reason), NULL)));
}

/* An attempt: a new session for the old one, its first frames gated to a key frame. Failed attempts send nothing
 * downstream: the flush waits for the first frame. */
static void
reconnect_replace (GstElement * element)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (element);
  RdsUriSrcSession *session = NULL;

  remove_session (self);
  GST_OBJECT_LOCK (self);
  self->gate = TRUE;
  self->gate_leading = FALSE;
  self->gate_dropped = 0;
  self->gate_pts = GST_CLOCK_TIME_NONE;
  /* never on the first connection (start-up retries) */
  self->reset_pending = self->frame > 0;
  self->reconnected_pending = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (!add_session (self))
    return;
  GST_OBJECT_LOCK (self);
  if (self->session != NULL)
    session = gst_object_ref (self->session);
  GST_OBJECT_UNLOCK (self);
  if (session != NULL) {
    gst_element_sync_state_with_parent (GST_ELEMENT (session));
    gst_object_unref (session);
  }
}

/* RTSP: the stream ends here, once: the session goes, and an EOS goes through the parser and decoder to vsrc_0 (none
 * without a parser yet, or once an EOS went out). Not on a session's thread. */
static void
end_stream (GstRdsUriSrcBin * self)
{
  GstPad *sinkpad = NULL;

  remove_session (self);
  GST_OBJECT_LOCK (self);
  if (!self->stopping && !self->eos_out && self->parser != NULL)
    sinkpad = gst_element_get_static_pad (self->parser, "sink");
  GST_OBJECT_UNLOCK (self);
  if (sinkpad != NULL) {
    gst_pad_send_event (sinkpad, gst_event_new_eos ());
    gst_object_unref (sinkpad);
  }
}

/* The attempts ran out: attempt-exceeded (DeepStream 8.0's name and fields: source-id a uint, so -1 reads as
 * G_MAXUINT). With vsrc_0, a WARNING, then (ours) the stream ends with an EOS, so a muxer ends the source instead of
 * waiting for it. Without (ours; never connected, or never decoded to caps): an ERROR, as there is no vsrc_0 to end.
 * DeepStream 8.0 posts the message alone either way. */
static void
reconnect_give_up (GstElement * element, guint attempts)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (element);
  gboolean connected;
  gint source_id;

  GST_OBJECT_LOCK (self);
  source_id = self->source_id;
  connected = self->have_video;
  GST_OBJECT_UNLOCK (self);
  if (connected)
    GST_ELEMENT_WARNING (self, STREAM, FAILED, ("Could not reconnect to the RTSP source: giving up after %u attempts",
            attempts), (NULL));
  gst_element_post_message (element, gst_message_new_element (GST_OBJECT (self),
          gst_structure_new ("attempt-exceeded", "source-id", G_TYPE_UINT, (guint) source_id, "attempt_exceeded",
              G_TYPE_BOOLEAN, TRUE, NULL)));
  if (connected) {
    end_stream (self);
    return;
  }
  remove_session (self);
  GST_ELEMENT_ERROR (self, RESOURCE, OPEN_READ, ("Could not connect to the RTSP source after %u attempts",
          attempts), (NULL));
}

/* The application's EOS (its own job, after any attempt under way, and never in another run). */
static void
reconnect_end (GstElement * element)
{
  end_stream (GST_RDS_URI_SRC_BIN (element));
}

static const RdsUriSrcReconnectCallbacks reconnect_callbacks = { reconnect_announce, reconnect_replace,
  reconnect_give_up, reconnect_end
};

/* PAUSED to READY, once the children are in READY: vsrc_0 and the elements pad-added made go. */
static void
remove_pad (GstRdsUriSrcBin * self)
{
  GstPad *vsrc, *decoded, *target;
  GstElement *parser;
  gulong decoded_id, target_id;
  GList *dynamic;
  gboolean added;

  GST_OBJECT_LOCK (self);
  vsrc = g_steal_pointer (&self->vsrc);
  parser = g_steal_pointer (&self->parser);
  self->codec = RDS_URISRC_CODEC_NONE;
  dynamic = g_steal_pointer (&self->dynamic);
  decoded = g_steal_pointer (&self->decoded);
  target = g_steal_pointer (&self->target);
  decoded_id = self->decoded_probe_id;
  target_id = self->target_probe_id;
  self->decoded_probe_id = self->target_probe_id = 0;
  /* with a vsrc_0: it is in the bin (RTSP: not before the decoder's first caps) */
  added = self->have_video;
  self->have_video = FALSE;
  GST_OBJECT_UNLOCK (self);
  if (decoded != NULL) {
    gst_pad_remove_probe (decoded, decoded_id);
    gst_object_unref (decoded);
  }
  if (target != NULL) {
    gst_pad_remove_probe (target, target_id);
    gst_object_unref (target);
  }
  if (vsrc != NULL) {
    gst_pad_set_active (vsrc, FALSE);
    if (added)
      gst_element_remove_pad (GST_ELEMENT (self), vsrc);
    gst_object_unref (vsrc);
  }
  gst_clear_object (&parser);
  for (GList * l = dynamic; l != NULL; l = l->next)
    remove_element (self, l->data);
  g_list_free (dynamic);
}

/* Back to NULL (or a failed start): every child goes. */
static void
remove_children (GstRdsUriSrcBin * self)
{
  GstElement *decodebin;

  remove_session (self);
  remove_pad (self);
  GST_OBJECT_LOCK (self);
  decodebin = g_steal_pointer (&self->decodebin);
  /* the next start takes the uri again */
  self->is_file = self->is_rtsp = FALSE;
  GST_OBJECT_UNLOCK (self);
  if (decodebin != NULL)
    remove_element (self, decodebin);
}

/* READY to PAUSED: a new run, its stream state from scratch. */
static void
reset_stream (GstRdsUriSrcBin * self)
{
  GST_OBJECT_LOCK (self);
  self->stopping = FALSE;
  self->seen_stream_start = self->seen_segment = FALSE;
  self->looping = self->loop_failed = self->eos_requested = self->eos_out = self->resync = self->discont = FALSE;
  self->loop_seqnum = GST_SEQNUM_INVALID;
  self->offset = 0;
  self->end = GST_CLOCK_TIME_NONE;
  self->frame = 0;
  self->dec_stream_start = self->dec_segment = FALSE;
  self->gate = self->gate_leading = self->reset_pending = self->reconnected_pending = FALSE;
  self->gate_dropped = 0;
  self->gate_pts = GST_CLOCK_TIME_NONE;
  GST_OBJECT_UNLOCK (self);
}

/* An EOS the application sends (to stop) leaves the bin as EOS, file-loop or not (ours). For RTSP the bin ends the
 * stream itself, in a job of the reconnection's (any thread may send the EOS, a session's too; stopping waits for the
 * job, and a job of an earlier run does nothing): rtspsrc pushes an EOS only into a stream that is set up, so a
 * source that is down (connecting, refused, between attempts) would never end, and the attempts would go on. Without
 * vsrc_0 yet the attempts stop, and nothing goes downstream. */
static gboolean
gst_rds_uri_src_bin_send_event (GstElement * element, GstEvent * event)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (element);
  gboolean rtsp = FALSE;

  if (GST_EVENT_TYPE (event) == GST_EVENT_EOS) {
    GST_OBJECT_LOCK (self);
    self->eos_requested = TRUE;
    rtsp = self->is_rtsp;
    GST_OBJECT_UNLOCK (self);
  }
  if (rtsp) {
    gst_event_unref (event);
    rds_urisrc_reconnect_end (self->reconnect);
    return TRUE;
  }
  return GST_ELEMENT_CLASS (gst_rds_uri_src_bin_parent_class)->send_event (element, event);
}

static GstStateChangeReturn
gst_rds_uri_src_bin_change_state (GstElement * element, GstStateChange transition)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (element);
  GstStateChangeReturn ret;
  guint interval, init_interval;
  gint attempts;
  gboolean rtsp;

  GST_OBJECT_LOCK (self);
  rtsp = self->is_rtsp;
  interval = self->reconnect_interval;
  init_interval = self->init_reconnect_interval;
  attempts = self->reconnect_attempts;
  GST_OBJECT_UNLOCK (self);
  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!check_settings (self) || !make_children (self))
        return GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      reset_stream (self);
      /* the parent's change of state takes the session to PAUSED with the other children */
      if (rtsp) {
        rds_urisrc_reconnect_start_run (self->reconnect, interval, init_interval, attempts);
        if (!add_session (self)) {
          rds_urisrc_reconnect_stop (self->reconnect);
          return GST_STATE_CHANGE_FAILURE;
        }
      }
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      if (rtsp)
        rds_urisrc_reconnect_play (self->reconnect);
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      rds_urisrc_reconnect_pause (self->reconnect);
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      GST_OBJECT_LOCK (self);
      self->stopping = TRUE;
      GST_OBJECT_UNLOCK (self);
      /* no attempt from now on, and the one under way finishes first */
      rds_urisrc_reconnect_stop (self->reconnect);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (gst_rds_uri_src_bin_parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (ret == GST_STATE_CHANGE_FAILURE)
        remove_children (self);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      if (ret == GST_STATE_CHANGE_FAILURE && transition == GST_STATE_CHANGE_READY_TO_PAUSED) {
        rds_urisrc_reconnect_stop (self->reconnect);
        remove_session (self);
      }
      /* live, as rtspsrc is: no preroll */
      else if (rtsp && ret != GST_STATE_CHANGE_FAILURE)
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      remove_session (self);
      remove_pad (self);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      remove_children (self);
      break;
    default:
      break;
  }
  return ret;
}

/* ---- properties ---- */

static void
gst_rds_uri_src_bin_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (object);
  gboolean warn = FALSE;

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_URI:
      g_free (self->uri);
      self->uri = g_value_dup_string (value);
      break;
    case PROP_TYPE:
      self->type = g_value_get_enum (value);
      break;
    case PROP_FILE_LOOP:
      self->file_loop = g_value_get_boolean (value);
      break;
    case PROP_LATENCY:
      self->latency = g_value_get_uint (value);
      break;
    case PROP_DROP_ON_LATENCY:
      self->drop_on_latency = g_value_get_boolean (value);
      break;
    case PROP_SELECT_RTP_PROTOCOL:
      self->rtp_protocol = g_value_get_enum (value);
      break;
    case PROP_UDP_BUFFER_SIZE:
      self->udp_buffer_size = g_value_get_uint (value);
      break;
    case PROP_RTSP_RECONNECT_INTERVAL:
      self->reconnect_interval = g_value_get_uint (value);
      break;
    case PROP_RTSP_RECONNECT_ATTEMPTS:
      self->reconnect_attempts = g_value_get_int (value);
      break;
    case PROP_INIT_RTSP_RECONNECT_INTERVAL:
      self->init_reconnect_interval = g_value_get_uint (value);
      break;
    case PROP_DROP_FRAME_INTERVAL:
      self->drop_frame_interval = g_value_get_uint (value);
      break;
    case PROP_SOURCE_ID:
      self->source_id = g_value_get_int (value);
      break;
    case PROP_GPU_ID:
      self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_DISABLE_AUDIO:
      self->disable_audio = g_value_get_boolean (value);
      break;
    case PROP_SMART_RECORD:
      self->smart_record = g_value_get_enum (value);
      break;
    default:
      if (prop_id >= PROP_NOOP_FIRST && prop_id < PROP_NOOP_FIRST + N_NOOPS) {
        g_value_copy (value, &self->noops[prop_id - PROP_NOOP_FIRST]);
        warn = !g_param_value_defaults (pspec, value);
      } else {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
  GST_OBJECT_UNLOCK (self);
  if (warn)
    GST_WARNING_OBJECT (self, "%s is not implemented; the property is accepted and has no effect", pspec->name);
}

static void
gst_rds_uri_src_bin_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_URI:
      g_value_set_string (value, self->uri);
      break;
    case PROP_TYPE:
      g_value_set_enum (value, self->type);
      break;
    case PROP_FILE_LOOP:
      g_value_set_boolean (value, self->file_loop);
      break;
    case PROP_LATENCY:
      g_value_set_uint (value, self->latency);
      break;
    case PROP_DROP_ON_LATENCY:
      g_value_set_boolean (value, self->drop_on_latency);
      break;
    case PROP_SELECT_RTP_PROTOCOL:
      g_value_set_enum (value, self->rtp_protocol);
      break;
    case PROP_UDP_BUFFER_SIZE:
      g_value_set_uint (value, self->udp_buffer_size);
      break;
    case PROP_RTSP_RECONNECT_INTERVAL:
      g_value_set_uint (value, self->reconnect_interval);
      break;
    case PROP_RTSP_RECONNECT_ATTEMPTS:
      g_value_set_int (value, self->reconnect_attempts);
      break;
    case PROP_INIT_RTSP_RECONNECT_INTERVAL:
      g_value_set_uint (value, self->init_reconnect_interval);
      break;
    case PROP_DROP_FRAME_INTERVAL:
      g_value_set_uint (value, self->drop_frame_interval);
      break;
    case PROP_SOURCE_ID:
      g_value_set_int (value, self->source_id);
      break;
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_DISABLE_AUDIO:
      g_value_set_boolean (value, self->disable_audio);
      break;
    case PROP_SMART_RECORD:
      g_value_set_enum (value, self->smart_record);
      break;
    case PROP_SMART_REC_STATUS:
      /* smart record never runs */
      g_value_set_boolean (value, FALSE);
      break;
    default:
      if (prop_id >= PROP_NOOP_FIRST && prop_id < PROP_NOOP_FIRST + N_NOOPS)
        g_value_copy (&self->noops[prop_id - PROP_NOOP_FIRST], value);
      else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_uri_src_bin_finalize (GObject * object)
{
  GstRdsUriSrcBin *self = GST_RDS_URI_SRC_BIN (object);

  g_free (self->uri);
  for (guint i = 0; i < N_NOOPS; i++)
    g_value_unset (&self->noops[i]);
  rds_urisrc_reconnect_free (self->reconnect);
  G_OBJECT_CLASS (gst_rds_uri_src_bin_parent_class)->finalize (object);
}

static void
gst_rds_uri_src_bin_init (GstRdsUriSrcBin * self)
{
  self->type = GST_RDS_URI_SRC_BIN_TYPE_AUTO;
  self->latency = DEFAULT_LATENCY;
  self->drop_on_latency = DEFAULT_DROP_ON_LATENCY;
  self->rtp_protocol = DEFAULT_RTP_PROTOCOL;
  self->udp_buffer_size = DEFAULT_UDP_BUFFER_SIZE;
  self->reconnect_interval = DEFAULT_RECONNECT_INTERVAL;
  self->reconnect_attempts = DEFAULT_RECONNECT_ATTEMPTS;
  self->source_id = DEFAULT_SOURCE_ID;
  self->disable_audio = DEFAULT_DISABLE_AUDIO;
  self->smart_record = GST_RDS_URI_SRC_BIN_SMART_REC_DISABLE;
  for (guint i = 0; i < N_NOOPS; i++) {
    g_value_init (&self->noops[i], G_PARAM_SPEC_VALUE_TYPE (noop_specs[i]));
    g_param_value_set_default (noop_specs[i], &self->noops[i]);
  }

  /* nvurisrcbin's default */
  g_object_set (self, "async-handling", TRUE, NULL);
  self->reconnect = rds_urisrc_reconnect_new (GST_ELEMENT (self), &reconnect_callbacks);

  /* no pad here: vsrc_0 comes from the vsrc_%u template when the first video stream is decoded */
}

static void
gst_rds_uri_src_bin_class_init (GstRdsUriSrcBinClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBinClass *bin_class = GST_BIN_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;
  /* the properties DeepStream 8.0 marks changeable only in NULL or READY */
  const GParamFlags ready = flags | GST_PARAM_MUTABLE_READY;

  GST_DEBUG_CATEGORY_INIT (gst_rds_uri_src_bin_debug, "rdsurisrcbin", 0, "Redstream URI source bin");

  gobject_class->set_property = gst_rds_uri_src_bin_set_property;
  gobject_class->get_property = gst_rds_uri_src_bin_get_property;
  gobject_class->finalize = gst_rds_uri_src_bin_finalize;

  /* Names, types, ranges and defaults as in DeepStream 8.0 behaviour, which differs from NVIDIA's nvurisrcbin pages
   * in some defaults (rtsp-reconnect-interval, rtsp-reconnect-attempts, source-id, select-rtp-protocol, buffer-mode,
   * max-size-buffers, disable-passthrough). Two properties 8.0 lacks come from the 9.2 page: simulate-fps-interval-ms
   * and ipc-frame-copy. */
  g_object_class_install_property (gobject_class, PROP_URI,
      g_param_spec_string ("uri", "URI", "URI of the file or RTSP source", NULL, ready));
  g_object_class_install_property (gobject_class, PROP_TYPE,
      g_param_spec_enum ("type", "Type", "Type of source (rtsp: the uri must be an RTSP URI)",
          gst_rds_uri_src_bin_type_get_type (), GST_RDS_URI_SRC_BIN_TYPE_AUTO, ready));
  g_object_class_install_property (gobject_class, PROP_FILE_LOOP,
      g_param_spec_boolean ("file-loop", "File loop", "Loop file:// sources after EOS", FALSE, flags));
  g_object_class_install_property (gobject_class, PROP_LATENCY,
      g_param_spec_uint ("latency", "Latency", "Jitterbuffer size in milliseconds (RTSP)", 0, G_MAXUINT,
          DEFAULT_LATENCY, ready));
  g_object_class_install_property (gobject_class, PROP_DROP_ON_LATENCY,
      g_param_spec_boolean ("drop-on-latency", "Drop on latency",
          "Tells the jitterbuffer to never exceed the given latency in size (RTSP)", DEFAULT_DROP_ON_LATENCY, flags));
  g_object_class_install_property (gobject_class, PROP_SELECT_RTP_PROTOCOL,
      g_param_spec_enum ("select-rtp-protocol", "RTP protocol", "Transport protocol to use for RTP (RTSP)",
          gst_rds_uri_src_bin_rtp_protocol_get_type (), DEFAULT_RTP_PROTOCOL, ready));
  g_object_class_install_property (gobject_class, PROP_UDP_BUFFER_SIZE,
      g_param_spec_uint ("udp-buffer-size", "UDP buffer size", "UDP buffer size in bytes (RTSP)", 0, G_MAXUINT,
          DEFAULT_UDP_BUFFER_SIZE, ready));
  g_object_class_install_property (gobject_class, PROP_RTSP_RECONNECT_INTERVAL,
      g_param_spec_uint ("rtsp-reconnect-interval", "RTSP reconnect interval",
          "Seconds without data from an RTSP source before reconnecting (0: never)", 0, G_MAXUINT,
          DEFAULT_RECONNECT_INTERVAL, ready));
  g_object_class_install_property (gobject_class, PROP_RTSP_RECONNECT_ATTEMPTS,
      g_param_spec_int ("rtsp-reconnect-attempts", "RTSP reconnect attempts",
          "Maximum number of reconnection attempts (negative: unlimited)", G_MININT, G_MAXINT,
          DEFAULT_RECONNECT_ATTEMPTS, ready));
  g_object_class_install_property (gobject_class, PROP_INIT_RTSP_RECONNECT_INTERVAL,
      g_param_spec_uint ("init-rtsp-reconnect-interval", "Initial RTSP reconnect interval",
          "Seconds to wait before retrying when the first RTSP connection fails (0: no retry)", 0, G_MAXUINT, 0,
          ready));
  g_object_class_install_property (gobject_class, PROP_DROP_FRAME_INTERVAL,
      g_param_spec_uint ("drop-frame-interval", "Drop frame interval",
          "Output every Nth decoded frame and drop the others (0, 1: every frame)", 0, 30, 0, ready));
  g_object_class_install_property (gobject_class, PROP_SOURCE_ID,
      g_param_spec_int ("source-id", "Source id", "Unique id of the input source", -1, G_MAXINT,
          DEFAULT_SOURCE_ID, ready));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU id", "GPU used by the decoder and converter", 0, G_MAXUINT, 0, ready));
  g_object_class_install_property (gobject_class, PROP_DISABLE_AUDIO,
      g_param_spec_boolean ("disable-audio", "Disable audio", "Disable the audio path", DEFAULT_DISABLE_AUDIO,
          flags));
  g_object_class_install_property (gobject_class, PROP_SMART_RECORD,
      g_param_spec_enum ("smart-record", "Smart record", "Smart record (not supported: anything but "
          "smart-rec-disable fails at start)", gst_rds_uri_src_bin_smart_record_get_type (),
          GST_RDS_URI_SRC_BIN_SMART_REC_DISABLE, flags));
  g_object_class_install_property (gobject_class, PROP_SMART_REC_STATUS,
      g_param_spec_boolean ("smart-rec-status", "Smart record status", "Whether smart record is active (always "
          "FALSE)", FALSE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

#define NOOP_DESC "Accepted for compatibility; has no effect"
  noop_specs[NOOP_NUM_EXTRA_SURFACES] = g_param_spec_uint ("num-extra-surfaces", "Extra surfaces", NOOP_DESC, 0,
      G_MAXUINT, 1, ready);
  noop_specs[NOOP_CUDADEC_MEMTYPE] = g_param_spec_enum ("cudadec-memtype", "Decoder memory type", NOOP_DESC,
      gst_rds_uri_src_bin_cudadec_memtype_get_type (), 0, ready);
  noop_specs[NOOP_DEC_SKIP_FRAMES] = g_param_spec_enum ("dec-skip-frames", "Skip frames", NOOP_DESC,
      gst_rds_uri_src_bin_skip_frames_get_type (), 0, ready);
  noop_specs[NOOP_LOW_LATENCY_MODE] = g_param_spec_boolean ("low-latency-mode", "Low latency mode", NOOP_DESC,
      FALSE, flags);
  noop_specs[NOOP_SMART_REC_DIR_PATH] = g_param_spec_string ("smart-rec-dir-path", "Smart record directory",
      NOOP_DESC, NULL, ready);
  noop_specs[NOOP_SMART_REC_FILE_PREFIX] = g_param_spec_string ("smart-rec-file-prefix", "Smart record file prefix",
      NOOP_DESC, "Smart_Record", ready);
  noop_specs[NOOP_SMART_REC_VIDEO_CACHE] = g_param_spec_uint ("smart-rec-video-cache", "Smart record video cache",
      NOOP_DESC, 0, G_MAXUINT, 0, ready);
  noop_specs[NOOP_SMART_REC_CACHE] = g_param_spec_uint ("smart-rec-cache", "Smart record cache", NOOP_DESC, 0,
      G_MAXUINT, 0, ready);
  noop_specs[NOOP_SMART_REC_CONTAINER] = g_param_spec_enum ("smart-rec-container", "Smart record container",
      NOOP_DESC, gst_rds_uri_src_bin_smart_rec_container_get_type (), 0, flags);
  noop_specs[NOOP_SMART_REC_MODE] = g_param_spec_enum ("smart-rec-mode", "Smart record mode", NOOP_DESC,
      gst_rds_uri_src_bin_smart_rec_mode_get_type (), 0, flags);
  noop_specs[NOOP_SMART_REC_DEFAULT_DURATION] = g_param_spec_uint ("smart-rec-default-duration",
      "Smart record default duration", NOOP_DESC, 0, G_MAXUINT, 20, ready);
  noop_specs[NOOP_IPC_BUFFER_TIMESTAMP_COPY] = g_param_spec_boolean ("ipc-buffer-timestamp-copy",
      "IPC buffer timestamp copy", NOOP_DESC, FALSE, flags);
  noop_specs[NOOP_IPC_CONNECTION_ATTEMPTS] = g_param_spec_int ("ipc-connection-attempts", "IPC connection attempts",
      NOOP_DESC, -1, G_MAXINT, -1, flags);
  noop_specs[NOOP_IPC_CONNECTION_INTERVAL] = g_param_spec_uint64 ("ipc-connection-interval",
      "IPC connection interval", NOOP_DESC, 0, G_MAXUINT64, 1000000, flags);
  noop_specs[NOOP_IPC_SOCKET_PATH] = g_param_spec_string ("ipc-socket-path", "IPC socket path", NOOP_DESC, NULL,
      ready);
  noop_specs[NOOP_IPC_FRAME_COPY] = g_param_spec_boolean ("ipc-frame-copy", "IPC frame copy", NOOP_DESC, FALSE,
      flags);
  noop_specs[NOOP_EXTRACT_SEI_TYPE5_DATA] = g_param_spec_boolean ("extract-sei-type5-data", "Extract SEI type 5 data",
      NOOP_DESC, FALSE, flags);
  noop_specs[NOOP_SEI_UUID] = g_param_spec_string ("sei-uuid", "SEI UUID", NOOP_DESC, NULL, ready);
  noop_specs[NOOP_SIMULATE_FPS_INTERVAL_MS] = g_param_spec_uint ("simulate-fps-interval-ms",
      "Simulated frame interval", NOOP_DESC, 0, 1000, 0, flags);
  noop_specs[NOOP_BUFFER_MODE] = g_param_spec_enum ("buffer-mode", "Buffer mode", NOOP_DESC,
      gst_rds_uri_src_bin_buffer_mode_get_type (), 3, ready);
  noop_specs[NOOP_LEAKY] = g_param_spec_enum ("leaky", "Leaky", NOOP_DESC, gst_rds_uri_src_bin_leaky_get_type (), 0,
      ready);
  noop_specs[NOOP_MAX_SIZE_BUFFERS] = g_param_spec_uint ("max-size-buffers", "Max size buffers", NOOP_DESC, 0,
      G_MAXUINT, 200, ready);
  noop_specs[NOOP_DISABLE_PASSTHROUGH] = g_param_spec_boolean ("disable-passthrough", "Disable passthrough",
      NOOP_DESC, TRUE, flags);
#undef NOOP_DESC
  for (guint i = 0; i < N_NOOPS; i++)
    g_object_class_install_property (gobject_class, PROP_NOOP_FIRST + i, noop_specs[i]);

  gst_element_class_add_static_pad_template (element_class, &vsrc_template);
  gst_element_class_set_static_metadata (element_class, "Redstream URI source bin", "Source/Video/Bin",
      "Decodes file and RTSP sources to memory:RDS frames", "Redstream contributors");

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_rds_uri_src_bin_change_state);
  element_class->send_event = GST_DEBUG_FUNCPTR (gst_rds_uri_src_bin_send_event);
  bin_class->handle_message = GST_DEBUG_FUNCPTR (gst_rds_uri_src_bin_handle_message);
}
