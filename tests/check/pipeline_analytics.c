/* rdsanalytics end to end (gpu): a public street clip through the decoder, the mux, rdsinfer (YOLO11n, the rds-test3
 * sample's config), rdstracker (the NvSORT sample config) and rdsanalytics with the rds-test3 sample's analytics
 * config (one ROI, one line, one direction rule), then rdsvideoconvert and rdsosd when the build has it (osd-mode 2
 * draws the rules, counts and statuses). Every frame has an analytics frame meta; the ROI counts people on some
 * frames, the line counts at least one crossing, the cumulative count never decreases, and two runs give the same
 * totals. The soak (its own test entry) runs four looped copies of the clip through the same chain into a fakesink:
 * host and GPU memory stay flat, and the per-object histories stay bounded as tracks come and go (eviction).
 * The clip comes from tests/models/fetch-tracker-clip.sh (.cache/media), YOLO11n from tests/models/fetch-yolo11n.sh
 * (.cache/models); neither is committed, and the tests skip without them.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-soak.h"

#include <gmodule.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <rds/rds.h>
#include <rds/rds-analytics-meta.h>
#include <string.h>
#include <unistd.h>

#define YOLO RDS_TEST_CACHE_MODELS "/yolo11n.onnx"
#define LABELS RDS_TEST_CACHE_MODELS "/coco-labels.txt"
#define CLIP RDS_TEST_CACHE_MEDIA "/crosswalk-720p.h264"
#define PGIE_CONFIG RDS_TEST_SAMPLES "/rds-test3/rds_test3_pgie_config.yml"
#define NVSORT_CONFIG RDS_TEST_SAMPLES "/tracker/config_tracker_NvSORT.yml"
#define ANALYTICS_CONFIG RDS_TEST_SAMPLES "/rds-test3/rds_test3_analytics_config.txt"
#define YOLO_HINT "run tests/models/fetch-yolo11n.sh"
#define CLIP_HINT "run tests/models/fetch-tracker-clip.sh"
/* The same directory as the other YOLO11n tests, so they share the cached programs. */
#define YOLO_DIR RDS_TEST_BUILD_DIR "/infer-engine-yolo"
#define WORK_DIR RDS_TEST_BUILD_DIR "/pipeline-analytics"

#define CLIP_W 1280
#define CLIP_H 720
#define CLIP_FRAMES 1356

/* the sample config's labels */
#define ROI_LABEL "Crosswalk"
#define LINE_LABEL "Eastbound"
#define DIR_LABEL "West"
#define UNIQUE_ID 17

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

/* The analytics frame meta of @fm (rdsanalytics attaches exactly one), or NULL. */
static const RdsAnalyticsFrameMeta *
analytics_frame_meta (const RdsFrameMeta * fm)
{
  const RdsAnalyticsFrameMeta *found = NULL;

  for (GList * l = fm->frame_user_meta_list; l; l = l->next) {
    const RdsUserMeta *um = l->data;

    if (um->base_meta.meta_type == RDS_FRAME_META_ANALYTICS) {
      g_assert_null (found);
      found = um->user_meta_data;
      g_assert_nonnull (found);
    }
  }
  return found;
}

/* The analytics object info of @om, or NULL. */
static const RdsAnalyticsObjInfo *
analytics_obj_info (const RdsObjectMeta * om)
{
  for (GList * l = om->obj_user_meta_list; l; l = l->next) {
    const RdsUserMeta *um = l->data;

    if (um->base_meta.meta_type == RDS_OBJ_META_ANALYTICS)
      return um->user_meta_data;
  }
  return NULL;
}

/* What one stream's analytics said over a run. Two runs compare equal field by field. */
typedef struct
{
  guint frames;
  guint frames_in_roi;          /* frames whose ROI count is above 0 */
  guint64 roi_sum;              /* the ROI counts summed over the frames */
  guint roi_max;
  guint64 crossings;            /* the line's final cumulative count */
  guint crossing_frames;        /* frames with a crossing */
  guint64 lc_objects;           /* objects whose lcStatus names the line */
  guint64 dir_objects;          /* objects whose dirStatus names the direction rule */
  guint64 stopped_objects;      /* objects STOPPED */
  guint64 objects;
} StreamTotals;

/* Checks one frame's analytics against the stream's totals so far, and adds it: the frame meta is there with the
 * element's unique-id, the ROI and line entries are there, the cumulative count never decreases and grows by the
 * frame's count, which is the number of objects whose lcStatus names the line. */
static void
check_frame (const RdsFrameMeta * fm, StreamTotals * t)
{
  const RdsAnalyticsFrameMeta *m = analytics_frame_meta (fm);
  guint32 in_roi = 0;
  guint64 curr = 0, cum = 0, lc_objects = 0;

  g_assert_nonnull (m);
  g_assert_cmpuint (rds_analytics_frame_meta_get_unique_id (m), ==, UNIQUE_ID);
  g_assert_true (rds_analytics_frame_meta_get_roi_count (m, ROI_LABEL, &in_roi));
  g_assert_true (rds_analytics_frame_meta_get_lc_count (m, LINE_LABEL, &curr));
  g_assert_true (rds_analytics_frame_meta_get_lc_cum_count (m, LINE_LABEL, &cum));
  g_assert_cmpuint (cum, >=, t->crossings);
  g_assert_cmpuint (cum, ==, t->crossings + curr);
  for (GList * o = fm->obj_meta_list; o; o = o->next) {
    const RdsAnalyticsObjInfo *info = analytics_obj_info (o->data);

    t->objects++;
    if (!info)
      continue;
    for (guint i = 0; i < rds_analytics_obj_info_n_lc_status (info); i++)
      if (strcmp (rds_analytics_obj_info_get_lc_status (info, i), LINE_LABEL) == 0)
        lc_objects++;
    if (strstr (rds_analytics_obj_info_get_dir_status (info), DIR_LABEL))
      t->dir_objects++;
    if (strstr (rds_analytics_obj_info_get_obj_status (info), "STOPPED"))
      t->stopped_objects++;
  }
  g_assert_cmpuint (lc_objects, ==, curr);
  t->frames++;
  t->frames_in_roi += in_roi > 0;
  t->roi_sum += in_roi;
  t->roi_max = MAX (t->roi_max, in_roi);
  t->crossings = cum;
  t->crossing_frames += curr > 0;
  t->lc_objects += lc_objects;
}

static void
print_totals (const gchar * what, const StreamTotals * t)
{
  g_test_message ("%s: %u frames, %" G_GUINT64_FORMAT " objects; " ROI_LABEL ": %u frames with people (mean %.2f, "
      "max %u); " LINE_LABEL ": %" G_GUINT64_FORMAT " crossings on %u frames; " DIR_LABEL ": %" G_GUINT64_FORMAT
      " object-frames; STOPPED: %" G_GUINT64_FORMAT " object-frames", what, t->frames, t->objects, t->frames_in_roi,
      t->frames ? (gdouble) t->roi_sum / t->frames : 0.0, t->roi_max, t->crossings, t->crossing_frames,
      t->dir_objects, t->stopped_objects);
}

/* ---- the clip ---- */

typedef struct
{
  StreamTotals t;
  gint next_frame;
} ClipRun;

/* rdsanalytics' source pad: every frame checked and counted */
static GstPadProbeReturn
clip_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  ClipRun *r = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  RdsFrameMeta *fm;

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, 1);
  fm = bm->frame_meta_list->data;
  g_assert_cmpint (fm->frame_num, ==, r->next_frame);
  check_frame (fm, &r->t);
  r->next_frame++;
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

/* The clip -> rdsvideodec -> rdsstreammux (batch 1, 1280x720) -> rdsinfer (YOLO11n) -> rdstracker (NvSORT) ->
 * rdsanalytics (the sample config) [-> rdsvideoconvert -> RGBA -> rdsosd] -> fakesink. */
static StreamTotals
run_clip (const gchar * pgie)
{
  ClipRun r = { 0 };
  gchar *desc;
  GstElement *pipe, *el;
  GstPad *pad;
  GError *err = NULL;
  gint64 t0;

  desc = g_strdup_printf ("filesrc location=" CLIP " ! h264parse ! rdsvideodec ! m.sink_0 "
      "rdsstreammux name=m batch-size=1 width=%u height=%u ! rdsinfer config-file-path=%s "
      "! rdstracker ll-config-file=" NVSORT_CONFIG " ! rdsanalytics name=an config-file=" ANALYTICS_CONFIG " %s"
      "! fakesink sync=false enable-last-sample=false", CLIP_W, CLIP_H, pgie,
      RDS_HAVE_OSD ? "! rdsvideoconvert ! video/x-raw(memory:RDS),format=RGBA ! rdsosd " : "");
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  el = gst_bin_get_by_name (GST_BIN (pipe), "an");
  pad = gst_element_get_static_pad (el, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, clip_probe, &r, NULL);
  gst_object_unref (pad);
  gst_object_unref (el);

  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* a first YOLO11n compile takes about a minute on a small GPU; a run about 35 s */
  wait_eos (pipe, 500 * GST_SECOND);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
  g_test_message ("run: %.1f s", (g_get_monotonic_time () - t0) / 1e6);
  g_assert_cmpint (r.next_frame, ==, CLIP_FRAMES);
  return r.t;
}

/* Two runs of the clip: plausible counts (people in the ROI on some frames, at least one crossing, some objects
 * going west), and the same totals both times. */
static void
test_clip (void)
{
  StreamTotals a, b;
  gchar *pgie;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_assets ())
    return;
  pgie = pgie_config ();
  a = run_clip (pgie);
  print_totals ("first run", &a);
  g_assert_cmpuint (a.frames, ==, CLIP_FRAMES);
  g_assert_cmpuint (a.frames_in_roi, >, 0);
  g_assert_cmpuint (a.crossings, >=, 1);
  g_assert_cmpuint (a.lc_objects, ==, a.crossings);
  g_assert_cmpuint (a.dir_objects, >, 0);

  b = run_clip (pgie);
  print_totals ("second run", &b);
  g_assert_cmpuint (b.frames_in_roi, ==, a.frames_in_roi);
  g_assert_cmpuint (b.roi_sum, ==, a.roi_sum);
  g_assert_cmpuint (b.roi_max, ==, a.roi_max);
  g_assert_cmpuint (b.crossings, ==, a.crossings);
  g_assert_cmpuint (b.crossing_frames, ==, a.crossing_frames);
  g_assert_cmpuint (b.dir_objects, ==, a.dir_objects);
  g_assert_cmpuint (b.stopped_objects, ==, a.stopped_objects);
  g_assert_cmpuint (b.objects, ==, a.objects);
  g_free (pgie);
}

/* ---- soak ---- */

#define SOAK_SOURCES 4
#define SOAK_MIN_BATCHES 3000
#define SOAK_SAMPLE_EVERY 300   /* batches between memory samples */
#define SOAK_MAX_SAMPLES 16
#define SOAK_MAX_LOOPS 4

typedef guint (*GetHistoryCountFunc) (GstElement * element);

typedef struct
{
  GstElement *analytics;
  GetHistoryCountFunc get_history_count;
  StreamTotals t[SOAK_SOURCES];
  guint64 loop_cum[SOAK_SOURCES][SOAK_MAX_LOOPS];       /* the cumulative count at each loop's last frame */
  GHashTable *ids;              /* the distinct (pad_index, object_id) seen */
  guint batches, frames;
  guint histories_max;
  guint n_samples;
  gsize rss[SOAK_MAX_SAMPLES], gpu_used[SOAK_MAX_SAMPLES];
  guint histories[SOAK_MAX_SAMPLES];
} AnalyticsSoakStats;

/* rdsanalytics' source pad: each frame checked and counted per source; memory use and the live histories sampled
 * every SOAK_SAMPLE_EVERY batches (the hook reads them under the element's lock, which no batch holds here) */
static GstPadProbeReturn
soak_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  AnalyticsSoakStats *st = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  guint histories;

  g_assert_nonnull (bm);
  st->batches++;
  st->frames += bm->num_frames_in_batch;
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;

    g_assert_cmpuint (fm->pad_index, <, SOAK_SOURCES);
    check_frame (fm, &st->t[fm->pad_index]);
    if (st->t[fm->pad_index].frames % CLIP_FRAMES == 0) {
      guint loop = st->t[fm->pad_index].frames / CLIP_FRAMES - 1;

      g_assert_cmpuint (loop, <, SOAK_MAX_LOOPS);
      st->loop_cum[fm->pad_index][loop] = st->t[fm->pad_index].crossings;
    }
    for (GList * o = fm->obj_meta_list; o; o = o->next) {
      guint64 key = ((guint64) fm->pad_index << 56) ^ ((RdsObjectMeta *) o->data)->object_id;

      g_hash_table_add (st->ids, g_memdup2 (&key, sizeof key));
    }
  }
  rds_release_meta_lock (bm);
  histories = st->get_history_count (st->analytics);
  st->histories_max = MAX (st->histories_max, histories);
  if (st->batches % SOAK_SAMPLE_EVERY == 0 && st->n_samples < SOAK_MAX_SAMPLES) {
    st->rss[st->n_samples] = host_memory_bytes ();
    st->gpu_used[st->n_samples] = gpu_used_bytes ();
    st->histories[st->n_samples] = histories;
    st->n_samples++;
  }
  return GST_PAD_PROBE_OK;
}

/* Four sources of the street clip, each looped until there are at least 3000 batches of 4, through rdsvideodec,
 * rdsstreammux, rdsinfer (YOLO11n), rdstracker (NvSORT) and rdsanalytics (the sample config, which has the same
 * rules on streams 0-3) into a fakesink: VmRSS (under ASan, the live heap) grows by less than 8 MiB and the
 * process's GPU memory moves by less than 16 MiB between the second memory sample (600 batches) and the last; the
 * live histories (one per tracked object, the direction and line rules need them) stay below half the distinct
 * objects of the run, since histories unseen for 30 s of PTS (or 900 frames) are dropped; and every source counts
 * crossings in every loop. Reports the fps. Measured (debug build, 3 loops, 4068 batches of 4 at 35 fps): 3308
 * distinct objects, at most 756 live histories, 87 crossings per source (29 per loop), VmRSS +0.70 MiB. */
static void
test_soak (void)
{
  AnalyticsSoakStats st = { 0 };
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *src[SOAK_SOURCES];
  GstPlugin *plugin;
  GModule *module;
  GError *err = NULL;
  GstPad *pad;
  GBytes *stream;
  gchar *cfg, *data = NULL;
  gsize len = 0;
  guint loops;
  gint64 t0, t1;
  gssize rss_growth, gpu_delta;
  gdouble secs;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_assets ())
    return;
  plugin = gst_registry_find_plugin (gst_registry_get (), "rdsanalytics");
  g_assert_nonnull (plugin);
  module = g_module_open (gst_plugin_get_filename (plugin), G_MODULE_BIND_LOCAL);
  g_assert_nonnull (module);
  g_assert_true (g_module_symbol (module, "gst_rds_analytics_test_get_history_count",
          (gpointer *) & st.get_history_count));
  gst_object_unref (plugin);
  st.ids = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);

  cfg = pgie_config ();
  g_assert_true (g_file_get_contents (CLIP, &data, &len, NULL));
  stream = g_bytes_new_take (data, len);
  loops = (SOAK_MIN_BATCHES + CLIP_FRAMES - 1) / CLIP_FRAMES;
  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_string_append_printf (desc, "appsrc name=s%u caps=video/x-h264,stream-format=byte-stream format=bytes "
        "! h264parse ! rdsvideodec ! m.sink_%u ", i, i);
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u "
      "! rdsinfer config-file-path=%s batch-size=%u ! rdstracker ll-config-file=" NVSORT_CONFIG
      " ! rdsanalytics name=an config-file=" ANALYTICS_CONFIG " ! fakesink sync=false enable-last-sample=false",
      CLIP_W, CLIP_H, SOAK_SOURCES, cfg, SOAK_SOURCES);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  st.analytics = gst_bin_get_by_name (GST_BIN (pipe), "an");
  pad = gst_element_get_static_pad (st.analytics, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, soak_probe, &st, NULL);
  gst_object_unref (pad);
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
  gst_object_unref (st.analytics);
  gst_object_unref (pipe);
  g_bytes_unref (stream);
  g_free (cfg);

  secs = (t1 - t0) / 1e6;
  for (guint i = 0; i < st.n_samples; i++)
    g_test_message ("batch %4u: " RDS_TEST_HOST_MEMORY_NAME " %.1f MiB, GPU used %.1f MiB, %u histories",
        (i + 1) * SOAK_SAMPLE_EVERY, MIB (st.rss[i]), MIB (st.gpu_used[i]), st.histories[i]);
  for (guint i = 0; i < SOAK_SOURCES; i++) {
    gchar *what = g_strdup_printf ("source %u", i);

    print_totals (what, &st.t[i]);
    g_free (what);
  }
  g_assert_cmpuint (st.n_samples, >=, 3);
  rss_growth = (gssize) st.rss[st.n_samples - 1] - (gssize) st.rss[1];
  gpu_delta = (gssize) st.gpu_used[st.n_samples - 1] - (gssize) st.gpu_used[1];
  g_test_message ("%u frames in %u batches in %.1f s (%.0f fps, %.1f batches/s); %u distinct objects, at most %u "
      "histories at once; " RDS_TEST_HOST_MEMORY_NAME " growth batch %u -> %u: %+.2f MiB; GPU used delta: %+.2f MiB",
      st.frames, st.batches, secs, st.frames / secs, st.batches / secs, g_hash_table_size (st.ids),
      st.histories_max, 2 * SOAK_SAMPLE_EVERY, st.n_samples * SOAK_SAMPLE_EVERY, MIB (rss_growth),
      MIB (gpu_delta));
  g_assert_cmpuint (st.batches, ==, CLIP_FRAMES * loops);
  g_assert_cmpuint (st.frames, ==, SOAK_SOURCES * CLIP_FRAMES * loops);
  g_assert_cmpuint (loops, <=, SOAK_MAX_LOOPS);
  for (guint i = 0; i < SOAK_SOURCES; i++) {
    g_assert_cmpuint (st.t[i].frames, ==, CLIP_FRAMES * loops);
    /* crossings counted in every loop */
    for (guint l = 0; l < loops; l++) {
      guint64 before = l ? st.loop_cum[i][l - 1] : 0;

      g_test_message ("source %u, loop %u: %" G_GUINT64_FORMAT " crossings", i, l, st.loop_cum[i][l] - before);
      g_assert_cmpuint (st.loop_cum[i][l], >, before);
    }
  }
  /* eviction: without it every distinct object would keep its history to the end */
  g_assert_cmpuint (st.histories_max, >, 0);
  g_assert_cmpuint (st.histories_max * 2, <, g_hash_table_size (st.ids));
  g_assert_cmpint (rss_growth, <, 8 * 1024 * 1024);
  g_assert_cmpint (ABS (gpu_delta), <, 16 * 1024 * 1024);
  g_hash_table_unref (st.ids);
  g_module_close (module);
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
  g_test_add_func ("/gpu/analytics-yolo/clip", test_clip);
  g_test_add_func ("/gpu/analytics-soak/clip", test_soak);
  ret = g_test_run ();
  /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at process
   * exit otherwise */
  gst_deinit ();
  return ret;
}
