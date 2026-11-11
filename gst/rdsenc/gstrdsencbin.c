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
 * per property (ours). Behaviour matching DeepStream 8.0 is marked "DeepStream 8.0 behaviour", our choices "ours";
 * every such choice has a row in docs/compat-matrix.md.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsencbin.h"
#include "gstrdsencbin-private.h"

#include <rds/rds.h>

#include "rds-enc-caps.h"
#include "rds-enc-props.h"
#include "rds-video-convert-caps.h"

GST_DEBUG_CATEGORY_STATIC (gst_rds_enc_bin_debug);
#define GST_CAT_DEFAULT gst_rds_enc_bin_debug

enum
{
  PROP_0,
  PROP_BITRATE,
  PROP_CONTROL_RATE,
  PROP_MAXBITRATE,
  PROP_CONSTQP,
  PROP_QP_RANGE,
  PROP_VBVBUFSIZE,
  PROP_IDRINTERVAL,
  PROP_IFRAMEINTERVAL,
  PROP_FORCE_IDR,
  PROP_FORCE_INTRA,
  PROP_PROFILE,
  PROP_NUM_B_FRAMES,
  PROP_NUM_REF_FRAMES,
  PROP_DISABLE_CABAC,
  PROP_INSERT_AUD,
  PROP_MIN_FORCE_KEY_UNIT_INTERVAL,
  PROP_QOS,
  PROP_COPY_META,
  PROP_GPU_ID,
  /* accepted no-ops */
  PROP_NOOP_FIRST,
  PROP_PRESET_ID = PROP_NOOP_FIRST,
  PROP_INSERT_SPS_PPS,
  PROP_INITQP,
  PROP_CQ,
  PROP_VBVINIT,
  PROP_AQ,
  PROP_TEMPORALAQ,
  PROP_INTRA_REFRESH,
  PROP_TUNING_INFO_ID,
  PROP_INSERT_VUI,
  PROP_EXTENDED_COLORFORMAT,
  PROP_SEI_UUID,
  PROP_CUDAENC_MEMTYPE,
  PROP_SLICE_HEADER_SPACING,
  PROP_BIT_PACKETIZATION,
  PROP_SLICE_INTRA_REFRESH_INTERVAL,
  PROP_ENABLE_LOSSLESS,
  PROP_CAPTURE_IO_MODE,
  PROP_OUTPUT_IO_MODE,
  PROP_EXTRA_CONTROLS,
  /* read-only */
  PROP_DEVICE,
  PROP_DEVICE_FD,
  PROP_DEVICE_NAME,
  N_PROPS,
};

#define IS_NOOP(id) ((id) >= PROP_NOOP_FIRST && (id) < PROP_DEVICE)

G_STATIC_ASSERT (PROP_DEVICE - PROP_NOOP_FIRST <= 32);

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

typedef struct _GstRdsEncBinPrivate GstRdsEncBinPrivate;

/* Locking: the object lock guards the property values and the inner-element pointers. Forward a property to an
 * inner element or send it an event after unlocking, never under the object lock: take a reference with
 * get_encoder () (or the like) and drop the lock first. va_lock serialises the writes of the properties the VA
 * encoder takes while running (bitrate; the forced key frames will take it too) with the start's configure (), so
 * a live value set during a start is not overwritten by the start's older snapshot: a forward takes va_lock, reads
 * the latest stored value under the object lock, and sets it with only va_lock held. Lock order: va_lock, then the
 * object lock. The probes never take va_lock (they run inside the VA encoder's own calls). configure () holds
 * va_lock while it sets the children's properties and posts nothing then, so don't set this bin's properties from
 * its children's notify / deep-notify or from a sync bus handler while it starts. The chain, and the
 * probes on it, are made once, in build_chain; per-run state (what the probes measure or warn about once) is reset
 * at READY to PAUSED and PAUSED to READY, not there. */
struct _GstRdsEncBinPrivate
{
  GMutex va_lock;

  /* the inner chain (object lock), made at the first NULL to READY and kept until finalize (the bin owns them) */
  GstElement *convert;          /* rdsvideoconvert */
  GstElement *rawcaps;          /* video/x-raw,format=NV12 */
  GstElement *encoder;          /* vah264enc / vah265enc */
  GstElement *srccaps;          /* the codec's byte-stream caps */
  GstPad *sink_ghost, *src_ghost;

  /* properties (object lock), indexed by property id */
  GValue values[N_PROPS];
  guint noops_warned;           /* bit per no-op property, from PROP_NOOP_FIRST */

  /* per run (object lock): set at READY to PAUSED, cleared at PAUSED to READY */
  const gchar *va_profile;      /* the profile property's VA caps profile */
  guint b_frames;               /* num-B-Frames read at the start */
  guint b_frames_set;           /* what the VA encoder's b-frames was last set to */
  gboolean b_frames_warned;     /* the Baseline + B frames warning was given */
  gboolean level_warned;        /* the downstream level warning was given */
};

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (GstRdsEncBin, gst_rds_enc_bin, GST_TYPE_BIN);

#define PRIV(self) ((GstRdsEncBinPrivate *) gst_rds_enc_bin_get_instance_private (self))

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

/* ---- setting the VA encoder's properties ---- */

/* @o's property @name, or NULL (with a debug line) when it has none: a VA encoder of another version might lack
 * one, and the tests' stand-ins lack most. */
static GParamSpec *
find_prop (GObject * o, const gchar * name)
{
  GParamSpec *ps = g_object_class_find_property (G_OBJECT_GET_CLASS (o), name);

  if (ps == NULL)
    GST_DEBUG_OBJECT (o, "no property %s", name);
  return ps;
}

/* Sets @o's uint @name to @v, clamped to its range (ours: e.g. VA's bitrate stops at 2048000 kbit/s and its
 * target-percentage starts at 50). */
static void
set_uint (GObject * o, const gchar * name, guint v)
{
  GParamSpec *ps = find_prop (o, name);
  GParamSpecUInt *u;

  if (ps == NULL)
    return;
  g_return_if_fail (G_IS_PARAM_SPEC_UINT (ps));
  u = G_PARAM_SPEC_UINT (ps);
  if (v < u->minimum || v > u->maximum) {
    GST_INFO_OBJECT (o, "%s %u clamped to %u..%u", name, v, u->minimum, u->maximum);
    v = CLAMP (v, u->minimum, u->maximum);
  }
  g_object_set (o, name, v, NULL);
}

static void
set_bool (GObject * o, const gchar * name, gboolean v)
{
  if (find_prop (o, name) != NULL)
    g_object_set (o, name, v, NULL);
}

static void
set_uint64 (GObject * o, const gchar * name, guint64 v)
{
  if (find_prop (o, name) != NULL)
    g_object_set (o, name, v, NULL);
}

/* Sets @o's @name back to its default (what "leave VA's choice" means across restarts). */
static void
set_default (GObject * o, const gchar * name)
{
  GParamSpec *ps = find_prop (o, name);

  if (ps != NULL)
    g_object_set_property (o, name, g_param_spec_get_default_value (ps));
}

/* ---- the preferred profile ---- */

/* The answer to the VA encoder's downstream CAPS queries, decided by rds_enc_caps_narrow: narrowed to the `profile`
 * property's profile when downstream allows it (downstream caps that fix another profile win: DeepStream 8.0
 * behaviour), and with B frames requested, Baseline left out when there is a choice. When only Baseline is left and
 * B frames were requested (H.264), the VA encoder's b-frames is set to 0 with one warning per run (ours: VA would
 * write B slices into a Baseline stream).
 *
 * Why setting b-frames here works, and what to re-check on a GStreamer update (written against gst-plugins-bad 1.28's
 * va plugin): VA's reconfiguration first decides the profile, asking downstream for its allowed caps (this query),
 * then resets its state, copying the b-frames *property* into the GOP settings, then builds the GOP structure. VA has
 * a Baseline check of its own that drops B frames, but it runs during the profile decision, before the reset, so the
 * reset undoes it. Our property change, made inside the same query, is what the reset copies. If VA reorders these
 * steps, /gpu/enc/b-frames-h264 (Baseline from the property and from downstream caps: no B slices) fails. */
static GstPadProbeReturn
caps_query_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstRdsEncBinPrivate *priv = PRIV (self);
  const GstRdsEncCodecInfo *codec = GST_RDS_ENC_BIN_GET_CLASS (self)->info;
  GstQuery *q = GST_PAD_PROBE_INFO_QUERY (info);
  GstCaps *res, *narrowed;
  const gchar *pref;
  guint requested, b_frames, b_set;
  gboolean only_baseline = FALSE, warn = FALSE;

  if (GST_QUERY_TYPE (q) != GST_QUERY_CAPS || !(GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_PULL))
    return GST_PAD_PROBE_OK;
  gst_query_parse_caps_result (q, &res);
  if (res == NULL || gst_caps_is_empty (res) || gst_caps_is_any (res))
    return GST_PAD_PROBE_OK;

  GST_OBJECT_LOCK (self);
  pref = priv->va_profile;      /* a static string */
  requested = priv->b_frames;
  GST_OBJECT_UNLOCK (self);
  if (pref == NULL)             /* not started */
    return GST_PAD_PROBE_OK;

  narrowed = rds_enc_caps_narrow (res, codec->caps_name, pref, requested, &only_baseline);
  only_baseline = only_baseline && requested > 0;
  b_frames = rds_enc_b_frames (requested, only_baseline, NULL);

  GST_DEBUG_OBJECT (self, "downstream caps %" GST_PTR_FORMAT " -> %" GST_PTR_FORMAT ", b-frames %u", res, narrowed,
      b_frames);
  gst_query_set_caps_result (q, narrowed);
  gst_caps_unref (narrowed);

  GST_OBJECT_LOCK (self);
  b_set = priv->b_frames_set;
  priv->b_frames_set = b_frames;
  if (only_baseline && !priv->b_frames_warned) {
    priv->b_frames_warned = TRUE;
    warn = TRUE;
  }
  GST_OBJECT_UNLOCK (self);

  if (warn)
    g_warning ("%s: num-B-Frames=%u with a Baseline profile: Baseline has no B frames, so none are encoded",
        GST_OBJECT_NAME (self), requested);
  if (b_frames != b_set) {
    GstElement *encoder = gst_pad_get_parent_element (pad);

    if (encoder != NULL) {
      set_uint (G_OBJECT (encoder), "b-frames", b_frames);
      gst_object_unref (encoder);
    }
  }
  return GST_PAD_PROBE_OK;
}

/* The VA encoder's output caps. VA computes the level from the size, frame rate and bitrate and writes it in its caps
 * and the stream; a `level` that downstream caps fix to another value makes the caps refused (not-negotiated).
 * DeepStream 8.0 writes the downstream level instead; here it is a warning, once per run, naming both (ours). */
static GstPadProbeReturn
caps_event_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBin *self = user_data;
  GstRdsEncBinPrivate *priv = PRIV (self);
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  GstCaps *caps, *allowed;
  const gchar *level;
  gchar *want = NULL;
  gboolean warn, refused;

  if (GST_EVENT_TYPE (ev) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  gst_event_parse_caps (ev, &caps);
  level = gst_structure_get_string (gst_caps_get_structure (caps, 0), "level");
  if (level == NULL)
    return GST_PAD_PROBE_OK;
  /* What downstream allows (the src capsfilter's accept-caps would only check its own caps). This query comes back
   * through caps_query_probe on this pad, which recomputes the same b-frames as during VA's profile decision, so it
   * sets nothing; were it ever to set b-frames here, that would only apply at VA's next reconfiguration. */
  allowed = gst_pad_peer_query_caps (pad, NULL);
  refused = rds_enc_caps_level_refused (caps, allowed, &want);
  gst_caps_unref (allowed);
  if (!refused)                 /* accepted, or refused for more than the level */
    return GST_PAD_PROBE_OK;

  GST_OBJECT_LOCK (self);
  warn = !priv->level_warned;
  priv->level_warned = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (warn)
    g_warning ("%s: downstream caps ask for level %s, but the VA encoder sets the level itself (%s for this size, "
        "frame rate and bitrate): drop the level from the caps after the encoder", GST_OBJECT_NAME (self), want,
        level);
  g_free (want);
  return GST_PAD_PROBE_OK;
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

  pad = gst_element_get_static_pad (encoder, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM | GST_PAD_PROBE_TYPE_PULL, caps_query_probe, self,
      NULL);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, caps_event_probe, self, NULL);
  gst_object_unref (pad);

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

/* Sets the VA encoder's rate-control to the mode @nick (cbr, vbr, cqp); the modes are the device's, so one it doesn't
 * offer is a warning and VA keeps its own. */
static void
set_rate_control (GstRdsEncBin * self, GObject * va, const gchar * nick)
{
  GParamSpec *ps = find_prop (va, "rate-control");
  GEnumClass *ec;
  GEnumValue *ev;

  if (ps == NULL)
    return;
  g_return_if_fail (G_IS_PARAM_SPEC_ENUM (ps));
  ec = G_PARAM_SPEC_ENUM (ps)->enum_class;
  ev = g_enum_get_value_by_nick (ec, nick);
  if (ev == NULL) {
    g_warning ("%s: the VA encoder has no %s rate control on this device; it keeps its own", GST_OBJECT_NAME (self),
        nick);
    return;
  }
  g_object_set (va, "rate-control", ev->value, NULL);
}

/* What a start reads, in one snapshot under the object lock. */
typedef struct
{
  guint bitrate, control_rate, maxbitrate, vbvbufsize, idrinterval, iframeinterval, b_frames, ref_frames, gpu_id;
  gchar *constqp, *qp_range;
  gboolean disable_cabac, insert_aud, qos;
  guint64 min_fku_interval;
  gint profile;
  GstElement *encoder, *convert;
} Settings;

/* Reads the properties for a start (READY to PAUSED) and sets them on the inner elements. FALSE (with an element
 * error): a setting the VA encoder can't do. */
static gboolean
configure (GstRdsEncBin * self)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  const GstRdsEncCodecInfo *info = GST_RDS_ENC_BIN_GET_CLASS (self)->info;
  const gchar *va_profile = NULL;
  GError *err = NULL;
  Settings st;
  GObject *va;
  guint qp[3], range[6], key_int_max, target_pct;
  gboolean capped, vbr, ok = FALSE;

  g_mutex_lock (&priv->va_lock);
  GST_OBJECT_LOCK (self);
  st.bitrate = g_value_get_uint (&priv->values[PROP_BITRATE]);
  st.control_rate = (guint) g_value_get_enum (&priv->values[PROP_CONTROL_RATE]);
  st.maxbitrate = g_value_get_uint (&priv->values[PROP_MAXBITRATE]);
  st.vbvbufsize = g_value_get_uint (&priv->values[PROP_VBVBUFSIZE]);
  st.idrinterval = g_value_get_uint (&priv->values[PROP_IDRINTERVAL]);
  st.iframeinterval = g_value_get_uint (&priv->values[PROP_IFRAMEINTERVAL]);
  st.b_frames = g_value_get_uint (&priv->values[PROP_NUM_B_FRAMES]);
  st.ref_frames = g_value_get_uint (&priv->values[PROP_NUM_REF_FRAMES]);
  st.gpu_id = g_value_get_uint (&priv->values[PROP_GPU_ID]);
  st.constqp = g_value_dup_string (&priv->values[PROP_CONSTQP]);
  st.qp_range = g_value_dup_string (&priv->values[PROP_QP_RANGE]);
  st.disable_cabac = info->has_cabac && g_value_get_boolean (&priv->values[PROP_DISABLE_CABAC]);
  st.insert_aud = g_value_get_boolean (&priv->values[PROP_INSERT_AUD]);
  st.qos = g_value_get_boolean (&priv->values[PROP_QOS]);
  st.min_fku_interval = g_value_get_uint64 (&priv->values[PROP_MIN_FORCE_KEY_UNIT_INTERVAL]);
  st.profile = g_value_get_enum (&priv->values[PROP_PROFILE]);
  st.encoder = gst_object_ref (priv->encoder);
  st.convert = gst_object_ref (priv->convert);
  GST_OBJECT_UNLOCK (self);

  /* ours: H.264 High444 (VA has no High 4:4:4) and H.265 Main10 / FREXT (they need P010 / 4:4:4 input) */
  if (!info->va_profile (st.profile, &va_profile, &err))
    goto out;                   /* the error is posted after va_lock is dropped */
  GST_DEBUG_OBJECT (self, "profile %d: VA profile %s", st.profile, va_profile);

  va = G_OBJECT (st.encoder);
  vbr = st.control_rate == 0;
  set_rate_control (self, va, vbr ? "vbr" : st.control_rate == 2 ? "cqp" : "cbr");
  set_uint (va, "bitrate", rds_enc_kbps (st.bitrate));
  /* maxbitrate: the peak in VBR only (DeepStream 8.0 behaviour) */
  target_pct = rds_enc_target_percentage (st.bitrate, st.maxbitrate, vbr);
  if (target_pct > 0)
    set_uint (va, "target-percentage", target_pct);
  else
    set_default (va, "target-percentage");
  /* constqp: in CQP only (DeepStream 8.0 behaviour) */
  if (st.control_rate == 2 && st.constqp != NULL && rds_enc_parse_qp_triplet (st.constqp, qp)) {
    set_uint (va, "qpi", qp[0]);
    set_uint (va, "qpp", qp[1]);
    set_uint (va, "qpb", qp[2]);
  } else {
    set_default (va, "qpi");
    set_default (va, "qpp");
    set_default (va, "qpb");
  }
  /* qp-range: VA has one range, taken from the P pair (ours) */
  if (st.qp_range != NULL && rds_enc_parse_qp_range (st.qp_range, range)) {
    set_uint (va, "min-qp", range[0]);
    set_uint (va, "max-qp", range[1]);
  } else {
    set_default (va, "min-qp");
    set_default (va, "max-qp");
  }
  set_uint (va, "cpb-size", st.vbvbufsize / 1000);
  key_int_max = rds_enc_key_int_max (st.idrinterval, &capped);
  if (capped)
    g_warning ("%s: idrinterval %u is above the VA encoder's largest key-int-max; using %u", GST_OBJECT_NAME (self),
        st.idrinterval, key_int_max);
  set_uint (va, "key-int-max", key_int_max);
  set_uint (va, "i-frames", rds_enc_i_frames (st.iframeinterval, key_int_max));
  /* ours: preset-id is a no-op; target-usage 1 is the only one that holds CBR at the target */
  set_uint (va, "target-usage", 1);
  /* b-frames: lowered to 0 by caps_query_probe when the profile turns out to be Baseline */
  set_uint (va, "b-frames", st.b_frames);
  if (st.ref_frames > 0)
    set_uint (va, "ref-frames", st.ref_frames);
  else
    set_default (va, "ref-frames");
  if (info->has_cabac)
    set_bool (va, "cabac", !st.disable_cabac);
  set_bool (va, "aud", st.insert_aud);
  set_uint64 (va, "min-force-key-unit-interval", st.min_fku_interval);
  set_bool (va, "qos", st.qos);
  g_object_set (st.convert, "gpu-id", st.gpu_id, NULL);

  GST_OBJECT_LOCK (self);
  priv->va_profile = va_profile;
  priv->b_frames = st.b_frames;
  priv->b_frames_set = st.b_frames;
  priv->b_frames_warned = FALSE;
  priv->level_warned = FALSE;
  GST_OBJECT_UNLOCK (self);
  ok = TRUE;

out:
  g_mutex_unlock (&priv->va_lock);
  if (err != NULL) {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, ("%s", err->message), ("profile=%d", st.profile));
    g_clear_error (&err);
  }
  g_free (st.constqp);
  g_free (st.qp_range);
  gst_object_unref (st.encoder);
  gst_object_unref (st.convert);
  return ok;
}

/* Clears the per-run state (PAUSED to READY). */
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
      if (!configure (self))
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
      set_uint (G_OBJECT (encoder), "bitrate", rds_enc_kbps (bitrate));
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
  g_mutex_clear (&priv->va_lock);
  G_OBJECT_CLASS (gst_rds_enc_bin_parent_class)->finalize (object);
}

static void
gst_rds_enc_bin_init (GstRdsEncBin * self)
{
  g_mutex_init (&PRIV (self)->va_lock);
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
