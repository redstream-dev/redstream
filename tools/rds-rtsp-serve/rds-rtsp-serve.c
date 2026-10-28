/* rds-rtsp-serve: serves a live test pattern (or a looped H.264/H.265 file) over RTSP, optionally behind a TCP relay
 * that injects faults on a schedule: a frozen camera, a dropped connection, a camera that refuses connections, one
 * that comes back with another size or codec. For manual checks and soaks of RTSP clients; the server and the relay
 * are the ones the tests use (rtsp-test-server.h).
 *
 * The URLs go to stdout ("direct URL" and "relay URL" lines, line-buffered), faults and errors to stderr. Runs until
 * SIGINT or SIGTERM. With the relay, SIGUSR1 cuts the relayed connections now (as a cut fault), for scripts that
 * cut once their client is up. Exit status: 0 after SIGINT or SIGTERM, 1 on errors.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rtsp-test-server.h"

#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gst/gst.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#define PROG "rds-rtsp-serve"
/* The latest fault time, in seconds (about 11.5 days; its milliseconds fit a guint timeout) */
#define MAX_FAULT_T 1e6
/* How long the --file steps may take: reading the stream's caps, and rewriting a container's stream */
#define FILE_CAPS_TIMEOUT (10 * GST_SECOND)
#define REMUX_TIMEOUT_US (300 * G_USEC_PER_SEC)

/* Set by SIGINT and SIGTERM; main_loop runs once the server is up. */
static gboolean stopping;
static GMainLoop *main_loop;

/* What the server streams now (for size= and codec=, which change one of the two). */
typedef struct
{
  RdsTestRtspServer *server;
  guint width, height;
  gchar *codec;
  gdouble start;                /* monotonic seconds at start */
} State;

typedef enum
{
  FAULT_RELAY,
  FAULT_SIZE,
  FAULT_CODEC,
} FaultKind;

typedef struct
{
  State *state;
  gdouble at;                   /* seconds from start */
  FaultKind kind;
  RdsRelayMode mode;
  guint width, height;
  gchar *codec;
  gchar *text;                  /* as given */
  guint source;                 /* its timeout, until it fires */
} Fault;

static void
fault_free (Fault * f)
{
  if (f->source)
    g_source_remove (f->source);
  g_free (f->codec);
  g_free (f->text);
  g_free (f);
}

static gboolean
parse_size (const gchar * s, guint * width, guint * height)
{
  gchar **wh = g_strsplit (s, "x", -1);
  guint64 w, h;
  gboolean ok = g_strv_length (wh) == 2
      && g_ascii_string_to_unsigned (wh[0], 10, 16, 8192, &w, NULL)
      && g_ascii_string_to_unsigned (wh[1], 10, 16, 8192, &h, NULL);

  g_strfreev (wh);
  if (ok) {
    *width = (guint) w;
    *height = (guint) h;
  }
  return ok;
}

/* "T:action,..." into a list of Fault, sorted by time. */
static gboolean
parse_faults (const gchar * spec, gboolean have_relay, GList ** out, GError ** err)
{
  gchar **items = g_strsplit (spec, ",", -1);
  gboolean ok = TRUE;

  for (gchar ** it = items; ok && *it != NULL; it++) {
    gchar *item = g_strstrip (*it), *colon = strchr (item, ':'), *end = NULL;
    Fault *f;

    if (*item == '\0')
      continue;
    f = g_new0 (Fault, 1);
    f->text = g_strdup (item);
    if (colon == NULL || (f->at = g_ascii_strtod (item, &end), end != colon) || !(f->at >= 0 && f->at <= MAX_FAULT_T)) {
      g_set_error (err, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
          "--faults: \"%s\" is not T:action (T in seconds, 0..%g)", item, MAX_FAULT_T);
      ok = FALSE;
    } else {
      const gchar *a = colon + 1;

      if (g_str_equal (a, "freeze") || g_str_equal (a, "cut") || g_str_equal (a, "refuse")
          || g_str_equal (a, "pass")) {
        f->kind = FAULT_RELAY;
        f->mode = g_str_equal (a, "freeze") ? RDS_RELAY_FREEZE : g_str_equal (a, "cut") ? RDS_RELAY_CUT :
            g_str_equal (a, "refuse") ? RDS_RELAY_REFUSE : RDS_RELAY_PASS;
        if (!have_relay) {
          g_set_error (err, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--faults: %s needs --proxy-port", a);
          ok = FALSE;
        }
      } else if (g_str_has_prefix (a, "size=")) {
        f->kind = FAULT_SIZE;
        if (!parse_size (a + 5, &f->width, &f->height)) {
          g_set_error (err, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--faults: bad size in \"%s\"", item);
          ok = FALSE;
        }
      } else if (g_str_has_prefix (a, "codec=")) {
        gchar *why = NULL;

        f->kind = FAULT_CODEC;
        f->codec = g_strdup (a + 6);
        if (!rds_test_rtsp_server_codec_available (f->codec, &why)) {
          g_set_error (err, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--faults: \"%s\": %s", item, why);
          g_free (why);
          ok = FALSE;
        }
      } else {
        g_set_error (err, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
            "--faults: unknown action in \"%s\" (freeze, cut, refuse, pass, size=WxH, codec=C)", item);
        ok = FALSE;
      }
    }
    *out = g_list_append (*out, f);
  }
  g_strfreev (items);
  return ok;
}

static gboolean
fire_fault (gpointer data)
{
  Fault *f = data;
  State *st = f->state;

  f->source = 0;
  switch (f->kind) {
    case FAULT_RELAY:
      rds_test_rtsp_server_set_relay (st->server, f->mode);
      break;
    case FAULT_SIZE:
      st->width = f->width;
      st->height = f->height;
      rds_test_rtsp_server_set_stream (st->server, st->width, st->height, st->codec);
      break;
    case FAULT_CODEC:
      g_free (st->codec);
      st->codec = g_strdup (f->codec);
      rds_test_rtsp_server_set_stream (st->server, st->width, st->height, st->codec);
      break;
  }
  g_printerr (PROG ": %.1f s: %s%s\n", g_get_monotonic_time () / 1e6 - st->start, f->text,
      f->kind == FAULT_RELAY ? "" : " (for new sessions)");
  return G_SOURCE_REMOVE;
}

/* ---- --file ---- */

/* @desc with @path set as the location of its "src" element (and @out of its "sink" element, if given): set as
 * properties, so no character of a path needs escaping. */
static GstElement *
file_pipeline (const gchar * desc, const gchar * path, const gchar * out, GError ** err)
{
  GstElement *pipe = gst_parse_launch (desc, err), *e;

  if (pipe == NULL)
    return NULL;
  e = gst_bin_get_by_name (GST_BIN (pipe), "src");
  g_object_set (e, "location", path, NULL);
  gst_object_unref (e);
  if (out) {
    e = gst_bin_get_by_name (GST_BIN (pipe), "sink");
    g_object_set (e, "location", out, NULL);
    gst_object_unref (e);
  }
  return pipe;
}

/* The caps of @path's first H.264 or H.265 stream as parsebin hands it out (other streams, audio say, are left
 * unlinked). */
static GstCaps *
file_caps (const gchar * path, GError ** err)
{
  GstElement *pipe = file_pipeline ("filesrc name=src ! parsebin "
      "! capsfilter caps=\"video/x-h264;video/x-h265\" ! fakesink name=sink", path, NULL, err), *sink;
  GstCaps *caps = NULL;
  GstMessage *msg;
  GstBus *bus;
  GstPad *pad;

  if (pipe == NULL)
    return NULL;
  bus = gst_element_get_bus (pipe);
  gst_element_set_state (pipe, GST_STATE_PAUSED);
  msg = gst_bus_timed_pop_filtered (bus, FILE_CAPS_TIMEOUT, GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR);
  if (msg == NULL || GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *e = NULL;

    if (msg)
      gst_message_parse_error (msg, &e, NULL);
    g_set_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND, "no H.264/H.265 video stream%s%s",
        e ? ": " : " found within 10 s", e ? e->message : "");
    g_clear_error (&e);
  } else {
    sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
    pad = gst_element_get_static_pad (sink, "sink");
    caps = gst_pad_get_current_caps (pad);
    gst_object_unref (pad);
    gst_object_unref (sink);
  }
  g_clear_pointer (&msg, gst_message_unref);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (pipe);
  return caps;
}

/* Rewrites @path's @codec stream as a byte stream (with parameter sets at every key frame) into @out. Gives up after
 * REMUX_TIMEOUT_US, or when SIGINT or SIGTERM come (stopping). */
static gboolean
remux_file (const gchar * path, const gchar * codec, const gchar * out, GError ** err)
{
  gchar *desc = g_strdup_printf ("filesrc name=src ! parsebin ! capsfilter caps=video/x-%s ! %sparse "
      "config-interval=-1 ! video/x-%s,stream-format=byte-stream,alignment=au ! filesink name=sink", codec, codec,
      codec);
  GstElement *pipe = file_pipeline (desc, path, out, err);
  gint64 deadline = g_get_monotonic_time () + REMUX_TIMEOUT_US;
  GstMessage *msg = NULL;
  GstBus *bus;
  gboolean ok = FALSE;

  g_free (desc);
  if (pipe == NULL)
    return FALSE;
  bus = gst_element_get_bus (pipe);
  gst_element_set_state (pipe, GST_STATE_PLAYING);
  while (msg == NULL && !stopping && g_get_monotonic_time () < deadline) {
    msg = gst_bus_timed_pop_filtered (bus, 100 * GST_MSECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    /* the signal sources */
    while (g_main_context_iteration (NULL, FALSE));
  }
  if (msg == NULL)
    g_set_error (err, GST_CORE_ERROR, GST_CORE_ERROR_FAILED, stopping ? "interrupted" :
        "rewriting the stream as a byte stream took over %d s", (gint) (REMUX_TIMEOUT_US / G_USEC_PER_SEC));
  else if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR)
    gst_message_parse_error (msg, err, NULL);
  else
    ok = TRUE;
  g_clear_pointer (&msg, gst_message_unref);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (pipe);
  return ok;
}

/* @path as a quoted launch-line value for multifilesrc: its location is a printf pattern ('%' doubled) and the
 * launch parser unescapes backslashes ('"' and '\' escaped). */
static gchar *
quote_location (const gchar * path)
{
  GString *q = g_string_new ("\"");

  for (const gchar * c = path; *c; c++) {
    if (*c == '%')
      g_string_append_c (q, '%');
    else if (*c == '"' || *c == '\\')
      g_string_append_c (q, '\\');
    g_string_append_c (q, *c);
  }
  g_string_append_c (q, '"');
  return g_string_free (q, FALSE);
}

/* The media launch line that loops @path (H.264 or H.265, byte stream or in a container; a container's stream is
 * first rewritten into *@tmp, which the caller deletes). The parser times the frames at the stream's frame rate
 * (30 fps when it has none). multifilesrc reads the whole file as one buffer each loop, so a session holds about the
 * file's size in memory: meant for short clips. */
static gchar *
file_launch (const gchar * path, gchar ** tmp, GError ** err)
{
  GstCaps *caps = file_caps (path, err);
  GstStructure *st;
  const gchar *codec, *format;
  gint fps_n = 0, fps_d = 1;
  gchar *launch = NULL, *loc;

  if (caps == NULL)
    return NULL;
  st = gst_caps_get_structure (caps, 0);
  codec = gst_structure_has_name (st, "video/x-h264") ? "h264" : "h265";
  if (!gst_structure_get_fraction (st, "framerate", &fps_n, &fps_d) || fps_n <= 0) {
    fps_n = 30;
    fps_d = 1;
  }
  format = gst_structure_get_string (st, "stream-format");
  if (g_strcmp0 (format, "byte-stream") == 0)
    loc = quote_location (path);
  else {
    gchar *name = g_strdup_printf (PROG "-XXXXXX.%s", codec);
    gint fd = g_file_open_tmp (name, tmp, err);

    g_free (name);
    if (fd < 0)
      goto out;
    g_close (fd, NULL);
    if (!remux_file (path, codec, *tmp, err))
      goto out;
    loc = quote_location (*tmp);
  }
  launch = g_strdup_printf ("( multifilesrc location=%s loop=true "
      "caps=\"video/x-%s,stream-format=byte-stream,framerate=%d/%d\" ! %sparse ! rtp%spay name=pay0 pt=96 "
      "config-interval=-1 )", loc, codec, fps_n, fps_d, codec, codec);
  g_free (loc);
out:
  gst_caps_unref (caps);
  return launch;
}

static gboolean
on_signal (G_GNUC_UNUSED gpointer data)
{
  stopping = TRUE;
  if (main_loop)
    g_main_loop_quit (main_loop);
  return G_SOURCE_CONTINUE;
}

/* SIGUSR1, with the relay: a cut now. */
static gboolean
on_cut (gpointer data)
{
  State *st = data;

  g_printerr (PROG ": SIGUSR1: cut\n");
  rds_test_rtsp_server_set_relay (st->server, RDS_RELAY_CUT);
  return G_SOURCE_CONTINUE;
}

int
main (int argc, char **argv)
{
  gint port = 8554, proxy_port = -1;
  gchar *mount = NULL, *size = NULL, *codec = NULL, *file = NULL, *faults = NULL, *address = NULL;
  const GOptionEntry entries[] = {
    {"port", 'p', 0, G_OPTION_ARG_INT, &port, "The RTSP port (default 8554; 0: a free one)", "PORT"},
    {"address", 'a', 0, G_OPTION_ARG_STRING, &address, "The address to bind (default 127.0.0.1)", "ADDR"},
    {"mount", 'm', 0, G_OPTION_ARG_STRING, &mount, "The mount point (default /test)", "PATH"},
    {"size", 's', 0, G_OPTION_ARG_STRING, &size,
        "The test pattern's size (default 1280x720; with --file, for size= and codec= faults)", "WxH"},
    {"codec", 'c', 0, G_OPTION_ARG_STRING, &codec,
        "h264 (default), h265 or jpeg (with --file, for size= and codec= faults)", "CODEC"},
    {"file", 'f', 0, G_OPTION_ARG_FILENAME, &file,
        "Loop this H.264/H.265 file (byte stream or a container such as MP4) instead of the test pattern", "PATH"},
    {"proxy-port", 'r', 0, G_OPTION_ARG_INT, &proxy_port, "Put the fault relay in front, on this port (0: a free one)",
        "PORT"},
    {"faults", 0, 0, G_OPTION_ARG_STRING, &faults,
        "Comma-separated T:action, T in seconds from the start; actions freeze, cut, refuse, pass (the relay's) and "
          "size=WxH, codec=C (for new sessions)", "LIST"},
    G_OPTION_ENTRY_NULL
  };
  RdsTestRtspServerOptions opts = { 0 };
  State st = { 0 };
  GOptionContext *ctx;
  GList *fault_list = NULL;
  GMainLoop *loop = NULL;
  gchar *launch = NULL, *tmp = NULL, *why = NULL, *url;
  guint signals[3] = { 0, 0, 0 };
  GError *err = NULL;
  int ret = 1;

  setvbuf (stdout, NULL, _IOLBF, 0);
  ctx = g_option_context_new ("- serve a test stream over RTSP, with scheduled faults");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  g_option_context_set_description (ctx,
      "Prints \"direct URL\" and, with --proxy-port, \"relay URL\". Only RTSP over TCP (interleaved) goes through\n"
      "the relay. freeze stops forwarding without closing, cut closes the relayed connections, refuse closes them\n"
      "and every new one, pass forwards again (SIGUSR1 cuts too). size= and codec= switch to the test pattern for\n"
      "new sessions.\n"
      "With --file, --size and --codec are ignored until such a fault switches to the test pattern.\n"
      "Example: --proxy-port 8555 --faults \"10:freeze,20:pass,30:cut,40:size=640x480,50:refuse,60:pass\"\n");
  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    g_printerr (PROG ": %s (see --help)\n", err->message);
    goto out;
  }
  if (argc > 1) {
    g_printerr (PROG ": unexpected argument %s (see --help)\n", argv[1]);
    goto out;
  }
  if (port < 0 || port > 65535 || proxy_port < -1 || proxy_port > 65535) {
    g_printerr (PROG ": ports must be 0..65535\n");
    goto out;
  }
  st.width = 1280;
  st.height = 720;
  st.codec = g_strdup (codec ? codec : "h264");
  if (size && !parse_size (size, &st.width, &st.height)) {
    g_printerr (PROG ": --size %s: not WxH (16..8192)\n", size);
    goto out;
  }
  if (faults && !parse_faults (faults, proxy_port >= 0, &fault_list, &err)) {
    g_printerr (PROG ": %s\n", err->message);
    goto out;
  }
  if (file && (size || codec))
    g_printerr (PROG ": warning: --size and --codec apply only once a size= or codec= fault switches from --file to "
        "the test pattern\n");

  /* from here on, SIGINT and SIGTERM end the run cleanly (also while a --file is rewritten) */
  signals[0] = g_unix_signal_add (SIGINT, on_signal, NULL);
  signals[1] = g_unix_signal_add (SIGTERM, on_signal, NULL);
  if (file && !(launch = file_launch (file, &tmp, &err))) {
    if (stopping) {
      g_printerr (PROG ": stopping\n");
      ret = 0;
    } else
      g_printerr (PROG ": --file %s: %s\n", file, err->message);
    goto out;
  }

  opts.address = address;
  opts.port = (guint) port;
  opts.mount = mount;
  opts.relay = proxy_port >= 0;
  opts.relay_port = proxy_port >= 0 ? (guint) proxy_port : 0;
  opts.width = st.width;
  opts.height = st.height;
  opts.codec = st.codec;
  opts.launch = launch;
  if (!(st.server = rds_test_rtsp_server_new_full (&opts, &why))) {
    g_printerr (PROG ": %s\n", why);
    goto out;
  }
  st.start = g_get_monotonic_time () / 1e6;
  if (file)
    g_printerr (PROG ": serving %s, looped\n", file);
  else
    g_printerr (PROG ": serving the test pattern, %ux%u %s\n", st.width, st.height, st.codec);
  url = rds_test_rtsp_server_url (st.server, FALSE);
  g_print ("direct %s\n", url);
  g_free (url);
  if (opts.relay) {
    signals[2] = g_unix_signal_add (SIGUSR1, on_cut, &st);
    url = rds_test_rtsp_server_url (st.server, TRUE);
    g_print ("relay %s\n", url);
    g_free (url);
  }

  loop = main_loop = g_main_loop_new (NULL, FALSE);
  for (GList * l = fault_list; l != NULL; l = l->next) {
    Fault *f = l->data;

    f->state = &st;
    f->source = g_timeout_add ((guint) (f->at * 1000), fire_fault, f);
  }
  /* a signal during the startup */
  if (!stopping)
    g_main_loop_run (loop);
  g_printerr (PROG ": stopping\n");
  ret = 0;

out:
  for (guint i = 0; i < G_N_ELEMENTS (signals); i++) {
    if (signals[i])
      g_source_remove (signals[i]);
  }
  main_loop = NULL;
  g_clear_pointer (&st.server, rds_test_rtsp_server_free);
  g_clear_pointer (&loop, g_main_loop_unref);
  g_list_free_full (fault_list, (GDestroyNotify) fault_free);
  if (tmp)
    g_unlink (tmp);
  g_clear_error (&err);
  g_option_context_free (ctx);
  g_free (st.codec);
  g_free (tmp);
  g_free (why);
  g_free (launch);
  g_free (address);
  g_free (faults);
  g_free (file);
  g_free (codec);
  g_free (size);
  g_free (mount);
  return ret;
}
