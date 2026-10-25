/* rdstracker: registration, pad templates, every property with its type, range, default and state flags (enforced
 * once streaming), a system-memory batch passing through with its pixels untouched, and the tracking on scripted
 * object meta: write-back on matched objects, removal of the others, objects created on frames without inference,
 * display-tracking-id, operate-on-class-ids, checkClassMatch, useUniqueID, the stream events and reset modes,
 * start errors and warnings, restart, the meta lock, a seeded random run and an NvSORT run (cpu: no GPU needed).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"

#include <gst/check/gstharness.h>
#include <glib/gstdio.h>
#include <rds/rds.h>
#include <string.h>

#define IN_CAPS "video/x-raw(memory:RDS), format=NV12, width=64, height=48, framerate=30/1"

static const RdsBufSurfaceCreateParams nv12_64 = {
  .width = 64, .height = 48, .colorFormat = RDS_BUF_COLOR_FORMAT_NV12, .layout = RDS_BUF_LAYOUT_PITCH,
};

static GstAllocator *sys_alloc;

static void
test_factory (void)
{
  GstElementFactory *factory = gst_element_factory_find ("rdstracker");

  g_assert_nonnull (factory);
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_LONGNAME), ==,
      "Redstream multi-object tracker");
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS), ==, "Filter/Video");
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), ==, GST_RANK_NONE);
  g_assert_cmpuint (gst_element_factory_get_num_pad_templates (factory), ==, 2);
  gst_object_unref (factory);
}

static void
check_template_caps (GstElement * e, const gchar * name)
{
  static const gchar *formats[] = { "NV12", "RGBA", "BGRA", "RGBx", "BGRx" };
  GstPadTemplate *templ = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (e), name);
  GstCaps *caps, *want, *other;

  g_assert_nonnull (templ);
  caps = gst_pad_template_get_caps (templ);
  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    want = gst_caps_from_string ("video/x-raw(memory:RDS), width=64, height=48");
    gst_caps_set_simple (want, "format", G_TYPE_STRING, formats[i], NULL);
    g_assert_true (gst_caps_can_intersect (caps, want));
    gst_caps_unref (want);
  }
  other = gst_caps_from_string ("video/x-raw, format=NV12, width=64, height=48");
  g_assert_false (gst_caps_can_intersect (caps, other));
  gst_caps_unref (other);
  other = gst_caps_from_string ("video/x-raw(memory:RDS), format=I420, width=64, height=48");
  g_assert_false (gst_caps_can_intersect (caps, other));
  gst_caps_unref (other);
  gst_caps_unref (caps);
}

static void
test_pad_templates (void)
{
  GstElement *e = gst_element_factory_make ("rdstracker", NULL);

  g_assert_nonnull (e);
  check_template_caps (e, "sink");
  check_template_caps (e, "src");
  gst_object_unref (e);
}

/* Every nvtracker property, with its type, range, default and whether it changes only in NULL or READY. */
static void
test_properties (void)
{
  static const struct
  {
    const gchar *name;
    GType type;
    gint64 min, max, def;               /* numeric and boolean properties */
    const gchar *def_str;               /* string properties */
    gboolean ready_only;
  } props[] = {
    {"ll-lib-file", G_TYPE_STRING, 0, 0, 0, NULL, FALSE},
    {"ll-config-file", G_TYPE_STRING, 0, 0, 0, NULL, FALSE},
    {"tracker-width", G_TYPE_UINT, 0, G_MAXUINT, 960, NULL, TRUE},
    {"tracker-height", G_TYPE_UINT, 0, G_MAXUINT, 544, NULL, TRUE},
    {"gpu-id", G_TYPE_UINT, 0, G_MAXUINT, 0, NULL, TRUE},
    {"display-tracking-id", G_TYPE_BOOLEAN, 0, 1, TRUE, NULL, FALSE},
    {"tracking-id-reset-mode", G_TYPE_UINT, 0, 3, 1, NULL, TRUE},
    {"tracking-surface-type", G_TYPE_UINT, 0, G_MAXUINT, 0, NULL, TRUE},
    {"compute-hw", G_TYPE_ENUM, 0, 2, 0, NULL, FALSE},
    {"input-tensor-meta", G_TYPE_BOOLEAN, 0, 1, FALSE, NULL, FALSE},
    {"tensor-meta-gie-id", G_TYPE_UINT, 0, G_MAXUINT, 0, NULL, TRUE},
    {"sub-batches", G_TYPE_STRING, 0, 0, 0, "", FALSE},
    {"sub-batch-err-recovery-trial-cnt", G_TYPE_INT, -1, G_MAXINT, 0, NULL, FALSE},
    {"user-meta-pool-size", G_TYPE_UINT, 1, G_MAXUINT, 32, NULL, TRUE},
    {"operate-on-class-ids", G_TYPE_STRING, 0, 0, 0, NULL, FALSE},
  };
  GstElement *e = gst_element_factory_make ("rdstracker", NULL);
  GObjectClass *klass = G_OBJECT_GET_CLASS (e);
  GParamSpec **all;
  guint n_all, n_own = 0;

  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    GParamSpec *pspec = g_object_class_find_property (klass, props[i].name);
    const GValue *def;

    g_test_message ("%s", props[i].name);
    g_assert_nonnull (pspec);
    g_assert_true (pspec->owner_type == G_OBJECT_TYPE (e));
    g_assert_true (g_type_is_a (G_PARAM_SPEC_VALUE_TYPE (pspec), props[i].type));
    g_assert_true ((pspec->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE);
    g_assert_cmpint (! !(pspec->flags & GST_PARAM_MUTABLE_READY), ==, props[i].ready_only);
    def = g_param_spec_get_default_value (pspec);
    if (G_IS_PARAM_SPEC_UINT (pspec)) {
      g_assert_cmpint (G_PARAM_SPEC_UINT (pspec)->minimum, ==, props[i].min);
      g_assert_cmpint (G_PARAM_SPEC_UINT (pspec)->maximum, ==, props[i].max);
      g_assert_cmpint (g_value_get_uint (def), ==, props[i].def);
    } else if (G_IS_PARAM_SPEC_INT (pspec)) {
      g_assert_cmpint (G_PARAM_SPEC_INT (pspec)->minimum, ==, props[i].min);
      g_assert_cmpint (G_PARAM_SPEC_INT (pspec)->maximum, ==, props[i].max);
      g_assert_cmpint (g_value_get_int (def), ==, props[i].def);
    } else if (G_IS_PARAM_SPEC_BOOLEAN (pspec)) {
      g_assert_cmpint (g_value_get_boolean (def), ==, props[i].def);
    } else if (G_IS_PARAM_SPEC_ENUM (pspec)) {
      GEnumClass *ec = G_PARAM_SPEC_ENUM (pspec)->enum_class;

      g_assert_cmpint (ec->minimum, ==, props[i].min);
      g_assert_cmpint (ec->maximum, ==, props[i].max);
      g_assert_cmpuint (ec->n_values, ==, props[i].max - props[i].min + 1);
      g_assert_cmpint (g_value_get_enum (def), ==, props[i].def);
    } else {
      g_assert_true (G_IS_PARAM_SPEC_STRING (pspec));
      g_assert_cmpstr (g_value_get_string (def), ==, props[i].def_str);
    }
  }

  /* nothing beyond the list (and GstObject's own) */
  all = g_object_class_list_properties (klass, &n_all);
  for (guint i = 0; i < n_all; i++)
    if (all[i]->owner_type == G_OBJECT_TYPE (e))
      n_own++;
  g_free (all);
  g_assert_cmpuint (n_own, ==, G_N_ELEMENTS (props));
  gst_object_unref (e);
}

/* Values set are read back; compute-hw takes its nicks too. */
static void
test_property_roundtrip (void)
{
  GstElement *e = gst_element_factory_make ("rdstracker", NULL);
  gchar *lib = NULL, *cfg = NULL, *sub = NULL, *ids = NULL;
  guint w, h, reset, pool;
  gint trials, hw;
  gboolean show;

  g_object_set (e, "ll-lib-file", "libtracker.so", "ll-config-file", "tracker.yml", "tracker-width", 640,
      "tracker-height", 384, "tracking-id-reset-mode", 3, "user-meta-pool-size", 8,
      "sub-batch-err-recovery-trial-cnt", -1, "display-tracking-id", FALSE, "sub-batches", "0,1;2,3",
      "operate-on-class-ids", "0;2", NULL);
  gst_util_set_object_arg (G_OBJECT (e), "compute-hw", "gpu");
  g_object_get (e, "ll-lib-file", &lib, "ll-config-file", &cfg, "tracker-width", &w, "tracker-height", &h,
      "tracking-id-reset-mode", &reset, "user-meta-pool-size", &pool, "sub-batch-err-recovery-trial-cnt", &trials,
      "display-tracking-id", &show, "sub-batches", &sub, "operate-on-class-ids", &ids, "compute-hw", &hw, NULL);
  g_assert_cmpstr (lib, ==, "libtracker.so");
  g_assert_cmpstr (cfg, ==, "tracker.yml");
  g_assert_cmpuint (w, ==, 640);
  g_assert_cmpuint (h, ==, 384);
  g_assert_cmpuint (reset, ==, 3);
  g_assert_cmpuint (pool, ==, 8);
  g_assert_cmpint (trials, ==, -1);
  g_assert_false (show);
  g_assert_cmpstr (sub, ==, "0,1;2,3");
  g_assert_cmpstr (ids, ==, "0;2");
  g_assert_cmpint (hw, ==, 1);
  g_free (lib);
  g_free (cfg);
  g_free (sub);
  g_free (ids);
  gst_object_unref (e);
}

/* The pixel value slot @slot of a test batch is filled with. */
#define SLOT_VALUE(slot) ((guint8) (10 * ((slot) + 1)))

/* Fills @om as object @k of frame @frame: detector fields as a detector leaves them (untracked), and sentinels in the
 * tracker fields, so any write shows. */
static void
fill_object (RdsObjectMeta * om, guint frame, guint k)
{
  om->unique_component_id = 1;
  om->class_id = (gint) (k + 2);
  om->object_id = RDS_UNTRACKED_OBJECT_ID;
  om->confidence = 0.5f + 0.1f * k;
  om->tracker_confidence = -0.25f;
  om->rect_params.left = 4.0f + frame;
  om->rect_params.top = 6.0f + k;
  om->rect_params.width = 10.0f + k;
  om->rect_params.height = 12.0f + frame;
  om->rect_params.border_width = 3;
  om->rect_params.border_color.red = 0.75;
  om->detector_bbox_info.org_bbox_coords.left = 5.0f + frame;
  om->detector_bbox_info.org_bbox_coords.top = 7.0f + k;
  om->detector_bbox_info.org_bbox_coords.width = 11.0f;
  om->detector_bbox_info.org_bbox_coords.height = 13.0f;
  om->tracker_bbox_info.org_bbox_coords.left = -1.0f;
  om->tracker_bbox_info.org_bbox_coords.top = -2.0f;
  om->tracker_bbox_info.org_bbox_coords.width = -3.0f;
  om->tracker_bbox_info.org_bbox_coords.height = -4.0f;
}

static void
assert_color_equal (const RdsOSD_ColorParams * a, const RdsOSD_ColorParams * b)
{
  g_assert_cmpfloat (a->red, ==, b->red);
  g_assert_cmpfloat (a->green, ==, b->green);
  g_assert_cmpfloat (a->blue, ==, b->blue);
  g_assert_cmpfloat (a->alpha, ==, b->alpha);
}

/* Field by field: the structs have padding, which the pool doesn't have to zero. */
static void
assert_rect_equal (const RdsOSD_RectParams * a, const RdsOSD_RectParams * b)
{
  g_assert_cmpfloat (a->left, ==, b->left);
  g_assert_cmpfloat (a->top, ==, b->top);
  g_assert_cmpfloat (a->width, ==, b->width);
  g_assert_cmpfloat (a->height, ==, b->height);
  g_assert_cmpuint (a->border_width, ==, b->border_width);
  assert_color_equal (&a->border_color, &b->border_color);
  g_assert_cmpuint (a->has_bg_color, ==, b->has_bg_color);
  g_assert_cmpuint (a->reserved, ==, b->reserved);
  assert_color_equal (&a->bg_color, &b->bg_color);
  g_assert_cmpint (a->has_color_info, ==, b->has_color_info);
  g_assert_cmpint (a->color_id, ==, b->color_id);
}

static void
assert_bbox_equal (const RdsComp_BboxInfo * a, const RdsComp_BboxInfo * b)
{
  g_assert_cmpfloat (a->org_bbox_coords.left, ==, b->org_bbox_coords.left);
  g_assert_cmpfloat (a->org_bbox_coords.top, ==, b->org_bbox_coords.top);
  g_assert_cmpfloat (a->org_bbox_coords.width, ==, b->org_bbox_coords.width);
  g_assert_cmpfloat (a->org_bbox_coords.height, ==, b->org_bbox_coords.height);
}

/* A system-memory batch of @n frames (slot i filled with SLOT_VALUE (i)) with two objects each (fill_object). */
static GstBuffer *
make_batch (guint n)
{
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (n);
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (sys_alloc, n, &nv12_64));
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

    g_assert_cmpint (rds_buf_surface_memset (s, i, -1, SLOT_VALUE (i)), ==, 0);
    fm->source_id = i;
    fm->pad_index = i;
    fm->batch_id = i;
    fm->source_frame_width = 64;
    fm->source_frame_height = 48;
    rds_add_frame_meta_to_batch (bm, fm);
    for (guint k = 0; k < 2; k++) {
      RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);

      fill_object (om, i, k);
      rds_add_obj_meta_to_frame (fm, om, NULL);
    }
  }
  s->numFilled = n;
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  GST_BUFFER_PTS (buf) = 5 * GST_SECOND;
  return buf;
}

/* TRUE if every pixel byte of surface @idx of @s (system memory) equals @v. */
static gboolean
slot_is (const RdsBufSurface * s, guint idx, guint8 v)
{
  const RdsBufSurfaceParams *sp = &s->surfaceList[idx];
  const RdsBufSurfacePlaneParams *pp = &sp->planeParams;

  for (guint p = 0; p < pp->num_planes; p++) {
    const guint8 *ptr = (const guint8 *) sp->dataPtr + pp->offset[p];

    for (guint32 y = 0; y < pp->height[p]; y++)
      for (guint32 x = 0; x < pp->width[p] * pp->bytesPerPix[p]; x++)
        if (ptr[(gsize) y * pp->pitch[p] + x] != v)
          return FALSE;
  }
  return TRUE;
}

/* The same buffer comes out, with the same surface and pixels, the same batch meta and every object field as it
 * went in: its objects' classes (2 and 3) are not in operate-on-class-ids. */
static void
test_passthrough (void)
{
  GstElement *e = gst_element_factory_make ("rdstracker", NULL);
  GstHarness *h;
  GstBuffer *in, *out;
  RdsBatchMeta *bm;
  RdsBufSurface *s;
  guint n_frames = 0;

  g_object_set (e, "operate-on-class-ids", "0;1", NULL);
  h = gst_harness_new_with_element (e, "sink", "src");
  gst_object_unref (e);
  gst_harness_set_caps_str (h, IN_CAPS, IN_CAPS);
  in = make_batch (2);
  s = rds_buffer_get_surface (in);
  bm = gst_buffer_get_rds_batch_meta (in);
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);

  g_assert_true (out == in);
  g_assert_true (rds_buffer_get_surface (out) == s);
  g_assert_cmpuint (s->numFilled, ==, 2);
  g_assert_true (slot_is (s, 0, SLOT_VALUE (0)));
  g_assert_true (slot_is (s, 1, SLOT_VALUE (1)));
  g_assert_true (gst_buffer_get_rds_batch_meta (out) == bm);
  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, 5 * GST_SECOND);
  g_assert_cmpuint (bm->num_frames_in_batch, ==, 2);
  for (RdsFrameMetaList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    guint k = 0;

    g_assert_cmpuint (fm->batch_id, ==, n_frames);
    g_assert_cmpuint (fm->num_obj_meta, ==, 2);
    for (RdsObjectMetaList * o = fm->obj_meta_list; o; o = o->next, k++) {
      RdsObjectMeta *om = o->data;
      RdsObjectMeta want;

      memset (&want, 0, sizeof (want));
      fill_object (&want, n_frames, k);
      g_assert_cmpint (om->unique_component_id, ==, want.unique_component_id);
      g_assert_cmpint (om->class_id, ==, want.class_id);
      g_assert_true (om->object_id == RDS_UNTRACKED_OBJECT_ID);
      g_assert_cmpfloat (om->confidence, ==, want.confidence);
      g_assert_cmpfloat (om->tracker_confidence, ==, want.tracker_confidence);
      assert_rect_equal (&om->rect_params, &want.rect_params);
      assert_bbox_equal (&om->detector_bbox_info, &want.detector_bbox_info);
      assert_bbox_equal (&om->tracker_bbox_info, &want.tracker_bbox_info);
    }
    g_assert_cmpuint (k, ==, 2);
    n_frames++;
  }
  g_assert_cmpuint (n_frames, ==, 2);

  gst_buffer_unref (out);
  gst_buffer_unref (in);
  gst_harness_teardown (h);
}

/* Once streaming, the READY-only properties refuse changes with a warning and keep their value; the others still
 * change. */
static void
test_ready_only (void)
{
  static const gchar *ready_only[] = { "tracker-width", "tracker-height", "gpu-id", "tracking-id-reset-mode",
    "tracking-surface-type", "tensor-meta-gie-id", "user-meta-pool-size",
  };
  GstHarness *h = gst_harness_new ("rdstracker");
  gboolean show = TRUE;

  gst_harness_set_caps_str (h, IN_CAPS, IN_CAPS);
  g_assert_cmpint (GST_STATE (h->element), ==, GST_STATE_PLAYING);
  for (guint i = 0; i < G_N_ELEMENTS (ready_only); i++) {
    GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (h->element), ready_only[i]);
    guint before, after;
    gchar *pattern = g_strdup_printf ("*'%s'*NULL or READY*", ready_only[i]);

    g_object_get (h->element, ready_only[i], &before, NULL);
    g_test_expect_message ("rdstracker", G_LOG_LEVEL_WARNING, pattern);
    g_object_set (h->element, ready_only[i], before == 0 ? 2u : before - 1, NULL);
    g_test_assert_expected_messages ();
    g_object_get (h->element, ready_only[i], &after, NULL);
    g_assert_cmpuint (after, ==, before);
    g_assert_true (pspec->flags & GST_PARAM_MUTABLE_READY);
    g_free (pattern);
  }
  g_object_set (h->element, "display-tracking-id", FALSE, NULL);
  g_object_get (h->element, "display-tracking-id", &show, NULL);
  g_assert_false (show);

  /* back in READY they change again */
  gst_element_set_state (h->element, GST_STATE_READY);
  g_object_set (h->element, "tracker-width", 320, NULL);
  {
    guint w;

    g_object_get (h->element, "tracker-width", &w, NULL);
    g_assert_cmpuint (w, ==, 320);
  }
  gst_harness_teardown (h);
}

/* ---- tracking ---- */

static gchar *work_dir;

/* Probation 0: a detection's target is Active, with its ID, on the frame that creates it. */
#define FAST_CONFIG \
  "TargetManagement:\n  probationAge: 0\n  maxShadowTrackingAge: 5\n  earlyTerminationAge: 1\n"

static gchar *
write_config (const gchar * name, const gchar * text)
{
  gchar *path = g_build_filename (work_dir, name, NULL);
  GError *err = NULL;

  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  return path;
}

/* Pops every message of @type from @bus; returns how many there were and the concatenated texts in *@texts. */
static guint
pop_messages (GstBus * bus, GstMessageType type, gchar ** texts)
{
  GString *all = g_string_new (NULL);
  GstMessage *msg;
  guint n = 0;

  while ((msg = gst_bus_pop_filtered (bus, type))) {
    GError *err = NULL;
    gchar *dbg = NULL;

    if (type == GST_MESSAGE_ERROR)
      gst_message_parse_error (msg, &err, &dbg);
    else
      gst_message_parse_warning (msg, &err, &dbg);
    g_string_append_printf (all, "%s\n", err->message);
    g_test_message ("%s: %s", type == GST_MESSAGE_ERROR ? "error" : "warning", err->message);
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

/* An rdstracker with @yaml (NULL: no config file) as its ll-config-file. */
static GstElement *
new_tracker (const gchar * yaml)
{
  GstElement *e = gst_element_factory_make ("rdstracker", NULL);

  if (yaml) {
    gchar *path = write_config ("tracker.yml", yaml);

    g_object_set (e, "ll-config-file", path, NULL);
    g_free (path);
  }
  return e;
}

/* Drops the events queued downstream so far. */
static void
drain_events (GstHarness * h)
{
  GstEvent *ev;

  while ((ev = gst_harness_try_pull_event (h)))
    gst_event_unref (ev);
}

/* A harness around @e (properties set; the harness takes it), with @bus (or none) set before it starts, so start's
 * messages reach it. */
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

/* Tears down a harness_for () harness that had @bus: the element and the bus's queued messages (which ref the
 * element) would otherwise keep each other alive. */
static void
harness_teardown_with_bus (GstHarness * h, GstBus * bus)
{
  gst_element_set_bus (h->element, NULL);
  gst_bus_set_flushing (bus, TRUE);
  gst_harness_teardown (h);
  gst_object_unref (bus);
}

#define FRAME_W 640
#define FRAME_H 480

/* A batch of @n frames of sources @sources (NULL: 0, 1, ...), with inference on frame i when @infer is NULL or
 * @infer[i], and no objects yet. */
static GstBuffer *
new_batch (guint n, const guint * sources, const gboolean * infer)
{
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (n);
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (sys_alloc, n, &nv12_64));
  s = rds_buffer_get_surface (buf);
  s->numFilled = n;
  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

    fm->source_id = sources ? sources[i] : i;
    fm->pad_index = fm->source_id;
    fm->batch_id = i;
    fm->pipeline_width = FRAME_W;
    fm->pipeline_height = FRAME_H;
    fm->bInferDone = infer ? infer[i] : TRUE;
    rds_add_frame_meta_to_batch (bm, fm);
  }
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  return buf;
}

static RdsFrameMeta *
nth_frame (GstBuffer * buf, guint n)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);

  g_assert_nonnull (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), >, n);
  return g_list_nth_data (bm->frame_meta_list, n);
}

static RdsObjectMeta *
nth_obj (RdsFrameMeta * fm, guint k)
{
  g_assert_cmpuint (g_list_length (fm->obj_meta_list), >, k);
  return g_list_nth_data (fm->obj_meta_list, k);
}

static guint
n_objs (RdsFrameMeta * fm)
{
  g_assert_cmpuint (fm->num_obj_meta, ==, g_list_length (fm->obj_meta_list));
  return fm->num_obj_meta;
}

/* A detection as rdsinfer leaves it: the unclipped box in detector_bbox_info, the clipped one in rect_params, the
 * label as obj_label and display_text (@text NULL: no display_text), component 1, untracked. */
static RdsObjectMeta *
add_det_text (GstBuffer * buf, guint frame, gfloat x, gfloat y, gfloat w, gfloat h, gint class_id, gfloat conf,
    const gchar * label, const gchar * text)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsFrameMeta *fm = nth_frame (buf, frame);
  RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);
  gfloat l = CLAMP (x, 0, FRAME_W), t = CLAMP (y, 0, FRAME_H);

  om->unique_component_id = 1;
  om->class_id = class_id;
  om->confidence = conf;
  om->object_id = RDS_UNTRACKED_OBJECT_ID;
  om->detector_bbox_info.org_bbox_coords = (RdsBbox_Coords) {
  x, y, w, h};
  om->rect_params.left = l;
  om->rect_params.top = t;
  om->rect_params.width = CLAMP (x + w, 0, FRAME_W) - l;
  om->rect_params.height = CLAMP (y + h, 0, FRAME_H) - t;
  om->tracker_bbox_info.org_bbox_coords = (RdsBbox_Coords) {
  -1, -2, -3, -4};
  om->tracker_confidence = -0.25f;
  g_strlcpy (om->obj_label, label, sizeof (om->obj_label));
  om->text_params.display_text = g_strdup (text);
  rds_add_obj_meta_to_frame (fm, om, NULL);
  return om;
}

static RdsObjectMeta *
add_det (GstBuffer * buf, guint frame, gfloat x, gfloat y, gfloat w, gfloat h, gint class_id, gfloat conf,
    const gchar * label)
{
  return add_det_text (buf, frame, x, y, w, h, class_id, conf, label, label);
}

/* Pushes @buf (taken) through and returns it as it comes out. */
static GstBuffer *
run (GstHarness * h, GstBuffer * buf)
{
  GstBuffer *out;

  g_assert_cmpint (gst_harness_push (h, buf), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  g_assert_true (out == buf);
  return out;
}

static void
assert_coords (const RdsBbox_Coords * c, gfloat l, gfloat t, gfloat w, gfloat h)
{
  g_assert_cmpfloat (c->left, ==, l);
  g_assert_cmpfloat (c->top, ==, t);
  g_assert_cmpfloat (c->width, ==, w);
  g_assert_cmpfloat (c->height, ==, h);
}

static void
assert_rect (const RdsOSD_RectParams * r, gfloat l, gfloat t, gfloat w, gfloat h)
{
  g_assert_cmpfloat (r->left, ==, l);
  g_assert_cmpfloat (r->top, ==, t);
  g_assert_cmpfloat (r->width, ==, w);
  g_assert_cmpfloat (r->height, ==, h);
}

/* The single frame's object IDs, in list order, as "0 1 ..." (untracked objects as "-"). */
static gchar *
frame_ids (RdsFrameMeta * fm)
{
  GString *str = g_string_new (NULL);

  for (GList * l = fm->obj_meta_list; l; l = l->next) {
    RdsObjectMeta *om = l->data;

    if (str->len)
      g_string_append_c (str, ' ');
    if (om->object_id == RDS_UNTRACKED_OBJECT_ID)
      g_string_append_c (str, '-');
    else
      g_string_append_printf (str, "%" G_GUINT64_FORMAT, om->object_id);
  }
  return g_string_free (str, FALSE);
}

#define assert_ids(fm, want) G_STMT_START {   \
    gchar *_ids = frame_ids (fm);              \
    g_assert_cmpstr (_ids, ==, want);          \
    g_free (_ids);                             \
  } G_STMT_END

/* Matched objects: object_id, class_id, tracker_bbox_info (unclipped), rect_params (clipped to the frame),
 * tracker_confidence and the ID in display_text; the detector's fields stay. An object without a detector box is
 * tracked on its rect. */
static void
test_write_back (void)
{
  GstHarness *h = harness_for (new_tracker (FAST_CONFIG), NULL);

  for (guint b = 0; b < 2; b++) {
    GstBuffer *buf = new_batch (1, NULL, NULL);
    gfloat dx = 5.0f * b;
    RdsObjectMeta *om;
    RdsFrameMeta *fm;

    /* past the right edge: rect_params is clipped, tracker_bbox_info isn't */
    om = add_det (buf, 0, 600 + dx, 10, 80, 50, 2, 0.8f, "car");
    om->unique_component_id = 3;
    om->rect_params.border_width = 7;
    add_det (buf, 0, 100 + dx, 100, 50, 60, 0, 0.7f, "person");
    om = add_det (buf, 0, 300 + dx, 300, 40, 40, 1, 0.6f, "bike");
    om->detector_bbox_info.org_bbox_coords = (RdsBbox_Coords) {
    0, 0, 0, 0};
    buf = run (h, buf);
    fm = nth_frame (buf, 0);
    g_assert_cmpuint (n_objs (fm), ==, 3);
    assert_ids (fm, "0 1 2");

    om = nth_obj (fm, 0);
    g_assert_cmpint (om->class_id, ==, 2);
    assert_coords (&om->tracker_bbox_info.org_bbox_coords, 600 + dx, 10, 80, 50);
    assert_rect (&om->rect_params, 600 + dx, 10, 40 - dx, 50);
    g_assert_cmpfloat (om->tracker_confidence, ==, 0.5f);
    g_assert_cmpfloat (om->confidence, ==, 0.8f);
    assert_coords (&om->detector_bbox_info.org_bbox_coords, 600 + dx, 10, 80, 50);
    g_assert_cmpstr (om->obj_label, ==, "car");
    g_assert_cmpint (om->unique_component_id, ==, 3);
    g_assert_cmpuint (om->rect_params.border_width, ==, 7);
    g_assert_cmpstr (om->text_params.display_text, ==, "car 0");

    om = nth_obj (fm, 1);
    g_assert_cmpint (om->class_id, ==, 0);
    assert_coords (&om->tracker_bbox_info.org_bbox_coords, 100 + dx, 100, 50, 60);
    assert_rect (&om->rect_params, 100 + dx, 100, 50, 60);
    g_assert_cmpfloat (om->confidence, ==, 0.7f);
    g_assert_cmpstr (om->text_params.display_text, ==, "person 1");

    om = nth_obj (fm, 2);
    assert_coords (&om->tracker_bbox_info.org_bbox_coords, 300 + dx, 300, 40, 40);
    assert_coords (&om->detector_bbox_info.org_bbox_coords, 0, 0, 0, 0);
    g_assert_cmpstr (om->text_params.display_text, ==, "bike 2");
    gst_buffer_unref (buf);
  }
  gst_harness_teardown (h);
}

/* Detections without a reported target leave the frame (their display_text is freed: ASan checks): a Tentative
 * target's, and one below minDetectorConfidence. The Tentative one is reported once Active. */
static void
test_removal (void)
{
  GstHarness *h = harness_for (new_tracker ("BaseConfig:\n  minDetectorConfidence: 0.5\n"
          "TargetManagement:\n  probationAge: 1\n"), NULL);

  for (guint b = 0; b < 3; b++) {
    GstBuffer *buf = new_batch (1, NULL, NULL);
    RdsFrameMeta *fm;

    add_det (buf, 0, 100, 100, 50, 50, 0, 0.9f, "person");
    add_det (buf, 0, 300, 100, 50, 50, 0, 0.2f, "person");
    buf = run (h, buf);
    fm = nth_frame (buf, 0);
    /* age 1 on the first frame, Active on the second (age 2 > probationAge 1) */
    if (b == 0) {
      g_assert_cmpuint (n_objs (fm), ==, 0);
    } else {
      g_assert_cmpuint (n_objs (fm), ==, 1);
      assert_ids (fm, "0");
      assert_coords (&nth_obj (fm, 0)->tracker_bbox_info.org_bbox_coords, 100, 100, 50, 50);
    }
    gst_buffer_unref (buf);
  }
  gst_harness_teardown (h);
}

/* A frame without inference: its own objects are removed, and every Active target becomes a new object: newest ID
 * first, confidence -0.1, no detector box, the last matched box, the drawing defaults, and the label, base text and
 * component of that target's last matched detection. */
static void
test_created (void)
{
  GstHarness *h = harness_for (new_tracker (FAST_CONFIG), NULL);
  const gboolean no_infer[] = { FALSE };
  GstBuffer *buf = new_batch (1, NULL, NULL);
  RdsObjectMeta *om;
  RdsFrameMeta *fm;

  om = add_det (buf, 0, 620, 300, 40, 40, 2, 0.8f, "car");
  om->unique_component_id = 3;
  om = add_det (buf, 0, 100, 100, 50, 60, 0, 0.7f, "person");
  om->unique_component_id = 5;
  gst_buffer_unref (run (h, buf));

  for (guint b = 0; b < 2; b++) {
    buf = new_batch (1, NULL, no_infer);
    /* never tracked, even on a target's box */
    add_det (buf, 0, 100, 100, 50, 60, 0, 0.9f, "stray");
    buf = run (h, buf);
    fm = nth_frame (buf, 0);
    g_assert_cmpuint (n_objs (fm), ==, 2);
    assert_ids (fm, "1 0");

    om = nth_obj (fm, 0);
    g_assert_cmpint (om->class_id, ==, 0);
    g_assert_cmpfloat (om->confidence, ==, -0.1f);
    g_assert_cmpfloat (om->tracker_confidence, ==, 0.5f);
    assert_coords (&om->detector_bbox_info.org_bbox_coords, 0, 0, 0, 0);
    assert_coords (&om->tracker_bbox_info.org_bbox_coords, 100, 100, 50, 60);
    assert_rect (&om->rect_params, 100, 100, 50, 60);
    g_assert_cmpstr (om->obj_label, ==, "person");
    g_assert_cmpint (om->unique_component_id, ==, 5);
    /* the base text, copied before its ID was appended */
    g_assert_cmpstr (om->text_params.display_text, ==, "person 1");
    g_assert_cmpuint (om->rect_params.border_width, ==, 3);
    g_assert_cmpfloat (om->rect_params.border_color.red, ==, 1.0);
    g_assert_cmpfloat (om->rect_params.border_color.green, ==, 0.0);
    g_assert_cmpfloat (om->rect_params.border_color.alpha, ==, 1.0);
    g_assert_cmpuint (om->rect_params.has_bg_color, ==, 0);
    g_assert_cmpuint (om->text_params.x_offset, ==, 100);
    g_assert_cmpuint (om->text_params.y_offset, ==, 80);
    g_assert_cmpstr (om->text_params.font_params.font_name, ==, "Serif");
    g_assert_cmpuint (om->text_params.font_params.font_size, ==, 10);
    g_assert_cmpfloat (om->text_params.font_params.font_color.blue, ==, 1.0);
    g_assert_cmpint (om->text_params.set_bg_clr, ==, 1);
    g_assert_cmpfloat (om->text_params.text_bg_clr.red, ==, 0.0);
    g_assert_cmpfloat (om->text_params.text_bg_clr.alpha, ==, 1.0);

    om = nth_obj (fm, 1);
    g_assert_cmpint (om->class_id, ==, 2);
    g_assert_cmpstr (om->obj_label, ==, "car");
    g_assert_cmpint (om->unique_component_id, ==, 3);
    g_assert_cmpstr (om->text_params.display_text, ==, "car 0");
    assert_coords (&om->tracker_bbox_info.org_bbox_coords, 620, 300, 40, 40);
    assert_rect (&om->rect_params, 620, 300, 20, 40);
    gst_buffer_unref (buf);
  }
  gst_harness_teardown (h);
}

/* display-tracking-id: "<text> <id>", " <id>" for an empty text, "<id>" for none (ours); FALSE (changeable while
 * playing) leaves display_text alone, on matched and created objects. */
static void
test_display_id (void)
{
  GstHarness *h = harness_for (new_tracker (FAST_CONFIG), NULL);
  const gboolean no_infer[] = { FALSE };
  GstBuffer *buf;
  RdsFrameMeta *fm;

  for (guint b = 0; b < 2; b++) {
    buf = new_batch (1, NULL, NULL);
    add_det_text (buf, 0, 10, 10, 50, 50, 0, 0.9f, "a", "lbl");
    add_det_text (buf, 0, 110, 10, 50, 50, 0, 0.9f, "b", "");
    add_det_text (buf, 0, 210, 10, 50, 50, 0, 0.9f, "c", NULL);
    buf = run (h, buf);
    fm = nth_frame (buf, 0);
    assert_ids (fm, "0 1 2");
    g_assert_cmpstr (nth_obj (fm, 0)->text_params.display_text, ==, b ? "lbl" : "lbl 0");
    g_assert_cmpstr (nth_obj (fm, 1)->text_params.display_text, ==, b ? "" : " 1");
    g_assert_cmpstr (nth_obj (fm, 2)->text_params.display_text, ==, b ? NULL : "2");
    gst_buffer_unref (buf);
    g_object_set (h->element, "display-tracking-id", FALSE, NULL);
  }

  buf = run (h, new_batch (1, NULL, no_infer));
  fm = nth_frame (buf, 0);
  assert_ids (fm, "2 1 0");
  g_assert_cmpstr (nth_obj (fm, 0)->text_params.display_text, ==, NULL);
  g_assert_cmpstr (nth_obj (fm, 1)->text_params.display_text, ==, "");
  g_assert_cmpstr (nth_obj (fm, 2)->text_params.display_text, ==, "lbl");
  gst_buffer_unref (buf);

  g_object_set (h->element, "display-tracking-id", TRUE, NULL);
  buf = run (h, new_batch (1, NULL, no_infer));
  fm = nth_frame (buf, 0);
  g_assert_cmpstr (nth_obj (fm, 0)->text_params.display_text, ==, "2");
  g_assert_cmpstr (nth_obj (fm, 1)->text_params.display_text, ==, " 1");
  g_assert_cmpstr (nth_obj (fm, 2)->text_params.display_text, ==, "lbl 0");
  gst_buffer_unref (buf);
  gst_harness_teardown (h);
}

/* operate-on-class-ids: objects of other classes are never given to the tracker and stay untouched and untracked,
 * also on frames without inference, after the created objects. */
static void
test_class_ids (void)
{
  GstElement *e = new_tracker (FAST_CONFIG);
  const gboolean no_infer[] = { FALSE };
  GstHarness *h;
  GstBuffer *buf;
  RdsFrameMeta *fm;
  RdsObjectMeta *om;

  g_object_set (e, "operate-on-class-ids", " 0; 2;", NULL);
  h = harness_for (e, NULL);
  for (guint b = 0; b < 2; b++) {
    buf = new_batch (1, NULL, b ? no_infer : NULL);
    add_det (buf, 0, 10, 10, 50, 50, 1, 0.9f, "skip");
    if (!b) {
      add_det (buf, 0, 110, 10, 50, 50, 0, 0.9f, "a");
      add_det (buf, 0, 210, 10, 50, 50, 2, 0.9f, "b");
    }
    buf = run (h, buf);
    fm = nth_frame (buf, 0);
    assert_ids (fm, b ? "1 0 -" : "- 0 1");
    om = nth_obj (fm, b ? 2 : 0);
    g_assert_cmpint (om->class_id, ==, 1);
    g_assert_cmpstr (om->text_params.display_text, ==, "skip");
    g_assert_cmpfloat (om->tracker_confidence, ==, -0.25f);
    assert_coords (&om->tracker_bbox_info.org_bbox_coords, -1, -2, -3, -4);
    gst_buffer_unref (buf);
  }
  gst_harness_teardown (h);
}

/* checkClassMatch 0: a target follows its detection's class (and reports it on frames without inference). With 1
 * (the default) the other class can't match, and it can't start a target either: the duplicate check ignores
 * class, so it is removed, and the missed target goes Inactive. */
static void
test_class_change (void)
{
  for (guint match = 0; match <= 1; match++) {
    gchar *yaml = g_strdup_printf ("BaseConfig:\n  checkClassMatch: %u\n" FAST_CONFIG, match);
    GstHarness *h = harness_for (new_tracker (yaml), NULL);
    const gboolean no_infer[] = { FALSE };
    GstBuffer *buf;
    RdsFrameMeta *fm;

    for (guint b = 0; b < 2; b++) {
      buf = new_batch (1, NULL, NULL);
      add_det (buf, 0, 100, 100, 50, 50, b ? 4 : 1, 0.9f, "x");
      buf = run (h, buf);
      fm = nth_frame (buf, 0);
      if (match && b) {
        g_assert_cmpuint (n_objs (fm), ==, 0);
      } else {
        assert_ids (fm, "0");
        g_assert_cmpint (nth_obj (fm, 0)->class_id, ==, b ? 4 : 1);
      }
      gst_buffer_unref (buf);
    }
    buf = run (h, new_batch (1, NULL, no_infer));
    fm = nth_frame (buf, 0);
    /* with 1 the target missed its frame: Inactive, not reported */
    assert_ids (fm, match ? "" : "0");
    if (!match)
      g_assert_cmpint (nth_obj (fm, 0)->class_id, ==, 4);
    gst_buffer_unref (buf);
    gst_harness_teardown (h);
    g_free (yaml);
  }
}

/* useUniqueID: one upper half for every stream, the counter in the lower half; without it the upper half is 0. */
static void
test_unique_id (void)
{
  for (guint unique = 0; unique <= 1; unique++) {
    gchar *yaml = g_strdup_printf ("TrajectoryManagement:\n  useUniqueID: %u\n" FAST_CONFIG, unique);
    GstHarness *h = harness_for (new_tracker (yaml), NULL);
    const guint sources[] = { 3, 7 };
    guint64 upper = 0;

    for (guint b = 0; b < 2; b++) {
      GstBuffer *buf = new_batch (2, sources, NULL);

      add_det (buf, 0, 100, 100, 50, 50, 0, 0.9f, "a");
      add_det (buf, 1, 100, 100, 50, 50, 0, 0.9f, "b");
      add_det (buf, 1, 300, 100, 50, 50, 0, 0.9f, "c");
      buf = run (h, buf);
      for (guint f = 0; f < 2; f++) {
        RdsFrameMeta *fm = nth_frame (buf, f);

        for (guint k = 0; k < n_objs (fm); k++) {
          guint64 id = nth_obj (fm, k)->object_id;

          if (f == 0 && k == 0 && b == 0)
            upper = id >> 32;
          g_assert_cmpuint (id >> 32, ==, upper);
          g_assert_cmpuint (id & G_MAXUINT32, ==, f + k);
        }
      }
      gst_buffer_unref (buf);
    }
    if (!unique)
      g_assert_cmpuint (upper, ==, 0);
    g_test_message ("useUniqueID %u: upper half %" G_GINT64_MODIFIER "x", unique, upper);
    gst_harness_teardown (h);
    g_free (yaml);
  }
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

/* One batch of sources 0 and 1, a detection each (source 0's at @x0), returning their IDs as "s0 s1". */
static gchar *
two_sources (GstHarness * h, gfloat x0)
{
  GstBuffer *buf = new_batch (2, NULL, NULL);
  gchar *a, *b, *ids;

  add_det (buf, 0, x0, 100, 50, 50, 0, 0.9f, "a");
  add_det (buf, 1, 100, 100, 50, 50, 0, 0.9f, "b");
  buf = run (h, buf);
  a = frame_ids (nth_frame (buf, 0));
  b = frame_ids (nth_frame (buf, 1));
  ids = g_strdup_printf ("%s %s", a, b);
  g_free (a);
  g_free (b);
  gst_buffer_unref (buf);
  return ids;
}

/* tracking-id-reset-mode 0-3 against a stream reset and a stream EOS of source 0, then (mode 0) a pad deletion of
 * source 1 and a flush: the IDs of sources 0 and 1 after each. Every event goes downstream unchanged. */
static void
test_events (void)
{
  static const struct
  {
    const gchar *start, *after_reset, *after_eos;
  } want[] = {
    /* 0: nothing changes; source 0's detection moves away after the EOS, so it is a new target */
    {"0 1", "0 1", "2 1"},
    /* 1: the reset ends source 0's target; the counter continues */
    {"0 1", "2 1", "3 1"},
    /* 2: per-stream IDs; the EOS ends source 0's target and restarts its IDs */
    {"0 0", "0 0", "0 0"},
    /* 3: both */
    {"0 0", "2 0", "0 0"},
  };

  for (guint mode = 0; mode < G_N_ELEMENTS (want); mode++) {
    GstElement *e = new_tracker (FAST_CONFIG);
    GstHarness *h;
    gchar *ids;
    GstSegment seg;

    g_test_message ("tracking-id-reset-mode %u", mode);
    g_object_set (e, "tracking-id-reset-mode", mode, NULL);
    h = harness_for (e, NULL);
    push_event_through (h, rds_event_new_pad_added (0));
    push_event_through (h, rds_event_new_pad_added (1));
    ids = two_sources (h, 100);
    g_assert_cmpstr (ids, ==, want[mode].start);
    g_free (ids);
    push_event_through (h, rds_event_new_stream_reset (0));
    ids = two_sources (h, 100);
    g_assert_cmpstr (ids, ==, want[mode].after_reset);
    g_free (ids);
    push_event_through (h, rds_event_new_stream_eos (0));
    ids = two_sources (h, 400);
    g_assert_cmpstr (ids, ==, want[mode].after_eos);
    g_free (ids);

    if (mode == 0) {
      /* the deleted source's state is gone: its object is a new target */
      push_event_through (h, rds_event_new_pad_deleted (1));
      ids = two_sources (h, 400);
      g_assert_cmpstr (ids, ==, "2 3");
      g_free (ids);
      /* a flush drops every stream; the counter continues */
      push_event_through (h, gst_event_new_flush_start ());
      push_event_through (h, gst_event_new_flush_stop (TRUE));
      gst_segment_init (&seg, GST_FORMAT_TIME);
      push_event_through (h, gst_event_new_segment (&seg));
      ids = two_sources (h, 400);
      g_assert_cmpstr (ids, ==, "4 5");
      g_free (ids);
    }
    gst_harness_teardown (h);
  }
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
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, &texts), >=, 1);
  gst_element_set_state (pipeline, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (pipeline);
  return texts;
}

/* A custom ll-lib-file, a config that can't be read or parsed or is invalid, and a bad operate-on-class-ids fail the
 * start with a message naming the property or the file. */
static void
test_start_errors (void)
{
  gchar *missing = g_build_filename (work_dir, "missing.yml", NULL), *texts, *path;
  GstElement *e;

  e = gst_element_factory_make ("rdstracker", NULL);
  g_object_set (e, "ll-lib-file", "/opt/trackers/libmytracker.so", NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "element property ll-lib-file: /opt/trackers/libmytracker.so:"));
  g_assert_nonnull (strstr (texts, "not supported"));
  g_free (texts);

  e = gst_element_factory_make ("rdstracker", NULL);
  g_object_set (e, "ll-config-file", missing, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, missing));
  g_free (texts);

  e = new_tracker ("BaseConfig:\n  minDetectorConfidence: [0.3\n");
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "tracker.yml"));
  g_free (texts);

  e = new_tracker ("TargetManagement:\n  probationAge: -3\n");
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "probationAge"));
  g_free (texts);

  e = gst_element_factory_make ("rdstracker", NULL);
  g_object_set (e, "operate-on-class-ids", "0;car", NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "element property operate-on-class-ids: 0;car:"));
  g_free (texts);

  /* the corpus' malformed config */
  path = g_build_filename (RDS_TRACKER_CONFIGS, "malformed.yml", NULL);
  e = gst_element_factory_make ("rdstracker", NULL);
  g_object_set (e, "ll-config-file", path, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "malformed.yml"));
  g_free (texts);
  g_free (path);
  g_free (missing);
}

/* NVIDIA's library name (bare or as a path), ours, and an empty name all select the built-in tracker. */
static void
test_builtin_lib (void)
{
  static const gchar *names[] = { "libnvds_nvmultiobjecttracker.so",
    "/opt/nvidia/deepstream/deepstream/lib/libnvds_nvmultiobjecttracker.so", "librdstracker-builtin.so",
    "librdstracker-builtin", "",
  };

  for (guint i = 0; i < G_N_ELEMENTS (names); i++) {
    GstElement *e = new_tracker (FAST_CONFIG);
    GstBus *bus = gst_bus_new ();
    GstHarness *h;
    GstBuffer *buf;

    g_test_message ("%s", names[i]);
    g_object_set (e, "ll-lib-file", names[i], NULL);
    h = harness_for (e, bus);
    g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
    g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, NULL), ==, 0);
    buf = new_batch (1, NULL, NULL);
    add_det (buf, 0, 100, 100, 50, 50, 0, 0.9f, "a");
    buf = run (h, buf);
    assert_ids (nth_frame (buf, 0), "0");
    gst_buffer_unref (buf);
    harness_teardown_with_bus (h, bus);
  }
}

/* Each accepted no-op property set to other than its default warns once at start, naming itself; at their defaults
 * nothing warns. The config's own warnings are posted too. */
static void
test_noop_warnings (void)
{
  static const gchar *props[] = { "compute-hw", "tracking-surface-type", "user-meta-pool-size",
    "input-tensor-meta", "tensor-meta-gie-id", "sub-batches", "sub-batch-err-recovery-trial-cnt",
  };
  GstElement *e = new_tracker (FAST_CONFIG);
  GstBus *bus = gst_bus_new ();
  GstHarness *h;
  gchar *texts, *path;

  g_object_set (e, "tracking-surface-type", 1, "user-meta-pool-size", 8, "input-tensor-meta", TRUE,
      "tensor-meta-gie-id", 2, "sub-batches", "0,1;2", "sub-batch-err-recovery-trial-cnt", -1,
      "tracker-width", 640, "tracker-height", 384, "gpu-id", 1, NULL);
  gst_util_set_object_arg (G_OBJECT (e), "compute-hw", "gpu");
  h = harness_for (e, bus);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, G_N_ELEMENTS (props));
  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    gchar *needle = g_strdup_printf ("element property %s:", props[i]);
    const gchar *at = strstr (texts, needle);

    g_assert_nonnull (at);
    g_assert_null (strstr (at + 1, needle));
    g_free (needle);
  }
  g_assert_nonnull (strstr (texts, "sub-batches: 0,1;2: ignored: the tracker runs as one instance"));
  g_free (texts);
  harness_teardown_with_bus (h, bus);

  bus = gst_bus_new ();
  h = harness_for (new_tracker (FAST_CONFIG), bus);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, NULL), ==, 0);
  harness_teardown_with_bus (h, bus);

  path = g_build_filename (RDS_TRACKER_CONFIGS, "noop.yml", NULL);
  e = gst_element_factory_make ("rdstracker", NULL);
  g_object_set (e, "ll-config-file", path, NULL);
  bus = gst_bus_new ();
  h = harness_for (e, bus);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, NULL), >, 0);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  harness_teardown_with_bus (h, bus);
  g_free (path);
}

/* PLAYING -> NULL -> PLAYING: the core is rebuilt (IDs start again at 0) and tracking works. */
static void
test_restart (void)
{
  GstHarness *h = harness_for (new_tracker (FAST_CONFIG), NULL);
  GstElement *e = h->element;

  for (guint run_no = 0; run_no < 2; run_no++) {
    for (guint b = 0; b < 2; b++) {
      GstBuffer *buf = new_batch (1, NULL, NULL);

      add_det (buf, 0, 100 + 5 * b, 100, 50, 50, 0, 0.9f, "a");
      add_det (buf, 0, 300 + 5 * b, 100, 50, 50, 0, 0.9f, "b");
      buf = run (h, buf);
      assert_ids (nth_frame (buf, 0), "0 1");
      gst_buffer_unref (buf);
    }
    if (run_no == 0) {
      g_assert_cmpint (gst_element_set_state (e, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
      g_assert_cmpint (gst_element_set_state (e, GST_STATE_PLAYING), ==, GST_STATE_CHANGE_SUCCESS);
      /* the element's pads lost their sticky events */
      g_assert_true (gst_harness_push_event (h, gst_event_new_stream_start ("restart")));
      gst_harness_set_caps_str (h, IN_CAPS, IN_CAPS);
      drain_events (h);
    }
  }
  gst_harness_teardown (h);
}

typedef struct
{
  GstHarness *h;
  GstBuffer *buf;
  gint done;
} PushJob;

static gpointer
push_thread (gpointer data)
{
  PushJob *job = data;

  g_assert_cmpint (gst_harness_push (job->h, job->buf), ==, GST_FLOW_OK);
  g_atomic_int_set (&job->done, 1);
  return NULL;
}

/* The batch is processed under the batch's meta lock: while another thread holds it, the push waits. */
static void
test_meta_lock (void)
{
  GstHarness *h = harness_for (new_tracker (FAST_CONFIG), NULL);
  PushJob job = {.h = h };
  RdsBatchMeta *bm;
  GstBuffer *out;
  GThread *t;

  job.buf = new_batch (1, NULL, NULL);
  add_det (job.buf, 0, 100, 100, 50, 50, 0, 0.9f, "a");
  bm = gst_buffer_get_rds_batch_meta (job.buf);
  rds_acquire_meta_lock (bm);
  t = g_thread_new ("push", push_thread, &job);
  g_usleep (200 * 1000);
  g_assert_cmpint (g_atomic_int_get (&job.done), ==, 0);
  g_assert_true (RDS_UNTRACKED_OBJECT_ID == nth_obj (nth_frame (job.buf, 0), 0)->object_id);
  rds_release_meta_lock (bm);
  g_thread_join (t);
  g_assert_cmpint (job.done, ==, 1);
  out = gst_harness_pull (h);
  assert_ids (nth_frame (out, 0), "0");
  gst_buffer_unref (out);
  gst_harness_teardown (h);
}

/* 500 batches of 4 frames with seeded random objects (moving boxes, some cut by the frame edges, random classes and
 * confidences, NULL and empty texts), about one frame in four without inference, and random stream events and
 * flushes, with the default IOU config and an NvSORT one. Checks the output's invariants; ASan checks the memory. */
static void
test_random (void)
{
  static const gchar *texts[] = { "car", "", NULL };
  const gchar *configs[] = { NULL, "nvsort.yml" };

  for (guint c = 0; c < G_N_ELEMENTS (configs); c++) {
    GstElement *e = gst_element_factory_make ("rdstracker", NULL);
    GRand *rand = g_rand_new_with_seed (1234 + c);
    GstHarness *h;
    guint n_tracked = 0, n_created = 0;

    if (configs[c]) {
      gchar *path = g_build_filename (RDS_TRACKER_CONFIGS, configs[c], NULL);

      g_object_set (e, "ll-config-file", path, NULL);
      g_free (path);
    }
    g_object_set (e, "operate-on-class-ids", "0;1;2", "tracking-id-reset-mode", 3, NULL);
    h = harness_for (e, NULL);
    for (guint b = 0; b < 500; b++) {
      gboolean infer[4];
      guint sources[4];
      GstBuffer *buf;

      for (guint f = 0; f < 4; f++) {
        infer[f] = g_rand_int_range (rand, 0, 4) != 0;
        sources[f] = f;
      }
      buf = new_batch (4, sources, infer);
      for (guint f = 0; f < 4; f++) {
        guint n = g_rand_int_range (rand, 0, 9);

        for (guint k = 0; k < n; k++) {
          /* objects on a slowly moving grid, so they persist from batch to batch, plus jitter */
          gfloat x = (k % 4) * 170.0f + (b % 50) * 2.0f - 30 + g_rand_double_range (rand, -3, 3);
          gfloat y = (k / 4) * 230.0f + g_rand_double_range (rand, -3, 3);

          add_det_text (buf, f, x, y, 80 + g_rand_double_range (rand, -4, 4), 120, (gint) (k % 4),
              g_rand_double_range (rand, 0.1, 1.0), "obj", texts[g_rand_int_range (rand, 0, 3)]);
        }
      }
      buf = run (h, buf);
      for (guint f = 0; f < 4; f++) {
        RdsFrameMeta *fm = nth_frame (buf, f);

        n_objs (fm);
        for (GList * l = fm->obj_meta_list; l; l = l->next) {
          RdsObjectMeta *om = l->data;

          if (om->object_id == RDS_UNTRACKED_OBJECT_ID) {
            g_assert_cmpint (om->class_id, ==, 3);
            continue;
          }
          g_assert_cmpint (om->class_id, !=, 3);
          g_assert_cmpfloat (om->rect_params.left, >=, 0);
          g_assert_cmpfloat (om->rect_params.left + om->rect_params.width, <=, FRAME_W);
          g_assert_cmpfloat (om->rect_params.width, >=, 0);
          g_assert_nonnull (om->text_params.display_text);
          if (om->confidence == -0.1f)
            n_created++;
          else
            n_tracked++;
          if (!infer[f])
            g_assert_cmpfloat (om->confidence, ==, -0.1f);
        }
      }
      gst_buffer_unref (buf);

      switch (g_rand_int_range (rand, 0, 40)) {
        case 0:
          push_event_through (h, rds_event_new_stream_reset (g_rand_int_range (rand, 0, 4)));
          break;
        case 1:
          push_event_through (h, rds_event_new_stream_eos (g_rand_int_range (rand, 0, 4)));
          break;
        case 2:
          push_event_through (h, rds_event_new_pad_deleted (g_rand_int_range (rand, 0, 4)));
          break;
        case 3:{
          GstSegment seg;

          push_event_through (h, gst_event_new_flush_start ());
          push_event_through (h, gst_event_new_flush_stop (TRUE));
          gst_segment_init (&seg, GST_FORMAT_TIME);
          push_event_through (h, gst_event_new_segment (&seg));
          break;
        }
        default:
          break;
      }
    }
    g_test_message ("%s: %u tracked, %u created objects", configs[c] ? configs[c] : "defaults", n_tracked,
        n_created);
    g_assert_cmpuint (n_tracked, >, 1000);
    g_assert_cmpuint (n_created, >, 100);
    g_rand_free (rand);
    gst_harness_teardown (h);
  }
}

/* NvSORT end to end (the corpus' nvsort.yml: probationAge 4, useUniqueID): a box moving 10 px a frame is reported
 * from its fifth frame, then on frames without inference at the Kalman prediction, which keeps moving. */
static void
test_nvsort (void)
{
  gchar *path = g_build_filename (RDS_TRACKER_CONFIGS, "nvsort.yml", NULL);
  GstElement *e = gst_element_factory_make ("rdstracker", NULL);
  const gboolean no_infer[] = { FALSE };
  GstHarness *h;
  guint64 id = 0;
  gfloat last_x = 0;

  g_object_set (e, "ll-config-file", path, NULL);
  h = harness_for (e, NULL);
  for (guint f = 0; f < 12; f++) {
    gboolean infer = f < 8;
    GstBuffer *buf = new_batch (1, NULL, infer ? NULL : no_infer);
    RdsFrameMeta *fm;
    RdsObjectMeta *om;

    if (infer)
      add_det (buf, 0, 100 + 10 * f, 200, 60, 120, 0, 0.9f, "person");
    buf = run (h, buf);
    fm = nth_frame (buf, 0);
    if (f < 4) {
      g_assert_cmpuint (n_objs (fm), ==, 0);
      gst_buffer_unref (buf);
      continue;
    }
    g_assert_cmpuint (n_objs (fm), ==, 1);
    om = nth_obj (fm, 0);
    if (f == 4)
      id = om->object_id;
    g_assert_cmpuint (om->object_id, ==, id);
    g_assert_cmpuint (id & G_MAXUINT32, ==, 0);
    if (infer) {
      assert_coords (&om->tracker_bbox_info.org_bbox_coords, 100 + 10 * f, 200, 60, 120);
      g_assert_cmpfloat (om->confidence, ==, 0.9f);
    } else {
      g_assert_cmpfloat (om->confidence, ==, -0.1f);
      g_assert_cmpfloat (om->tracker_bbox_info.org_bbox_coords.left, >, last_x);
      /* the filter's velocity lags the true 10 px a frame: behind the object, never far ahead of it */
      g_assert_cmpfloat (om->tracker_bbox_info.org_bbox_coords.left, <, 100 + 10 * f + 5);
      g_assert_cmpstr (om->obj_label, ==, "person");
    }
    last_x = om->tracker_bbox_info.org_bbox_coords.left;
    gst_buffer_unref (buf);
  }
  gst_harness_teardown (h);
  g_free (path);
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
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  sys_alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);
  work_dir = g_dir_make_tmp ("rdstracker-XXXXXX", NULL);
  g_assert_nonnull (work_dir);

  g_test_add_func ("/cpu/tracker/factory", test_factory);
  g_test_add_func ("/cpu/tracker/pad-templates", test_pad_templates);
  g_test_add_func ("/cpu/tracker/properties", test_properties);
  g_test_add_func ("/cpu/tracker/property-roundtrip", test_property_roundtrip);
  g_test_add_func ("/cpu/tracker/passthrough", test_passthrough);
  g_test_add_func ("/cpu/tracker/ready-only", test_ready_only);
  g_test_add_func ("/cpu/tracker/write-back", test_write_back);
  g_test_add_func ("/cpu/tracker/removal", test_removal);
  g_test_add_func ("/cpu/tracker/created", test_created);
  g_test_add_func ("/cpu/tracker/display-id", test_display_id);
  g_test_add_func ("/cpu/tracker/class-ids", test_class_ids);
  g_test_add_func ("/cpu/tracker/class-change", test_class_change);
  g_test_add_func ("/cpu/tracker/unique-id", test_unique_id);
  g_test_add_func ("/cpu/tracker/events", test_events);
  g_test_add_func ("/cpu/tracker/start-errors", test_start_errors);
  g_test_add_func ("/cpu/tracker/builtin-lib", test_builtin_lib);
  g_test_add_func ("/cpu/tracker/noop-warnings", test_noop_warnings);
  g_test_add_func ("/cpu/tracker/restart", test_restart);
  g_test_add_func ("/cpu/tracker/meta-lock", test_meta_lock);
  g_test_add_func ("/cpu/tracker/random", test_random);
  g_test_add_func ("/cpu/tracker/nvsort", test_nvsort);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_object_unref (sys_alloc);
    remove_dir (work_dir);
    g_free (work_dir);
    gst_deinit ();
    return ret;
  }
}
