/* rdstracker: registration, pad templates, every property with its type, range, default and state flags (enforced
 * once streaming), and a system-memory batch passing through with its pixels and metadata untouched (cpu).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"

#include <gst/check/gstharness.h>
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
 * went in. */
static void
test_passthrough (void)
{
  GstHarness *h = gst_harness_new ("rdstracker");
  GstBuffer *in, *out;
  RdsBatchMeta *bm;
  RdsBufSurface *s;
  guint n_frames = 0;

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

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  sys_alloc = rds_allocator_new (0, RDS_BUF_MEM_SYSTEM);

  g_test_add_func ("/cpu/tracker/factory", test_factory);
  g_test_add_func ("/cpu/tracker/pad-templates", test_pad_templates);
  g_test_add_func ("/cpu/tracker/properties", test_properties);
  g_test_add_func ("/cpu/tracker/property-roundtrip", test_property_roundtrip);
  g_test_add_func ("/cpu/tracker/passthrough", test_passthrough);
  g_test_add_func ("/cpu/tracker/ready-only", test_ready_only);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_object_unref (sys_alloc);
    gst_deinit ();
    return ret;
  }
}
