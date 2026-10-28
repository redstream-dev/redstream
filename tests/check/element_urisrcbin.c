/* rdsurisrcbin: factory, pad template and the nvurisrcbin remap, properties (names, types, ranges and defaults as in
 * DeepStream 8.0 behaviour), the warning for accepted no-ops, the settings checked when it starts, and for RTSP URIs
 * the live state changes, rtspsrc's protocols, a server that isn't there, a stream in a codec it can't decode and an
 * audio-only stream (cpu; the last two need gst-rtsp-server, jpegenc and opusenc). Decoding file URIs to vsrc_0 (an
 * MP4, a raw H.264 byte stream, MJPEG through rdsvideoconvert, a file with audio), file-loop and drop-frame-interval;
 * RTSP streams from the test server (H.264 and H.265, TCP through its relay and the default protocols direct),
 * rtspsrc's settings, a restart, a cut connection, drop-frame-interval and disable-audio (gpu, skipped without a ROCm
 * device, the VA encoder or gst-rtsp-server).
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
  gboolean discont;
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
  guint flush_at;               /* buffers before the last FLUSH_STOP */
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
      GST_BUFFER_FLAG_IS_SET (buf, GST_BUFFER_FLAG_DISCONT)
    };

    g_array_append_val (w->buffers, b);
  } else {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);

    switch (GST_EVENT_TYPE (ev)) {
      case GST_EVENT_STREAM_START:
        w->n_stream_start++;
        break;
      case GST_EVENT_CAPS:
        w->n_caps++;
        break;
      case GST_EVENT_SEGMENT:
        if (w->n_segment++ == 0)
          gst_event_copy_segment (ev, &w->segment);
        gst_event_copy_segment (ev, &w->last_segment);
        break;
      case GST_EVENT_EOS:
        w->n_eos++;
        break;
      case GST_EVENT_FLUSH_START:
        w->n_flush++;
        break;
      case GST_EVENT_FLUSH_STOP:
        w->n_flush++;
        w->flush_at = w->buffers->len;
        break;
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
typedef struct
{
  GstElement *pipe, *src, *sink;
  Watch w;
} Run;

static Run *
run_new (const gchar * uri, const gchar * first, ...)
{
  Run *r = g_new0 (Run, 1);
  const gchar *prop = first;
  va_list args;

  r->pipe = gst_pipeline_new (NULL);
  r->src = gst_element_factory_make ("rdsurisrcbin", NULL);
  r->sink = gst_element_factory_make ("appsink", NULL);
  g_assert_nonnull (r->src);
  /* a short queue: a looping source blocks in the appsink instead of filling memory */
  g_object_set (r->src, "uri", uri, NULL);
  g_object_set (r->sink, "sync", FALSE, "max-buffers", 4u, NULL);
  va_start (args, first);
  while (prop != NULL) {
    gst_util_set_object_arg (G_OBJECT (r->src), prop, va_arg (args, const gchar *));
    prop = va_arg (args, const gchar *);
  }
  va_end (args);
  gst_bin_add_many (GST_BIN (r->pipe), r->src, r->sink, NULL);
  g_mutex_init (&r->w.lock);
  r->w.sink = r->sink;
  r->w.need_caps = g_str_has_prefix (uri, "rtsp");
  r->w.buffers = g_array_new (FALSE, FALSE, sizeof (BufInfo));
  g_signal_connect (r->src, "pad-added", G_CALLBACK (on_pad_added), &r->w);
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
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
  g_array_set_size (w->buffers, 0);
  g_mutex_unlock (&w->lock);
}

/* NULL and free, whatever the pads. */
static void
run_destroy (Run * r)
{
  g_assert_cmpint (gst_element_set_state (r->pipe, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  gst_object_unref (r->pipe);
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
  ret = g_test_run ();
  remove_media ();

  gst_debug_remove_log_function (collect_warnings);
  g_ptr_array_unref (warnings);
  gst_deinit ();
  return ret;
}
