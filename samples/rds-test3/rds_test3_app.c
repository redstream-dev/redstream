/* rds-test3: multi-stream detection sample in the style of a DeepStream application.
 * Built against Redstream's compat header, so DeepStream factory names are remapped at runtime.
 *
 *   rds-test3 [--dry-run] [--no-display] [--pgie-config FILE] [--tracker-config FILE] URI [URI...]
 *
 * --pgie-config defaults to rds_test3_pgie_config.yml in this sample's source directory. --tracker-config adds a
 * tracker (nvtracker, with that low-level config file) after the primary detector; without it there is none, as in
 * DeepStream's test3. --no-display replaces the display sink with "fakesink sync=false" (as fast as it goes).
 *
 * For every frame of every source it prints
 *
 *   source <source_id> frame <frame_num>: objects <n> (person: <p>, cars: <c>)
 *
 * counted before the tiler (which merges the batch into one frame). The class ids of "person" and "car" come from
 * the PGIE config's label file; a label the file doesn't have is counted as 0.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <math.h>
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
} BusState;

static void
on_pad_added (GstElement * decodebin, GstPad * pad, gpointer user_data)
{
  GstPad *mux_pad = GST_PAD (user_data);
  GstCaps *caps = gst_pad_get_current_caps (pad);

  if (caps == NULL)
    caps = gst_pad_query_caps (pad, NULL);
  if (g_str_has_prefix (gst_structure_get_name (gst_caps_get_structure (caps, 0)), "video/")
      && !gst_pad_is_linked (mux_pad)) {
    if (gst_pad_link (pad, mux_pad) != GST_PAD_LINK_OK)
      g_printerr ("Failed to link %s to the stream muxer\n", GST_ELEMENT_NAME (decodebin));
  }
  gst_caps_unref (caps);
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

/* The element made from DeepStream factory @name, or NULL if this run has none. */
static GstElement *
element_for (const gchar ** factories, GstElement ** elements, guint n, const gchar * name)
{
  for (guint i = 0; i < n; i++)
    if (strcmp (factories[i], name) == 0)
      return elements[i];
  return NULL;
}

static int
run_pipeline (gchar ** uris, guint num_sources, const gchar * pgie_config, const gchar * tracker_config,
    gboolean display)
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

  for (guint i = 0; i < num_sources; i++) {
    GstElement *src = gst_element_factory_make ("uridecodebin", NULL);

    if (src == NULL) {
      g_printerr ("Element uridecodebin is not available\n");
      gst_object_unref (pipeline);
      return 1;
    }
    GstPad *mux_pad = gst_element_request_pad_simple (mux, "sink_%u");

    g_object_set (src, "uri", uris[i], NULL);
    g_signal_connect_data (src, "pad-added", G_CALLBACK (on_pad_added), mux_pad,
        (GClosureNotify) (void (*)(void)) gst_object_unref, 0);
    gst_bin_add (GST_BIN (pipeline), src);
  }
  for (guint i = 0; i + 1 < n_factories; i++) {
    if (!gst_element_link (elements[i], elements[i + 1])) {
      g_printerr ("Failed to link %s -> %s\n", factories[i], factories[i + 1]);
      gst_object_unref (pipeline);
      return 1;
    }
  }

  BusState state = { g_main_loop_new (NULL, FALSE), FALSE };
  GstBus *bus = gst_pipeline_get_bus (GST_PIPELINE (pipeline));
  guint watch = gst_bus_add_watch (bus, on_bus_message, &state);
  gst_object_unref (bus);

  gst_element_set_state (pipeline, GST_STATE_PLAYING);
  g_main_loop_run (state.loop);
  gst_element_set_state (pipeline, GST_STATE_NULL);

  g_source_remove (watch);
  g_main_loop_unref (state.loop);
  gst_object_unref (pipeline);
  return state.error ? 1 : 0;
}

int
main (int argc, char **argv)
{
  gboolean dry_run = FALSE, no_display = FALSE;
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
    g_printerr ("Usage: %s [--dry-run] [--no-display] [--pgie-config FILE] [--tracker-config FILE] URI [URI...]\n",
        argv[0]);
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
    for (guint i = 0; i < n; i++)
      g_print ("%s -> %s\n", factories[i], rds_compat_map_factory_name (factories[i]));
    ret = 0;
  } else {
    ret = run_pipeline (uris, num_sources, pgie_config ? pgie_config : DEFAULT_PGIE_CONFIG, tracker_config,
        !no_display);
  }

  g_strfreev (uris);
  g_free (pgie_config);
  g_free (tracker_config);
  return ret;
}
