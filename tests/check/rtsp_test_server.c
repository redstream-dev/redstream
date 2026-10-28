/* The RTSP test server and its TCP fault relay (tools/rds-rtsp-serve/rtsp-test-server.h), as the rdsurisrcbin stream
 * tests use them: plain rtspsrc clients through the relay and direct (gpu; skips without the VA encoder or
 * gst-rtsp-server).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include "rtsp-test-server.h"

/* How long a client may take to get its first buffers (RTSP setup and the encoder's start). */
#define PLAY_TIMEOUT_US (15 * G_USEC_PER_SEC)

/* A server, or NULL and the test skipped. */
static RdsTestRtspServer *
server_or_skip (guint width, guint height, const gchar * codec)
{
  gchar *why = NULL;
  RdsTestRtspServer *s = rds_test_rtsp_server_new (width, height, codec, &why);

  if (s == NULL) {
    g_test_skip (why);
    g_free (why);
  }
  return s;
}

/* ---- a client: rtspsrc ! depay ! parse ! fakesink, counting buffers ---- */

typedef struct
{
  GstElement *pipe;
  GstPad *sinkpad;
  gint buffers;                 /* atomic */
} Client;

static GstPadProbeReturn
count_buffer (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer data)
{
  g_atomic_int_inc (&((Client *) data)->buffers);
  return GST_PAD_PROBE_OK;
}

/* Starts a client of @url; @codec picks the depayloader (and parser, for h264 and h265). latency=100: the jitter
 * buffer's default 2 s would keep handing out buffers well after the relay stops. */
static Client *
client_start (const gchar * url, const gchar * codec)
{
  Client *c = g_new0 (Client, 1);
  GError *err = NULL;
  gchar *parse = g_str_equal (codec, "jpeg") ? g_strdup ("") : g_strdup_printf ("! %sparse", codec);
  gchar *desc = g_strdup_printf ("rtspsrc location=%s protocols=tcp latency=100 ! rtp%sdepay %s "
      "! fakesink name=sink sync=false", url, codec, parse);
  GstElement *sink;

  c->pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  sink = gst_bin_get_by_name (GST_BIN (c->pipe), "sink");
  c->sinkpad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (c->sinkpad, GST_PAD_PROBE_TYPE_BUFFER, count_buffer, c, NULL);
  gst_object_unref (sink);
  g_assert_cmpint (gst_element_set_state (c->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_free (desc);
  g_free (parse);
  return c;
}

static void
client_free (Client * c)
{
  gst_element_set_state (c->pipe, GST_STATE_NULL);
  gst_object_unref (c->sinkpad);
  gst_object_unref (c->pipe);
  g_free (c);
}

static gint
client_buffers (Client * c)
{
  return g_atomic_int_get (&c->buffers);
}

/* The first ERROR or EOS on the bus within @timeout_ms, or NULL. Fails the test on an error when @error_fails. */
static GstMessage *
client_pop (Client * c, guint timeout_ms, gboolean error_fails)
{
  GstBus *bus = gst_element_get_bus (c->pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, timeout_ms * GST_MSECOND,
      GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  gst_object_unref (bus);
  if (msg != NULL && error_fails) {
    GError *err = NULL;
    gchar *dbg = NULL;

    if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_EOS)
      g_error ("unexpected EOS from %s", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)));
    gst_message_parse_error (msg, &err, &dbg);
    g_error ("client error from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message, dbg);
  }
  return msg;
}

/* Waits until the client has @n buffers; fails on a bus error or after PLAY_TIMEOUT_US. */
static void
client_wait_buffers (Client * c, gint n)
{
  gint64 deadline = g_get_monotonic_time () + PLAY_TIMEOUT_US;

  while (client_buffers (c) < n) {
    if (g_get_monotonic_time () > deadline)
      g_error ("%d buffers after %d s, expected %d", client_buffers (c), (gint) (PLAY_TIMEOUT_US / G_USEC_PER_SEC), n);
    g_assert_null (client_pop (c, 50, TRUE));
  }
}

/* The negotiated size at the client's sink. */
static void
client_size (Client * c, gint * width, gint * height)
{
  GstCaps *caps = gst_pad_get_current_caps (c->sinkpad);
  GstStructure *st;

  g_assert_nonnull (caps);
  st = gst_caps_get_structure (caps, 0);
  g_assert_true (gst_structure_get_int (st, "width", width));
  g_assert_true (gst_structure_get_int (st, "height", height));
  gst_caps_unref (caps);
}

/* An ERROR or EOS within @timeout_ms; returns its type. */
static GstMessageType
client_wait_end (Client * c, guint timeout_ms)
{
  GstMessage *msg = client_pop (c, timeout_ms, FALSE);
  GstMessageType type;

  if (msg == NULL)
    g_error ("no error or EOS within %u ms", timeout_ms);
  type = GST_MESSAGE_TYPE (msg);
  gst_message_unref (msg);
  return type;
}

/* ---- the tests ---- */

static void
play (const gchar * codec, gboolean via_relay)
{
  RdsTestRtspServer *s = server_or_skip (320, 240, codec);
  gchar *url;
  Client *c;
  gint w, h;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, via_relay);
  g_assert_true (g_str_has_prefix (url, "rtsp://127.0.0.1:"));
  g_assert_true (g_str_has_suffix (url, "/test"));
  c = client_start (url, codec);
  client_wait_buffers (c, 30);
  if (!g_str_equal (codec, "jpeg")) {
    client_size (c, &w, &h);
    g_assert_cmpint (w, ==, 320);
    g_assert_cmpint (h, ==, 240);
  }
  client_free (c);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

static void
test_plays (void)
{
  play ("h264", TRUE);
}

static void
test_plays_direct (void)
{
  play ("h264", FALSE);
}

static void
test_h265 (void)
{
  play ("h265", TRUE);
}

static void
test_jpeg (void)
{
  play ("jpeg", TRUE);
}

/* Waits until the client's buffer count stays the same over a 200 ms step (what was already through the relay has
 * drained), for up to 3 s; returns the count. */
static gint
client_wait_stable (Client * c)
{
  gint64 deadline = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
  gint n = client_buffers (c), prev;

  do {
    prev = n;
    g_assert_null (client_pop (c, 200, TRUE));
    n = client_buffers (c);
  } while (n != prev && g_get_monotonic_time () < deadline);
  if (n != prev)
    g_error ("buffers still arriving 3 s after the freeze (%d)", n);
  return n;
}

/* Frozen: once drained, no buffers and no error for 2 s; passing again, the same session goes on. */
static void
test_freeze (void)
{
  RdsTestRtspServer *s = server_or_skip (320, 240, "h264");
  gchar *url;
  Client *c;
  gint n;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  c = client_start (url, "h264");
  client_wait_buffers (c, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_FREEZE);
  n = client_wait_stable (c);
  g_assert_null (client_pop (c, 2000, TRUE));
  g_assert_cmpint (client_buffers (c), ==, n);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
  client_wait_buffers (c, n + 30);
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, 1);
  client_free (c);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* Cut: the client ends (rtspsrc posts EOS or an error) within 5 s; a new client plays again. */
static void
test_cut (void)
{
  RdsTestRtspServer *s = server_or_skip (320, 240, "h264");
  gchar *url;
  Client *c;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  c = client_start (url, "h264");
  client_wait_buffers (c, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
  (void) client_wait_end (c, 5000);
  client_free (c);

  c = client_start (url, "h264");
  client_wait_buffers (c, 30);
  client_free (c);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* Refuse: a playing client is cut, and a new one fails with an error; passing again, a new client plays. */
static void
test_refuse (void)
{
  RdsTestRtspServer *s = server_or_skip (320, 240, "h264");
  gchar *url;
  Client *c;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  c = client_start (url, "h264");
  client_wait_buffers (c, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_REFUSE);
  (void) client_wait_end (c, 5000);
  client_free (c);

  c = client_start (url, "h264");
  g_assert_cmpint (client_wait_end (c, 10000), ==, GST_MESSAGE_ERROR);
  g_assert_cmpint (client_buffers (c), ==, 0);
  client_free (c);

  rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
  c = client_start (url, "h264");
  client_wait_buffers (c, 30);
  client_free (c);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* A new stream size reaches the sessions made after the change, not the playing one. */
static void
test_set_stream (void)
{
  RdsTestRtspServer *s = server_or_skip (320, 240, "h264");
  gchar *url;
  Client *a, *b;
  gint w, h;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, FALSE);
  a = client_start (url, "h264");
  client_wait_buffers (a, 10);
  rds_test_rtsp_server_set_stream (s, 640, 480, "h264");
  b = client_start (url, "h264");
  client_wait_buffers (b, 10);
  client_size (b, &w, &h);
  g_assert_cmpint (w, ==, 640);
  g_assert_cmpint (h, ==, 480);
  client_wait_buffers (a, client_buffers (a) + 10);
  client_size (a, &w, &h);
  g_assert_cmpint (w, ==, 320);
  g_assert_cmpint (h, ==, 240);
  client_free (b);
  client_free (a);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The session counter: one per client, direct or through the relay. */
static void
test_sessions (void)
{
  RdsTestRtspServer *s = server_or_skip (320, 240, "h264");
  gchar *direct, *relayed;
  Client *c;

  if (s == NULL)
    return;
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, 0);
  direct = rds_test_rtsp_server_url (s, FALSE);
  relayed = rds_test_rtsp_server_url (s, TRUE);
  g_assert_cmpstr (direct, !=, relayed);
  c = client_start (direct, "h264");
  client_wait_buffers (c, 10);
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, 1);
  client_free (c);
  c = client_start (relayed, "h264");
  client_wait_buffers (c, 10);
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, 2);
  client_free (c);
  g_free (relayed);
  g_free (direct);
  rds_test_rtsp_server_free (s);
}

/* Freed with clients still connected through the relay and direct (they end, the server's threads stop). */
static void
test_free_connected (void)
{
  RdsTestRtspServer *s = server_or_skip (320, 240, "h264");
  gchar *direct, *relayed;
  Client *a, *b;

  if (s == NULL)
    return;
  direct = rds_test_rtsp_server_url (s, FALSE);
  relayed = rds_test_rtsp_server_url (s, TRUE);
  a = client_start (direct, "h264");
  b = client_start (relayed, "h264");
  client_wait_buffers (a, 10);
  client_wait_buffers (b, 10);
  rds_test_rtsp_server_free (s);
  (void) client_wait_end (b, 5000);
  client_free (b);
  client_free (a);
  g_free (relayed);
  g_free (direct);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/rtsp-server/plays", test_plays);
  g_test_add_func ("/gpu/rtsp-server/plays-direct", test_plays_direct);
  g_test_add_func ("/gpu/rtsp-server/h265", test_h265);
  g_test_add_func ("/gpu/rtsp-server/jpeg", test_jpeg);
  g_test_add_func ("/gpu/rtsp-server/freeze", test_freeze);
  g_test_add_func ("/gpu/rtsp-server/cut", test_cut);
  g_test_add_func ("/gpu/rtsp-server/refuse", test_refuse);
  g_test_add_func ("/gpu/rtsp-server/set-stream", test_set_stream);
  g_test_add_func ("/gpu/rtsp-server/sessions", test_sessions);
  g_test_add_func ("/gpu/rtsp-server/free-connected", test_free_connected);
  ret = g_test_run ();
  gst_deinit ();
  return ret;
}
