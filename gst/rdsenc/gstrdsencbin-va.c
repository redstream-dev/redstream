/* Redstream: GstRdsEncBin's VA encoder configuration: at READY to PAUSED the bin's properties, read in one snapshot,
 * are mapped onto the VA encoder's (rds-enc-props) and the inner rdsvideoconvert's. Locking: gstrdsencbin-private.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsencbin-private.h"

#include "rds-enc-props.h"

#define GST_CAT_DEFAULT gst_rds_enc_bin_debug

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
void
gst_rds_enc_bin_set_va_uint (GObject * o, const gchar * name, guint v)
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
  gboolean disable_cabac, insert_aud, qos, copy_meta;
  guint64 min_fku_interval;
  gint profile;
  GstElement *encoder, *convert;
} Settings;

/* Reads the properties for a start (READY to PAUSED) and sets them on the inner elements. FALSE (with an element
 * error): a setting the VA encoder can't do. */
gboolean
gst_rds_enc_bin_configure (GstRdsEncBin * self)
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
  st.copy_meta = g_value_get_boolean (&priv->values[PROP_COPY_META]);
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
  gst_rds_enc_bin_set_va_uint (va, "bitrate", rds_enc_kbps (st.bitrate));
  /* maxbitrate: the peak in VBR only (DeepStream 8.0 behaviour) */
  target_pct = rds_enc_target_percentage (st.bitrate, st.maxbitrate, vbr);
  if (target_pct > 0)
    gst_rds_enc_bin_set_va_uint (va, "target-percentage", target_pct);
  else
    set_default (va, "target-percentage");
  /* constqp: in CQP only (DeepStream 8.0 behaviour) */
  if (st.control_rate == 2 && st.constqp != NULL && rds_enc_parse_qp_triplet (st.constqp, qp)) {
    gst_rds_enc_bin_set_va_uint (va, "qpi", qp[0]);
    gst_rds_enc_bin_set_va_uint (va, "qpp", qp[1]);
    gst_rds_enc_bin_set_va_uint (va, "qpb", qp[2]);
  } else {
    set_default (va, "qpi");
    set_default (va, "qpp");
    set_default (va, "qpb");
  }
  /* qp-range: VA has one range, taken from the P pair (ours) */
  if (st.qp_range != NULL && rds_enc_parse_qp_range (st.qp_range, range)) {
    gst_rds_enc_bin_set_va_uint (va, "min-qp", range[0]);
    gst_rds_enc_bin_set_va_uint (va, "max-qp", range[1]);
  } else {
    set_default (va, "min-qp");
    set_default (va, "max-qp");
  }
  gst_rds_enc_bin_set_va_uint (va, "cpb-size", st.vbvbufsize / 1000);
  key_int_max = rds_enc_key_int_max (st.idrinterval, &capped);
  if (capped)
    g_warning ("%s: idrinterval %u is above the VA encoder's largest key-int-max; using %u", GST_OBJECT_NAME (self),
        st.idrinterval, key_int_max);
  gst_rds_enc_bin_set_va_uint (va, "key-int-max", key_int_max);
  gst_rds_enc_bin_set_va_uint (va, "i-frames", rds_enc_i_frames (st.iframeinterval, key_int_max));
  /* ours: preset-id is a no-op; target-usage 1 is the only one that holds CBR at the target */
  gst_rds_enc_bin_set_va_uint (va, "target-usage", 1);
  /* b-frames: lowered to 0 by caps_query_probe when the profile turns out to be Baseline */
  gst_rds_enc_bin_set_va_uint (va, "b-frames", st.b_frames);
  if (st.ref_frames > 0)
    gst_rds_enc_bin_set_va_uint (va, "ref-frames", st.ref_frames);
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
  gst_rds_enc_bin_reset_probes (self, st.copy_meta);
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
