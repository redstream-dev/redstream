/* The rds-test2 pipeline soak (gpu, soak suite): four sources of the street clip, looped into appsrcs -> rdsvideodec
 * -> rdsstreammux (batch 4, 1280x720) -> rdsinfer (YOLO11n, the sample's PGIE config) -> rdstracker (the NvSORT
 * sample config) -> rdsinfer (YOLO11n-cls on the vehicles, the sample's SGIE config) -> rdsmultistreamtiler (2x2) ->
 * rdsvideoconvert -> RGBA -> rdsosd -> fakesink, as rds-test2 runs it, three loops of the clip (4068 batches of 4,
 * also under ASan). After the warm-up (batch 600) this process's GPU memory (rds-test-soak.h) may not move by 1 MiB,
 * host memory (VmRSS; under ASan the live heap) may not grow by SOAK_MAX_HOST_GROWTH, and the classifier's per-object
 * cache stays bounded: never more entries than tracked vehicles seen in the last frames of their source (entries
 * absent for 30 frames are evicted). Skips without the clip, YOLO11n or YOLO11n-cls.
 *
 * Host memory does grow here, by about 1.1 MiB of live heap (about 1.9 MiB of VmRSS) from batch 600 to the end, and
 * stops growing after about batch 3000: rdsosd's text caches filling up. Measured with counters in rdsosd's planner
 * (not committed): rds-test2's labels ("car 1234 minivan ...") are new for every track and then repeat while it is
 * tracked, about one new text per batch, so
 * - the mask cache (a Mask and its key per text, about 150 bytes, the masks themselves in the preallocated 8 MiB
 *   heap) grows from about 1700 masks at batch 600 to about 7300 at batch 2700, where the heap is full and the least
 *   recently used masks are evicted from then on;
 * - the layout cache (a key and a Layout per text, about 100 bytes) grows from 640 entries to 4048 at the end, and is
 *   cleared whenever it reaches its cap of 4096 entries.
 * Together that is about (7300 - 1700) x 150 + (4096 - 640) x 100 bytes, 1.2 MiB of heap at most for these labels.
 * pipeline_osd_soak, the same pipeline without the classifier, gives every label a new text in every batch, so both
 * caches fill within the first few hundred batches there and its heap stays flat (-0.23 MiB under ASan over the
 * same 4068 batches). So the host bound is 3 MiB: measured were +1.10 MiB of live heap under ASan and +1.72 to
 * +1.83 MiB of VmRSS (the caches plus malloc's overhead) in the release build, which leaves about 1.2 MiB of slack, so
 * over the 3300 batches measured it catches a leak of roughly 0.4 KiB per batch or more, not a smaller one. That is
 * well under the 8 MiB of the sibling soaks (they bound unrelated pools and caches). The GPU bound stays 1 MiB, as
 * nothing on the GPU grows (+0.000 MiB measured; the siblings allow 16 MiB).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-soak.h"

#include <gmodule.h>
#include <gst/app/gstappsrc.h>
#include <rds/rds.h>
#include <unistd.h>

#define YOLO RDS_TEST_CACHE_MODELS "/yolo11n.onnx"
#define COCO_LABELS RDS_TEST_CACHE_MODELS "/coco-labels.txt"
#define YOLO_CLS RDS_TEST_CACHE_MODELS "/yolo11n-cls.onnx"
#define IMAGENET_LABELS RDS_TEST_CACHE_MODELS "/imagenet-labels.txt"
#define CLIP RDS_TEST_CACHE_MEDIA "/crosswalk-720p.h264"
#define PGIE_CONFIG RDS_TEST_SAMPLES "/rds-test2/rds_test2_pgie_config.yml"
#define SGIE_CONFIG RDS_TEST_SAMPLES "/rds-test2/rds_test2_sgie_config.yml"
#define NVSORT_CONFIG RDS_TEST_SAMPLES "/tracker/config_tracker_NvSORT.yml"
/* The same directory as the other YOLO11n tests, so they share the cached programs. */
#define YOLO_DIR RDS_TEST_BUILD_DIR "/infer-engine-yolo"
#define WORK_DIR RDS_TEST_BUILD_DIR "/pipeline-test2-soak"

#define SOURCES 4
#define CLIP_W 1280
#define CLIP_H 720
#define CLIP_FRAMES 1356
#define SOAK_LOOPS 3
#define SOAK_SAMPLE_EVERY 300   /* batches between memory samples */
#define SOAK_WARMUP_SAMPLE 1    /* the baseline: the sample after 600 batches */
#define SOAK_MAX_SAMPLES 16
#define SOAK_MAX_HOST_GROWTH (3 * 1024 * 1024)
#define SOAK_MAX_GPU_DELTA (1024 * 1024)
/* rdsinfer evicts a cache entry once its object has been absent from 30 frames of its source; the bound counts the
 * tracked vehicles (the classifier's classes) seen in a window a little longer than that. It has no margin (36 entries
 * against a bound of 36 at the peak): it holds because a track keeps its class, so should a tracker change let a
 * track's class flip while its cache entry lives on, this check fails. */
#define CACHE_WINDOW 32

typedef guint (*GetCacheSizeFunc) (GstElement * element);

typedef struct
{
  GetCacheSizeFunc get_cache_size;
  GstElement *sgie;
  guint batches, frames, objects, labelled;
  guint64 source_frames[SOURCES];
  GHashTable *last_seen[SOURCES];       /* vehicle object_id -> the source's frame count when last seen */
  guint cache_size, cache_max, bound_at_max, cache_over_bound;
  guint n_samples;
  gsize host[SOAK_MAX_SAMPLES], gpu_used[SOAK_MAX_SAMPLES];
  guint cache[SOAK_MAX_SAMPLES];
} Test2SoakStats;

/* @src (a sample config) with onnx-file and labelfile-path replaced by @onnx and @labels, written to WORK_DIR/@name;
 * returns its path. */
static gchar *
rewrite_config (const gchar * src, const gchar * onnx, const gchar * labels, const gchar * name)
{
  gchar *path = g_build_filename (WORK_DIR, name, NULL);
  gchar *text = NULL, **lines;
  GString *out = g_string_new (NULL);
  guint replaced = 0;
  GError *err = NULL;

  g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
  g_assert_true (g_file_get_contents (src, &text, NULL, &err));
  g_assert_no_error (err);
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    if (g_str_has_prefix (lines[i], "  onnx-file:")) {
      g_string_append_printf (out, "  onnx-file: %s\n", onnx);
      replaced++;
    } else if (g_str_has_prefix (lines[i], "  labelfile-path:")) {
      g_string_append_printf (out, "  labelfile-path: %s\n", labels);
      replaced++;
    } else if (lines[i + 1] || *lines[i]) {
      g_string_append_printf (out, "%s\n", lines[i]);
    }
  }
  g_assert_cmpuint (replaced, ==, 2);
  g_assert_true (g_file_set_contents (path, out->str, (gssize) out->len, &err));
  g_assert_no_error (err);
  g_string_free (out, TRUE);
  g_strfreev (lines);
  g_free (text);
  return path;
}

/* The fetched model @target behind a symlink in YOLO_DIR, so its programs are cached there. */
static gchar *
model_link (const gchar * target)
{
  gchar *base = g_path_get_basename (target);
  gchar *link = g_build_filename (YOLO_DIR, base, NULL);

  g_assert_cmpint (g_mkdir_with_parents (YOLO_DIR, 0755), ==, 0);
  if (!g_file_test (link, G_FILE_TEST_IS_SYMLINK))
    g_assert_cmpint (symlink (target, link), ==, 0);
  g_free (base);
  return link;
}

/* The classifier's classes in the sample config: COCO car, motorcycle, bus and truck. */
static gboolean
is_vehicle (gint class_id)
{
  return class_id == 2 || class_id == 3 || class_id == 5 || class_id == 7;
}

/* On the classifier's source pad (its streaming thread): counts, the cache size against its bound, and memory use
 * every SOAK_SAMPLE_EVERY batches. */
static GstPadProbeReturn
test2_soak_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Test2SoakStats *st = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  guint bound = 0;

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  for (GList * f = bm->frame_meta_list; f; f = f->next) {
    RdsFrameMeta *fm = f->data;
    guint s = fm->source_id;
    GHashTableIter it;
    gpointer value;

    g_assert_cmpuint (s, <, SOURCES);
    st->source_frames[s]++;
    st->frames++;
    for (GList * o = fm->obj_meta_list; o; o = o->next) {
      RdsObjectMeta *om = o->data;

      st->objects++;
      if (om->classifier_meta_list)
        st->labelled++;
      if (om->object_id != RDS_UNTRACKED_OBJECT_ID && is_vehicle (om->class_id))
        g_hash_table_insert (st->last_seen[s], g_memdup2 (&om->object_id, sizeof (guint64)),
            GSIZE_TO_POINTER ((gsize) st->source_frames[s]));
    }
    g_hash_table_iter_init (&it, st->last_seen[s]);
    while (g_hash_table_iter_next (&it, NULL, &value))
      if (st->source_frames[s] - GPOINTER_TO_SIZE (value) >= CACHE_WINDOW)
        g_hash_table_iter_remove (&it);
  }
  rds_release_meta_lock (bm);
  for (guint s = 0; s < SOURCES; s++)
    bound += g_hash_table_size (st->last_seen[s]);
  st->cache_size = st->get_cache_size (st->sgie);
  if (st->cache_size > st->cache_max) {
    st->cache_max = st->cache_size;
    st->bound_at_max = bound;
  }
  if (st->cache_size > bound)
    st->cache_over_bound++;
  st->batches++;
  if (st->batches % SOAK_SAMPLE_EVERY == 0 && st->n_samples < SOAK_MAX_SAMPLES) {
    st->host[st->n_samples] = host_memory_bytes ();
    st->gpu_used[st->n_samples] = gpu_used_bytes ();
    st->cache[st->n_samples] = st->cache_size;
    st->n_samples++;
  }
  return GST_PAD_PROBE_OK;
}

static gboolean
have_file (const gchar * path, const gchar * hint)
{
  gchar *msg;

  if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return TRUE;
  msg = g_strdup_printf ("no %s (run %s)", path, hint);
  g_test_skip (msg);
  g_free (msg);
  return FALSE;
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

static void
test_test2_soak (void)
{
  Test2SoakStats st = { 0 };
  GstElement *pipe, *src[SOURCES];
  GString *desc = g_string_new (NULL);
  GstPlugin *found, *plugin;
  GModule *module;
  GBytes *stream;
  gchar *yolo, *cls, *pgie, *sgie, *data = NULL;
  gsize len = 0;
  GError *err = NULL;
  GstPad *pad;
  guint warnings;
  gint64 t0, t1;
  gssize host_growth, gpu_delta;
  gdouble secs;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_file (CLIP, "tests/models/fetch-tracker-clip.sh") || !have_file (YOLO, "tests/models/fetch-yolo11n.sh")
      || !have_file (COCO_LABELS, "tests/models/fetch-yolo11n.sh")
      || !have_file (YOLO_CLS, "tests/models/fetch-yolo11n-cls.sh")
      || !have_file (IMAGENET_LABELS, "tests/models/fetch-yolo11n-cls.sh"))
    return;
  found = gst_registry_find_plugin (gst_registry_get (), "rdsinfer");
  g_assert_nonnull (found);
  plugin = gst_plugin_load (found);
  g_assert_nonnull (plugin);
  gst_object_unref (found);
  module = g_module_open (gst_plugin_get_filename (plugin), G_MODULE_BIND_LOCAL);
  g_assert_nonnull (module);
  g_assert_true (g_module_symbol (module, "rds_infer_test_get_cache_size", (gpointer *) & st.get_cache_size));
  /* the module stays open: the plugin is never unloaded either */
  gst_object_unref (plugin);
  for (guint s = 0; s < SOURCES; s++)
    st.last_seen[s] = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL);

  yolo = model_link (YOLO);
  cls = model_link (YOLO_CLS);
  pgie = rewrite_config (PGIE_CONFIG, yolo, COCO_LABELS, "pgie.yml");
  sgie = rewrite_config (SGIE_CONFIG, cls, IMAGENET_LABELS, "sgie.yml");
  g_assert_true (g_file_get_contents (CLIP, &data, &len, NULL));
  stream = g_bytes_new_take (data, len);

  for (guint i = 0; i < SOURCES; i++)
    g_string_append_printf (desc, "appsrc name=s%u caps=video/x-h264,stream-format=byte-stream format=bytes "
        "! h264parse ! rdsvideodec ! m.sink_%u ", i, i);
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u "
      "! rdsinfer name=pgie config-file-path=%s batch-size=%u ! rdstracker ll-config-file=" NVSORT_CONFIG
      " ! rdsinfer name=sgie config-file-path=%s ! rdsmultistreamtiler rows=2 columns=2 width=%u height=%u "
      "! rdsvideoconvert ! video/x-raw(memory:RDS),format=RGBA ! rdsosd ! fakesink sync=false enable-last-sample=false",
      CLIP_W, CLIP_H, SOURCES, pgie, SOURCES, sgie, CLIP_W, CLIP_H);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  st.sgie = gst_bin_get_by_name (GST_BIN (pipe), "sgie");
  pad = gst_element_get_static_pad (st.sgie, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, test2_soak_probe, &st, NULL);
  gst_object_unref (pad);
  for (guint i = 0; i < SOURCES; i++) {
    gchar name[4];

    g_snprintf (name, sizeof (name), "s%u", i);
    src[i] = gst_bin_get_by_name (GST_BIN (pipe), name);
  }

  /* the programs are loaded (or built) in the state change, before the clock starts */
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* each loop is a complete stream (SPS/PPS + IDR first); the buffers share the encoded bytes */
  for (guint l = 0; l < SOAK_LOOPS; l++)
    for (guint i = 0; i < SOURCES; i++)
      g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), gst_buffer_new_wrapped_bytes (stream)), ==,
          GST_FLOW_OK);
  for (guint i = 0; i < SOURCES; i++)
    g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src[i])), ==, GST_FLOW_OK);
  warnings = wait_eos (pipe, 1700 * GST_SECOND);
  t1 = g_get_monotonic_time ();
  gst_element_set_state (pipe, GST_STATE_NULL);
  for (guint i = 0; i < SOURCES; i++)
    gst_object_unref (src[i]);
  gst_object_unref (st.sgie);
  gst_object_unref (pipe);
  g_bytes_unref (stream);
  g_free (sgie);
  g_free (pgie);
  g_free (cls);
  g_free (yolo);

  secs = (t1 - t0) / 1e6;
  for (guint i = 0; i < st.n_samples; i++)
    g_test_message ("batch %4u: " RDS_TEST_HOST_MEMORY_NAME " %.2f MiB, GPU used %.2f MiB, cache %u entries",
        (i + 1) * SOAK_SAMPLE_EVERY, MIB (st.host[i]), MIB (st.gpu_used[i]), st.cache[i]);
  g_assert_cmpuint (st.n_samples, >, SOAK_WARMUP_SAMPLE + 1);
  host_growth = (gssize) st.host[st.n_samples - 1] - (gssize) st.host[SOAK_WARMUP_SAMPLE];
  gpu_delta = (gssize) st.gpu_used[st.n_samples - 1] - (gssize) st.gpu_used[SOAK_WARMUP_SAMPLE];
  g_test_message ("%u batches of %u in %.1f s (%.1f fps), %u objects, %u labelled; cache: at most %u entries "
      "(%u tracked vehicles in the window then), over the bound in %u batches; " RDS_TEST_HOST_MEMORY_NAME
      " growth batch %u -> %u: %+.3f MiB; GPU used delta: %+.3f MiB", st.batches, SOURCES, secs, st.frames / secs,
      st.objects, st.labelled, st.cache_max, st.bound_at_max, st.cache_over_bound,
      (SOAK_WARMUP_SAMPLE + 1) * SOAK_SAMPLE_EVERY, st.n_samples * SOAK_SAMPLE_EVERY, MIB (host_growth),
      MIB (gpu_delta));
  g_assert_cmpuint (st.batches, ==, CLIP_FRAMES * SOAK_LOOPS);
  g_assert_cmpuint (st.frames, ==, SOURCES * CLIP_FRAMES * SOAK_LOOPS);
  g_assert_cmpuint (st.labelled, >, 0);
  g_assert_cmpuint (st.cache_max, >, 0);
  g_assert_cmpuint (st.cache_over_bound, ==, 0);
  g_assert_cmpuint (warnings, ==, 0);
  g_assert_cmpint (host_growth, <, SOAK_MAX_HOST_GROWTH);
  g_assert_cmpint (ABS (gpu_delta), <, SOAK_MAX_GPU_DELTA);

  for (guint s = 0; s < SOURCES; s++)
    g_hash_table_unref (st.last_seen[s]);
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
  g_test_add_func ("/gpu/test2-soak/clip", test_test2_soak);
  ret = g_test_run ();
  /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at process
   * exit otherwise */
  gst_deinit ();
  return ret;
}
