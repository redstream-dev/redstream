/* The pipeline pipeline_osd and pipeline_osd_soak run, as rds-test3 does with a tracker: four sources of the street
 * clip -> rdsvideodec -> rdsstreammux (batch 4, 1280x720) -> rdsinfer (YOLO11n, the rds-test3 sample's config) ->
 * rdstracker (the NvSORT sample config) -> rdsmultistreamtiler (2x2, 1280x720) -> rdsvideoconvert -> RGBA
 * memory:RDS -> rdsosd -> sink. The clip comes from tests/models/fetch-tracker-clip.sh (.cache/media), YOLO11n from
 * tests/models/fetch-yolo11n.sh (.cache/models); neither is committed, and the tests skip without them.
 * Header-only; include it from one source file per executable.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_OSD_PIPELINE_H
#define RDS_TEST_OSD_PIPELINE_H

#include <gst/gst.h>
#include <unistd.h>

#define YOLO RDS_TEST_CACHE_MODELS "/yolo11n.onnx"
#define LABELS RDS_TEST_CACHE_MODELS "/coco-labels.txt"
#define CLIP RDS_TEST_CACHE_MEDIA "/crosswalk-720p.h264"
#define PGIE_CONFIG RDS_TEST_SAMPLES "/rds-test3/rds_test3_pgie_config.yml"
#define NVSORT_CONFIG RDS_TEST_SAMPLES "/tracker/config_tracker_NvSORT.yml"
#define YOLO_HINT "run tests/models/fetch-yolo11n.sh"
#define CLIP_HINT "run tests/models/fetch-tracker-clip.sh"
/* The same directory as infer_engine's, pipeline_infer's and pipeline_tracker's YOLO11n cases, so they share the
 * cached programs. */
#define YOLO_DIR RDS_TEST_BUILD_DIR "/infer-engine-yolo"
#define WORK_DIR RDS_TEST_BUILD_DIR "/pipeline-osd"

#define SOURCES 4
#define CLIP_W 1280
#define CLIP_H 720
#define CLIP_FRAMES 1356
#define TILED_W 1280
#define TILED_H 720

/* FALSE (and the test skipped) when the clip or YOLO11n is missing. */
static gboolean
have_assets (void)
{
  static const gchar *files[][2] = { {CLIP, CLIP_HINT}, {YOLO, YOLO_HINT}, {LABELS, YOLO_HINT} };

  for (guint i = 0; i < G_N_ELEMENTS (files); i++) {
    if (!g_file_test (files[i][0], G_FILE_TEST_IS_REGULAR)) {
      gchar *msg = g_strdup_printf ("no %s (%s)", files[i][0], files[i][1]);

      g_test_skip (msg);
      g_free (msg);
      return FALSE;
    }
  }
  return TRUE;
}

/* The rds-test3 sample's PGIE config with its model and labels pointed at YOLO11n behind a symlink in YOLO_DIR, so
 * the programs are cached there and not next to the model. Written into WORK_DIR; returns its path. */
static gchar *
pgie_config (void)
{
  gchar *link = g_build_filename (YOLO_DIR, "yolo11n.onnx", NULL);
  gchar *path = g_build_filename (WORK_DIR, "pgie.yml", NULL);
  gchar *text = NULL, **lines;
  GString *out = g_string_new (NULL);
  guint replaced = 0;
  GError *err = NULL;

  g_assert_cmpint (g_mkdir_with_parents (YOLO_DIR, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
  if (!g_file_test (link, G_FILE_TEST_IS_SYMLINK))
    g_assert_cmpint (symlink (YOLO, link), ==, 0);
  g_assert_true (g_file_get_contents (PGIE_CONFIG, &text, NULL, &err));
  g_assert_no_error (err);
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    if (g_str_has_prefix (lines[i], "  onnx-file:")) {
      g_string_append_printf (out, "  onnx-file: %s\n", link);
      replaced++;
    } else if (g_str_has_prefix (lines[i], "  labelfile-path:")) {
      g_string_append (out, "  labelfile-path: " LABELS "\n");
      replaced++;
    } else if (lines[i + 1]) {
      g_string_append_printf (out, "%s\n", lines[i]);
    }
  }
  g_assert_cmpuint (replaced, ==, 2);
  g_assert_true (g_file_set_contents (path, out->str, (gssize) out->len, &err));
  g_assert_no_error (err);
  g_string_free (out, TRUE);
  g_strfreev (lines);
  g_free (text);
  g_free (link);
  return path;
}

/* The pipeline with @pgie: sources s0..s3 (appsrc for H.264 byte-stream with @appsrc, else filesrc of the clip),
 * elements named infer, trk, tiler, conv and osd, and @sink (a description, named k) at the end. */
static GstElement *
osd_pipeline_new (const gchar * pgie, gboolean appsrc, const gchar * sink)
{
  GString *desc = g_string_new (NULL);
  GstElement *pipe;
  GError *err = NULL;

  for (guint i = 0; i < SOURCES; i++) {
    if (appsrc)
      g_string_append_printf (desc, "appsrc name=s%u caps=video/x-h264,stream-format=byte-stream format=bytes ", i);
    else
      g_string_append_printf (desc, "filesrc name=s%u location=" CLIP " ", i);
    g_string_append_printf (desc, "! h264parse ! rdsvideodec ! m.sink_%u ", i);
  }
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u "
      "! rdsinfer name=infer config-file-path=%s batch-size=%u ! rdstracker name=trk ll-config-file=" NVSORT_CONFIG
      " ! rdsmultistreamtiler name=tiler rows=2 columns=2 width=%u height=%u ! rdsvideoconvert name=conv "
      "! video/x-raw(memory:RDS),format=RGBA ! rdsosd name=osd ! %s name=k", CLIP_W, CLIP_H, SOURCES, pgie, SOURCES,
      TILED_W, TILED_H, sink);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  return pipe;
}

/* Adds @cb as a buffer probe on pad @pad_name of element @el_name in @pipe. */
static void
add_probe (GstElement * pipe, const gchar * el_name, const gchar * pad_name, GstPadProbeCallback cb, gpointer data)
{
  GstElement *el = gst_bin_get_by_name (GST_BIN (pipe), el_name);
  GstPad *pad;

  g_assert_nonnull (el);
  pad = gst_element_get_static_pad (el, pad_name);
  g_assert_nonnull (pad);
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, cb, data, NULL);
  gst_object_unref (pad);
  gst_object_unref (el);
}

/* Waits for EOS on the bus; fails on an error or after @timeout. Returns the number of warnings posted before. */
static guint
wait_eos (GstElement * pipe, GstClockTime timeout)
{
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg;
  guint warnings = 0;

  while ((msg = gst_bus_timed_pop_filtered (bus, timeout,
              GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS)) != NULL) {
    GError *err = NULL;
    gchar *dbg = NULL;

    switch (GST_MESSAGE_TYPE (msg)) {
      case GST_MESSAGE_EOS:
        gst_message_unref (msg);
        gst_object_unref (bus);
        return warnings;
      case GST_MESSAGE_WARNING:
        gst_message_parse_warning (msg, &err, &dbg);
        g_test_message ("warning from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message,
            dbg ? dbg : "");
        warnings++;
        break;
      default:
        gst_message_parse_error (msg, &err, &dbg);
        g_error ("pipeline error from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message,
            dbg ? dbg : "");
    }
    g_clear_error (&err);
    g_free (dbg);
    gst_message_unref (msg);
  }
  g_error ("no EOS within %" GST_TIME_FORMAT, GST_TIME_ARGS (timeout));
  return warnings;
}

/* The MIOpen and comgr caches in the build directory (shared with infer_engine), so re-runs compile nothing. Call
 * before gst_init. */
static void
set_compile_caches (void)
{
  g_setenv ("MIOPEN_USER_DB_PATH", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_setenv ("MIOPEN_CUSTOM_CACHE_DIR", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_setenv ("AMD_COMGR_CACHE_DIR", RDS_TEST_BUILD_DIR "/comgr", FALSE);
}

#endif /* RDS_TEST_OSD_PIPELINE_H */
