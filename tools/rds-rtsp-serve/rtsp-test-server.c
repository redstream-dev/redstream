/* An RTSP test server with a TCP fault relay in front of it (see rtsp-test-server.h).
 *
 * The server is attached to its own GMainContext, run by its own thread, and so are its clients' connections (no
 * client threads): the shutdown closes them on the thread that reads them. Its media pipelines run on
 * gst-rtsp-server's media threads.
 * The relay is plain GSocket I/O on threads of its own: one accepts, and one per connection polls both sockets and
 * copies whatever is readable to the other side, unless frozen. Cutting cancels a connection's GCancellable, which
 * wakes its thread wherever it waits; the thread closes both sockets and is joined.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rtsp-test-server.h"

#include <errno.h>
#include <gst/gst.h>
#include <string.h>

#define DEFAULT_ADDRESS "127.0.0.1"
#define DEFAULT_MOUNT "/test"

/* The elements the test pattern needs per codec (the encoder first). */
static const gchar *const h264_elements[] = { "vah264enc", "h264parse", "rtph264pay", NULL };
static const gchar *const h265_elements[] = { "vah265enc", "h265parse", "rtph265pay", NULL };
static const gchar *const jpeg_elements[] = { "jpegenc", "rtpjpegpay", NULL };
static const gchar *const audio_elements[] = { "audiotestsrc", "opusenc", "rtpopuspay", NULL };

/* TRUE when every element of @need is there; FALSE and the first missing one in *@why otherwise. */
static gboolean
have_elements (const gchar * const *need, gchar ** why)
{
  for (; *need != NULL; need++) {
    GstElementFactory *f = gst_element_factory_find (*need);

    if (f == NULL) {
      if (why)
        *why = g_strdup_printf ("no %s element%s", *need,
            g_str_has_prefix (*need, "va") ? " (the VA encoder needs a VA device)" : "");
      return FALSE;
    }
    gst_object_unref (f);
  }
  return TRUE;
}

gboolean
rds_test_rtsp_server_codec_available (const gchar * codec, gchar ** why)
{
  const gchar *const *need;

  if (g_strcmp0 (codec, "h264") == 0)
    need = h264_elements;
  else if (g_strcmp0 (codec, "h265") == 0)
    need = h265_elements;
  else if (g_strcmp0 (codec, "jpeg") == 0)
    need = jpeg_elements;
  else {
    if (why)
      *why = g_strdup_printf ("unknown codec \"%s\" (h264, h265 or jpeg)", codec ? codec : "(null)");
    return FALSE;
  }
  return have_elements (need, why);
}

gboolean
rds_test_rtsp_server_audio_available (gchar ** why)
{
  return have_elements (audio_elements, why);
}

#ifdef RDS_HAVE_RTSP_SERVER

#include <gio/gio.h>
#include <gst/rtsp-server/rtsp-server.h>

/* One relayed connection: the client's socket, the server's, and the thread copying between them. */
typedef struct
{
  RdsTestRtspServer *s;
  GSocket *down;                /* accepted from the client */
  GSocket *up;                  /* to the server, connected by the thread */
  GCancellable *cancel;
  GThread *thread;
  gint done;                    /* atomic: the thread has finished */
} RelayConn;

struct _RdsTestRtspServer
{
  gchar *address;               /* for URLs and the relay's upstream connections */
  gchar *mount;
  guint port;

  GMainContext *ctx;
  GMainLoop *loop;
  GThread *thread;
  GstRTSPServer *server;
  GstRTSPMediaFactory *factory;
  gboolean audio;               /* the test pattern comes with a test tone */
  GSource *server_source;
  gint sessions;                /* atomic */
  GPtrArray *clients;           /* HeldClient, on the server's thread (or in free, once it has stopped) */
  GSource *reaper;              /* releases the closed ones */

  /* the relay (listen NULL without one) */
  GSocket *listen;
  guint relay_port;
  GCancellable *accept_cancel;
  GThread *accept_thread;
  gint mode;                    /* atomic RdsRelayMode: PASS, FREEZE or REFUSE */
  GMutex lock;                  /* conns */
  GList *conns;                 /* RelayConn */
};

/* A client we keep a ref on: gst-rtsp-server 1.28 closes a client whose data send failed (its peer went away) in an
 * idle source holding no ref on it (do_send_data and do_send_data_list attach do_close in rtsp-client.c), and the
 * closed connection can finalize the client first; the idle then closes freed memory (seen on cuts, about 1 in
 * 1000). The ref is dropped some time after the client closed: closing clears the client's send functions under its
 * send lock before "closed" is emitted, so after that only a send already in flight can still attach the idle, which
 * takes microseconds; 2 s is ample. */
typedef struct
{
  GstRTSPClient *client;
  gint64 closed_at;             /* monotonic us, 0 while open */
} HeldClient;

#define HELD_CLIENT_LINGER_US (2 * G_USEC_PER_SEC)

static void
held_client_free (HeldClient * h)
{
  g_object_unref (h->client);
  g_free (h);
}

/* Servers alive: gst-rtsp-server's media thread pool is process-wide, cleaned up when the last one goes. */
static gint live_servers;

/* ---- the media ---- */

/* The media factory, with media keyed by the mount path alone: a shared media is then the same for clients through
 * the relay and direct (gst-rtsp-server's key is the whole request URI, port included). */
typedef GstRTSPMediaFactory RdsTestFactory;
typedef GstRTSPMediaFactoryClass RdsTestFactoryClass;

G_DEFINE_TYPE (RdsTestFactory, rds_test_factory, GST_TYPE_RTSP_MEDIA_FACTORY);

static gchar *
rds_test_factory_gen_key (G_GNUC_UNUSED GstRTSPMediaFactory * factory, const GstRTSPUrl * url)
{
  return g_strdup (url->abspath);
}

static void
rds_test_factory_class_init (RdsTestFactoryClass * klass)
{
  klass->gen_key = rds_test_factory_gen_key;
}

static void
rds_test_factory_init (G_GNUC_UNUSED RdsTestFactory * factory)
{
}

/* The test pattern in @codec (NULL: none), and with @audio a test tone in Opus after it (pay1, or pay0 alone). */
static gchar *
pattern_launch (guint width, guint height, const gchar * codec, gboolean audio)
{
  gchar *video, *tone, *launch;

  if (codec == NULL)
    video = g_strdup ("");
  else if (g_str_equal (codec, "jpeg"))
    video = g_strdup_printf ("videotestsrc is-live=true pattern=ball "
        "! video/x-raw,format=I420,width=%u,height=%u,framerate=30/1 ! jpegenc ! rtpjpegpay name=pay0 pt=26", width,
        height);
  else
    /* config-interval=-1: parameter sets with every key frame, so a client that joins or comes back late can start */
    video = g_strdup_printf ("videotestsrc is-live=true pattern=ball "
        "! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 ! va%senc key-int-max=30 b-frames=0 "
        "! %sparse ! rtp%spay name=pay0 pt=96 config-interval=-1", width, height, codec, codec, codec);
  tone = audio ? g_strdup_printf ("audiotestsrc is-live=true ! opusenc ! rtpopuspay name=pay%d pt=97",
      codec != NULL ? 1 : 0) : g_strdup ("");
  launch = g_strdup_printf ("( %s %s )", video, tone);
  g_free (tone);
  g_free (video);
  return launch;
}

static void
on_new_session (G_GNUC_UNUSED GstRTSPClient * client, G_GNUC_UNUSED GstRTSPSession * session,
    RdsTestRtspServer * s)
{
  /* emitted while the client's requests are handled: on the server's thread (see the thread pool in new_full) */
  g_warn_if_fail (g_main_context_is_owner (s->ctx));
  g_atomic_int_inc (&s->sessions);
}

static void
on_client_closed (GstRTSPClient * client, RdsTestRtspServer * s)
{
  for (guint i = 0; i < s->clients->len; i++) {
    HeldClient *h = g_ptr_array_index (s->clients, i);

    if (h->client == client)
      h->closed_at = g_get_monotonic_time ();
  }
}

static void
on_client_connected (G_GNUC_UNUSED GstRTSPServer * server, GstRTSPClient * client, RdsTestRtspServer * s)
{
  HeldClient *h = g_new0 (HeldClient, 1);

  h->client = g_object_ref (client);
  g_ptr_array_add (s->clients, h);
  g_signal_connect (client, "new-session", G_CALLBACK (on_new_session), s);
  g_signal_connect (client, "closed", G_CALLBACK (on_client_closed), s);
}

static gboolean
reap_clients (gpointer data)
{
  RdsTestRtspServer *s = data;
  gint64 now = g_get_monotonic_time ();

  for (guint i = s->clients->len; i-- > 0;) {
    HeldClient *h = g_ptr_array_index (s->clients, i);

    if (h->closed_at != 0 && now - h->closed_at > HELD_CLIENT_LINGER_US)
      g_ptr_array_remove_index_fast (s->clients, i);
  }
  return G_SOURCE_CONTINUE;
}

/* ---- the relay ---- */

/* Sends all of @data, or FALSE (cancelled, or the peer is gone). */
static gboolean
send_all (GSocket * sock, const gchar * data, gssize len, GCancellable * cancel)
{
  while (len > 0) {
    gssize n = g_socket_send (sock, data, len, cancel, NULL);

    if (n <= 0)
      return FALSE;
    data += n;
    len -= n;
  }
  return TRUE;
}

static gpointer
relay_conn_run (gpointer data)
{
  RelayConn *c = data;
  GSocketAddress *addr = g_inet_socket_address_new_from_string (c->s->address, c->s->port);
  gchar buf[64 * 1024];
  GPollFD fds[3];

  c->up = g_socket_new (g_socket_address_get_family (addr), G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL);
  if (c->up == NULL || !g_socket_connect (c->up, addr, c->cancel, NULL))
    goto out;
  if (!g_cancellable_make_pollfd (c->cancel, &fds[2]))
    goto out;
  while (!g_cancellable_is_cancelled (c->cancel)) {
    /* frozen: read neither side (the peers' writes back up in the kernel), but wake up to see the mode change */
    gboolean frozen = g_atomic_int_get (&c->s->mode) == RDS_RELAY_FREEZE;
    GSocket *socks[2] = { c->down, c->up };

    for (guint i = 0; i < 2; i++) {
      fds[i].fd = frozen ? -1 : g_socket_get_fd (socks[i]);
      fds[i].events = G_IO_IN | G_IO_HUP | G_IO_ERR;
      fds[i].revents = 0;
    }
    fds[2].revents = 0;
    if (g_poll (fds, 3, 50) < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    for (guint i = 0; i < 2; i++) {
      gssize n;

      if (fds[i].revents == 0)
        continue;
      n = g_socket_receive (socks[i], buf, sizeof buf, c->cancel, NULL);
      if (n <= 0 || !send_all (socks[1 - i], buf, n, c->cancel))
        goto done;
    }
  }
done:
  g_cancellable_release_fd (c->cancel);
out:
  /* either side gone: close both, so the other peer sees it too */
  g_socket_close (c->down, NULL);
  if (c->up)
    g_socket_close (c->up, NULL);
  g_object_unref (addr);
  g_atomic_int_set (&c->done, 1);
  return NULL;
}

static void
relay_conn_free (RelayConn * c)
{
  g_cancellable_cancel (c->cancel);
  g_thread_join (c->thread);
  g_clear_object (&c->up);
  g_object_unref (c->down);
  g_object_unref (c->cancel);
  g_free (c);
}

/* Takes the connections out of the list (all, or those whose thread is done) and frees them. */
static void
relay_close (RdsTestRtspServer * s, gboolean all)
{
  GList *gone = NULL;

  g_mutex_lock (&s->lock);
  for (GList * l = s->conns, *next; l != NULL; l = next) {
    RelayConn *c = l->data;

    next = l->next;
    if (all || g_atomic_int_get (&c->done)) {
      s->conns = g_list_remove_link (s->conns, l);
      gone = g_list_concat (l, gone);
    }
  }
  g_mutex_unlock (&s->lock);
  g_list_free_full (gone, (GDestroyNotify) relay_conn_free);
}

static gpointer
relay_accept_run (gpointer data)
{
  RdsTestRtspServer *s = data;

  for (;;) {
    GError *err = NULL;
    GSocket *down = g_socket_accept (s->listen, s->accept_cancel, &err);
    RelayConn *c;

    if (down == NULL) {
      if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        g_warning ("relay: accept failed: %s", err->message);
      g_error_free (err);
      break;
    }
    relay_close (s, FALSE);
    /* under the lock: a connection is either refused or listed before set_relay closes the listed ones */
    g_mutex_lock (&s->lock);
    if (g_atomic_int_get (&s->mode) == RDS_RELAY_REFUSE) {
      g_mutex_unlock (&s->lock);
      g_socket_close (down, NULL);
      g_object_unref (down);
      continue;
    }
    c = g_new0 (RelayConn, 1);
    c->s = s;
    c->down = down;
    c->cancel = g_cancellable_new ();
    c->thread = g_thread_new ("rds-relay-conn", relay_conn_run, c);
    s->conns = g_list_prepend (s->conns, c);
    g_mutex_unlock (&s->lock);
  }
  return NULL;
}

static gboolean
relay_start (RdsTestRtspServer * s, const gchar * address, guint port, GError ** err)
{
  GSocketAddress *addr = g_inet_socket_address_new_from_string (address, port), *bound;

  if (addr == NULL) {
    g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "not an IP address: %s", address);
    return FALSE;
  }
  s->listen = g_socket_new (g_socket_address_get_family (addr), G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, err);
  if (s->listen == NULL || !g_socket_bind (s->listen, addr, TRUE, err) || !g_socket_listen (s->listen, err)
      || !(bound = g_socket_get_local_address (s->listen, err))) {
    g_object_unref (addr);
    g_clear_object (&s->listen);
    return FALSE;
  }
  s->relay_port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (bound));
  g_object_unref (bound);
  g_object_unref (addr);
  s->accept_cancel = g_cancellable_new ();
  s->accept_thread = g_thread_new ("rds-relay-accept", relay_accept_run, s);
  return TRUE;
}

static void
relay_stop (RdsTestRtspServer * s)
{
  if (s->listen == NULL)
    return;
  g_cancellable_cancel (s->accept_cancel);
  g_thread_join (s->accept_thread);
  relay_close (s, TRUE);
  g_socket_close (s->listen, NULL);
  g_clear_object (&s->listen);
  g_clear_object (&s->accept_cancel);
}

/* ---- the server thread ---- */

static gpointer
server_run (gpointer data)
{
  RdsTestRtspServer *s = data;

  g_main_context_push_thread_default (s->ctx);
  g_main_loop_run (s->loop);
  /* let the closed clients' and sessions' cleanups run */
  while (g_main_context_iteration (s->ctx, FALSE));
  g_main_context_pop_thread_default (s->ctx);
  return NULL;
}

static GstRTSPFilterResult
remove_client (G_GNUC_UNUSED GstRTSPServer * server, G_GNUC_UNUSED GstRTSPClient * client,
    G_GNUC_UNUSED gpointer data)
{
  return GST_RTSP_FILTER_REMOVE;
}

static GstRTSPFilterResult
remove_session (G_GNUC_UNUSED GstRTSPSessionPool * pool, G_GNUC_UNUSED GstRTSPSession * session,
    G_GNUC_UNUSED gpointer data)
{
  return GST_RTSP_FILTER_REMOVE;
}

/* On the server thread: stop accepting, close the clients and their sessions, quit. */
static gboolean
server_shutdown (gpointer data)
{
  RdsTestRtspServer *s = data;
  GstRTSPSessionPool *pool = gst_rtsp_server_get_session_pool (s->server);

  g_source_destroy (s->server_source);
  g_list_free (gst_rtsp_server_client_filter (s->server, remove_client, NULL));
  g_list_free (gst_rtsp_session_pool_filter (pool, remove_session, NULL));
  g_object_unref (pool);
  g_main_loop_quit (s->loop);
  return G_SOURCE_REMOVE;
}

RdsTestRtspServer *
rds_test_rtsp_server_new_full (const RdsTestRtspServerOptions * opts, gchar ** why)
{
  RdsTestRtspServer *s;
  GstRTSPMountPoints *mounts;
  GstRTSPThreadPool *pool;
  GError *err = NULL;
  gchar *launch, *service;

  if (opts->launch == NULL) {
    if (opts->codec == NULL && !opts->audio) {
      if (why)
        *why = g_strdup ("no codec and no audio: nothing to serve");
      return NULL;
    }
    if (opts->codec != NULL && !rds_test_rtsp_server_codec_available (opts->codec, why))
      return NULL;
    if (opts->audio && !rds_test_rtsp_server_audio_available (why))
      return NULL;
  }
  s = g_new0 (RdsTestRtspServer, 1);
  s->audio = opts->audio;
  s->address = g_strdup (opts->address ? opts->address : DEFAULT_ADDRESS);
  s->mount = g_strdup (opts->mount ? opts->mount : DEFAULT_MOUNT);
  g_mutex_init (&s->lock);
  s->ctx = g_main_context_new ();
  s->loop = g_main_loop_new (s->ctx, FALSE);
  s->clients = g_ptr_array_new_with_free_func ((GDestroyNotify) held_client_free);

  s->factory = g_object_new (rds_test_factory_get_type (), NULL);
  launch = opts->launch ? g_strdup (opts->launch) : pattern_launch (opts->width, opts->height, opts->codec,
      opts->audio);
  gst_rtsp_media_factory_set_launch (s->factory, launch);
  g_free (launch);
  gst_rtsp_media_factory_set_shared (s->factory, opts->shared);

  s->server = gst_rtsp_server_new ();
  gst_rtsp_server_set_address (s->server, s->address);
  service = g_strdup_printf ("%u", opts->port);
  gst_rtsp_server_set_service (s->server, service);
  g_free (service);
  mounts = gst_rtsp_server_get_mount_points (s->server);
  gst_rtsp_mount_points_add_factory (mounts, s->mount, g_object_ref (s->factory));
  g_object_unref (mounts);
  g_signal_connect (s->server, "client-connected", G_CALLBACK (on_client_connected), s);
  /* Clients on the server's own context. With the default pool (one client thread) gst_rtsp_client_close, which
   * the shutdown calls, raced that thread's read of the connection it closes (a critical now and then). */
  pool = gst_rtsp_server_get_thread_pool (s->server);
  gst_rtsp_thread_pool_set_max_threads (pool, 0);
  g_object_unref (pool);
  s->server_source = gst_rtsp_server_create_source (s->server, NULL, &err);
  if (s->server_source == NULL) {
    if (why)
      *why = g_strdup_printf ("RTSP server on %s:%u: %s", s->address, opts->port, err->message);
    goto fail;
  }
  s->port = (guint) gst_rtsp_server_get_bound_port (s->server);
  if (opts->relay && !relay_start (s, s->address, opts->relay_port, &err)) {
    if (why)
      *why = g_strdup_printf ("relay on %s:%u: %s", s->address, opts->relay_port, err->message);
    goto fail;
  }
  g_source_attach (s->server_source, s->ctx);
  s->reaper = g_timeout_source_new (500);
  g_source_set_callback (s->reaper, reap_clients, s, NULL);
  g_source_attach (s->reaper, s->ctx);
  g_atomic_int_inc (&live_servers);
  s->thread = g_thread_new ("rds-rtsp-server", server_run, s);
  return s;

fail:
  g_clear_error (&err);
  relay_stop (s);
  if (s->server_source)
    g_source_unref (s->server_source);
  gst_object_unref (s->server);
  g_object_unref (s->factory);
  g_ptr_array_unref (s->clients);
  g_main_loop_unref (s->loop);
  g_main_context_unref (s->ctx);
  g_mutex_clear (&s->lock);
  g_free (s->mount);
  g_free (s->address);
  g_free (s);
  return NULL;
}

RdsTestRtspServer *
rds_test_rtsp_server_new (guint width, guint height, const gchar * codec, gchar ** why)
{
  RdsTestRtspServerOptions opts = {
    .relay = TRUE,
    .width = width,
    .height = height,
    .codec = codec,
  };

  return rds_test_rtsp_server_new_full (&opts, why);
}

gchar *
rds_test_rtsp_server_url (RdsTestRtspServer * s, gboolean via_relay)
{
  /* a wildcard bind is reached on the loopback; IPv6 hosts go in brackets */
  const gchar *host = g_str_equal (s->address, "0.0.0.0") ? "127.0.0.1" : g_str_equal (s->address, "::") ? "::1" :
      s->address;
  gboolean v6 = strchr (host, ':') != NULL;

  if (via_relay && s->listen == NULL)
    return NULL;
  return g_strdup_printf ("rtsp://%s%s%s:%u%s", v6 ? "[" : "", host, v6 ? "]" : "",
      via_relay ? s->relay_port : s->port, s->mount);
}

void
rds_test_rtsp_server_set_stream (RdsTestRtspServer * s, guint width, guint height, const gchar * codec)
{
  gchar *launch, *why = NULL;

  if (!rds_test_rtsp_server_codec_available (codec, &why)) {
    g_warning ("stream not changed: %s", why);
    g_free (why);
    return;
  }
  launch = pattern_launch (width, height, codec, s->audio);
  gst_rtsp_media_factory_set_launch (s->factory, launch);
  g_free (launch);
}

void
rds_test_rtsp_server_set_relay (RdsTestRtspServer * s, RdsRelayMode mode)
{
  GList *gone = NULL;

  g_return_if_fail (s->listen != NULL);
  /* the mode and the connections it closes in one step: a connection accepted meanwhile is either in the snapshot
   * or sees the new mode */
  g_mutex_lock (&s->lock);
  g_atomic_int_set (&s->mode, mode == RDS_RELAY_CUT ? RDS_RELAY_PASS : mode);
  if (mode == RDS_RELAY_CUT || mode == RDS_RELAY_REFUSE) {
    gone = s->conns;
    s->conns = NULL;
  }
  g_mutex_unlock (&s->lock);
  g_list_free_full (gone, (GDestroyNotify) relay_conn_free);
}

guint
rds_test_rtsp_server_sessions (RdsTestRtspServer * s)
{
  return (guint) g_atomic_int_get (&s->sessions);
}

void
rds_test_rtsp_server_free (RdsTestRtspServer * s)
{
  if (s == NULL)
    return;
  relay_stop (s);
  g_main_context_invoke (s->ctx, server_shutdown, s);
  g_thread_join (s->thread);
  g_source_unref (s->server_source);
  gst_object_unref (s->server);
  g_object_unref (s->factory);
  /* The server's thread has stopped: run what is left on its context (do_close idles in flight, which need their
   * client), then let the clients go. A client finalized here unprepares the media it still holds (one prepared for a
   * DESCRIBE with no SETUP, say), whose thread the cleanup below would otherwise wait for forever; then run what that
   * left on the context. */
  g_source_destroy (s->reaper);
  g_source_unref (s->reaper);
  while (g_main_context_iteration (s->ctx, FALSE));
  g_ptr_array_set_size (s->clients, 0);
  while (g_main_context_iteration (s->ctx, FALSE));
  /* waits for the media threads (each session's pipeline is set to NULL there) */
  if (g_atomic_int_dec_and_test (&live_servers))
    gst_rtsp_thread_pool_cleanup ();
  g_ptr_array_unref (s->clients);
  g_main_loop_unref (s->loop);
  g_main_context_unref (s->ctx);
  g_mutex_clear (&s->lock);
  g_free (s->mount);
  g_free (s->address);
  g_free (s);
}

#else /* !RDS_HAVE_RTSP_SERVER */

#define NO_SERVER "built without gst-rtsp-server"

RdsTestRtspServer *
rds_test_rtsp_server_new_full (G_GNUC_UNUSED const RdsTestRtspServerOptions * opts, gchar ** why)
{
  if (why)
    *why = g_strdup (NO_SERVER);
  return NULL;
}

RdsTestRtspServer *
rds_test_rtsp_server_new (G_GNUC_UNUSED guint width, G_GNUC_UNUSED guint height,
    G_GNUC_UNUSED const gchar * codec, gchar ** why)
{
  return rds_test_rtsp_server_new_full (NULL, why);
}

/* Never called: there is no server to call them on. */
gchar *
rds_test_rtsp_server_url (G_GNUC_UNUSED RdsTestRtspServer * s, G_GNUC_UNUSED gboolean via_relay)
{
  g_return_val_if_reached (NULL);
}

void
rds_test_rtsp_server_set_stream (G_GNUC_UNUSED RdsTestRtspServer * s, G_GNUC_UNUSED guint width,
    G_GNUC_UNUSED guint height, G_GNUC_UNUSED const gchar * codec)
{
  g_return_if_reached ();
}

void
rds_test_rtsp_server_set_relay (G_GNUC_UNUSED RdsTestRtspServer * s, G_GNUC_UNUSED RdsRelayMode mode)
{
  g_return_if_reached ();
}

guint
rds_test_rtsp_server_sessions (G_GNUC_UNUSED RdsTestRtspServer * s)
{
  g_return_val_if_reached (0);
}

void
rds_test_rtsp_server_free (RdsTestRtspServer * s)
{
  g_return_if_fail (s == NULL);
}

#endif /* RDS_HAVE_RTSP_SERVER */
