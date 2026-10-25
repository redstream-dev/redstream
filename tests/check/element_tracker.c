/* rdstracker: registration, pad templates, every property with its type, range, default and state flags, and a
 * system-memory batch passing through with its metadata untouched (cpu).
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

/* A system-memory batch of @n frames with two objects each (untracked, as a detector leaves them). */
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

    g_assert_cmpint (rds_buf_surface_memset (s, i, -1, (guint8) (10 * (i + 1))), ==, 0);
    fm->source_id = i;
    fm->pad_index = i;
    fm->batch_id = i;
    fm->source_frame_width = 64;
    fm->source_frame_height = 48;
    rds_add_frame_meta_to_batch (bm, fm);
    for (guint k = 0; k < 2; k++) {
      RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);

      om->unique_component_id = 1;
      om->class_id = (gint) k;
      om->object_id = RDS_UNTRACKED_OBJECT_ID;
      om->confidence = 0.5f + 0.1f * k;
      om->rect_params.left = 4.0f + i;
      om->rect_params.top = 6.0f + k;
      om->rect_params.width = 10.0f;
      om->rect_params.height = 12.0f;
      om->detector_bbox_info.org_bbox_coords.left = om->rect_params.left;
      om->detector_bbox_info.org_bbox_coords.top = om->rect_params.top;
      om->detector_bbox_info.org_bbox_coords.width = om->rect_params.width;
      om->detector_bbox_info.org_bbox_coords.height = om->rect_params.height;
      rds_add_obj_meta_to_frame (fm, om, NULL);
    }
  }
  s->numFilled = n;
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  GST_BUFFER_PTS (buf) = 5 * GST_SECOND;
  return buf;
}

/* The same buffer comes out, with the same surface, the same batch meta and every object as it went in. */
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

      g_assert_true (om->object_id == RDS_UNTRACKED_OBJECT_ID);
      g_assert_cmpfloat (om->rect_params.width, ==, 10.0f);
      g_assert_cmpfloat (om->rect_params.height, ==, 12.0f);
      g_assert_cmpfloat (om->rect_params.left, ==, 4.0f + n_frames);
      g_assert_cmpfloat (om->tracker_bbox_info.org_bbox_coords.width, ==, 0.0f);
      g_assert_cmpfloat (om->tracker_confidence, ==, 0.0f);
    }
    g_assert_cmpuint (k, ==, 2);
    n_frames++;
  }
  g_assert_cmpuint (n_frames, ==, 2);

  gst_buffer_unref (out);
  gst_buffer_unref (in);
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
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_object_unref (sys_alloc);
    gst_deinit ();
    return ret;
  }
}
