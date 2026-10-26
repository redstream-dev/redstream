/* rdsmultistreamtiler and rdsosd end to end (gpu): four copies of a public street clip through the decoder, the mux,
 * rdsinfer (YOLO11n), rdstracker (NvSORT), the tiler (2x2) and rdsosd, as rds-test3 runs them
 * (rds-test-osd-pipeline.h). The tiler's one frame meta per batch carries every source's objects, scaled into their
 * cells; rdsosd draws their boxes on the tiled frame; no errors or warnings. Prints the frame rate and the tiler's
 * and rdsosd's time per batch. Skips without the clip or YOLO11n.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include "rds-test-osd-pipeline.h"

#include <gst/app/gstappsink.h>
#include <rds/rds.h>
#include <math.h>

/* every CHECK_EVERY-th output frame is read back and its box edges checked */
#define CHECK_EVERY 10
#define RECT_TOLERANCE 0.01f

typedef struct
{
  gfloat left, top, width, height;
} Rect;

typedef struct
{
  GArray *expected;             /* Rect: the objects entering the tiler, scaled into their cells */
  guint source_objects[SOURCES];        /* objects per source over the run */
  guint batches, frames, objects;
  guint edges, red_edges;       /* box left edges sampled on the output, and how many are the border colour */
  gint64 t_tiler, tiler_us, t_osd, osd_us;
} OsdRun;

/* rdsmultistreamtiler's sink pad: each frame's objects as they should come out, scaled into the cell of its
 * pad_index (2x2 grid of 640x360 cells over 1280x720 frames: origin + v * 0.5) */
static GstPadProbeReturn
tiler_in_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  OsdRun *r = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  const gdouble sx = (gdouble) (TILED_W / 2) / CLIP_W, sy = (gdouble) (TILED_H / 2) / CLIP_H;

  g_assert_nonnull (bm);
  g_array_set_size (r->expected, 0);
  rds_acquire_meta_lock (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, SOURCES);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    gdouble ox = (fm->pad_index % 2) * (TILED_W / 2), oy = (fm->pad_index / 2) * (TILED_H / 2);

    g_assert_cmpuint (fm->pad_index, <, SOURCES);
    for (GList * o = fm->obj_meta_list; o; o = o->next) {
      const RdsOSD_RectParams *rp = &((RdsObjectMeta *) o->data)->rect_params;
      Rect e = { (gfloat) (ox + rp->left * sx), (gfloat) (oy + rp->top * sy), (gfloat) (rp->width * sx),
        (gfloat) (rp->height * sy)
      };

      g_array_append_val (r->expected, e);
      r->source_objects[fm->pad_index]++;
    }
  }
  rds_release_meta_lock (bm);
  r->t_tiler = g_get_monotonic_time ();
  return GST_PAD_PROBE_OK;
}

static gboolean
rect_near (const Rect * a, const RdsOSD_RectParams * b)
{
  return fabsf (a->left - b->left) < RECT_TOLERANCE && fabsf (a->top - b->top) < RECT_TOLERANCE
      && fabsf (a->width - b->width) < RECT_TOLERANCE && fabsf (a->height - b->height) < RECT_TOLERANCE;
}

/* rdsmultistreamtiler's source pad: one frame meta holding exactly the objects that went in, each in its cell (the
 * pipeline has no queue after the mux, so this is the batch the sink probe saw) */
static GstPadProbeReturn
tiler_out_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  OsdRun *r = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  RdsFrameMeta *fm;
  gboolean *used;

  r->tiler_us += g_get_monotonic_time () - r->t_tiler;
  r->batches++;
  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, 1);
  fm = bm->frame_meta_list->data;
  g_assert_cmpuint (fm->pad_index, ==, 0);
  g_assert_cmpuint (fm->batch_id, ==, 0);
  g_assert_cmpuint (fm->num_obj_meta, ==, r->expected->len);
  g_assert_cmpuint (g_list_length (fm->obj_meta_list), ==, r->expected->len);
  used = g_new0 (gboolean, r->expected->len);
  for (GList * o = fm->obj_meta_list; o; o = o->next) {
    const RdsOSD_RectParams *rp = &((RdsObjectMeta *) o->data)->rect_params;
    guint k;

    for (k = 0; k < r->expected->len; k++)
      if (!used[k] && rect_near (&g_array_index (r->expected, Rect, k), rp))
        break;
    if (k == r->expected->len)
      g_error ("batch %u: tiled object %.2f,%.2f %.2fx%.2f matches no object of the batch", r->batches,
          rp->left, rp->top, rp->width, rp->height);
    used[k] = TRUE;
  }
  g_free (used);
  r->objects += r->expected->len;
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
osd_in_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  ((OsdRun *) user_data)->t_osd = g_get_monotonic_time ();
  return GST_PAD_PROBE_OK;
}

static GstPadProbeReturn
osd_out_probe (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  OsdRun *r = user_data;

  r->osd_us += g_get_monotonic_time () - r->t_osd;
  return GST_PAD_PROBE_OK;
}

static gboolean
is_rgb (const HostImage * img, gint64 x, gint64 y, guint8 cr, guint8 cg, guint8 cb)
{
  const guint8 *p = img->im.plane[0] + (gsize) y * img->im.pitch[0] + 4 * (gsize) x;

  return p[0] == cr && p[1] == cg && p[2] == cb;
}

/* TRUE if any pixel of the left border band of @rp (columns L - (bw-1)/2 .. L + bw/2, with L the truncated left) is
 * opaque red over rows T+2 .. B-2. Text drawn later (another object's label) may cover part of a band, rarely all. */
static gboolean
left_band_drawn (const HostImage * img, const RdsOSD_RectParams * rp)
{
  gint64 bw = MIN (rp->border_width, 32), L = (gint64) rp->left, T = (gint64) rp->top, B = T + (gint64) rp->height;

  for (gint64 x = MAX (L - (bw - 1) / 2, 0); x <= MIN (L + bw / 2, TILED_W - 1); x++)
    for (gint64 y = T + 2; y <= B - 2; y++)
      if (is_rgb (img, x, y, 255, 0, 0))
        return TRUE;
  return FALSE;
}

/* appsink: on every CHECK_EVERY-th frame, the left border band of each opaque red box inside the frame must be drawn
 * (left_band_drawn). */
static GstFlowReturn
on_sample (GstAppSink * sink, gpointer user_data)
{
  OsdRun *r = user_data;
  GstSample *sample = gst_app_sink_pull_sample (sink);
  GstBuffer *buf = gst_sample_get_buffer (sample);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsBufSurface *s = rds_buffer_get_surface (buf);
  HostImage img;

  g_assert_nonnull (bm);
  g_assert_nonnull (s);
  g_assert_cmpuint (s->numFilled, ==, 1);
  g_assert_cmpint (s->surfaceList[0].colorFormat, ==, RDS_BUF_COLOR_FORMAT_RGBA);
  g_assert_cmpuint (s->surfaceList[0].width, ==, TILED_W);
  g_assert_cmpuint (s->surfaceList[0].height, ==, TILED_H);
  if (r->frames++ % CHECK_EVERY != 0) {
    gst_sample_unref (sample);
    return GST_FLOW_OK;
  }
  snapshot (s, 0, &img);
  rds_acquire_meta_lock (bm);
  g_assert_nonnull (bm->frame_meta_list);
  for (GList * o = ((RdsFrameMeta *) bm->frame_meta_list->data)->obj_meta_list; o; o = o->next) {
    const RdsOSD_RectParams *rp = &((RdsObjectMeta *) o->data)->rect_params;
    const RdsOSD_ColorParams *c = &rp->border_color;
    gint64 L = (gint64) rp->left, T = (gint64) rp->top, B = T + (gint64) rp->height;


    if (rp->border_width == 0 || c->red != 1.0 || c->green != 0.0 || c->blue != 0.0 || c->alpha != 1.0)
      continue;
    if (L < 0 || L >= TILED_W || T < 0 || B >= TILED_H || rp->height < 8)
      continue;
    r->edges++;
    r->red_edges += left_band_drawn (&img, rp);
  }
  rds_release_meta_lock (bm);
  host_image_clear (&img);
  gst_sample_unref (sample);
  return GST_FLOW_OK;
}

static void
test_clip (void)
{
  OsdRun r = { 0 };
  GstAppSinkCallbacks cb = {.new_sample = on_sample };
  GstElement *pipe, *el;
  gchar *pgie;
  guint warnings;
  gint64 t0, t1;
  gdouble secs;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_assets ())
    return;
  pgie = pgie_config ();
  r.expected = g_array_new (FALSE, FALSE, sizeof (Rect));
  pipe = osd_pipeline_new (pgie, FALSE, "appsink sync=false");
  add_probe (pipe, "tiler", "sink", tiler_in_probe, &r);
  add_probe (pipe, "tiler", "src", tiler_out_probe, &r);
  add_probe (pipe, "osd", "sink", osd_in_probe, &r);
  add_probe (pipe, "osd", "src", osd_out_probe, &r);
  el = gst_bin_get_by_name (GST_BIN (pipe), "k");
  gst_app_sink_set_callbacks (GST_APP_SINK (el), &cb, &r, NULL);
  gst_object_unref (el);

  /* the program is loaded (or built: a first YOLO11n compile at batch 4 takes minutes) before the clock starts */
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  warnings = wait_eos (pipe, 900 * GST_SECOND);
  t1 = g_get_monotonic_time ();
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);

  secs = (t1 - t0) / 1e6;
  /* into an appsink, every CHECK_EVERY-th frame read back: not the end-to-end figure (rds-test3 into a fakesink) */
  g_test_message ("%u batches of %u (%u frames) in %.1f s: %.1f fps, %.1f tiled frames/s (appsink and periodic "
      "readback included); %u objects (per source %u %u %u %u)", r.batches, SOURCES, SOURCES * r.batches, secs,
      SOURCES * r.batches / secs, r.batches / secs, r.objects, r.source_objects[0], r.source_objects[1],
      r.source_objects[2], r.source_objects[3]);
  g_test_message ("rdsmultistreamtiler %.3f ms per batch, rdsosd %.3f ms per frame (%.1f objects per frame)",
      r.tiler_us / 1e3 / MAX (r.batches, 1), r.osd_us / 1e3 / MAX (r.frames, 1),
      (gdouble) r.objects / MAX (r.batches, 1));
  g_test_message ("box edges: %u of %u sampled have their left border band drawn", r.red_edges, r.edges);
  g_assert_cmpuint (r.batches, ==, CLIP_FRAMES);
  g_assert_cmpuint (r.frames, ==, CLIP_FRAMES);
  g_assert_cmpuint (warnings, ==, 0);
  /* the same clip in every cell: each source has its objects in the merged frame meta */
  for (guint i = 0; i < SOURCES; i++) {
    g_assert_cmpuint (r.source_objects[i], >, CLIP_FRAMES);
    g_assert_cmpuint (r.source_objects[i], ==, r.source_objects[0]);
  }
  g_assert_cmpuint (r.edges, >, 1000);
  g_assert_cmpuint (r.red_edges * 100, >=, r.edges * 99);
  g_array_unref (r.expected);
  g_free (pgie);
}

int
main (int argc, char **argv)
{
  int ret;

  set_compile_caches ();
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/osd-yolo/clip", test_clip);
  ret = g_test_run ();
  /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at process
   * exit otherwise */
  gst_deinit ();
  return ret;
}
