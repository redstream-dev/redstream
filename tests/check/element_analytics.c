/* rdsanalytics: registration, the properties with their types, ranges, defaults and state flags, start errors (cpu);
 * and on scripted object meta in device-memory batches shaped as rdsstreammux makes them (gpu): pass-through, the
 * frame and object metas, pad_index against source_id, reloading config-file while playing, stream resets, partial
 * batches, sources removed and added at runtime, the untracked-object warning, a change of the mux output size and
 * the display meta of each osd-mode.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"

#include <gst/check/gstharness.h>
#include <glib/gstdio.h>
#include <rds/rds.h>
#include <rds/rds-analytics-meta.h>
#include <string.h>

#define W 640
#define H 480
#define IN_CAPS "video/x-raw(memory:RDS), format=NV12, width=640, height=480, framerate=30/1"

/* An ROI over the left half and a horizontal line at y = 200 crossed downwards, on stream 0 only. */
#define CONFIG \
  "[property]\nenable=1\nconfig-width=640\nconfig-height=480\nosd-mode=0\n\n" \
  "[roi-filtering-stream-0]\nenable=1\nroi-RF=0;0;320;0;320;480;0;480\n\n" \
  "[line-crossing-stream-0]\nenable=1\nline-crossing-Down=320;100;320;300;0;200;640;200\n"

/* Another rule set on stream 0: one line, named otherwise. */
#define CONFIG_OTHER \
  "[property]\nenable=1\nconfig-width=640\nconfig-height=480\nosd-mode=0\n\n" \
  "[line-crossing-stream-0]\nenable=1\nline-crossing-Exit=320;100;320;300;0;200;640;200\n"

/* The same line on streams 0 and 1. */
#define CONFIG_TWO \
  "[property]\nenable=1\nconfig-width=640\nconfig-height=480\nosd-mode=0\n\n" \
  "[line-crossing-stream-0]\nenable=1\nline-crossing-Down=320;100;320;300;0;200;640;200\n\n" \
  "[line-crossing-stream-1]\nenable=1\nline-crossing-Down=320;100;320;300;0;200;640;200\n"

static gchar *work_dir;
static GstAllocator *dev_alloc;
static guint64 next_pts;

/* ---- registration and properties (cpu) ---- */

static void
test_factory (void)
{
  GstElementFactory *factory = gst_element_factory_find ("rdsanalytics");

  g_assert_nonnull (factory);
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_LONGNAME), ==,
      "Redstream video analytics");
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS), ==, "Filter/Video");
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), ==, GST_RANK_NONE);
  g_assert_cmpuint (gst_element_factory_get_num_pad_templates (factory), ==, 2);
  gst_object_unref (factory);
}

/* memory:RDS in any format and size; system memory is refused. */
static void
test_pad_templates (void)
{
  GstElement *e = gst_element_factory_make ("rdsanalytics", NULL);
  static const gchar *names[] = { "sink", "src" };

  for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
    GstPadTemplate *templ = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (e), names[i]);
    GstCaps *caps = gst_pad_template_get_caps (templ), *c;

    c = gst_caps_from_string ("video/x-raw(memory:RDS), format=NV12, width=1920, height=1080");
    g_assert_true (gst_caps_can_intersect (caps, c));
    gst_caps_unref (c);
    c = gst_caps_from_string ("video/x-raw(memory:RDS), format=RGBA, width=64, height=48");
    g_assert_true (gst_caps_can_intersect (caps, c));
    gst_caps_unref (c);
    c = gst_caps_from_string ("video/x-raw, format=NV12, width=64, height=48");
    g_assert_false (gst_caps_can_intersect (caps, c));
    gst_caps_unref (c);
    gst_caps_unref (caps);
  }
  gst_object_unref (e);
}

/* DeepStream 8.0's properties: config-file (any state), enable (NULL/READY only, default TRUE), unique-id (default
 * 17), and nothing else of the element's own. */
static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsanalytics", NULL);
  GObjectClass *klass = G_OBJECT_GET_CLASS (e);
  GParamSpec *p, **all;
  guint n_all, n_own = 0;

  p = g_object_class_find_property (klass, "config-file");
  g_assert_nonnull (p);
  g_assert_true (G_IS_PARAM_SPEC_STRING (p));
  g_assert_null (g_value_get_string (g_param_spec_get_default_value (p)));
  g_assert_true ((p->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE);
  g_assert_false (p->flags & GST_PARAM_MUTABLE_READY);

  p = g_object_class_find_property (klass, "enable");
  g_assert_nonnull (p);
  g_assert_true (G_IS_PARAM_SPEC_BOOLEAN (p));
  g_assert_true (g_value_get_boolean (g_param_spec_get_default_value (p)));
  g_assert_true ((p->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE);
  g_assert_true (p->flags & GST_PARAM_MUTABLE_READY);

  p = g_object_class_find_property (klass, "unique-id");
  g_assert_nonnull (p);
  g_assert_true (G_IS_PARAM_SPEC_UINT (p));
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->minimum, ==, 0);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->maximum, ==, G_MAXUINT);
  g_assert_cmpuint (g_value_get_uint (g_param_spec_get_default_value (p)), ==, 17);
  g_assert_false (p->flags & GST_PARAM_MUTABLE_READY);

  /* GstBaseTransform's qos, default FALSE, as on 8.0 */
  p = g_object_class_find_property (klass, "qos");
  g_assert_nonnull (p);
  g_assert_false (g_value_get_boolean (g_param_spec_get_default_value (p)));

  all = g_object_class_list_properties (klass, &n_all);
  for (guint i = 0; i < n_all; i++)
    if (all[i]->owner_type == G_OBJECT_TYPE (e))
      n_own++;
  g_free (all);
  g_assert_cmpuint (n_own, ==, 3);
  gst_object_unref (e);
}

static gchar *
write_config (const gchar * name, const gchar * text)
{
  gchar *path = g_build_filename (work_dir, name, NULL);
  GError *err = NULL;

  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  return path;
}

/* Pops every ERROR message from @bus; returns how many there were and their texts in *@texts. */
static guint
pop_errors (GstBus * bus, gchar ** texts)
{
  GString *all = g_string_new (NULL);
  GstMessage *msg;
  guint n = 0;

  while ((msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR))) {
    GError *err = NULL;
    gchar *dbg = NULL;

    gst_message_parse_error (msg, &err, &dbg);
    g_string_append_printf (all, "%s\n", err->message);
    g_test_message ("error: %s", err->message);
    g_error_free (err);
    g_free (dbg);
    gst_message_unref (msg);
    n++;
  }
  if (texts)
    *texts = g_string_free (all, FALSE);
  else
    g_string_free (all, TRUE);
  return n;
}

/* An rdsanalytics with @text (NULL: no config file) written as its config-file. */
static GstElement *
new_analytics (const gchar * text)
{
  GstElement *e = gst_element_factory_make ("rdsanalytics", NULL);

  if (text) {
    gchar *path = write_config ("analytics.txt", text);

    g_object_set (e, "config-file", path, NULL);
    g_free (path);
  }
  return e;
}

/* Sets @e (in a pipeline of its own) to PAUSED, which must fail, and returns the error texts. */
static gchar *
start_error (GstElement * e)
{
  GstElement *pipeline = gst_pipeline_new (NULL);
  GstBus *bus = gst_element_get_bus (pipeline);
  gchar *texts = NULL;

  gst_bin_add (GST_BIN (pipeline), e);
  g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpuint (pop_errors (bus, &texts), >=, 1);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
  return texts;
}

/* No config-file, a missing one and an invalid one fail the start with an ERROR naming the property, the file or
 * the key; enable=FALSE still needs a config (DeepStream 8.0 behaviour). */
static void
test_start_errors (void)
{
  gchar *missing = g_build_filename (work_dir, "missing.txt", NULL), *texts;
  GstElement *e;

  e = new_analytics (NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "config-file"));
  g_free (texts);

  e = new_analytics (NULL);
  g_object_set (e, "enable", FALSE, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "config-file"));
  g_free (texts);

  e = new_analytics (NULL);
  g_object_set (e, "config-file", missing, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, missing));
  g_free (texts);

  e = new_analytics ("[property]\nenable=1\nconfig-width=0\nconfig-height=480\n");
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "config-width"));
  g_free (texts);
  g_free (missing);
}

/* Once streaming, enable refuses changes with a warning; config-file and unique-id still change. */
static void
test_ready_only (void)
{
  GstHarness *h = gst_harness_new_with_element (new_analytics (CONFIG), "sink", "src");
  gboolean enable = FALSE;
  guint uid = 0;

  gst_object_unref (h->element);
  gst_harness_set_caps_str (h, IN_CAPS, IN_CAPS);
  g_assert_cmpint (GST_STATE (h->element), ==, GST_STATE_PLAYING);
  g_test_expect_message ("rdsanalytics", G_LOG_LEVEL_WARNING, "*'enable'*NULL or READY*");
  g_object_set (h->element, "enable", FALSE, NULL);
  g_test_assert_expected_messages ();
  g_object_get (h->element, "enable", &enable, NULL);
  g_assert_true (enable);
  g_object_set (h->element, "unique-id", 5u, NULL);
  g_object_get (h->element, "unique-id", &uid, NULL);
  g_assert_cmpuint (uid, ==, 5);

  gst_element_set_state (h->element, GST_STATE_READY);
  g_object_set (h->element, "enable", FALSE, NULL);
  g_object_get (h->element, "enable", &enable, NULL);
  g_assert_false (enable);
  gst_harness_teardown (h);
}

/* ---- streaming (gpu) ---- */

static void
drain_events (GstHarness * h)
{
  GstEvent *ev;

  while ((ev = gst_harness_try_pull_event (h)))
    gst_event_unref (ev);
}

/* A harness around @e (the harness takes it), with @bus (or none) set first. */
static GstHarness *
harness_for (GstElement * e, GstBus * bus)
{
  GstHarness *h;

  if (bus)
    gst_element_set_bus (e, bus);
  h = gst_harness_new_with_element (e, "sink", "src");
  gst_object_unref (e);
  gst_harness_set_caps_str (h, IN_CAPS, IN_CAPS);
  drain_events (h);
  return h;
}

static void
harness_teardown_with_bus (GstHarness * h, GstBus * bus)
{
  gst_element_set_bus (h->element, NULL);
  gst_bus_set_flushing (bus, TRUE);
  gst_harness_teardown (h);
  gst_object_unref (bus);
}

/* A device-memory batch of @slots surfaces of @w x @h, the first @n filled, frame i on pad @pads[i] (NULL: i) with
 * source_id @sources[i] (NULL: its pad), as rdsstreammux makes them; no objects yet. Each frame gets the next PTS. */
static GstBuffer *
new_batch_full (guint slots, guint n, const guint * pads, const guint * sources, guint w, guint h)
{
  RdsBufSurfaceCreateParams params = {
    .width = w,.height = h,.colorFormat = RDS_BUF_COLOR_FORMAT_NV12,.layout = RDS_BUF_LAYOUT_PITCH,
  };
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (slots);
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (dev_alloc, slots, &params));
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  s->numFilled = n;
  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

    fm->pad_index = pads ? pads[i] : i;
    fm->source_id = sources ? sources[i] : fm->pad_index;
    fm->batch_id = i;
    fm->buf_pts = next_pts;
    fm->pipeline_width = w;
    fm->pipeline_height = h;
    rds_add_frame_meta_to_batch (bm, fm);
  }
  next_pts += 33 * GST_MSECOND;
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  return buf;
}

static GstBuffer *
new_batch (guint n, const guint * pads)
{
  return new_batch_full (n, n, pads, NULL, W, H);
}

static RdsFrameMeta *
nth_frame (GstBuffer * buf, guint n)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);

  g_assert_cmpuint (g_list_length (bm->frame_meta_list), >, n);
  return g_list_nth_data (bm->frame_meta_list, n);
}

/* A 20 x 40 object on frame @frame whose bottom centre is (@cx, @by). */
static RdsObjectMeta *
add_obj (GstBuffer * buf, guint frame, guint64 id, gint class_id, gfloat cx, gfloat by)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);

  om->object_id = id;
  om->class_id = class_id;
  om->confidence = 0.9f;
  om->rect_params.left = cx - 10;
  om->rect_params.top = by - 40;
  om->rect_params.width = 20;
  om->rect_params.height = 40;
  om->detector_bbox_info.org_bbox_coords = (RdsBbox_Coords) {
  cx - 10, by - 40, 20, 40};
  rds_add_obj_meta_to_frame (nth_frame (buf, frame), om, NULL);
  return om;
}

static GstBuffer *
run (GstHarness * h, GstBuffer * buf)
{
  GstBuffer *out;

  g_assert_cmpint (gst_harness_push (h, buf), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  g_assert_true (out == buf);
  return out;
}

static guint
count_user (RdsUserMetaList * list, RdsMetaType type)
{
  guint n = 0;

  for (RdsUserMetaList * l = list; l; l = l->next)
    n += ((RdsUserMeta *) l->data)->base_meta.meta_type == type;
  return n;
}

/* The frame's one analytics frame meta. */
static RdsAnalyticsFrameMeta *
frame_meta (RdsFrameMeta * fm)
{
  g_assert_cmpuint (count_user (fm->frame_user_meta_list, RDS_FRAME_META_ANALYTICS), ==, 1);
  for (RdsUserMetaList * l = fm->frame_user_meta_list; l; l = l->next) {
    RdsUserMeta *um = l->data;

    if (um->base_meta.meta_type == RDS_FRAME_META_ANALYTICS) {
      g_assert_nonnull (um->user_meta_data);
      return um->user_meta_data;
    }
  }
  return NULL;
}

/* The object's analytics object info, or NULL when it has none. */
static RdsAnalyticsObjInfo *
obj_info (RdsObjectMeta * om)
{
  g_assert_cmpuint (count_user (om->obj_user_meta_list, RDS_OBJ_META_ANALYTICS), <=, 1);
  for (RdsUserMetaList * l = om->obj_user_meta_list; l; l = l->next) {
    RdsUserMeta *um = l->data;

    if (um->base_meta.meta_type == RDS_OBJ_META_ANALYTICS)
      return um->user_meta_data;
  }
  return NULL;
}

static guint64
lc_cum (RdsAnalyticsFrameMeta * m, const gchar * label)
{
  guint64 v = G_MAXUINT64;

  g_assert_true (rds_analytics_frame_meta_get_lc_cum_count (m, label, &v));
  return v;
}

static guint64
lc_cur (RdsAnalyticsFrameMeta * m, const gchar * label)
{
  guint64 v = G_MAXUINT64;

  g_assert_true (rds_analytics_frame_meta_get_lc_count (m, label, &v));
  return v;
}

/* Walks object @id on @pad (one frame per batch) down from a bottom of 140 in steps of 20 for @n frames, x = 100.
 * Returns the index of the frame that counted it on line @label (-1: none) and its final cumulative count in *@cum. */
static gint
walk (GstHarness * h, guint pad, guint64 id, guint n, const gchar * label, guint64 * cum)
{
  gint counted = -1;

  for (guint f = 0; f < n; f++) {
    GstBuffer *buf = new_batch (1, &pad);
    RdsObjectMeta *om = add_obj (buf, 0, id, 0, 100, 140 + 20 * f);
    RdsAnalyticsFrameMeta *m;

    buf = run (h, buf);
    m = frame_meta (nth_frame (buf, 0));
    if (lc_cur (m, label) == 1) {
      RdsAnalyticsObjInfo *info = obj_info (om);

      g_assert_cmpint (counted, ==, -1);
      counted = f;
      g_assert_nonnull (info);
      g_assert_cmpuint (rds_analytics_obj_info_n_lc_status (info), ==, 1);
      g_assert_cmpstr (rds_analytics_obj_info_get_lc_status (info, 0), ==, label);
    }
    *cum = lc_cum (m, label);
    gst_buffer_unref (buf);
  }
  return counted;
}

/* No user meta anywhere in the batch. */
static void
assert_no_meta (GstBuffer * buf)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);

  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;

    g_assert_null (fm->frame_user_meta_list);
    g_assert_null (fm->display_meta_list);
    for (GList * o = fm->obj_meta_list; o; o = o->next)
      g_assert_null (((RdsObjectMeta *) o->data)->obj_user_meta_list);
  }
}

/* enable=FALSE and [property] enable=0: the batch goes through with no meta added (DeepStream 8.0 behaviour). */
static void
test_passthrough (void)
{
  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < 2; i++) {
    GstElement *e = new_analytics (i == 0 ? CONFIG : "[property]\nenable=0\nconfig-width=640\nconfig-height=480\n"
        "[roi-filtering-stream-0]\nroi-RF=0;0;320;0;320;480;0;480\n");
    GstHarness *h;
    GstBuffer *buf;

    if (i == 0)
      g_object_set (e, "enable", FALSE, NULL);
    h = harness_for (e, NULL);
    buf = new_batch (2, NULL);
    add_obj (buf, 0, 1, 0, 100, 300);
    add_obj (buf, 1, 2, 0, 100, 300);
    buf = run (h, buf);
    assert_no_meta (buf);
    gst_buffer_unref (buf);
    gst_harness_teardown (h);
  }
}

static void
count_entry (const gchar * label, guint64 count, gpointer data)
{
  (void) label;
  (void) count;
  (*(guint *) data)++;
}

static void
count_status (const gchar * label, gboolean status, gpointer data)
{
  (void) label;
  (void) status;
  (*(guint *) data)++;
}

static void
count_class (gint class_id, guint32 count, gpointer data)
{
  (void) class_id;
  (void) count;
  (*(guint *) data)++;
}

/* A frame meta on every frame, a stream without groups included (empty maps, objCnt filled); an object info only on
 * the object with something to report; unique-id in both, read per batch. */
static void
test_metas (void)
{
  GstHarness *h;
  GstBuffer *buf;
  RdsObjectMeta *in_roi, *outside, *other;
  RdsAnalyticsFrameMeta *m;
  RdsAnalyticsObjInfo *info;
  guint32 v = 0;
  guint n;

  RDS_TEST_REQUIRE_GPU ();
  h = harness_for (new_analytics (CONFIG), NULL);
  buf = new_batch (2, NULL);
  in_roi = add_obj (buf, 0, 1, 0, 100, 300);
  outside = add_obj (buf, 0, 2, 2, 500, 300);
  other = add_obj (buf, 1, 3, 0, 100, 300);
  buf = run (h, buf);

  m = frame_meta (nth_frame (buf, 0));
  g_assert_cmpuint (rds_analytics_frame_meta_get_unique_id (m), ==, 17);
  g_assert_true (rds_analytics_frame_meta_get_roi_count (m, "RF", &v));
  g_assert_cmpuint (v, ==, 1);
  g_assert_cmpuint (lc_cur (m, "Down"), ==, 0);
  g_assert_cmpuint (lc_cum (m, "Down"), ==, 0);
  g_assert_true (rds_analytics_frame_meta_get_obj_count (m, 0, &v));
  g_assert_cmpuint (v, ==, 1);
  g_assert_true (rds_analytics_frame_meta_get_obj_count (m, 2, &v));
  g_assert_cmpuint (v, ==, 1);
  n = 0;
  rds_analytics_frame_meta_foreach_oc_status (m, count_status, &n);
  g_assert_cmpuint (n, ==, 0);

  info = obj_info (in_roi);
  g_assert_nonnull (info);
  g_assert_cmpuint (rds_analytics_obj_info_get_unique_id (info), ==, 17);
  g_assert_cmpuint (rds_analytics_obj_info_n_roi_status (info), ==, 1);
  g_assert_cmpstr (rds_analytics_obj_info_get_roi_status (info, 0), ==, "RF");
  g_assert_cmpuint (rds_analytics_obj_info_n_oc_status (info), ==, 0);
  g_assert_cmpuint (rds_analytics_obj_info_n_lc_status (info), ==, 0);
  g_assert_cmpstr (rds_analytics_obj_info_get_dir_status (info), ==, "");
  g_assert_cmpstr (rds_analytics_obj_info_get_obj_status (info), ==, " ROI:RF");
  g_assert_null (obj_info (outside));

  /* pad 1 has no groups: empty maps, objCnt filled, no object info */
  m = frame_meta (nth_frame (buf, 1));
  n = 0;
  rds_analytics_frame_meta_foreach_roi_count (m, count_entry, &n);
  rds_analytics_frame_meta_foreach_lc_cum_count (m, count_entry, &n);
  g_assert_cmpuint (n, ==, 0);
  n = 0;
  rds_analytics_frame_meta_foreach_obj_count (m, count_class, &n);
  g_assert_cmpuint (n, ==, 1);
  g_assert_null (obj_info (other));
  gst_buffer_unref (buf);

  /* a frame without objects still gets its frame meta, with the zero entries */
  buf = run (h, new_batch (1, NULL));
  m = frame_meta (nth_frame (buf, 0));
  g_assert_true (rds_analytics_frame_meta_get_roi_count (m, "RF", &v));
  g_assert_cmpuint (v, ==, 0);
  g_assert_cmpuint (lc_cum (m, "Down"), ==, 0);
  gst_buffer_unref (buf);

  /* unique-id changes from the next batch */
  g_object_set (h->element, "unique-id", 9u, NULL);
  buf = new_batch (1, NULL);
  in_roi = add_obj (buf, 0, 1, 0, 100, 300);
  buf = run (h, buf);
  g_assert_cmpuint (rds_analytics_frame_meta_get_unique_id (frame_meta (nth_frame (buf, 0))), ==, 9);
  g_assert_cmpuint (rds_analytics_obj_info_get_unique_id (obj_info (in_roi)), ==, 9);
  /* the display meta of osd-mode 0 without display-obj-cnt: none */
  g_assert_null (nth_frame (buf, 0)->display_meta_list);
  gst_buffer_unref (buf);
  gst_harness_teardown (h);
}

/* The [...-stream-<n>] rules follow the frame meta's pad_index, not its source_id (DeepStream 8.0 behaviour). */
static void
test_pad_index (void)
{
  static const guint pads[] = { 0, 1 }, sources[] = { 1, 0 };
  GstHarness *h;
  GstBuffer *buf;
  guint32 v = 0;

  RDS_TEST_REQUIRE_GPU ();
  h = harness_for (new_analytics (CONFIG), NULL);
  buf = new_batch_full (2, 2, pads, sources, W, H);
  add_obj (buf, 0, 1, 0, 100, 300);
  add_obj (buf, 1, 1, 0, 100, 300);
  buf = run (h, buf);
  g_assert_true (rds_analytics_frame_meta_get_roi_count (frame_meta (nth_frame (buf, 0)), "RF", &v));
  g_assert_cmpuint (v, ==, 1);
  g_assert_false (rds_analytics_frame_meta_get_roi_count (frame_meta (nth_frame (buf, 1)), "RF", &v));
  gst_buffer_unref (buf);
  gst_harness_teardown (h);
}

/* Pushes @ev and checks that the same event comes out downstream. */
static void
push_event_through (GstHarness * h, GstEvent * ev)
{
  GstEvent *out;

  gst_event_ref (ev);
  g_assert_true (gst_harness_push_event (h, ev));
  out = gst_harness_try_pull_event (h);
  g_assert_true (out == ev);
  gst_event_unref (out);
  gst_event_unref (ev);
}

/* An object walking down across the line is counted once, on its fourth observation (loose: n = 3); walking again
 * it isn't. A stream reset keeps the cumulative count but clears the history and the "already counted" flag, so it
 * is counted again (ours). A stream EOS alone changes nothing. */
static void
test_stream_reset (void)
{
  GstHarness *h;
  guint64 cum = 0;

  RDS_TEST_REQUIRE_GPU ();
  h = harness_for (new_analytics (CONFIG), NULL);
  push_event_through (h, rds_event_new_pad_added (0));
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  g_assert_cmpuint (cum, ==, 1);
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, -1);
  g_assert_cmpuint (cum, ==, 1);
  push_event_through (h, rds_event_new_stream_eos (0));
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, -1);
  g_assert_cmpuint (cum, ==, 1);
  /* another stream's reset, or the removal of a pad never seen, leaves stream 0 alone */
  push_event_through (h, rds_event_new_stream_reset (1));
  push_event_through (h, rds_event_new_pad_deleted (7));
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, -1);
  push_event_through (h, rds_event_new_stream_reset (0));
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  g_assert_cmpuint (cum, ==, 2);
  gst_harness_teardown (h);
}

/* A source removed (rds-pad-deleted) and another added on its pad starts fresh: the same id is counted again, and
 * the cumulative count continues (ours). A new pad mid-stream gets its frame meta. */
static void
test_runtime_source (void)
{
  static const guint pad2 = 2;
  GstHarness *h;
  GstBuffer *buf;
  RdsObjectMeta *om;
  RdsAnalyticsFrameMeta *m;
  guint64 cum = 0;
  guint32 v = 0;
  guint n = 0;

  RDS_TEST_REQUIRE_GPU ();
  h = harness_for (new_analytics (CONFIG), NULL);
  push_event_through (h, rds_event_new_pad_added (0));
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  push_event_through (h, rds_event_new_stream_eos (0));
  push_event_through (h, rds_event_new_pad_deleted (0));
  push_event_through (h, rds_event_new_pad_added (0));
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  g_assert_cmpuint (cum, ==, 2);

  /* pad 2, added at runtime, has no groups: no label entries, objCnt only, no object info */
  push_event_through (h, rds_event_new_pad_added (2));
  buf = new_batch (1, &pad2);
  om = add_obj (buf, 0, 1, 4, 100, 300);
  buf = run (h, buf);
  m = frame_meta (nth_frame (buf, 0));
  rds_analytics_frame_meta_foreach_obj_count (m, count_class, &n);
  g_assert_cmpuint (n, ==, 1);
  g_assert_true (rds_analytics_frame_meta_get_obj_count (m, 4, &v));
  g_assert_cmpuint (v, ==, 1);
  n = 0;
  rds_analytics_frame_meta_foreach_roi_count (m, count_entry, &n);
  rds_analytics_frame_meta_foreach_lc_count (m, count_entry, &n);
  rds_analytics_frame_meta_foreach_lc_cum_count (m, count_entry, &n);
  rds_analytics_frame_meta_foreach_oc_status (m, count_status, &n);
  g_assert_cmpuint (n, ==, 0);
  g_assert_null (obj_info (om));
  gst_buffer_unref (buf);
  gst_harness_teardown (h);
}

/* Batches with fewer frames than slots, and two frames of one stream in a batch, each processed in order. */
static void
test_partial_batch (void)
{
  static const guint pads[] = { 0, 0 };
  GstHarness *h;
  GstBuffer *buf;
  guint32 v = 0;

  RDS_TEST_REQUIRE_GPU ();
  h = harness_for (new_analytics (CONFIG), NULL);
  buf = new_batch_full (4, 1, NULL, NULL, W, H);
  add_obj (buf, 0, 1, 0, 100, 300);
  buf = run (h, buf);
  g_assert_cmpuint (g_list_length (gst_buffer_get_rds_batch_meta (buf)->frame_meta_list), ==, 1);
  g_assert_true (rds_analytics_frame_meta_get_roi_count (frame_meta (nth_frame (buf, 0)), "RF", &v));
  g_assert_cmpuint (v, ==, 1);
  gst_buffer_unref (buf);

  /* an empty batch goes through */
  buf = run (h, new_batch_full (4, 0, NULL, NULL, W, H));
  gst_buffer_unref (buf);

  /* id 7 seen twice in one batch, then twice more: four observations, so the second batch's last frame counts it */
  for (guint b = 0; b < 2; b++) {
    buf = new_batch_full (2, 2, pads, NULL, W, H);
    add_obj (buf, 0, 7, 0, 100, 140 + 40 * b);
    add_obj (buf, 1, 7, 0, 100, 160 + 40 * b);
    buf = run (h, buf);
    g_assert_cmpuint (lc_cur (frame_meta (nth_frame (buf, 0)), "Down"), ==, 0);
    g_assert_cmpuint (lc_cur (frame_meta (nth_frame (buf, 1)), "Down"), ==, b == 1 ? 1 : 0);
    gst_buffer_unref (buf);
  }
  gst_harness_teardown (h);
}

/* config-file set while playing applies from the next buffer with all state reset (DeepStream 8.0 behaviour): the
 * cumulative count restarts, a new rule set replaces the labels. A bad file posts an ERROR and keeps the old config
 * (ours). */
static void
test_reload (void)
{
  GstBus *bus;
  GstHarness *h;
  GstElement *e;
  gchar *path, *other, *bad, *off, *got = NULL, *texts = NULL;
  GstBuffer *buf;
  RdsAnalyticsFrameMeta *m;
  guint64 cum = 0, v = 0;

  RDS_TEST_REQUIRE_GPU ();
  path = write_config ("reload.txt", CONFIG);
  other = write_config ("other.txt", CONFIG_OTHER);
  bad = write_config ("bad.txt", "[property]\nenable=1\nconfig-width=640\n");
  off = write_config ("off.txt", "[property]\nenable=0\nconfig-width=640\nconfig-height=480\n");
  bus = gst_bus_new ();
  e = gst_element_factory_make ("rdsanalytics", NULL);
  g_object_set (e, "config-file", path, NULL);
  h = harness_for (e, bus);
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  g_assert_cmpuint (cum, ==, 1);

  /* the same file again: all state reset, from the next buffer */
  g_object_set (h->element, "config-file", path, NULL);
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  g_assert_cmpuint (cum, ==, 1);

  /* a bad file: an ERROR, and the old config stays, with its state */
  g_object_set (h->element, "config-file", bad, NULL);
  g_assert_cmpuint (pop_errors (bus, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "bad.txt"));
  g_free (texts);
  g_object_get (h->element, "config-file", &got, NULL);
  g_assert_cmpstr (got, ==, path);
  g_free (got);
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, -1);
  g_assert_cmpuint (cum, ==, 1);

  /* another rule set: the old labels are gone, the new ones start at 0 */
  g_object_set (h->element, "config-file", other, NULL);
  buf = new_batch (1, NULL);
  add_obj (buf, 0, 5, 0, 100, 300);
  buf = run (h, buf);
  m = frame_meta (nth_frame (buf, 0));
  g_assert_false (rds_analytics_frame_meta_get_lc_cum_count (m, "Down", &v));
  g_assert_false (rds_analytics_frame_meta_get_roi_count (m, "RF", NULL));
  g_assert_cmpuint (lc_cum (m, "Exit"), ==, 0);
  gst_buffer_unref (buf);
  g_assert_cmpuint (pop_errors (bus, NULL), ==, 0);

  /* a config with [property] enable=0: pass-through from the next buffer */
  g_object_set (h->element, "config-file", off, NULL);
  buf = new_batch (1, NULL);
  add_obj (buf, 0, 5, 0, 100, 300);
  buf = run (h, buf);
  assert_no_meta (buf);
  gst_buffer_unref (buf);
  g_assert_cmpuint (pop_errors (bus, NULL), ==, 0);

  harness_teardown_with_bus (h, bus);
  g_free (path);
  g_free (other);
  g_free (bad);
  g_free (off);
}

/* A stop/start cycle with buffers on both sides: the restart loads config-file again, every stream's state starts
 * afresh, and a config still pending at the stop is dropped. */
static void
test_restart (void)
{
  GstHarness *h;
  GstElement *e;
  GstBuffer *buf;
  RdsAnalyticsFrameMeta *m;
  gchar *path, *other;
  guint64 cum = 0;

  RDS_TEST_REQUIRE_GPU ();
  path = write_config ("restart.txt", CONFIG);
  other = write_config ("restart-other.txt", CONFIG_OTHER);
  e = gst_element_factory_make ("rdsanalytics", NULL);
  g_object_set (e, "config-file", path, NULL);
  h = harness_for (e, NULL);
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  g_assert_cmpuint (cum, ==, 1);

  /* pending at the stop, then replaced while stopped */
  g_object_set (h->element, "config-file", other, NULL);
  g_assert_cmpint (gst_element_set_state (h->element, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  g_object_set (h->element, "config-file", path, NULL);
  g_assert_cmpint (gst_element_set_state (h->element, GST_STATE_PLAYING), ==, GST_STATE_CHANGE_SUCCESS);
  /* the element's pads lost their sticky events */
  g_assert_true (gst_harness_push_event (h, gst_event_new_stream_start ("restart")));
  gst_harness_set_caps_str (h, IN_CAPS, IN_CAPS);
  drain_events (h);

  buf = new_batch (1, NULL);
  add_obj (buf, 0, 9, 0, 100, 300);
  buf = run (h, buf);
  m = frame_meta (nth_frame (buf, 0));
  g_assert_false (rds_analytics_frame_meta_get_lc_cum_count (m, "Exit", NULL));
  g_assert_cmpuint (lc_cum (m, "Down"), ==, 0);
  gst_buffer_unref (buf);
  /* the same id is counted again: fresh state */
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);
  g_assert_cmpuint (cum, ==, 1);
  gst_harness_teardown (h);
  g_free (path);
  g_free (other);
}

static guint n_untracked_warnings;

static void
count_untracked (GstDebugCategory * cat, GstDebugLevel level, const gchar * file, const gchar * function, gint line,
    GObject * object, GstDebugMessage * message, gpointer data)
{
  (void) file;
  (void) function;
  (void) line;
  (void) object;
  (void) data;
  if (level == GST_LEVEL_WARNING && strcmp (gst_debug_category_get_name (cat), "rdsanalytics") == 0
      && strstr (gst_debug_message_get (message), "untracked"))
    g_atomic_int_inc (&n_untracked_warnings);
}

/* Untracked objects: one warning per stream with direction or line crossing (ours), however many frames, and again
 * after a stream reset. They still count for the ROIs. */
static void
test_untracked_warning (void)
{
  static const guint pads[] = { 0, 1 };
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  gst_debug_set_threshold_for_name ("rdsanalytics", GST_LEVEL_WARNING);
  gst_debug_add_log_function (count_untracked, NULL, NULL);
  n_untracked_warnings = 0;
  h = harness_for (new_analytics (CONFIG_TWO), NULL);
  for (guint f = 0; f < 4; f++) {
    GstBuffer *buf = new_batch (2, pads);

    add_obj (buf, 0, RDS_UNTRACKED_OBJECT_ID, 0, 100, 140 + 20 * f);
    add_obj (buf, 1, RDS_UNTRACKED_OBJECT_ID, 0, 100, 140 + 20 * f);
    gst_buffer_unref (run (h, buf));
  }
  g_assert_cmpuint (g_atomic_int_get (&n_untracked_warnings), ==, 2);
  push_event_through (h, rds_event_new_stream_reset (1));
  {
    GstBuffer *buf = new_batch (1, &pads[1]);

    add_obj (buf, 0, RDS_UNTRACKED_OBJECT_ID, 0, 100, 140);
    gst_buffer_unref (run (h, buf));
  }
  g_assert_cmpuint (g_atomic_int_get (&n_untracked_warnings), ==, 3);
  gst_harness_teardown (h);
  gst_debug_remove_log_function (count_untracked);
  gst_debug_unset_threshold_for_name ("rdsanalytics");
}

/* The rules are scaled to the batch surface's size, the mux output; a new size rebuilds the analytics, resetting
 * every stream's state (ours). */
static void
test_out_size (void)
{
  GstHarness *h;
  GstBuffer *buf;
  guint32 v = 0;
  guint64 cum = 0;

  RDS_TEST_REQUIRE_GPU ();
  h = harness_for (new_analytics (CONFIG), NULL);
  g_assert_cmpint (walk (h, 0, 5, 6, "Down", &cum), ==, 3);

  /* 1280 x 960: the ROI is the left 640 columns, x = 500 is in it; the counts restarted */
  gst_harness_set_caps_str (h, "video/x-raw(memory:RDS), format=NV12, width=1280, height=960, framerate=30/1",
      "video/x-raw(memory:RDS), format=NV12, width=1280, height=960, framerate=30/1");
  buf = new_batch_full (1, 1, NULL, NULL, 1280, 960);
  add_obj (buf, 0, 1, 0, 500, 600);
  buf = run (h, buf);
  g_assert_true (rds_analytics_frame_meta_get_roi_count (frame_meta (nth_frame (buf, 0)), "RF", &v));
  g_assert_cmpuint (v, ==, 1);
  g_assert_cmpuint (lc_cum (frame_meta (nth_frame (buf, 0)), "Down"), ==, 0);
  gst_buffer_unref (buf);
  gst_harness_teardown (h);
}

/* CONFIG with @property_extra in [property] in place of osd-mode=0. */
static gchar *
config_with (const gchar * property_extra)
{
  GString *s = g_string_new (CONFIG);

  g_string_replace (s, "osd-mode=0\n", property_extra, 1);
  return g_string_free (s, FALSE);
}

/* The frame's display metas, as one string per meta: "texts|lines", each text "s@x,y" and each line
 * "x1,y1,x2,y2", ';'-separated, metas joined by " / ". */
static gchar *
display_string (RdsFrameMeta * fm)
{
  GString *s = g_string_new (NULL);

  for (GList * l = fm->display_meta_list; l; l = l->next) {
    const RdsDisplayMeta *dm = l->data;

    if (s->len)
      g_string_append (s, " / ");
    for (guint i = 0; i < dm->num_labels; i++)
      g_string_append_printf (s, "%s%s@%u,%u", i ? ";" : "", dm->text_params[i].display_text,
          dm->text_params[i].x_offset, dm->text_params[i].y_offset);
    g_string_append_c (s, '|');
    for (guint i = 0; i < dm->num_lines; i++)
      g_string_append_printf (s, "%s%u,%u,%u,%u", i ? ";" : "", dm->line_params[i].x1, dm->line_params[i].y1,
          dm->line_params[i].x2, dm->line_params[i].y2);
  }
  return g_string_free (s, FALSE);
}

#define RF_LINES "0,0,320,0;320,0,320,480;320,480,0,480;0,480,0,0"
#define DOWN_LINES "320,100,320,300;312,271,320,300;327,271,320,300;0,200,640,200"

/* The overlays per osd-mode on a two-frame batch (stream 1 has no groups): none in mode 0, the count text alone in
 * mode 0 with display-obj-cnt, the rules with their labels in mode 1, the counts and the objects' status text in
 * mode 2 (DeepStream 8.0 behaviour; the field-by-field checks are analytics_draw's). */
static void
test_display (void)
{
  static const struct
  {
    const gchar *property;
    const gchar *frame0, *frame1;
    const gchar *obj_text;      /* the object in the ROI's text; NULL: none */
  } cases[] = {
    {"osd-mode=0\n", "", "", NULL},
    {"osd-mode=0\ndisplay-obj-cnt=1\n", "Count for ClassId0=1@5,5|", "Count for ClassId0=1@5,5|", NULL},
    {"osd-mode=1\n", "RF@0,0;Down @640,200|" RF_LINES ";" DOWN_LINES, "", NULL},
    {"osd-mode=2\n", "RF=1@0,0;Down=0@640,200|" RF_LINES ";" DOWN_LINES, "", " ROI:RF"},
  };

  RDS_TEST_REQUIRE_GPU ();
  for (guint c = 0; c < G_N_ELEMENTS (cases); c++) {
    gchar *config = config_with (cases[c].property), *got;
    GstHarness *h = harness_for (new_analytics (config), NULL);
    GstBuffer *buf = new_batch (2, NULL);
    RdsObjectMeta *in_roi = add_obj (buf, 0, 1, 0, 100, 300), *other = add_obj (buf, 1, 2, 0, 100, 300);

    g_test_message ("%s", cases[c].property);
    buf = run (h, buf);
    got = display_string (nth_frame (buf, 0));
    g_assert_cmpstr (got, ==, cases[c].frame0);
    g_free (got);
    got = display_string (nth_frame (buf, 1));
    g_assert_cmpstr (got, ==, cases[c].frame1);
    g_free (got);
    g_assert_cmpstr (in_roi->text_params.display_text, ==, cases[c].obj_text);
    if (cases[c].obj_text) {
      g_assert_cmpuint (in_roi->text_params.x_offset, ==, 90);
      g_assert_cmpuint (in_roi->text_params.y_offset, ==, 250);
      g_assert_cmpfloat (in_roi->rect_params.border_color.green, ==, 1);
      g_assert_cmpfloat (in_roi->rect_params.border_color.blue, ==, 1);
    }
    g_assert_null (other->text_params.display_text);
    gst_buffer_unref (buf);
    gst_harness_teardown (h);
    g_free (config);
  }
}

static void
remove_dir (const gchar * dir)
{
  GDir *d = g_dir_open (dir, 0, NULL);
  const gchar *name;

  while (d && (name = g_dir_read_name (d))) {
    gchar *path = g_build_filename (dir, name, NULL);

    g_remove (path);
    g_free (path);
  }
  if (d)
    g_dir_close (d);
  g_rmdir (dir);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  dev_alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  work_dir = g_dir_make_tmp ("rdsanalytics-XXXXXX", NULL);
  g_assert_nonnull (work_dir);

  g_test_add_func ("/cpu/analytics/factory", test_factory);
  g_test_add_func ("/cpu/analytics/pad-templates", test_pad_templates);
  g_test_add_func ("/cpu/analytics/properties", test_properties);
  g_test_add_func ("/cpu/analytics/start-errors", test_start_errors);
  g_test_add_func ("/cpu/analytics/ready-only", test_ready_only);
  g_test_add_func ("/gpu/analytics/passthrough", test_passthrough);
  g_test_add_func ("/gpu/analytics/metas", test_metas);
  g_test_add_func ("/gpu/analytics/pad-index", test_pad_index);
  g_test_add_func ("/gpu/analytics/stream-reset", test_stream_reset);
  g_test_add_func ("/gpu/analytics/runtime-source", test_runtime_source);
  g_test_add_func ("/gpu/analytics/partial-batch", test_partial_batch);
  g_test_add_func ("/gpu/analytics/reload", test_reload);
  g_test_add_func ("/gpu/analytics/restart", test_restart);
  g_test_add_func ("/gpu/analytics/untracked-warning", test_untracked_warning);
  g_test_add_func ("/gpu/analytics/out-size", test_out_size);
  g_test_add_func ("/gpu/analytics/display", test_display);
  /* gst_deinit stops GStreamer's task-pool threads before exit (see AGENTS.md) */
  ret = g_test_run ();
  gst_object_unref (dev_alloc);
  remove_dir (work_dir);
  g_free (work_dir);
  gst_deinit ();
  return ret;
}
