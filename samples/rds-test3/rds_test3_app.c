/* rds-test3: multi-stream detection sample in the style of a DeepStream application.
 * Built against Redstream's compat header, so DeepStream factory names are remapped at runtime.
 *
 *   rds-test3 [--dry-run] [--no-display] [--pgie-config FILE] [--tracker-config FILE]
 *             [--rtsp-reconnect-interval S] [--rtsp-reconnect-attempts N] [--select-rtp-protocol P] URI [URI...]
 *
 * --pgie-config defaults to rds_test3_pgie_config.yml in this sample's source directory. --tracker-config adds a
 * tracker (nvtracker, with that low-level config file) after the primary detector; without it there is none, as in
 * DeepStream's test3. --no-display replaces the display sink with "fakesink sync=false" (as fast as it goes).
 *
 * Each URI is an nvurisrcbin (file, RTSP, anything GStreamer has a source for), its source-id the URI's position, its
 * vsrc_0 linked to the muxer's sink_<position>. For RTSP URIs, --rtsp-reconnect-interval (seconds, default 0: no
 * reconnection, as DeepStream 8.0), --rtsp-reconnect-attempts (default -1: unlimited) and --select-rtp-protocol (4:
 * TCP; default: nvurisrcbin's) set the properties of the same names on every source, and the reconnection messages
 * are printed as they come:
 *
 *   source <source_id>: reconnecting, attempt <n> (<reason>)
 *   source <source_id>: reconnected
 *   source <source_id>: gave up reconnecting
 *
 * Ctrl-C (SIGINT) stops with an EOS: the sources end their streams, and the muxer ends each source that never had a
 * stream (no vsrc_0 yet: such a source never sends an EOS, so the app treats it as already stopped). Should the EOS
 * still not come within 10 s, the app stops anyway and says so; a second Ctrl-C stops at once, also while the
 * pipeline is going to NULL.
 *
 * For every frame of every source it prints
 *
 *   source <source_id> frame <frame_num>: objects <n> (person: <p>, cars: <c>)
 *
 * counted before the tiler (which merges the batch into one frame). The class ids of "person" and "car" come from
 * the PGIE config's label file; a label the file doesn't have is counted as 0.
 *
 * SPDX-License-Identifier: MIT */
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <rds/rds_compat.h>
/* YAML PGIE configs are read for their label file with the YAML reader rdsinfer uses, an in-tree helper that is not
 * installed: an app built outside this tree uses a key-file config, or its own YAML reader. */
#if RDS_TEST3_HAVE_YAML
#include "rds-keyfile-yaml.h"
#endif

#define MUXER_WIDTH 1280
#define MUXER_HEIGHT 720
#define MUXER_BATCH_TIMEOUT_USEC 40000
#define TILED_WIDTH 1280
#define TILED_HEIGHT 720
/* how long Ctrl-C waits for the EOS */
#define EOS_TIMEOUT_S 10

#ifndef RDS_TEST3_SOURCE_DIR
#define RDS_TEST3_SOURCE_DIR "."
#endif
#define DEFAULT_PGIE_CONFIG RDS_TEST3_SOURCE_DIR "/rds_test3_pgie_config.yml"

/* DeepStream factory names, as a DeepStream app would write them, in pipeline order; the tracker only with
 * --tracker-config, and fakesink instead of the display sink with --no-display. */
static const gchar *all_factories[] = {
  "nvstreammux", "nvinfer", "nvtracker", "nvmultistreamtiler", "nvvideoconvert", "nvdsosd", "nveglglessink",
};

#define MAX_FACTORIES G_N_ELEMENTS (all_factories)

/* The factories of this run into @names; returns how many. */
static guint
pipeline_factories (gboolean tracker, gboolean display, const gchar ** names)
{
  guint n = 0;

  for (gsize i = 0; i < MAX_FACTORIES; i++) {
    if (!tracker && strcmp (all_factories[i], "nvtracker") == 0)
      continue;
    names[n++] = !display && strcmp (all_factories[i], "nveglglessink") == 0 ? "fakesink" : all_factories[i];
  }
  return n;
}

/* Class ids counted per frame; -1 when the label file doesn't name the class. */
typedef struct
{
  gint person;
  gint car;
} CountedClasses;

/* The [property] group of the nvinfer config at @path: YAML for .yml/.yaml, a key file otherwise. */
static GKeyFile *
read_pgie_config (const gchar * path, GError ** error)
{
  if (g_str_has_suffix (path, ".yml") || g_str_has_suffix (path, ".yaml")) {
#if RDS_TEST3_HAVE_YAML
    return rds_keyfile_from_yaml (path, error);
#else
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOSYS, "%s: built without YAML support", path);
    return NULL;
#endif
  }

  GKeyFile *kf = g_key_file_new ();

  if (!g_key_file_load_from_file (kf, path, G_KEY_FILE_NONE, error)) {
    g_key_file_unref (kf);
    return NULL;
  }
  return kf;
}

/* Looks up "person" and "car" in the label file of the PGIE config at @pgie_config. The label file is read as
 * rdsinfer reads it: labelfile-path relative to the config file's directory, one label per line (or all on one
 * ';'-separated line), whitespace around labels ignored; the line number is the class id, and only the first
 * num-detected-classes labels count. Without a label file or num-detected-classes both stay -1 (counted as 0), with
 * a warning. */
static CountedClasses
lookup_counted_classes (const gchar * pgie_config)
{
  CountedClasses ids = { -1, -1 };
  GError *err = NULL;
  GKeyFile *kf = read_pgie_config (pgie_config, &err);
  gchar *label_path = NULL, *text = NULL;
  gint num_classes = 0;

  if (kf) {
    label_path = g_key_file_get_string (kf, "property", "labelfile-path", NULL);
    num_classes = g_key_file_get_integer (kf, "property", "num-detected-classes", NULL);
  }
  if (label_path && *label_path && !g_path_is_absolute (label_path)) {
    gchar *config_abs = g_canonicalize_filename (pgie_config, NULL);
    gchar *config_dir = g_path_get_dirname (config_abs);
    gchar *abs = g_canonicalize_filename (label_path, config_dir);

    g_free (config_abs);
    g_free (config_dir);
    g_free (label_path);
    label_path = abs;
  }
  if (num_classes <= 0) {
    g_printerr ("Warning: %s has no num-detected-classes: person and car counts stay 0\n", pgie_config);
  } else if (label_path && *label_path && g_file_get_contents (label_path, &text, NULL, &err)) {
    const gchar *start = g_str_has_prefix (text, "\xEF\xBB\xBF") ? text + 3 : text;  /* UTF-8 byte order mark */
    gchar **labels = g_strsplit (start, "\n", -1);
    guint n = g_strv_length (labels);

    for (guint i = 0; i < n; i++)
      g_strstrip (labels[i]);
    while (n > 0 && !*labels[n - 1]) {
      g_free (labels[n - 1]);
      labels[--n] = NULL;
    }
    if (n == 1 && strchr (labels[0], ';')) {
      gchar **split = g_strsplit (labels[0], ";", -1);

      g_strfreev (labels);
      labels = split;
      n = g_strv_length (labels);
      for (guint i = 0; i < n; i++)
        g_strstrip (labels[i]);
      if (n > 0 && !*labels[n - 1]) {
        g_free (labels[n - 1]);
        labels[--n] = NULL;
      }
    }
    for (guint i = 0; i < n && i < (guint) num_classes; i++) {
      const gchar *label = labels[i];

      if (ids.person < 0 && strcmp (label, "person") == 0)
        ids.person = (gint) i;
      else if (ids.car < 0 && strcmp (label, "car") == 0)
        ids.car = (gint) i;
    }
    g_strfreev (labels);
  } else {
    g_printerr ("Warning: no labels from %s (%s): person and car counts stay 0\n", pgie_config,
        err ? err->message : "no labelfile-path");
  }
  g_clear_error (&err);
  g_free (text);
  g_free (label_path);
  if (kf)
    g_key_file_unref (kf);
  return ids;
}

/* Before the tiler (it merges the batch into one frame meta): one line per frame with its object counts. */
static GstPadProbeReturn
count_objects_probe (GstPad * pad G_GNUC_UNUSED, GstPadProbeInfo * info, gpointer user_data)
{
  const CountedClasses *ids = user_data;
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  if (batch_meta == NULL)
    return GST_PAD_PROBE_OK;
  nvds_acquire_meta_lock (batch_meta);
  for (NvDsMetaList * l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = l_frame->data;
    guint persons = 0, cars = 0;

    for (NvDsMetaList * l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = l_obj->data;

      /* -1: the label file doesn't name the class, so nothing is counted for it */
      if (ids->person >= 0 && obj_meta->class_id == ids->person)
        persons++;
      else if (ids->car >= 0 && obj_meta->class_id == ids->car)
        cars++;
    }
    g_print ("source %u frame %d: objects %u (person: %u, cars: %u)\n", frame_meta->source_id,
        frame_meta->frame_num, frame_meta->num_obj_meta, persons, cars);
  }
  nvds_release_meta_lock (batch_meta);
  return GST_PAD_PROBE_OK;
}

typedef struct
{
  GMainLoop *loop;
  gboolean error;
  GstElement *pipeline;
  GPtrArray *mux_pads;          /* the muxer's sink pad of each source */
  gint stopping;                /* atomic: Ctrl-C came */
  guint eos_timeout;
} BusState;

static void
on_pad_added (GstElement * srcbin, GstPad * pad, gpointer user_data)
{
  GstPad *mux_pad = GST_PAD (user_data);
  GstCaps *caps = gst_pad_get_current_caps (pad);

  if (caps == NULL)
    caps = gst_pad_query_caps (pad, NULL);
  /* a vsrc_%u pad (video, in memory:RDS); also after Ctrl-C (the source's EOS ends it then), as an unlinked pad
   * would fail upstream with not-linked */
  if (g_str_has_prefix (gst_structure_get_name (gst_caps_get_structure (caps, 0)), "video/")
      && !gst_pad_is_linked (mux_pad)) {
    if (gst_pad_link (pad, mux_pad) != GST_PAD_LINK_OK)
      g_printerr ("Failed to link %s to the stream muxer\n", GST_ELEMENT_NAME (srcbin));
  }
  gst_caps_unref (caps);
}

static gboolean
on_eos_timeout (gpointer user_data)
{
  BusState *state = user_data;

  g_printerr ("No end of stream within %d s: stopping anyway\n", EOS_TIMEOUT_S);
  state->eos_timeout = 0;
  g_main_loop_quit (state->loop);
  return G_SOURCE_REMOVE;
}

/* Ctrl-C: an EOS from every source; a source with no stream yet (its muxer pad unlinked) is stopped already, so its
 * muxer pad gets the EOS directly. A second Ctrl-C stops at once. */
static gboolean
on_sigint (gpointer user_data)
{
  BusState *state = user_data;

  if (!g_atomic_int_compare_and_exchange (&state->stopping, FALSE, TRUE)) {
    g_main_loop_quit (state->loop);
    return G_SOURCE_CONTINUE;
  }
  g_print ("Stopping: sending EOS\n");
  gst_element_send_event (state->pipeline, gst_event_new_eos ());
  for (guint i = 0; i < state->mux_pads->len; i++) {
    GstPad *mux_pad = g_ptr_array_index (state->mux_pads, i);

    if (!gst_pad_is_linked (mux_pad))
      gst_pad_send_event (mux_pad, gst_event_new_eos ());
  }
  state->eos_timeout = g_timeout_add_seconds (EOS_TIMEOUT_S, on_eos_timeout, state);
  return G_SOURCE_CONTINUE;
}

/* The reconnection messages of the sources (their source-id is the URI's position). */
static void
print_reconnection (GstMessage * msg)
{
  const GstStructure *st = gst_message_get_structure (msg);
  gint id = -1, attempt = 0;
  guint uid;

  if (gst_structure_has_name (st, "rds-reconnect")) {
    gst_structure_get_int (st, "source-id", &id);
    gst_structure_get_int (st, "attempt", &attempt);
    g_print ("source %d: reconnecting, attempt %d (%s)\n", id, attempt, gst_structure_get_string (st, "reason"));
  } else if (gst_structure_has_name (st, "rds-reconnected")) {
    gst_structure_get_int (st, "source-id", &id);
    g_print ("source %d: reconnected\n", id);
  } else if (gst_structure_has_name (st, "attempt-exceeded")) {
    gst_structure_get_uint (st, "source-id", &uid);
    g_print ("source %u: gave up reconnecting\n", uid);
  }
}

static gboolean
on_bus_message (GstBus * bus G_GNUC_UNUSED, GstMessage * msg, gpointer user_data)
{
  BusState *state = user_data;

  switch (GST_MESSAGE_TYPE (msg)) {
    case GST_MESSAGE_EOS:
      g_print ("End of stream\n");
      g_main_loop_quit (state->loop);
      break;
    case GST_MESSAGE_ELEMENT:
      print_reconnection (msg);
      break;
    case GST_MESSAGE_ERROR:{
      GError *err = NULL;
      gchar *dbg = NULL;
      gst_message_parse_error (msg, &err, &dbg);
      g_printerr ("Error from %s: %s\n%s\n", GST_MESSAGE_SRC_NAME (msg), err->message, dbg ? dbg : "");
      g_clear_error (&err);
      g_free (dbg);
      state->error = TRUE;
      g_main_loop_quit (state->loop);
      break;
    }
    default:
      break;
  }
  return TRUE;
}

static void
unref_mux_pad (gpointer data, GClosure * closure G_GNUC_UNUSED)
{
  gst_object_unref (data);
}

/* The element made from DeepStream factory @name, or NULL if this run has none. */
static GstElement *
element_for (const gchar ** factories, GstElement ** elements, guint n, const gchar * name)
{
  for (guint i = 0; i < n; i++)
    if (strcmp (factories[i], name) == 0)
      return elements[i];
  return NULL;
}

/* The RTSP options: negative (attempts: G_MININT) when not given. */
typedef struct
{
  gint reconnect_interval;
  gint reconnect_attempts;
  gint rtp_protocol;
} SourceOptions;

static int
run_pipeline (gchar ** uris, guint num_sources, const gchar * pgie_config, const gchar * tracker_config,
    gboolean display, const SourceOptions * opts)
{
  const gchar *factories[MAX_FACTORIES];
  guint n_factories = pipeline_factories (tracker_config != NULL, display, factories);
  GstElement *elements[MAX_FACTORIES];
  GstElement *pipeline = gst_pipeline_new ("rds-test3");

  for (guint i = 0; i < n_factories; i++) {
    elements[i] = gst_element_factory_make (factories[i], NULL);
    if (elements[i] == NULL) {
      const gchar *name = rds_compat_map_factory_name (factories[i]);

      g_printerr ("Element %s (%s) is not available\n", factories[i], name);
      /* the optional plugins: the meson setup log says which dependency was missing */
      if (g_strcmp0 (name, "rdsosd") == 0)
        g_printerr ("Redstream was built without rdsosd (-Dosd: needs cairo and pangocairo)\n");
      else if (g_strcmp0 (name, "rdsinfer") == 0)
        g_printerr ("Redstream was built without rdsinfer (-Dinfer: needs MIGraphX, libyaml and json-glib)\n");
      else if (g_strcmp0 (name, "rdstracker") == 0)
        g_printerr ("Redstream was built without rdstracker (-Dtracker: needs libyaml)\n");
      else if (g_str_has_prefix (name, "rds"))
        g_printerr ("Is GST_PLUGIN_PATH_1_0 set to the build tree's gst/ directory?\n");
      gst_object_unref (pipeline);
      return 1;
    }
    gst_bin_add (GST_BIN (pipeline), elements[i]);
  }
  GstElement *mux = element_for (factories, elements, n_factories, "nvstreammux");
  GstElement *pgie = element_for (factories, elements, n_factories, "nvinfer");
  GstElement *tracker = element_for (factories, elements, n_factories, "nvtracker");
  GstElement *tiler = element_for (factories, elements, n_factories, "nvmultistreamtiler");
  guint rows = (guint) sqrt (num_sources);
  guint cols = (guint) ceil (1.0 * num_sources / rows);

  g_object_set (mux, "batch-size", num_sources, "width", MUXER_WIDTH, "height", MUXER_HEIGHT,
      "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);
  g_object_set (pgie, "config-file-path", pgie_config, "batch-size", num_sources, NULL);
  if (tracker)
    g_object_set (tracker, "ll-config-file", tracker_config, NULL);
  g_object_set (tiler, "rows", rows, "columns", cols, "width", TILED_WIDTH, "height", TILED_HEIGHT, NULL);
  if (!display)
    g_object_set (element_for (factories, elements, n_factories, "fakesink"), "sync", FALSE, NULL);

  CountedClasses ids = lookup_counted_classes (pgie_config);
  GstPad *count_pad = gst_element_get_static_pad (tracker ? tracker : pgie, "src");

  gst_pad_add_probe (count_pad, GST_PAD_PROBE_TYPE_BUFFER, count_objects_probe, &ids, NULL);
  gst_object_unref (count_pad);

  BusState state = { g_main_loop_new (NULL, FALSE), FALSE, pipeline,
    g_ptr_array_new_with_free_func (gst_object_unref), FALSE, 0
  };

  for (guint i = 0; i < num_sources; i++) {
    GstElement *src = gst_element_factory_make ("nvurisrcbin", NULL);

    if (src == NULL) {
      g_printerr ("Element nvurisrcbin (%s) is not available\n", rds_compat_map_factory_name ("nvurisrcbin"));
      g_ptr_array_unref (state.mux_pads);
      g_main_loop_unref (state.loop);
      gst_object_unref (pipeline);
      return 1;
    }
    gchar *pad_name = g_strdup_printf ("sink_%u", i);
    GstPad *mux_pad = gst_element_request_pad_simple (mux, pad_name);

    g_free (pad_name);
    g_ptr_array_add (state.mux_pads, gst_object_ref (mux_pad));
    g_object_set (src, "uri", uris[i], "source-id", (gint) i, NULL);
    if (opts->reconnect_interval >= 0)
      g_object_set (src, "rtsp-reconnect-interval", (guint) opts->reconnect_interval, NULL);
    if (opts->reconnect_attempts != G_MININT)
      g_object_set (src, "rtsp-reconnect-attempts", opts->reconnect_attempts, NULL);
    if (opts->rtp_protocol >= 0)
      g_object_set (src, "select-rtp-protocol", opts->rtp_protocol, NULL);
    g_signal_connect_data (src, "pad-added", G_CALLBACK (on_pad_added), mux_pad, unref_mux_pad, 0);
    gst_bin_add (GST_BIN (pipeline), src);
  }
  for (guint i = 0; i + 1 < n_factories; i++) {
    if (!gst_element_link (elements[i], elements[i + 1])) {
      g_printerr ("Failed to link %s -> %s\n", factories[i], factories[i + 1]);
      g_ptr_array_unref (state.mux_pads);
      g_main_loop_unref (state.loop);
      gst_object_unref (pipeline);
      return 1;
    }
  }

  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  guint watch = gst_bus_add_watch (bus, on_bus_message, &state);
  guint sigint = g_unix_signal_add (SIGINT, on_sigint, &state);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  g_main_loop_run (state.loop);
  /* from here a Ctrl-C ends the process, also should stopping hang */
  g_source_remove (sigint);
  gst_element_set_state (pipeline, GST_STATE_NULL);

  if (state.eos_timeout)
    g_source_remove (state.eos_timeout);
  g_source_remove (watch);
  g_ptr_array_unref (state.mux_pads);
  g_main_loop_unref (state.loop);
  gst_object_unref (pipeline);
  return state.error ? 1 : 0;
}

int
main (int argc, char **argv)
{
  gboolean dry_run = FALSE, no_display = FALSE;
  SourceOptions opts = { -1, G_MININT, -1 };
  gchar *pgie_config = NULL, *tracker_config = NULL;
  gchar **uris = NULL;
  GOptionEntry entries[] = {
    {"dry-run", 0, 0, G_OPTION_ARG_NONE, &dry_run, "Print the element mapping and exit", NULL},
    {"no-display", 0, 0, G_OPTION_ARG_NONE, &no_display, "Use fakesink (sync=false) instead of the display sink",
        NULL},
    {"pgie-config", 0, 0, G_OPTION_ARG_FILENAME, &pgie_config,
        "Primary inference config file (default: " DEFAULT_PGIE_CONFIG ")", "FILE"},
    {"tracker-config", 0, 0, G_OPTION_ARG_FILENAME, &tracker_config,
        "Add a tracker after the primary detector, with this low-level tracker config file", "FILE"},
    {"rtsp-reconnect-interval", 0, 0, G_OPTION_ARG_INT, &opts.reconnect_interval,
        "RTSP: seconds without data before reconnecting (default 0: no reconnection, as in DeepStream 8.0)", "S"},
    {"rtsp-reconnect-attempts", 0, 0, G_OPTION_ARG_INT, &opts.reconnect_attempts,
        "RTSP: reconnection attempts before giving up (default -1: unlimited)", "N"},
    {"select-rtp-protocol", 0, 0, G_OPTION_ARG_INT, &opts.rtp_protocol,
        "RTSP: the sources' select-rtp-protocol: 0, 1 (UDP), 2 (UDP multicast), 4 (TCP), 7 (any of the three, "
        "nvurisrcbin's default), 10 (HTTP), 20 (TLS)", "P"},
    {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &uris, NULL, "URI..."},
    {NULL, 0, 0, 0, NULL, NULL, NULL},
  };
  GOptionContext *ctx = g_option_context_new ("- Redstream multi-stream detection sample");
  GError *err = NULL;
  int ret;

  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_add_group (ctx, gst_init_get_option_group ());
  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    g_printerr ("%s\n", err->message);
    g_clear_error (&err);
    g_option_context_free (ctx);
    return 1;
  }
  g_option_context_free (ctx);

  guint num_sources = uris ? g_strv_length (uris) : 0;
  if (num_sources == 0) {
    g_printerr ("Usage: %s [--dry-run] [--no-display] [--pgie-config FILE] [--tracker-config FILE] "
        "[--rtsp-reconnect-interval S] [--rtsp-reconnect-attempts N] [--select-rtp-protocol P] URI [URI...]\n",
        argv[0]);
    return 1;
  }
  if (opts.rtp_protocol >= 0 && opts.rtp_protocol != 0 && opts.rtp_protocol != 1 && opts.rtp_protocol != 2
      && opts.rtp_protocol != 4 && opts.rtp_protocol != 7 && opts.rtp_protocol != 10 && opts.rtp_protocol != 20) {
    g_printerr ("--select-rtp-protocol: %d is none of 0, 1, 2, 4, 7, 10 and 20\n", opts.rtp_protocol);
    g_strfreev (uris);
    g_free (pgie_config);
    g_free (tracker_config);
    return 1;
  }
  if (tracker_config && !*tracker_config) {
    g_printerr ("--tracker-config needs a file\n");
    g_strfreev (uris);
    g_free (pgie_config);
    g_free (tracker_config);
    return 1;
  }

  if (dry_run) {
    const gchar *factories[MAX_FACTORIES];
    guint n = pipeline_factories (tracker_config != NULL, !no_display, factories);

    g_print ("sources: %u\n", num_sources);
    g_print ("pgie config: %s\n", pgie_config ? pgie_config : DEFAULT_PGIE_CONFIG);
    g_print ("nvurisrcbin -> %s\n", rds_compat_map_factory_name ("nvurisrcbin"));
    for (guint i = 0; i < n; i++)
      g_print ("%s -> %s\n", factories[i], rds_compat_map_factory_name (factories[i]));
    ret = 0;
  } else {
    ret = run_pipeline (uris, num_sources, pgie_config ? pgie_config : DEFAULT_PGIE_CONFIG, tracker_config,
        !no_display, &opts);
  }

  g_strfreev (uris);
  g_free (pgie_config);
  g_free (tracker_config);
  return ret;
}
