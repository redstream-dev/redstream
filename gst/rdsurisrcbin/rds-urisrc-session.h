/* Redstream: one RTSP session of rdsurisrcbin, a GstBin of rtspsrc and the depayloader of its video stream.
 *
 * Contract: a session is self-contained and used once. rdsurisrcbin adds it to itself and sets its state; to start over
 * (a restart, a reconnection) it stops it (rds_urisrc_session_stop) and adds a fresh one. Nothing outside the session
 * links to its children: it has at most one pad, the ghost pad "src", added when rtspsrc exposes the first H.264 or
 * H.265 video stream, carrying that stream depayloaded (video/x-h264 or video/x-h265, not parsed). Every other stream
 * rtspsrc exposes goes to a fakesink inside the session that is not a sink of the bin (no SINK flag). Its messages
 * come from its children, so a parent tells them apart with gst_object_has_as_ancestor (msg->src, session).
 *
 * Streams are chosen before RTSP SETUP (rtspsrc's select-stream): video streams in H.264 and H.265 are set up, other
 * video streams refused (the first one's encoding-name remembered), and the streams that are not video set up only
 * without disable_audio.
 * When the session ends up with no decodable video stream (rtspsrc's no-more-pads with no video pad, or rtspsrc's
 * error for having nothing to set up after it refused some stream, which the session then drops), the no_video
 * callback says so, once.
 *
 * An EOS leaving "src" (the server ended the stream, or rtspsrc took a closed connection as its end) goes through the
 * eos callback first, which decides whether it leaves the session.
 *
 * The callbacks run on rtspsrc's threads (pad-added, message handling, streaming).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_URISRC_SESSION_H
#define RDS_URISRC_SESSION_H

#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum
{
  RDS_URISRC_CODEC_NONE,
  RDS_URISRC_CODEC_H264,
  RDS_URISRC_CODEC_H265,
} RdsUriSrcCodec;

/* "H.264", "H.265" (NONE: "none"), for messages. */
const gchar *rds_urisrc_codec_name (RdsUriSrcCodec codec);

typedef struct
{
  const gchar *uri;             /* rtsp:// or rtsps:// */
  guint latency;                /* ms, rtspsrc's latency */
  gboolean drop_on_latency;
  guint udp_buffer_size;        /* bytes; clamped to rtspsrc's G_MAXINT */
  guint protocols;              /* GstRTSPLowerTrans flags for rtspsrc's protocols; 0 leaves rtspsrc's default */
  gboolean disable_audio;       /* refuse the streams that are not video */
} RdsUriSrcSessionSettings;

typedef struct _RdsUriSrcSession RdsUriSrcSession;

typedef struct
{
  /* The session's "src" pad was added: the video stream in @codec. Link it here, before returning: data may follow
   * right after. */
  void (*video_pad) (RdsUriSrcSession * session, GstPad * pad, RdsUriSrcCodec codec, gpointer user_data);
  /* No decodable video stream: @encoding is the encoding-name of the first video stream refused for its codec, NULL
   * when there was no video stream at all. */
  void (*no_video) (RdsUriSrcSession * session, const gchar * encoding, gpointer user_data);
  /* An EOS is about to leave "src", on the streaming thread: TRUE lets it through, FALSE drops it. */
  gboolean (*eos) (RdsUriSrcSession * session, gpointer user_data);
} RdsUriSrcSessionCallbacks;

#define RDS_TYPE_URISRC_SESSION (rds_urisrc_session_get_type ())
G_DECLARE_FINAL_TYPE (RdsUriSrcSession, rds_urisrc_session, RDS, URISRC_SESSION, GstBin)

/* A new session (floating, in NULL) for @settings, which are copied; NULL when rtspsrc is missing. */
RdsUriSrcSession *rds_urisrc_session_new (const RdsUriSrcSessionSettings * settings,
    const RdsUriSrcSessionCallbacks * callbacks, gpointer user_data);

/* Takes @session to NULL (rtspsrc's TEARDOWN, bounded by its 100 ms teardown-timeout; its threads are stopped when it
 * returns, so no callback runs after), unlinks "src" from its peer and removes the session from its parent bin, which
 * drops the parent's ref. Not on one of the session's own threads. The session's messages posted meanwhile still come
 * from inside it: a parent that wants to tell them apart keeps its own ref until this returns. Unlinking comes after
 * NULL, so nothing is pushed into an unlinked pad (a not-linked flow error). */
void rds_urisrc_session_stop (RdsUriSrcSession * session);

G_END_DECLS

#endif /* RDS_URISRC_SESSION_H */
