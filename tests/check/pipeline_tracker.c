/* rdstracker end to end (gpu): a public street clip through the decoder, the mux, rdsinfer (YOLO11n, the rds-test3
 * sample's config) and rdstracker with the sample tracker configs, measured without ground truth (ID switches by
 * fragmentation, coverage, track length; rds-test-tracks.h), and a long four-source run (the soak, its own test
 * entry) that neither leaks host nor GPU memory.
 * The clip comes from tests/models/fetch-tracker-clip.sh (.cache/media), YOLO11n from tests/models/fetch-yolo11n.sh
 * (.cache/models); neither is committed. The clip test skips without them. Without YOLO11n the soak runs the
 * committed scale2 model with scripted objects added after it, and without the clip a generated one, so the leak
 * check always runs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"
#include "rds-test-soak.h"
#include "rds-test-tracks.h"
#include "rds-tracker-config.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <rds/rds.h>
#include <string.h>
#include <unistd.h>

#define YOLO RDS_TEST_CACHE_MODELS "/yolo11n.onnx"
#define LABELS RDS_TEST_CACHE_MODELS "/coco-labels.txt"
#define CLIP RDS_TEST_CACHE_MEDIA "/crosswalk-720p.h264"
#define PGIE_CONFIG RDS_TEST_SAMPLES "/rds-test3/rds_test3_pgie_config.yml"
#define NVSORT_CONFIG RDS_TEST_SAMPLES "/tracker/config_tracker_NvSORT.yml"
#define IOU_CONFIG RDS_TEST_SAMPLES "/tracker/config_tracker_IOU.yml"
#define YOLO_HINT "run tests/models/fetch-yolo11n.sh"
#define CLIP_HINT "run tests/models/fetch-tracker-clip.sh"
/* The same directory as infer_engine's and pipeline_infer's YOLO11n cases, so they share the cached programs. */
#define YOLO_DIR RDS_TEST_BUILD_DIR "/infer-engine-yolo"
#define WORK_DIR RDS_TEST_BUILD_DIR "/pipeline-tracker"

#define CLIP_W 1280
#define CLIP_H 720
#define CLIP_FRAMES 1356

static gboolean
have_file (const gchar * path, const gchar * hint)
{
  gchar *msg;

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return TRUE;
  msg = g_strdup_printf ("no %s (%s)", path, hint);
  g_test_skip (msg);
  g_free (msg);
  return FALSE;
}

static gboolean
have_yolo (void)
{
  return g_file_test (YOLO, G_FILE_TEST_IS_REGULAR) && g_file_test (LABELS, G_FILE_TEST_IS_REGULAR);
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

/* scale2 (output = 2 x input) at 640x640 with tensor output only: the soak's stand-in without YOLO11n. It is copied
 * into WORK_DIR, so its programs are cached there. */
static gchar *
scale2_config (void)
{
  gchar *model = g_build_filename (WORK_DIR, "scale2.onnx", NULL);
  gchar *path = g_build_filename (WORK_DIR, "scale2-soak.txt", NULL);
  gchar *contents = NULL, *text;
  gsize len = 0;
  GError *err = NULL;

  g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
  if (!g_file_test (model, G_FILE_TEST_EXISTS)) {
    g_assert_true (g_file_get_contents (RDS_TEST_MODELS "/scale2.onnx", &contents, &len, &err));
    g_assert_true (g_file_set_contents (model, contents, (gssize) len, &err));
    g_free (contents);
  }
  text = g_strdup_printf ("[property]\nonnx-file=%s\ninfer-dims=3;640;640\nbatch-size=4\nnetwork-type=100\n"
      "output-tensor-meta=1\nnet-scale-factor=0.00392156862745098\nmaintain-aspect-ratio=1\n"
      "symmetric-padding=1\n", model);
  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  g_free (text);
  g_free (model);
  return path;
}

/* probationAge of a tracker config as rdstracker reads it (the metrics need it). */
static guint
config_probation (const gchar * path)
{
  GError *err = NULL;
  RdsTrackerConfig *cfg = rds_tracker_config_load (path, &err);
  guint v;

  g_assert_no_error (err);
  v = cfg->probation_age;
  rds_tracker_config_free (cfg);
  return v;
}

/* Waits for EOS on the bus; fails on an error or after @timeout. */
static void
wait_eos (GstElement * pipe, GstClockTime timeout)
{
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, timeout, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  if (msg == NULL)
    g_error ("no EOS within %" GST_TIME_FORMAT, GST_TIME_ARGS (timeout));
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;
    gchar *dbg = NULL;

    gst_message_parse_error (msg, &err, &dbg);
    g_error ("pipeline error from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message,
        dbg ? dbg : "");
  }
  gst_message_unref (msg);
  gst_object_unref (bus);
}

/* ---- the clip ---- */

typedef struct
{
  GArray *dets;                 /* TrackDet, every detection of every frame, in frame order */
  GPtrArray *objs;              /* the RdsObjectMeta of each detection of the current frame */
  guint frame_start;            /* index in dets of the current frame's first detection */
  gint frames;
  guint created;                /* tracker-created objects (none expected: inference runs on every frame) */
  gint64 t_in, tracker_us;      /* rdstracker's time: from its sink pad to its source pad */
  guint batches;
} ClipRun;

/* rdsinfer's source pad: records the frame's detections (what rdstracker gets), with the detector's box */
static GstPadProbeReturn
detections_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  ClipRun *r = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  RdsFrameMeta *fm;

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, 1);
  fm = bm->frame_meta_list->data;
  g_assert_true (fm->bInferDone);
  g_assert_cmpint (fm->frame_num, ==, r->frames);
  r->frame_start = r->dets->len;
  g_ptr_array_set_size (r->objs, 0);
  for (GList * o = fm->obj_meta_list; o; o = o->next) {
    RdsObjectMeta *om = o->data;
    const RdsBbox_Coords *b = &om->detector_bbox_info.org_bbox_coords;
    TrackDet d = {.frame = r->frames,.class_id = om->class_id,.conf = om->confidence,.x = b->left,.y = b->top,
      .w = b->width,.h = b->height,.id = TRACK_NO_ID
    };

    g_array_append_val (r->dets, d);
    g_ptr_array_add (r->objs, om);
  }
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
tracker_in_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  ((ClipRun *) user_data)->t_in = g_get_monotonic_time ();
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
tracker_out_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  ClipRun *r = user_data;

  r->tracker_us += g_get_monotonic_time () - r->t_in;
  r->batches++;
  return GST_PAD_PROBE_OK;
}

/* appsink: the IDs rdstracker gave the frame's detections (it writes them into the same object metas) */
static GstFlowReturn
on_sample (GstAppSink * sink, gpointer user_data)
{
  ClipRun *r = user_data;
  GstSample *sample = gst_app_sink_pull_sample (sink);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (gst_sample_get_buffer (sample));
  RdsFrameMeta *fm;

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  fm = bm->frame_meta_list->data;
  for (GList * o = fm->obj_meta_list; o; o = o->next) {
    RdsObjectMeta *om = o->data;
    guint k;

    if (om->confidence < 0.0f) {
      r->created++;
      continue;
    }
    g_assert_true (g_ptr_array_find (r->objs, om, &k));
    g_assert_cmpuint (om->object_id, !=, RDS_UNTRACKED_OBJECT_ID);
    g_array_index (r->dets, TrackDet, r->frame_start + k).id = om->object_id;
  }
  rds_release_meta_lock (bm);
  r->frames++;
  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

/* The clip -> rdsvideodec -> rdsstreammux (batch 1, 1280x720) -> rdsinfer (YOLO11n) -> rdstracker (@tracker_config)
 * -> appsink. Returns every detection with its ID; prints the tracker's time per batch. */
static GArray *
run_clip (const gchar * tracker_config)
{
  ClipRun r = { 0 };
  gchar *pgie = pgie_config ();
  gchar *desc;
  GstElement *pipe, *el;
  GstPad *pad;
  GError *err = NULL;
  GstAppSinkCallbacks cb = {.new_sample = on_sample };
  gint64 t0;

  r.dets = g_array_new (FALSE, FALSE, sizeof (TrackDet));
  r.objs = g_ptr_array_new ();
  desc = g_strdup_printf ("filesrc location=" CLIP " ! h264parse ! rdsvideodec ! m.sink_0 "
      "rdsstreammux name=m batch-size=1 width=%u height=%u ! rdsinfer name=infer config-file-path=%s "
      "! rdstracker name=trk ll-config-file=%s ! appsink name=k sync=false", CLIP_W, CLIP_H, pgie, tracker_config);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);

  el = gst_bin_get_by_name (GST_BIN (pipe), "infer");
  pad = gst_element_get_static_pad (el, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, detections_probe, &r, NULL);
  gst_object_unref (pad);
  gst_object_unref (el);
  el = gst_bin_get_by_name (GST_BIN (pipe), "trk");
  pad = gst_element_get_static_pad (el, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, tracker_in_probe, &r, NULL);
  gst_object_unref (pad);
  pad = gst_element_get_static_pad (el, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, tracker_out_probe, &r, NULL);
  gst_object_unref (pad);
  gst_object_unref (el);
  el = gst_bin_get_by_name (GST_BIN (pipe), "k");
  gst_app_sink_set_callbacks (GST_APP_SINK (el), &cb, &r, NULL);
  gst_object_unref (el);

  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* a first YOLO11n compile takes about a minute on a small GPU; a run about 30 s */
  wait_eos (pipe, 600 * GST_SECOND);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);

  g_test_message ("%s: %d frames, %u detections in %.1f s; rdstracker %.3f ms per batch", tracker_config,
      r.frames, r.dets->len, (g_get_monotonic_time () - t0) / 1e6, r.batches ? r.tracker_us / 1e3 / r.batches : 0.0);
  g_assert_cmpint (r.frames, ==, CLIP_FRAMES);
  g_assert_cmpuint (r.batches, ==, CLIP_FRAMES);
  g_assert_cmpuint (r.created, ==, 0);
  g_ptr_array_unref (r.objs);
  g_free (pgie);
  return r.dets;
}

/* Saves @dets (with their IDs, and hashes of the config and the core's sources) into WORK_DIR, for tracker_replay. */
static void
save_dets (const GArray * dets, const gchar * config, const gchar * name)
{
  gchar *path = g_build_filename (WORK_DIR, name, NULL);
  GError *err = NULL;

  g_assert_true (track_dets_save (dets, config, RDS_TEST_TRACKER_CORE, path, &err));
  g_assert_no_error (err);
  g_test_message ("detections and IDs saved to %s", path);
  g_free (path);
}

/* Budgets for every class together, from a measured run (YOLO11n, the NvSORT sample config, this clip: coverage
 * 0.988, 4 switches, 283 tracks of 75.6 frames on average, 16835 detections with conf >= 0.5): coverage at most 0.05
 * below the measured, switches at most 20 % (and at least 2) above. */
#define NVSORT_COVERAGE_MIN (0.988 - 0.05)
#define NVSORT_SWITCHES_MAX 6
/* and enough data that the budgets mean something: at least 80 % of the measured detections, and tracks at least
 * 0.7 times as long on average */
#define NVSORT_ELIGIBLE_MIN (16835 * 8 / 10)
#define NVSORT_MEAN_LENGTH_MIN (0.7 * 75.6)

/* The clip with the NvSORT sample config (checked against the budgets), then with the IOU one (printed only). */
static void
test_clip (void)
{
  TrackStats all;
  GArray *dets;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_file (CLIP, CLIP_HINT) || !have_file (YOLO, YOLO_HINT) || !have_file (LABELS, YOLO_HINT))
    return;

  dets = run_clip (NVSORT_CONFIG);
  save_dets (dets, NVSORT_CONFIG, "crosswalk-nvsort.txt");
  track_print_table ("NvSORT (samples/tracker/config_tracker_NvSORT.yml):", dets, config_probation (NVSORT_CONFIG),
      &all);
  g_test_message ("budget: coverage >= %.3f, switches <= %u; at least %u detections, mean length >= %.1f",
      NVSORT_COVERAGE_MIN, NVSORT_SWITCHES_MAX, NVSORT_ELIGIBLE_MIN, NVSORT_MEAN_LENGTH_MIN);
  g_assert_cmpuint (all.tracks, >, 0);
  g_assert_cmpuint (all.eligible, >=, NVSORT_ELIGIBLE_MIN);
  g_assert_cmpfloat (all.mean_length, >=, NVSORT_MEAN_LENGTH_MIN);
  g_assert_cmpfloat (track_coverage (&all), >=, NVSORT_COVERAGE_MIN);
  g_assert_cmpuint (all.switches, <=, NVSORT_SWITCHES_MAX);
  g_array_unref (dets);

  dets = run_clip (IOU_CONFIG);
  save_dets (dets, IOU_CONFIG, "crosswalk-iou.txt");
  track_print_table ("IOU (samples/tracker/config_tracker_IOU.yml):", dets, config_probation (IOU_CONFIG), NULL);
  g_array_unref (dets);
}

/* ---- soak ---- */

#define SOAK_SOURCES 4
#define SOAK_MIN_BATCHES 3000
#define SOAK_SAMPLE_EVERY 300   /* batches between memory samples */
#define SOAK_MAX_SAMPLES 16
#define SOAK_SYNTH_FRAMES 300   /* the generated clip, without the real one */
#define SOAK_OBJECTS 24         /* scripted objects per frame, without YOLO11n */

typedef struct
{
  gboolean inject;
  guint batches, frames, objects, tracked;
  guint n_samples;
  gsize rss[SOAK_MAX_SAMPLES], gpu_used[SOAK_MAX_SAMPLES];
} TrackerSoakStats;

/* Without YOLO11n, on rdsinfer's source pad: SOAK_OBJECTS objects per frame moving across it, two classes; each
 * object is missing on some frames and leaves for a while every few hundred frames, so targets are created, go
 * inactive and end all through the run. */
static GstPadProbeReturn
inject_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, G_GNUC_UNUSED gpointer user_data)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    gint f = fm->frame_num;

    for (guint k = 0; k < SOAK_OBJECTS; k++) {
      RdsObjectMeta *om;
      gfloat x, y;

      if ((f + 7 * (gint) k) % 13 == 0 || (f + 37 * (gint) k) % 400 < 40)
        continue;
      om = rds_acquire_obj_meta_from_pool (bm);
      x = (gfloat) ((f * (gint) (k % 5 + 1) + 97 * (gint) k) % (CLIP_W - 80));
      y = (gfloat) (20 + 28 * k);
      om->unique_component_id = 1;
      om->class_id = k % 2 ? TRACK_CLASS_CAR : TRACK_CLASS_PERSON;
      om->object_id = RDS_UNTRACKED_OBJECT_ID;
      om->confidence = 0.6f + 0.01f * (gfloat) k;
      om->detector_bbox_info.org_bbox_coords.left = om->rect_params.left = x;
      om->detector_bbox_info.org_bbox_coords.top = om->rect_params.top = y;
      om->detector_bbox_info.org_bbox_coords.width = om->rect_params.width = 60.0f;
      om->detector_bbox_info.org_bbox_coords.height = om->rect_params.height = 24.0f;
      rds_add_obj_meta_to_frame (fm, om, NULL);
    }
  }
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

/* On rdstracker's source pad: counts frames, objects and tracked objects, and samples memory use every
 * SOAK_SAMPLE_EVERY batches. */
static GstPadProbeReturn
tracker_soak_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  TrackerSoakStats *st = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  g_assert_nonnull (bm);
  st->batches++;
  st->frames += bm->num_frames_in_batch;
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next)
    for (GList * o = ((RdsFrameMeta *) l->data)->obj_meta_list; o; o = o->next) {
      st->objects++;
      if (((RdsObjectMeta *) o->data)->object_id != RDS_UNTRACKED_OBJECT_ID)
        st->tracked++;
    }
  rds_release_meta_lock (bm);
  if (st->batches % SOAK_SAMPLE_EVERY == 0 && st->n_samples < SOAK_MAX_SAMPLES) {
    st->rss[st->n_samples] = host_memory_bytes ();
    st->gpu_used[st->n_samples] = gpu_used_bytes ();
    st->n_samples++;
  }
  return GST_PAD_PROBE_OK;
}

/* Four 1280x720 H.264 sources (the street clip, else a generated 300-frame one), each looped until there are at
 * least 3000 batches of 4, through rdsvideodec, rdsstreammux, rdsinfer (YOLO11n with the rds-test3 config, or scale2
 * with scripted objects added after it) and rdstracker (the NvSORT sample config) into a fakesink: VmRSS (under
 * ASan, the live heap) grows by less than 8 MiB and the process's GPU memory moves by less than 16 MiB between the
 * second memory sample (600 batches) and the last. Reports the fps. */
static void
test_tracker_soak (void)
{
  TrackerSoakStats st = { 0 };
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *el, *src[SOAK_SOURCES];
  gboolean yolo, clip;
  gchar *cfg;
  GError *err = NULL;
  GstPad *pad;
  GBytes *stream;
  guint frames_per_loop, loops;
  gint64 t0, t1;
  gssize rss_growth, gpu_delta;
  gdouble secs;

  RDS_TEST_REQUIRE_GPU ();
  if (!rds_test_have_element ("vah264enc") || !rds_test_have_element ("h264parse")) {
    g_test_skip ("vah264enc or h264parse not available");
    return;
  }
  yolo = have_yolo ();
  clip = g_file_test (CLIP, G_FILE_TEST_IS_REGULAR);
  if (!yolo)
    g_test_message ("no " YOLO ": running scale2 with scripted objects");
  if (!clip)
    g_test_message ("no " CLIP ": running a generated clip");
  cfg = yolo ? pgie_config () : scale2_config ();
  if (clip) {
    gchar *data = NULL;
    gsize len = 0;

    g_assert_true (g_file_get_contents (CLIP, &data, &len, NULL));
    stream = g_bytes_new_take (data, len);
    frames_per_loop = CLIP_FRAMES;
  } else {
    stream = rds_test_encode ("h264", CLIP_W, CLIP_H, SOAK_SYNTH_FRAMES, 2, "ball");
    frames_per_loop = SOAK_SYNTH_FRAMES;
  }
  g_assert_nonnull (stream);
  loops = (SOAK_MIN_BATCHES + frames_per_loop - 1) / frames_per_loop;
  st.inject = !yolo;

  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_string_append_printf (desc, "appsrc name=s%u caps=video/x-h264,stream-format=byte-stream format=bytes "
        "! h264parse ! rdsvideodec ! m.sink_%u ", i, i);
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u "
      "! rdsinfer name=infer config-file-path=%s batch-size=%u ! rdstracker name=trk ll-config-file=" NVSORT_CONFIG
      " ! fakesink sync=false enable-last-sample=false", CLIP_W, CLIP_H, SOAK_SOURCES, cfg, SOAK_SOURCES);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  if (st.inject) {
    el = gst_bin_get_by_name (GST_BIN (pipe), "infer");
    pad = gst_element_get_static_pad (el, "src");
    gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, inject_probe, NULL, NULL);
    gst_object_unref (pad);
    gst_object_unref (el);
  }
  el = gst_bin_get_by_name (GST_BIN (pipe), "trk");
  pad = gst_element_get_static_pad (el, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, tracker_soak_probe, &st, NULL);
  gst_object_unref (pad);
  gst_object_unref (el);
  for (guint i = 0; i < SOAK_SOURCES; i++) {
    gchar name[4];

    g_snprintf (name, sizeof (name), "s%u", i);
    src[i] = gst_bin_get_by_name (GST_BIN (pipe), name);
  }

  /* the program is loaded (or built) in the state change, before the clock starts */
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* each loop is a complete stream (SPS/PPS + IDR first); the buffers share the encoded bytes */
  for (guint l = 0; l < loops; l++)
    for (guint i = 0; i < SOAK_SOURCES; i++)
      g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), gst_buffer_new_wrapped_bytes (stream)), ==,
          GST_FLOW_OK);
  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src[i])), ==, GST_FLOW_OK);
  wait_eos (pipe, 1700 * GST_SECOND);
  t1 = g_get_monotonic_time ();
  gst_element_set_state (pipe, GST_STATE_NULL);
  for (guint i = 0; i < SOAK_SOURCES; i++)
    gst_object_unref (src[i]);
  gst_object_unref (pipe);
  g_bytes_unref (stream);
  g_free (cfg);

  secs = (t1 - t0) / 1e6;
  for (guint i = 0; i < st.n_samples; i++)
    g_test_message ("batch %4u: " RDS_TEST_HOST_MEMORY_NAME " %.1f MiB, GPU used %.1f MiB",
        (i + 1) * SOAK_SAMPLE_EVERY, MIB (st.rss[i]), MIB (st.gpu_used[i]));
  g_assert_cmpuint (st.n_samples, >=, 3);
  rss_growth = (gssize) st.rss[st.n_samples - 1] - (gssize) st.rss[1];
  gpu_delta = (gssize) st.gpu_used[st.n_samples - 1] - (gssize) st.gpu_used[1];
  g_test_message ("%s, %s: %u frames in %u batches in %.1f s (%.0f fps, %.1f batches/s), %u objects (%u tracked); "
      RDS_TEST_HOST_MEMORY_NAME " growth batch %u -> %u: %+.2f MiB; GPU used delta: %+.2f MiB",
      yolo ? "YOLO11n" : "scale2 + scripted objects", clip ? "street clip" : "generated clip", st.frames,
      st.batches, secs, st.frames / secs, st.batches / secs, st.objects, st.tracked, 2 * SOAK_SAMPLE_EVERY,
      st.n_samples * SOAK_SAMPLE_EVERY, MIB (rss_growth), MIB (gpu_delta));
  g_assert_cmpuint (st.batches, ==, frames_per_loop * loops);
  g_assert_cmpuint (st.batches, >=, SOAK_MIN_BATCHES);
  g_assert_cmpuint (st.frames, ==, SOAK_SOURCES * frames_per_loop * loops);
  g_assert_cmpuint (st.tracked, >, 0);
  g_assert_cmpint (rss_growth, <, 8 * 1024 * 1024);
  g_assert_cmpint (ABS (gpu_delta), <, 16 * 1024 * 1024);
}

int
main (int argc, char **argv)
{
  int ret;

  /* MIOpen and comgr caches in the build directory (shared with infer_engine), so re-runs compile nothing */
  g_setenv ("MIOPEN_USER_DB_PATH", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_setenv ("MIOPEN_CUSTOM_CACHE_DIR", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_setenv ("AMD_COMGR_CACHE_DIR", RDS_TEST_BUILD_DIR "/comgr", FALSE);
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/tracker-yolo/clip", test_clip);
  g_test_add_func ("/gpu/tracker-soak/clip", test_tracker_soak);
  ret = g_test_run ();
  /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at process
   * exit otherwise */
  gst_deinit ();
  return ret;
}
