/* rdsurisrcbin: factory, pad template and the nvurisrcbin remap, properties (names, types, ranges and defaults as in
 * DeepStream 8.0 behaviour), the warning for accepted no-ops, the settings checked when it starts, and for RTSP URIs
 * the live state changes, rtspsrc's protocols, a server that isn't there, a stream in a codec it can't decode and an
 * audio-only stream (cpu; the last two need gst-rtsp-server, jpegenc and opusenc). Decoding file URIs to vsrc_0 (an
 * MP4, a raw H.264 byte stream, MJPEG through rdsvideoconvert, a file with audio), file-loop and drop-frame-interval;
 * RTSP streams from the test server (H.264 and H.265, TCP through its relay and the default protocols direct),
 * rtspsrc's settings, a restart, a cut connection, drop-frame-interval and disable-audio; and reconnection through the
 * relay's faults: a freeze, a cut, attempts running out or unlimited, the count's reset, two sources in rdsstreammux,
 * the start-up retries and giving up before any connection, the key-frame gate (a shared server), stopping during an
 * attempt, the application's EOS with the source up and down, and the bin's codec error while reconnecting (gpu,
 * skipped without a ROCm device, the VA encoder or gst-rtsp-server; the codec error is cpu).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"
#include "rtsp-test-server.h"

#include <glib/gstdio.h>
#include <rds/rds.h>
#include <rds/rds-compat-remap.h>
#include <stdlib.h>
#include <string.h>

/* ---- the element's warnings, collected from GStreamer's debug log ---- */

static GMutex log_lock;
static GPtrArray *warnings;     /* the text of each WARNING in the rdsurisrcbin category */

static void
collect_warnings (GstDebugCategory * category, GstDebugLevel level, G_GNUC_UNUSED const gchar * file,
    G_GNUC_UNUSED const gchar * function, G_GNUC_UNUSED gint line, G_GNUC_UNUSED GObject * object,
    GstDebugMessage * message, G_GNUC_UNUSED gpointer user_data)
{
  if (level != GST_LEVEL_WARNING || strcmp (gst_debug_category_get_name (category), "rdsurisrcbin") != 0)
    return;
  g_mutex_lock (&log_lock);
  g_ptr_array_add (warnings, g_strdup (gst_debug_message_get (message)));
  g_mutex_unlock (&log_lock);
}

static void
warnings_clear (void)
{
  g_mutex_lock (&log_lock);
  g_ptr_array_set_size (warnings, 0);
  g_mutex_unlock (&log_lock);
}

/* The number of warnings collected so far; with @needle, each must contain it. */
static guint
warnings_count (const gchar * needle)
{
  guint n;

  g_mutex_lock (&log_lock);
  n = warnings->len;
  for (guint i = 0; i < n && needle != NULL; i++) {
    if (strstr (g_ptr_array_index (warnings, i), needle) == NULL)
      g_error ("warning \"%s\" does not mention %s", (const gchar *) g_ptr_array_index (warnings, i), needle);
  }
  g_mutex_unlock (&log_lock);
  return n;
}

/* ---- registered ---- */

static void
test_registered (void)
{
  GstElement *e = gst_element_factory_make ("rdsurisrcbin", NULL);
  GstElementFactory *f;
  const GList *templates;
  GstStaticPadTemplate *t;
  GstCaps *caps;

  g_assert_nonnull (e);
  g_assert_true (GST_IS_BIN (e));
  f = gst_element_get_factory (e);
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (f)), ==, GST_RANK_NONE);

  /* one pad template: sometimes video src pads, memory:RDS NV12 or system-memory NV12 (no audio template) */
  templates = gst_element_factory_get_static_pad_templates (f);
  g_assert_cmpuint (g_list_length ((GList *) templates), ==, 1);
  t = templates->data;
  g_assert_cmpstr (t->name_template, ==, "vsrc_%u");
  g_assert_cmpint (t->direction, ==, GST_PAD_SRC);
  g_assert_cmpint (t->presence, ==, GST_PAD_SOMETIMES);
  caps = gst_static_caps_get (&t->static_caps);
  g_assert_cmpuint (gst_caps_get_size (caps), ==, 2);
  for (guint i = 0; i < 2; i++) {
    g_assert_true (gst_structure_has_name (gst_caps_get_structure (caps, i), "video/x-raw"));
    g_assert_cmpstr (gst_structure_get_string (gst_caps_get_structure (caps, i), "format"), ==, "NV12");
  }
  g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, 0), RDS_CAPS_FEATURE_MEMORY_RDS));
  g_assert_true (gst_caps_features_is_equal (gst_caps_get_features (caps, 1),
          GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY));
  gst_caps_unref (caps);

  /* no pad until the stream's caps are known */
  g_assert_cmpuint (e->numpads, ==, 0);
  g_assert_null (e->pads);
  gst_object_unref (e);

  /* the compat remap makes it for nvurisrcbin */
  g_assert_cmpstr (rds_compat_map_factory_name ("nvurisrcbin"), ==, "rdsurisrcbin");
  e = gst_element_factory_make (rds_compat_map_factory_name ("nvurisrcbin"), NULL);
  g_assert_nonnull (e);
  g_assert_cmpstr (GST_OBJECT_NAME (gst_element_get_factory (e)), ==, "rdsurisrcbin");
  gst_object_unref (e);
}

/* ---- properties ---- */

typedef struct
{
  const gchar *name;
  GType type;                   /* G_TYPE_ENUM: any enum type, checked against enums[] */
  gint64 min;                   /* numbers */
  guint64 max;
  const gchar *def;             /* the default, serialized (gst_value_serialize; enums by nick) */
  gboolean noop;                /* an accepted no-op */
  gboolean ready;               /* GST_PARAM_MUTABLE_READY: changeable only in NULL or READY */
} PropRow;

/* Names, types, ranges, defaults and NULL-or-READY flags as in DeepStream 8.0 behaviour (the element as shipped, where
 * it differs from NVIDIA's nvurisrcbin pages: rtsp-reconnect-interval, rtsp-reconnect-attempts, source-id,
 * select-rtp-protocol, buffer-mode, max-size-buffers, disable-passthrough). Two properties 8.0 lacks come from the 9.2
 * page: simulate-fps-interval-ms and ipc-frame-copy. */
static const PropRow props[] = {
  {"uri", G_TYPE_STRING, 0, 0, "NULL", FALSE, TRUE},
  {"type", G_TYPE_ENUM, 0, 0, "auto", FALSE, TRUE},
  {"file-loop", G_TYPE_BOOLEAN, 0, 0, "false", FALSE, FALSE},
  {"latency", G_TYPE_UINT, 0, G_MAXUINT, "100", FALSE, TRUE},
  {"drop-on-latency", G_TYPE_BOOLEAN, 0, 0, "true", FALSE, FALSE},
  {"select-rtp-protocol", G_TYPE_ENUM, 0, 0, "rtp-udp-udpmcast-tcp", FALSE, TRUE},
  {"udp-buffer-size", G_TYPE_UINT, 0, G_MAXUINT, "524288", FALSE, TRUE},
  {"rtsp-reconnect-interval", G_TYPE_UINT, 0, G_MAXUINT, "0", FALSE, TRUE},
  {"rtsp-reconnect-attempts", G_TYPE_INT, G_MININT, G_MAXINT, "-1", FALSE, TRUE},
  {"init-rtsp-reconnect-interval", G_TYPE_UINT, 0, G_MAXUINT, "0", FALSE, TRUE},
  {"drop-frame-interval", G_TYPE_UINT, 0, 30, "0", FALSE, TRUE},
  {"source-id", G_TYPE_INT, -1, G_MAXINT, "-1", FALSE, TRUE},
  {"gpu-id", G_TYPE_UINT, 0, G_MAXUINT, "0", FALSE, TRUE},
  {"disable-audio", G_TYPE_BOOLEAN, 0, 0, "true", FALSE, FALSE},
  {"smart-record", G_TYPE_ENUM, 0, 0, "smart-rec-disable", FALSE, FALSE},
  {"smart-rec-status", G_TYPE_BOOLEAN, 0, 0, "false", FALSE, FALSE},
  /* accepted no-ops */
  {"num-extra-surfaces", G_TYPE_UINT, 0, G_MAXUINT, "1", TRUE, TRUE},
  {"cudadec-memtype", G_TYPE_ENUM, 0, 0, "memtype_device", TRUE, TRUE},
  {"dec-skip-frames", G_TYPE_ENUM, 0, 0, "decode_all", TRUE, TRUE},
  {"low-latency-mode", G_TYPE_BOOLEAN, 0, 0, "false", TRUE, FALSE},
  {"smart-rec-dir-path", G_TYPE_STRING, 0, 0, "NULL", TRUE, TRUE},
  {"smart-rec-file-prefix", G_TYPE_STRING, 0, 0, "Smart_Record", TRUE, TRUE},
  {"smart-rec-video-cache", G_TYPE_UINT, 0, G_MAXUINT, "0", TRUE, TRUE},
  {"smart-rec-cache", G_TYPE_UINT, 0, G_MAXUINT, "0", TRUE, TRUE},
  {"smart-rec-container", G_TYPE_ENUM, 0, 0, "smart-rec-mp4", TRUE, FALSE},
  {"smart-rec-mode", G_TYPE_ENUM, 0, 0, "smart-rec-mode-av", TRUE, FALSE},
  {"smart-rec-default-duration", G_TYPE_UINT, 0, G_MAXUINT, "20", TRUE, TRUE},
  {"ipc-buffer-timestamp-copy", G_TYPE_BOOLEAN, 0, 0, "false", TRUE, FALSE},
  {"ipc-connection-attempts", G_TYPE_INT, -1, G_MAXINT, "-1", TRUE, FALSE},
  {"ipc-connection-interval", G_TYPE_UINT64, 0, G_MAXUINT64, "1000000", TRUE, FALSE},
  {"ipc-socket-path", G_TYPE_STRING, 0, 0, "NULL", TRUE, TRUE},
  {"ipc-frame-copy", G_TYPE_BOOLEAN, 0, 0, "false", TRUE, FALSE},
  {"extract-sei-type5-data", G_TYPE_BOOLEAN, 0, 0, "false", TRUE, FALSE},
  {"sei-uuid", G_TYPE_STRING, 0, 0, "NULL", TRUE, TRUE},
  {"simulate-fps-interval-ms", G_TYPE_UINT, 0, 1000, "0", TRUE, FALSE},
  {"buffer-mode", G_TYPE_ENUM, 0, 0, "auto", TRUE, TRUE},
  {"leaky", G_TYPE_ENUM, 0, 0, "no-leak", TRUE, TRUE},
  {"max-size-buffers", G_TYPE_UINT, 0, G_MAXUINT, "200", TRUE, TRUE},
  {"disable-passthrough", G_TYPE_BOOLEAN, 0, 0, "true", TRUE, FALSE},
};

typedef struct
{
  const gchar *property;
  const gchar *values;          /* value:nick, ... */
} EnumRow;

static const EnumRow enums[] = {
  {"type", "0:auto,1:uri,2:rtsp"},
  {"select-rtp-protocol",
      "0:rtp-unknown,1:rtp-udp,2:rtp-udp-mcast,4:rtp-tcp,7:rtp-udp-udpmcast-tcp,10:rtp-http,20:rtp-tls"},
  {"smart-record", "0:smart-rec-disable,1:smart-rec-cloud,2:smart-rec-multi"},
  {"smart-rec-container", "0:smart-rec-mp4,1:smart-rec-mkv"},
  {"smart-rec-mode", "0:smart-rec-mode-av,1:smart-rec-mode-video,2:smart-rec-mode-audio"},
  {"cudadec-memtype", "0:memtype_device,1:memtype_pinned,2:memtype_unified"},
  {"dec-skip-frames", "0:decode_all,1:decode_non_ref,2:decode_key"},
  {"buffer-mode", "0:unknown,1:slave,2:buffer,3:auto,4:synced"},
  {"leaky", "0:no-leak,1:upstream-leak,2:downstream-leak"},
};

static void
check_range (GParamSpec * ps, const PropRow * row)
{
  if (row->type == G_TYPE_UINT) {
    g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->minimum, ==, row->min);
    g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->maximum, ==, row->max);
  } else if (row->type == G_TYPE_INT) {
    g_assert_cmpint (G_PARAM_SPEC_INT (ps)->minimum, ==, row->min);
    g_assert_cmpint (G_PARAM_SPEC_INT (ps)->maximum, ==, (gint64) row->max);
  } else if (row->type == G_TYPE_UINT64) {
    g_assert_cmpuint (G_PARAM_SPEC_UINT64 (ps)->minimum, ==, row->min);
    g_assert_cmpuint (G_PARAM_SPEC_UINT64 (ps)->maximum, ==, row->max);
  }
}

static void
check_enum (GParamSpec * ps)
{
  GEnumClass *klass = G_PARAM_SPEC_ENUM (ps)->enum_class;
  const EnumRow *row = NULL;
  gchar **values;

  for (guint i = 0; i < G_N_ELEMENTS (enums); i++) {
    if (strcmp (enums[i].property, ps->name) == 0)
      row = &enums[i];
  }
  g_assert_nonnull (row);
  values = g_strsplit (row->values, ",", -1);
  g_assert_cmpuint (klass->n_values, ==, g_strv_length (values));
  for (guint i = 0; values[i] != NULL; i++) {
    gchar **pair = g_strsplit (values[i], ":", 2);
    GEnumValue *v = g_enum_get_value (klass, atoi (pair[0]));

    g_assert_nonnull (v);
    g_assert_cmpstr (v->value_nick, ==, pair[1]);
    g_strfreev (pair);
  }
  g_strfreev (values);
}

/* gst_value_serialize, with "NULL" for a NULL string */
static gchar *
serialize (const GValue * v)
{
  gchar *s = gst_value_serialize (v);

  return s != NULL ? s : g_strdup ("NULL");
}

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsurisrcbin", NULL);
  GObjectClass *klass = G_OBJECT_GET_CLASS (e);
  GParamSpec *ps, **all;
  guint n_all;
  gboolean b;

  /* the table's, plus GstObject's name and parent and GstBin's async-handling and message-forward: nothing else */
  all = g_object_class_list_properties (klass, &n_all);
  g_assert_cmpuint (n_all, ==, G_N_ELEMENTS (props) + 4);
  g_free (all);

  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    const PropRow *row = &props[i];
    GValue v = G_VALUE_INIT;
    gchar *s;

    g_test_message ("%s", row->name);
    ps = g_object_class_find_property (klass, row->name);
    g_assert_nonnull (ps);
    if (row->type == G_TYPE_ENUM)
      g_assert_true (G_IS_PARAM_SPEC_ENUM (ps));
    else
      g_assert_true (G_PARAM_SPEC_VALUE_TYPE (ps) == row->type);
    g_assert_true (ps->flags & G_PARAM_READABLE);
    if (strcmp (row->name, "smart-rec-status") == 0)
      g_assert_false (ps->flags & G_PARAM_WRITABLE);
    else
      g_assert_true (ps->flags & G_PARAM_WRITABLE);
    g_assert_cmpint (! !(ps->flags & GST_PARAM_MUTABLE_READY), ==, row->ready);
    check_range (ps, row);
    if (row->type == G_TYPE_ENUM)
      check_enum (ps);

    /* the default in the param spec and the value of a new element */
    g_value_init (&v, G_PARAM_SPEC_VALUE_TYPE (ps));
    g_param_value_set_default (ps, &v);
    s = serialize (&v);
    g_assert_cmpstr (s, ==, row->def);
    g_free (s);
    g_object_get_property (G_OBJECT (e), row->name, &v);
    s = serialize (&v);
    g_assert_cmpstr (s, ==, row->def);
    g_free (s);
    g_value_unset (&v);
  }

  /* GstBin's own: nvurisrcbin has async-handling TRUE by default, message-forward FALSE */
  ps = g_object_class_find_property (klass, "async-handling");
  g_assert_true (ps->owner_type == GST_TYPE_BIN);
  g_object_get (e, "async-handling", &b, NULL);
  g_assert_true (b);
  ps = g_object_class_find_property (klass, "message-forward");
  g_assert_true (ps->owner_type == GST_TYPE_BIN);
  g_object_get (e, "message-forward", &b, NULL);
  g_assert_false (b);

  /* values are stored and read back */
  g_object_set (e, "uri", "rtsp://127.0.0.1:8554/cam", "latency", 300u, "rtsp-reconnect-attempts", 4, "source-id",
      7, NULL);
  gst_util_set_object_arg (G_OBJECT (e), "select-rtp-protocol", "rtp-tcp");
  {
    gchar *uri, *proto;
    guint latency;
    gint attempts, id;
    GValue v = G_VALUE_INIT;

    g_value_init (&v, G_PARAM_SPEC_VALUE_TYPE (g_object_class_find_property (klass, "select-rtp-protocol")));
    g_object_get_property (G_OBJECT (e), "select-rtp-protocol", &v);
    proto = serialize (&v);
    g_object_get (e, "uri", &uri, "latency", &latency, "rtsp-reconnect-attempts", &attempts, "source-id", &id, NULL);
    g_assert_cmpstr (uri, ==, "rtsp://127.0.0.1:8554/cam");
    g_assert_cmpuint (latency, ==, 300);
    g_assert_cmpint (attempts, ==, 4);
    g_assert_cmpint (id, ==, 7);
    g_assert_cmpstr (proto, ==, "rtp-tcp");
    g_assert_cmpint (g_value_get_enum (&v), ==, 4);
    g_free (proto);
    g_free (uri);
    g_value_unset (&v);
  }
  gst_object_unref (e);
}

/* ---- no-op warning ---- */

/* A valid value other than the default of @ps. */
static void
non_default_value (GParamSpec * ps, GValue * v)
{
  g_value_init (v, G_PARAM_SPEC_VALUE_TYPE (ps));
  g_param_value_set_default (ps, v);
  if (G_IS_PARAM_SPEC_BOOLEAN (ps)) {
    g_value_set_boolean (v, !g_value_get_boolean (v));
  } else if (G_IS_PARAM_SPEC_UINT (ps)) {
    g_value_set_uint (v, g_value_get_uint (v) + 1);
  } else if (G_IS_PARAM_SPEC_INT (ps)) {
    g_value_set_int (v, g_value_get_int (v) + 1);
  } else if (G_IS_PARAM_SPEC_UINT64 (ps)) {
    g_value_set_uint64 (v, g_value_get_uint64 (v) + 1);
  } else if (G_IS_PARAM_SPEC_STRING (ps)) {
    g_value_set_string (v, "x");
  } else if (G_IS_PARAM_SPEC_ENUM (ps)) {
    GEnumClass *klass = G_PARAM_SPEC_ENUM (ps)->enum_class;

    g_value_set_enum (v, klass->values[klass->n_values - 1].value);
  } else {
    g_assert_not_reached ();
  }
  g_assert_false (g_param_value_defaults (ps, v));
  g_assert_false (g_param_value_validate (ps, v));
}

static void
test_noop_warning (void)
{
  GstElement *e = gst_element_factory_make ("rdsurisrcbin", NULL);
  GObjectClass *klass = G_OBJECT_GET_CLASS (e);

  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    GParamSpec *ps = g_object_class_find_property (klass, props[i].name);
    GValue v = G_VALUE_INIT, back = G_VALUE_INIT;

    if (!props[i].noop)
      continue;
    g_test_message ("%s", props[i].name);
    non_default_value (ps, &v);
    warnings_clear ();
    g_object_set_property (G_OBJECT (e), ps->name, &v);
    g_assert_cmpuint (warnings_count (ps->name), ==, 1);
    /* stored and read back */
    g_value_init (&back, G_PARAM_SPEC_VALUE_TYPE (ps));
    g_object_get_property (G_OBJECT (e), ps->name, &back);
    g_assert_cmpint (g_param_values_cmp (ps, &v, &back), ==, 0);
    g_value_unset (&back);
    /* back to the default: no warning */
    g_param_value_set_default (ps, &v);
    warnings_clear ();
    g_object_set_property (G_OBJECT (e), ps->name, &v);
    g_assert_cmpuint (warnings_count (NULL), ==, 0);
    g_value_unset (&v);
  }

  /* the properties that are not no-ops: no warning */
  warnings_clear ();
  g_object_set (e, "latency", 300u, "file-loop", TRUE, "gpu-id", 0u, "smart-record", 1, NULL);
  g_assert_cmpuint (warnings_count (NULL), ==, 0);
  gst_object_unref (e);
}

/* ---- start errors ---- */

/* Sets "property", "value" pairs (NULL-terminated) as gst-launch would. */
static GstElement *
make_with (const gchar * first, va_list args)
{
  GstElement *e = gst_element_factory_make ("rdsurisrcbin", NULL);
  const gchar *prop = first;

  while (prop != NULL) {
    gst_util_set_object_arg (G_OBJECT (e), prop, va_arg (args, const gchar *));
    prop = va_arg (args, const gchar *);
  }
  return e;
}

/* NULL to READY fails, with one ERROR message from the element that names @property. */
static void
expect_start_error (const gchar * property, const gchar * first, ...)
{
  GstElement *p = gst_pipeline_new (NULL), *e;
  GstBus *bus = gst_element_get_bus (p);
  GstMessage *m;
  GError *err = NULL;
  gchar *dbg = NULL;
  va_list args;

  va_start (args, first);
  e = make_with (first, args);
  va_end (args);
  gst_bin_add (GST_BIN (p), e);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_FAILURE);
  m = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
  g_assert_nonnull (m);
  g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (e));
  gst_message_parse_error (m, &err, &dbg);
  g_test_message ("%s: %s (%s)", property, err->message, dbg);
  g_assert_nonnull (strstr (err->message, property));
  g_clear_error (&err);
  g_free (dbg);
  gst_message_unref (m);
  g_assert_null (gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR));
  gst_element_set_state (p, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (p);
}

/* NULL to READY succeeds without an ERROR message. */
static void
expect_start (const gchar * first, ...)
{
  GstElement *p = gst_pipeline_new (NULL), *e;
  GstBus *bus = gst_element_get_bus (p);
  va_list args;

  va_start (args, first);
  e = make_with (first, args);
  va_end (args);
  gst_bin_add (GST_BIN (p), e);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_null (gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR));
  /* nothing is opened before PAUSED, so there is no pad yet */
  g_assert_cmpuint (e->numpads, ==, 0);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  gst_object_unref (bus);
  gst_object_unref (p);
}

static void
test_start_errors (void)
{
  GstElementFactory *rtspsrc;

  expect_start_error ("uri", NULL);
  expect_start_error ("uri", "uri", "", NULL);
  expect_start_error ("uri", "uri", "nonsense", NULL);
  /* a valid uri that no source element handles */
  expect_start_error ("uri", "uri", "rdsnosuchscheme://host/x", NULL);
  expect_start_error ("type", "uri", "file:///x.h264", "type", "2", NULL);

  /* the settings that start (to READY: the uri is not opened yet). (GST_ELEMENT_ERROR also logs a warning in the
   * rdsurisrcbin category, which the collector counts, so the counts start here, after the errors.) */
  warnings_clear ();
  expect_start ("uri", "file:///x.h264", NULL);
  expect_start ("uri", "file:///x.h264", "type", "uri", "file-loop", "true", NULL);
  g_assert_cmpuint (warnings_count (NULL), ==, 0);

  /* rtsp:// uris need a source element for the scheme */
  rtspsrc = gst_element_factory_find ("rtspsrc");
  if (rtspsrc == NULL) {
    g_test_skip ("no rtspsrc: the rtsp:// cases are skipped");
    return;
  }
  gst_object_unref (rtspsrc);
  expect_start_error ("smart-record", "uri", "rtsp://127.0.0.1:8554/cam", "smart-record", "1", NULL);
  expect_start_error ("smart-record", "uri", "rtsp://127.0.0.1:8554/cam", "smart-record", "smart-rec-multi", NULL);
  warnings_clear ();
  expect_start ("uri", "rtsp://127.0.0.1:8554/cam", "type", "rtsp", NULL);
  g_assert_cmpuint (warnings_count (NULL), ==, 0);
  /* file-loop applies to file:// uris: elsewhere a warning, and the element starts */
  expect_start ("uri", "rtsp://127.0.0.1:8554/cam", "file-loop", "true", NULL);
  g_assert_cmpuint (warnings_count ("file-loop"), ==, 1);
}

/* ---- decoding (gpu) ---- */

#define CLIP_W 320
#define CLIP_H 240
#define CLIP_FRAMES 30
#define CLIP_FRAME_NS (GST_SECOND / 30)

/* The test clips, made once per process in a temporary directory that main () deletes. */
static gchar *media_dir;

static gboolean
require_elements (const gchar * const *names)
{
  for (; *names != NULL; names++) {
    if (!rds_test_have_element (*names)) {
      gchar *msg = g_strdup_printf ("%s not available", *names);

      g_test_skip (msg);
      g_free (msg);
      return FALSE;
    }
  }
  return TRUE;
}

/* Runs @desc (a pipeline ending in filesink location=@path) to EOS. */
static void
write_clip (const gchar * desc)
{
  GError *err = NULL;
  GstElement *pipe = gst_parse_launch (desc, &err);
  GstBus *bus;
  GstMessage *msg;

  g_assert_no_error (err);
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  bus = gst_element_get_bus (pipe);
  msg = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  gst_object_unref (bus);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
}

/* The file URI of the clip @name in media_dir, made on first use: "mp4" (H.264 with B-frames in MP4), "h264" (a raw
 * H.264 byte stream), "mjpeg" (JPEG frames in AVI, which rdsvideodec can't decode) or "audio" (H.264 and Opus in
 * Matroska). CLIP_FRAMES frames of CLIP_W x CLIP_H at 30 fps. NULL after g_test_skip when an element is missing. */
static gchar *
clip_uri (const gchar * name)
{
  const gchar *need_mp4[] = { "vah264enc", "h264parse", "mp4mux", "qtdemux", NULL };
  const gchar *need_h264[] = { "vah264enc", "h264parse", NULL };
  const gchar *need_mjpeg[] = { "jpegenc", "jpegdec", "avimux", "avidemux", NULL };
  const gchar *need_audio[] = { "vah264enc", "h264parse", "opusenc", "matroskamux", "matroskademux", NULL };
  GError *err = NULL;
  gchar *path, *uri, *desc = NULL;

  if (!require_elements (g_str_equal (name, "mp4") ? need_mp4 : g_str_equal (name, "h264") ? need_h264 :
          g_str_equal (name, "mjpeg") ? need_mjpeg : need_audio) || !require_elements ((const gchar *[]) {
              "uridecodebin3", NULL}))
    return NULL;
  if (media_dir == NULL) {
    media_dir = g_dir_make_tmp ("rds-urisrcbin-XXXXXX", &err);
    g_assert_no_error (err);
  }
  path = g_build_filename (media_dir, name, NULL);
  if (!g_file_test (path, G_FILE_TEST_EXISTS)) {
    if (g_str_equal (name, "mp4")) {
      rds_test_write_mp4 ("h264", CLIP_W, CLIP_H, CLIP_FRAMES, 2, path);
    } else if (g_str_equal (name, "h264")) {
      GBytes *stream = rds_test_encode ("h264", CLIP_W, CLIP_H, CLIP_FRAMES, 0, "ball");

      g_assert_true (g_file_set_contents (path, g_bytes_get_data (stream, NULL), g_bytes_get_size (stream), &err));
      g_assert_no_error (err);
      g_bytes_unref (stream);
    } else if (g_str_equal (name, "mjpeg")) {
      desc = g_strdup_printf ("videotestsrc pattern=ball num-buffers=%u ! video/x-raw,format=I420,width=%u,"
          "height=%u,framerate=30/1 ! jpegenc ! avimux ! filesink location=\"%s\"", CLIP_FRAMES, CLIP_W, CLIP_H,
          path);
    } else {
      desc = g_strdup_printf ("matroskamux name=mux ! filesink location=\"%s\" "
          "videotestsrc pattern=ball num-buffers=%u ! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 "
          "! vah264enc ! h264parse ! mux. "
          "audiotestsrc num-buffers=50 samplesperbuffer=960 ! audio/x-raw,rate=48000,channels=2 ! opusenc ! mux.",
          path, CLIP_FRAMES, CLIP_W, CLIP_H);
    }
    if (desc != NULL)
      write_clip (desc);
  }
  uri = g_filename_to_uri (path, NULL, &err);
  g_assert_no_error (err);
  g_free (desc);
  g_free (path);
  return uri;
}

typedef struct
{
  GstClockTime pts, dts, duration;
  gboolean discont, delta;
  gint width, height;           /* memory:RDS: the surface's size, else 0 */
  gint caps_width, caps_height; /* the pad's caps when it was pushed */
} BufInfo;

/* What leaves the bin's pads, seen by a pad probe on each new pad (which pad-added links to the appsink). */
typedef struct
{
  GMutex lock;
  GstElement *sink;
  gboolean need_caps;           /* the pad has its caps when it is added (RTSP) */
  guint n_pads;
  gchar *pad_name;
  GstPad *pad;                  /* the first pad (a ref) */
  gboolean other_pad;           /* a buffer or event on another pad */
  guint n_stream_start, n_caps, n_segment, n_eos, n_flush;
  GstSegment segment;           /* the first one */
  GstSegment last_segment;
  gint caps_width, caps_height; /* the last CAPS */
  guint flush_at;               /* buffers before the last FLUSH_STOP */
  gboolean flush_reset_time;    /* a FLUSH_STOP had reset-time TRUE */
  gboolean segment_after_flush; /* a SEGMENT came after the last FLUSH_STOP and before the next buffer */
  GArray *buffers;              /* BufInfo */
} Watch;

static GstPadProbeReturn
watch_probe (GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Watch *w = user_data;

  g_mutex_lock (&w->lock);
  if (pad != w->pad) {
    w->other_pad = TRUE;
  } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);
    BufInfo b = { GST_BUFFER_PTS (buf), GST_BUFFER_DTS (buf), GST_BUFFER_DURATION (buf),
      GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DISCONT), GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT),
      0, 0, w->caps_width, w->caps_height
    };

    if (gst_buffer_n_memory (buf) == 1 && rds_is_rds_memory (gst_buffer_peek_memory (buf, 0))) {
      RdsBufSurface *surf = rds_buffer_get_surface (buf);

      b.width = surf->surfaceList[0].width;
      b.height = surf->surfaceList[0].height;
    }
    g_array_append_val (w->buffers, b);
  } else {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);

    switch (GST_EVENT_TYPE (ev)) {
      case GST_EVENT_STREAM_START:
        w->n_stream_start++;
        break;
      case GST_EVENT_CAPS:{
        GstCaps *caps;
        GstStructure *st;

        gst_event_parse_caps (ev, &caps);
        st = gst_caps_get_structure (caps, 0);
        gst_structure_get_int (st, "width", &w->caps_width);
        gst_structure_get_int (st, "height", &w->caps_height);
        w->n_caps++;
        break;
      }
      case GST_EVENT_SEGMENT:
        if (w->n_segment++ == 0)
          gst_event_copy_segment (ev, &w->segment);
        gst_event_copy_segment (ev, &w->last_segment);
        if (w->n_flush > 0 && w->flush_at == w->buffers->len)
          w->segment_after_flush = TRUE;
        break;
      case GST_EVENT_EOS:
        w->n_eos++;
        break;
      case GST_EVENT_FLUSH_START:
        w->n_flush++;
        break;
      case GST_EVENT_FLUSH_STOP:{
        gboolean reset;

        gst_event_parse_flush_stop (ev, &reset);
        w->n_flush++;
        w->flush_at = w->buffers->len;
        w->flush_reset_time |= reset;
        w->segment_after_flush = FALSE;
        break;
      }
      default:
        break;
    }
  }
  g_mutex_unlock (&w->lock);
  return GST_PAD_PROBE_OK;
}

static void
on_pad_added (GstElement * bin, GstPad * pad, Watch * w)
{
  GstPad *sinkpad = gst_element_get_static_pad (w->sink, "sink");

  g_mutex_lock (&w->lock);
  if (w->n_pads++ == 0) {
    w->pad_name = gst_pad_get_name (pad);
    w->pad = gst_object_ref (pad);
  }
  g_mutex_unlock (&w->lock);
  g_assert_true (GST_OBJECT_PARENT (pad) == GST_OBJECT (bin));
  if (w->need_caps)
    g_assert_true (gst_pad_has_current_caps (pad));
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM
      | GST_PAD_PROBE_TYPE_EVENT_FLUSH, watch_probe, w, NULL);
  if (!gst_pad_is_linked (sinkpad))
    g_assert_cmpint (gst_pad_link (pad, sinkpad), ==, GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
}

/* A pipeline: rdsurisrcbin uri=@uri (plus "property", "value" pairs, NULL-terminated) ! appsink, linked in
 * pad-added. Free with run_free. */
/* The bus messages a test looks at, as they are posted: element messages, WARNINGs and ERRORs (they also stay on the
 * bus). With block_name, the thread posting an element message of that name is held for block_us after it is
 * logged, as an application's sync handler might hold it. */
typedef struct
{
  GMutex lock;
  GCond cond;
  GPtrArray *msgs;              /* GstMessage */
  GArray *at;                   /* gint64: when each was posted (monotonic us) */
  const gchar *block_name;
  gulong block_us;
} BusLog;

static GstBusSyncReply
bus_log_sync (G_GNUC_UNUSED GstBus * bus, GstMessage * msg, gpointer user_data)
{
  BusLog *l = user_data;
  gint64 now = g_get_monotonic_time ();
  gulong block = 0;

  if (GST_MESSAGE_TYPE (msg) & (GST_MESSAGE_ELEMENT | GST_MESSAGE_WARNING | GST_MESSAGE_ERROR)) {
    g_mutex_lock (&l->lock);
    g_ptr_array_add (l->msgs, gst_message_ref (msg));
    g_array_append_val (l->at, now);
    if (l->block_name != NULL && GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ELEMENT
        && gst_message_has_name (msg, l->block_name))
      block = l->block_us;
    g_cond_broadcast (&l->cond);
    g_mutex_unlock (&l->lock);
  }
  if (block > 0)
    g_usleep (block);
  return GST_BUS_PASS;
}

typedef struct
{
  GstElement *pipe, *src, *sink;
  Watch w;
  BusLog log;
} Run;

/* run_new with the property pairs in @args. With @watch_only the appsink drops its oldest sample instead of blocking
 * (and posts EOS without waiting for its samples to be pulled), so a test that only watches the pad and the bus never
 * stalls the stream, and RTSP goes over TCP (through the relay, with its URL). */
static Run *
run_new_valist (const gchar * uri, gboolean watch_only, const gchar * first, va_list args)
{
  Run *r = g_new0 (Run, 1);
  const gchar *prop = first;
  GstBus *bus;

  r->pipe = gst_pipeline_new (NULL);
  r->src = gst_element_factory_make ("rdsurisrcbin", NULL);
  r->sink = gst_element_factory_make ("appsink", NULL);
  g_assert_nonnull (r->src);
  /* a short queue: a looping source blocks in the appsink instead of filling memory */
  g_object_set (r->src, "uri", uri, NULL);
  g_object_set (r->sink, "sync", FALSE, "max-buffers", 4u, "drop", watch_only, "wait-on-eos", !watch_only, NULL);
  if (watch_only)
    g_object_set (r->src, "select-rtp-protocol", 4, NULL);
  while (prop != NULL) {
    gst_util_set_object_arg (G_OBJECT (r->src), prop, va_arg (args, const gchar *));
    prop = va_arg (args, const gchar *);
  }
  gst_bin_add_many (GST_BIN (r->pipe), r->src, r->sink, NULL);
  g_mutex_init (&r->log.lock);
  g_cond_init (&r->log.cond);
  r->log.msgs = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_message_unref);
  r->log.at = g_array_new (FALSE, FALSE, sizeof (gint64));
  bus = gst_element_get_bus (r->pipe);
  gst_bus_set_sync_handler (bus, bus_log_sync, &r->log, NULL);
  gst_object_unref (bus);
  g_mutex_init (&r->w.lock);
  r->w.sink = r->sink;
  r->w.need_caps = g_str_has_prefix (uri, "rtsp");
  r->w.buffers = g_array_new (FALSE, FALSE, sizeof (BufInfo));
  g_signal_connect (r->src, "pad-added", G_CALLBACK (on_pad_added), &r->w);
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  return r;
}

static Run *
run_new (const gchar * uri, const gchar * first, ...)
{
  va_list args;
  Run *r;

  va_start (args, first);
  r = run_new_valist (uri, FALSE, first, args);
  va_end (args);
  return r;
}

/* Pulls @n samples (or up to EOS with @n 0), each memory:RDS NV12 at @width x @height; returns how many. */
static guint
run_pull_size (Run * r, guint n, gint width, gint height)
{
  gchar *str = g_strdup_printf ("video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "),format=NV12,width=%d,height=%d", width,
      height);
  GstCaps *want = gst_caps_from_string (str);
  GstSample *s;
  guint got = 0;

  while ((n == 0 || got < n) && (s = rds_test_pull (r->pipe, r->sink)) != NULL) {
    GstBuffer *buf = gst_sample_get_buffer (s);

    if (!gst_caps_is_subset (gst_sample_get_caps (s), want)) {
      gchar *caps = gst_caps_to_string (gst_sample_get_caps (s));

      g_error ("unexpected caps %s", caps);
    }
    g_assert_cmpuint (gst_buffer_n_memory (buf), ==, 1);
    g_assert_true (rds_is_rds_memory (gst_buffer_peek_memory (buf, 0)));
    gst_sample_unref (s);
    got++;
  }
  gst_caps_unref (want);
  g_free (str);
  return got;
}

/* run_pull_size at the clip's size. */
static guint
run_pull (Run * r, guint n)
{
  return run_pull_size (r, n, CLIP_W, CLIP_H);
}

/* Back to READY: the pad goes (it must have been there until now). */
static void
run_stop (Run * r)
{
  g_assert_cmpuint (r->src->numpads, ==, 1);
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpuint (r->src->numpads, ==, 0);
}

/* Forgets what the pads carried so far, for a new run. */
static void
watch_reset (Watch * w)
{
  g_mutex_lock (&w->lock);
  w->n_pads = 0;
  g_clear_pointer (&w->pad_name, g_free);
  g_clear_object (&w->pad);
  w->other_pad = FALSE;
  w->n_stream_start = w->n_caps = w->n_segment = w->n_eos = w->n_flush = w->flush_at = 0;
  w->flush_reset_time = w->segment_after_flush = FALSE;
  g_array_set_size (w->buffers, 0);
  g_mutex_unlock (&w->lock);
}

/* NULL and free, whatever the pads. */
static void
run_destroy (Run * r)
{
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  gst_object_unref (r->pipe);
  g_ptr_array_unref (r->log.msgs);
  g_array_unref (r->log.at);
  g_mutex_clear (&r->log.lock);
  g_cond_clear (&r->log.cond);
  g_clear_object (&r->w.pad);
  g_free (r->w.pad_name);
  g_array_unref (r->w.buffers);
  g_mutex_clear (&r->w.lock);
  g_free (r);
}

/* run_stop, then NULL and free. */
static void
run_free (Run * r)
{
  run_stop (r);
  run_destroy (r);
}

/* Waits for the pipeline's EOS message (the appsink reports EOS before it is posted); fails on an error. */
static void
run_wait_eos (Run * r)
{
  GstBus *bus = gst_element_get_bus (r->pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  g_assert_nonnull (msg);
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;

    gst_message_parse_error (msg, &err, NULL);
    g_error ("pipeline error from %s: %s", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message);
  }
  gst_message_unref (msg);
  gst_object_unref (bus);
}

static BufInfo *
buf_at (Run * r, guint i)
{
  return &g_array_index (r->w.buffers, BufInfo, i);
}

/* The first element made by @factory inside @bin, at any depth (a ref), or NULL. */
static GstElement *
bin_find_factory (GstBin * bin, const gchar * factory)
{
  GstIterator *it = gst_bin_iterate_recurse (bin);
  GValue item = G_VALUE_INIT;
  GstElement *found = NULL;

  while (found == NULL && gst_iterator_next (it, &item) == GST_ITERATOR_OK) {
    GstElementFactory *f = gst_element_get_factory (g_value_get_object (&item));

    if (f != NULL && g_str_equal (GST_OBJECT_NAME (f), factory))
      found = g_value_dup_object (&item);
    g_value_reset (&item);
  }
  g_value_unset (&item);
  gst_iterator_free (it);
  return found;
}

/* Whether an element made by @factory is inside @bin, at any depth. */
static gboolean
bin_has_factory (GstBin * bin, const gchar * factory)
{
  GstElement *e = bin_find_factory (bin, factory);
  gboolean found = e != NULL;

  gst_clear_object (&e);
  return found;
}

/* One pad, vsrc_0, carrying one STREAM_START, one SEGMENT and no flush; @eos EOS events. */
static void
check_events (Run * r, guint eos)
{
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_pads, ==, 1);
  g_assert_cmpstr (r->w.pad_name, ==, "vsrc_0");
  g_assert_false (r->w.other_pad);
  g_assert_cmpuint (r->w.n_stream_start, ==, 1);
  g_assert_cmpuint (r->w.n_segment, ==, 1);
  g_assert_cmpuint (r->w.n_flush, ==, 0);
  g_assert_cmpuint (r->w.n_eos, ==, eos);
  g_assert_cmpuint (r->w.n_caps, >=, 1);
  g_mutex_unlock (&r->w.lock);
}

/* A file decodes to memory:RDS NV12 on vsrc_0, every frame then EOS, with increasing PTS (@name: "mp4", "h264" (a raw
 * byte stream) or "audio" (the audio stream is ignored)). */
static void
test_file_decode (gconstpointer data)
{
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri (data)) == NULL)
    return;
  r = run_new (uri, NULL);
  g_assert_cmpuint (run_pull (r, 0), ==, CLIP_FRAMES);
  run_wait_eos (r);
  check_events (r, 1);
  g_assert_cmpuint (r->w.buffers->len, ==, CLIP_FRAMES);
  /* only the video stream is selected: the audio is never decoded */
  g_assert_false (bin_has_factory (GST_BIN (r->src), "opusdec"));
  g_assert_false (bin_has_factory (GST_BIN (r->src), "fakesink"));
  for (guint i = 1; i < r->w.buffers->len; i++) {
    if (GST_CLOCK_TIME_IS_VALID (buf_at (r, i)->pts))
      g_assert_cmpuint (buf_at (r, i)->pts, >, buf_at (r, i - 1)->pts);
  }
  run_free (r);
  g_free (uri);
}

/* The JPEG frames of an AVI decode to system memory (no rdsvideodec for them): rdsvideoconvert comes in between, and
 * vsrc_0 still carries memory:RDS NV12. */
static void
test_system_memory (void)
{
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri ("mjpeg")) == NULL)
    return;
  r = run_new (uri, NULL);
  g_assert_cmpuint (run_pull (r, 0), ==, CLIP_FRAMES);
  run_wait_eos (r);
  check_events (r, 1);
  g_assert_true (bin_has_factory (GST_BIN (r->src), "rdsvideoconvert"));
  run_free (r);
  g_free (uri);
}

/* file-loop (DeepStream 8.0 behaviour): the file plays again and again on the same vsrc_0, with no EOS, flush,
 * second SEGMENT or STREAM_START at the loop points; the one SEGMENT is open-ended; PTS go on from the end of the
 * previous loop, and each loop's first buffer is DISCONT (@data: the clip; "mjpeg" loops through rdsvideoconvert). */
static void
test_file_loop (gconstpointer data)
{
  const guint loops = 4;
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri (data)) == NULL)
    return;
  r = run_new (uri, "file-loop", "true", NULL);
  /* some frames into the fifth pass */
  g_assert_cmpuint (run_pull (r, loops * CLIP_FRAMES + 5), ==, loops * CLIP_FRAMES + 5);
  (void) rds_test_bus_check (r->pipe);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.buffers->len, >=, loops * CLIP_FRAMES + 5);
  g_assert_false (GST_CLOCK_TIME_IS_VALID (r->w.segment.stop));
  for (guint i = 1; i < r->w.buffers->len; i++) {
    BufInfo *b = buf_at (r, i), *prev = buf_at (r, i - 1);

    g_assert_true (GST_CLOCK_TIME_IS_VALID (b->pts));
    g_assert_cmpuint (b->pts, >, prev->pts);
    if (GST_CLOCK_TIME_IS_VALID (b->dts) && GST_CLOCK_TIME_IS_VALID (prev->dts))
      g_assert_cmpuint (b->dts, >=, prev->dts);
    if (i % CLIP_FRAMES == 0) {
      /* a loop point: DISCONT, and the PTS right where the previous frame ends */
      g_test_message ("loop at %u: %" GST_TIME_FORMAT " after %" GST_TIME_FORMAT, i, GST_TIME_ARGS (b->pts),
          GST_TIME_ARGS (prev->pts));
      g_assert_true (b->discont);
      g_assert_cmpuint (b->pts, ==, prev->pts + prev->duration);
    } else {
      g_assert_false (b->discont);
      /* one frame apart, give or take the rounding of 1/30 s */
      g_assert_cmpuint (b->pts - prev->pts, <=, CLIP_FRAME_NS + 1);
      g_assert_cmpuint (b->pts - prev->pts, >=, CLIP_FRAME_NS - 1);
    }
  }
  g_mutex_unlock (&r->w.lock);
  check_events (r, 0);
  run_free (r);
  g_free (uri);
}

/* drop-frame-interval=N (DeepStream 8.0 behaviour; @data: N): frames 0, N, 2N, ... of a run without it, and nothing
 * else, with their timestamps and durations (1: every frame). */
static void
test_drop_frame_interval (gconstpointer data)
{
  const guint interval = GPOINTER_TO_UINT (data);
  gchar value[16];
  GArray *all;
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri ("mp4")) == NULL)
    return;
  r = run_new (uri, NULL);
  g_assert_cmpuint (run_pull (r, 0), ==, CLIP_FRAMES);
  all = g_array_ref (r->w.buffers);
  run_free (r);

  g_snprintf (value, sizeof value, "%u", interval);
  r = run_new (uri, "drop-frame-interval", value, NULL);
  g_assert_cmpuint (run_pull (r, 0), ==, CLIP_FRAMES / interval);
  check_events (r, 1);
  for (guint i = 0; i < r->w.buffers->len; i++) {
    BufInfo *b = buf_at (r, i), *want = &g_array_index (all, BufInfo, i * interval);

    g_assert_cmpuint (b->pts, ==, want->pts);
    g_assert_cmpuint (b->duration, ==, want->duration);
  }
  run_free (r);
  g_array_unref (all);
  g_free (uri);
}

/* drop-frame-interval with file-loop: the count goes on across the loop point. */
static void
test_drop_frame_interval_loop (void)
{
  const guint interval = 4;
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri ("mp4")) == NULL)
    return;
  r = run_new (uri, "drop-frame-interval", "4", "file-loop", "true", NULL);
  /* three passes' worth of kept frames */
  g_assert_cmpuint (run_pull (r, 3 * CLIP_FRAMES / interval), ==, 3 * CLIP_FRAMES / interval);
  g_mutex_lock (&r->w.lock);
  for (guint i = 1; i < r->w.buffers->len; i++) {
    /* each frame's 1/30 s rounds by up to 1 ns */
    g_assert_cmpuint (buf_at (r, i)->pts - buf_at (r, i - 1)->pts, >=, interval * (CLIP_FRAME_NS - 1));
    g_assert_cmpuint (buf_at (r, i)->pts - buf_at (r, i - 1)->pts, <=, interval * (CLIP_FRAME_NS + 1));
    /* DISCONT on the first kept frame of each pass */
    g_assert_cmpint (buf_at (r, i)->discont, ==, i * interval / CLIP_FRAMES != (i - 1) * interval / CLIP_FRAMES);
  }
  g_mutex_unlock (&r->w.lock);
  check_events (r, 0);
  run_free (r);
  g_free (uri);
}

/* A flushing seek from the application while the file loops: it completes, its flushes and SEGMENT reach downstream
 * (stop cleared), and the frames start again from the file's start and loop on with increasing PTS. */
static void
test_file_loop_seek (void)
{
  GstBus *bus;
  GstMessage *msg;
  gchar *uri;
  Run *r;
  guint at;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri ("mp4")) == NULL)
    return;
  r = run_new (uri, "file-loop", "true", NULL);
  g_assert_cmpuint (run_pull (r, CLIP_FRAMES + 10), ==, CLIP_FRAMES + 10);
  bus = gst_element_get_bus (r->pipe);
  gst_bus_set_flushing (bus, TRUE);
  gst_bus_set_flushing (bus, FALSE);
  g_assert_true (gst_element_seek_simple (r->pipe, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0));
  msg = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_ASYNC_DONE);
  gst_message_unref (msg);
  gst_object_unref (bus);
  /* more than a pass after the seek */
  g_assert_cmpuint (run_pull (r, 2 * CLIP_FRAMES), ==, 2 * CLIP_FRAMES);

  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_flush, ==, 2);
  g_assert_cmpuint (r->w.n_segment, ==, 2);
  g_assert_cmpuint (r->w.n_stream_start, ==, 1);
  g_assert_cmpuint (r->w.n_eos, ==, 0);
  g_assert_false (GST_CLOCK_TIME_IS_VALID (r->w.last_segment.stop));
  at = r->w.flush_at;
  g_assert_cmpuint (at, >, 0);
  g_assert_cmpuint (r->w.buffers->len, >=, at + CLIP_FRAMES + 5);
  /* the file's first frame again (one frame in: the B-frames' composition offset), then on and over a loop point */
  g_assert_cmpuint (buf_at (r, at)->pts, <=, CLIP_FRAME_NS + 1);
  for (guint i = 1; i < r->w.buffers->len; i++) {
    if (i != at)
      g_assert_cmpuint (buf_at (r, i)->pts, >, buf_at (r, i - 1)->pts);
  }
  g_mutex_unlock (&r->w.lock);
  run_free (r);
  g_free (uri);
}

/* An EOS the application sends to the pipeline (the usual way to stop) gets out as EOS even with file-loop (ours:
 * DeepStream 8.0's behaviour here was not observed). */
static void
test_file_loop_eos (void)
{
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri ("mp4")) == NULL)
    return;
  r = run_new (uri, "file-loop", "true", NULL);
  g_assert_cmpuint (run_pull (r, CLIP_FRAMES + 10), ==, CLIP_FRAMES + 10);
  g_assert_true (gst_element_send_event (r->pipe, gst_event_new_eos ()));
  /* what was queued, then EOS */
  (void) run_pull (r, 0);
  run_wait_eos (r);
  check_events (r, 1);
  run_free (r);
  g_free (uri);
}

/* Waits for ASYNC_DONE (after a flushing seek); fails on an error. */
static void
run_wait_async_done (Run * r)
{
  GstBus *bus = gst_element_get_bus (r->pipe);
  GstMessage *msg;

  msg = gst_bus_timed_pop_filtered (bus, RDS_TEST_MEDIA_TIMEOUT, GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_ASYNC_DONE);
  gst_message_unref (msg);
  gst_object_unref (bus);
}

/* After the application's EOS, a flushing seek from the application turns file-loop back on: the file plays from the
 * start and loops again (ours). */
static void
test_file_loop_eos_seek (void)
{
  GstBus *bus;
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri ("mp4")) == NULL)
    return;
  r = run_new (uri, "file-loop", "true", NULL);
  g_assert_cmpuint (run_pull (r, 10), ==, 10);
  g_assert_true (gst_element_send_event (r->pipe, gst_event_new_eos ()));
  (void) run_pull (r, 0);
  run_wait_eos (r);

  bus = gst_element_get_bus (r->pipe);
  gst_bus_set_flushing (bus, TRUE);
  gst_bus_set_flushing (bus, FALSE);
  gst_object_unref (bus);
  g_assert_true (gst_element_seek_simple (r->pipe, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH, 0));
  run_wait_async_done (r);
  /* over a loop point: the EOS at the end of the file is a loop again */
  g_assert_cmpuint (run_pull (r, 2 * CLIP_FRAMES), ==, 2 * CLIP_FRAMES);
  (void) rds_test_bus_check (r->pipe);
  run_free (r);
  g_free (uri);
}

/* PLAYING, READY, PLAYING again on the same bin: each run has one vsrc_0, one STREAM_START and one SEGMENT, and
 * frames. */
static void
test_restart (gconstpointer data)
{
  gchar *uri;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((uri = clip_uri (data)) == NULL)
    return;
  r = run_new (uri, "file-loop", "true", NULL);
  g_assert_cmpuint (run_pull (r, CLIP_FRAMES + 10), ==, CLIP_FRAMES + 10);
  check_events (r, 0);
  run_stop (r);
  watch_reset (&r->w);

  g_object_set (r->src, "file-loop", FALSE, NULL);
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpuint (run_pull (r, 0), ==, CLIP_FRAMES);
  run_wait_eos (r);
  check_events (r, 1);
  g_assert_cmpuint (r->w.buffers->len, ==, CLIP_FRAMES);
  run_free (r);
  g_free (uri);
}

/* ---- RTSP ---- */

/* The test server's picture size */
#define RTSP_W 640
#define RTSP_H 480

/* How long an RTSP failure may take to reach the application */
#define RTSP_END_TIMEOUT (10 * GST_SECOND)

/* A URI nothing listens on: rtspsrc fails to connect, after the state change. */
#define RTSP_NOWHERE "rtsp://127.0.0.1:1/test"

/* A test server of @codec (NULL: no video) at RTSP_W x RTSP_H, with the relay and, with @audio, a test tone in Opus;
 * or NULL and the test skipped (no rtspsrc, encoder or gst-rtsp-server). One at a time: free it before the next. */
static RdsTestRtspServer *
rtsp_server_full_or_skip (const gchar * codec, gboolean audio)
{
  RdsTestRtspServerOptions opts = {
    .relay = TRUE,
    .width = RTSP_W,
    .height = RTSP_H,
    .codec = codec,
    .audio = audio,
  };
  RdsTestRtspServer *s;
  gchar *why = NULL;

  if (!require_elements ((const gchar *[]) {
          "rtspsrc", NULL}))
    return NULL;
  s = rds_test_rtsp_server_new_full (&opts, &why);
  if (s == NULL) {
    g_test_skip (why);
    g_free (why);
  }
  return s;
}

/* rtsp_server_full_or_skip with video in @codec and no audio. */
static RdsTestRtspServer *
rtsp_server_or_skip (const gchar * codec)
{
  return rtsp_server_full_or_skip (codec, FALSE);
}

/* The first ERROR on the bus within RTSP_END_TIMEOUT (EOS fails the test). */
static GstMessage *
run_wait_error (Run * r)
{
  GstBus *bus = gst_element_get_bus (r->pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, RTSP_END_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_ERROR);
  gst_object_unref (bus);
  return msg;
}

/* NULL to PAUSED and PLAYING to PAUSED return NO_PREROLL, as for any live source (the server is never reached). */
static void
test_rtsp_live (void)
{
  GstElement *p, *e;

  if (!require_elements ((const gchar *[]) {
          "rtspsrc", NULL}))
    return;
  p = gst_pipeline_new (NULL);
  e = gst_element_factory_make ("rdsurisrcbin", NULL);
  g_object_set (e, "uri", RTSP_NOWHERE, NULL);
  gst_bin_add (GST_BIN (p), e);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_NO_PREROLL);
  g_assert_cmpint (gst_element_set_state (e, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_NO_PREROLL);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_NO_PREROLL);
  /* no pad: nothing was decoded */
  g_assert_cmpuint (e->numpads, ==, 0);
  g_assert_cmpint (gst_element_set_state (p, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  gst_object_unref (p);
}

/* select-rtp-protocol becomes rtspsrc's protocols (GstRTSPLowerTrans: UDP 1, UDP_MCAST 2, TCP 4, HTTP 16, TLS 32), set
 * going from READY to PAUSED. */
static void
test_rtsp_protocols (void)
{
  static const struct
  {
    const gchar *value;
    guint protocols;
  } rows[] = {
    {"1", 1}, {"2", 2}, {"4", 4}, {"7", 1 | 2 | 4}, {"10", 16 | 4}, {"20", 32 | 4},
    /* 0: rtspsrc's default */
    {"0", 1 | 2 | 4},
  };

  if (!require_elements ((const gchar *[]) {
          "rtspsrc", NULL}))
    return;
  for (guint i = 0; i < G_N_ELEMENTS (rows); i++) {
    GstElement *p = gst_pipeline_new (NULL), *e = gst_element_factory_make ("rdsurisrcbin", NULL), *rtspsrc;
    guint protocols;

    g_test_message ("select-rtp-protocol=%s", rows[i].value);
    g_object_set (e, "uri", RTSP_NOWHERE, NULL);
    gst_util_set_object_arg (G_OBJECT (e), "select-rtp-protocol", rows[i].value);
    gst_bin_add (GST_BIN (p), e);
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_NO_PREROLL);
    rtspsrc = bin_find_factory (GST_BIN (e), "rtspsrc");
    g_assert_nonnull (rtspsrc);
    g_object_get (rtspsrc, "protocols", &protocols, NULL);
    g_assert_cmpuint (protocols, ==, rows[i].protocols);
    gst_object_unref (rtspsrc);
    g_assert_cmpint (gst_element_set_state (p, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
    /* back in NULL, the session is gone */
    g_assert_false (bin_has_factory (GST_BIN (e), "rtspsrc"));
    gst_object_unref (p);
  }
}

/* A video stream in a codec the bin can't decode (JPEG): an ERROR from the bin naming the encoding, and no pad. Needs
 * no GPU: nothing is decoded. */
static void
test_rtsp_bad_codec (void)
{
  RdsTestRtspServer *s = rtsp_server_or_skip ("jpeg");
  GstMessage *msg;
  GError *err = NULL;
  gchar *url, *dbg = NULL;
  Run *r;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "4", NULL);
  msg = run_wait_error (r);
  gst_message_parse_error (msg, &err, &dbg);
  g_test_message ("%s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message, dbg);
  g_assert_true (GST_MESSAGE_SRC (msg) == GST_OBJECT (r->src));
  g_assert_true (g_error_matches (err, GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND));
  g_assert_nonnull (strstr (err->message, "JPEG"));
  /* the URI may hold credentials: never in the message */
  g_assert_null (strstr (err->message, "rtsp://"));
  g_clear_error (&err);
  g_free (dbg);
  gst_message_unref (msg);
  g_assert_cmpuint (r->src->numpads, ==, 0);
  g_assert_cmpuint (r->w.n_pads, ==, 0);
  g_assert_false (bin_has_factory (GST_BIN (r->src), "rdsvideodec"));
  run_destroy (r);
  g_free (url);
  /* the bin sent DESCRIBE and no SETUP: freeing the server lets go of the media it prepared for it */
  rds_test_rtsp_server_free (s);
}

/* Nothing listens at the URI: rtspsrc's ERROR reaches the bus from inside the bin (no reconnection), and no pad. */
static void
test_rtsp_error (void)
{
  GstMessage *msg;
  GError *err = NULL;
  Run *r;

  if (!require_elements ((const gchar *[]) {
          "rtspsrc", NULL}))
    return;
  r = run_new (RTSP_NOWHERE, NULL);
  msg = run_wait_error (r);
  gst_message_parse_error (msg, &err, NULL);
  g_test_message ("%s: %s", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message);
  g_assert_true (gst_object_has_as_ancestor (GST_MESSAGE_SRC (msg), GST_OBJECT (r->src)));
  g_clear_error (&err);
  gst_message_unref (msg);
  g_assert_cmpuint (r->src->numpads, ==, 0);
  run_destroy (r);
}

/* An audio-only RTSP source: an ERROR from the bin saying it has no video stream, and no pad (@data: the value of
 * disable-audio; "true" refuses the audio, so rtspsrc has nothing to set up; "false" sets it up and discards it). No
 * GPU needed. */
static void
test_rtsp_audio_only (gconstpointer data)
{
  RdsTestRtspServer *s = rtsp_server_full_or_skip (NULL, TRUE);
  GstMessage *msg;
  GError *err = NULL;
  gchar *url;
  Run *r;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "4", "disable-audio", data, NULL);
  msg = run_wait_error (r);
  gst_message_parse_error (msg, &err, NULL);
  g_test_message ("%s: %s", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message);
  g_assert_true (GST_MESSAGE_SRC (msg) == GST_OBJECT (r->src));
  g_assert_true (g_error_matches (err, GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND));
  g_assert_nonnull (strstr (err->message, "no video stream"));
  g_clear_error (&err);
  gst_message_unref (msg);
  g_assert_cmpuint (r->src->numpads, ==, 0);
  run_destroy (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* PTS increase from frame to frame. */
static void
check_pts_increase (Run * r)
{
  g_mutex_lock (&r->w.lock);
  for (guint i = 1; i < r->w.buffers->len; i++) {
    g_assert_true (GST_CLOCK_TIME_IS_VALID (buf_at (r, i)->pts));
    g_assert_cmpuint (buf_at (r, i)->pts, >, buf_at (r, i - 1)->pts);
  }
  g_mutex_unlock (&r->w.lock);
}

/* An RTSP stream decodes to memory:RDS NV12 at the server's size on vsrc_0, with one STREAM_START and one SEGMENT and
 * increasing PTS, through rtspsrc, the depayloader, the parser and rdsvideodec (@data: "tcp", H.264 through the relay
 * with select-rtp-protocol=4; "default", H.264 direct with the default protocols; "h265", H.265 through the relay). */
static void
test_rtsp_play (gconstpointer data)
{
  const gboolean h265 = g_str_equal (data, "h265"), relay = !g_str_equal (data, "default");
  RdsTestRtspServer *s;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip (h265 ? "h265" : "h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, relay);
  r = relay ? run_new (url, "select-rtp-protocol", "4", NULL) : run_new (url, NULL);
  g_assert_cmpuint (run_pull_size (r, 60, RTSP_W, RTSP_H), ==, 60);
  (void) rds_test_bus_check (r->pipe);
  check_events (r, 0);
  check_pts_increase (r);
  g_assert_true (bin_has_factory (GST_BIN (r->src), "rtspsrc"));
  g_assert_true (bin_has_factory (GST_BIN (r->src), h265 ? "rtph265depay" : "rtph264depay"));
  g_assert_true (bin_has_factory (GST_BIN (r->src), h265 ? "h265parse" : "h264parse"));
  g_assert_true (bin_has_factory (GST_BIN (r->src), "rdsvideodec"));
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, 1);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The bin's RTSP settings reach the rtspsrc inside it (non-default values). */
static void
test_rtsp_props (void)
{
  RdsTestRtspServer *s;
  GstElement *rtspsrc;
  gboolean drop;
  guint latency, protocols;
  gint udp_size;
  guint64 teardown;
  gchar *url, *location;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "rtp-tcp", "latency", "250", "drop-on-latency", "false",
      "udp-buffer-size", "1048576", NULL);
  g_assert_cmpuint (run_pull_size (r, 10, RTSP_W, RTSP_H), ==, 10);
  rtspsrc = bin_find_factory (GST_BIN (r->src), "rtspsrc");
  g_assert_nonnull (rtspsrc);
  g_object_get (rtspsrc, "location", &location, "latency", &latency, "drop-on-latency", &drop, "udp-buffer-size",
      &udp_size, "protocols", &protocols, "teardown-timeout", &teardown, NULL);
  g_assert_cmpstr (location, ==, url);
  g_assert_cmpuint (latency, ==, 250);
  g_assert_false (drop);
  g_assert_cmpint (udp_size, ==, 1048576);
  g_assert_cmpuint (protocols, ==, 4);
  /* ours: a dead server can't hold up stopping for long */
  g_assert_cmpuint (teardown, ==, 100 * GST_MSECOND);
  g_free (location);
  gst_object_unref (rtspsrc);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* PLAYING, READY, PLAYING again: a new RTSP session, and each run has one vsrc_0, one STREAM_START and one SEGMENT,
 * and frames. */
static void
test_rtsp_restart (void)
{
  RdsTestRtspServer *s;
  gchar *url;
  guint sessions;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "4", NULL);
  g_assert_cmpuint (run_pull_size (r, 30, RTSP_W, RTSP_H), ==, 30);
  check_events (r, 0);
  sessions = rds_test_rtsp_server_sessions (s);
  run_stop (r);
  g_assert_false (bin_has_factory (GST_BIN (r->src), "rtspsrc"));
  watch_reset (&r->w);

  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpuint (run_pull_size (r, 30, RTSP_W, RTSP_H), ==, 30);
  check_events (r, 0);
  check_pts_increase (r);
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, sessions + 1);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The connection cut under a playing stream, with no reconnection (rtsp-reconnect-interval 0, the default): rtspsrc
 * takes the closed connection as the end of the stream, so EOS reaches the application through vsrc_0, with no
 * ERROR. */
static void
test_rtsp_cut (void)
{
  RdsTestRtspServer *s;
  GstBus *bus;
  GstMessage *msg;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "4", NULL);
  g_assert_cmpuint (run_pull_size (r, 30, RTSP_W, RTSP_H), ==, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
  /* what was on its way, then EOS (an ERROR fails here) */
  (void) run_pull_size (r, 0, RTSP_W, RTSP_H);
  bus = gst_element_get_bus (r->pipe);
  msg = gst_bus_timed_pop_filtered (bus, RTSP_END_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  gst_object_unref (bus);
  check_events (r, 1);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* drop-frame-interval=3 on an RTSP stream: every third frame, so the PTS step is about three frames at 30 fps. */
static void
test_rtsp_drop_frame_interval (void)
{
  const guint n = 20;
  RdsTestRtspServer *s;
  GstClockTime span;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "4", "drop-frame-interval", "3", NULL);
  g_assert_cmpuint (run_pull_size (r, n, RTSP_W, RTSP_H), ==, n);
  check_pts_increase (r);
  g_mutex_lock (&r->w.lock);
  for (guint i = 1; i < n; i++)
    g_assert_cmpuint (buf_at (r, i)->pts - buf_at (r, i - 1)->pts, >, 2 * CLIP_FRAME_NS);
  /* the jitter buffer smooths the timestamps: the mean step is three frames, within 10 % */
  span = buf_at (r, n - 1)->pts - buf_at (r, 0)->pts;
  g_test_message ("mean step %" GST_TIME_FORMAT, GST_TIME_ARGS (span / (n - 1)));
  g_assert_cmpuint (span / (n - 1), >=, 3 * CLIP_FRAME_NS * 9 / 10);
  g_assert_cmpuint (span / (n - 1), <=, 3 * CLIP_FRAME_NS * 11 / 10);
  g_mutex_unlock (&r->w.lock);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* A source with video and audio (@data: the value of disable-audio). "true" (the default): the audio is refused before
 * SETUP, so rtspsrc has one pad and nothing is discarded; "false": rtspsrc sets it up too and the bin discards it in a
 * fakesink. Video plays either way. */
static void
test_rtsp_disable_audio (gconstpointer data)
{
  const gboolean disable = g_str_equal (data, "true");
  RdsTestRtspServer *s;
  GstElement *rtspsrc;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_full_or_skip ("h264", TRUE)) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "4", "disable-audio", data, NULL);
  g_assert_cmpuint (run_pull_size (r, 30, RTSP_W, RTSP_H), ==, 30);
  (void) rds_test_bus_check (r->pipe);
  check_events (r, 0);
  rtspsrc = bin_find_factory (GST_BIN (r->src), "rtspsrc");
  g_assert_nonnull (rtspsrc);
  g_assert_cmpuint (rtspsrc->numsrcpads, ==, disable ? 1 : 2);
  gst_object_unref (rtspsrc);
  g_assert_cmpint (bin_has_factory (GST_BIN (r->src), "fakesink"), ==, !disable);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* ---- RTSP reconnection (gpu) ---- */

/* The tests' rtsp-reconnect-interval (s): short, and longer than the watchdog's one-second tick */
#define RECONNECT_INTERVAL "2"

/* How long a step of a reconnection test may take */
#define RECONNECT_TIMEOUT_US (20 * G_USEC_PER_SEC)

/* The name of logged message @i: an element message's structure name, or "WARNING" or "ERROR". (lock) */
static const gchar *
log_name (BusLog * l, guint i)
{
  GstMessage *m = g_ptr_array_index (l->msgs, i);

  switch (GST_MESSAGE_TYPE (m)) {
    case GST_MESSAGE_WARNING:
      return "WARNING";
    case GST_MESSAGE_ERROR:
      return "ERROR";
    default:
      return gst_structure_get_name (gst_message_get_structure (m));
  }
}

/* (lock) The index of the @n-th (from 0) message named @name from @src (NULL: from anywhere), or -1. */
static gint
log_find (BusLog * l, GstObject * src, const gchar * name, guint n)
{
  for (guint i = 0; i < l->msgs->len; i++) {
    GstMessage *m = g_ptr_array_index (l->msgs, i);

    if ((src == NULL || GST_MESSAGE_SRC (m) == src) && g_str_equal (log_name (l, i), name) && n-- == 0)
      return (gint) i;
  }
  return -1;
}

/* The messages named @name from @src (NULL: from anywhere) so far. */
static guint
log_count (BusLog * l, GstObject * src, const gchar * name)
{
  guint n = 0;

  g_mutex_lock (&l->lock);
  while (log_find (l, src, name, n) >= 0)
    n++;
  g_mutex_unlock (&l->lock);
  return n;
}

/* The @n-th (from 0) message named @name from @src (a ref), and when it was posted (@at may be NULL); it must be
 * there. */
static GstMessage *
log_get (BusLog * l, GstObject * src, const gchar * name, guint n, gint64 * at)
{
  GstMessage *m;
  gint i;

  g_mutex_lock (&l->lock);
  i = log_find (l, src, name, n);
  g_assert_cmpint (i, >=, 0);
  m = gst_message_ref (g_ptr_array_index (l->msgs, i));
  if (at != NULL)
    *at = g_array_index (l->at, gint64, i);
  g_mutex_unlock (&l->lock);
  return m;
}

/* Waits for the @n-th message named @name from @src; fails after RECONNECT_TIMEOUT_US. */
static void
log_wait (BusLog * l, GstObject * src, const gchar * name, guint n)
{
  gint64 deadline = g_get_monotonic_time () + RECONNECT_TIMEOUT_US;

  g_mutex_lock (&l->lock);
  while (log_find (l, src, name, n - 1) < 0) {
    if (!g_cond_wait_until (&l->cond, &l->lock, deadline))
      g_error ("no %u %s message within %u s", n, name, (guint) (RECONNECT_TIMEOUT_US / G_USEC_PER_SEC));
  }
  g_mutex_unlock (&l->lock);
}

/* A bin playing @url (rtsp-tcp through the relay, plus "property", "value" pairs) to an appsink that drops instead
 * of blocking: the tests watch vsrc_0 and the bus, and pull nothing. */
static Run *
recon_new (const gchar * url, const gchar * first, ...)
{
  va_list args;
  Run *r;

  va_start (args, first);
  r = run_new_valist (url, TRUE, first, args);
  va_end (args);
  return r;
}

static guint
n_buffers (Run * r)
{
  guint n;

  g_mutex_lock (&r->w.lock);
  n = r->w.buffers->len;
  g_mutex_unlock (&r->w.lock);
  return n;
}

/* Waits until vsrc_0 carried @n buffers; fails on an ERROR or after RECONNECT_TIMEOUT_US. */
static void
wait_buffers (Run * r, guint n)
{
  gint64 deadline = g_get_monotonic_time () + RECONNECT_TIMEOUT_US;

  while (n_buffers (r) < n) {
    if (g_get_monotonic_time () > deadline)
      g_error ("%u of %u buffers within %u s", n_buffers (r), n, (guint) (RECONNECT_TIMEOUT_US / G_USEC_PER_SEC));
    (void) rds_test_bus_check (r->pipe);
    g_usleep (20 * 1000);
  }
}

/* rds-reconnect from the bin: source-id @source_id, attempt @attempt and reason @reason (NULL: error or eos). */
static void
check_reconnect_message (Run * r, GstMessage * m, gint source_id, gint attempt, const gchar * reason)
{
  const GstStructure *st = gst_message_get_structure (m);
  const gchar *got;
  gint v;

  g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (r->src));
  g_assert_cmpstr (gst_structure_get_name (st), ==, "rds-reconnect");
  g_assert_true (gst_structure_get_int (st, "source-id", &v));
  g_assert_cmpint (v, ==, source_id);
  g_assert_true (gst_structure_get_int (st, "attempt", &v));
  g_assert_cmpint (v, ==, attempt);
  got = gst_structure_get_string (st, "reason");
  g_assert_nonnull (got);
  if (reason != NULL)
    g_assert_cmpstr (got, ==, reason);
  else
    g_assert_true (g_str_equal (got, "error") || g_str_equal (got, "eos"));
}

/* Each rds-reconnect so far from @r's bin: attempts 1, 2, ... (one streak), with @reason for the first (NULL: error
 * or eos) and any for the others; returns how many. */
static guint
check_reconnect_messages (Run * r, gint source_id, const gchar * reason)
{
  guint n = log_count (&r->log, GST_OBJECT (r->src), "rds-reconnect");

  for (guint i = 0; i < n; i++) {
    GstMessage *m = log_get (&r->log, GST_OBJECT (r->src), "rds-reconnect", i, NULL);

    gchar *str = gst_structure_to_string (gst_message_get_structure (m));

    g_test_message ("%s", str);
    g_free (str);
    check_reconnect_message (r, m, source_id, i + 1, i == 0 ? reason : gst_structure_get_string
        (gst_message_get_structure (m), "reason"));
    gst_message_unref (m);
  }
  return n;
}

/* rds-reconnected from the bin, with source-id @source_id. */
static void
check_reconnected_message (Run * r, guint n, gint source_id)
{
  GstMessage *m = log_get (&r->log, GST_OBJECT (r->src), "rds-reconnected", n, NULL);
  gint v;

  g_assert_true (gst_structure_get_int (gst_message_get_structure (m), "source-id", &v));
  g_assert_cmpint (v, ==, source_id);
  gst_message_unref (m);
}

/* After one recovery (ours): frames went on with increasing PTS, one STREAM_START and no EOS; one flush pair
 * (reset-time FALSE), then a SEGMENT before the next buffer, which is a DISCONT key frame. */
static void
check_one_reset (Run * r)
{
  BufInfo *b, *prev;

  check_pts_increase (r);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_pads, ==, 1);
  g_assert_cmpuint (r->w.n_stream_start, ==, 1);
  g_assert_cmpuint (r->w.n_eos, ==, 0);
  g_assert_cmpuint (r->w.n_flush, ==, 2);
  g_assert_false (r->w.flush_reset_time);
  g_assert_true (r->w.segment_after_flush);
  g_assert_cmpuint (r->w.n_segment, ==, 2);
  g_assert_cmpuint (r->w.flush_at, >, 0);
  g_assert_cmpuint (r->w.flush_at, <, r->w.buffers->len);
  b = buf_at (r, r->w.flush_at);
  prev = buf_at (r, r->w.flush_at - 1);
  g_test_message ("first frame after the reconnection: PTS %" GST_TIME_FORMAT ", %" GST_TIME_FORMAT " after the last "
      "one before", GST_TIME_ARGS (b->pts), GST_TIME_ARGS (b->pts - prev->pts));
  g_assert_true (b->discont);
  g_assert_false (b->delta);
  g_mutex_unlock (&r->w.lock);
}

/* A video stream in a codec the bin can't decode (JPEG) with reconnection on (rtsp-reconnect-interval 1, rtsp-reconnect-attempts 1): the bin's own error about the
 * codec is a WARNING and a failed attempt, as rtspsrc's errors are; the attempts run out before any connection, so
 * the one ERROR is the bin's, after attempt-exceeded (ours). No GPU needed. */
static void
test_rtsp_bad_codec_reconnect (void)
{
  RdsTestRtspServer *s = rtsp_server_or_skip ("jpeg");
  GstMessage *m;
  GError *err = NULL;
  gchar *url;
  Run *r;

  if (s == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = run_new (url, "select-rtp-protocol", "4", "rtsp-reconnect-interval", "1", "rtsp-reconnect-attempts", "1", NULL);
  m = run_wait_error (r);
  gst_message_parse_error (m, &err, NULL);
  g_test_message ("%s", err->message);
  g_assert_true (GST_MESSAGE_SRC (m) == GST_OBJECT (r->src));
  g_assert_true (g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ));
  g_clear_error (&err);
  gst_message_unref (m);
  m = log_get (&r->log, GST_OBJECT (r->src), "WARNING", 0, NULL);
  gst_message_parse_warning (m, &err, NULL);
  g_test_message ("%s", err->message);
  g_assert_true (g_error_matches (err, GST_STREAM_ERROR, GST_STREAM_ERROR_CODEC_NOT_FOUND));
  g_assert_nonnull (strstr (err->message, "JPEG"));
  g_clear_error (&err);
  gst_message_unref (m);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 1);
  g_assert_cmpuint (log_count (&r->log, GST_OBJECT (r->src), "attempt-exceeded"), ==, 1);
  run_destroy (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* rtsp-reconnect-interval, relay frozen for about 5 s (DeepStream 8.0 behaviour: the watchdog): one attempt or more
 * with reason timeout while it lasts, then the stream goes on (ours: no EOS or second STREAM_START, the running time
 * goes on, one flush pair and a SEGMENT before the first frame back, a DISCONT key frame), with one rds-reconnected. */
static void
test_reconnect_freeze (void)
{
  RdsTestRtspServer *s;
  gint64 frozen;
  gchar *url;
  guint n, attempts;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, "source-id", "3", NULL);
  wait_buffers (r, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_FREEZE);
  frozen = g_get_monotonic_time ();
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", 1);
  g_usleep (MAX (0, frozen + 5 * G_USEC_PER_SEC - g_get_monotonic_time ()));
  g_assert_cmpuint (log_count (&r->log, GST_OBJECT (r->src), "rds-reconnected"), ==, 0);
  n = n_buffers (r);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", 1);
  wait_buffers (r, n + 30);

  attempts = check_reconnect_messages (r, 3, "timeout");
  g_assert_cmpuint (attempts, >=, 1);
  for (guint i = 0; i < attempts; i++) {
    GstMessage *m = log_get (&r->log, GST_OBJECT (r->src), "rds-reconnect", i, NULL);

    g_assert_cmpstr (gst_structure_get_string (gst_message_get_structure (m), "reason"), ==, "timeout");
    gst_message_unref (m);
  }
  check_reconnected_message (r, 0, 3);
  g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnected"), ==, 1);
  /* one of ours per attempt */
  g_assert_cmpuint (log_count (&r->log, GST_OBJECT (r->src), "WARNING"), >=, attempts);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  g_assert_cmpuint (log_count (&r->log, NULL, "attempt-exceeded"), ==, 0);
  check_one_reset (r);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* A cut connection (rtspsrc takes it as EOS) starts an attempt at once, not after the interval (ours), with reason
 * error or eos; the stream comes back through a new RTSP session. */
static void
test_reconnect_cut (void)
{
  RdsTestRtspServer *s;
  GstMessage *m;
  gint64 cut, at;
  gchar *url;
  guint n, sessions;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, NULL);
  wait_buffers (r, 30);
  sessions = rds_test_rtsp_server_sessions (s);
  cut = g_get_monotonic_time ();
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", 1);
  m = log_get (&r->log, GST_OBJECT (r->src), "rds-reconnect", 0, &at);
  check_reconnect_message (r, m, -1, 1, NULL);
  gst_message_unref (m);
  g_test_message ("attempt %" G_GINT64_FORMAT " ms after the cut", (at - cut) / 1000);
  g_assert_cmpint (at - cut, <, G_USEC_PER_SEC);
  n = n_buffers (r);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", 1);
  wait_buffers (r, n + 30);

  g_assert_cmpuint (check_reconnect_messages (r, -1, NULL), ==, 1);
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, sessions + 1);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  check_one_reset (r);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* rtsp-reconnect-attempts 2 and a server that refuses: two attempts, then attempt-exceeded (DeepStream 8.0's name and
 * fields: source-id a uint, -1 as G_MAXUINT; attempt_exceeded TRUE), a WARNING and EOS on vsrc_0 (ours), no ERROR, and
 * no attempt after it. */
static void
test_reconnect_exhausted (void)
{
  RdsTestRtspServer *s;
  const GstStructure *st;
  GstMessage *m;
  GstBus *bus;
  gboolean exceeded;
  gchar *url, *str;
  guint id;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", "1", "rtsp-reconnect-attempts", "2", NULL);
  wait_buffers (r, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_REFUSE);
  log_wait (&r->log, GST_OBJECT (r->src), "attempt-exceeded", 1);
  m = log_get (&r->log, GST_OBJECT (r->src), "attempt-exceeded", 0, NULL);
  st = gst_message_get_structure (m);
  str = gst_structure_to_string (st);
  g_test_message ("%s", str);
  g_free (str);
  g_assert_true (G_VALUE_HOLDS_UINT (gst_structure_get_value (st, "source-id")));
  g_assert_true (gst_structure_get_uint (st, "source-id", &id));
  g_assert_cmpuint (id, ==, G_MAXUINT);
  g_assert_true (G_VALUE_HOLDS_BOOLEAN (gst_structure_get_value (st, "attempt_exceeded")));
  g_assert_true (gst_structure_get_boolean (st, "attempt_exceeded", &exceeded));
  g_assert_true (exceeded);
  gst_message_unref (m);

  /* EOS reaches the application */
  bus = gst_element_get_bus (r->pipe);
  m = gst_bus_timed_pop_filtered (bus, RTSP_END_TIMEOUT, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (m);
  g_assert_cmpint (GST_MESSAGE_TYPE (m), ==, GST_MESSAGE_EOS);
  gst_message_unref (m);
  gst_object_unref (bus);
  g_assert_cmpuint (check_reconnect_messages (r, -1, NULL), ==, 2);
  /* one per attempt and one for giving up */
  g_assert_cmpuint (log_count (&r->log, GST_OBJECT (r->src), "WARNING"), >=, 3);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  /* nothing more */
  g_usleep (3 * G_USEC_PER_SEC);
  g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnect"), ==, 2);
  g_assert_cmpuint (log_count (&r->log, NULL, "attempt-exceeded"), ==, 1);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_eos, ==, 1);
  g_assert_cmpuint (r->w.n_flush, ==, 0);
  g_mutex_unlock (&r->w.lock);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* rtsp-reconnect-attempts -1 (the default): attempts go on through four intervals of a refusing server, and the
 * stream comes back when it accepts again. */
static void
test_reconnect_unlimited (void)
{
  RdsTestRtspServer *s;
  gchar *url;
  guint n;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", "1", NULL);
  wait_buffers (r, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_REFUSE);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", 5);
  n = n_buffers (r);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", 1);
  wait_buffers (r, n + 30);
  g_assert_cmpuint (check_reconnect_messages (r, -1, NULL), >=, 5);
  g_assert_cmpuint (log_count (&r->log, NULL, "attempt-exceeded"), ==, 0);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  check_one_reset (r);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* rtsp-reconnect-attempts 1 and two separate freezes: each recovers at its first attempt, since a buffer resets the
 * count. */
static void
test_reconnect_attempts_reset (void)
{
  RdsTestRtspServer *s;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, "rtsp-reconnect-attempts", "1", NULL);
  wait_buffers (r, 30);
  for (guint i = 1; i <= 2; i++) {
    GstMessage *m;

    rds_test_rtsp_server_set_relay (s, RDS_RELAY_FREEZE);
    log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", i);
    rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
    m = log_get (&r->log, GST_OBJECT (r->src), "rds-reconnect", i - 1, NULL);
    check_reconnect_message (r, m, -1, 1, "timeout");
    gst_message_unref (m);
    log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", i);
    wait_buffers (r, n_buffers (r) + 30);
  }
  g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnect"), ==, 2);
  g_assert_cmpuint (log_count (&r->log, NULL, "attempt-exceeded"), ==, 0);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  check_pts_increase (r);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_stream_start, ==, 1);
  g_assert_cmpuint (r->w.n_eos, ==, 0);
  g_assert_cmpuint (r->w.n_flush, ==, 4);
  g_mutex_unlock (&r->w.lock);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* What leaves rdsstreammux: which sources each batch has, and the stream events per source. */
typedef struct
{
  GMutex lock;
  GArray *batches;              /* guint: bit i for source i */
  guint resets[2], eos[2];
} MuxWatch;

static GstPadProbeReturn
mux_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  MuxWatch *mw = user_data;
  guint id;

  g_mutex_lock (&mw->lock);
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
    guint mask = 0;

    for (GList * l = bm != NULL ? bm->frame_meta_list : NULL; l != NULL; l = l->next)
      mask |= 1u << ((RdsFrameMeta *) l->data)->source_id;
    g_array_append_val (mw->batches, mask);
  } else if (rds_event_parse_stream_reset (GST_PAD_PROBE_INFO_EVENT (info), &id) && id < 2) {
    mw->resets[id]++;
  } else if (rds_event_parse_stream_eos (GST_PAD_PROBE_INFO_EVENT (info), &id) && id < 2) {
    mw->eos[id]++;
  }
  g_mutex_unlock (&mw->lock);
  return GST_PAD_PROBE_OK;
}

/* Waits for @n batches of exactly the sources in @mask after the first @from batches. */
static void
mux_wait (MuxWatch * mw, guint from, guint mask, guint n)
{
  gint64 deadline = g_get_monotonic_time () + RECONNECT_TIMEOUT_US;

  for (;;) {
    guint got = 0;

    g_mutex_lock (&mw->lock);
    for (guint i = from; i < mw->batches->len; i++)
      got += g_array_index (mw->batches, guint, i) == mask;
    g_mutex_unlock (&mw->lock);
    if (got >= n)
      return;
    if (g_get_monotonic_time () > deadline)
      g_error ("%u of %u batches of sources 0x%x within %u s", got, n, mask,
          (guint) (RECONNECT_TIMEOUT_US / G_USEC_PER_SEC));
    g_usleep (20 * 1000);
  }
}

static guint
mux_batches (MuxWatch * mw)
{
  guint n;

  g_mutex_lock (&mw->lock);
  n = mw->batches->len;
  g_mutex_unlock (&mw->lock);
  return n;
}

static void
mux_pad_added (GstElement * bin, GstPad * pad, GstElement * mux)
{
  gchar *name = g_strdup_printf ("sink_%s", (const gchar *) g_object_get_data (G_OBJECT (bin), "index"));
  GstPad *sinkpad = gst_element_request_pad_simple (mux, name);

  g_assert_cmpint (gst_pad_link (pad, sinkpad), ==, GST_PAD_LINK_OK);
  gst_object_unref (sinkpad);
  g_free (name);
}

/* Two RTSP sources into rdsstreammux (live, batches of 2 at RTSP_W x RTSP_H): source 0 through the relay with
 * rtsp-reconnect-interval 2 and rtsp-reconnect-attempts 1, source 1 direct, so the relay's faults are source 0's
 * alone. Free with mux_run_free. */
typedef struct
{
  GstElement *pipe, *mux, *src[2];
  gchar *urls[2];
  MuxWatch mw;
  BusLog log;
} MuxRun;

static MuxRun *
mux_run_new (RdsTestRtspServer * s)
{
  MuxRun *m = g_new0 (MuxRun, 1);
  GstElement *sink;
  GstBus *bus;
  GstPad *pad;

  m->urls[0] = rds_test_rtsp_server_url (s, TRUE);
  m->urls[1] = rds_test_rtsp_server_url (s, FALSE);
  g_mutex_init (&m->mw.lock);
  m->mw.batches = g_array_new (FALSE, FALSE, sizeof (guint));
  g_mutex_init (&m->log.lock);
  g_cond_init (&m->log.cond);
  m->log.msgs = g_ptr_array_new_with_free_func ((GDestroyNotify) gst_message_unref);
  m->log.at = g_array_new (FALSE, FALSE, sizeof (gint64));

  m->pipe = gst_pipeline_new (NULL);
  m->mux = gst_element_factory_make ("rdsstreammux", NULL);
  sink = gst_element_factory_make ("fakesink", NULL);
  g_object_set (m->mux, "batch-size", 2u, "width", RTSP_W, "height", RTSP_H, "live-source", TRUE,
      "batched-push-timeout", 40000, NULL);
  g_object_set (sink, "sync", FALSE, "async", FALSE, NULL);
  gst_bin_add_many (GST_BIN (m->pipe), m->mux, sink, NULL);
  g_assert_true (gst_element_link (m->mux, sink));
  pad = gst_element_get_static_pad (m->mux, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, mux_probe, &m->mw, NULL);
  gst_object_unref (pad);
  for (guint i = 0; i < 2; i++) {
    m->src[i] = gst_element_factory_make ("rdsurisrcbin", NULL);
    g_object_set (m->src[i], "uri", m->urls[i], "select-rtp-protocol", 4, NULL);
    g_object_set_data (G_OBJECT (m->src[i]), "index", (gpointer) (i == 0 ? "0" : "1"));
    g_signal_connect (m->src[i], "pad-added", G_CALLBACK (mux_pad_added), m->mux);
    gst_bin_add (GST_BIN (m->pipe), m->src[i]);
  }
  g_object_set (m->src[0], "rtsp-reconnect-interval", 2u, "rtsp-reconnect-attempts", 1, NULL);
  bus = gst_element_get_bus (m->pipe);
  gst_bus_set_sync_handler (bus, bus_log_sync, &m->log, NULL);
  gst_object_unref (bus);
  g_assert_cmpint (gst_element_set_state (m->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  return m;
}

static void
mux_run_free (MuxRun * m)
{
  GstBus *bus = gst_element_get_bus (m->pipe);

  g_assert_cmpint (gst_element_set_state (m->pipe, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  gst_bus_set_sync_handler (bus, NULL, NULL, NULL);
  gst_object_unref (bus);
  gst_object_unref (m->pipe);
  g_ptr_array_unref (m->log.msgs);
  g_array_unref (m->log.at);
  g_mutex_clear (&m->log.lock);
  g_cond_clear (&m->log.cond);
  g_array_unref (m->mw.batches);
  g_mutex_clear (&m->mw.lock);
  g_free (m->urls[0]);
  g_free (m->urls[1]);
  g_free (m);
}

/* The rds-stream-reset events per source so far. */
static void
mux_check_resets (MuxRun * m, guint source0, guint source1)
{
  g_mutex_lock (&m->mw.lock);
  g_assert_cmpuint (m->mw.resets[0], ==, source0);
  g_assert_cmpuint (m->mw.resets[1], ==, source1);
  g_mutex_unlock (&m->mw.lock);
}

/* Two RTSP sources into rdsstreammux (mux_run_new): while source 0 is frozen the batches go on with source 1 alone,
 * after its recovery both are in them again, and downstream sees one rds-stream-reset for source 0; once source 0
 * gives up (a refusing server) the mux ends its stream and goes on batching source 1. */
static void
test_reconnect_mux (void)
{
  RdsTestRtspServer *s;
  MuxRun *m;
  guint from;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  m = mux_run_new (s);
  mux_wait (&m->mw, 0, 0x3, 10);
  /* frozen: source 1 alone */
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_FREEZE);
  log_wait (&m->log, GST_OBJECT (m->src[0]), "rds-reconnect", 1);
  from = mux_batches (&m->mw);
  mux_wait (&m->mw, from, 0x2, 10);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
  /* back: both */
  log_wait (&m->log, GST_OBJECT (m->src[0]), "rds-reconnected", 1);
  from = mux_batches (&m->mw);
  mux_wait (&m->mw, from, 0x3, 10);
  mux_check_resets (m, 1, 0);

  /* source 0 gives up: its stream ends, source 1 goes on */
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_REFUSE);
  log_wait (&m->log, GST_OBJECT (m->src[0]), "attempt-exceeded", 1);
  for (gint64 deadline = g_get_monotonic_time () + RECONNECT_TIMEOUT_US;;) {
    guint eos;

    g_mutex_lock (&m->mw.lock);
    eos = m->mw.eos[0];
    g_mutex_unlock (&m->mw.lock);
    if (eos == 1)
      break;
    g_assert_cmpint (g_get_monotonic_time (), <, deadline);
    g_usleep (20 * 1000);
  }
  from = mux_batches (&m->mw);
  mux_wait (&m->mw, from, 0x2, 10);
  g_mutex_lock (&m->mw.lock);
  g_assert_cmpuint (m->mw.eos[1], ==, 0);
  g_mutex_unlock (&m->mw.lock);
  mux_check_resets (m, 1, 0);
  g_assert_cmpuint (log_count (&m->log, NULL, "ERROR"), ==, 0);
  g_assert_cmpuint (log_count (&m->log, GST_OBJECT (m->src[1]), "rds-reconnect"), ==, 0);
  mux_run_free (m);
  rds_test_rtsp_server_free (s);
}

/* The current caps of vsrc_0 are @width x @height. */
static void
check_size (Run * r, gint width, gint height)
{
  GstCaps *caps = gst_pad_get_current_caps (r->w.pad);
  GstVideoInfo info;

  g_assert_nonnull (caps);
  g_assert_true (gst_video_info_from_caps (&info, caps));
  g_assert_cmpint (GST_VIDEO_INFO_WIDTH (&info), ==, width);
  g_assert_cmpint (GST_VIDEO_INFO_HEIGHT (&info), ==, height);
  gst_caps_unref (caps);
}

/* The server's picture grows from RTSP_W x RTSP_H to 1280x720 across a cut (ours): a successful reconnection, with
 * new CAPS on vsrc_0 and frames at the new size, and no ERROR. */
static void
test_reconnect_resize (void)
{
  RdsTestRtspServer *s;
  gchar *url;
  guint caps;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, NULL);
  wait_buffers (r, 30);
  check_size (r, RTSP_W, RTSP_H);
  g_mutex_lock (&r->w.lock);
  caps = r->w.n_caps;
  g_mutex_unlock (&r->w.lock);
  rds_test_rtsp_server_set_stream (s, 1280, 720, "h264");
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", 1);
  wait_buffers (r, n_buffers (r) + 30);
  check_size (r, 1280, 720);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_caps, >, caps);
  /* each frame at the size of the caps it was pushed under: the old picture the decoder held leaves before the new
   * CAPS */
  for (guint i = 0; i < r->w.buffers->len; i++) {
    BufInfo *b = buf_at (r, i);

    g_assert_cmpint (b->width, ==, b->caps_width);
    g_assert_cmpint (b->height, ==, b->caps_height);
  }
  g_assert_cmpint (buf_at (r, 0)->width, ==, RTSP_W);
  g_assert_cmpint (buf_at (r, r->w.buffers->len - 1)->width, ==, 1280);
  g_mutex_unlock (&r->w.lock);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  check_one_reset (r);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The same behind rdsstreammux (mux_run_new): source 0 comes back at 1280x720, and the mux takes the new caps as any
 * caps change, scaling the frames into its fixed-size slots and batching both sources again, with one
 * rds-stream-reset for source 0. */
static void
test_reconnect_resize_mux (void)
{
  RdsTestRtspServer *s;
  MuxRun *m;
  guint from;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  m = mux_run_new (s);
  mux_wait (&m->mw, 0, 0x3, 10);
  rds_test_rtsp_server_set_stream (s, 1280, 720, "h264");
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
  log_wait (&m->log, GST_OBJECT (m->src[0]), "rds-reconnected", 1);
  from = mux_batches (&m->mw);
  mux_wait (&m->mw, from, 0x3, 30);
  mux_check_resets (m, 1, 0);
  g_assert_cmpuint (log_count (&m->log, NULL, "ERROR"), ==, 0);
  mux_run_free (m);
  rds_test_rtsp_server_free (s);
}

/* The server switches from H.264 to H.265 across a cut (ours; DeepStream 8.0 fails every attempt): the bin swaps the
 * parser in front of the same rdsvideodec, which takes the new codec, and the reconnection is a normal one on the same
 * vsrc_0: one flush pair, the SEGMENT again, PTS going on, no ERROR. */
static void
test_reconnect_codec (void)
{
  RdsTestRtspServer *s;
  GstElement *dec, *dec_after;
  gchar *url, *why = NULL;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if (!rds_test_rtsp_server_codec_available ("h265", &why)) {
    g_test_skip (why);
    g_free (why);
    return;
  }
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, NULL);
  wait_buffers (r, 30);
  dec = bin_find_factory (GST_BIN (r->src), "rdsvideodec");
  g_assert_nonnull (dec);
  rds_test_rtsp_server_set_stream (s, RTSP_W, RTSP_H, "h265");
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", 1);
  wait_buffers (r, n_buffers (r) + 30);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnect"), ==, 1);
  g_assert_true (bin_has_factory (GST_BIN (r->src), "h265parse"));
  g_assert_false (bin_has_factory (GST_BIN (r->src), "h264parse"));
  dec_after = bin_find_factory (GST_BIN (r->src), "rdsvideodec");
  g_assert_true (dec_after == dec);
  gst_object_unref (dec_after);
  gst_object_unref (dec);
  check_size (r, RTSP_W, RTSP_H);
  check_one_reset (r);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* This process's threads. */
static guint
n_threads (void)
{
  gchar *status = NULL, *line;
  guint n = 0;

  g_assert_true (g_file_get_contents ("/proc/self/status", &status, NULL, NULL));
  line = strstr (status, "\nThreads:");
  g_assert_nonnull (line);
  n = (guint) atoi (line + strlen ("\nThreads:"));
  g_free (status);
  return n;
}

/* Ten cycles of PLAYING then NULL, then ten of PLAYING then READY, on a live RTSP source: each is quick, leaves no
 * rtspsrc and no pad behind, makes one server session, and the threads don't pile up (ASan checks the leaks). With
 * @data "faults", reconnection is on (rtsp-reconnect-interval 1) and some cycles stop during a fault: a server that
 * refuses from the start (start-up retries), or a freeze while playing. */
static void
test_state_cycles (gconstpointer data)
{
  const gboolean faults = g_str_equal (data, "faults");
  RdsTestRtspServer *s;
  guint healthy = 0, threads = 0;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", faults ? "1" : "0", NULL);
  for (guint cycle = 0; cycle < 20; cycle++) {
    /* the first cycle (started by recon_new) plays */
    gboolean refuse = faults && cycle % 3 == 1, freeze = faults && cycle % 3 == 2;
    GstState down = cycle < 10 ? GST_STATE_NULL : GST_STATE_READY;
    guint attempts = log_count (&r->log, NULL, "rds-reconnect");
    gint64 t;

    if (cycle > 0) {
      rds_test_rtsp_server_set_relay (s, refuse ? RDS_RELAY_REFUSE : RDS_RELAY_PASS);
      watch_reset (&r->w);
      g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
    }
    if (refuse) {
      log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", attempts + 1);
      g_assert_cmpuint (r->src->numpads, ==, 0);
    } else {
      wait_buffers (r, 10);
      healthy++;
      if (freeze) {
        rds_test_rtsp_server_set_relay (s, RDS_RELAY_FREEZE);
        log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", attempts + 1);
      }
    }
    t = g_get_monotonic_time ();
    g_assert_cmpint (gst_element_set_state (r->pipe, down), ==, GST_STATE_CHANGE_SUCCESS);
    t = g_get_monotonic_time () - t;
    g_assert_cmpint (t, <, 2 * G_USEC_PER_SEC);
    g_assert_cmpuint (r->src->numpads, ==, 0);
    g_assert_false (bin_has_factory (GST_BIN (r->src), "rtspsrc"));
    if (cycle == 1)
      threads = n_threads ();
  }
  g_test_message ("threads after the second cycle %u, at the end %u", threads, n_threads ());
  g_assert_cmpuint (n_threads (), <=, threads + 8);
  /* one per playing cycle: refused connections never reach the server, and the relay's FREEZE also holds the
   * connections made while it lasts, so the attempts during a freeze set up no session either */
  g_assert_cmpuint (rds_test_rtsp_server_sessions (s), ==, healthy);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
  run_destroy (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The first connection refused for about 3 s (@data): "init", init-rtsp-reconnect-interval 1 (ours: it does nothing in
 * DeepStream 8.0); "interval", rtsp-reconnect-interval 1 (DeepStream 8.0 behaviour); either way the bin retries,
 * rtspsrc's errors are WARNINGs, vsrc_0 comes with the first frames, and there is no flush. "off": neither, and the
 * first failure is an ERROR. */
static void
test_init_retry (gconstpointer data)
{
  RdsTestRtspServer *s;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_REFUSE);
  if (g_str_equal (data, "init"))
    r = recon_new (url, "init-rtsp-reconnect-interval", "1", NULL);
  else if (g_str_equal (data, "interval"))
    r = recon_new (url, "rtsp-reconnect-interval", "1", NULL);
  else
    r = recon_new (url, NULL);
  if (g_str_equal (data, "off")) {
    GstMessage *m = run_wait_error (r);

    g_assert_true (gst_object_has_as_ancestor (GST_MESSAGE_SRC (m), GST_OBJECT (r->src)));
    gst_message_unref (m);
    g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnect"), ==, 0);
    run_destroy (r);
    g_free (url);
    rds_test_rtsp_server_free (s);
    return;
  }

  g_usleep (3 * G_USEC_PER_SEC);
  g_assert_cmpuint (r->src->numpads, ==, 0);
  g_assert_cmpuint (log_count (&r->log, GST_OBJECT (r->src), "rds-reconnect"), >=, 1);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_PASS);
  wait_buffers (r, 30);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", 1);
  (void) check_reconnect_messages (r, -1, NULL);
  /* rtspsrc's failures, as WARNINGs */
  g_assert_cmpuint (log_count (&r->log, NULL, "WARNING"), >,
      log_count (&r->log, GST_OBJECT (r->src), "WARNING"));
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  check_events (r, 0);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The parser around the key-frame gate. Its input is one access unit per buffer (the depayloader's), each session's
 * first one DISCONT; the gate drops at its src pad, after which the probe here sees what passed. So the access units
 * in from a session's first one to its first one out, less that one, are what the gate dropped. */
typedef struct
{
  GMutex lock;
  guint in;                     /* access units in since the session's first */
  gboolean out;                 /* one of them came out */
  guint dropped;                /* in all */
  guint discont, discont_delta; /* DISCONT buffers out, and those of them that are delta units */
} ParserWatch;

static GstPadProbeReturn
parser_in_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  ParserWatch *pw = user_data;

  g_mutex_lock (&pw->lock);
  if (GST_BUFFER_FLAG_IS_SET (GST_PAD_PROBE_INFO_BUFFER (info), GST_BUFFER_FLAG_DISCONT)) {
    pw->in = 0;
    pw->out = FALSE;
  }
  pw->in++;
  g_mutex_unlock (&pw->lock);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
parser_out_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  ParserWatch *pw = user_data;
  GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER (info);

  g_mutex_lock (&pw->lock);
  if (!pw->out) {
    pw->out = TRUE;
    pw->dropped += pw->in - 1;
  }
  if (GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DISCONT)) {
    pw->discont++;
    pw->discont_delta += GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DELTA_UNIT);
  }
  g_mutex_unlock (&pw->lock);
  return GST_PAD_PROBE_OK;
}

/* The key-frame gate (ours): with a shared server, a new session joins the stream mid-GOP (a second client, direct,
 * keeps the media playing while the bin's connection is cut); after a reconnection the delta units before the first
 * key frame are dropped, and that key frame goes on DISCONT. */
static void
test_keyframe_gate (void)
{
  RdsTestRtspServerOptions opts = {
    .relay = TRUE,
    .width = RTSP_W,
    .height = RTSP_H,
    .codec = "h264",
    .shared = TRUE,
  };
  ParserWatch pw = { 0 };
  RdsTestRtspServer *s;
  GstElement *parse, *keeper;
  GError *err = NULL;
  GstPad *pad;
  gchar *url, *desc, *why = NULL;
  guint dropped = 0, round;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if (!require_elements ((const gchar *[]) {
          "rtspsrc", NULL}))
    return;
  if ((s = rds_test_rtsp_server_new_full (&opts, &why)) == NULL) {
    g_test_skip (why);
    g_free (why);
    return;
  }
  g_mutex_init (&pw.lock);
  url = rds_test_rtsp_server_url (s, FALSE);
  desc = g_strdup_printf ("rtspsrc location=%s protocols=tcp latency=100 ! fakesink sync=false", url);
  keeper = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  g_free (url);
  g_assert_cmpint (gst_element_set_state (keeper, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, NULL);
  wait_buffers (r, 30);
  parse = bin_find_factory (GST_BIN (r->src), "h264parse");
  g_assert_nonnull (parse);
  pad = gst_element_get_static_pad (parse, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, parser_out_probe, &pw, NULL);
  gst_object_unref (pad);
  pad = gst_element_get_static_pad (parse, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, parser_in_probe, &pw, NULL);
  gst_object_unref (pad);
  gst_object_unref (parse);
  /* a session can join on a key frame (one in 30): a few tries */
  for (round = 1; round <= 3 && dropped == 0; round++) {
    guint n = n_buffers (r);

    rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
    log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", round);
    wait_buffers (r, n + 30);
    g_mutex_lock (&pw.lock);
    dropped = pw.dropped;
    g_mutex_unlock (&pw.lock);
    g_test_message ("reconnection %u: %u delta units dropped", round, dropped);
  }
  g_assert_cmpuint (dropped, >, 0);
  g_mutex_lock (&pw.lock);
  /* the first key frame of each new session is DISCONT */
  g_assert_cmpuint (pw.discont, >=, round - 1);
  g_assert_cmpuint (pw.discont_delta, ==, 0);
  g_mutex_unlock (&pw.lock);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_flush, ==, 2 * (round - 1));
  g_assert_true (buf_at (r, r->w.flush_at)->discont);
  g_mutex_unlock (&r->w.lock);
  run_free (r);
  gst_element_set_state (keeper, GST_STATE_NULL);
  gst_object_unref (keeper);
  g_mutex_clear (&pw.lock);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* Stopping while an attempt hangs (@data "freeze": its connection never answers) or waits for the next one
 * ("refuse"): back to NULL within 2 s. The bus holds the attempt's thread 500 ms on rds-reconnect, as a sync handler
 * might, so stopping waits for an attempt under way. */
static void
test_stop_during_reconnect (gconstpointer data)
{
  RdsTestRtspServer *s;
  gint64 t;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, NULL);
  g_mutex_lock (&r->log.lock);
  r->log.block_name = "rds-reconnect";
  r->log.block_us = 500 * 1000;
  g_mutex_unlock (&r->log.lock);
  wait_buffers (r, 30);
  rds_test_rtsp_server_set_relay (s, g_str_equal (data, "freeze") ? RDS_RELAY_FREEZE : RDS_RELAY_REFUSE);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", 1);
  t = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  t = g_get_monotonic_time () - t;
  g_test_message ("to NULL in %" G_GINT64_FORMAT " ms", t / 1000);
  /* it waited for the attempt, held on the bus */
  g_assert_cmpint (t, >=, 100 * 1000);
  g_assert_cmpint (t, <, 2 * G_USEC_PER_SEC);
  g_assert_false (bin_has_factory (GST_BIN (r->src), "rtspsrc"));
  run_destroy (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The application's EOS while the source is down (a refusing server, attempts going on): the stream ends anyway, EOS
 * on vsrc_0 and from the pipeline, and the attempts stop. */
static void
test_reconnect_app_eos_down (void)
{
  RdsTestRtspServer *s;
  GstMessage *m;
  GstBus *bus;
  gchar *url;
  guint attempts;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", "1", NULL);
  wait_buffers (r, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_REFUSE);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnect", 1);
  g_assert_true (gst_element_send_event (r->pipe, gst_event_new_eos ()));
  bus = gst_element_get_bus (r->pipe);
  m = gst_bus_timed_pop_filtered (bus, 5 * GST_SECOND, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  g_assert_nonnull (m);
  g_assert_cmpint (GST_MESSAGE_TYPE (m), ==, GST_MESSAGE_EOS);
  gst_message_unref (m);
  gst_object_unref (bus);
  attempts = log_count (&r->log, NULL, "rds-reconnect");
  g_usleep (2500 * 1000);
  g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnect"), ==, attempts);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_eos, ==, 1);
  g_mutex_unlock (&r->w.lock);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The attempts run out before the first connection (init-rtsp-reconnect-interval 1, rtsp-reconnect-attempts 1, a
 * server that refuses throughout): attempt-exceeded, then an ERROR from the bin (ours: DeepStream 8.0 posts the
 * message only, and the application waits forever). */
static void
test_init_give_up (void)
{
  RdsTestRtspServer *s;
  GstMessage *m;
  GError *err = NULL;
  gchar *url;
  gint exceeded, error;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_REFUSE);
  r = recon_new (url, "init-rtsp-reconnect-interval", "1", "rtsp-reconnect-attempts", "1", NULL);
  log_wait (&r->log, GST_OBJECT (r->src), "ERROR", 1);
  g_mutex_lock (&r->log.lock);
  exceeded = log_find (&r->log, GST_OBJECT (r->src), "attempt-exceeded", 0);
  error = log_find (&r->log, GST_OBJECT (r->src), "ERROR", 0);
  g_mutex_unlock (&r->log.lock);
  g_assert_cmpint (exceeded, >=, 0);
  g_assert_cmpint (exceeded, <, error);
  m = log_get (&r->log, GST_OBJECT (r->src), "ERROR", 0, NULL);
  gst_message_parse_error (m, &err, NULL);
  g_test_message ("%s", err->message);
  g_assert_true (g_error_matches (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ));
  g_clear_error (&err);
  gst_message_unref (m);
  g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnect"), ==, 1);
  g_assert_cmpuint (r->src->numpads, ==, 0);
  run_destroy (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* The application's EOS right before a restart (READY, then PLAYING again): the second run still reconnects, and no
 * ERROR reaches the application from the teardown. */
static void
test_reconnect_app_eos_restart (void)
{
  RdsTestRtspServer *s;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, NULL);
  wait_buffers (r, 30);
  g_assert_true (gst_element_send_event (r->pipe, gst_event_new_eos ()));
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  watch_reset (&r->w);
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  wait_buffers (r, 30);
  rds_test_rtsp_server_set_relay (s, RDS_RELAY_CUT);
  log_wait (&r->log, GST_OBJECT (r->src), "rds-reconnected", 1);
  wait_buffers (r, n_buffers (r) + 30);
  g_assert_cmpuint (log_count (&r->log, NULL, "ERROR"), ==, 0);
  g_mutex_lock (&r->w.lock);
  g_assert_cmpuint (r->w.n_eos, ==, 0);
  g_mutex_unlock (&r->w.lock);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

/* With reconnection on, an EOS the application sends still ends the stream: no attempt, then or after the interval. */
static void
test_reconnect_app_eos (void)
{
  RdsTestRtspServer *s;
  gchar *url;
  Run *r;

  RDS_TEST_REQUIRE_GPU ();
  if ((s = rtsp_server_or_skip ("h264")) == NULL)
    return;
  url = rds_test_rtsp_server_url (s, TRUE);
  r = recon_new (url, "rtsp-reconnect-interval", RECONNECT_INTERVAL, NULL);
  wait_buffers (r, 30);
  g_assert_true (gst_element_send_event (r->pipe, gst_event_new_eos ()));
  run_wait_eos (r);
  g_usleep (3500 * 1000);
  g_assert_cmpuint (log_count (&r->log, NULL, "rds-reconnect"), ==, 0);
  check_events (r, 1);
  run_free (r);
  g_free (url);
  rds_test_rtsp_server_free (s);
}

static void
remove_media (void)
{
  GDir *dir;
  const gchar *name;

  if (media_dir == NULL)
    return;
  dir = g_dir_open (media_dir, 0, NULL);
  while ((name = g_dir_read_name (dir)) != NULL) {
    gchar *path = g_build_filename (media_dir, name, NULL);

    g_assert_cmpint (g_unlink (path), ==, 0);
    g_free (path);
  }
  g_dir_close (dir);
  g_assert_cmpint (g_rmdir (media_dir), ==, 0);
  g_clear_pointer (&media_dir, g_free);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  warnings = g_ptr_array_new_with_free_func (g_free);
  gst_debug_set_threshold_for_name ("rdsurisrcbin", GST_LEVEL_WARNING);
  gst_debug_add_log_function (collect_warnings, NULL, NULL);

  g_test_add_func ("/cpu/urisrcbin/registered", test_registered);
  g_test_add_func ("/cpu/urisrcbin/properties", test_properties);
  g_test_add_func ("/cpu/urisrcbin/noop-warning", test_noop_warning);
  g_test_add_func ("/cpu/urisrcbin/start-errors", test_start_errors);
  g_test_add_func ("/cpu/urisrcbin/rtsp-live", test_rtsp_live);
  g_test_add_func ("/cpu/urisrcbin/rtsp-protocols", test_rtsp_protocols);
  g_test_add_func ("/cpu/urisrcbin/rtsp-bad-codec", test_rtsp_bad_codec);
  g_test_add_func ("/cpu/urisrcbin/rtsp-error", test_rtsp_error);
  g_test_add_func ("/cpu/urisrcbin/rtsp-bad-codec-reconnect", test_rtsp_bad_codec_reconnect);
  g_test_add_data_func ("/cpu/urisrcbin/rtsp-audio-only", "true", test_rtsp_audio_only);
  g_test_add_data_func ("/cpu/urisrcbin/rtsp-audio-only-kept", "false", test_rtsp_audio_only);
  g_test_add_data_func ("/gpu/urisrcbin/file-decode", "mp4", test_file_decode);
  g_test_add_data_func ("/gpu/urisrcbin/file-decode-byte-stream", "h264", test_file_decode);
  g_test_add_data_func ("/gpu/urisrcbin/audio-ignored", "audio", test_file_decode);
  g_test_add_func ("/gpu/urisrcbin/system-memory", test_system_memory);
  g_test_add_data_func ("/gpu/urisrcbin/file-loop", "mp4", test_file_loop);
  g_test_add_data_func ("/gpu/urisrcbin/file-loop-byte-stream", "h264", test_file_loop);
  g_test_add_data_func ("/gpu/urisrcbin/file-loop-system-memory", "mjpeg", test_file_loop);
  g_test_add_func ("/gpu/urisrcbin/file-loop-seek", test_file_loop_seek);
  g_test_add_func ("/gpu/urisrcbin/file-loop-eos", test_file_loop_eos);
  g_test_add_func ("/gpu/urisrcbin/file-loop-eos-seek", test_file_loop_eos_seek);
  g_test_add_data_func ("/gpu/urisrcbin/restart", "mp4", test_restart);
  g_test_add_data_func ("/gpu/urisrcbin/restart-system-memory", "mjpeg", test_restart);
  g_test_add_data_func ("/gpu/urisrcbin/drop-frame-interval", GUINT_TO_POINTER (5), test_drop_frame_interval);
  g_test_add_data_func ("/gpu/urisrcbin/drop-frame-interval-1", GUINT_TO_POINTER (1), test_drop_frame_interval);
  g_test_add_func ("/gpu/urisrcbin/drop-frame-interval-loop", test_drop_frame_interval_loop);
  g_test_add_data_func ("/gpu/urisrcbin/rtsp-tcp", "tcp", test_rtsp_play);
  g_test_add_data_func ("/gpu/urisrcbin/rtsp-default", "default", test_rtsp_play);
  g_test_add_data_func ("/gpu/urisrcbin/rtsp-h265", "h265", test_rtsp_play);
  g_test_add_func ("/gpu/urisrcbin/rtsp-props", test_rtsp_props);
  g_test_add_func ("/gpu/urisrcbin/rtsp-restart", test_rtsp_restart);
  g_test_add_func ("/gpu/urisrcbin/rtsp-cut", test_rtsp_cut);
  g_test_add_func ("/gpu/urisrcbin/rtsp-drop-frame-interval", test_rtsp_drop_frame_interval);
  g_test_add_data_func ("/gpu/urisrcbin/rtsp-disable-audio", "true", test_rtsp_disable_audio);
  g_test_add_data_func ("/gpu/urisrcbin/rtsp-keep-audio", "false", test_rtsp_disable_audio);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-freeze", test_reconnect_freeze);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-cut", test_reconnect_cut);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-exhausted", test_reconnect_exhausted);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-unlimited", test_reconnect_unlimited);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-attempts-reset", test_reconnect_attempts_reset);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-mux", test_reconnect_mux);
  g_test_add_data_func ("/gpu/urisrcbin-reconnect/init-retry", "init", test_init_retry);
  g_test_add_data_func ("/gpu/urisrcbin-reconnect/init-retry-interval", "interval", test_init_retry);
  g_test_add_data_func ("/gpu/urisrcbin-reconnect/init-retry-off", "off", test_init_retry);
  g_test_add_func ("/gpu/urisrcbin-reconnect/keyframe-gate", test_keyframe_gate);
  g_test_add_data_func ("/gpu/urisrcbin-reconnect/stop-during-reconnect-freeze", "freeze", test_stop_during_reconnect);
  g_test_add_data_func ("/gpu/urisrcbin-reconnect/stop-during-reconnect-refuse", "refuse", test_stop_during_reconnect);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-app-eos", test_reconnect_app_eos);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-app-eos-down", test_reconnect_app_eos_down);
  g_test_add_func ("/gpu/urisrcbin-reconnect/reconnect-app-eos-restart", test_reconnect_app_eos_restart);
  g_test_add_func ("/gpu/urisrcbin-reconnect/init-give-up", test_init_give_up);
  g_test_add_func ("/gpu/urisrcbin-reconnect/resize", test_reconnect_resize);
  g_test_add_func ("/gpu/urisrcbin-reconnect/resize-mux", test_reconnect_resize_mux);
  g_test_add_func ("/gpu/urisrcbin-reconnect/codec", test_reconnect_codec);
  g_test_add_data_func ("/gpu/urisrcbin-reconnect/state-cycles", "plain", test_state_cycles);
  g_test_add_data_func ("/gpu/urisrcbin-reconnect/state-cycles-faults", "faults", test_state_cycles);
  ret = g_test_run ();
  remove_media ();

  gst_debug_remove_log_function (collect_warnings);
  g_ptr_array_unref (warnings);
  gst_deinit ();
  return ret;
}
