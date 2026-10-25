/* rdsinfer: registration, pad templates, properties and how they override the config file (cpu); start errors,
 * the tensor path on the scale2 test model (output = 2 x input) against the CPU preprocessing reference, partial
 * batches, tensor meta through rdsstreamdemux, interval, the detector path with the fixture parser, and concurrent
 * metadata readers (gpu, skipped without a ROCm device).
 * The element's test hooks (gstrdsinfer.h) are looked up in the loaded plugin.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include "infer-preprocess-ref.h"
#include "infer-parser-fixture.h"
#include "rds-infer-config.h"

#include <gmodule.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <rds/rds.h>
#include <rds/rds-infer-meta.h>
#include <string.h>

#define RDS_CAPS "video/x-raw(memory:RDS), format=NV12, width=64, height=48, framerate=30/1"
#define WORK_DIR RDS_TEST_BUILD_DIR "/element-infer"

typedef GstStructure *(*GetConfigFunc) (GstElement * element, GError ** error);
typedef gboolean (*GetOutputSetsFunc) (GstElement * element, guint * n_sets, guint * n_free);
typedef gboolean (*ReadInputFunc) (GstElement * element, gfloat * dst, gsize n_floats);

static GetConfigFunc hook_get_config;
static GetOutputSetsFunc hook_get_output_sets;
static ReadInputFunc hook_read_input;

static void
load_hooks (void)
{
  GstPlugin *found, *plugin;
  GModule *module;

  if (hook_get_config)
    return;
  found = gst_registry_find_plugin (gst_registry_get (), "rdsinfer");
  g_assert_nonnull (found);
  plugin = gst_plugin_load (found);
  g_assert_nonnull (plugin);
  gst_object_unref (found);
  module = g_module_open (gst_plugin_get_filename (plugin), G_MODULE_BIND_LOCAL);
  g_assert_nonnull (module);
  g_assert_true (g_module_symbol (module, "rds_infer_test_get_config", (gpointer *) & hook_get_config));
  g_assert_true (g_module_symbol (module, "rds_infer_test_get_output_sets", (gpointer *) & hook_get_output_sets));
  g_assert_true (g_module_symbol (module, "rds_infer_test_read_input", (gpointer *) & hook_read_input));
  /* the module stays open: the plugin is never unloaded either */
  gst_object_unref (plugin);
}

/* Writes @text to WORK_DIR/@name; returns the path. */
static gchar *
write_file (const gchar * name, const gchar * text)
{
  gchar *path = g_build_filename (WORK_DIR, name, NULL);
  GError *err = NULL;

  g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  return path;
}

/* scale2.onnx copied into WORK_DIR, so its compiled programs are cached there (not in the source tree). */
static void
ensure_model (void)
{
  gchar *contents = NULL, *dst = g_build_filename (WORK_DIR, "scale2.onnx", NULL);
  gsize len = 0;
  GError *err = NULL;

  if (!g_file_test (dst, G_FILE_TEST_EXISTS)) {
    g_assert_true (g_file_get_contents (RDS_TEST_MODELS "/scale2.onnx", &contents, &len, &err));
    g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
    g_assert_true (g_file_set_contents (dst, contents, (gssize) len, &err));
    g_free (contents);
  }
  g_free (dst);
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

/* ---- cpu: element basics ---- */

static void
test_factory (void)
{
  GstElementFactory *factory = gst_element_factory_find ("rdsinfer");

  g_assert_nonnull (factory);
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS), ==,
      "Filter/Analyzer/Video");
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), ==, GST_RANK_NONE);
  g_assert_cmpuint (gst_element_factory_get_num_pad_templates (factory), ==, 2);
  gst_object_unref (factory);
}

static void
check_template_caps (GstElement * e, const gchar * name)
{
  GstPadTemplate *templ = gst_element_class_get_pad_template (GST_ELEMENT_GET_CLASS (e), name);
  GstCaps *caps, *want, *sysmem;
  const gchar *formats[] = { "NV12", "RGBA", "BGRA", "RGBx", "BGRx" };

  g_assert_nonnull (templ);
  g_assert_cmpint (GST_PAD_TEMPLATE_PRESENCE (templ), ==, GST_PAD_ALWAYS);
  caps = gst_pad_template_get_caps (templ);
  for (guint i = 0; i < G_N_ELEMENTS (formats); i++) {
    want = gst_caps_from_string (RDS_CAPS);
    gst_caps_set_simple (want, "format", G_TYPE_STRING, formats[i], NULL);
    g_assert_true (gst_caps_can_intersect (caps, want));
    gst_caps_unref (want);
  }
  /* memory:RDS only: no system memory, no other formats */
  sysmem = gst_caps_from_string ("video/x-raw, format=NV12, width=64, height=48");
  g_assert_false (gst_caps_can_intersect (caps, sysmem));
  gst_caps_unref (sysmem);
  want = gst_caps_from_string ("video/x-raw(memory:RDS), format=I420, width=64, height=48");
  g_assert_false (gst_caps_can_intersect (caps, want));
  gst_caps_unref (want);
  gst_caps_unref (caps);
}

static void
test_pad_templates (void)
{
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);

  g_assert_nonnull (e);
  check_template_caps (e, "sink");
  check_template_caps (e, "src");
  gst_object_unref (e);
}

/* Every documented nvinfer property, with its documented type. */
static void
test_properties (void)
{
  static const struct
  {
    const gchar *name;
    GType type;
  } props[] = {
    {"config-file-path", G_TYPE_STRING},
    {"unique-id", G_TYPE_UINT},
    {"process-mode", G_TYPE_UINT},
    {"infer-on-gie-id", G_TYPE_UINT},
    {"infer-on-class-ids", G_TYPE_STRING},
    {"filter-out-class-ids", G_TYPE_STRING},
    {"model-engine-file", G_TYPE_STRING},
    {"batch-size", G_TYPE_UINT},
    {"interval", G_TYPE_INT},
    {"gpu-id", G_TYPE_UINT},
    {"output-tensor-meta", G_TYPE_BOOLEAN},
    {"raw-output-file-write", G_TYPE_BOOLEAN},
    {"raw-output-generated-callback", G_TYPE_POINTER},
    {"raw-output-generated-userdata", G_TYPE_POINTER},
    {"output-instance-mask", G_TYPE_BOOLEAN},
    {"input-tensor-meta", G_TYPE_BOOLEAN},
    {"crop-objects-to-roi-boundary", G_TYPE_BOOLEAN},
    {"clip-object-outside-roi", G_TYPE_BOOLEAN},
  };
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);
  gchar *path = NULL;

  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    GParamSpec *pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (e), props[i].name);

    g_assert_nonnull (pspec);
    g_test_message ("%s", props[i].name);
    g_assert_true (G_PARAM_SPEC_VALUE_TYPE (pspec) == props[i].type);
    g_assert_true ((pspec->flags & G_PARAM_READWRITE) == G_PARAM_READWRITE);
  }
  g_object_get (e, "config-file-path", &path, NULL);
  g_assert_null (path);
  g_object_set (e, "config-file-path", "/tmp/pgie.yml", NULL);
  g_object_get (e, "config-file-path", &path, NULL);
  g_assert_cmpstr (path, ==, "/tmp/pgie.yml");
  g_free (path);
  gst_object_unref (e);
}

/* "MIN..MAX", or ">= MIN" when MAX is the type's maximum: the format of the config key table's ranges. */
static gchar *
pspec_range (GParamSpec * pspec)
{
  guint64 min, max, type_max;

  if (G_IS_PARAM_SPEC_UINT (pspec)) {
    min = G_PARAM_SPEC_UINT (pspec)->minimum;
    max = G_PARAM_SPEC_UINT (pspec)->maximum;
    type_max = G_MAXUINT;
  } else {
    g_assert_true (G_IS_PARAM_SPEC_INT (pspec));
    g_assert_cmpint (G_PARAM_SPEC_INT (pspec)->minimum, >=, 0);
    min = (guint64) G_PARAM_SPEC_INT (pspec)->minimum;
    max = (guint64) G_PARAM_SPEC_INT (pspec)->maximum;
    type_max = G_MAXINT;
  }
  return max == type_max ? g_strdup_printf (">= %" G_GUINT64_FORMAT, min)
      : g_strdup_printf ("%" G_GUINT64_FORMAT "..%" G_GUINT64_FORMAT, min, max);
}

typedef struct
{
  const gchar *key;
  gchar *range;
} KeyRange;

static void
find_range (const RdsInferKeyDesc * key, gpointer user_data)
{
  KeyRange *kr = user_data;

  if (!strcmp (key->group, "property") && !strcmp (key->name, kr->key))
    kr->range = g_strdup (key->range);
}

/* The numeric properties carry the ranges of their config keys (the config validation after overrides doesn't
 * re-check ranges). */
static void
test_property_ranges (void)
{
  static const struct
  {
    const gchar *prop, *key;
  } map[] = {
    {"batch-size", "batch-size"},
    {"interval", "interval"},
    {"process-mode", "process-mode"},
    {"gpu-id", "gpu-id"},
  };
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);
  gchar *r;

  for (guint i = 0; i < G_N_ELEMENTS (map); i++) {
    KeyRange kr = {.key = map[i].key };

    rds_infer_config_foreach_key (find_range, &kr);
    g_assert_nonnull (kr.range);
    r = pspec_range (g_object_class_find_property (G_OBJECT_GET_CLASS (e), map[i].prop));
    g_test_message ("%s: property %s, config %s", map[i].prop, r, kr.range);
    g_assert_cmpstr (r, ==, kr.range);
    g_free (r);
    g_free (kr.range);
  }
  /* unique-id is documented as 0..G_MAXUINT (gie-unique-id starts at 1) */
  r = pspec_range (g_object_class_find_property (G_OBJECT_GET_CLASS (e), "unique-id"));
  g_assert_cmpstr (r, ==, ">= 0");
  g_free (r);
  gst_object_unref (e);
}

#define BASE_CONFIG \
  "[property]\n" \
  "onnx-file=scale2.onnx\n" \
  "infer-dims=3;48;64\n" \
  "network-type=100\n"

/* Properties set explicitly override the file, also when set to their default value. */
static void
test_override (void)
{
  gchar *cfg = write_file ("override.txt", BASE_CONFIG "batch-size=1\ninterval=3\ngie-unique-id=5\n"
      "output-tensor-meta=1\nfilter-out-class-ids=7\n");
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);
  GstStructure *s;
  GError *err = NULL;
  guint u;
  gboolean b;

  load_hooks ();
  g_object_set (e, "config-file-path", cfg, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_uint (s, "batch-size", &u) && u == 1);
  g_assert_true (gst_structure_get_uint (s, "interval", &u) && u == 3);
  g_assert_true (gst_structure_get_uint (s, "gie-unique-id", &u) && u == 5);
  g_assert_true (gst_structure_get_boolean (s, "output-tensor-meta", &b) && b);
  g_assert_cmpstr (gst_structure_get_string (s, "filter-out-class-ids"), ==, "7");
  gst_structure_free (s);

  g_object_set (e, "batch-size", 4, "interval", 0, "unique-id", 0, "output-tensor-meta", FALSE,
      "filter-out-class-ids", "1;2", "infer-on-class-ids", "1:2:4", "model-engine-file", "/x/y.mxr",
      "gpu-id", 0, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_uint (s, "batch-size", &u) && u == 4);
  g_assert_true (gst_structure_get_uint (s, "interval", &u) && u == 0);
  g_assert_true (gst_structure_get_uint (s, "gie-unique-id", &u) && u == 0);
  g_assert_true (gst_structure_get_boolean (s, "output-tensor-meta", &b) && !b);
  g_assert_cmpstr (gst_structure_get_string (s, "filter-out-class-ids"), ==, "1;2");
  g_assert_cmpstr (gst_structure_get_string (s, "operate-on-class-ids"), ==, "1;2;4");
  g_assert_cmpstr (gst_structure_get_string (s, "model-engine-file"), ==, "/x/y.mxr");
  /* infer-on-class-ids is accepted with a warning (secondary mode only) */
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 1);
  gst_structure_free (s);

  /* an empty list clears the file's */
  g_object_set (e, "filter-out-class-ids", "", NULL);
  s = hook_get_config (e, &err);
  g_assert_cmpstr (gst_structure_get_string (s, "filter-out-class-ids"), ==, "");
  gst_structure_free (s);

  /* malformed lists name the property */
  g_object_set (e, "filter-out-class-ids", "1:2", NULL);
  g_assert_null (hook_get_config (e, &err));
  g_assert_nonnull (strstr (err->message, "element property filter-out-class-ids: 1:2"));
  g_clear_error (&err);
  g_object_set (e, "filter-out-class-ids", "-1", NULL);
  g_assert_null (hook_get_config (e, &err));
  g_clear_error (&err);
  g_object_set (e, "filter-out-class-ids", NULL, "infer-on-class-ids", "1;x", NULL);
  g_assert_null (hook_get_config (e, &err));
  g_assert_nonnull (strstr (err->message, "element property infer-on-class-ids: 1;x"));
  g_clear_error (&err);

  gst_object_unref (e);
  g_free (cfg);
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

/* Config errors fail the state change before any GPU work, naming the property or the file and key. */
static void
test_start_config_errors (void)
{
  gchar *cfg = write_file ("process-mode.txt", BASE_CONFIG), *missing = g_build_filename (WORK_DIR,
      "missing.txt", NULL), *bad = write_file ("bad-mode.txt", BASE_CONFIG "network-mode=1\n"), *texts;
  GstElement *e;

  e = gst_element_factory_make ("rdsinfer", NULL);
  g_object_set (e, "config-file-path", cfg, "process-mode", 2, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "element property process-mode: 2:"));
  g_free (texts);

  e = gst_element_factory_make ("rdsinfer", NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "config-file-path"));
  g_free (texts);

  e = gst_element_factory_make ("rdsinfer", NULL);
  g_object_set (e, "config-file-path", missing, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, missing));
  g_free (texts);

  e = gst_element_factory_make ("rdsinfer", NULL);
  g_object_set (e, "config-file-path", bad, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "bad-mode.txt:network-mode: 1:"));
  g_free (texts);

  e = gst_element_factory_make ("rdsinfer", NULL);
  g_object_set (e, "config-file-path", cfg, "output-instance-mask", TRUE, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "element property output-instance-mask"));
  g_free (texts);

  g_free (cfg);
  g_free (missing);
  g_free (bad);
}

/* ---- gpu: helpers ---- */

static GstAllocator *dev_alloc;

static GstAllocator *
device_allocator (void)
{
  if (!dev_alloc)
    dev_alloc = rds_allocator_new (0, RDS_BUF_MEM_DEFAULT);
  return dev_alloc;
}

/* A device batch of @slots slots of @w x @h @fmt with @filled frames (slot i = source i, random pattern seeded
 * by @seed + i, or a plain value with @seed 0), like rdsstreammux output. */
static GstBuffer *
make_batch (RdsBufSurfaceColorFormat fmt, guint w, guint h, guint slots, guint filled, guint32 seed)
{
  RdsBufSurfaceCreateParams cp = {.width = w,.height = h,.colorFormat = fmt,.layout = RDS_BUF_LAYOUT_PITCH,
    .memType = RDS_BUF_MEM_DEFAULT,
  };
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *bm = rds_create_batch_meta (slots);
  RdsBufSurface *s;

  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (device_allocator (), slots, &cp));
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  for (guint i = 0; i < slots; i++) {
    if (seed)
      fill_random (s, i, seed + i);
    else
      g_assert_cmpint (rds_buf_surface_memset (s, i, -1, (guint8) (40 + 30 * i)), ==, 0);
  }
  for (guint i = 0; i < filled; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

    fm->source_id = i;
    fm->pad_index = i;
    fm->batch_id = i;
    fm->source_frame_width = w;
    fm->source_frame_height = h;
    fm->pipeline_width = w;
    fm->pipeline_height = h;
    rds_add_frame_meta_to_batch (bm, fm);
  }
  s->numFilled = filled;
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  return buf;
}

static const gchar *
fmt_caps_name (RdsBufSurfaceColorFormat fmt)
{
  return fmt == RDS_BUF_COLOR_FORMAT_NV12 ? "NV12" : "RGBA";
}

/* A harness around @e (properties already set; the harness owns it afterwards), with @bus (or none) for its
 * messages, negotiated for @fmt @width x @height. */
static GstHarness *
harness_for (GstElement * e, GstBus * bus, RdsBufSurfaceColorFormat fmt, guint width, guint height)
{
  gchar *caps = g_strdup_printf ("video/x-raw(memory:RDS), format=%s, width=%u, height=%u, framerate=30/1",
      fmt_caps_name (fmt), width, height);
  GstHarness *h;

  /* the harness takes its own ref (transfer none): hand it ours */
  gst_object_ref_sink (e);
  h = gst_harness_new_with_element (e, "sink", "src");
  gst_object_unref (e);

  if (bus)
    gst_element_set_bus (e, bus);
  gst_harness_set_caps_str (h, caps, caps);
  g_free (caps);
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

static GstElement *
new_infer (const gchar * config)
{
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);

  g_object_set (e, "config-file-path", config, NULL);
  return e;
}

static RdsFrameMeta *
nth_frame (GstBuffer * buf, guint n)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);

  g_assert_nonnull (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), >, n);
  return g_list_nth_data (bm->frame_meta_list, n);
}

static RdsInferTensorMeta *
frame_tensor_meta (RdsFrameMeta * fm)
{
  RdsInferTensorMeta *tm = NULL;

  for (GList * l = fm->frame_user_meta_list; l; l = l->next) {
    RdsUserMeta *um = l->data;

    if (um->base_meta.meta_type == RDS_INFER_TENSOR_OUTPUT_META) {
      g_assert_null (tm);       /* one per frame */
      tm = um->user_meta_data;
    }
  }
  return tm;
}

/* ---- gpu: start ---- */

static void
test_start_errors (void)
{
  gchar *no_model = write_file ("no-model.txt", "[property]\nonnx-file=missing.onnx\nnetwork-type=100\n");
  gchar *gray = write_file ("gray.txt", BASE_CONFIG "model-color-format=2\n"), *texts;
  GstElement *e;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  e = new_infer (no_model);
  texts = start_error (e);
  /* the file, the key and the value */
  g_assert_nonnull (strstr (texts, "no-model.txt:onnx-file:"));
  g_assert_nonnull (strstr (texts, "missing.onnx"));
  g_free (texts);

  e = new_infer (gray);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "gray.txt:model-color-format: 2:"));
  g_free (texts);

  e = new_infer (gray);
  g_object_set (e, "gpu-id", 99, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "element property gpu-id: 99"));
  g_free (texts);
  g_free (no_model);
  g_free (gray);
}

/* A buffer that isn't a batch is an error. */
static void
test_no_batch_meta (void)
{
  gchar *cfg = write_file ("plain.txt", BASE_CONFIG);
  GstElement *e;
  GstHarness *h;
  GstBus *bus = gst_bus_new ();
  RdsBufSurfaceCreateParams cp = {.width = 64,.height = 48,.colorFormat = RDS_BUF_COLOR_FORMAT_NV12 };
  GstBuffer *buf;
  gchar *texts;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  e = new_infer (cfg);
  h = harness_for (e, bus, RDS_BUF_COLOR_FORMAT_NV12, 64, 48);
  buf = gst_buffer_new ();
  gst_buffer_append_memory (buf, rds_allocator_alloc_surface (device_allocator (), 1, &cp));
  g_assert_cmpint (gst_harness_push (h, buf), ==, GST_FLOW_ERROR);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "rdsstreammux"));
  g_free (texts);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
}

/* ---- gpu: tensor path ---- */

#define DELTA_NEAREST 0.002
#define TOL 1e-4

/* The bilinear rounding window of infer_preprocess.c for a source of side @len (YUV into RGB). */
static double
bilinear_delta (guint len)
{
  return 2.0 * 255.0 * 3.0 * len * ldexp (1.0, -24) * (1.164 + 2.017) + DELTA_NEAREST;
}

/* Checks image @t (NCHW RGB, net_w x net_h, the model's output = 2 x the input) of the slot @src against the CPU
 * reference: the slot resized into the letterbox rect (dl, dt, dw, dh), then scale * (x - offset). */
static void
check_tensor_image (const HostImage * src, const gfloat * t, guint net_w, guint net_h, guint dl, guint dt, guint dw,
    guint dh, gboolean bilinear, gfloat scale, const gfloat offsets[3])
{
  double delta = bilinear ? bilinear_delta (MAX (src->im.width, src->im.height)) : DELTA_NEAREST, worst = 0;

  for (guint y = 0; y < net_h; y++)
    for (guint x = 0; x < net_w; x++) {
      gboolean inside = x >= dl && x < dl + dw && y >= dt && y < dt + dh;
      double rgb[3], gray;

      if (inside)
        ref_pre_sample (&src->im, x, y, dl, dt, dw, dh, bilinear, rgb, &gray);
      for (guint c = 0; c < 3; c++) {
        float got = t[(gsize) c * net_h * net_w + (gsize) y * net_w + x] / 2.0f;
        gboolean alt;
        double d = inside ? ref_pre_diff (got, rgb[c], delta, scale, offsets[c], &alt)
            : fabs ((double) got - (double) scale * (0.0 - offsets[c]));

        if (d > TOL && worst <= TOL)
          g_test_message ("first mismatch at (%u, %u) channel %u: %f (diff %g)", x, y, c, got, d);
        worst = MAX (worst, d);
      }
    }
  g_assert_cmpfloat (worst, <=, TOL);
}

#define TENSOR_CONFIG \
  "[property]\n" \
  "onnx-file=scale2.onnx\n" \
  "infer-dims=3;48;64\n" \
  "network-type=100\n" \
  "output-tensor-meta=1\n" \
  "maintain-aspect-ratio=1\n" \
  "symmetric-padding=1\n" \
  "net-scale-factor=0.5\n" \
  "offsets=10;20;30\n"

/* scale2 with network-type=100: each frame's tensor meta holds 2 x the preprocessed slot. 96x64 NV12 slots into a
 * 64x48 network, aspect kept and centred: scaled to 64x43 at (0, 2). */
static void
test_tensor_meta (void)
{
  static const gfloat offsets[3] = { 10, 20, 30 };
  gchar *cfg = write_file ("tensor.txt", TENSOR_CONFIG "batch-size=2\n");
  GstElement *e;
  GstHarness *h;
  GstBuffer *in, *out;
  RdsBufSurface *s;
  gsize image = 3 * 48 * 64;
  gfloat *dev_copy = g_new (gfloat, image);
  guint n_sets, n_free;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  e = new_infer (cfg);
  g_object_set (e, "unique-id", 7, NULL);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 96, 64);
  in = make_batch (RDS_BUF_COLOR_FORMAT_NV12, 96, 64, 2, 2, 11);
  s = rds_buffer_get_surface (in);
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  g_assert_true (out == in);    /* passthrough */

  for (guint i = 0; i < 2; i++) {
    RdsFrameMeta *fm = nth_frame (out, i);
    RdsInferTensorMeta *tm = frame_tensor_meta (fm);
    const RdsInferLayerInfo *l;
    HostImage src;

    g_assert_true (fm->bInferDone);
    g_assert_null (fm->obj_meta_list);
    g_assert_nonnull (tm);
    g_assert_cmpuint (tm->unique_id, ==, 7);
    g_assert_cmpuint (tm->num_output_layers, ==, 1);
    g_assert_cmpint (tm->gpu_id, ==, 0);
    g_assert_nonnull (tm->priv_data);
    g_assert_cmpuint (tm->network_info.width, ==, 64);
    g_assert_cmpuint (tm->network_info.height, ==, 48);
    g_assert_cmpuint (tm->network_info.channels, ==, 3);
    g_assert_true (tm->maintain_aspect_ratio);
    g_assert_true (tm->symmetric_padding);
    l = &tm->output_layers_info[0];
    g_assert_cmpint (l->dataType, ==, RDS_INFER_DATA_FLOAT);
    g_assert_cmpstr (l->layerName, ==, "output0");
    g_assert_cmpint (l->bindingIndex, ==, 0);
    g_assert_cmpint (l->isInput, ==, 0);
    g_assert_cmpuint (l->inferDims.numDims, ==, 3);
    g_assert_cmpuint (l->inferDims.d[0], ==, 3);
    g_assert_cmpuint (l->inferDims.d[1], ==, 48);
    g_assert_cmpuint (l->inferDims.d[2], ==, 64);
    g_assert_cmpuint (l->inferDims.numElements, ==, image);
    g_assert_null (l->buffer);  /* documented: not valid in the tensor meta */
    g_assert_null (l->deviceBuffer);

    snapshot (s, i, &src);
    check_tensor_image (&src, tm->out_buf_ptrs_host[0], 64, 48, 0, 2, 64, 43, TRUE, 0.5f, offsets);
    host_image_clear (&src);
    /* the device pointer holds the same values */
    g_assert_cmpint (hipMemcpy (dev_copy, tm->out_buf_ptrs_dev[0], image * sizeof (gfloat),
            hipMemcpyDeviceToHost), ==, hipSuccess);
    g_assert_cmpint (memcmp (dev_copy, tm->out_buf_ptrs_host[0], image * sizeof (gfloat)), ==, 0);
  }
  /* one set, in use by the two metas */
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 1);
  g_assert_cmpuint (n_free, ==, 0);
  gst_buffer_unref (out);
  gst_buffer_unref (in);
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_free, ==, 1);
  gst_harness_teardown (h);
  g_free (dev_copy);
  g_free (cfg);
}

/* ---- gpu: through rdsstreammux and rdsstreamdemux ---- */

typedef struct
{
  GstElement *pipeline, *infer;
  GstAppSrc *src[2];
  GstAppSink *sink[2];
} MuxPipeline;

/* Two 96x64 NV12 system-memory sources -> rdsstreammux (batch-size 4, so batches are half full) -> rdsinfer
 * (batch-size 4 as a property, the file says 2) -> rdsstreamdemux -> one appsink per source. */
static void
mux_pipeline_new (MuxPipeline * mp, const gchar * config)
{
  GError *err = NULL;
  gchar *desc = g_strdup_printf ("rdsstreammux name=m batch-size=4 width=96 height=64 "
      "! rdsinfer name=infer config-file-path=%s batch-size=4 ! rdsstreamdemux name=d "
      "appsrc name=s0 format=time ! m.sink_0  appsrc name=s1 format=time ! m.sink_1 "
      "d.src_0 ! queue ! appsink name=k0 sync=false enable-last-sample=false "
      "d.src_1 ! queue ! appsink name=k1 sync=false enable-last-sample=false", config);
  GstCaps *caps = gst_caps_from_string ("video/x-raw, format=NV12, width=96, height=64, framerate=30/1");

  mp->pipeline = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  mp->infer = gst_bin_get_by_name (GST_BIN (mp->pipeline), "infer");
  for (guint i = 0; i < 2; i++) {
    gchar name[4];

    g_snprintf (name, sizeof (name), "s%u", i);
    mp->src[i] = GST_APP_SRC (gst_bin_get_by_name (GST_BIN (mp->pipeline), name));
    gst_app_src_set_caps (mp->src[i], caps);
    g_snprintf (name, sizeof (name), "k%u", i);
    mp->sink[i] = GST_APP_SINK (gst_bin_get_by_name (GST_BIN (mp->pipeline), name));
  }
  gst_caps_unref (caps);
  g_assert_cmpint (gst_element_set_state (mp->pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
}

static void
mux_pipeline_push (MuxPipeline * mp, guint n)
{
  for (guint i = 0; i < 2; i++) {
    GstBuffer *b = gst_buffer_new_allocate (NULL, 96 * 64 * 3 / 2, NULL);

    gst_buffer_memset (b, 0, (guint8) (60 + 50 * i + n), 96 * 64 * 3 / 2);
    GST_BUFFER_PTS (b) = n * GST_SECOND / 30;
    GST_BUFFER_DURATION (b) = GST_SECOND / 30;
    g_assert_cmpint (gst_app_src_push_buffer (mp->src[i], b), ==, GST_FLOW_OK);
  }
}

static void
mux_pipeline_free (MuxPipeline * mp)
{
  gst_element_set_state (mp->pipeline, GST_STATE_NULL);
  for (guint i = 0; i < 2; i++) {
    gst_object_unref (mp->src[i]);
    gst_object_unref (mp->sink[i]);
  }
  gst_object_unref (mp->infer);
  gst_object_unref (mp->pipeline);
}

/* The next sample of @sink. The first one is also appsink's preroll buffer, which it keeps (holding the batch)
 * until the preroll is pulled too: drop that ref. */
static GstSample *
pull_sample (GstAppSink * sink)
{
  GstSample *sample = gst_app_sink_pull_sample (sink), *preroll;

  g_assert_nonnull (sample);
  if ((preroll = gst_app_sink_try_pull_preroll (sink, 0)))
    gst_sample_unref (preroll);
  return sample;
}

static RdsInferTensorMeta *
sample_tensor_meta (GstSample * sample)
{
  GstBuffer *buf = gst_sample_get_buffer (sample);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);

  g_assert_nonnull (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, 1);
  return frame_tensor_meta (bm->frame_meta_list->data);
}

/* Waits (up to 2 s) until the element has @want free output sets. */
static guint
wait_free_sets (GstElement * infer, guint want)
{
  guint n_sets = 0, n_free = 0;

  for (guint i = 0; i < 200; i++) {
    g_assert_true (hook_get_output_sets (infer, &n_sets, &n_free));
    if (n_free == want)
      break;
    g_usleep (10000);
  }
  return n_free;
}

/* Partial batches from a real mux, and the tensor meta in demuxed copies: the copies keep the output set (and
 * its buffers) alive, the set returns to the free list when the last meta is gone, and a meta may outlive the
 * element. */
static void
test_demux (void)
{
  gchar *cfg = write_file ("demux.txt", TENSOR_CONFIG "batch-size=2\n");
  gsize image = 3 * 48 * 64;
  gfloat *input = g_new (gfloat, 4 * image), *saved = g_new (gfloat, image);
  MuxPipeline mp;
  GstSample *samples[2];
  RdsBatchMeta *copy;
  RdsInferTensorMeta *tm;
  guint n_sets, n_free;
  gfloat centre[2];

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  mux_pipeline_new (&mp, cfg);

  mux_pipeline_push (&mp, 0);
  for (guint i = 0; i < 2; i++) {
    samples[i] = pull_sample (mp.sink[i]);
    tm = sample_tensor_meta (samples[i]);
    g_assert_nonnull (tm);
    centre[i] = ((gfloat *) tm->out_buf_ptrs_host[0])[image / 2];
  }
  /* each view's meta points at its own source's slot (the sources have different values) */
  g_assert_cmpfloat (centre[0], !=, centre[1]);
  /* the two slots without a frame were zeroed */
  g_assert_true (hook_read_input (mp.infer, input, 4 * image));
  for (gsize k = 2 * image; k < 4 * image; k++)
    g_assert_cmpfloat (input[k], ==, 0.0f);
  g_assert_cmpfloat (input[image / 2], !=, 0.0f);

  g_assert_true (hook_get_output_sets (mp.infer, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 1);
  g_assert_cmpuint (n_free, ==, 0);
  /* A copy of a demuxed view's meta holds the set on its own. appsink keeps its last sample until the next one
   * arrives, so the views of a batch are gone only once the next batch has been pulled. */
  copy = rds_batch_meta_copy (gst_buffer_get_rds_batch_meta (gst_sample_get_buffer (samples[0])));
  gst_sample_unref (samples[0]);
  gst_sample_unref (samples[1]);
  mux_pipeline_push (&mp, 1);
  for (guint i = 0; i < 2; i++)
    gst_sample_unref (pull_sample (mp.sink[i]));
  /* batch 0's set (held by the copy only) and batch 1's (held by appsink) */
  g_assert_true (hook_get_output_sets (mp.infer, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 2);
  g_assert_cmpuint (n_free, ==, 0);
  rds_destroy_batch_meta (copy);
  g_assert_cmpuint (wait_free_sets (mp.infer, 1), ==, 1);

  /* the next batch reuses it */
  mux_pipeline_push (&mp, 2);
  for (guint i = 0; i < 2; i++)
    samples[i] = pull_sample (mp.sink[i]);
  g_assert_true (hook_get_output_sets (mp.infer, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 2);

  /* a copy that outlives the pipeline keeps valid buffers (ASan checks the release after the element is gone) */
  copy = rds_batch_meta_copy (gst_buffer_get_rds_batch_meta (gst_sample_get_buffer (samples[1])));
  tm = frame_tensor_meta (copy->frame_meta_list->data);
  memcpy (saved, tm->out_buf_ptrs_host[0], image * sizeof (gfloat));
  gst_sample_unref (samples[0]);
  gst_sample_unref (samples[1]);
  mux_pipeline_free (&mp);
  g_assert_cmpint (memcmp (saved, tm->out_buf_ptrs_host[0], image * sizeof (gfloat)), ==, 0);
  rds_destroy_batch_meta (copy);

  g_free (input);
  g_free (saved);
  g_free (cfg);
}

/* ---- gpu: interval ---- */

static void
push_batches_check_infer (GstHarness * h, const gboolean * want, guint n)
{
  for (guint i = 0; i < n; i++) {
    GstBuffer *out;

    g_assert_cmpint (gst_harness_push (h, make_batch (RDS_BUF_COLOR_FORMAT_NV12, 64, 48, 1, 1, 0)), ==,
        GST_FLOW_OK);
    out = gst_harness_pull (h);
    g_test_message ("batch %u: inferred %d", i, nth_frame (out, 0)->bInferDone);
    g_assert_cmpint (nth_frame (out, 0)->bInferDone, ==, want[i]);
    g_assert_true ((frame_tensor_meta (nth_frame (out, 0)) != NULL) == want[i]);
    gst_buffer_unref (out);
  }
}

/* interval=2: batches 0, 3, 6 are inferred; a flush restarts the count. */
static void
test_interval (void)
{
  static const gboolean before[] = { TRUE, FALSE, FALSE, TRUE, FALSE, FALSE, TRUE, FALSE };
  static const gboolean after[] = { TRUE, FALSE, FALSE, TRUE };
  gchar *cfg = write_file ("interval.txt", TENSOR_CONFIG "batch-size=1\n");
  GstElement *e;
  GstHarness *h;
  GstSegment seg;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  e = new_infer (cfg);
  g_object_set (e, "interval", 2, NULL);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 64, 48);
  push_batches_check_infer (h, before, G_N_ELEMENTS (before));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (h, gst_event_new_segment (&seg)));
  push_batches_check_infer (h, after, G_N_ELEMENTS (after));
  gst_harness_teardown (h);
  g_free (cfg);
}

/* ---- gpu: detector path ---- */

typedef enum
{
  ASPECT_OFF,
  ASPECT_TOP_LEFT,
  ASPECT_SYMMETRIC,
} Aspect;

static gchar *
detector_config (const gchar * name, Aspect aspect, const gchar * func)
{
  gchar *labels = write_file ("labels.txt", "person\ncar\n"), *text, *path;

  text = g_strdup_printf ("[property]\nonnx-file=scale2.onnx\ninfer-dims=3;64;64\nbatch-size=3\nnetwork-type=0\n"
      "num-detected-classes=2\ncustom-lib-path=%s\nparse-bbox-func-name=%s\nlabelfile-path=labels.txt\n"
      "gie-unique-id=3\ncluster-mode=2\nmaintain-aspect-ratio=%d\nsymmetric-padding=%d\n"
      "[class-attrs-all]\npre-cluster-threshold=0.2\nnms-iou-threshold=0.5\n", RDS_TEST_PARSER_FIXTURE, func,
      aspect != ASPECT_OFF, aspect == ASPECT_SYMMETRIC);
  path = write_file (name, text);
  g_free (text);
  g_free (labels);
  return path;
}

typedef struct
{
  gfloat left, top, width, height;
} TestBox;

typedef struct
{
  gint class_id;
  gfloat confidence;
  const gchar *label;
  TestBox box, rect;
} Expected;

static void
check_object (const RdsObjectMeta * om, const Expected * x)
{
  const RdsBbox_Coords *b = &om->detector_bbox_info.org_bbox_coords;
  const RdsOSD_RectParams *r = &om->rect_params;

  g_test_message ("object class %d: box (%g, %g, %g, %g) rect (%g, %g, %g, %g)", om->class_id, b->left, b->top,
      b->width, b->height, r->left, r->top, r->width, r->height);
  g_assert_cmpint (om->class_id, ==, x->class_id);
  g_assert_cmpfloat_with_epsilon (om->confidence, x->confidence, 1e-6);
  g_assert_cmpint (om->unique_component_id, ==, 3);
  g_assert_true (om->object_id == RDS_UNTRACKED_OBJECT_ID);
  g_assert_null (om->parent);
  g_assert_cmpstr (om->obj_label, ==, x->label);
  g_assert_cmpfloat_with_epsilon (b->left, x->box.left, 1e-4);
  g_assert_cmpfloat_with_epsilon (b->top, x->box.top, 1e-4);
  g_assert_cmpfloat_with_epsilon (b->width, x->box.width, 1e-4);
  g_assert_cmpfloat_with_epsilon (b->height, x->box.height, 1e-4);
  g_assert_cmpfloat_with_epsilon (r->left, x->rect.left, 1e-4);
  g_assert_cmpfloat_with_epsilon (r->top, x->rect.top, 1e-4);
  g_assert_cmpfloat_with_epsilon (r->width, x->rect.width, 1e-4);
  g_assert_cmpfloat_with_epsilon (r->height, x->rect.height, 1e-4);
  g_assert_cmpuint (r->border_width, ==, 3);
  g_assert_true (r->border_color.red == 1.0 && r->border_color.green == 0.0 && r->border_color.blue == 0.0
      && r->border_color.alpha == 1.0);
  g_assert_cmpuint (r->has_bg_color, ==, 0);
  g_assert_cmpstr (om->text_params.display_text, ==, x->label);
  g_assert_cmpuint (om->text_params.x_offset, ==, (guint) x->rect.left);
  g_assert_cmpuint (om->text_params.y_offset, ==, x->rect.top > 20 ? (guint) (x->rect.top - 20) : 0);
  g_assert_cmpstr (om->text_params.font_params.font_name, ==, "Serif");
  g_assert_cmpuint (om->text_params.font_params.font_size, ==, 10);
  g_assert_cmpint (om->text_params.set_bg_clr, ==, 1);
}

/* The fixture's boxes (infer-parser-fixture.h) on 128x64 frames into a 64x64 network: box 1 is suppressed by NMS
 * (IoU 0.62 > 0.5), box 3 is below the 0.2 threshold; boxes 0 and 2 are mapped back to frame pixels. */
static void
run_detector (Aspect aspect, const Expected * want, guint n_want, const gchar * filter_out)
{
  gchar *name = g_strdup_printf ("detector-%d.txt", aspect), *cfg = detector_config (name, aspect, "FixtureBoxes");
  GstElement *e;
  GstHarness *h;
  GstBuffer *out;

  e = new_infer (cfg);
  if (filter_out)
    g_object_set (e, "filter-out-class-ids", filter_out, NULL);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_RGBA, 128, 64);
  /* three slots, two frames: the padded slot gets nothing (it has no frame) */
  g_assert_cmpint (gst_harness_push (h, make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, 3, 2, 0)), ==,
      GST_FLOW_OK);
  out = gst_harness_pull (h);
  g_assert_cmpuint (g_list_length (gst_buffer_get_rds_batch_meta (out)->frame_meta_list), ==, 2);
  for (guint f = 0; f < 2; f++) {
    RdsFrameMeta *fm = nth_frame (out, f);
    GList *l = fm->obj_meta_list;

    g_assert_true (fm->bInferDone);
    g_assert_cmpuint (fm->num_obj_meta, ==, n_want);
    g_assert_cmpuint (g_list_length (l), ==, n_want);
    g_assert_null (frame_tensor_meta (fm));
    for (guint i = 0; i < n_want; i++, l = l->next)
      check_object (l->data, &want[i]);
  }
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
  g_free (name);
}

static void
test_detector (void)
{
  /* per axis: x * 128 / 64, y * 64 / 64 */
  static const Expected off[] = {
    {0, 0.9f, "person", {8, 4, 32, 16}, {8, 4, 32, 16}},
    {1, 0.7f, "car", {80, 8, 24, 20}, {80, 8, 24, 20}},
  };
  /* scale 0.5 (the frame is 64x32 in the network), at the top-left */
  static const Expected top_left[] = {
    {0, 0.9f, "person", {8, 8, 32, 32}, {8, 8, 32, 32}},
    {1, 0.7f, "car", {80, 16, 24, 40}, {80, 16, 24, 40}},
  };
  /* centred: 16 rows of padding on top, so boxes start above the frame; the rect is clipped, the detector box
   * isn't */
  static const Expected symmetric[] = {
    {0, 0.9f, "person", {8, -24, 32, 32}, {8, 0, 32, 8}},
    {1, 0.7f, "car", {80, -16, 24, 40}, {80, 0, 24, 24}},
  };

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  run_detector (ASPECT_OFF, off, G_N_ELEMENTS (off), NULL);
  run_detector (ASPECT_TOP_LEFT, top_left, G_N_ELEMENTS (top_left), NULL);
  run_detector (ASPECT_SYMMETRIC, symmetric, G_N_ELEMENTS (symmetric), NULL);
  /* filter-out-class-ids as a property drops the car */
  run_detector (ASPECT_TOP_LEFT, top_left, 1, "1");
}

/* A parser returning false: one warning, frames without objects, and the stream goes on. */
static void
test_parser_failure (void)
{
  gchar *cfg = detector_config ("fail.txt", ASPECT_OFF, "FixtureFail"), *texts;
  GstBus *bus = gst_bus_new ();
  GstElement *e;
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  e = new_infer (cfg);
  h = harness_for (e, bus, RDS_BUF_COLOR_FORMAT_RGBA, 128, 64);
  for (guint i = 0; i < 3; i++) {
    GstBuffer *out;

    g_assert_cmpint (gst_harness_push (h, make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, 3, 2, 0)), ==,
        GST_FLOW_OK);
    out = gst_harness_pull (h);
    for (guint f = 0; f < 2; f++) {
      g_assert_null (nth_frame (out, f)->obj_meta_list);
      g_assert_true (nth_frame (out, f)->bInferDone);
    }
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "FixtureFail"));
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  g_free (texts);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
}

/* ---- gpu: concurrent metadata readers ---- */

#define CONCURRENCY_BATCHES 500

typedef struct
{
  GAsyncQueue *in_flight;       /* GstBuffer refs of batches pushed into rdsinfer; GINT_TO_POINTER (1) ends it */
  /* reader thread; partial: traversals that saw one frame of a batch done and not the other */
  guint traversals, seen_done, partial;
  guint n_missed;               /* batches that passed the probe without the reader seeing them done */
  gint missed_seq;              /* the last of them */
  gint probe_batches;           /* atomic */
  gboolean bad;
} Readers;

/* Walks every frame and object of @bm under the meta lock; FALSE if the counts disagree. Returns the number of
 * frames done in *@done. */
static gboolean
traverse (RdsBatchMeta * bm, guint * done)
{
  gboolean ok = TRUE;

  *done = 0;
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    guint n = 0;

    for (GList * o = fm->obj_meta_list; o; o = o->next) {
      RdsObjectMeta *om = o->data;

      ok = ok && om->class_id >= 0 && om->class_id < 2 && om->rect_params.width > 0;
      n++;
    }
    ok = ok && n == fm->num_obj_meta;
    *done += fm->bInferDone;
  }
  rds_release_meta_lock (bm);
  return ok;
}

/* Keeps walking every batch it was given that rdsinfer hasn't finished, while rdsinfer may be adding to it; a batch
 * is dropped once its frames are done or it has passed the probe. (The first batch never gets done here: appsrc
 * makes it writable to flag it DISCONT, so rdsinfer gets a copy.) */
static gpointer
reader_thread (gpointer data)
{
  Readers *r = data;
  GQueue window = G_QUEUE_INIT;
  gint next_seq = 0, head_seq = 0;
  gboolean ended = FALSE;

  while (!ended || window.length > 0) {
    gpointer item;

    while (!ended && (item = window.length ? g_async_queue_try_pop (r->in_flight)
            : g_async_queue_timeout_pop (r->in_flight, 1000))) {
      if (item == GINT_TO_POINTER (1))
        ended = TRUE;
      else
        g_queue_push_tail (&window, item), next_seq++;
    }
    for (GList * l = window.head; l; l = l->next) {
      guint done = 0;

      if (!traverse (gst_buffer_get_rds_batch_meta (l->data), &done))
        r->bad = TRUE;
      r->traversals++;
      r->partial += done == 1;
    }
    /* retire finished batches from the front */
    while (window.length > 0) {
      GstBuffer *buf = g_queue_peek_head (&window);
      guint done = 0;

      traverse (gst_buffer_get_rds_batch_meta (buf), &done);
      if (done < 2 && g_atomic_int_get (&r->probe_batches) <= head_seq)
        break;
      r->seen_done += done == 2;
      if (done < 2)
        r->n_missed++, r->missed_seq = head_seq;
      gst_buffer_unref (g_queue_pop_head (&window));
      head_seq++;
    }
    /* let rdsinfer take the lock (a mutex isn't fair) */
    g_thread_yield ();
  }
  g_assert_cmpint (head_seq, ==, next_seq);
  return NULL;
}

/* After rdsinfer, on the queue's thread, while rdsinfer works on the next batch. */
static GstPadProbeReturn
after_probe (GstPad * pad G_GNUC_UNUSED, GstPadProbeInfo * info, gpointer data)
{
  Readers *r = data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  guint done = 0;

  if (!traverse (bm, &done) || done != 2)
    r->bad = TRUE;
  for (GList * l = bm->frame_meta_list; l; l = l->next)
    if (((RdsFrameMeta *) l->data)->num_obj_meta != 2)
      r->bad = TRUE;
  g_atomic_int_inc (&r->probe_batches);
  return GST_PAD_PROBE_OK;
}

static void
test_concurrency (void)
{
  gchar *cfg = detector_config ("concurrency.txt", ASPECT_TOP_LEFT, "FixtureBoxes"), *desc;
  GstElement *pipeline;
  GstAppSrc *src;
  GstElement *sink;
  GstPad *pad;
  GstBus *bus;
  GstMessage *msg;
  GError *err = NULL;
  Readers r = { 0 };
  GThread *reader;
  GstCaps *caps;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  desc = g_strdup_printf ("appsrc name=src format=time max-buffers=4 block=true ! rdsinfer config-file-path=%s "
      "! queue max-size-buffers=2 ! fakesink name=sink sync=false", cfg);
  pipeline = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  src = GST_APP_SRC (gst_bin_get_by_name (GST_BIN (pipeline), "src"));
  caps = gst_caps_from_string ("video/x-raw(memory:RDS), format=RGBA, width=128, height=64, framerate=30/1");
  gst_app_src_set_caps (src, caps);
  gst_caps_unref (caps);
  sink = gst_bin_get_by_name (GST_BIN (pipeline), "sink");
  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, after_probe, &r, NULL);
  gst_object_unref (pad);
  r.in_flight = g_async_queue_new ();
  reader = g_thread_new ("meta-reader", reader_thread, &r);
  g_assert_cmpint (gst_element_set_state (pipeline, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);

  for (guint i = 0; i < CONCURRENCY_BATCHES; i++) {
    GstBuffer *b = make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, 3, 2, 0);

    GST_BUFFER_PTS (b) = i * GST_SECOND / 30;
    g_async_queue_push (r.in_flight, gst_buffer_ref (b));
    g_assert_cmpint (gst_app_src_push_buffer (src, b), ==, GST_FLOW_OK);
  }
  g_assert_cmpint (gst_app_src_end_of_stream (src), ==, GST_FLOW_OK);
  bus = gst_element_get_bus (pipeline);
  msg = gst_bus_timed_pop_filtered (bus, 120 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  g_async_queue_push (r.in_flight, GINT_TO_POINTER (1));
  g_thread_join (reader);
  g_test_message ("%d batches, %u reader traversals (%u in the middle of a batch), %u batches seen done by the "
      "reader", r.probe_batches, r.traversals, r.partial, r.seen_done);
  g_assert_false (r.bad);
  g_assert_cmpint (r.probe_batches, ==, CONCURRENCY_BATCHES);
  /* exactly the first batch, which rdsinfer got as a copy */
  g_assert_cmpuint (r.n_missed, ==, 1);
  g_assert_cmpint (r.missed_seq, ==, 0);
  g_assert_cmpuint (r.seen_done, ==, CONCURRENCY_BATCHES - 1);
  /* a batch's results are added under one hold of the lock: never half of them */
  g_assert_cmpuint (r.partial, ==, 0);

  gst_element_set_state (pipeline, GST_STATE_NULL);
  g_async_queue_unref (r.in_flight);
  gst_object_unref (bus);
  gst_object_unref (sink);
  gst_object_unref (src);
  gst_object_unref (pipeline);
  g_free (cfg);
}

int
main (int argc, char **argv)
{
  int ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  /* the comgr kernel cache next to the compiled programs, so re-runs compile nothing */
  g_setenv ("AMD_COMGR_CACHE_DIR", RDS_TEST_BUILD_DIR "/comgr", FALSE);
  g_test_add_func ("/cpu/infer/factory", test_factory);
  g_test_add_func ("/cpu/infer/pad-templates", test_pad_templates);
  g_test_add_func ("/cpu/infer/properties", test_properties);
  g_test_add_func ("/cpu/infer/property-ranges", test_property_ranges);
  g_test_add_func ("/cpu/infer/override", test_override);
  g_test_add_func ("/cpu/infer/start-config-errors", test_start_config_errors);
  g_test_add_func ("/gpu/infer/start-errors", test_start_errors);
  g_test_add_func ("/gpu/infer/no-batch-meta", test_no_batch_meta);
  g_test_add_func ("/gpu/infer/tensor-meta", test_tensor_meta);
  g_test_add_func ("/gpu/infer/demux", test_demux);
  g_test_add_func ("/gpu/infer/interval", test_interval);
  g_test_add_func ("/gpu/infer/detector", test_detector);
  g_test_add_func ("/gpu/infer/parser-failure", test_parser_failure);
  g_test_add_func ("/gpu/infer/concurrency", test_concurrency);
  ret = g_test_run ();
  if (dev_alloc)
    gst_object_unref (dev_alloc);
  gst_deinit ();
  return ret;
}
