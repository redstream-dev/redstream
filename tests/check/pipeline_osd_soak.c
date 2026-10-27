/* rdsmultistreamtiler and rdsosd soak (gpu, soak suite): the pipeline of pipeline_osd (rds-test-osd-pipeline.h) with
 * the street clip looped into four appsrcs until at least 3000 batches have gone through, checking that neither host
 * nor GPU memory grows (rds-test-soak.h). rdsosd draws on the GPU (its default process-mode), and every label gets
 * its batch number appended, so each batch's texts are new masks and the text-mask cache evicts all along. Skips
 * without the clip or YOLO11n.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-soak.h"
#include "rds-test-osd-pipeline.h"

#include <gst/app/gstappsrc.h>
#include <rds/rds.h>

#define SOAK_MIN_BATCHES 3000
#define SOAK_SAMPLE_EVERY 300   /* batches between memory samples */
#define SOAK_MAX_SAMPLES 16
/* rdsosd's text-mask heap (RDS_OSD_MASK_HEAP_SIZE) and an assumed minimum mask size per label (a size-10 label of a
 * few words is wider than 32 px and taller than 8 px). rdsosd exposes no eviction count, so this is not one measured:
 * the soak requires enough relabelled objects to fill the heap 4 times over at that size, a margin that covers labels
 * clipped at the frame edge or not drawn */
#define SOAK_MASK_HEAP (8 * 1024 * 1024)
#define SOAK_MASK_MIN_BYTES 256

typedef struct
{
  guint batches, objects;
  guint in_batches, labels;     /* batches into rdsosd, and the labels made unique there */
  guint n_samples;
  gsize host[SOAK_MAX_SAMPLES], gpu_used[SOAK_MAX_SAMPLES];
} OsdSoakStats;

/* On rdsosd's sink pad: appends " #<batch>" to every object's label, so no label repeats from one batch to the next
 * and each batch rasterizes new masks. */
static GstPadProbeReturn
osd_label_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  OsdSoakStats *st = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  for (GList * f = bm->frame_meta_list; f; f = f->next)
    for (GList * o = ((RdsFrameMeta *) f->data)->obj_meta_list; o; o = o->next) {
      RdsOSD_TextParams *tp = &((RdsObjectMeta *) o->data)->text_params;
      gchar *old = tp->display_text;

      if (old == NULL)
        continue;
      tp->display_text = g_strdup_printf ("%s #%u", old, st->in_batches);
      g_free (old);
      st->labels++;
    }
  rds_release_meta_lock (bm);
  st->in_batches++;
  return GST_PAD_PROBE_OK;
}

/* On rdsosd's source pad: counts tiled frames and their objects, and samples memory use every SOAK_SAMPLE_EVERY
 * batches. */
static GstPadProbeReturn
osd_soak_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  OsdSoakStats *st = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, 1);
  st->objects += ((RdsFrameMeta *) bm->frame_meta_list->data)->num_obj_meta;
  rds_release_meta_lock (bm);
  st->batches++;
  if (st->batches % SOAK_SAMPLE_EVERY == 0 && st->n_samples < SOAK_MAX_SAMPLES) {
    st->host[st->n_samples] = host_memory_bytes ();
    st->gpu_used[st->n_samples] = gpu_used_bytes ();
    st->n_samples++;
  }
  return GST_PAD_PROBE_OK;
}

static void
test_osd_soak (void)
{
  OsdSoakStats st = { 0 };
  GstElement *pipe, *osd, *src[SOURCES];
  GBytes *stream;
  gchar *pgie, *data = NULL;
  gsize len = 0;
  guint loops, warnings;
  gint64 t0, t1;
  gssize host_growth, gpu_delta;
  gdouble secs;
  gint mode = -1;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_assets ())
    return;
  pgie = pgie_config ();
  g_assert_true (g_file_get_contents (CLIP, &data, &len, NULL));
  stream = g_bytes_new_take (data, len);
  loops = (SOAK_MIN_BATCHES + CLIP_FRAMES - 1) / CLIP_FRAMES;

  pipe = osd_pipeline_new (pgie, TRUE, "fakesink sync=false enable-last-sample=false");
  osd = gst_bin_get_by_name (GST_BIN (pipe), "osd");
  g_object_get (osd, "process-mode", &mode, NULL);
  g_assert_cmpint (mode, ==, 1);
  gst_object_unref (osd);
  add_probe (pipe, "osd", "sink", osd_label_probe, &st);
  add_probe (pipe, "osd", "src", osd_soak_probe, &st);
  for (guint i = 0; i < SOURCES; i++) {
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
  gst_object_unref (pipe);
  g_bytes_unref (stream);
  g_free (pgie);

  secs = (t1 - t0) / 1e6;
  for (guint i = 0; i < st.n_samples; i++)
    g_test_message ("batch %4u: " RDS_TEST_HOST_MEMORY_NAME " %.1f MiB, GPU used %.1f MiB",
        (i + 1) * SOAK_SAMPLE_EVERY, MIB (st.host[i]), MIB (st.gpu_used[i]));
  g_assert_cmpuint (st.n_samples, >=, 3);
  host_growth = (gssize) st.host[st.n_samples - 1] - (gssize) st.host[1];
  gpu_delta = (gssize) st.gpu_used[st.n_samples - 1] - (gssize) st.gpu_used[1];
  g_test_message ("%u batches of %u in %.1f s (%.0f fps, %.1f tiled frames/s), %u objects drawn, %u relabelled "
      "objects; " RDS_TEST_HOST_MEMORY_NAME " growth batch %u -> %u: %+.2f MiB; GPU used delta: %+.2f MiB", st.batches,
      SOURCES, secs, SOURCES * st.batches / secs, st.batches / secs, st.objects, st.labels, 2 * SOAK_SAMPLE_EVERY,
      st.n_samples * SOAK_SAMPLE_EVERY, MIB (host_growth), MIB (gpu_delta));
  g_assert_cmpuint (st.batches, ==, CLIP_FRAMES * loops);
  g_assert_cmpuint (st.batches, >=, SOAK_MIN_BATCHES);
  g_assert_cmpuint (st.objects, >, 0);
  g_assert_cmpuint (st.labels, >, 4 * SOAK_MASK_HEAP / SOAK_MASK_MIN_BYTES);
  g_assert_cmpuint (warnings, ==, 0);
  g_assert_cmpint (host_growth, <, 8 * 1024 * 1024);
  g_assert_cmpint (ABS (gpu_delta), <, 16 * 1024 * 1024);
}

int
main (int argc, char **argv)
{
  int ret;

  set_compile_caches ();
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/osd-soak/clip", test_osd_soak);
  ret = g_test_run ();
  /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at process
   * exit otherwise */
  gst_deinit ();
  return ret;
}
