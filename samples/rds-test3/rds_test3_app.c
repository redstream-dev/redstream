/* rds-test3: multi-stream detection sample in the style of a DeepStream application.
 * Built against Redstream's compat header, so DeepStream factory names are remapped at runtime.
 *
 *   rds-test3 [--dry-run] [--pgie-config FILE] [--tracker-config FILE] URI [URI...]
 *
 * --pgie-config defaults to rds_test3_pgie_config.yml in this sample's source directory. --tracker-config adds a
 * tracker (nvtracker, with that low-level config file) after the primary detector; without it there is none, as in
 * DeepStream's test3.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <rds/rds_compat.h>

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
 * --tracker-config. */
static const gchar *all_factories[] = {
  "nvstreammux", "nvinfer", "nvtracker", "nvmultistreamtiler", "nvvideoconvert", "nvdsosd", "nveglglessink",
};

#define MAX_FACTORIES G_N_ELEMENTS (all_factories)

/* The factories of this run into @names; returns how many. */
static guint
pipeline_factories (gboolean tracker, const gchar ** names)
{
  guint n = 0;

  for (gsize i = 0; i < MAX_FACTORIES; i++)
    if (tracker || strcmp (all_factories[i], "nvtracker") != 0)
      names[n++] = all_factories[i];
  return n;
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
run_pipeline (gchar ** uris, guint num_sources, const gchar * pgie_config, const gchar * tracker_config)
{
  const gchar *factories[MAX_FACTORIES];
  guint n_factories = pipeline_factories (tracker_config != NULL, factories);
  GstElement *elements[MAX_FACTORIES];
  GstElement *pipeline = gst_pipeline_new ("rds-test3");

  for (guint i = 0; i < n_factories; i++) {
    elements[i] = gst_element_factory_make (factories[i], NULL);
    if (elements[i] == NULL) {
      g_printerr ("Element %s (%s) is not available\n", factories[i], rds_compat_map_factory_name (factories[i]));
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
  gboolean dry_run = FALSE;
  gchar *pgie_config = NULL, *tracker_config = NULL;
  gchar **uris = NULL;
  GOptionEntry entries[] = {
    {"dry-run", 0, 0, G_OPTION_ARG_NONE, &dry_run, "Print the element mapping and exit", NULL},
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
    g_printerr ("Usage: %s [--dry-run] [--pgie-config FILE] [--tracker-config FILE] URI [URI...]\n", argv[0]);
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
    guint n = pipeline_factories (tracker_config != NULL, factories);

    g_print ("sources: %u\n", num_sources);
    g_print ("pgie config: %s\n", pgie_config ? pgie_config : DEFAULT_PGIE_CONFIG);
    for (guint i = 0; i < n; i++)
      g_print ("%s -> %s\n", factories[i], rds_compat_map_factory_name (factories[i]));
    ret = 0;
  } else {
    ret = run_pipeline (uris, num_sources, pgie_config ? pgie_config : DEFAULT_PGIE_CONFIG, tracker_config);
  }

  g_strfreev (uris);
  g_free (pgie_config);
  g_free (tracker_config);
  return ret;
}
