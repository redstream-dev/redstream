/* Redstream: one RTSP session of rdsurisrcbin (see rds-urisrc-session.h for the contract).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-urisrc-session.h"

GST_DEBUG_CATEGORY_EXTERN (gst_rds_uri_src_bin_debug);
#define GST_CAT_DEFAULT gst_rds_uri_src_bin_debug

/* ours: a dead server can't hold up stopping for longer (rtspsrc's teardown-timeout, nanoseconds) */
#define TEARDOWN_TIMEOUT (100 * GST_MSECOND)

struct _RdsUriSrcSession
{
  GstBin parent;

  RdsUriSrcSessionCallbacks callbacks;
  gpointer user_data;
  gboolean disable_audio;

  /* (object lock) */
  gchar *refused;               /* the encoding-name of the first video stream refused for its codec */
  gboolean refused_any;         /* some stream was refused (for its codec, or disable_audio) */
  gboolean have_video;          /* the video pad was taken for "src" */
  gboolean reported;            /* no_video was called */
};

G_DEFINE_TYPE (RdsUriSrcSession, rds_urisrc_session, GST_TYPE_BIN);

const gchar *
rds_urisrc_codec_name (RdsUriSrcCodec codec)
{
  switch (codec) {
    case RDS_URISRC_CODEC_H264:
      return "H.264";
    case RDS_URISRC_CODEC_H265:
      return "H.265";
    default:
      return "none";
  }
}

/* The codec of a stream's caps (application/x-unknown from the SDP in select-stream, application/x-rtp on the pads),
 * NONE when it is not video in H.264 or H.265. */
static RdsUriSrcCodec
codec_of (const GstStructure * s)
{
  const gchar *encoding = gst_structure_get_string (s, "encoding-name");

  if (g_strcmp0 (gst_structure_get_string (s, "media"), "video") != 0 || encoding == NULL)
    return RDS_URISRC_CODEC_NONE;
  if (g_ascii_strcasecmp (encoding, "H264") == 0)
    return RDS_URISRC_CODEC_H264;
  if (g_ascii_strcasecmp (encoding, "H265") == 0)
    return RDS_URISRC_CODEC_H265;
  return RDS_URISRC_CODEC_NONE;
}

/* Calls no_video once, outside the object lock. */
static void
report_no_video (RdsUriSrcSession * self)
{
  gchar *refused = NULL;
  gboolean report;

  GST_OBJECT_LOCK (self);
  report = !self->reported && !self->have_video;
  self->reported = TRUE;
  if (report)
    refused = g_strdup (self->refused);
  GST_OBJECT_UNLOCK (self);
  if (report)
    self->callbacks.no_video (self, refused, self->user_data);
  g_free (refused);
}

/* rtspsrc's select-stream, before SETUP: H.264 and H.265 video; the rest of the video refused (and its encoding
 * remembered); the streams that are not video unless disable_audio. */
static gboolean
select_stream (G_GNUC_UNUSED GstElement * rtspsrc, guint num, GstCaps * caps, RdsUriSrcSession * self)
{
  const GstStructure *s = gst_caps_get_structure (caps, 0);
  const gchar *encoding = gst_structure_get_string (s, "encoding-name");
  gboolean select;

  if (g_strcmp0 (gst_structure_get_string (s, "media"), "video") != 0) {
    select = !self->disable_audio;
  } else {
    select = codec_of (s) != RDS_URISRC_CODEC_NONE;
    GST_OBJECT_LOCK (self);
    if (!select && self->refused == NULL)
      self->refused = g_strdup (encoding != NULL ? encoding : "(no encoding-name)");
    GST_OBJECT_UNLOCK (self);
  }
  if (!select) {
    GST_OBJECT_LOCK (self);
    self->refused_any = TRUE;
    GST_OBJECT_UNLOCK (self);
  }
  GST_DEBUG_OBJECT (self, "stream %u %s: %" GST_PTR_FORMAT, num, select ? "selected" : "refused", caps);
  return select;
}

/* A pad that is not the video's: to a fakesink, which is not a sink of the session (no SINK flag) and never
 * prerolls. */
static void
discard_pad (RdsUriSrcSession * self, GstPad * pad)
{
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);
  GstPad *sinkpad;

  if (sink == NULL)
    return;
  GST_DEBUG_OBJECT (self, "discarding %s:%s", GST_DEBUG_PAD_NAME (pad));
  g_object_set (sink, "sync", FALSE, "async", FALSE, NULL);
  GST_OBJECT_FLAG_UNSET (sink, GST_ELEMENT_FLAG_SINK);
  gst_bin_add (GST_BIN (self), sink);
  sinkpad = gst_element_get_static_pad (sink, "sink");
  gst_pad_link (pad, sinkpad);
  gst_object_unref (sinkpad);
  gst_element_sync_state_with_parent (sink);
}

/* EOS on "src": the eos callback decides. */
static GstPadProbeReturn
src_eos_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  RdsUriSrcSession *self = user_data;

  if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) != GST_EVENT_EOS
      || self->callbacks.eos (self, self->user_data))
    return GST_PAD_PROBE_OK;
  GST_DEBUG_OBJECT (self, "EOS dropped");
  /* HANDLED, not DROP: a dropped sticky event stays pending on the pad */
  gst_event_unref (GST_PAD_PROBE_INFO_EVENT (info));
  return GST_PAD_PROBE_HANDLED;
}

/* rtspsrc's pads: the first H.264 or H.265 video stream through its depayloader to "src", the others discarded. */
static void
on_pad_added (G_GNUC_UNUSED GstElement * rtspsrc, GstPad * pad, RdsUriSrcSession * self)
{
  GstCaps *caps = gst_pad_get_current_caps (pad);
  RdsUriSrcCodec codec;
  GstElement *depay;
  GstPad *sinkpad, *srcpad, *ghost;
  gboolean take;

  if (caps == NULL)
    caps = gst_pad_query_caps (pad, NULL);
  codec = gst_caps_is_empty (caps) || gst_caps_is_any (caps) ? RDS_URISRC_CODEC_NONE :
      codec_of (gst_caps_get_structure (caps, 0));
  GST_DEBUG_OBJECT (self, "pad %s:%s, caps %" GST_PTR_FORMAT, GST_DEBUG_PAD_NAME (pad), caps);
  gst_caps_unref (caps);
  GST_OBJECT_LOCK (self);
  take = codec != RDS_URISRC_CODEC_NONE && !self->have_video;
  if (take)
    self->have_video = TRUE;
  GST_OBJECT_UNLOCK (self);
  if (!take) {
    discard_pad (self, pad);
    return;
  }

  depay = gst_element_factory_make (codec == RDS_URISRC_CODEC_H264 ? "rtph264depay" : "rtph265depay", NULL);
  if (depay == NULL) {
    GST_ELEMENT_ERROR (self, CORE, MISSING_PLUGIN, ("%s is missing",
            codec == RDS_URISRC_CODEC_H264 ? "rtph264depay" : "rtph265depay"), (NULL));
    discard_pad (self, pad);
    return;
  }
  gst_bin_add (GST_BIN (self), depay);
  sinkpad = gst_element_get_static_pad (depay, "sink");
  if (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK) {
    gst_object_unref (sinkpad);
    GST_ELEMENT_ERROR (self, CORE, NEGOTIATION, ("Could not link rtspsrc to the depayloader"), (NULL));
    return;
  }
  gst_object_unref (sinkpad);
  gst_element_sync_state_with_parent (depay);
  srcpad = gst_element_get_static_pad (depay, "src");
  ghost = gst_ghost_pad_new ("src", srcpad);
  gst_object_unref (srcpad);
  gst_pad_add_probe (ghost, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, src_eos_probe, self, NULL);
  gst_pad_set_active (ghost, TRUE);
  gst_element_add_pad (GST_ELEMENT (self), ghost);
  self->callbacks.video_pad (self, ghost, codec, self->user_data);
}

/* rtspsrc's pads are all there: no video pad means nothing to decode. */
static void
on_no_more_pads (G_GNUC_UNUSED GstElement * rtspsrc, RdsUriSrcSession * self)
{
  report_no_video (self);
}

/* rtspsrc's error for having no stream to set up, after select-stream refused some (a video codec, or audio with
 * disable_audio), is no_video instead (its message is about transports and extension plugins). */
static void
rds_urisrc_session_handle_message (GstBin * bin, GstMessage * msg)
{
  RdsUriSrcSession *self = RDS_URISRC_SESSION (bin);

  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;
    gboolean nothing;

    gst_message_parse_error (msg, &err, NULL);
    GST_OBJECT_LOCK (self);
    nothing = g_error_matches (err, GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND) && !self->have_video
        && self->refused_any;
    GST_OBJECT_UNLOCK (self);
    g_clear_error (&err);
    if (nothing) {
      GST_DEBUG_OBJECT (self, "no stream to set up: %" GST_PTR_FORMAT, msg);
      gst_message_unref (msg);
      report_no_video (self);
      return;
    }
  }
  GST_BIN_CLASS (rds_urisrc_session_parent_class)->handle_message (bin, msg);
}

RdsUriSrcSession *
rds_urisrc_session_new (const RdsUriSrcSessionSettings * settings, const RdsUriSrcSessionCallbacks * callbacks,
    gpointer user_data)
{
  GstElement *rtspsrc = gst_element_factory_make ("rtspsrc", NULL);
  RdsUriSrcSession *self;

  if (rtspsrc == NULL)
    return NULL;
  self = g_object_new (RDS_TYPE_URISRC_SESSION, NULL);
  self->callbacks = *callbacks;
  self->user_data = user_data;
  self->disable_audio = settings->disable_audio;
  g_object_set (rtspsrc, "location", settings->uri, "latency", settings->latency, "drop-on-latency",
      settings->drop_on_latency, "udp-buffer-size", (gint) MIN (settings->udp_buffer_size, (guint) G_MAXINT),
      "teardown-timeout", (guint64) TEARDOWN_TIMEOUT, NULL);
  if (settings->protocols != 0)
    g_object_set (rtspsrc, "protocols", settings->protocols, NULL);
  g_signal_connect (rtspsrc, "select-stream", G_CALLBACK (select_stream), self);
  g_signal_connect (rtspsrc, "pad-added", G_CALLBACK (on_pad_added), self);
  g_signal_connect (rtspsrc, "no-more-pads", G_CALLBACK (on_no_more_pads), self);
  gst_bin_add (GST_BIN (self), rtspsrc);
  return self;
}

void
rds_urisrc_session_stop (RdsUriSrcSession * self)
{
  GstObject *parent = gst_object_get_parent (GST_OBJECT (self));
  GstPad *src, *peer = NULL;

  gst_element_set_state (GST_ELEMENT (self), GST_STATE_NULL);
  src = gst_element_get_static_pad (GST_ELEMENT (self), "src");
  if (src != NULL && (peer = gst_pad_get_peer (src)) != NULL)
    gst_pad_unlink (src, peer);
  gst_clear_object (&peer);
  gst_clear_object (&src);
  if (parent != NULL) {
    gst_bin_remove (GST_BIN (parent), GST_ELEMENT (self));
    gst_object_unref (parent);
  }
}

static void
rds_urisrc_session_finalize (GObject * object)
{
  RdsUriSrcSession *self = RDS_URISRC_SESSION (object);

  g_free (self->refused);
  G_OBJECT_CLASS (rds_urisrc_session_parent_class)->finalize (object);
}

static void
rds_urisrc_session_init (G_GNUC_UNUSED RdsUriSrcSession * self)
{
}

static void
rds_urisrc_session_class_init (RdsUriSrcSessionClass * klass)
{
  G_OBJECT_CLASS (klass)->finalize = rds_urisrc_session_finalize;
  GST_BIN_CLASS (klass)->handle_message = GST_DEBUG_FUNCPTR (rds_urisrc_session_handle_message);
}
