/* Redstream: GstRdsEncBin, the base of rdsh264enc and rdsh265enc, standing in for DeepStream's nvv4l2h264enc and
 * nvv4l2h265enc. A bin:
 *
 *   ghost sink pad -> rdsvideoconvert -> video/x-raw,format=NV12 -> vah26xenc -> capsfilter -> ghost src pad
 *
 * The frames, memory:RDS or system memory (ours: DeepStream 8.0 takes NVMM only), are converted to system-memory
 * NV12 and encoded by GStreamer's stock VA encoder. The inner elements are made when the bin first goes to READY, so
 * the elements always register and a missing VA encoder is an element error at start. Every property gst-inspect
 * shows for DeepStream 8.0's encoders keeps its name, type, range and default; the flags say what really happens
 * (ours: 8.0's are wrong both ways): bitrate, force-idr and force-intra are live, every other writable property is
 * read at READY to PAUSED (set later, it is stored with a warning and applied at the next start). The properties
 * the VA encoder has no counterpart for are stored and do nothing; a value other than the default logs one warning
 * per property (ours). Pad probes, not extra elements, shape the VA encoder's output: filler NAL units stripped, the
 * H.265 conformance window fixed, the input's timestamps and segment restored (without B frames), DELTA_UNIT set from
 * the picture type, the input's framerate kept in the caps, batches refused and, with copy-meta, the input's metas
 * copied over. Behaviour matching DeepStream 8.0 is marked "DeepStream 8.0 behaviour", our choices "ours"; every
 * such choice has a row in docs/compat-matrix.md. The pad probes are in gstrdsencbin-probes.c, the VA encoder's
 * configuration in gstrdsencbin-va.c, the shared struct and the locking rules in gstrdsencbin-private.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsencbin.h"
#include "gstrdsencbin-private.h"

#include "rds-enc-props.h"
#include "rds-video-convert-caps.h"

GST_DEBUG_CATEGORY (gst_rds_enc_bin_debug);
#define GST_CAT_DEFAULT gst_rds_enc_bin_debug

/* DeepStream 8.0's defaults, as gst-inspect shows them */
#define DEFAULT_BITRATE 4000000
#define DEFAULT_CONTROL_RATE 1  /* constant_bitrate */
#define DEFAULT_IDRINTERVAL 256
#define DEFAULT_IFRAMEINTERVAL G_MAXUINT        /* none */
#define DEFAULT_PRESET_ID 1
#define DEFAULT_TUNING_INFO_ID 2        /* LowLatencyPreset */
#define DEFAULT_INTRA_REFRESH "1,30,1"
#define DEFAULT_SEI_UUID "NVDS_CUSTOMMETA"
#define DEFAULT_DEVICE "/dev/v4l2-nvenc"

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GstRdsEncBin, gst_rds_enc_bin, GST_TYPE_BIN);

GstRdsEncBinPrivate *
gst_rds_enc_bin_get_priv (GstRdsEncBin * self)
{
  return gst_rds_enc_bin_get_instance_private (self);
}

/* what rdsvideoconvert takes, memory:RDS or system memory */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_VIDEO_CONVERT_CAPS));

/* ---- enums (DeepStream 8.0's values and nicks; type names and descriptions are ours) ---- */

RDS_ENC_DEFINE_ENUM (gst_rds_enc_rate_control_get_type, "GstRdsEncRateControl",
    {0, "Variable bitrate", "variable_bitrate"},
    {1, "Constant bitrate", "constant_bitrate"},
    {2, "Constant QP", "constantQP"});

RDS_ENC_DEFINE_ENUM (gst_rds_enc_io_mode_get_type, "GstRdsEncIOMode",
    {0, "Automatic", "auto"},
    {2, "Memory-mapped buffers", "mmap"},
    {5, "Imported DMABufs", "dmabuf-import"});

RDS_ENC_DEFINE_ENUM (gst_rds_enc_cuda_mem_type_get_type, "GstRdsEncCudaMemType",
    {0, "Device memory", "memtype_device"},
    {1, "Pinned host memory", "memtype_pinned"},
    {2, "Unified memory", "memtype_unified"});

RDS_ENC_DEFINE_ENUM (gst_rds_enc_tuning_info_get_type, "GstRdsEncTuningInfo",
    {1, "Tuned for quality", "HighQualityPreset"},
    {2, "Tuned for low latency", "LowLatencyPreset"},
    {3, "Tuned for the lowest latency", "UltraLowLatencyPreset"},
    {4, "Tuned for lossless coding", "LosslessPreset"});

/* ---- the inner chain ---- */

/* Releases a new element that was never added to the bin. */
static void
drop_floating (GstElement ** e)
{
  if (*e != NULL)
    gst_object_unref (gst_object_ref_sink (*e));
  *e = NULL;
}

/* Makes and links the inner elements and points the ghost pads at them, once. FALSE (with an element error naming
 * what is missing): nothing is left in the bin. Called with the state lock held. */
static gboolean
build_chain (GstRdsEncBin * self)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  const GstRdsEncCodecInfo *info = GST_RDS_ENC_BIN_GET_CLASS (self)->info;
  GstElement *convert, *rawcaps, *encoder, *srccaps;
  GstPad *pad;
  GstCaps *caps;

  GST_OBJECT_LOCK (self);
  encoder = priv->encoder;
  GST_OBJECT_UNLOCK (self);
  if (encoder != NULL)
    return TRUE;

  convert = gst_element_factory_make ("rdsvideoconvert", "convert");
  rawcaps = gst_element_factory_make ("capsfilter", "rawcaps");
  encoder = gst_element_factory_make (info->va_factory, "encoder");
  srccaps = gst_element_factory_make ("capsfilter", "srccaps");
  if (convert == NULL || rawcaps == NULL || encoder == NULL || srccaps == NULL) {
    if (encoder == NULL)
      GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("The VA encoder '%s' is not available", info->va_factory),
          ("no element '%s': GStreamer's va plugin is missing, or the VA driver can't encode %s", info->va_factory,
              info->codec_name));
    else
      GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("An inner element is missing"),
          ("needs rdsvideoconvert and capsfilter"));
    drop_floating (&convert);
    drop_floating (&rawcaps);
    drop_floating (&encoder);
    drop_floating (&srccaps);
    return FALSE;
  }

  caps = gst_caps_from_string ("video/x-raw, format = (string) NV12");
  g_object_set (rawcaps, "caps", caps, NULL);
  gst_caps_unref (caps);
  caps = gst_caps_new_simple (info->caps_name, "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
      G_TYPE_STRING, "au", NULL);
  g_object_set (srccaps, "caps", caps, NULL);
  gst_caps_unref (caps);

  gst_bin_add_many (GST_BIN (self), convert, rawcaps, encoder, srccaps, NULL);
  if (!gst_element_link_many (convert, rawcaps, encoder, srccaps, NULL)) {
    GstElement *made[] = { convert, rawcaps, encoder, srccaps };

    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("Cannot link the inner elements"),
        ("rdsvideoconvert ! capsfilter ! %s ! capsfilter", info->va_factory));
    for (guint i = 0; i < G_N_ELEMENTS (made); i++) {
      gst_element_set_state (made[i], GST_STATE_NULL);
      gst_bin_remove (GST_BIN (self), made[i]);
    }
    return FALSE;
  }

  pad = gst_element_get_static_pad (convert, "sink");
  gst_ghost_pad_set_target (GST_GHOST_PAD (priv->sink_ghost), pad);
  gst_object_unref (pad);
  pad = gst_element_get_static_pad (srccaps, "src");
  gst_ghost_pad_set_target (GST_GHOST_PAD (priv->src_ghost), pad);
  gst_object_unref (pad);

  gst_rds_enc_bin_add_probes (self, encoder);

  GST_OBJECT_LOCK (self);
  priv->convert = convert;
  priv->rawcaps = rawcaps;
  priv->encoder = encoder;
  priv->srccaps = srccaps;
  GST_OBJECT_UNLOCK (self);
  return TRUE;
}

/* The VA encoder, with a reference, or NULL before the first start: for the paths that reach it while streaming
 * (live properties, events), which use it after dropping the object lock. */
static GstElement *
get_encoder (GstRdsEncBin * self)
{
  GstElement *e;

  GST_OBJECT_LOCK (self);
  e = PRIV (self)->encoder != NULL ? gst_object_ref (PRIV (self)->encoder) : NULL;
  GST_OBJECT_UNLOCK (self);
  return e;
}

/* Clears the per-run state (PAUSED to READY, or a failed start). */
static void
reset_run (GstRdsEncBin * self)
{
  GstRdsEncBinPrivate *priv = PRIV (self);

  GST_OBJECT_LOCK (self);
  priv->va_profile = NULL;
  priv->b_frames = 0;
  priv->b_frames_set = 0;
  priv->b_frames_warned = FALSE;
  priv->level_warned = FALSE;
  GST_OBJECT_UNLOCK (self);
  gst_rds_enc_bin_reset_probes (self, FALSE);
}

static GstStateChangeReturn
gst_rds_enc_bin_change_state (GstElement * element, GstStateChange transition)
{
  GstRdsEncBin *self = GST_RDS_ENC_BIN (element);
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!build_chain (self))
        return GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* before the chain-up: the VA encoder reads its properties when it configures, after READY */
      if (!gst_rds_enc_bin_configure (self))
        return GST_STATE_CHANGE_FAILURE;
      break;
    default:
      break;
  }
  ret = GST_ELEMENT_CLASS (gst_rds_enc_bin_parent_class)->change_state (element, transition);
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY
      || (transition == GST_STATE_CHANGE_READY_TO_PAUSED && ret == GST_STATE_CHANGE_FAILURE))
    reset_run (self);
  return ret;
}

/* ---- properties ---- */

/* Started (PAUSED or PLAYING, or on the way there). Object lock held. */
static gboolean
is_started (GstRdsEncBin * self)
{
  return MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self))) > GST_STATE_READY;
}

/* A QP string property's new value @s is well formed (NULL unsets it). */
static gboolean
qp_string_ok (guint prop_id, const gchar * s)
{
  guint triplet[3], range[6];

  if (s == NULL)
    return TRUE;
  if (prop_id == PROP_QP_RANGE)
    return rds_enc_parse_qp_range (s, range);
  return rds_enc_parse_qp_triplet (s, triplet);
}

static void
gst_rds_enc_bin_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsEncBin *self = GST_RDS_ENC_BIN (object);
  GstRdsEncBinPrivate *priv = PRIV (self);
  gboolean warn_noop = FALSE, warn_started = FALSE;

  if (prop_id >= N_PROPS) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    return;
  }

  /* ours: DeepStream 8.0 crashes on a malformed string */
  if ((prop_id == PROP_CONSTQP || prop_id == PROP_QP_RANGE || prop_id == PROP_INITQP)
      && !qp_string_ok (prop_id, g_value_get_string (value))) {
    g_warning ("%s: malformed %s \"%s\" (expected %s, each 0..%d); keeping the previous value",
        GST_OBJECT_NAME (self), pspec->name, g_value_get_string (value),
        prop_id == PROP_QP_RANGE ? "minP,maxP:minI,maxI:minB,maxB" : "I:P:B", RDS_ENC_QP_MAX);
    return;
  }

  /* force-idr and force-intra are requests, never stored: they read back FALSE */
  if (prop_id == PROP_FORCE_IDR || prop_id == PROP_FORCE_INTRA) {
    GST_DEBUG_OBJECT (self, "%s %d", pspec->name, g_value_get_boolean (value));
    return;
  }

  GST_OBJECT_LOCK (self);
  g_value_copy (value, &priv->values[prop_id]);
  if (IS_NOOP (prop_id)) {
    guint bit = 1u << (prop_id - PROP_NOOP_FIRST);

    if (!g_param_value_defaults (pspec, value) && !(priv->noops_warned & bit)) {
      priv->noops_warned |= bit;
      warn_noop = TRUE;
    }
  } else if (!(pspec->flags & GST_PARAM_MUTABLE_PLAYING) && is_started (self)) {
    /* ours: DeepStream 8.0 takes some of them in PLAYING and silently ignores others */
    warn_started = TRUE;
  }
  GST_OBJECT_UNLOCK (self);

  if (warn_noop)
    g_warning ("%s: %s is not implemented; the property is accepted and has no effect", GST_OBJECT_NAME (self),
        pspec->name);
  if (warn_started)
    g_warning ("%s: %s set after the start is stored and applied at the next start (NULL or READY to PAUSED)",
        GST_OBJECT_NAME (self), pspec->name);

  /* bitrate is live (DeepStream 8.0 behaviour): on VA a change forces an IDR and restarts rate control (ours) */
  if (prop_id == PROP_BITRATE) {
    GstElement *encoder;
    guint bitrate;

    /* the latest stored value, under va_lock: a concurrent forward or start can't leave an older one on VA */
    g_mutex_lock (&priv->va_lock);
    encoder = get_encoder (self);
    GST_OBJECT_LOCK (self);
    bitrate = g_value_get_uint (&priv->values[PROP_BITRATE]);
    GST_OBJECT_UNLOCK (self);
    if (encoder != NULL) {
      gst_rds_enc_bin_set_va_uint (G_OBJECT (encoder), "bitrate", rds_enc_kbps (bitrate));
      gst_object_unref (encoder);
    }
    g_mutex_unlock (&priv->va_lock);
  }
}

static void
gst_rds_enc_bin_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsEncBin *self = GST_RDS_ENC_BIN (object);

  if (prop_id >= N_PROPS) {
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    return;
  }
  if (prop_id == PROP_FORCE_IDR || prop_id == PROP_FORCE_INTRA) {
    g_value_set_boolean (value, FALSE);
    return;
  }
  GST_OBJECT_LOCK (self);
  g_value_copy (&PRIV (self)->values[prop_id], value);
  GST_OBJECT_UNLOCK (self);
}

/* ---- life cycle ---- */

static void
gst_rds_enc_bin_constructed (GObject * object)
{
  GstRdsEncBin *self = GST_RDS_ENC_BIN (object);
  GstRdsEncBinPrivate *priv = PRIV (self);
  GstRdsEncBinClass *klass = GST_RDS_ENC_BIN_GET_CLASS (self);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  G_OBJECT_CLASS (gst_rds_enc_bin_parent_class)->constructed (object);

  /* the codec's specs live on the subclass, so the values and pads are set up here rather than in init */
  for (guint i = 1; i < N_PROPS; i++) {
    if (klass->specs[i] == NULL)
      continue;
    g_value_init (&priv->values[i], G_PARAM_SPEC_VALUE_TYPE (klass->specs[i]));
    g_param_value_set_default (klass->specs[i], &priv->values[i]);
  }

  priv->sink_ghost = gst_ghost_pad_new_no_target_from_template ("sink",
      gst_element_class_get_pad_template (element_class, "sink"));
  gst_element_add_pad (GST_ELEMENT (self), priv->sink_ghost);
  priv->src_ghost = gst_ghost_pad_new_no_target_from_template ("src",
      gst_element_class_get_pad_template (element_class, "src"));
  gst_element_add_pad (GST_ELEMENT (self), priv->src_ghost);
}

static void
gst_rds_enc_bin_finalize (GObject * object)
{
  GstRdsEncBinPrivate *priv = PRIV (GST_RDS_ENC_BIN (object));

  for (guint i = 0; i < N_PROPS; i++) {
    if (G_IS_VALUE (&priv->values[i]))
      g_value_unset (&priv->values[i]);
  }
  gst_rds_enc_bin_reset_probes (GST_RDS_ENC_BIN (object), FALSE);
  g_mutex_clear (&priv->probe_lock);
  g_mutex_clear (&priv->va_lock);
  G_OBJECT_CLASS (gst_rds_enc_bin_parent_class)->finalize (object);
}

static void
gst_rds_enc_bin_init (GstRdsEncBin * self)
{
  GstRdsEncBinPrivate *priv = PRIV (self);

  g_mutex_init (&priv->va_lock);
  g_mutex_init (&priv->probe_lock);
  g_queue_init (&priv->in_segments);
  g_queue_init (&priv->records);
  gst_segment_init (&priv->last_in_segment, GST_FORMAT_UNDEFINED);
}

static void
gst_rds_enc_bin_class_init (GstRdsEncBinClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_rds_enc_bin_debug, "rdsenc", 0, "Redstream H.264 / H.265 encoders");

  gobject_class->constructed = gst_rds_enc_bin_constructed;
  gobject_class->finalize = gst_rds_enc_bin_finalize;
  element_class->change_state = GST_DEBUG_FUNCPTR (gst_rds_enc_bin_change_state);
}

/* ---- the codec's class ---- */

void
gst_rds_enc_bin_class_setup (GstRdsEncBinClass * klass, const GstRdsEncCodecInfo * info)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  /* live: takes effect in PLAYING; stored: read at the next start (ours: the flags say what happens) */
  const GParamFlags live = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING;
  const GParamFlags stored = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY;
  const GParamFlags ro = G_PARAM_READABLE | G_PARAM_STATIC_STRINGS;
  GParamSpec **s = g_new0 (GParamSpec *, N_PROPS);
  GstCaps *caps;
  gchar *long_name, *desc;

  klass->info = info;
  klass->specs = s;
  /* GObject doesn't inherit these: the class that installs the properties sets them */
  gobject_class->set_property = gst_rds_enc_bin_set_property;
  gobject_class->get_property = gst_rds_enc_bin_get_property;

  /* names, types, ranges and defaults as gst-inspect shows them for DeepStream 8.0's nvv4l2h264enc / nvv4l2h265enc */
  s[PROP_BITRATE] = g_param_spec_uint ("bitrate", "Bitrate",
      "Target bitrate in bit/s (sent to the VA encoder in kbit/s; can be changed while playing)", 0, G_MAXUINT,
      DEFAULT_BITRATE, live);
  s[PROP_CONTROL_RATE] = g_param_spec_enum ("control-rate", "Rate control", "Rate control mode",
      gst_rds_enc_rate_control_get_type (), DEFAULT_CONTROL_RATE, stored);
  s[PROP_MAXBITRATE] = g_param_spec_uint ("maxbitrate", "Maximum bitrate",
      "Peak bitrate in bit/s with variable_bitrate (0: none)", 0, G_MAXUINT, 0, stored);
  s[PROP_CONSTQP] = g_param_spec_string ("constqp", "Constant QP",
      "QP of I, P and B frames with constantQP, as \"I:P:B\" (each 0-51)", NULL, stored);
  s[PROP_QP_RANGE] = g_param_spec_string ("qp-range", "QP range",
      "Smallest and largest QP of P, I and B frames, as \"minP,maxP:minI,maxI:minB,maxB\" (each 0-51)", NULL,
      stored);
  s[PROP_VBVBUFSIZE] = g_param_spec_uint ("vbvbufsize", "VBV buffer size",
      "Size of the rate-control buffer in bits (0: the encoder's choice)", 0, G_MAXUINT, 0, stored);
  s[PROP_IDRINTERVAL] = g_param_spec_uint ("idrinterval", "IDR interval",
      "Frames from one IDR frame to the next (0: 256)", 0, G_MAXUINT, DEFAULT_IDRINTERVAL, stored);
  s[PROP_IFRAMEINTERVAL] = g_param_spec_uint ("iframeinterval", "I-frame interval",
      "Frames from one intra frame to the next between IDR frames (0 or 4294967295: none)", 0, G_MAXUINT,
      DEFAULT_IFRAMEINTERVAL, stored);
  s[PROP_FORCE_IDR] = g_param_spec_boolean ("force-idr", "Force IDR",
      "Set to TRUE to make the next frame an IDR frame; always reads FALSE", FALSE, live);
  s[PROP_FORCE_INTRA] = g_param_spec_boolean ("force-intra", "Force intra",
      "Set to TRUE to make the next frame an intra frame (an IDR frame here); always reads FALSE", FALSE, live);
  s[PROP_PROFILE] = g_param_spec_enum ("profile", "Profile", "Preferred profile (output caps may impose another)",
      info->profile_type (), 0, stored);
  s[PROP_NUM_B_FRAMES] = g_param_spec_uint ("num-B-Frames", "B frames",
      "B frames between two reference frames", 0, 2, 0, stored);
  s[PROP_NUM_REF_FRAMES] = g_param_spec_uint ("num-Ref-Frames", "Reference frames",
      "Reference frames for inter prediction (0: the encoder's default)", 0, 8, 0, stored);
  if (info->has_cabac)
    s[PROP_DISABLE_CABAC] = g_param_spec_boolean ("disable-cabac", "Disable CABAC",
        "Entropy coding: TRUE for CAVLC, FALSE for CABAC", FALSE, stored);
  s[PROP_INSERT_AUD] = g_param_spec_boolean ("insert-aud", "Insert AUD",
      "Write an access unit delimiter before every frame", FALSE, stored);
  s[PROP_MIN_FORCE_KEY_UNIT_INTERVAL] = g_param_spec_uint64 ("min-force-key-unit-interval",
      "Minimum force-key-unit interval", "Shortest time between two forced key frames, in nanoseconds", 0,
      G_MAXUINT64, 0, stored);
  s[PROP_QOS] = g_param_spec_boolean ("qos", "QoS", "Drop frames on quality-of-service events from downstream",
      FALSE, stored);
  s[PROP_COPY_META] = g_param_spec_boolean ("copy-meta", "Copy meta",
      "Copy the input buffers' metadata onto the encoded buffers", FALSE, stored);
  s[PROP_GPU_ID] = g_param_spec_uint ("gpu-id", "GPU id", "GPU that converts the frames before encoding", 0,
      G_MAXUINT, 0, stored);

#define NOOP(what) what " (accepted for compatibility; has no effect)"
  s[PROP_PRESET_ID] = g_param_spec_uint ("preset-id", "Preset", NOOP ("Speed/quality preset, 1 fastest to 7 best"),
      1, 7, DEFAULT_PRESET_ID, stored);
  s[PROP_INSERT_SPS_PPS] = g_param_spec_boolean ("insert-sps-pps", "Insert parameter sets",
      NOOP ("Write the parameter sets before every IDR frame (always done)"), FALSE, stored);
  s[PROP_INITQP] = g_param_spec_string ("initqp", "Initial QP",
      NOOP ("Starting QP of I, P and B frames, as \"I:P:B\" (each 0-51)"), NULL, stored);
  s[PROP_CQ] = g_param_spec_uint ("cq", "Constant quality",
      NOOP ("Target quality with variable_bitrate, 0-51 (0: automatic)"), 0, 51, 0, stored);
  s[PROP_VBVINIT] = g_param_spec_uint ("vbvinit", "VBV initial delay",
      NOOP ("Initial fullness of the rate-control buffer in bits"), 0, G_MAXUINT, 0, stored);
  s[PROP_AQ] = g_param_spec_uint ("aq", "Spatial AQ", NOOP ("Spatial adaptive quantization strength, 1-15 "
          "(0: automatic)"), 0, 15, 0, stored);
  s[PROP_TEMPORALAQ] = g_param_spec_boolean ("temporalaq", "Temporal AQ", NOOP ("Temporal adaptive quantization"),
      FALSE, stored);
  s[PROP_INTRA_REFRESH] = g_param_spec_string ("intra-refresh", "Intra refresh",
      NOOP ("Gradual intra refresh as \"enable,period,count\""), DEFAULT_INTRA_REFRESH, stored);
  s[PROP_TUNING_INFO_ID] = g_param_spec_enum ("tuning-info-id", "Tuning", NOOP ("What the presets are tuned for"),
      gst_rds_enc_tuning_info_get_type (), DEFAULT_TUNING_INFO_ID, stored);
  s[PROP_INSERT_VUI] = g_param_spec_boolean ("insert-vui", "Insert VUI",
      NOOP ("Write video usability information in the sequence parameter set"), TRUE, stored);
  s[PROP_EXTENDED_COLORFORMAT] = g_param_spec_boolean ("extended-colorformat", "Full-range colour",
      NOOP ("Signal full-range (0-255) samples in the VUI"), FALSE, stored);
  s[PROP_SEI_UUID] = g_param_spec_string ("sei-uuid", "SEI UUID", NOOP ("16-byte UUID of user-data SEI messages"),
      DEFAULT_SEI_UUID, stored);
  s[PROP_CUDAENC_MEMTYPE] = g_param_spec_enum ("cudaenc-memtype", "Input memory type",
      NOOP ("Memory type of the encoder's input buffers"), gst_rds_enc_cuda_mem_type_get_type (), 0, stored);
  s[PROP_SLICE_HEADER_SPACING] = g_param_spec_uint64 ("slice-header-spacing", "Slice header spacing",
      NOOP ("Slice size, in macroblocks or bits"), 0, G_MAXUINT64, 0, stored);
  s[PROP_BIT_PACKETIZATION] = g_param_spec_boolean ("bit-packetization", "Bit packetization",
      NOOP ("slice-header-spacing counts bits rather than macroblocks"), FALSE, stored);
  s[PROP_SLICE_INTRA_REFRESH_INTERVAL] = g_param_spec_uint ("SliceIntraRefreshInterval",
      "Slice intra refresh interval", NOOP ("Slice intra refresh interval"), 0, G_MAXUINT, 0, stored);
  s[PROP_ENABLE_LOSSLESS] = g_param_spec_boolean ("enable-lossless", "Lossless", NOOP ("Lossless coding of 4:4:4 "
          "input"), FALSE, stored);
  s[PROP_CAPTURE_IO_MODE] = g_param_spec_enum ("capture-io-mode", "Capture I/O mode",
      NOOP ("I/O mode of the encoded side"), gst_rds_enc_io_mode_get_type (), 0, stored);
  s[PROP_OUTPUT_IO_MODE] = g_param_spec_enum ("output-io-mode", "Output I/O mode",
      NOOP ("I/O mode of the raw side"), gst_rds_enc_io_mode_get_type (), 0, stored);
  s[PROP_EXTRA_CONTROLS] = g_param_spec_boxed ("extra-controls", "Extra controls",
      NOOP ("Extra device controls"), GST_TYPE_STRUCTURE, stored);
#undef NOOP

  /* read-only, DeepStream 8.0's values */
  s[PROP_DEVICE] = g_param_spec_string ("device", "Device", "Device node (DeepStream's; not used)", DEFAULT_DEVICE,
      ro);
  s[PROP_DEVICE_FD] = g_param_spec_int ("device-fd", "Device fd", "File descriptor of the device (not used)", -1,
      G_MAXINT, -1, ro);
  s[PROP_DEVICE_NAME] = g_param_spec_string ("device-name", "Device name", "Name of the device (not used)", "", ro);

  for (guint i = 1; i < N_PROPS; i++) {
    if (s[i] != NULL)
      g_object_class_install_property (gobject_class, i, s[i]);
  }

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  caps = gst_caps_new_simple (info->caps_name, "stream-format", G_TYPE_STRING, "byte-stream", "alignment",
      G_TYPE_STRING, "au", NULL);
  gst_element_class_add_pad_template (element_class, gst_pad_template_new ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
          caps));
  gst_caps_unref (caps);

  long_name = g_strdup_printf ("Redstream %s encoder", info->codec_name);
  desc = g_strdup_printf ("Encodes memory:RDS or system-memory frames to %s with the VA encoder (%s)",
      info->codec_name, info->va_factory);
  gst_element_class_set_metadata (element_class, long_name, "Codec/Encoder/Video/Hardware", desc,
      "Redstream contributors");
  g_free (long_name);
  g_free (desc);
}
