/* rdsosd: factory, properties, caps and accepted no-ops (cpu); drawing through the element on memory:RDS device
 * surfaces: single frames, batches (each frame on its own surface), demuxed read-only views (drawn on a private
 * copy, the batch untouched), tracker-created objects, the clock, clamped borders (gpu, skipped without a ROCm
 * device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"

#include <gst/check/gstharness.h>
#include <rds/rds.h>
#include <string.h>

#define W 320
#define H 240
#define GREY 128

static GstAllocator *dev_alloc;

/* ---- cpu ---- */

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsosd", NULL);
  gboolean text, bbox, clock, mask, blur, qos;
  guint size, x, y, color, gpu;
  gint mode;
  gchar *font, *ids;
  GParamSpec *ps;
  GEnumClass *ec;

  g_assert_nonnull (e);
  g_assert_true (GST_IS_BASE_TRANSFORM (e));
  /* defaults as gst-inspect shows them for DeepStream 8.0 */
  g_object_get (e, "display-text", &text, "display-bbox", &bbox, "display-clock", &clock, "display-mask", &mask,
      "clock-font", &font, "clock-font-size", &size, "x-clock-offset", &x, "y-clock-offset", &y, "clock-color",
      &color, "process-mode", &mode, "gpu-id", &gpu, "blur-bbox", &blur, "blur-on-gie-class-ids", &ids, "qos", &qos,
      NULL);
  g_assert_true (text);
  g_assert_true (bbox);
  g_assert_false (clock);
  g_assert_false (mask);
  g_assert_null (font);
  g_assert_cmpuint (size, ==, 0);
  g_assert_cmpuint (x, ==, 0);
  g_assert_cmpuint (y, ==, 0);
  g_assert_cmpuint (color, ==, 0);
  g_assert_cmpint (mode, ==, 1);
  g_assert_cmpuint (gpu, ==, 0);
  g_assert_false (blur);
  g_assert_null (ids);
  g_assert_false (qos);

  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "clock-font-size");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->maximum, ==, 60);
  g_assert_true (ps->flags & GST_PARAM_MUTABLE_READY);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "clock-color");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->maximum, ==, G_MAXUINT);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "x-clock-offset");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->maximum, ==, G_MAXUINT);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "display-text");
  g_assert_false (ps->flags & GST_PARAM_MUTABLE_READY);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "process-mode");
  ec = g_type_class_ref (ps->value_type);
  g_assert_cmpstr (g_enum_get_value (ec, 0)->value_nick, ==, "MODE_CPU");
  g_assert_cmpstr (g_enum_get_value (ec, 1)->value_nick, ==, "MODE_GPU");
  g_assert_null (g_enum_get_value (ec, 2));
  g_type_class_unref (ec);

  g_object_set (e, "display-text", FALSE, "display-bbox", FALSE, "display-clock", TRUE, "clock-font", "Sans",
      "clock-font-size", 20u, "x-clock-offset", 7u, "y-clock-offset", 9u, "clock-color", 0x00ff00ffu,
      "process-mode", 0, "gpu-id", 1u, NULL);
  g_object_get (e, "display-text", &text, "display-bbox", &bbox, "display-clock", &clock, "clock-font", &font,
      "clock-font-size", &size, "x-clock-offset", &x, "y-clock-offset", &y, "clock-color", &color, "process-mode",
      &mode, "gpu-id", &gpu, NULL);
  g_assert_false (text);
  g_assert_false (bbox);
  g_assert_true (clock);
  g_assert_cmpstr (font, ==, "Sans");
  g_assert_cmpuint (size, ==, 20);
  g_assert_cmpuint (x, ==, 7);
  g_assert_cmpuint (y, ==, 9);
  g_assert_cmpuint (color, ==, 0x00ff00ffu);
  g_assert_cmpint (mode, ==, 0);
  g_assert_cmpuint (gpu, ==, 1);
  g_free (font);
  gst_object_unref (e);
}

static void
test_noops_warn_once (void)
{
  GstElement *e = gst_element_factory_make ("rdsosd", NULL);
  gboolean blur, mask;
  gchar *ids;

  /* accepted, stored, one warning each */
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_WARNING, "*display-mask*no effect*");
  g_object_set (e, "display-mask", TRUE, NULL);
  g_object_set (e, "display-mask", FALSE, "display-mask", TRUE, NULL);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_WARNING, "*blur-bbox*no effect*");
  g_object_set (e, "blur-bbox", TRUE, NULL);
  g_object_set (e, "blur-bbox", TRUE, NULL);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_WARNING, "*blur-on-gie-class-ids*no effect*");
  g_object_set (e, "blur-on-gie-class-ids", "1,1", NULL);
  g_object_set (e, "blur-on-gie-class-ids", "1,2", NULL);
  g_test_assert_expected_messages ();
  g_object_get (e, "blur-bbox", &blur, "display-mask", &mask, "blur-on-gie-class-ids", &ids, NULL);
  g_assert_true (blur);
  g_assert_true (mask);
  g_assert_cmpstr (ids, ==, "1,2");
  g_free (ids);
  gst_object_unref (e);
}

static void
test_caps (void)
{
  GstElement *e = gst_element_factory_make ("rdsosd", NULL);
  GstPad *sink = gst_element_get_static_pad (e, "sink");
  GstCaps *templ = gst_pad_get_pad_template_caps (sink);
  GstCaps *rgba = gst_caps_from_string ("video/x-raw(memory:RDS),format=RGBA,width=320,height=240,framerate=25/1");
  GstCaps *nv12 = gst_caps_from_string ("video/x-raw(memory:RDS),format=NV12,width=320,height=240,framerate=25/1");
  GstCaps *sys = gst_caps_from_string ("video/x-raw,format=RGBA,width=320,height=240,framerate=25/1");

  /* RGBA memory:RDS only (ours: DeepStream 8.0's GPU mode also takes NV12) */
  g_assert_true (gst_caps_can_intersect (templ, rgba));
  g_assert_false (gst_caps_can_intersect (templ, nv12));
  g_assert_false (gst_caps_can_intersect (templ, sys));
  gst_caps_unref (sys);
  gst_caps_unref (nv12);
  gst_caps_unref (rgba);
  gst_caps_unref (templ);
  gst_object_unref (sink);
  gst_object_unref (e);
}

/* ---- gpu: buffers ---- */

static RdsOSD_RectParams
box_at (gfloat left, gfloat top, guint bw)
{
  return (RdsOSD_RectParams) {.left = left,.top = top,.width = 60,.height = 40,.border_width = bw,
    .border_color = {1, 0, 0, 1}
  };
}

/* A device batch of @n grey RGBA slots, slot i with a frame meta (batch_id i) holding one object: a red 3 px box at
 * (20 + 80 i, 30). Without @with_meta, no batch meta at all. */
static GstBuffer *
make_batch_full (guint n, gboolean with_meta)
{
  RdsBufSurfaceCreateParams cp = {.width = W,.height = H,.layout = RDS_BUF_LAYOUT_PITCH,
    .colorFormat = RDS_BUF_COLOR_FORMAT_RGBA
  };
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (n);
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (dev_alloc, n, &cp));
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);
    RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);

    g_assert_cmpint (rds_buf_surface_memset (s, i, 0, GREY), ==, 0);
    fm->batch_id = i;
    fm->pad_index = i;
    fm->source_id = i;
    rds_add_frame_meta_to_batch (bm, fm);
    om->rect_params = box_at (20 + 80 * i, 30, 3);
    om->object_id = RDS_UNTRACKED_OBJECT_ID;
    om->confidence = 0.8f;
    rds_add_obj_meta_to_frame (fm, om, NULL);
  }
  s->numFilled = n;
  if (with_meta)
    g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  else
    rds_destroy_batch_meta (bm);
  GST_BUFFER_PTS (buf) = 0;
  return buf;
}

static GstBuffer *
make_batch (guint n)
{
  return make_batch_full (n, TRUE);
}

static RdsFrameMeta *
frame_of (GstBuffer * buf, guint i)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);

  g_assert_nonnull (bm);
  return g_list_nth_data (bm->frame_meta_list, i);
}

typedef struct
{
  GstHarness *h;
  GstBus *bus;
} Osd;

static void
osd_init (Osd * o, const gchar * props)
{
  gchar *desc = g_strdup_printf ("rdsosd %s", props != NULL ? props : "");
  GError *err = NULL;
  GstElement *e = gst_parse_launch (desc, &err);

  g_assert_no_error (err);
  o->bus = gst_bus_new ();
  gst_element_set_bus (e, o->bus);
  o->h = gst_harness_new_with_element (e, "sink", "src");
  gst_object_unref (e);
  gst_harness_set_src_caps_str (o->h, "video/x-raw(memory:RDS),format=RGBA,width=320,height=240,framerate=25/1");
  g_free (desc);
}

static void
osd_clear (Osd * o)
{
  gst_element_set_bus (o->h->element, NULL);
  gst_bus_set_flushing (o->bus, TRUE);
  gst_harness_teardown (o->h);
  gst_object_unref (o->bus);
}

static void
test_mutable_ready (void)
{
  Osd o;
  guint size, x, color;
  gint mode;
  gboolean text;

  /* the properties gst-inspect marks "changeable only in NULL or READY" keep their value while PLAYING, with a
   * warning; the others change */
  osd_init (&o, NULL);
  g_assert_cmpint (GST_STATE (o.h->element), ==, GST_STATE_PLAYING);
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_WARNING, "*clock-font-size*NULL or READY*");
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_WARNING, "*x-clock-offset*NULL or READY*");
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_WARNING, "*clock-color*NULL or READY*");
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_WARNING, "*process-mode*NULL or READY*");
  g_object_set (o.h->element, "clock-font-size", 20u, "x-clock-offset", 5u, "clock-color", 0x00ff00ffu,
      "process-mode", 0, "display-text", FALSE, NULL);
  g_test_assert_expected_messages ();
  g_object_get (o.h->element, "clock-font-size", &size, "x-clock-offset", &x, "clock-color", &color,
      "process-mode", &mode, "display-text", &text, NULL);
  g_assert_cmpuint (size, ==, 0);
  g_assert_cmpuint (x, ==, 0);
  g_assert_cmpuint (color, ==, 0);
  g_assert_cmpint (mode, ==, 1);
  g_assert_false (text);
  osd_clear (&o);
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

static GstBuffer *
osd_run (Osd * o, GstBuffer * in)
{
  g_assert_cmpint (gst_harness_push (o->h, in), ==, GST_FLOW_OK);
  return gst_harness_pull (o->h);
}

static const guint8 *
pixel (const HostImage * img, guint x, guint y)
{
  return img->im.plane[0] + (gsize) y * img->im.pitch[0] + 4 * x;
}

static gboolean
is_rgb (const HostImage * img, guint x, guint y, guint8 r, guint8 g, guint8 b)
{
  const guint8 *p = pixel (img, x, y);

  return p[0] == r && p[1] == g && p[2] == b;
}

static gboolean
is_grey (const HostImage * img, guint x, guint y)
{
  return is_rgb (img, x, y, GREY, GREY, GREY);
}

static guint
count_rgb (const HostImage * img, guint x0, guint y0, guint x1, guint y1, guint8 r, guint8 g, guint8 b)
{
  guint n = 0;

  for (guint y = y0; y <= y1; y++)
    for (guint x = x0; x <= x1; x++)
      n += is_rgb (img, x, y, r, g, b);
  return n;
}

/* Slot @i shows exactly the box of slot @box (left edge 20 + 80 box, 3 px band 19..21), nothing of the others. */
static void
assert_slot_box (const HostImage * img, guint box, guint n_boxes)
{
  for (guint k = 0; k < n_boxes; k++) {
    guint L = 20 + 80 * k;

    g_assert_cmpint (is_rgb (img, L, 50, 255, 0, 0), ==, k == box);
    g_assert_cmpint (is_rgb (img, L - 1, 50, 255, 0, 0), ==, k == box);
    g_assert_cmpint (is_rgb (img, L + 1, 50, 255, 0, 0), ==, k == box);
    g_assert_true (is_grey (img, L - 2, 50));
    g_assert_true (is_grey (img, L + 2, 50));
    g_assert_true (is_grey (img, L + 30, 50));
  }
}

static void
test_single (void)
{
  Osd o;
  GstBuffer *in, *out;
  HostImage img;

  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, NULL);
  in = make_batch (1);
  out = osd_run (&o, in);
  snapshot (rds_buffer_get_surface (out), 0, &img);
  assert_slot_box (&img, 0, 1);
  /* the meta passes through unchanged */
  g_assert_cmpfloat (((RdsObjectMeta *) frame_of (out, 0)->obj_meta_list->data)->rect_params.left, ==, 20);
  host_image_clear (&img);
  gst_buffer_unref (out);
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING), ==, 0);
  osd_clear (&o);
}

static void
test_batch (void)
{
  Osd o;
  GstBuffer *out;
  RdsBufSurface *s;

  /* ours: each frame's meta on its own surface (DeepStream 8.0 draws them all on surface 0) */
  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, NULL);
  out = osd_run (&o, make_batch (3));
  s = rds_buffer_get_surface (out);
  g_assert_cmpuint (s->numFilled, ==, 3);
  for (guint i = 0; i < 3; i++) {
    HostImage img;

    snapshot (s, i, &img);
    assert_slot_box (&img, i, 3);
    host_image_clear (&img);
  }
  gst_buffer_unref (out);
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING), ==, 0);
  osd_clear (&o);
}

static void
test_view_copied (void)
{
  Osd o;
  GstBuffer *batch, *view, *out;
  RdsBatchMeta *view_meta;
  HostImage img;

  /* a demuxed view (read-only memory of a batch slot, with a one-frame meta, batch_id 0) is drawn on a private copy;
   * the batch keeps its pixels */
  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, NULL);
  batch = make_batch (2);
  view = rds_buffer_new_surface_view (batch, 1);
  g_assert_nonnull (view);
  view_meta = rds_create_batch_meta (1);
  g_assert_nonnull (rds_copy_frame_meta_to_batch (frame_of (batch, 1), view_meta));
  ((RdsFrameMeta *) view_meta->frame_meta_list->data)->batch_id = 0;
  g_assert_nonnull (rds_buffer_add_batch_meta (view, view_meta));

  out = osd_run (&o, view);
  snapshot (rds_buffer_get_surface (out), 0, &img);
  assert_slot_box (&img, 1, 2);
  host_image_clear (&img);
  gst_buffer_unref (out);

  for (guint i = 0; i < 2; i++) {
    snapshot (rds_buffer_get_surface (batch), i, &img);
    g_assert_cmpuint (count_rgb (&img, 0, 0, W - 1, H - 1, GREY, GREY, GREY), ==, W * H);
    host_image_clear (&img);
  }
  gst_buffer_unref (batch);
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING), ==, 0);
  osd_clear (&o);
}

static void
test_shared_copied (void)
{
  Osd o;
  GstBuffer *in, *keep, *out;
  HostImage img;

  /* a buffer whose memory someone else holds too (a tee, say) is drawn on a copy */
  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, NULL);
  in = make_batch (1);
  keep = gst_buffer_ref (in);
  out = osd_run (&o, in);
  snapshot (rds_buffer_get_surface (out), 0, &img);
  assert_slot_box (&img, 0, 1);
  host_image_clear (&img);
  snapshot (rds_buffer_get_surface (keep), 0, &img);
  g_assert_cmpuint (count_rgb (&img, 0, 0, W - 1, H - 1, GREY, GREY, GREY), ==, W * H);
  host_image_clear (&img);
  gst_buffer_unref (out);
  gst_buffer_unref (keep);
  osd_clear (&o);
}

static void
test_tracker_object (void)
{
  Osd o;
  GstBuffer *in, *out;
  RdsObjectMeta *om;
  HostImage img;

  /* a tracker-created object (confidence -0.1) is drawn like any other (DeepStream 8.0 behaviour) */
  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, NULL);
  in = make_batch (1);
  om = frame_of (in, 0)->obj_meta_list->data;
  om->object_id = 7;
  om->confidence = -0.1f;
  out = osd_run (&o, in);
  snapshot (rds_buffer_get_surface (out), 0, &img);
  assert_slot_box (&img, 0, 1);
  host_image_clear (&img);
  gst_buffer_unref (out);
  osd_clear (&o);
}

/* Pixels drawn in the clock's region (offsets 100, 150; the frame's object box lies above it) of each of the @n
 * slots of the output, and how many of them are opaque red (@red, may be NULL). */
static guint
clock_pixels_full (const gchar * props, guint n, gboolean with_meta, guint slot, guint * red)
{
  Osd o;
  GstBuffer *out;
  HostImage img;
  guint drawn;

  osd_init (&o, props);
  out = osd_run (&o, make_batch_full (n, with_meta));
  snapshot (rds_buffer_get_surface (out), slot, &img);
  drawn = (W - 100) * (H - 150) - count_rgb (&img, 100, 150, W - 1, H - 1, GREY, GREY, GREY);
  if (red != NULL)
    *red = count_rgb (&img, 100, 150, W - 1, H - 1, 255, 0, 0);
  host_image_clear (&img);
  gst_buffer_unref (out);
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING), ==, 0);
  osd_clear (&o);
  return drawn;
}

static guint
clock_pixels (const gchar * props)
{
  return clock_pixels_full (props, 1, TRUE, 0, NULL);
}

static void
test_clock (void)
{
  guint red = 0, drawn;

  RDS_TEST_REQUIRE_GPU ();
  /* clock-color never set: opaque red; 0: invisible; display-text=0 hides it (DeepStream 8.0 behaviour) */
  drawn = clock_pixels_full ("display-clock=true x-clock-offset=100 y-clock-offset=150", 1, TRUE, 0, &red);
  g_assert_cmpuint (drawn, >, 30);
  g_assert_cmpuint (red, >, 20);
  /* the antialiased edges are red over grey: no other hue */
  drawn = clock_pixels_full ("display-clock=true x-clock-offset=100 y-clock-offset=150 clock-color=0x00ff00ff", 1,
      TRUE, 0, &red);
  g_assert_cmpuint (drawn, >, 30);
  g_assert_cmpuint (red, ==, 0);
  g_assert_cmpuint (clock_pixels ("display-clock=true x-clock-offset=100 y-clock-offset=150 clock-color=0"), ==, 0);
  g_assert_cmpuint (clock_pixels ("display-clock=true x-clock-offset=100 y-clock-offset=150 display-text=false"),
      ==, 0);
  g_assert_cmpuint (clock_pixels ("display-clock=false x-clock-offset=100 y-clock-offset=150"), ==, 0);
}

static void
test_no_frame_meta (void)
{
  Osd o;
  GstBuffer *out;
  HostImage img;
  guint red = 0;

  /* ours: a batch without frame meta passes through, and gets the clock on every frame */
  RDS_TEST_REQUIRE_GPU ();
  for (guint slot = 0; slot < 3; slot++) {
    g_assert_cmpuint (clock_pixels_full ("display-clock=true x-clock-offset=100 y-clock-offset=150", 3, FALSE, slot,
            &red), >, 30);
    g_assert_cmpuint (red, >, 20);
  }
  osd_init (&o, NULL);
  out = osd_run (&o, make_batch_full (3, FALSE));
  for (guint slot = 0; slot < 3; slot++) {
    snapshot (rds_buffer_get_surface (out), slot, &img);
    g_assert_cmpuint (count_rgb (&img, 0, 0, W - 1, H - 1, GREY, GREY, GREY), ==, W * H);
    host_image_clear (&img);
  }
  gst_buffer_unref (out);
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_ERROR | GST_MESSAGE_WARNING), ==, 0);
  osd_clear (&o);
}

static void
test_batch_id_outside (void)
{
  Osd o;
  GstBuffer *in, *out;
  HostImage img;

  /* ours: a frame meta whose batch_id is outside the batch is skipped, with one element warning */
  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, NULL);
  for (guint k = 0; k < 3; k++) {
    in = make_batch (1);
    frame_of (in, 0)->batch_id = 5;
    out = osd_run (&o, in);
    snapshot (rds_buffer_get_surface (out), 0, &img);
    g_assert_cmpuint (count_rgb (&img, 0, 0, W - 1, H - 1, GREY, GREY, GREY), ==, W * H);
    host_image_clear (&img);
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_WARNING), ==, 1);
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_ERROR), ==, 0);
  osd_clear (&o);
}

static void
test_border_clamped (void)
{
  Osd o;
  GstBuffer *in, *out;
  HostImage img;

  /* ours: border_width 40 drawn 32 wide (L-15 .. L+16) with one element warning; DeepStream 8.0 stops */
  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, NULL);
  for (guint k = 0; k < 2; k++) {
    in = make_batch (1);
    ((RdsObjectMeta *) frame_of (in, 0)->obj_meta_list->data)->rect_params = box_at (100, 100, 40);
    out = osd_run (&o, in);
    snapshot (rds_buffer_get_surface (out), 0, &img);
    g_assert_true (is_grey (&img, 84, 120));
    g_assert_true (is_rgb (&img, 85, 120, 255, 0, 0));
    g_assert_true (is_rgb (&img, 116, 120, 255, 0, 0));
    g_assert_true (is_grey (&img, 117, 120));
    host_image_clear (&img);
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_WARNING), ==, 1);
  g_assert_cmpuint (count_messages (o.bus, GST_MESSAGE_ERROR), ==, 0);
  osd_clear (&o);
}

static void
test_switches (void)
{
  Osd o;
  GstBuffer *out;
  HostImage img;

  RDS_TEST_REQUIRE_GPU ();
  osd_init (&o, "display-bbox=false");
  out = osd_run (&o, make_batch (1));
  snapshot (rds_buffer_get_surface (out), 0, &img);
  g_assert_cmpuint (count_rgb (&img, 0, 0, W - 1, H - 1, GREY, GREY, GREY), ==, W * H);
  host_image_clear (&img);
  gst_buffer_unref (out);
  osd_clear (&o);
}

int
main (int argc, char **argv)
{
  gint ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  dev_alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);

  g_test_add_func ("/cpu/osd-element/properties", test_properties);
  g_test_add_func ("/cpu/osd-element/noops-warn-once", test_noops_warn_once);
  g_test_add_func ("/cpu/osd-element/caps", test_caps);
  g_test_add_func ("/cpu/osd-element/mutable-ready", test_mutable_ready);
  g_test_add_func ("/gpu/osd-element/single", test_single);
  g_test_add_func ("/gpu/osd-element/batch", test_batch);
  g_test_add_func ("/gpu/osd-element/view-copied", test_view_copied);
  g_test_add_func ("/gpu/osd-element/shared-copied", test_shared_copied);
  g_test_add_func ("/gpu/osd-element/tracker-object", test_tracker_object);
  g_test_add_func ("/gpu/osd-element/clock", test_clock);
  g_test_add_func ("/gpu/osd-element/no-frame-meta", test_no_frame_meta);
  g_test_add_func ("/gpu/osd-element/batch-id-outside", test_batch_id_outside);
  g_test_add_func ("/gpu/osd-element/border-clamped", test_border_clamped);
  g_test_add_func ("/gpu/osd-element/switches", test_switches);
  ret = g_test_run ();

  gst_object_unref (dev_alloc);
  gst_deinit ();
  return ret;
}
