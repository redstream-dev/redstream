/* rds-test2: a detector, a tracker and a secondary classifier, in the style of a DeepStream application.
 * Built against Redstream's compat header, so DeepStream factory names are remapped at runtime.
 *
 *   rds-test2 [--dry-run] [--no-display] [--pgie-config FILE] [--sgie-config FILE] [--tracker-config FILE]
 *             URI [URI...]
 *
 * The pipeline: one nvurisrcbin per URI (its source-id the URI's position) -> nvstreammux -> nvinfer, the primary
 * detector (--pgie-config, default rds_test2_pgie_config.yml) -> nvtracker (--tracker-config, default the NvSORT
 * config in samples/tracker) -> nvinfer, a secondary classifier on the vehicles the detector found (--sgie-config,
 * default rds_test2_sgie_config.yml) -> nvmultistreamtiler -> nvvideoconvert -> nvdsosd -> the display sink, or
 * "fakesink sync=false" with --no-display (as fast as it goes). The defaults are in this sample's source directory.
 *
 * For every frame of every source it prints, before the tiler (which merges the batch into one frame),
 *
 *   source <source_id> frame <frame_num>: objects <n> (vehicles: <v>, labelled: <l>)
 *
 * where vehicles are the objects whose class is car, motorcycle, bus or truck in the PGIE config's label file, and
 * labelled the objects that carry a classifier meta from the secondary classifier (its gie-unique-id, read from the
 * SGIE config). The classifier keeps a tracked object's result and attaches it again on later frames, so a labelled
 * vehicle stays labelled while it is tracked. At the end it prints the totals and the five most frequent labels, each
 * counted once per frame an object carries it (object-frames, not distinct objects):
 *
 *   frames <f>, objects <n>, vehicles <v>, labelled <l>
 *   label <name>: <count>
 *
 * Ctrl-C (SIGINT) stops with an EOS: the sources end their streams, and the muxer ends each source that never had a
 * stream (no vsrc_0 yet: such a source never sends an EOS, so the app treats it as already stopped). Should the EOS
 * still not come within 10 s, the app stops anyway and says so; a second Ctrl-C stops at once, also while the
 * pipeline is going to NULL.
 *
 * SPDX-License-Identifier: MIT */
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <rds/rds_compat.h>
/* YAML configs are read for their label file and ids with the YAML reader rdsinfer uses, an in-tree helper that is
 * not installed: an app built outside this tree uses key-file configs, or its own YAML reader. */
#if RDS_TEST2_HAVE_YAML
#include "rds-keyfile-yaml.h"
#endif

#define MUXER_WIDTH 1280
#define MUXER_HEIGHT 720
#define MUXER_BATCH_TIMEOUT_USEC 40000
#define TILED_WIDTH 1280
#define TILED_HEIGHT 720
#define TOP_LABELS 5
/* how long Ctrl-C waits for the EOS */
#define EOS_TIMEOUT_S 10

#ifndef RDS_TEST2_SOURCE_DIR
#define RDS_TEST2_SOURCE_DIR "."
#endif
#define DEFAULT_PGIE_CONFIG RDS_TEST2_SOURCE_DIR "/rds_test2_pgie_config.yml"
#define DEFAULT_SGIE_CONFIG RDS_TEST2_SOURCE_DIR "/rds_test2_sgie_config.yml"
#define DEFAULT_TRACKER_CONFIG RDS_TEST2_SOURCE_DIR "/../tracker/config_tracker_NvSORT.yml"

/* DeepStream factory names, as a DeepStream app would write them, in pipeline order; fakesink instead of the display
 * sink with --no-display. The two nvinfer are told apart by position: the first is the PGIE. */
static const gchar *all_factories[] = {
  "nvstreammux", "nvinfer", "nvtracker", "nvinfer", "nvmultistreamtiler", "nvvideoconvert", "nvdsosd",
  "nveglglessink",
};

#define N_FACTORIES G_N_ELEMENTS (all_factories)
enum
{ MUX, PGIE, TRACKER, SGIE, TILER, CONVERT, OSD, SINK };

static const gchar *
factory_name (guint i, gboolean display)
{
  return i == SINK && !display ? "fakesink" : all_factories[i];
}

/* The [property] group of the nvinfer config at @path: YAML for .yml/.yaml, a key file otherwise. */
static GKeyFile *
read_infer_config (const gchar * path, GError ** error)
{
  if (g_str_has_suffix (path, ".yml") || g_str_has_suffix (path, ".yaml")) {
#if RDS_TEST2_HAVE_YAML
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

/* What the count probe needs from the configs. */
typedef struct
{
  gboolean *is_vehicle;         /* per PGIE class id */
  guint num_classes;
  gint sgie_id;                 /* the classifier's gie-unique-id */
  /* totals, written by the probe (one streaming thread), read after the pipeline stopped */
  guint64 frames, objects, vehicles, labelled;
  GHashTable *labels;           /* label -> guint count, owned */
} Counts;

/* The vehicle classes of the PGIE config at @pgie_config, by name in its label file: labelfile-path relative to the
 * config file's directory, one label per line (or all on one ';'-separated line), whitespace around labels ignored;
 * the line number is the class id, and only the first num-detected-classes labels count. Without them nothing is
 * a vehicle, with a warning. */
static void
lookup_vehicles (const gchar * pgie_config, Counts * counts)
{
  static const gchar *vehicles[] = { "car", "motorcycle", "bus", "truck" };
  GError *err = NULL;
  GKeyFile *kf = read_infer_config (pgie_config, &err);
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
  counts->num_classes = num_classes > 0 ? (guint) num_classes : 0;
  counts->is_vehicle = g_new0 (gboolean, counts->num_classes + 1);
  if (num_classes <= 0) {
    g_printerr ("Warning: %s has no num-detected-classes: no vehicles counted\n", pgie_config);
  } else if (label_path && *label_path && g_file_get_contents (label_path, &text, NULL, &err)) {
    const gchar *start = g_str_has_prefix (text, "\xEF\xBB\xBF") ? text + 3 : text;  /* UTF-8 byte order mark */
    gchar **labels = g_strsplit (start, "\n", -1);
    guint n = g_strv_length (labels);

    /* as rds-test3 reads them: trailing empty lines dropped, one ';'-separated line split */
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
    for (guint i = 0; i < n && i < counts->num_classes; i++)
      for (guint v = 0; v < G_N_ELEMENTS (vehicles); v++)
        if (strcmp (labels[i], vehicles[v]) == 0)
          counts->is_vehicle[i] = TRUE;
    g_strfreev (labels);
  } else {
    g_printerr ("Warning: no labels from %s (%s): no vehicles counted\n", pgie_config,
        err ? err->message : "no labelfile-path");
  }
  g_clear_error (&err);
  g_free (text);
  g_free (label_path);
  if (kf)
    g_key_file_unref (kf);
}

/* The gie-unique-id of the SGIE config at @sgie_config, or -1 (nothing counts as labelled), with a warning. */
static gint
lookup_sgie_id (const gchar * sgie_config)
{
  GError *err = NULL;
  GKeyFile *kf = read_infer_config (sgie_config, &err);
  gint id = -1;

  if (kf) {
    id = g_key_file_get_integer (kf, "property", "gie-unique-id", &err);
    g_key_file_unref (kf);
  }
  if (err || id < 0) {
    g_printerr ("Warning: no gie-unique-id from %s (%s): no labelled objects counted\n", sgie_config,
        err ? err->message : "negative");
    id = -1;
  }
  g_clear_error (&err);
  return id;
}

/* After the classifier, before the tiler: one line per frame with its counts, and the totals. */
static GstPadProbeReturn
count_probe (GstPad * pad G_GNUC_UNUSED, GstPadProbeInfo * info, gpointer user_data)
{
  Counts *counts = user_data;
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  if (batch_meta == NULL)
    return GST_PAD_PROBE_OK;
  nvds_acquire_meta_lock (batch_meta);
  for (NvDsMetaList * l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = l_frame->data;
    guint vehicles = 0, labelled = 0;

    for (NvDsMetaList * l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
      NvDsObjectMeta *obj_meta = l_obj->data;
      gboolean has_label = FALSE;

      if (obj_meta->class_id >= 0 && (guint) obj_meta->class_id < counts->num_classes
          && counts->is_vehicle[obj_meta->class_id])
        vehicles++;
      for (NvDsMetaList * l_cls = obj_meta->classifier_meta_list; l_cls != NULL; l_cls = l_cls->next) {
        NvDsClassifierMeta *cls_meta = l_cls->data;

        if (cls_meta->unique_component_id != counts->sgie_id)
          continue;
        for (NvDsMetaList * l_label = cls_meta->label_info_list; l_label != NULL; l_label = l_label->next) {
          NvDsLabelInfo *label = l_label->data;
          const gchar *text = label->pResult_label ? label->pResult_label : label->result_label;
          guint *n = g_hash_table_lookup (counts->labels, text);

          /* a copy of the label only the first time it is seen */
          if (n == NULL) {
            n = g_new0 (guint, 1);
            g_hash_table_insert (counts->labels, g_strdup (text), n);
          }
          (*n)++;
          has_label = TRUE;
        }
      }
      if (has_label)
        labelled++;
    }
    g_print ("source %u frame %d: objects %u (vehicles: %u, labelled: %u)\n", frame_meta->source_id,
        frame_meta->frame_num, frame_meta->num_obj_meta, vehicles, labelled);
    counts->frames++;
    counts->objects += frame_meta->num_obj_meta;
    counts->vehicles += vehicles;
    counts->labelled += labelled;
  }
  nvds_release_meta_lock (batch_meta);
  return GST_PAD_PROBE_OK;
}

static gint
by_count_desc (gconstpointer a, gconstpointer b, gpointer user_data)
{
  GHashTable *labels = user_data;
  guint ca = *(const guint *) g_hash_table_lookup (labels, *(const gchar * const *) a);
  guint cb = *(const guint *) g_hash_table_lookup (labels, *(const gchar * const *) b);

  return ca < cb ? 1 : ca > cb ? -1 : strcmp (*(const gchar * const *) a, *(const gchar * const *) b);
}

static void
print_totals (const Counts * counts)
{
  guint n = 0;
  gpointer *keys = g_hash_table_get_keys_as_array (counts->labels, &n);

  g_print ("frames %" G_GUINT64_FORMAT ", objects %" G_GUINT64_FORMAT ", vehicles %" G_GUINT64_FORMAT
      ", labelled %" G_GUINT64_FORMAT "\n", counts->frames, counts->objects, counts->vehicles, counts->labelled);
  g_qsort_with_data (keys, n, sizeof (gpointer), by_count_desc, counts->labels);
  for (guint i = 0; i < n && i < TOP_LABELS; i++)
    g_print ("label %s: %u\n", (const gchar *) keys[i],
        *(const guint *) g_hash_table_lookup (counts->labels, keys[i]));
  g_free (keys);
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

static void
unref_mux_pad (gpointer data, GClosure * closure G_GNUC_UNUSED)
{
  gst_object_unref (data);
}

static int
run_pipeline (gchar ** uris, guint num_sources, const gchar * pgie_config, const gchar * sgie_config,
    const gchar * tracker_config, gboolean display)
{
  GstElement *elements[N_FACTORIES];
  GstElement *pipeline = gst_pipeline_new ("rds-test2");

  for (guint i = 0; i < N_FACTORIES; i++) {
    const gchar *factory = factory_name (i, display);

    elements[i] = gst_element_factory_make (factory, NULL);
    if (elements[i] == NULL) {
      const gchar *name = rds_compat_map_factory_name (factory);

      g_printerr ("Element %s (%s) is not available\n", factory, name);
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
  guint rows = (guint) sqrt (num_sources);
  guint cols = (guint) ceil (1.0 * num_sources / rows);

  g_object_set (elements[MUX], "batch-size", num_sources, "width", MUXER_WIDTH, "height", MUXER_HEIGHT,
      "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);
  g_object_set (elements[PGIE], "config-file-path", pgie_config, "batch-size", num_sources, NULL);
  g_object_set (elements[TRACKER], "ll-config-file", tracker_config, NULL);
  /* the SGIE's batch-size (objects per inference run) stays the config's */
  g_object_set (elements[SGIE], "config-file-path", sgie_config, NULL);
  g_object_set (elements[TILER], "rows", rows, "columns", cols, "width", TILED_WIDTH, "height", TILED_HEIGHT, NULL);
  if (!display)
    g_object_set (elements[SINK], "sync", FALSE, NULL);

  Counts counts = { 0 };
  counts.labels = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
  lookup_vehicles (pgie_config, &counts);
  counts.sgie_id = lookup_sgie_id (sgie_config);

  GstPad *count_pad = gst_element_get_static_pad (elements[SGIE], "src");

  gst_pad_add_probe (count_pad, GST_PAD_PROBE_TYPE_BUFFER, count_probe, &counts, NULL);
  gst_object_unref (count_pad);

  BusState state = { g_main_loop_new (NULL, FALSE), FALSE, pipeline,
    g_ptr_array_new_with_free_func (gst_object_unref), FALSE, 0
  };
  gboolean ok = TRUE;

  for (guint i = 0; ok && i < num_sources; i++) {
    GstElement *src = gst_element_factory_make ("nvurisrcbin", NULL);

    if (src == NULL) {
      g_printerr ("Element nvurisrcbin (%s) is not available\n", rds_compat_map_factory_name ("nvurisrcbin"));
      ok = FALSE;
      break;
    }
    gchar *pad_name = g_strdup_printf ("sink_%u", i);
    GstPad *mux_pad = gst_element_request_pad_simple (elements[MUX], pad_name);

    g_free (pad_name);
    if (mux_pad == NULL) {
      g_printerr ("No sink pad %u on the stream muxer\n", i);
      gst_object_unref (gst_object_ref_sink (src));
      ok = FALSE;
      break;
    }
    g_ptr_array_add (state.mux_pads, gst_object_ref (mux_pad));
    g_object_set (src, "uri", uris[i], "source-id", (gint) i, NULL);
    g_signal_connect_data (src, "pad-added", G_CALLBACK (on_pad_added), mux_pad, unref_mux_pad, 0);
    gst_bin_add (GST_BIN (pipeline), src);
  }
  for (guint i = 0; ok && i + 1 < N_FACTORIES; i++) {
    if (!gst_element_link (elements[i], elements[i + 1])) {
      g_printerr ("Failed to link %s -> %s\n", factory_name (i, display), factory_name (i + 1, display));
      ok = FALSE;
    }
  }

  if (ok) {
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
    print_totals (&counts);
  }

  g_ptr_array_unref (state.mux_pads);
  g_main_loop_unref (state.loop);
  gst_object_unref (pipeline);
  g_hash_table_unref (counts.labels);
  g_free (counts.is_vehicle);
  return ok && !state.error ? 0 : 1;
}

int
main (int argc, char **argv)
{
  gboolean dry_run = FALSE, no_display = FALSE;
  gchar *pgie_config = NULL, *sgie_config = NULL, *tracker_config = NULL;
  gchar **uris = NULL;
  GOptionEntry entries[] = {
    {"dry-run", 0, 0, G_OPTION_ARG_NONE, &dry_run, "Print the element mapping and the configs, and exit", NULL},
    {"no-display", 0, 0, G_OPTION_ARG_NONE, &no_display, "Use fakesink (sync=false) instead of the display sink",
        NULL},
    {"pgie-config", 0, 0, G_OPTION_ARG_FILENAME, &pgie_config,
        "Primary detector config file (default: " DEFAULT_PGIE_CONFIG ")", "FILE"},
    {"sgie-config", 0, 0, G_OPTION_ARG_FILENAME, &sgie_config,
        "Secondary classifier config file (default: " DEFAULT_SGIE_CONFIG ")", "FILE"},
    {"tracker-config", 0, 0, G_OPTION_ARG_FILENAME, &tracker_config,
        "Low-level tracker config file (default: " DEFAULT_TRACKER_CONFIG ")", "FILE"},
    {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &uris, NULL, "URI..."},
    {NULL, 0, 0, 0, NULL, NULL, NULL},
  };
  GOptionContext *ctx = g_option_context_new ("- Redstream detector, tracker and secondary classifier sample");
  GError *err = NULL;
  int ret = 1;

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
  const gchar *pgie = pgie_config ? pgie_config : DEFAULT_PGIE_CONFIG;
  const gchar *sgie = sgie_config ? sgie_config : DEFAULT_SGIE_CONFIG;
  const gchar *tracker = tracker_config ? tracker_config : DEFAULT_TRACKER_CONFIG;

  if (num_sources == 0) {
    g_printerr ("Usage: %s [--dry-run] [--no-display] [--pgie-config FILE] [--sgie-config FILE] "
        "[--tracker-config FILE] URI [URI...]\n", argv[0]);
  } else if (!*pgie || !*sgie || !*tracker) {
    g_printerr ("--pgie-config, --sgie-config and --tracker-config need a file\n");
  } else if (dry_run) {
    g_print ("sources: %u\n", num_sources);
    g_print ("pgie config: %s\n", pgie);
    g_print ("sgie config: %s\n", sgie);
    g_print ("tracker config: %s\n", tracker);
    g_print ("nvurisrcbin -> %s\n", rds_compat_map_factory_name ("nvurisrcbin"));
    for (guint i = 0; i < N_FACTORIES; i++)
      g_print ("%s -> %s\n", factory_name (i, !no_display), rds_compat_map_factory_name (factory_name (i,
                  !no_display)));
    ret = 0;
  } else {
    ret = run_pipeline (uris, num_sources, pgie, sgie, tracker, !no_display);
  }

  g_strfreev (uris);
  g_free (pgie_config);
  g_free (sgie_config);
  g_free (tracker_config);
  return ret;
}
