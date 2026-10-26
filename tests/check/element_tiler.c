/* rdsmultistreamtiler: factory, properties and pads (cpu); batches of solid grey device surfaces with scripted object
 * and display meta, tiled on the GPU: pixels per cell and black cells, the merged frame meta, grid growth,
 * show-source, absent sources, out-of-grid pads, empty batches, NV12 and RGBA, and a 500-batch run (gpu, skipped
 * without a ROCm device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"

#include <gst/check/gstharness.h>
#include <rds/rds.h>
#include <string.h>

#define SLOT_W 320
#define SLOT_H 180
/* the grey every byte of slot @i is filled with (RGBA), or its Y value (NV12, chroma 128) */
#define SLOT_VALUE(i) ((guint8) (40 + 30 * (i)))

static GstAllocator *dev_alloc;
/* what make_batch allocates from: dev_alloc unless a test switches it */
static GstAllocator *batch_alloc;

/* A device batch: slot i holds the frame of pad @pads[i] (batch_id i), filled with SLOT_VALUE (i). Every frame gets
 * one tag object rect [100,100,200,100] (object_id 100 + pad, label "p<pad>", detector box [1,2,3,4], misc_obj_info[0]
 * the slot) and a display
 * meta with one rect [10,20,30,40], one label at (40,60) and one circle (160,90) r 20. */
static GstBuffer *
make_batch (const gchar * format, const guint * pads, guint n, guint64 seq)
{
  RdsBufSurfaceCreateParams cp = {
    .width = SLOT_W,.height = SLOT_H,.layout = RDS_BUF_LAYOUT_PITCH,
    .colorFormat = g_str_equal (format, "NV12") ? RDS_BUF_COLOR_FORMAT_NV12 : RDS_BUF_COLOR_FORMAT_RGBA,
  };
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (MAX (n, 1));
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (batch_alloc, MAX (n, 1), &cp));
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);
    RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);
    RdsDisplayMeta *dm = rds_acquire_display_meta_from_pool (bm);

    if (cp.colorFormat == RDS_BUF_COLOR_FORMAT_NV12) {
      g_assert_cmpint (rds_buf_surface_memset (s, i, 0, SLOT_VALUE (i)), ==, 0);
      g_assert_cmpint (rds_buf_surface_memset (s, i, 1, 128), ==, 0);
    } else {
      g_assert_cmpint (rds_buf_surface_memset (s, i, -1, SLOT_VALUE (i)), ==, 0);
    }
    fm->source_id = pads[i];
    fm->pad_index = pads[i];
    fm->batch_id = i;
    fm->frame_num = (gint) seq;
    fm->buf_pts = 1000 + seq;
    fm->ntp_timestamp = 77;
    fm->source_frame_width = SLOT_W;
    fm->source_frame_height = SLOT_H;
    fm->pipeline_width = SLOT_W;
    fm->pipeline_height = SLOT_H;
    fm->num_surfaces_per_frame = 1;
    rds_add_frame_meta_to_batch (bm, fm);

    om->rect_params.left = 100;
    om->rect_params.top = 100;
    om->rect_params.width = 200;
    om->rect_params.height = 60;
    om->rect_params.border_width = 3;
    om->text_params.x_offset = 100;
    om->text_params.y_offset = 80;
    om->text_params.display_text = g_strdup_printf ("p%u", pads[i]);
    om->detector_bbox_info.org_bbox_coords = (RdsBbox_Coords) {1, 2, 3, 4};
    om->object_id = 100 + pads[i];
    om->class_id = (gint) pads[i];
    om->confidence = 0.5f;
    om->misc_obj_info[0] = i;
    g_snprintf (om->obj_label, sizeof om->obj_label, "p%u", pads[i]);
    rds_add_obj_meta_to_frame (fm, om, NULL);

    dm->num_rects = 1;
    dm->rect_params[0] = (RdsOSD_RectParams) {.left = 10,.top = 20,.width = 30,.height = 40,.border_width = 2 };
    dm->num_labels = 1;
    dm->text_params[0].x_offset = 40;
    dm->text_params[0].y_offset = 60;
    dm->text_params[0].display_text = g_strdup ("L");
    dm->num_circles = 1;
    dm->circle_params[0] = (RdsOSD_CircleParams) {.xc = 160,.yc = 90,.radius = 20 };
    dm->misc_osd_data[0] = pads[i];
    rds_add_display_meta_to_frame (fm, dm);
  }
  s->numFilled = n;
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  GST_BUFFER_PTS (buf) = 5 * GST_SECOND + seq;
  GST_BUFFER_DURATION (buf) = 40 * GST_MSECOND;
  return buf;
}

/* ---- harness ---- */

typedef struct
{
  GstHarness *h;
  GstBus *bus;
  const gchar *format;
} Tiler;

/* A started harness without input caps yet: tiler_set_caps () sends them. */
static void
tiler_init_no_caps (Tiler * t, const gchar * props, const gchar * format)
{
  gchar *desc = g_strdup_printf ("rdsmultistreamtiler %s", props != NULL ? props : "");
  GError *err = NULL;
  GstElement *e = gst_parse_launch (desc, &err);

  g_assert_no_error (err);
  t->bus = gst_bus_new ();
  gst_element_set_bus (e, t->bus);
  t->h = gst_harness_new_with_element (e, "sink", "src");
  gst_object_unref (e);
  t->format = format;
  g_free (desc);
}

static void
tiler_set_caps (Tiler * t)
{
  gchar *caps = g_strdup_printf ("video/x-raw(memory:RDS),format=%s,width=%d,height=%d,framerate=25/1", t->format,
      SLOT_W, SLOT_H);

  gst_harness_set_src_caps_str (t->h, caps);
  g_free (caps);
}

static void
tiler_init (Tiler * t, const gchar * props, const gchar * format)
{
  tiler_init_no_caps (t, props, format);
  tiler_set_caps (t);
}

static void
tiler_clear (Tiler * t)
{
  gst_element_set_bus (t->h->element, NULL);
  gst_bus_set_flushing (t->bus, TRUE);
  gst_harness_teardown (t->h);
  gst_object_unref (t->bus);
}

static GstBuffer *
tile (Tiler * t, const guint * pads, guint n, guint64 seq)
{
  g_assert_cmpint (gst_harness_push (t->h, make_batch (t->format, pads, n, seq)), ==, GST_FLOW_OK);
  return gst_harness_pull (t->h);
}

static guint
count_messages (GstBus * bus, GstMessageType type)
{
  GstMessage *m;
  guint n = 0;

  while ((m = gst_bus_pop_filtered (bus, type)) != NULL) {
    n++;
    gst_message_unref (m);
  }
  return n;
}

/* ---- pixels ---- */

typedef struct
{
  HostImage img;
  gboolean nv12;
  guint width, height;
} Out;

static void
out_snapshot (GstBuffer * buf, Out * o)
{
  RdsBufSurface *s = rds_buffer_get_surface (buf);

  g_assert_nonnull (s);
  g_assert_cmpuint (s->batchSize, ==, 1);
  g_assert_cmpuint (s->numFilled, ==, 1);
  o->nv12 = s->surfaceList[0].colorFormat == RDS_BUF_COLOR_FORMAT_NV12;
  o->width = s->surfaceList[0].width;
  o->height = s->surfaceList[0].height;
  snapshot (s, 0, &o->img);
}

/* TRUE when pixel (x, y) is grey @v (RGBA: every byte; NV12: Y @v and chroma 128). */
static gboolean
pixel_is (const Out * o, guint x, guint y, guint8 v)
{
  if (o->nv12) {
    const guint8 *uv = o->img.im.plane[1] + (gsize) (y / 2) * o->img.im.pitch[1] + (x / 2) * 2;

    return o->img.im.plane[0][(gsize) y * o->img.im.pitch[0] + x] == v && uv[0] == 128 && uv[1] == 128;
  } else {
    const guint8 *p = o->img.im.plane[0] + (gsize) y * o->img.im.pitch[0] + 4 * x;

    return p[0] == v && p[1] == v && p[2] == v && p[3] == v;
  }
}

/* Black: RGBA all zero, NV12 Y 16 with chroma 128. */
static gboolean
pixel_is_black (const Out * o, guint x, guint y)
{
  return pixel_is (o, x, y, o->nv12 ? 16 : 0);
}

/* Every pixel of the rect is grey @v (black when @v is -1). */
static gboolean
rect_is (const Out * o, guint left, guint top, guint w, guint h, gint v)
{
  for (guint y = top; y < top + h; y++)
    for (guint x = left; x < left + w; x++)
      if (!(v < 0 ? pixel_is_black (o, x, y) : pixel_is (o, x, y, (guint8) v))) {
        g_test_message ("pixel %u,%u is not %d", x, y, v);
        return FALSE;
      }
  return TRUE;
}

/* ---- meta ---- */

static RdsFrameMeta *
only_frame (GstBuffer * buf)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsFrameMeta *fm;

  g_assert_nonnull (bm);
  g_assert_cmpuint (bm->num_frames_in_batch, ==, 1);
  g_assert_cmpuint (bm->max_frames_in_batch, ==, 1);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, 1);
  fm = bm->frame_meta_list->data;
  /* a fresh frame meta (DeepStream 8.0 behaviour) */
  g_assert_cmpuint (fm->pad_index, ==, 0);
  g_assert_cmpuint (fm->source_id, ==, 0);
  g_assert_cmpuint (fm->batch_id, ==, 0);
  g_assert_cmpint (fm->frame_num, ==, 0);
  g_assert_cmpuint (fm->buf_pts, ==, 0);
  g_assert_cmpuint (fm->ntp_timestamp, ==, 0);
  g_assert_cmpuint (fm->source_frame_width, ==, 0);
  g_assert_cmpuint (fm->source_frame_height, ==, 0);
  g_assert_cmpuint (fm->pipeline_width, ==, 0);
  g_assert_cmpuint (fm->pipeline_height, ==, 0);
  g_assert_cmpuint (fm->surface_index, ==, 0);
  g_assert_false (fm->bInferDone);
  g_assert_cmpuint (fm->num_obj_meta, ==, g_list_length (fm->obj_meta_list));
  return fm;
}

static RdsObjectMeta *
find_object (RdsFrameMeta * fm, guint pad)
{
  for (GList * l = fm->obj_meta_list; l != NULL; l = l->next)
    if (((RdsObjectMeta *) l->data)->object_id == 100 + pad)
      return l->data;
  return NULL;
}

static RdsDisplayMeta *
find_display (RdsFrameMeta * fm, guint pad)
{
  for (GList * l = fm->display_meta_list; l != NULL; l = l->next)
    if (((RdsDisplayMeta *) l->data)->misc_osd_data[0] == (gint64) pad)
      return l->data;
  return NULL;
}

#define CLOSE(a, b) (ABS ((gdouble) (a) - (gdouble) (b)) < 1e-3)

/* The tag object of @pad scaled into @cell (left, top, w, h) of a SLOT_W x SLOT_H frame. */
static void
assert_object_in (RdsFrameMeta * fm, guint pad, guint left, guint top, guint w, guint h)
{
  RdsObjectMeta *om = find_object (fm, pad);
  gdouble sx = (gdouble) w / SLOT_W, sy = (gdouble) h / SLOT_H;

  g_assert_nonnull (om);
  g_assert_true (om->base_meta.batch_meta == fm->base_meta.batch_meta);
  g_assert_true (CLOSE (om->rect_params.left, left + 100 * sx));
  g_assert_true (CLOSE (om->rect_params.top, top + 100 * sy));
  g_assert_true (CLOSE (om->rect_params.width, 200 * sx));
  g_assert_true (CLOSE (om->rect_params.height, 60 * sy));
  g_assert_cmpuint (om->text_params.x_offset, ==, (guint) (left + 100 * sx));
  g_assert_cmpuint (om->text_params.y_offset, ==, (guint) (top + 80 * sy));
  g_assert_cmpuint (om->rect_params.border_width, ==, 3);
  g_assert_true (om->detector_bbox_info.org_bbox_coords.left == 1
      && om->detector_bbox_info.org_bbox_coords.height == 4);
  g_assert_cmpint (om->class_id, ==, (gint) pad);
  g_assert_cmpfloat (om->confidence, ==, 0.5f);
}

static void
assert_display_in (RdsFrameMeta * fm, guint pad, guint left, guint top, guint w, guint h)
{
  RdsDisplayMeta *dm = find_display (fm, pad);
  gdouble sx = (gdouble) w / SLOT_W, sy = (gdouble) h / SLOT_H;

  g_assert_nonnull (dm);
  g_assert_true (CLOSE (dm->rect_params[0].left, left + 10 * sx));
  g_assert_true (CLOSE (dm->rect_params[0].height, 40 * sy));
  g_assert_cmpuint (dm->rect_params[0].border_width, ==, 2);
  g_assert_cmpuint (dm->text_params[0].x_offset, ==, (guint) (left + 40 * sx));
  g_assert_cmpstr (dm->text_params[0].display_text, ==, "L");
  g_assert_cmpuint (dm->circle_params[0].xc, ==, (guint) (left + 160 * sx));
  g_assert_cmpuint (dm->circle_params[0].yc, ==, (guint) (top + 90 * sy));
  g_assert_cmpuint (dm->circle_params[0].radius, ==, (guint) (20 * MIN (sx, sy)));
}

/* ---- cpu ---- */

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsmultistreamtiler", NULL);
  guint rows, columns, width, height, gpu_id, pool;
  gint show_source, mem, compute_hw, interp;
  gboolean square, qos;
  GParamSpec *ps;

  g_assert_nonnull (e);
  g_assert_true (GST_IS_BASE_TRANSFORM (e));
  g_object_get (e, "rows", &rows, "columns", &columns, "width", &width, "height", &height, "show-source",
      &show_source, "gpu-id", &gpu_id, "nvbuf-memory-type", &mem, "compute-hw", &compute_hw,
      "interpolation-method", &interp, "square-seq-grid", &square, "buffer-pool-size", &pool, "qos", &qos, NULL);
  g_assert_cmpuint (rows, ==, 1);
  g_assert_cmpuint (columns, ==, 1);
  g_assert_cmpuint (width, ==, 1920);
  g_assert_cmpuint (height, ==, 1080);
  g_assert_cmpint (show_source, ==, -1);
  g_assert_cmpuint (gpu_id, ==, 0);
  g_assert_cmpint (mem, ==, 2);
  g_assert_cmpint (compute_hw, ==, 0);
  g_assert_cmpint (interp, ==, 6);
  g_assert_false (square);
  g_assert_cmpuint (pool, ==, 5);
  g_assert_false (qos);

  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "buffer-pool-size");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->minimum, ==, 4);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->maximum, ==, 10);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "width");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->minimum, ==, 16);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "rows");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->minimum, ==, 1);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "show-source");
  g_assert_cmpint (G_PARAM_SPEC_INT (ps)->minimum, ==, -1);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "custom-tile-config");
  g_assert_nonnull (ps);
  g_assert_true (G_IS_PARAM_SPEC_POINTER (ps));
  g_assert_true ((ps->flags & G_PARAM_WRITABLE) && !(ps->flags & G_PARAM_READABLE));

  g_object_set (e, "rows", 2u, "columns", 3u, "show-source", 4, "square-seq-grid", TRUE, "interpolation-method", 1,
      NULL);
  g_object_get (e, "rows", &rows, "columns", &columns, "show-source", &show_source, "square-seq-grid", &square,
      "interpolation-method", &interp, NULL);
  g_assert_cmpuint (rows, ==, 2);
  g_assert_cmpuint (columns, ==, 3);
  g_assert_cmpint (show_source, ==, 4);
  g_assert_true (square);
  g_assert_cmpint (interp, ==, 1);

  /* reserved: accepted, no effect, one warning */
  g_test_expect_message ("rdstiler", G_LOG_LEVEL_WARNING, "*custom-tile-config*no effect*");
  g_object_set (e, "custom-tile-config", GINT_TO_POINTER (1), NULL);
  g_object_set (e, "custom-tile-config", GINT_TO_POINTER (2), NULL);
  g_test_assert_expected_messages ();
  gst_object_unref (e);
}

static void
test_pads (void)
{
  GstElementFactory *f = gst_element_factory_find ("rdsmultistreamtiler");
  const GList *l;

  g_assert_nonnull (f);
  for (l = gst_element_factory_get_static_pad_templates (f); l != NULL; l = l->next) {
    GstStaticPadTemplate *t = l->data;
    GstCaps *caps = gst_static_caps_get (&t->static_caps);
    GstCaps *rgba = gst_caps_from_string ("video/x-raw(memory:RDS),format=RGBA,width=64,height=48");
    GstCaps *nv12 = gst_caps_from_string ("video/x-raw(memory:RDS),format=NV12,width=64,height=48");
    GstCaps *i420 = gst_caps_from_string ("video/x-raw(memory:RDS),format=I420,width=64,height=48");
    GstCaps *sys = gst_caps_from_string ("video/x-raw,format=RGBA,width=64,height=48");

    g_assert_cmpint (t->presence, ==, GST_PAD_ALWAYS);
    g_assert_true (gst_caps_can_intersect (caps, rgba));
    g_assert_true (gst_caps_can_intersect (caps, nv12));
    g_assert_false (gst_caps_can_intersect (caps, i420));
    g_assert_false (gst_caps_can_intersect (caps, sys));
    gst_caps_unref (caps);
    gst_caps_unref (rgba);
    gst_caps_unref (nv12);
    gst_caps_unref (i420);
    gst_caps_unref (sys);
  }
  gst_object_unref (f);
}

/* compute-hw=2 (VIC) fails at start, before any GPU work. */
static void
test_compute_hw_vic (void)
{
  GstElement *e = gst_element_factory_make ("rdsmultistreamtiler", NULL);
  GstBus *bus = gst_bus_new ();

  gst_element_set_bus (e, bus);
  g_object_set (e, "compute-hw", 2, NULL);
  g_assert_cmpint (gst_element_set_state (e, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpuint (count_messages (bus, GST_MESSAGE_ERROR), ==, 1);
  gst_element_set_state (e, GST_STATE_NULL);
  gst_element_set_bus (e, NULL);
  gst_object_unref (bus);
  gst_object_unref (e);
}

/* ---- gpu ---- */

/* 2 x 2 grid of 640 x 360: cell by pad_index, pixels from slot batch_id (pads given out of order). */
static void
check_grid (const gchar * format)
{
  const guint pads[] = { 2, 0, 3, 1 };
  Tiler t;
  GstBuffer *out;
  GstCaps *caps;
  GstStructure *st;
  RdsFrameMeta *fm;
  Out o;
  gint w, h, fn, fd;

  tiler_init (&t, "rows=2 columns=2 width=640 height=360", format);
  out = tile (&t, pads, 4, 1);

  caps = gst_pad_get_current_caps (t.h->sinkpad);
  st = gst_caps_get_structure (caps, 0);
  g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, 0), "memory:RDS"));
  g_assert_cmpstr (gst_structure_get_string (st, "format"), ==, format);
  g_assert_true (gst_structure_get_int (st, "width", &w) && gst_structure_get_int (st, "height", &h));
  g_assert_cmpint (w, ==, 640);
  g_assert_cmpint (h, ==, 360);
  g_assert_true (gst_structure_get_fraction (st, "framerate", &fn, &fd));
  g_assert_cmpint (fn, ==, 25);
  g_assert_cmpint (fd, ==, 1);
  gst_caps_unref (caps);

  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, 5 * GST_SECOND + 1);
  out_snapshot (out, &o);
  g_assert_cmpuint (o.width, ==, 640);
  g_assert_cmpuint (o.height, ==, 360);
  for (guint i = 0; i < 4; i++) {
    guint pad = pads[i], left = (pad % 2) * 320, top = (pad / 2) * 180;

    g_assert_true (rect_is (&o, left, top, 320, 180, SLOT_VALUE (i)));
  }
  host_image_clear (&o.img);

  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 4);
  g_assert_cmpuint (g_list_length (fm->display_meta_list), ==, 4);
  for (guint pad = 0; pad < 4; pad++) {
    assert_object_in (fm, pad, (pad % 2) * 320, (pad / 2) * 180, 320, 180);
    assert_display_in (fm, pad, (pad % 2) * 320, (pad / 2) * 180, 320, 180);
  }
  gst_buffer_unref (out);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING), ==, 0);
  tiler_clear (&t);
}

static void
test_grid_rgba (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_grid ("RGBA");
}

static void
test_grid_nv12 (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_grid ("NV12");
}

/* The input's meta is copied, never moved or changed. */
static void
test_input_meta_untouched (void)
{
  const guint pads[] = { 0, 1 };
  Tiler t;
  GstBuffer *in, *out;
  RdsBatchMeta *bm;
  RdsObjectMeta *om;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=1 columns=2 width=640 height=180", "RGBA");
  in = make_batch ("RGBA", pads, 2, 0);
  bm = gst_buffer_get_rds_batch_meta (in);
  g_assert_cmpint (gst_harness_push (t.h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  out = gst_harness_pull (t.h);
  g_assert_true (gst_buffer_get_rds_batch_meta (out) != bm);
  g_assert_cmpuint (bm->num_frames_in_batch, ==, 2);
  om = ((RdsFrameMeta *) bm->frame_meta_list->next->data)->obj_meta_list->data;
  g_assert_cmpfloat (om->rect_params.left, ==, 100);
  g_assert_cmpuint (om->text_params.x_offset, ==, 100);
  g_assert_cmpuint (((RdsFrameMeta *) bm->frame_meta_list->data)->pad_index, ==, 0);
  gst_buffer_unref (out);
  gst_buffer_unref (in);
  tiler_clear (&t);
}

/* 1000 x 700 on 2 x 3 (332 x 350 cells, 4 px strip at the right): two sources, unused cells and the strip black.
 * 1001 x 701 negotiates 1004 x 704. */
static void
check_unused (const gchar * format)
{
  const guint pads[] = { 0, 4 };
  Tiler t;
  GstBuffer *out;
  GstCaps *caps;
  Out o;
  gint w, h;

  tiler_init (&t, "rows=2 columns=3 width=1000 height=700", format);
  /* several times: the later outputs come from reused pool buffers */
  for (guint k = 0; k < 7; k++) {
    out = tile (&t, pads, 2, k);
    out_snapshot (out, &o);
    g_assert_true (rect_is (&o, 0, 0, 332, 350, SLOT_VALUE (0)));
    g_assert_true (rect_is (&o, 332, 350, 332, 350, SLOT_VALUE (1)));
    for (guint cell = 0; cell < 6; cell++)
      if (cell != 0 && cell != 4)
        g_assert_true (rect_is (&o, (cell % 3) * 332 + 100, (cell / 3) * 350 + 100, 50, 50, -1));
    g_assert_true (rect_is (&o, 996, 0, 4, 700, -1));
    host_image_clear (&o.img);
    gst_buffer_unref (out);
  }
  tiler_clear (&t);

  tiler_init (&t, "rows=2 columns=3 width=1001 height=701", format);
  out = tile (&t, pads, 2, 0);
  caps = gst_pad_get_current_caps (t.h->sinkpad);
  g_assert_true (gst_structure_get_int (gst_caps_get_structure (caps, 0), "width", &w));
  g_assert_true (gst_structure_get_int (gst_caps_get_structure (caps, 0), "height", &h));
  g_assert_cmpint (w, ==, 1004);
  g_assert_cmpint (h, ==, 704);
  gst_caps_unref (caps);
  out_snapshot (out, &o);
  g_assert_cmpuint (o.width, ==, 1004);
  g_assert_cmpuint (o.height, ==, 704);
  /* 334 x 352 cells */
  g_assert_true (rect_is (&o, 334, 352, 334, 352, SLOT_VALUE (1)));
  g_assert_true (rect_is (&o, 1002, 0, 2, 704, -1));
  host_image_clear (&o.img);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

static void
test_unused_rgba (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_unused ("RGBA");
}

static void
test_unused_nv12 (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_unused ("NV12");
}

/* Grid growth: from rds-stream-pad-added events and from frame metas; never shrunk. */
static void
test_growth (void)
{
  const guint p0[] = { 0 }, p4[] = { 4, 0 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "width=640 height=360", "RGBA");
  /* 1 x 1: one source fills the output */
  out = tile (&t, p0, 1, 0);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 640, 360, SLOT_VALUE (0)));
  host_image_clear (&o.img);
  fm = only_frame (out);
  assert_object_in (fm, 0, 0, 0, 640, 360);
  gst_buffer_unref (out);

  /* a second pad: 1 x 2 */
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_added (1)));
  out = tile (&t, p0, 1, 1);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 360, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 360, -1));
  host_image_clear (&o.img);
  assert_object_in (only_frame (out), 0, 0, 0, 320, 360);
  gst_buffer_unref (out);

  /* a frame meta with pad 4: n 5 -> 2 x 3 (212 x 180 cells: 640 / 3 = 213 -> 212) */
  out = tile (&t, p4, 2, 2);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 212, 180, 212, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 0, 0, 212, 180, SLOT_VALUE (1)));
  g_assert_true (rect_is (&o, 424, 0, 212, 180, -1));
  g_assert_true (rect_is (&o, 636, 0, 4, 360, -1));
  host_image_clear (&o.img);
  fm = only_frame (out);
  assert_object_in (fm, 4, 212, 180, 212, 180);
  assert_object_in (fm, 0, 0, 0, 212, 180);
  gst_buffer_unref (out);

  /* pad 4 ends: the grid stays 2 x 3 */
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_stream_eos (4)));
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_deleted (4)));
  out = tile (&t, p0, 1, 3);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 212, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 212, 180, 212, 180, -1));
  host_image_clear (&o.img);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

static void
test_square_seq_grid (void)
{
  const guint pads[] = { 0, 1, 2 };
  Tiler t;
  GstBuffer *out;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=1 columns=4 square-seq-grid=true width=640 height=360", "RGBA");
  out = tile (&t, pads, 3, 0);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 180, SLOT_VALUE (1)));
  g_assert_true (rect_is (&o, 0, 180, 320, 180, SLOT_VALUE (2)));
  g_assert_true (rect_is (&o, 320, 180, 320, 180, -1));
  host_image_clear (&o.img);
  assert_object_in (only_frame (out), 2, 0, 180, 320, 180);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

/* A source missing from a batch keeps its last pixels and meta until its rds-stream-eos; then its cell is black. */
static void
check_absent (const gchar * format)
{
  const guint both[] = { 0, 1 }, first[] = { 0 }, second_only[] = { 1 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  GstEvent *ev;
  gboolean saw_eos = FALSE;
  Out o;

  tiler_init (&t, "rows=1 columns=2 width=640 height=180", format);
  out = tile (&t, both, 2, 0);
  gst_buffer_unref (out);

  /* pad 1 absent twice: its cell and tag object repeat (slot 1's grey) */
  for (guint k = 1; k <= 2; k++) {
    out = tile (&t, first, 1, k);
    out_snapshot (out, &o);
    g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
    g_assert_true (rect_is (&o, 320, 0, 320, 180, SLOT_VALUE (1)));
    host_image_clear (&o.img);
    fm = only_frame (out);
    g_assert_cmpuint (fm->num_obj_meta, ==, 2);
    g_assert_cmpuint (g_list_length (fm->display_meta_list), ==, 2);
    assert_object_in (fm, 1, 320, 0, 320, 180);
    assert_display_in (fm, 1, 320, 0, 320, 180);
    gst_buffer_unref (out);
  }

  /* pad 0 absent now, pad 1 back in slot 0: cell 0 repeats slot 0's grey from the first batches */
  out = tile (&t, second_only, 1, 3);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 180, SLOT_VALUE (0)));
  host_image_clear (&o.img);
  g_assert_cmpuint (only_frame (out)->num_obj_meta, ==, 2);
  gst_buffer_unref (out);

  /* pad 1 ends: black and no meta from the next batch on; the event goes downstream */
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_stream_eos (1)));
  while ((ev = gst_harness_try_pull_event (t.h)) != NULL) {
    guint id;

    if (rds_event_parse_stream_eos (ev, &id) && id == 1)
      saw_eos = TRUE;
    gst_event_unref (ev);
  }
  g_assert_true (saw_eos);
  out = tile (&t, first, 1, 4);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 180, -1));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 1);
  g_assert_null (find_object (fm, 1));
  g_assert_null (find_display (fm, 1));
  gst_buffer_unref (out);

  /* pad 1 comes back (a new stream on the same pad) */
  out = tile (&t, both, 2, 5);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 320, 0, 320, 180, SLOT_VALUE (1)));
  host_image_clear (&o.img);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

static void
test_absent_rgba (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_absent ("RGBA");
}

static void
test_absent_nv12 (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_absent ("NV12");
}

/* An absent source's pixels survive a grid change. */
static void
test_absent_regrid (void)
{
  const guint both[] = { 0, 1 }, first[] = { 0 };
  Tiler t;
  GstBuffer *out;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "width=640 height=360", "RGBA");
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_added (0)));
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_added (1)));
  gst_buffer_unref (tile (&t, both, 2, 0));
  /* a third pad: 1 x 2 -> 1 x 3 while pad 1 is absent */
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_added (2)));
  out = tile (&t, first, 1, 1);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 212, 360, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 212, 0, 212, 360, SLOT_VALUE (1)));
  g_assert_true (rect_is (&o, 424, 0, 212, 360, -1));
  host_image_clear (&o.img);
  assert_object_in (only_frame (out), 1, 212, 0, 212, 360);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

/* show-source: one pad stretched to the output with only its meta; absent -> black and an empty frame meta;
 * changes apply from the next batch; a pad past the cells is an element error. */
static void
check_show_source (const gchar * format)
{
  const guint pads[] = { 0, 1, 2 }, first[] = { 0 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  Out o;

  tiler_init (&t, "rows=2 columns=2 width=640 height=360 show-source=1", format);
  out = tile (&t, pads, 3, 0);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 640, 360, SLOT_VALUE (1)));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 1);
  g_assert_cmpuint (g_list_length (fm->display_meta_list), ==, 1);
  assert_object_in (fm, 1, 0, 0, 640, 360);
  assert_display_in (fm, 1, 0, 0, 640, 360);
  gst_buffer_unref (out);

  /* pad 3 is a cell but has no frame: black, empty frame meta */
  g_object_set (t.h->element, "show-source", 3, NULL);
  out = tile (&t, pads, 3, 1);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 640, 360, -1));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 0);
  g_assert_null (fm->display_meta_list);
  gst_buffer_unref (out);

  /* pad 1 absent from this batch (not ended): black too */
  g_object_set (t.h->element, "show-source", 1, NULL);
  out = tile (&t, first, 1, 2);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 640, 360, -1));
  host_image_clear (&o.img);
  g_assert_cmpuint (only_frame (out)->num_obj_meta, ==, 0);
  gst_buffer_unref (out);

  /* back to the grid: pad 1's cell still has its last pixels */
  g_object_set (t.h->element, "show-source", -1, NULL);
  out = tile (&t, first, 1, 3);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 180, SLOT_VALUE (1)));
  g_assert_true (rect_is (&o, 0, 180, 320, 180, SLOT_VALUE (2)));
  host_image_clear (&o.img);
  g_assert_cmpuint (only_frame (out)->num_obj_meta, ==, 3);
  gst_buffer_unref (out);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR), ==, 0);

  /* 4 cells: show-source 4 is an error, at the next batch */
  g_object_set (t.h->element, "show-source", 4, NULL);
  g_assert_cmpint (gst_harness_push (t.h, make_batch (format, pads, 3, 4)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR), ==, 1);
  tiler_clear (&t);

  /* also at the first batch */
  tiler_init (&t, "rows=2 columns=2 width=640 height=360 show-source=7", format);
  g_assert_cmpint (gst_harness_push (t.h, make_batch (format, pads, 3, 0)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR), ==, 1);
  tiler_clear (&t);
}

static void
test_show_source_rgba (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_show_source ("RGBA");
}

static void
test_show_source_nv12 (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_show_source ("NV12");
}

/* A pad_index the grid cannot hold (cells would vanish) is skipped with one warning (ours). */
static void
test_out_of_grid (void)
{
  const guint pads[] = { 0, 1000 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=1 columns=1 width=16 height=16", "RGBA");
  for (guint k = 0; k < 3; k++) {
    out = tile (&t, pads, 2, k);
    out_snapshot (out, &o);
    g_assert_true (rect_is (&o, 0, 0, 16, 16, SLOT_VALUE (0)));
    host_image_clear (&o.img);
    fm = only_frame (out);
    g_assert_cmpuint (fm->num_obj_meta, ==, 1);
    g_assert_nonnull (find_object (fm, 0));
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_WARNING), ==, 1);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR), ==, 0);
  tiler_clear (&t);
}

/* An empty batch (no frames) clears the output: black with one empty frame meta (DeepStream's docs: the mux sends
 * one to clear the tiler's output). The caches stay: the next batch still repeats absent sources. */
static void
test_empty_batch (void)
{
  const guint first[] = { 0 }, second[] = { 1 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=1 columns=2 width=640 height=180", "NV12");
  out = tile (&t, NULL, 0, 0);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 640, 180, -1));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 0);
  g_assert_null (fm->display_meta_list);
  gst_buffer_unref (out);

  gst_buffer_unref (tile (&t, first, 1, 1));
  out = tile (&t, NULL, 0, 2);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 640, 180, -1));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 0);
  g_assert_null (fm->display_meta_list);
  gst_buffer_unref (out);

  /* pad 0 absent from the next batch: its last pixels and meta are back */
  out = tile (&t, second, 1, 3);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 180, SLOT_VALUE (0)));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 2);
  assert_object_in (fm, 0, 0, 0, 320, 180);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

/* rds-pad-deleted alone ends a pad, as stream-eos does. */
static void
test_pad_deleted (void)
{
  const guint both[] = { 0, 1 }, first[] = { 0 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=1 columns=2 width=640 height=180", "RGBA");
  gst_buffer_unref (tile (&t, both, 2, 0));
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_deleted (1)));
  out = tile (&t, first, 1, 1);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 180, -1));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 1);
  g_assert_null (find_object (fm, 1));
  g_assert_null (find_display (fm, 1));
  gst_buffer_unref (out);
  tiler_clear (&t);
}

/* Two frames of one pad in a batch: both drawn, the later on top, and only the later one's meta kept (ours). */
static void
test_duplicate_pad (void)
{
  const guint pads[] = { 0, 0 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=1 columns=2 width=640 height=180", "RGBA");
  out = tile (&t, pads, 2, 0);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (1)));
  host_image_clear (&o.img);
  fm = only_frame (out);
  g_assert_cmpuint (fm->num_obj_meta, ==, 1);
  g_assert_cmpint (((RdsObjectMeta *) fm->obj_meta_list->data)->misc_obj_info[0], ==, 1);
  g_assert_cmpuint (g_list_length (fm->display_meta_list), ==, 1);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

/* The merged lists are in pad order (ours), whatever the batch order. */
static void
test_merged_order (void)
{
  const guint pads[] = { 2, 0, 3, 1 };
  Tiler t;
  GstBuffer *out;
  RdsFrameMeta *fm;
  GList *l;
  guint i;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=2 columns=2 width=640 height=360", "RGBA");
  out = tile (&t, pads, 4, 0);
  fm = only_frame (out);
  for (l = fm->obj_meta_list, i = 0; l != NULL; l = l->next, i++)
    g_assert_cmpuint (((RdsObjectMeta *) l->data)->object_id, ==, 100 + i);
  g_assert_cmpuint (i, ==, 4);
  for (l = fm->display_meta_list, i = 0; l != NULL; l = l->next, i++)
    g_assert_cmpint (((RdsDisplayMeta *) l->data)->misc_osd_data[0], ==, i);
  g_assert_cmpuint (i, ==, 4);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

/* A flush forgets every cached cell and meta (ours). */
static void
test_flush (void)
{
  const guint both[] = { 0, 1 }, first[] = { 0 };
  Tiler t;
  GstBuffer *out;
  GstSegment seg;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=1 columns=2 width=640 height=180", "RGBA");
  gst_buffer_unref (tile (&t, both, 2, 0));
  g_assert_true (gst_harness_push_event (t.h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (t.h, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (t.h, gst_event_new_segment (&seg)));
  out = tile (&t, first, 1, 1);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 320, 180, SLOT_VALUE (0)));
  g_assert_true (rect_is (&o, 320, 0, 320, 180, -1));
  host_image_clear (&o.img);
  g_assert_cmpuint (only_frame (out)->num_obj_meta, ==, 1);
  gst_buffer_unref (out);
  tiler_clear (&t);
}

/* A system-memory batch is an element error (ours). */
static void
test_system_memory (void)
{
  const guint first[] = { 0 };
  GstAllocator *sys_alloc;
  Tiler t;

  RDS_TEST_REQUIRE_GPU ();
  sys_alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  batch_alloc = sys_alloc;
  tiler_init (&t, "width=320 height=180", "RGBA");
  g_assert_cmpint (gst_harness_push (t.h, make_batch ("RGBA", first, 1, 0)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR), ==, 1);
  tiler_clear (&t);
  batch_alloc = dev_alloc;
  gst_object_unref (sys_alloc);
}

/* A pad announced before the caps that the grid cannot hold: skipped with one warning, like one seen later. */
static void
test_out_of_grid_before_caps (void)
{
  const guint first[] = { 0 };
  Tiler t;
  GstBuffer *out;
  Out o;

  RDS_TEST_REQUIRE_GPU ();
  tiler_init_no_caps (&t, "rows=1 columns=1 width=16 height=16", "RGBA");
  g_assert_true (gst_harness_push_event (t.h, gst_event_new_stream_start ("tiler-test")));
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_added (0)));
  g_assert_true (gst_harness_push_event (t.h, rds_event_new_pad_added (1000)));
  tiler_set_caps (&t);
  out = tile (&t, first, 1, 0);
  out_snapshot (out, &o);
  g_assert_true (rect_is (&o, 0, 0, 16, 16, SLOT_VALUE (0)));
  host_image_clear (&o.img);
  gst_buffer_unref (out);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_WARNING), ==, 1);
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR), ==, 0);
  tiler_clear (&t);
}

/* 500 batches of 4 pads, each present with a fixed pseudo-random pattern; every output keeps the last pixels of
 * every pad and one tag object per pad seen, except the empty batches, which are black and empty. */
static void
test_run_500 (void)
{
  Tiler t;
  guint32 rng = 12345;
  guint8 last[4] = { 0 };
  gboolean seen[4] = { FALSE };

  RDS_TEST_REQUIRE_GPU ();
  tiler_init (&t, "rows=2 columns=2 width=320 height=180", "RGBA");
  for (guint k = 0; k < 500; k++) {
    guint pads[4], n = 0, n_seen = 0;
    GstBuffer *out;
    Out o;

    for (guint p = 0; p < 4; p++) {
      rng = rng * 1103515245u + 12345u;
      if ((rng >> 16) % 3 != 0)
        pads[n++] = p;
    }
    for (guint i = 0; i < n; i++) {
      last[pads[i]] = SLOT_VALUE (i);
      seen[pads[i]] = TRUE;
    }
    out = tile (&t, pads, n, k);
    if (k % 50 == 0 || k == 499 || n == 0) {
      out_snapshot (out, &o);
      for (guint p = 0; p < 4; p++)
        g_assert_true (seen[p] && n > 0 ? pixel_is (&o, (p % 2) * 160 + 80, (p / 2) * 90 + 45, last[p]) :
            pixel_is_black (&o, (p % 2) * 160 + 80, (p / 2) * 90 + 45));
      host_image_clear (&o.img);
    }
    for (guint p = 0; p < 4; p++)
      n_seen += seen[p];
    /* an empty batch clears the output */
    g_assert_cmpuint (only_frame (out)->num_obj_meta, ==, n > 0 ? n_seen : 0);
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (count_messages (t.bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING), ==, 0);
  tiler_clear (&t);
}

int
main (int argc, char **argv)
{
  gint ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  dev_alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  batch_alloc = dev_alloc;

  g_test_add_func ("/cpu/tiler/properties", test_properties);
  g_test_add_func ("/cpu/tiler/pads", test_pads);
  g_test_add_func ("/cpu/tiler/compute-hw-vic", test_compute_hw_vic);
  g_test_add_func ("/gpu/tiler/grid-rgba", test_grid_rgba);
  g_test_add_func ("/gpu/tiler/grid-nv12", test_grid_nv12);
  g_test_add_func ("/gpu/tiler/input-meta-untouched", test_input_meta_untouched);
  g_test_add_func ("/gpu/tiler/unused-rgba", test_unused_rgba);
  g_test_add_func ("/gpu/tiler/unused-nv12", test_unused_nv12);
  g_test_add_func ("/gpu/tiler/growth", test_growth);
  g_test_add_func ("/gpu/tiler/square-seq-grid", test_square_seq_grid);
  g_test_add_func ("/gpu/tiler/absent-rgba", test_absent_rgba);
  g_test_add_func ("/gpu/tiler/absent-nv12", test_absent_nv12);
  g_test_add_func ("/gpu/tiler/absent-regrid", test_absent_regrid);
  g_test_add_func ("/gpu/tiler/show-source-rgba", test_show_source_rgba);
  g_test_add_func ("/gpu/tiler/show-source-nv12", test_show_source_nv12);
  g_test_add_func ("/gpu/tiler/out-of-grid", test_out_of_grid);
  g_test_add_func ("/gpu/tiler/empty-batch", test_empty_batch);
  g_test_add_func ("/gpu/tiler/pad-deleted", test_pad_deleted);
  g_test_add_func ("/gpu/tiler/duplicate-pad", test_duplicate_pad);
  g_test_add_func ("/gpu/tiler/merged-order", test_merged_order);
  g_test_add_func ("/gpu/tiler/flush", test_flush);
  g_test_add_func ("/gpu/tiler/system-memory", test_system_memory);
  g_test_add_func ("/gpu/tiler/out-of-grid-before-caps", test_out_of_grid_before_caps);
  g_test_add_func ("/gpu/tiler/run-500", test_run_500);
  ret = g_test_run ();

  gst_object_unref (dev_alloc);
  gst_deinit ();
  return ret;
}
