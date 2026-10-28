/* An RTSP test server (gst-rtsp-server) with a TCP fault relay in front of it, shared by the tests and
 * rds-rtsp-serve. The server runs in its own thread and GMainContext and serves one mount: a live videotestsrc
 * (pattern ball, 30 fps) encoded with the VA encoder (h264, h265; key frame every 30 frames) or jpegenc (jpeg),
 * optionally with a live test tone in Opus as a second stream (or the only one), or a launch line of the caller's.
 * Its media factory is not shared by default: each session builds a pipeline from the current launch line, so
 * rds_test_rtsp_server_set_stream changes what later sessions get, and each starts on a key frame. With the shared
 * option every session plays one pipeline, through the relay or direct, so a client that comes later joins it where it
 * is, mid-GOP (while another client keeps it playing).
 *
 * The relay listens on its own port and forwards TCP both ways to the server, so only RTSP with interleaved RTP
 * (rtspsrc protocols=tcp) goes through it: with UDP transport the RTP packets bypass it (use the direct URL there).
 *
 * Keep one server alive per process at a time: gst-rtsp-server's media threads are process-wide, and a server waits
 * for them (and cleans them up) only when it is the last one freed.
 *
 * Built without gst-rtsp-server, the constructors return NULL with the reason, so callers compile and skip.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_RTSP_TEST_SERVER_H
#define RDS_RTSP_TEST_SERVER_H

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  RDS_RELAY_PASS,               /* forward both ways (also for connections made from now on) */
  RDS_RELAY_FREEZE,             /* stop reading both ways without closing: a stall without EOS (new connections too) */
  RDS_RELAY_CUT,                /* close every relayed connection once; new connections are forwarded again */
  RDS_RELAY_REFUSE,             /* close every relayed connection, and each new one right after accepting it */
} RdsRelayMode;

typedef struct _RdsTestRtspServer RdsTestRtspServer;

typedef struct
{
  const gchar *address;         /* the address both ports bind; NULL: 127.0.0.1 */
  guint port;                   /* the server's port; 0: a free one */
  const gchar *mount;           /* NULL: "/test" */
  gboolean relay;               /* start the relay */
  guint relay_port;             /* 0: a free one */
  guint width, height;          /* the test pattern's size */
  const gchar *codec;           /* "h264", "h265" or "jpeg"; NULL with audio: audio only */
  gboolean audio;               /* a test tone in Opus (payload type 97) after the video */
  const gchar *launch;          /* a whole media launch line ("( ... rtp...pay name=pay0 ... )") instead of the pattern */
  gboolean shared;              /* one media pipeline for all clients (the factory's shared) */
} RdsTestRtspServerOptions;

/* TRUE when the elements a test-pattern stream of @codec needs are there; FALSE and the reason in *@why (free with
 * g_free; @why may be NULL) otherwise. */
gboolean rds_test_rtsp_server_codec_available (const gchar * codec, gchar ** why);

/* Starts a server on 127.0.0.1 (free ports) with the relay, mount "/test", the test pattern at @width x @height in
 * @codec. NULL and a reason to skip in *@why (free with g_free) when the encoder or the server library is missing. */
/* The same for the test tone (audio in the options). */
gboolean rds_test_rtsp_server_audio_available (gchar ** why);

RdsTestRtspServer *rds_test_rtsp_server_new (guint width, guint height, const gchar * codec, gchar ** why);
/* The same with every option (the tool's). NULL and the reason in *@why when it cannot start (also a port in use). */
RdsTestRtspServer *rds_test_rtsp_server_new_full (const RdsTestRtspServerOptions * opts, gchar ** why);

/* rtsp://<address>:<port><mount>, through the relay (NULL without one) or direct; free with g_free. */
gchar *rds_test_rtsp_server_url (RdsTestRtspServer * s, gboolean via_relay);
/* The test pattern at @width x @height in @codec for the sessions made from now on (it replaces a launch line too;
 * the test tone stays as the options set it). */
void rds_test_rtsp_server_set_stream (RdsTestRtspServer * s, guint width, guint height, const gchar * codec);
void rds_test_rtsp_server_set_relay (RdsTestRtspServer * s, RdsRelayMode mode);
/* The RTSP sessions created so far. */
guint rds_test_rtsp_server_sessions (RdsTestRtspServer * s);
/* Closes the relay's and the server's connections, stops the server and waits for its threads. */
void rds_test_rtsp_server_free (RdsTestRtspServer * s);

G_END_DECLS

#endif /* RDS_RTSP_TEST_SERVER_H */
