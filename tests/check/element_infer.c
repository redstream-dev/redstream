/* rdsinfer: registration, pad templates, properties and how they override the config file (cpu); start errors,
 * the tensor path on the scale2 test model (output = 2 x input) against the CPU preprocessing reference, a restart,
 * partial batches, tensor meta through rdsstreamdemux, interval, the detector path with the fixture parser, and
 * concurrent metadata readers (gpu, skipped without a ROCm device); secondary mode (/gpu/infer-secondary): object
 * crops, chunks, object selection, secondary detectors on fixeddet and their chains, objects replaced while the lock
 * is released, parser failures and the ROI offsets; classifiers on meancls and meancls2 (built-in and custom
 * parsers, thresholds, labels, display text, chains) and their per-object cache (tracked ids, growth, the reinfer
 * interval, stream events, eviction; a detector with the interval).
 * The element's test hooks (gstrdsinfer.h) are looked up in the loaded plugin.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"
#include "infer-preprocess-ref.h"
#include "infer-parser-fixture.h"
#include "rds-infer-config.h"
#include "rds-infer-output.h"
#include "rds-infer-parse.h"

#include <gmodule.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/check/gstharness.h>
#include <gst/video/video.h>
#include <rds/rds.h>
#include <rds/rds-infer-meta.h>
#include <rds/rds-meta-private.h>
#include <string.h>

#define RDS_CAPS "video/x-raw(memory:RDS), format=NV12, width=64, height=48, framerate=30/1"
#define WORK_DIR RDS_TEST_BUILD_DIR "/element-infer"

typedef GstStructure *(*GetConfigFunc) (GstElement * element, GError ** error);
typedef gboolean (*GetOutputSetsFunc) (GstElement * element, guint * n_sets, guint * n_free);
typedef gboolean (*ReadInputFunc) (GstElement * element, gfloat * dst, gsize n_floats);
typedef guint64 (*GetUnitCountFunc) (GstElement * element);
typedef gboolean (*GetOutputSetTotalsFunc) (GstElement * element, guint64 * allocated, guint64 * freed);
typedef void (*AttachClassifierResultFunc) (gpointer batch_meta, gpointer obj_meta, gconstpointer result,
    gint unique_id, const gchar * classifier_type);

static GetConfigFunc hook_get_config;
static GetOutputSetsFunc hook_get_output_sets;
static ReadInputFunc hook_read_input;
static GetUnitCountFunc hook_get_unit_count;
static GetOutputSetTotalsFunc hook_get_output_set_totals;
static AttachClassifierResultFunc hook_attach_classifier_result;

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
  g_assert_true (g_module_symbol (module, "rds_infer_test_get_unit_count", (gpointer *) & hook_get_unit_count));
  g_assert_true (g_module_symbol (module, "rds_infer_test_get_output_set_totals",
          (gpointer *) & hook_get_output_set_totals));
  g_assert_true (g_module_symbol (module, "rds_infer_test_attach_classifier_result",
          (gpointer *) & hook_attach_classifier_result));
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

/* tests/models/@name copied into WORK_DIR, so its compiled programs are cached there (not in the source tree). */
static void
ensure_test_model (const gchar * name)
{
  gchar *contents = NULL, *src = g_build_filename (RDS_TEST_MODELS, name, NULL);
  gchar *dst = g_build_filename (WORK_DIR, name, NULL);
  gsize len = 0;
  GError *err = NULL;

  if (!g_file_test (dst, G_FILE_TEST_EXISTS)) {
    g_assert_true (g_file_get_contents (src, &contents, &len, &err));
    g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
    g_assert_true (g_file_set_contents (dst, contents, (gssize) len, &err));
    g_free (contents);
  }
  g_free (src);
  g_free (dst);
}

static void
ensure_model (void)
{
  ensure_test_model ("scale2.onnx");
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
    {"infer-on-gie-id", G_TYPE_INT},
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
  /* infer-on-gie-id: -1 (every GIE) by default, as DeepStream 8.0's */
  {
    GParamSpecInt *gie = G_PARAM_SPEC_INT (g_object_class_find_property (G_OBJECT_GET_CLASS (e),
            "infer-on-gie-id"));
    gint v = 0;

    g_assert_cmpint (gie->default_value, ==, -1);
    g_assert_cmpint (gie->minimum, ==, -1);
    g_assert_cmpint (gie->maximum, ==, G_MAXINT);
    g_object_get (e, "infer-on-gie-id", &v, NULL);
    g_assert_cmpint (v, ==, -1);
    g_object_set (e, "infer-on-gie-id", 0, NULL);
    g_object_get (e, "infer-on-gie-id", &v, NULL);
    g_assert_cmpint (v, ==, 0);
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
  gint64 min, max, type_max;

  if (G_IS_PARAM_SPEC_UINT (pspec)) {
    min = G_PARAM_SPEC_UINT (pspec)->minimum;
    max = G_PARAM_SPEC_UINT (pspec)->maximum;
    type_max = G_MAXUINT;
  } else {
    g_assert_true (G_IS_PARAM_SPEC_INT (pspec));
    min = G_PARAM_SPEC_INT (pspec)->minimum;
    max = G_PARAM_SPEC_INT (pspec)->maximum;
    type_max = G_MAXINT;
  }
  return max == type_max ? g_strdup_printf (">= %" G_GINT64_FORMAT, min)
      : g_strdup_printf ("%" G_GINT64_FORMAT "..%" G_GINT64_FORMAT, min, max);
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
    {"infer-on-gie-id", "operate-on-gie-id"},
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
  /* infer-on-class-ids in primary mode: a warning (secondary mode only) */
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 1);
  g_assert_cmpstr (gst_structure_get_string (s, "warnings"), ==, "element properties infer-on-gie-id and "
      "infer-on-class-ids: ignored: they apply to secondary mode (process-mode=2) only");
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

/* How often @needle is in @haystack. */
static guint
count_substr (const gchar * haystack, const gchar * needle)
{
  guint n = 0;

  for (const gchar * p = haystack; (p = strstr (p, needle)); p += strlen (needle))
    n++;
  return n;
}

/* The secondary-mode properties: process-mode, infer-on-gie-id and infer-on-class-ids override the file, and the
 * warnings follow the final mode. */
static void
test_override_secondary (void)
{
  gchar *cfg = write_file ("override-secondary.txt", BASE_CONFIG "operate-on-gie-id=4\noperate-on-class-ids=1;2\n"
      "interval=2\n");
  GstElement *e = gst_element_factory_make ("rdsinfer", NULL);
  GstStructure *s;
  GError *err = NULL;
  gint i;
  guint u;

  load_hooks ();
  /* primary mode: the file's secondary keys warn */
  g_object_set (e, "config-file-path", cfg, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_int (s, "process-mode", &i) && i == 1);
  g_assert_true (gst_structure_get_int (s, "operate-on-gie-id", &i) && i == 4);
  g_assert_cmpstr (gst_structure_get_string (s, "operate-on-class-ids"), ==, "1;2");
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 2);
  gst_structure_free (s);

  /* process-mode=2 from the property: those warnings go, the file's interval warns instead */
  g_object_set (e, "process-mode", 2, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_int (s, "process-mode", &i) && i == 2);
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 1);
  g_assert_cmpuint (count_substr (gst_structure_get_string (s, "warnings"), ":interval: 2: ignored in secondary mode"),
      ==, 1);
  /* the effective config: secondary mode runs with interval 0 */
  g_assert_true (gst_structure_get_uint (s, "interval", &u) && u == 0);
  gst_structure_free (s);

  /* the properties win over the file, without warnings in secondary mode; 0 is a real GIE id, -1 every class */
  g_object_set (e, "infer-on-gie-id", 0, "infer-on-class-ids", "-1", "interval", 0, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_int (s, "operate-on-gie-id", &i) && i == 0);
  g_assert_cmpstr (gst_structure_get_string (s, "operate-on-class-ids"), ==, "");
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 0);
  gst_structure_free (s);

  /* infer-on-gie-id=-1 explicitly: every GIE again; class ids from the property */
  g_object_set (e, "infer-on-gie-id", -1, "infer-on-class-ids", "3:5", NULL);
  s = hook_get_config (e, &err);
  g_assert_true (gst_structure_get_int (s, "operate-on-gie-id", &i) && i == -1);
  g_assert_cmpstr (gst_structure_get_string (s, "operate-on-class-ids"), ==, "3;5");
  gst_structure_free (s);

  /* the interval property in secondary mode: a warning naming the property */
  g_object_set (e, "interval", 3, NULL);
  s = hook_get_config (e, &err);
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 1);
  g_assert_cmpstr (gst_structure_get_string (s, "warnings"), ==,
      "element property interval: 3: ignored in secondary mode (process-mode=2)");
  gst_structure_free (s);

  /* primary mode: the properties at their defaults don't warn */
  g_object_set (e, "process-mode", 1, "infer-on-gie-id", -1, "infer-on-class-ids", "", "interval", 0, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 0);
  gst_structure_free (s);
  g_object_set (e, "infer-on-gie-id", 2, NULL);
  s = hook_get_config (e, &err);
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 1);
  g_assert_cmpuint (count_substr (gst_structure_get_string (s, "warnings"), "element properties infer-on-gie-id "
          "and infer-on-class-ids: ignored: "), ==, 1);
  gst_structure_free (s);
  g_object_set (e, "infer-on-gie-id", -1, NULL);

  /* a malformed list names the property; ids are ints */
  g_object_set (e, "infer-on-class-ids", "1:-2", NULL);
  g_assert_null (hook_get_config (e, &err));
  g_assert_nonnull (strstr (err->message, "element property infer-on-class-ids: 1:-2"));
  g_clear_error (&err);
  g_object_set (e, "infer-on-class-ids", "2147483648", NULL);
  g_assert_null (hook_get_config (e, &err));
  g_clear_error (&err);

  gst_object_unref (e);
  g_free (cfg);

  /* a classifier file without process-mode, made secondary by the property: valid (the mode is checked after the
   * overrides) */
  cfg = write_file ("classifier.txt", "[property]\nonnx-file=scale2.onnx\ninfer-dims=3;48;64\nnetwork-type=1\n");
  e = gst_element_factory_make ("rdsinfer", NULL);
  g_object_set (e, "config-file-path", cfg, NULL);
  g_assert_null (hook_get_config (e, &err));
  g_assert_nonnull (strstr (err->message, "classifier.txt:network-type: 1: classifiers in primary mode"));
  g_clear_error (&err);
  g_object_set (e, "process-mode", 2, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_int (s, "process-mode", &i) && i == 2);
  gst_structure_free (s);
  gst_object_unref (e);
  g_free (cfg);

  /* the ROI offsets follow the final mode too: ignored in secondary mode, with a warning (ours) */
  cfg = write_file ("roi-mode.txt", "[property]\nonnx-file=fixeddet.onnx\nnetwork-type=0\nnum-detected-classes=2\n"
      "parse-bbox-func-name=RdsInferParseYolo\n[class-attrs-all]\nroi-top-offset=30\n");
  e = gst_element_factory_make ("rdsinfer", NULL);
  g_object_set (e, "config-file-path", cfg, NULL);
  s = hook_get_config (e, &err);
  g_assert_no_error (err);
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 0);
  gst_structure_free (s);
  g_object_set (e, "process-mode", 2, NULL);
  s = hook_get_config (e, &err);
  g_assert_true (gst_structure_get_uint (s, "n-warnings", &u) && u == 1);
  g_assert_nonnull (strstr (gst_structure_get_string (s, "warnings"), "roi-mode.txt:roi-top-offset: 30 (every "
          "class): ignored in secondary mode (process-mode=2)"));
  gst_structure_free (s);
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
  gchar *classifier = write_file ("start-classifier.txt", "[property]\nonnx-file=scale2.onnx\n"
      "infer-dims=3;48;64\nnetwork-type=1\n");
  GstElement *e;

  /* a classifier in primary mode: refused before any GPU work (classifiers run in secondary mode) */
  e = gst_element_factory_make ("rdsinfer", NULL);
  g_object_set (e, "config-file-path", classifier, NULL);
  texts = start_error (e);
  g_assert_nonnull (strstr (texts, "start-classifier.txt:network-type: 1: classifiers in primary mode"));
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
  g_free (classifier);
}

/* One classifier result attached to two objects, and twice to one of them: each attach makes its own metas from the
 * values (the result is only read), the display text grows each time, classifier_type is stored as given; a result
 * with a description but no labels attaches nothing. ASan checks the ownership. */
static void
test_classifier_attach (void)
{
  RdsBatchMeta *bm = rds_create_batch_meta (1);
  RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);
  RdsObjectMeta *om[2];
  RdsInferClassifierResult *r = rds_infer_classifier_result_new (), *empty = rds_infer_classifier_result_new ();
  const gchar *type = g_intern_string ("attach-test");

  load_hooks ();
  rds_add_frame_meta_to_batch (bm, fm);
  for (guint i = 0; i < 2; i++) {
    om[i] = rds_acquire_obj_meta_from_pool (bm);
    om[i]->text_params.display_text = i ? NULL : g_strdup ("T");
    rds_add_obj_meta_to_frame (fm, om[i], NULL);
  }
  rds_infer_classifier_result_add (r, 0, 1, 3, 0.5f, "b");
  rds_infer_classifier_result_add (r, 1, 0, 2, 0.25f, "x");
  empty->desc = g_strdup ("d");

  hook_attach_classifier_result (bm, om[0], r, 4, type);
  hook_attach_classifier_result (bm, om[1], r, 4, type);
  hook_attach_classifier_result (bm, om[0], r, 5, NULL);
  hook_attach_classifier_result (bm, om[1], empty, 4, type);
  rds_infer_classifier_result_free (r);
  rds_infer_classifier_result_free (empty);

  g_assert_cmpstr (om[0]->text_params.display_text, ==, "T b x  b x ");
  g_assert_cmpstr (om[1]->text_params.display_text, ==, " b x ");
  g_assert_cmpuint (g_list_length (om[0]->classifier_meta_list), ==, 2);
  g_assert_cmpuint (g_list_length (om[1]->classifier_meta_list), ==, 1);
  for (guint i = 0; i < 3; i++) {
    RdsClassifierMeta *c = g_list_nth_data (i < 2 ? om[0]->classifier_meta_list : om[1]->classifier_meta_list,
        i < 2 ? i : 0);
    const RdsLabelInfo *b, *x;

    g_assert_cmpint (c->unique_component_id, ==, i == 1 ? 5 : 4);
    g_assert_true (c->classifier_type == (i == 1 ? NULL : type));
    g_assert_cmpuint (c->num_labels, ==, 2);
    b = c->label_info_list->data;
    x = c->label_info_list->next->data;
    g_assert_cmpuint (b->label_id, ==, 0);
    g_assert_cmpuint (b->result_class_id, ==, 1);
    g_assert_cmpuint (b->num_classes, ==, 3);
    g_assert_cmpfloat (b->result_prob, ==, 0.5f);
    g_assert_cmpstr (b->result_label, ==, "b");
    g_assert_cmpuint (x->label_id, ==, 1);
    g_assert_cmpstr (x->result_label, ==, "x");
    g_assert_null (x->pResult_label);
  }
  /* every attach has its own metas */
  g_assert_true (om[0]->classifier_meta_list->data != om[0]->classifier_meta_list->next->data);
  g_assert_true (om[0]->classifier_meta_list->data != om[1]->classifier_meta_list->data);
  rds_destroy_batch_meta (bm);
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
 * reference: the source rect (sx, sy, sw, sh) of the slot resized into the letterbox rect (dl, dt, dw, dh), then
 * scale * (x - offset). */
static void
check_tensor_rect (const HostImage * src, const gfloat * t, guint net_w, guint net_h, guint sx, guint sy, guint sw,
    guint sh, guint dl, guint dt, guint dw, guint dh, gboolean bilinear, gfloat scale, const gfloat offsets[3])
{
  double delta = bilinear ? bilinear_delta (MAX (src->im.width, src->im.height)) : DELTA_NEAREST, worst = 0;

  for (guint y = 0; y < net_h; y++)
    for (guint x = 0; x < net_w; x++) {
      gboolean inside = x >= dl && x < dl + dw && y >= dt && y < dt + dh;
      double rgb[3], gray;

      if (inside)
        ref_pre_sample_rect (&src->im, x, y, sx, sy, sw, sh, dl, dt, dw, dh, bilinear, rgb, &gray);
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

/* check_tensor_rect with the whole slot as the source rect. */
static void
check_tensor_image (const HostImage * src, const gfloat * t, guint net_w, guint net_h, guint dl, guint dt, guint dw,
    guint dh, gboolean bilinear, gfloat scale, const gfloat offsets[3])
{
  check_tensor_rect (src, t, net_w, net_h, 0, 0, src->im.width, src->im.height, dl, dt, dw, dh, bilinear, scale,
      offsets);
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

/* Pushes one 2-slot 96x64 NV12 batch (pattern @seed) through @h and checks both frames' tensor meta (TENSOR_CONFIG,
 * as test_tensor_meta) and its unique_id. */
static void
push_check_tensor (GstHarness * h, guint32 seed, guint unique_id)
{
  static const gfloat offsets[3] = { 10, 20, 30 };
  GstBuffer *in = make_batch (RDS_BUF_COLOR_FORMAT_NV12, 96, 64, 2, 2, seed), *out;
  RdsBufSurface *s = rds_buffer_get_surface (in);

  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  for (guint i = 0; i < 2; i++) {
    RdsInferTensorMeta *tm = frame_tensor_meta (nth_frame (out, i));
    HostImage src;

    g_assert_nonnull (tm);
    g_assert_cmpuint (tm->unique_id, ==, unique_id);
    snapshot (s, i, &src);
    check_tensor_image (&src, tm->out_buf_ptrs_host[0], 64, 48, 0, 2, 64, 43, TRUE, 0.5f, offsets);
    host_image_clear (&src);
  }
  gst_buffer_unref (out);
  gst_buffer_unref (in);
}

/* The same element PLAYING -> NULL -> PLAYING: NULL releases the config, engine, parser and output sets, the
 * restart builds them again (a property set in between applies) and the results are right. One-time warnings are
 * once per element, not per start: the raw-output one is not posted again. */
static void
test_restart (void)
{
  gchar *cfg = write_file ("restart.txt", TENSOR_CONFIG "batch-size=2\n"), *texts;
  const gchar *caps = "video/x-raw(memory:RDS), format=NV12, width=96, height=64, framerate=30/1";
  GstBus *bus = gst_bus_new ();
  GstElement *e;
  GstHarness *h;
  guint n_sets, n_free;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  e = new_infer (cfg);
  g_object_set (e, "unique-id", 7, "raw-output-file-write", TRUE, NULL);
  /* before the harness starts it, so start's warnings reach the bus */
  gst_element_set_bus (e, bus);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 96, 64);
  pop_messages (bus, GST_MESSAGE_WARNING, &texts);
  g_assert_nonnull (strstr (texts, "raw-output-file-write"));
  g_free (texts);
  for (guint b = 0; b < 2; b++)
    push_check_tensor (h, 11 + b, 7);
  /* one unit per frame */
  g_assert_cmpuint (hook_get_unit_count (e), ==, 4);

  g_assert_cmpint (gst_element_set_state (e, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_false (hook_get_output_sets (e, &n_sets, &n_free));
  g_object_set (e, "unique-id", 8, NULL);
  g_assert_cmpint (gst_element_set_state (e, GST_STATE_PLAYING), ==, GST_STATE_CHANGE_SUCCESS);
  /* the element's pads lost their sticky events */
  g_assert_true (gst_harness_push_event (h, gst_event_new_stream_start ("restart")));
  gst_harness_set_caps_str (h, caps, caps);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 0);
  g_free (texts);
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 0);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 0);
  for (guint b = 0; b < 3; b++)
    push_check_tensor (h, 21 + b, 8);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 6);
  /* a new pool: one set, reused for each batch */
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 1);
  g_assert_cmpuint (n_free, ==, 1);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  harness_teardown_with_bus (h, bus);
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
  /* one unit per frame: two in a batch of four slots */
  g_assert_cmpuint (hook_get_unit_count (mp.infer), ==, 2);

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

/* Also checks that the inferred batches (one frame each) count one unit each, the skipped ones none. */
static void
push_batches_check_infer (GstHarness * h, const gboolean * want, guint n)
{
  guint64 units = hook_get_unit_count (h->element);

  for (guint i = 0; i < n; i++) {
    GstBuffer *out;

    g_assert_cmpint (gst_harness_push (h, make_batch (RDS_BUF_COLOR_FORMAT_NV12, 64, 48, 1, 1, 0)), ==,
        GST_FLOW_OK);
    out = gst_harness_pull (h);
    g_test_message ("batch %u: inferred %d", i, nth_frame (out, 0)->bInferDone);
    g_assert_cmpint (nth_frame (out, 0)->bInferDone, ==, want[i]);
    g_assert_true ((frame_tensor_meta (nth_frame (out, 0)) != NULL) == want[i]);
    units += want[i];
    g_assert_cmpuint (hook_get_unit_count (h->element), ==, units);
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
  load_hooks ();
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

/* An object made by the rdsinfer of @unique_id, with @parent (NULL in primary mode). */
static void
check_object_from (const RdsObjectMeta * om, const Expected * x, gint unique_id, const RdsObjectMeta * parent)
{
  const RdsBbox_Coords *b = &om->detector_bbox_info.org_bbox_coords;
  const RdsOSD_RectParams *r = &om->rect_params;

  g_test_message ("object class %d: box (%g, %g, %g, %g) rect (%g, %g, %g, %g)", om->class_id, b->left, b->top,
      b->width, b->height, r->left, r->top, r->width, r->height);
  g_assert_cmpint (om->class_id, ==, x->class_id);
  g_assert_cmpfloat_with_epsilon (om->confidence, x->confidence, 1e-6);
  g_assert_cmpint (om->unique_component_id, ==, unique_id);
  g_assert_true (om->object_id == RDS_UNTRACKED_OBJECT_ID);
  g_assert_true (om->parent == parent);
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

static void
check_object (const RdsObjectMeta * om, const Expected * x)
{
  check_object_from (om, x, 3, NULL);
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

/* A raw batch pointer remains alive in the blocked push; keeping an extra buffer reference would allow
 * GstBaseTransform to copy it and would test a different batch. */
typedef struct
{
  GstHarness *h;
  GstBuffer *in;
  GstFlowReturn flow;
} ReplacementPush;

static gpointer
replacement_push (gpointer data)
{
  ReplacementPush *p = data;

  p->flow = gst_harness_push (p->h, p->in);
  return NULL;
}

/* Pointer-only attachment must fail this test: the replacement deliberately repeats every identity scalar.
 * A second, untouched frame also proves that ordinary results are still attached. */
static void
run_frame_replacement (gboolean parser_fails)
{
  const gchar *func = parser_fails ? "FixtureBarrierFail" : "FixtureBarrierBoxes";
  gchar *cfg, *texts = NULL;
  GModule *module;
  void (*reset) (void), (*release) (void);
  gboolean (*wait) (void);
  GstBus *bus;
  GstElement *e;
  GstBuffer *out;
  RdsBatchMeta *bm;
  RdsFrameMeta *original, *replacement, saved;
  guint64 generation;
  ReplacementPush push = { 0 };
  GThread *thread;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  module = g_module_open (RDS_TEST_PARSER_FIXTURE, G_MODULE_BIND_LOCAL);
  g_assert_nonnull (module);
  g_assert_true (g_module_symbol (module, "FixtureBarrierReset", (gpointer *) &reset));
  g_assert_true (g_module_symbol (module, "FixtureBarrierWait", (gpointer *) &wait));
  g_assert_true (g_module_symbol (module, "FixtureBarrierRelease", (gpointer *) &release));
  reset ();
  cfg = detector_config (parser_fails ? "replace-fail.txt" : "replace-boxes.txt", ASPECT_OFF, func);
  bus = gst_bus_new ();
  e = new_infer (cfg);
  g_object_set (e, "output-tensor-meta", TRUE, NULL);
  push.h = harness_for (e, bus, RDS_BUF_COLOR_FORMAT_RGBA, 128, 64);
  push.in = make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, 3, 2, 0);
  bm = gst_buffer_get_rds_batch_meta (push.in);
  rds_acquire_meta_lock (bm);
  original = bm->frame_meta_list->data;
  generation = rds_meta_get_generation (original);
  original->source_id = 17;
  original->frame_num = 23;
  original->buf_pts = 42;
  saved = *original;
  rds_release_meta_lock (bm);
  thread = g_thread_new ("replace-push", replacement_push, &push);
  g_assert_true (wait ());
  rds_acquire_meta_lock (bm);
  rds_remove_frame_meta_from_batch (bm, original);
  replacement = rds_acquire_frame_meta_from_pool (bm);
  g_assert_true (replacement == original);
  g_assert_cmpuint (rds_meta_get_generation (replacement), !=, generation);
  *replacement = saved;
  if (parser_fails)
    replacement->source_id = 99;
  rds_add_frame_meta_to_batch (bm, replacement);
  rds_release_meta_lock (bm);
  release ();
  g_thread_join (thread);
  g_assert_cmpint (push.flow, ==, GST_FLOW_OK);
  out = gst_harness_pull (push.h);
  g_assert_true (gst_buffer_get_rds_batch_meta (out) == bm);
  if (parser_fails) {
    g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
    g_assert_nonnull (strstr (texts, "source 17,"));
    g_assert_null (strstr (texts, "source 99,"));
  }
  g_assert_cmpuint (replacement->num_obj_meta, ==, 0);
  g_assert_null (replacement->obj_meta_list);
  g_assert_null (frame_tensor_meta (replacement));
  g_assert_false (replacement->bInferDone);
  /* Removal/re-add appends the replacement after the untouched frame. */
  g_assert_true (nth_frame (out, 0)->bInferDone);
  g_assert_cmpuint (nth_frame (out, 0)->num_obj_meta, ==, parser_fails ? 0 : 2);
  g_assert_nonnull (frame_tensor_meta (nth_frame (out, 0)));
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  gst_buffer_unref (out);
  harness_teardown_with_bus (push.h, bus);
  g_module_close (module);
  g_free (texts);
  g_free (cfg);
}

static void
test_frame_replacement_failure (void)
{
  run_frame_replacement (TRUE);
}

static void
test_frame_replacement (void)
{
  run_frame_replacement (FALSE);
}

/* ---- gpu: secondary mode ---- */

/* Adds an object from GIE @gie of class @class_id with rect_params (@left, @top, @w, @h) to frame @n of @buf. */
static RdsObjectMeta *
add_test_object (GstBuffer * buf, guint n, gint gie, gint class_id, gfloat left, gfloat top, gfloat w, gfloat h)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsFrameMeta *fm = nth_frame (buf, n);
  RdsObjectMeta *om;

  rds_acquire_meta_lock (bm);
  om = rds_acquire_obj_meta_from_pool (bm);
  om->unique_component_id = gie;
  om->class_id = class_id;
  om->object_id = RDS_UNTRACKED_OBJECT_ID;
  om->confidence = 1.0f;
  om->rect_params.left = left;
  om->rect_params.top = top;
  om->rect_params.width = w;
  om->rect_params.height = h;
  rds_add_obj_meta_to_frame (fm, om, NULL);
  rds_release_meta_lock (bm);
  return om;
}

/* The tensor meta in @om's obj_user_meta_list (at most one), or NULL. */
static RdsInferTensorMeta *
obj_tensor_meta (const RdsObjectMeta * om)
{
  RdsInferTensorMeta *tm = NULL;

  for (GList * l = om->obj_user_meta_list; l; l = l->next) {
    RdsUserMeta *um = l->data;

    if (um->base_meta.meta_type == RDS_INFER_TENSOR_OUTPUT_META) {
      g_assert_null (tm);
      tm = um->user_meta_data;
    }
  }
  return tm;
}

/* Every object of @buf, frame by frame, in list order. Free with g_ptr_array_unref. */
static GPtrArray *
all_objects (GstBuffer * buf)
{
  GPtrArray *objs = g_ptr_array_new ();

  for (GList * f = gst_buffer_get_rds_batch_meta (buf)->frame_meta_list; f; f = f->next)
    for (GList * o = ((RdsFrameMeta *) f->data)->obj_meta_list; o; o = o->next)
      g_ptr_array_add (objs, o->data);
  return objs;
}

/* Neither the batch nor a frame of @buf has tensor meta (secondary mode attaches to objects), and bInferDone is still
 * FALSE on every frame but frame @done (-1: none), which had it TRUE: secondary mode leaves it alone. */
static void
check_frames_untouched (GstBuffer * buf, gint done)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  gint n = 0;

  for (GList * l = bm->batch_user_meta_list; l; l = l->next)
    g_assert_cmpint (((RdsUserMeta *) l->data)->base_meta.meta_type, !=, RDS_INFER_TENSOR_OUTPUT_META);
  for (GList * f = bm->frame_meta_list; f; f = f->next, n++) {
    g_assert_null (frame_tensor_meta (f->data));
    g_assert_cmpint (((RdsFrameMeta *) f->data)->bInferDone, ==, n == done);
  }
}

#define SECONDARY_TENSOR_CONFIG TENSOR_CONFIG "process-mode=2\n"

/* An object's rect_params, its crop (rds_infer_crop_rect) and where the crop goes in the 64x48 network (aspect kept,
 * centred: rds_infer_crop_letterbox), worked out by hand. */
typedef struct
{
  gfloat left, top, w, h;
  guint sx, sy, sw, sh;
  guint dl, dt, dw, dh;
} CropCase;

/* Checks @om's tensor meta (SECONDARY_TENSOR_CONFIG, unique-id 7) against its crop @c of slot @slot of @s. */
static void
check_crop_tensor (const RdsObjectMeta * om, RdsBufSurface * s, guint slot, const CropCase * c)
{
  static const gfloat offsets[3] = { 10, 20, 30 };
  RdsInferTensorMeta *tm = obj_tensor_meta (om);
  const RdsInferLayerInfo *l;
  gsize image = 3 * 48 * 64;
  gfloat *dev_copy = g_new (gfloat, image);
  HostImage src;

  g_test_message ("object (%g, %g) %gx%g: crop (%u, %u) %ux%u", c->left, c->top, c->w, c->h, c->sx, c->sy, c->sw,
      c->sh);
  g_assert_nonnull (tm);
  g_assert_cmpuint (tm->unique_id, ==, 7);
  g_assert_cmpuint (tm->num_output_layers, ==, 1);
  g_assert_cmpuint (tm->network_info.width, ==, 64);
  g_assert_cmpuint (tm->network_info.height, ==, 48);
  g_assert_true (tm->maintain_aspect_ratio);
  g_assert_true (tm->symmetric_padding);
  /* the dims of one image, without the batch */
  l = &tm->output_layers_info[0];
  g_assert_cmpuint (l->inferDims.numDims, ==, 3);
  g_assert_cmpuint (l->inferDims.d[0], ==, 3);
  g_assert_cmpuint (l->inferDims.d[1], ==, 48);
  g_assert_cmpuint (l->inferDims.d[2], ==, 64);
  g_assert_cmpuint (l->inferDims.numElements, ==, image);
  snapshot (s, slot, &src);
  check_tensor_rect (&src, tm->out_buf_ptrs_host[0], 64, 48, c->sx, c->sy, c->sw, c->sh, c->dl, c->dt, c->dw, c->dh,
      TRUE, 0.5f, offsets);
  host_image_clear (&src);
  g_assert_cmpint (hipMemcpy (dev_copy, tm->out_buf_ptrs_dev[0], image * sizeof (gfloat), hipMemcpyDeviceToHost), ==,
      hipSuccess);
  g_assert_cmpint (memcmp (dev_copy, tm->out_buf_ptrs_host[0], image * sizeof (gfloat)), ==, 0);
  g_free (dev_copy);
}

/* scale2 in secondary mode: each selected object gets one tensor meta in its obj_user_meta_list holding 2 x its crop
 * through the crop letterbox (CPU reference), its own slice of its chunk's output set; nothing goes on the frames.
 * Two batches of different slot sizes, objects at odd offsets and past the frame edges. batch-size 2: the first
 * batch's three objects run as a chunk of 2 (frame 0) and one of 1 (frame 1). Objects whose crop is empty are
 * skipped. */
static void
test_secondary_tensor_crop (void)
{
  /* 96x64 slots: frame 0, frame 0, frame 1 */
  static const CropCase big[] = {
    {11, 7, 31.6f, 21, 12, 8, 30, 20, 0, 2, 64, 42},
    {85, 41, 20, 30, 86, 42, 10, 22, 21, 0, 20, 48},
    {3, 5, 64, 40, 4, 6, 64, 40, 0, 4, 64, 40},
  };
  /* a 70x50 slot: left of the frame and past its bottom */
  static const CropCase small = { -3, 9, 20, 45, 0, 10, 18, 40, 21, 0, 20, 48 };
  gchar *cfg = write_file ("secondary-tensor.txt", SECONDARY_TENSOR_CONFIG "batch-size=2\n");
  const gchar *caps = "video/x-raw(memory:RDS), format=NV12, width=70, height=50, framerate=30/1";
  GstElement *e;
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om[3], *skipped[2];
  RdsInferTensorMeta *tm[3];
  guint n_sets, n_free;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  e = new_infer (cfg);
  g_object_set (e, "unique-id", 7, NULL);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 96, 64);
  in = make_batch (RDS_BUF_COLOR_FORMAT_NV12, 96, 64, 2, 2, 11);
  om[0] = add_test_object (in, 0, 1, 0, big[0].left, big[0].top, big[0].w, big[0].h);
  /* under a pixel wide, and wholly right of the frame: no crop */
  skipped[0] = add_test_object (in, 0, 1, 0, 30, 30, 0.5f, 10);
  om[1] = add_test_object (in, 0, 1, 0, big[1].left, big[1].top, big[1].w, big[1].h);
  skipped[1] = add_test_object (in, 0, 1, 0, 200, 10, 10, 10);
  om[2] = add_test_object (in, 1, 1, 0, big[2].left, big[2].top, big[2].w, big[2].h);
  /* a frame an earlier element marked inferred is inferred on all the same, and keeps its flag */
  nth_frame (in, 1)->bInferDone = TRUE;
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  g_assert_true (out == in);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 3);
  check_frames_untouched (out, 1);
  for (guint i = 0; i < 2; i++)
    g_assert_null (obj_tensor_meta (skipped[i]));
  for (guint i = 0; i < 3; i++) {
    check_crop_tensor (om[i], rds_buffer_get_surface (in), i < 2 ? 0 : 1, &big[i]);
    tm[i] = obj_tensor_meta (om[i]);
  }
  /* images 0 and 1 of the first chunk's set, image 0 of the second's */
  g_assert_true (tm[0]->priv_data == tm[1]->priv_data);
  g_assert_true (tm[2]->priv_data != tm[0]->priv_data);
  g_assert_true ((gfloat *) tm[1]->out_buf_ptrs_host[0] == (gfloat *) tm[0]->out_buf_ptrs_host[0] + 3 * 48 * 64);
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 2);
  g_assert_cmpuint (n_free, ==, 0);
  gst_buffer_unref (out);
  gst_buffer_unref (in);

  gst_harness_set_caps_str (h, caps, caps);
  in = make_batch (RDS_BUF_COLOR_FORMAT_NV12, 70, 50, 1, 1, 21);
  om[0] = add_test_object (in, 0, 1, 0, small.left, small.top, small.w, small.h);
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  check_frames_untouched (out, -1);
  check_crop_tensor (om[0], rds_buffer_get_surface (in), 0, &small);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 4);
  gst_buffer_unref (out);
  gst_buffer_unref (in);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* @objs ran in chunks of @sizes, in this order: the objects of a chunk share its output set and are consecutive
 * images of it in list order; consecutive chunks have distinct sets. */
static void
check_chunks (GPtrArray * objs, const guint * sizes, guint n_chunks, gsize image)
{
  gpointer prev_set = NULL;
  guint total = 0;

  for (guint c = 0; c < n_chunks; c++)
    total += sizes[c];
  g_assert_cmpuint (objs->len, ==, total);
  for (guint c = 0, o = 0; c < n_chunks; o += sizes[c], c++) {
    RdsInferTensorMeta *head = obj_tensor_meta (g_ptr_array_index (objs, o));

    g_assert_nonnull (head);
    g_assert_true (head->priv_data != prev_set);
    for (guint i = 1; i < sizes[c]; i++) {
      RdsInferTensorMeta *tm = obj_tensor_meta (g_ptr_array_index (objs, o + i));

      g_assert_true (tm->priv_data == head->priv_data);
      g_assert_true ((gfloat *) tm->out_buf_ptrs_host[0] == (gfloat *) head->out_buf_ptrs_host[0] + i * image);
    }
    prev_set = head->priv_data;
  }
}

/* A 64x48 NV12 batch of @frames frames (one slot each) with @per_frame[f] 16x16 objects in frame f (all inside the
 * frame, so each one is a unit). */
static GstBuffer *
batch_with_objects (guint frames, const guint * per_frame)
{
  GstBuffer *buf = make_batch (RDS_BUF_COLOR_FORMAT_NV12, 64, 48, frames, frames, 0);

  for (guint f = 0; f < frames; f++)
    for (guint i = 0; i < per_frame[f]; i++)
      add_test_object (buf, f, 1, 0, 2 + 4 * (i % 10), 2 + 2 * (i % 10), 16, 16);
  return buf;
}

/* Units are packed in frame list order, then object list order, in chunks of batch-size, continuing across the
 * frames of a buffer but never across buffers (DeepStream 8.0 behaviour): 10 objects (6 in frame 0, 4 in frame 1) at
 * batch-size 4 run as 4, 4, 2; two buffers of 5 as 4, 1 and 4, 1. interval is ignored in secondary mode: every buffer
 * is inferred. */
static void
test_secondary_chunks (void)
{
  static const guint ten[] = { 6, 4 }, five[] = { 5 }, c442[] = { 4, 4, 2 }, c41[] = { 4, 1 };
  gchar *cfg = write_file ("secondary-chunks.txt", "[property]\nonnx-file=scale2.onnx\ninfer-dims=3;16;16\n"
      "network-type=100\nprocess-mode=2\nbatch-size=4\n");
  const gsize image = 3 * 16 * 16;
  GstElement *e;
  GstHarness *h;
  GstBuffer *out[2];
  GPtrArray *objs[2];
  guint n_sets, n_free;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  e = new_infer (cfg);
  g_object_set (e, "interval", 1, NULL);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 64, 48);

  g_assert_cmpint (gst_harness_push (h, batch_with_objects (2, ten)), ==, GST_FLOW_OK);
  out[0] = gst_harness_pull (h);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 10);
  objs[0] = all_objects (out[0]);
  g_assert_cmpuint (objs[0]->len, ==, 10);
  check_chunks (objs[0], c442, G_N_ELEMENTS (c442), image);
  /* one output set per chunk, all held by the tensor metas */
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 3);
  g_assert_cmpuint (n_free, ==, 0);
  g_ptr_array_unref (objs[0]);
  gst_buffer_unref (out[0]);
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_free, ==, 3);

  for (guint b = 0; b < 2; b++) {
    g_assert_cmpint (gst_harness_push (h, batch_with_objects (1, five)), ==, GST_FLOW_OK);
    out[b] = gst_harness_pull (h);
    g_assert_cmpuint (hook_get_unit_count (e), ==, 15 + 5 * b);
  }
  for (guint b = 0; b < 2; b++) {
    objs[b] = all_objects (out[b]);
    check_chunks (objs[b], c41, G_N_ELEMENTS (c41), image);
  }
  /* the buffers' chunks have their own sets */
  g_assert_true (obj_tensor_meta (objs[0]->pdata[4])->priv_data != obj_tensor_meta (objs[1]->pdata[0])->priv_data);
  for (guint b = 0; b < 2; b++) {
    g_ptr_array_unref (objs[b]);
    gst_buffer_unref (out[b]);
  }
  gst_harness_teardown (h);
  g_free (cfg);
}

/* Objects in a 128x64 frame for the selection test: GIE, class, size. */
static const struct
{
  gint gie, class_id;
  gfloat w, h;
} sel_objects[] = {
  {1, 0, 20, 20},
  {2, 0, 20, 20},
  {1, 1, 20, 20},
  {1, 2, 8, 20},                /* narrower than input-object-min-width */
  {1, 2, 10, 40},               /* at the limits: they are inclusive */
  {1, 0, 20, 41},               /* taller than input-object-max-height */
  {0, 0, 20, 20},               /* GIE 0 is a real id */
};

/* Runs the selection config @cfg with @gie and @classes as the infer-on-gie-id and infer-on-class-ids properties
 * (unset if -2 or NULL); @want lists, per object of sel_objects, whether it gets tensor meta. */
static void
run_selection (const gchar * cfg, gint gie, const gchar * classes, const gboolean * want)
{
  GstElement *e = new_infer (cfg);
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om[G_N_ELEMENTS (sel_objects)];
  guint n = 0;

  if (gie != -2)
    g_object_set (e, "infer-on-gie-id", gie, NULL);
  if (classes)
    g_object_set (e, "infer-on-class-ids", classes, NULL);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 128, 64);
  in = make_batch (RDS_BUF_COLOR_FORMAT_NV12, 128, 64, 1, 1, 0);
  for (guint i = 0; i < G_N_ELEMENTS (sel_objects); i++)
    om[i] = add_test_object (in, 0, sel_objects[i].gie, sel_objects[i].class_id, 2 + 16 * i, 2, sel_objects[i].w,
        sel_objects[i].h);
  g_assert_cmpint (gst_harness_push (h, in), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  for (guint i = 0; i < G_N_ELEMENTS (sel_objects); i++) {
    g_test_message ("object %u: tensor meta %d, want %d", i, obj_tensor_meta (om[i]) != NULL, want[i]);
    g_assert_true ((obj_tensor_meta (om[i]) != NULL) == want[i]);
    n += want[i];
  }
  g_assert_cmpuint (hook_get_unit_count (e), ==, n);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
}

/* Which objects are inferred on, end to end: the producer GIE, the class list and the size limits from the file,
 * then the element properties over them. */
static void
test_secondary_selection (void)
{
  static const gboolean from_file[] = { TRUE, FALSE, FALSE, FALSE, TRUE, FALSE, FALSE };
  static const gboolean gie2[] = { FALSE, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE };
  static const gboolean every[] = { TRUE, TRUE, TRUE, FALSE, TRUE, FALSE, TRUE };
  static const gboolean gie0[] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, TRUE };
  gchar *cfg = write_file ("secondary-select.txt", "[property]\nonnx-file=scale2.onnx\ninfer-dims=3;16;16\n"
      "network-type=100\nprocess-mode=2\nbatch-size=4\noperate-on-gie-id=1\noperate-on-class-ids=0;2\n"
      "input-object-min-width=10\ninput-object-max-height=40\n");

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  run_selection (cfg, -2, NULL, from_file);
  run_selection (cfg, 2, "0", gie2);
  run_selection (cfg, -1, "-1", every);
  run_selection (cfg, 0, "-1", gie0);
  g_free (cfg);
}

/* A secondary detector on fixeddet (README.md: six boxes in network pixels), GIE 5, on the objects of GIE 1;
 * @extra goes at the end of the file. */
static gchar *
fixeddet_config (const gchar * name, const gchar * extra)
{
  gchar *labels = write_file ("labels.txt", "person\ncar\n"), *text, *path;

  text = g_strdup_printf ("[property]\nonnx-file=fixeddet.onnx\nbatch-size=2\nnetwork-type=0\nprocess-mode=2\n"
      "num-detected-classes=2\nparse-bbox-func-name=RdsInferParseYolo\nlabelfile-path=labels.txt\n"
      "gie-unique-id=5\noperate-on-gie-id=1\n%s", extra ? extra : "");
  path = write_file (name, text);
  g_free (text);
  g_free (labels);
  return path;
}

/* fixeddet's children of the two parents of run_children, in frame pixels. Parent 0: rect (100, 40) 64x64, the crop
 * is the same and the network input 1:1, so a child is its network box moved by (100, 40); boxes reaching outside
 * the crop keep that part (inside the frame). */
static const Expected children_p0[] = {
  {0, 0.9f, "person", {108, 48, 16, 16}, {108, 48, 16, 16}},
  {1, 0.8f, "car", {138, 50, 20, 12}, {138, 50, 20, 12}},
  {0, 0.85f, "person", {152, 92, 16, 16}, {152, 92, 16, 16}},
  {0, 0.75f, "person", {96, 78, 16, 20}, {96, 78, 16, 20}},
  {1, 0.7f, "car", {152, 66, 12, 8}, {152, 66, 12, 8}},
  {1, 0.6f, "car", {132, 76, 24, 16}, {132, 76, 24, 16}},
};

/* Parent 1: rect (223.4, 95.2) 40x40 at the bottom-right corner of the 256x128 frame: crop (224, 96) 32x32 (origin up
 * to even, clipped to the frame), stretched to the network: half the network box, moved by (224, 96), the rect
 * clipped to the frame. */
static const Expected children_p1[] = {
  {0, 0.9f, "person", {228, 100, 8, 8}, {228, 100, 8, 8}},
  {1, 0.8f, "car", {243, 101, 10, 6}, {243, 101, 10, 6}},
  {0, 0.85f, "person", {250, 122, 8, 8}, {250, 122, 6, 6}},
  {0, 0.75f, "person", {222, 115, 8, 10}, {222, 115, 8, 10}},
  {1, 0.7f, "car", {250, 109, 6, 4}, {250, 109, 6, 4}},
  {1, 0.6f, "car", {240, 114, 12, 8}, {240, 114, 12, 8}},
};

/* Pushes a 256x128 RGBA batch with two GIE 1 parents (and a GIE 2 object, not inferred on) through @h; returns the
 * output and the parents in @parents[0..2]. */
static GstBuffer *
run_children (GstHarness * h, RdsObjectMeta * parents[3])
{
  GstBuffer *in = make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 256, 128, 1, 1, 0);

  parents[0] = add_test_object (in, 0, 1, 0, 100, 40, 64, 64);
  parents[1] = add_test_object (in, 0, 1, 0, 223.4f, 95.2f, 40, 40);
  parents[2] = add_test_object (in, 0, 2, 0, 10, 10, 40, 40);
  g_assert_cmpint (gst_harness_push (h, in), ==, GST_FLOW_OK);
  return gst_harness_pull (h);
}

/* Checks @out's objects: the three parents, then parent 0's children and parent 1's, appended in the parser's order
 * (ours: 8.0 prepends). */
static void
check_children (GstBuffer * out, RdsObjectMeta * parents[3])
{
  GPtrArray *objs = all_objects (out);

  g_assert_cmpuint (objs->len, ==, 3 + 12);
  g_assert_cmpuint (nth_frame (out, 0)->num_obj_meta, ==, 15);
  for (guint i = 0; i < 3; i++)
    g_assert_true (objs->pdata[i] == parents[i]);
  for (guint i = 0; i < 6; i++) {
    check_object_from (objs->pdata[3 + i], &children_p0[i], 5, parents[0]);
    check_object_from (objs->pdata[9 + i], &children_p1[i], 5, parents[1]);
  }
  check_frames_untouched (out, -1);
  g_ptr_array_unref (objs);
}

/* A secondary detector: each parent's children in frame coordinates, with the parent set and the labels of the label
 * file; no tensor meta without output-tensor-meta. A second rdsinfer with infer-on-gie-id = the detector's id
 * infers the children (and only them). */
static void
test_secondary_detector_children (void)
{
  gchar *cfg = fixeddet_config ("secondary-det.txt", NULL);
  gchar *chain = write_file ("secondary-chain.txt", "[property]\nonnx-file=scale2.onnx\ninfer-dims=3;16;16\n"
      "network-type=100\nprocess-mode=2\nbatch-size=4\n");
  GstElement *e, *e2;
  GstHarness *h, *h2;
  GstBuffer *out;
  RdsObjectMeta *parents[3];
  GPtrArray *objs;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  ensure_test_model ("fixeddet.onnx");
  load_hooks ();
  e = new_infer (cfg);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_RGBA, 256, 128);
  e2 = new_infer (chain);
  g_object_set (e2, "infer-on-gie-id", 5, "unique-id", 9, NULL);
  h2 = harness_for (e2, NULL, RDS_BUF_COLOR_FORMAT_RGBA, 256, 128);

  out = run_children (h, parents);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 2);
  check_children (out, parents);
  objs = all_objects (out);
  for (guint i = 0; i < objs->len; i++)
    g_assert_null (obj_tensor_meta (objs->pdata[i]));

  g_assert_cmpint (gst_harness_push (h2, out), ==, GST_FLOW_OK);
  out = gst_harness_pull (h2);
  g_assert_cmpuint (hook_get_unit_count (e2), ==, 12);
  for (guint i = 0; i < objs->len; i++) {
    RdsInferTensorMeta *tm = obj_tensor_meta (objs->pdata[i]);

    if (i < 3) {
      g_assert_null (tm);
    } else {
      g_assert_nonnull (tm);
      g_assert_cmpuint (tm->unique_id, ==, 9);
    }
  }
  g_ptr_array_unref (objs);
  gst_buffer_unref (out);
  gst_harness_teardown (h2);
  gst_harness_teardown (h);
  g_free (chain);
  g_free (cfg);
}

/* roi-top-offset and roi-bottom-offset are not applied in secondary mode (ours): a band that would drop every child
 * (centre y in [30, 34) of the crop) drops none, and start warns about each key. */
static void
test_secondary_roi_ignored (void)
{
  gchar *cfg = fixeddet_config ("secondary-roi.txt", "[class-attrs-all]\nroi-top-offset=30\nroi-bottom-offset=30\n");
  GstBus *bus = gst_bus_new ();
  GstElement *e;
  GstHarness *h;
  GstBuffer *out;
  RdsObjectMeta *parents[3];
  gchar *texts;

  RDS_TEST_REQUIRE_GPU ();
  ensure_test_model ("fixeddet.onnx");
  load_hooks ();
  e = new_infer (cfg);
  gst_element_set_bus (e, bus);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_RGBA, 256, 128);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 2);
  g_assert_nonnull (strstr (texts, "secondary-roi.txt:roi-top-offset: 30 (every class): ignored in secondary mode"));
  g_assert_nonnull (strstr (texts, "secondary-roi.txt:roi-bottom-offset: 30 (every class): ignored in secondary "
          "mode"));
  g_free (texts);
  out = run_children (h, parents);
  check_children (out, parents);
  gst_buffer_unref (out);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
}

/* The fixture parser @func on scale2 (64x64 network, stretched) as a secondary detector of the objects of GIE 1, GIE
 * 3; FixtureBoxes gives two children per crop (box 1 is suppressed by NMS, box 3 is below the threshold). */
static gchar *
fixture_secondary_config (const gchar * name, const gchar * func, gboolean tensor_meta)
{
  gchar *labels = write_file ("labels.txt", "person\ncar\n"), *text, *path;

  text = g_strdup_printf ("[property]\nonnx-file=scale2.onnx\ninfer-dims=3;64;64\nbatch-size=4\nnetwork-type=0\n"
      "process-mode=2\noperate-on-gie-id=1\nnum-detected-classes=2\ncustom-lib-path=%s\nparse-bbox-func-name=%s\n"
      "labelfile-path=labels.txt\ngie-unique-id=3\noutput-tensor-meta=%d\n"
      "[class-attrs-all]\npre-cluster-threshold=0.2\nnms-iou-threshold=0.5\n", RDS_TEST_PARSER_FIXTURE, func,
      tensor_meta);
  path = write_file (name, text);
  g_free (text);
  g_free (labels);
  return path;
}

/* The children of @parent in @fm. */
static guint
count_children (const RdsFrameMeta * fm, const RdsObjectMeta * parent)
{
  guint n = 0;

  for (GList * o = fm->obj_meta_list; o; o = o->next)
    n += ((RdsObjectMeta *) o->data)->parent == parent;
  return n;
}

/* An object removed, or replaced by a new one at the same address, between the snapshot and the attachment gets
 * nothing: no children, no tensor meta. The other object is unaffected. The replacement repeats every identity
 * scalar, so a pointer-only check would attach to it. */
static void
run_object_replacement (gboolean replace)
{
  gchar *cfg;
  GModule *module;
  void (*reset) (void), (*release) (void);
  gboolean (*wait) (void);
  GstBus *bus;
  GstElement *e;
  GstBuffer *out;
  RdsBatchMeta *bm;
  RdsFrameMeta *fm;
  RdsObjectMeta *original, *other, *replacement = NULL;
  ReplacementPush push = { 0 };
  GThread *thread;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  module = g_module_open (RDS_TEST_PARSER_FIXTURE, G_MODULE_BIND_LOCAL);
  g_assert_nonnull (module);
  g_assert_true (g_module_symbol (module, "FixtureBarrierReset", (gpointer *) & reset));
  g_assert_true (g_module_symbol (module, "FixtureBarrierWait", (gpointer *) & wait));
  g_assert_true (g_module_symbol (module, "FixtureBarrierRelease", (gpointer *) & release));
  reset ();
  cfg = fixture_secondary_config ("secondary-replace.txt", "FixtureBarrierBoxes", TRUE);
  bus = gst_bus_new ();
  e = new_infer (cfg);
  push.h = harness_for (e, bus, RDS_BUF_COLOR_FORMAT_RGBA, 128, 64);
  push.in = make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, 1, 1, 0);
  original = add_test_object (push.in, 0, 1, 0, 0, 0, 64, 64);
  other = add_test_object (push.in, 0, 1, 1, 64, 0, 64, 64);
  bm = gst_buffer_get_rds_batch_meta (push.in);
  fm = nth_frame (push.in, 0);
  thread = g_thread_new ("replace-push", replacement_push, &push);
  /* the parser of the first unit: after the snapshot and the GPU work, before the attachment */
  g_assert_true (wait ());
  rds_acquire_meta_lock (bm);
  rds_remove_obj_meta_from_frame (fm, original);
  if (replace) {
    replacement = rds_acquire_obj_meta_from_pool (bm);
    g_assert_true (replacement == original);
    replacement->unique_component_id = 1;
    replacement->class_id = 0;
    replacement->object_id = RDS_UNTRACKED_OBJECT_ID;
    replacement->rect_params.width = 64;
    replacement->rect_params.height = 64;
    rds_add_obj_meta_to_frame (fm, replacement, NULL);
  }
  rds_release_meta_lock (bm);
  release ();
  g_thread_join (thread);
  g_assert_cmpint (push.flow, ==, GST_FLOW_OK);
  out = gst_harness_pull (push.h);
  g_assert_true (gst_buffer_get_rds_batch_meta (out) == bm);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 2);
  /* the other object, its two children, and the replacement */
  g_assert_cmpuint (fm->num_obj_meta, ==, replace ? 4 : 3);
  g_assert_cmpuint (count_children (fm, other), ==, 2);
  g_assert_nonnull (obj_tensor_meta (other));
  if (replace) {
    g_assert_cmpuint (count_children (fm, replacement), ==, 0);
    g_assert_null (obj_tensor_meta (replacement));
  }
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  gst_buffer_unref (out);
  harness_teardown_with_bus (push.h, bus);
  g_module_close (module);
  g_free (cfg);
}

static void
test_secondary_object_removed (void)
{
  run_object_replacement (FALSE);
}

static void
test_secondary_object_replaced (void)
{
  run_object_replacement (TRUE);
}

/* Units of frames A, B and C (one chunk), and B removed between the snapshot and the attachment, then a new frame
 * acquired at B's address with a new object at the address of B's object, repeating their identity scalars: A's
 * and C's objects get their results, the units of B nothing, also through the per-frame resolve that attachment
 * keeps from one unit to the next. */
static void
test_secondary_frame_replaced (void)
{
  gchar *cfg;
  GModule *module;
  void (*reset) (void), (*release) (void);
  gboolean (*wait) (void);
  GstBus *bus;
  GstElement *e;
  GstBuffer *out;
  RdsBatchMeta *bm;
  RdsFrameMeta *fb, *fb2;
  RdsObjectMeta *om[5], *ob2;
  ReplacementPush push = { 0 };
  GThread *thread;
  static const guint frame_of[] = { 0, 0, 1, 2, 2 };
  guint source_id, batch_id;
  gint frame_num;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  module = g_module_open (RDS_TEST_PARSER_FIXTURE, G_MODULE_BIND_LOCAL);
  g_assert_nonnull (module);
  g_assert_true (g_module_symbol (module, "FixtureBarrierReset", (gpointer *) & reset));
  g_assert_true (g_module_symbol (module, "FixtureBarrierWait", (gpointer *) & wait));
  g_assert_true (g_module_symbol (module, "FixtureBarrierRelease", (gpointer *) & release));
  reset ();
  cfg = fixture_secondary_config ("secondary-frame-replace.txt", "FixtureBarrierBoxes", TRUE);
  bus = gst_bus_new ();
  e = new_infer (cfg);
  g_object_set (e, "batch-size", 8, NULL);
  push.h = harness_for (e, bus, RDS_BUF_COLOR_FORMAT_RGBA, 128, 64);
  push.in = make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, 3, 3, 0);
  /* A: om[0], om[1]; B: om[2]; C: om[3], om[4] */
  for (guint i = 0; i < 5; i++)
    om[i] = add_test_object (push.in, frame_of[i], 1, 0, 64 * (i % 2), 0, 64, 64);
  bm = gst_buffer_get_rds_batch_meta (push.in);
  fb = nth_frame (push.in, 1);
  source_id = fb->source_id;
  batch_id = fb->batch_id;
  frame_num = fb->frame_num;
  thread = g_thread_new ("replace-push", replacement_push, &push);
  g_assert_true (wait ());
  rds_acquire_meta_lock (bm);
  rds_remove_frame_meta_from_batch (bm, fb);
  fb2 = rds_acquire_frame_meta_from_pool (bm);
  g_assert_true (fb2 == fb);
  fb2->source_id = source_id;
  fb2->pad_index = source_id;
  fb2->batch_id = batch_id;
  fb2->frame_num = frame_num;
  fb2->source_frame_width = fb2->pipeline_width = 128;
  fb2->source_frame_height = fb2->pipeline_height = 64;
  rds_add_frame_meta_to_batch (bm, fb2);
  ob2 = rds_acquire_obj_meta_from_pool (bm);
  g_assert_true (ob2 == om[2]);
  ob2->unique_component_id = 1;
  ob2->object_id = RDS_UNTRACKED_OBJECT_ID;
  ob2->rect_params.width = 64;
  ob2->rect_params.height = 64;
  rds_add_obj_meta_to_frame (fb2, ob2, NULL);
  rds_release_meta_lock (bm);
  release ();
  g_thread_join (thread);
  g_assert_cmpint (push.flow, ==, GST_FLOW_OK);
  out = gst_harness_pull (push.h);
  g_assert_cmpuint (hook_get_unit_count (e), ==, 5);
  /* the frame list is now A, C, B' */
  for (guint i = 0; i < 5; i++) {
    if (i == 2)
      continue;
    g_assert_cmpuint (count_children (nth_frame (out, frame_of[i] ? 1 : 0), om[i]), ==, 2);
    g_assert_nonnull (obj_tensor_meta (om[i]));
  }
  g_assert_true (nth_frame (out, 2) == fb2);
  g_assert_cmpuint (fb2->num_obj_meta, ==, 1);
  g_assert_null (obj_tensor_meta (ob2));
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  gst_buffer_unref (out);
  harness_teardown_with_bus (push.h, bus);
  g_module_close (module);
  g_free (cfg);
}

/* A parser that returns false on some crops (FixtureFailDark: the dark frame's): one warning, those objects get no
 * children, the others (in the same chunk) theirs, and the stream goes on (ours: DeepStream 8.0 crashes). With
 * tensor meta, the failed objects still get theirs, as frames do in primary mode. */
static void
run_parser_false (gboolean tensor_meta)
{
  gchar *name = g_strdup_printf ("secondary-fail-%d.txt", tensor_meta), *texts;
  gchar *cfg = fixture_secondary_config (name, "FixtureFailDark", tensor_meta);
  GstBus *bus = gst_bus_new ();
  GstElement *e;
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  e = new_infer (cfg);
  h = harness_for (e, bus, RDS_BUF_COLOR_FORMAT_RGBA, 128, 64);
  for (guint b = 0; b < 2; b++) {
    GstBuffer *in = make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, 2, 2, 0), *out;
    RdsObjectMeta *om[4];

    /* FixtureFailDark tells the slots apart by their pixel value: 40 (dark) and 70 */
    for (guint slot = 0; slot < 2; slot++) {
      HostImage img;

      snapshot (rds_buffer_get_surface (in), slot, &img);
      g_assert_cmpuint (img.im.plane[0][0], ==, slot ? 70 : 40);
      host_image_clear (&img);
    }
    for (guint i = 0; i < 4; i++)
      om[i] = add_test_object (in, i / 2, 1, 0, 64 * (i % 2), 0, 64, 64);
    g_assert_cmpint (gst_harness_push (h, in), ==, GST_FLOW_OK);
    out = gst_harness_pull (h);
    for (guint i = 0; i < 4; i++) {
      g_assert_cmpuint (count_children (nth_frame (out, i / 2), om[i]), ==, i < 2 ? 0 : 2);
      g_assert_true ((obj_tensor_meta (om[i]) != NULL) == tensor_meta);
    }
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "FixtureFailDark"));
  g_assert_nonnull (strstr (texts, "an object of source 0, which gets no children"));
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  g_free (texts);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
  g_free (name);
}

static void
test_secondary_parser_false (void)
{
  run_parser_false (FALSE);
}

static void
test_secondary_parser_false_tensor (void)
{
  run_parser_false (TRUE);
}

/* Pushes @n buffers of one 64x48 frame with @objects objects each through @h, dropping each output at once. */
static void
push_object_buffers (GstHarness * h, guint n, guint objects)
{
  for (guint b = 0; b < n; b++) {
    g_assert_cmpint (gst_harness_push (h, batch_with_objects (1, &objects)), ==, GST_FLOW_OK);
    gst_buffer_unref (gst_harness_pull (h));
  }
}

/* The element's output sets: in use and free, and allocated and freed since start. */
static void
get_set_counts (GstElement * e, guint * n_sets, guint * n_free, guint64 * allocated, guint64 * freed)
{
  g_assert_true (hook_get_output_sets (e, n_sets, n_free));
  g_assert_true (hook_get_output_set_totals (e, allocated, freed));
}

#define POOL_CONFIG "[property]\nonnx-file=scale2.onnx\ninfer-dims=3;16;16\nnetwork-type=100\nprocess-mode=2\n" \
  "batch-size=1\n"

/* Steady load never frees and reallocates: buffers of 20 objects at batch-size 1 (20 sets in use per buffer) keep
 * the first buffer's 20 sets, reused by every later one. */
static void
test_secondary_pool_steady (void)
{
  gchar *cfg = write_file ("secondary-steady.txt", POOL_CONFIG);
  GstElement *e;
  GstHarness *h;
  guint n_sets, n_free;
  guint64 allocated, freed;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  e = new_infer (cfg);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 64, 48);
  push_object_buffers (h, 1, 20);
  get_set_counts (e, &n_sets, &n_free, &allocated, &freed);
  g_assert_cmpuint (n_sets, ==, 20);
  g_assert_cmpuint (n_free, ==, 20);
  g_assert_cmpuint (allocated, ==, 20);
  g_assert_cmpuint (freed, ==, 0);
  /* past several windows of the recent peak: it stays at 20 */
  push_object_buffers (h, 3 * RDS_INFER_OUTPUT_PEAK_WINDOW / 20 + 1, 20);
  get_set_counts (e, &n_sets, &n_free, &allocated, &freed);
  g_assert_cmpuint (n_sets, ==, 20);
  g_assert_cmpuint (allocated, ==, 20);
  g_assert_cmpuint (freed, ==, 0);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* A spike is trimmed eventually: after one buffer of 30 objects at batch-size 1 (30 sets), the sets stay while the
 * peak is recent (the next buffer reuses them), then buffers of 4 objects bring the pool back to
 * RDS_INFER_OUTPUT_MAX_FREE once the spike is more than two windows of acquires old. */
static void
test_secondary_pool_trim (void)
{
  gchar *cfg = write_file ("secondary-trim.txt", POOL_CONFIG);
  GstElement *e;
  GstHarness *h;
  GstBuffer *out;
  guint n_sets, n_free;
  guint64 allocated, freed;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  e = new_infer (cfg);
  h = harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_NV12, 64, 48);
  g_assert_cmpint (gst_harness_push (h, batch_with_objects (1, (const guint[]) { 30 })), ==, GST_FLOW_OK);
  out = gst_harness_pull (h);
  get_set_counts (e, &n_sets, &n_free, &allocated, &freed);
  g_assert_cmpuint (n_sets, ==, 30);
  g_assert_cmpuint (n_free, ==, 0);
  gst_buffer_unref (out);
  get_set_counts (e, &n_sets, &n_free, &allocated, &freed);
  g_assert_cmpuint (n_sets, ==, 30);
  g_assert_cmpuint (n_free, ==, 30);
  /* a recent peak: kept and reused */
  push_object_buffers (h, 1, 30);
  get_set_counts (e, &n_sets, &n_free, &allocated, &freed);
  g_assert_cmpuint (allocated, ==, 30);
  g_assert_cmpuint (freed, ==, 0);
  /* two windows of small buffers: back to the cap, and the next small buffer allocates nothing */
  push_object_buffers (h, 2 * RDS_INFER_OUTPUT_PEAK_WINDOW / 4 + 1, 4);
  get_set_counts (e, &n_sets, &n_free, &allocated, &freed);
  g_assert_cmpuint (n_sets, ==, RDS_INFER_OUTPUT_MAX_FREE);
  g_assert_cmpuint (n_free, ==, RDS_INFER_OUTPUT_MAX_FREE);
  g_assert_cmpuint (allocated, ==, 30);
  g_assert_cmpuint (freed, ==, 30 - RDS_INFER_OUTPUT_MAX_FREE);
  push_object_buffers (h, 1, 4);
  get_set_counts (e, &n_sets, &n_free, &allocated, &freed);
  g_assert_cmpuint (allocated, ==, 30);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* A chunk that fails after earlier chunks attached (slot 1's surface has no valid format, so its preprocessing
 * fails): an element error, the earlier objects keep their tensor meta, and every set is back in the pool once the
 * buffer is gone. */
static void
test_secondary_chunk_failure (void)
{
  static const guint per_frame[] = { 2, 1 };
  gchar *cfg = write_file ("secondary-fail-chunk.txt", "[property]\nonnx-file=scale2.onnx\ninfer-dims=3;16;16\n"
      "network-type=100\nprocess-mode=2\nbatch-size=1\n"), *texts;
  GstBus *bus = gst_bus_new ();
  GstElement *e;
  GstHarness *h;
  GstBuffer *in;
  RdsBufSurface *s;
  RdsBufSurfaceColorFormat fmt;
  GPtrArray *objs;
  guint n_sets, n_free;

  RDS_TEST_REQUIRE_GPU ();
  ensure_model ();
  load_hooks ();
  e = new_infer (cfg);
  h = harness_for (e, bus, RDS_BUF_COLOR_FORMAT_NV12, 64, 48);
  in = batch_with_objects (2, per_frame);
  s = rds_buffer_get_surface (in);
  fmt = s->surfaceList[1].colorFormat;
  s->surfaceList[1].colorFormat = RDS_BUF_COLOR_FORMAT_INVALID;
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "Preprocessing failed"));
  g_free (texts);
  s->surfaceList[1].colorFormat = fmt;
  objs = all_objects (in);
  g_assert_nonnull (obj_tensor_meta (objs->pdata[0]));
  g_assert_nonnull (obj_tensor_meta (objs->pdata[1]));
  g_assert_null (obj_tensor_meta (objs->pdata[2]));
  g_ptr_array_unref (objs);
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_sets, ==, 2);
  g_assert_cmpuint (n_free, ==, 0);
  gst_buffer_unref (in);
  g_assert_true (hook_get_output_sets (e, &n_sets, &n_free));
  g_assert_cmpuint (n_free, ==, n_sets);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
}

/* ---- gpu: secondary classifiers ---- */

/* Paints the @w x @h rect at (@x, @y) of RGBA slot @slot of @buf with (@r, @g, @b), alpha 255. */
static void
paint_rect (GstBuffer * buf, guint slot, guint x, guint y, guint w, guint h, guint8 r, guint8 g, guint8 b)
{
  RdsBufSurface *s = rds_buffer_get_surface (buf);
  RdsBufSurfaceParams *p = &s->surfaceList[slot];

  g_assert_cmpint (p->colorFormat, ==, RDS_BUF_COLOR_FORMAT_RGBA);
  g_assert_cmpint (rds_buf_surface_map (s, slot, 0, RDS_BUF_MAP_READ_WRITE), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, slot, 0), ==, 0);
  for (guint row = y; row < y + h; row++) {
    guint8 *px = (guint8 *) p->mappedAddr.addr[0] + (gsize) row * p->planeParams.pitch[0] + (gsize) x * 4;

    for (guint col = 0; col < w; col++, px += 4) {
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = 255;
    }
  }
  g_assert_cmpint (rds_buf_surface_sync_for_device (s, slot, 0), ==, 0);
  g_assert_cmpint (rds_buf_surface_unmap (s, slot, 0), ==, 0);
}

/* A classifier on @model, GIE @gie, on the objects of GIE 1 (batch-size 4), with the label file @labels (NULL: no
 * labelfile-path) and @extra at the end of [property]. meancls gives the channel means of its 32x32 input, and with
 * net-scale-factor 1/256 a 32x32 crop of pixel value v gives exactly v / 256 (the crop is the network input, 1:1);
 * meancls2 also gives 1 - the means as a second output layer. */
static gchar *
classifier_config (const gchar * name, const gchar * model, guint gie, const gchar * labels, const gchar * extra)
{
  gchar *label_name = g_strdup_printf ("%s.labels", name), *label_path = NULL, *text, *path;

  ensure_test_model (model);
  if (labels)
    label_path = write_file (label_name, labels);
  text = g_strdup_printf ("[property]\nonnx-file=%s\nnetwork-type=1\nprocess-mode=2\nbatch-size=4\n"
      "net-scale-factor=0.00390625\ngie-unique-id=%u\noperate-on-gie-id=1\n%s%s%s%s", model, gie,
      labels ? "labelfile-path=" : "", labels ? label_name : "", labels ? "\n" : "", extra ? extra : "");
  path = write_file (name, text);
  g_free (text);
  g_free (label_path);
  g_free (label_name);
  return path;
}

/* A 32x32 object of GIE 1 at (@left, @top) of frame @frame, painted (@r, @g, @b), with obj_label "car" and the
 * display text @text (NULL: none). */
static RdsObjectMeta *
add_colour_object (GstBuffer * buf, guint frame, guint left, guint top, guint8 r, guint8 g, guint8 b,
    const gchar * text)
{
  RdsObjectMeta *om = add_test_object (buf, frame, 1, 0, left, top, 32, 32);

  paint_rect (buf, frame, left, top, 32, 32, r, g, b);
  g_strlcpy (om->obj_label, "car", sizeof (om->obj_label));
  om->text_params.display_text = g_strdup (text);
  return om;
}

/* A 128x64 RGBA batch of @frames frames (grey 40, then 70, as make_batch), for objects from add_colour_object. */
static GstBuffer *
colour_batch (guint frames)
{
  return make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 128, 64, frames, frames, 0);
}

static GstHarness *
classifier_harness (const gchar * cfg, GstBus * bus)
{
  GstElement *e = new_infer (cfg);

  if (bus)
    gst_element_set_bus (e, bus);
  return harness_for (e, NULL, RDS_BUF_COLOR_FORMAT_RGBA, 128, 64);
}

static GstBuffer *
push_pull (GstHarness * h, GstBuffer * in)
{
  g_assert_cmpint (gst_harness_push (h, in), ==, GST_FLOW_OK);
  return gst_harness_pull (h);
}

typedef struct
{
  guint label_id, class_id, num_classes;
  gfloat prob;
  const gchar *label;
} WantLabel;

/* @om has @n_cls classifier metas, and meta @i comes from GIE @gie with type @type (NULL: none) and has the labels
 * @want, in order, each within the fixed-size result_label. */
static void
check_classifier (const RdsObjectMeta * om, guint i, guint n_cls, gint gie, const gchar * type,
    const WantLabel * want, guint n_want)
{
  RdsClassifierMeta *c;

  g_assert_cmpuint (g_list_length (om->classifier_meta_list), ==, n_cls);
  c = g_list_nth_data (om->classifier_meta_list, i);
  g_assert_cmpuint (c->num_labels, ==, n_want);
  g_assert_cmpuint (g_list_length (c->label_info_list), ==, n_want);
  g_assert_cmpint (c->unique_component_id, ==, gie);
  /* not owned: interned */
  if (type)
    g_assert_true (c->classifier_type == g_intern_string (type));
  else
    g_assert_null (c->classifier_type);
  for (guint l = 0; l < n_want; l++) {
    const RdsLabelInfo *li = g_list_nth_data (c->label_info_list, l);

    g_test_message ("label %u: layer %u class %u prob %g \"%s\"", l, li->label_id, li->result_class_id,
        li->result_prob, li->result_label);
    g_assert_cmpuint (li->label_id, ==, want[l].label_id);
    g_assert_cmpuint (li->result_class_id, ==, want[l].class_id);
    g_assert_cmpuint (li->num_classes, ==, want[l].num_classes);
    g_assert_cmpfloat (li->result_prob, ==, want[l].prob);
    g_assert_cmpstr (li->result_label, ==, want[l].label);
    g_assert_null (li->pResult_label);
  }
}

/* The parent object as it came in: a classifier never changes obj_label, the class or the producer. */
static void
check_object_kept (const RdsObjectMeta * om, const gchar * text)
{
  g_assert_cmpstr (om->obj_label, ==, "car");
  g_assert_cmpint (om->class_id, ==, 0);
  g_assert_cmpint (om->unique_component_id, ==, 1);
  g_assert_null (om->parent);
  g_assert_cmpstr (om->text_params.display_text, ==, text);
}

#define RGB_LABELS "red;green;blue\n"

/* meancls: per object the raw argmax of the channel means (no softmax: 200 / 256 gives 0.78125), ties to the lowest
 * class, kept above the default threshold 0 (an all-zero output gives nothing); one classifier meta of this GIE and
 * classifier-type, with one label (DeepStream 8.0 behaviour; num_classes is ours), " label " appended to the
 * display text (DeepStream 8.0 behaviour), nothing else of the object changed, no tensor meta. */
static void
test_classifier_classes (void)
{
  static const WantLabel red = { 0, 0, 3, 0.78125f, "red" }, green = { 0, 1, 3, 0.625f, "green" };
  static const WantLabel blue = { 0, 2, 3, 0.99609375f, "blue" }, grey = { 0, 0, 3, 0.5f, "red" };
  gchar *cfg = classifier_config ("cls-classes.txt", "meancls.onnx", 4, RGB_LABELS, "classifier-type=colour\n");
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om[6];

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (2);
  om[0] = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
  om[1] = add_colour_object (in, 0, 32, 0, 0, 160, 0, NULL);
  om[2] = add_colour_object (in, 0, 64, 0, 0, 0, 0, "B");
  om[3] = add_colour_object (in, 0, 96, 0, 128, 128, 128, "G");
  om[4] = add_colour_object (in, 1, 0, 32, 0, 0, 255, "T");
  /* another GIE's object is not inferred on */
  om[5] = add_test_object (in, 1, 2, 0, 32, 32, 32, 32);
  out = push_pull (h, in);
  g_assert_cmpuint (hook_get_unit_count (h->element), ==, 5);
  check_classifier (om[0], 0, 1, 4, "colour", &red, 1);
  check_object_kept (om[0], "T red ");
  check_classifier (om[1], 0, 1, 4, "colour", &green, 1);
  check_object_kept (om[1], " green ");
  g_assert_null (om[2]->classifier_meta_list);
  check_object_kept (om[2], "B");
  check_classifier (om[3], 0, 1, 4, "colour", &grey, 1);
  check_object_kept (om[3], "G red ");
  check_classifier (om[4], 0, 1, 4, "colour", &blue, 1);
  check_object_kept (om[4], "T blue ");
  g_assert_null (om[5]->classifier_meta_list);
  for (guint i = 0; i < 6; i++)
    g_assert_null (obj_tensor_meta (om[i]));
  g_assert_cmpuint (nth_frame (out, 0)->num_obj_meta, ==, 4);
  check_frames_untouched (out, -1);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* classifier-threshold is strict (DeepStream 8.0 behaviour): 0.5 rejects 0.5 (no classifier meta at all, the text
 * unchanged) and keeps 0.50390625; without classifier-type the meta has none. */
static void
test_classifier_threshold (void)
{
  static const WantLabel above = { 0, 0, 3, 0.50390625f, "red" };
  gchar *cfg = classifier_config ("cls-threshold.txt", "meancls.onnx", 4, RGB_LABELS, "classifier-threshold=0.5\n");
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *at, *over;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (1);
  at = add_colour_object (in, 0, 0, 0, 128, 128, 128, "T");
  over = add_colour_object (in, 0, 32, 0, 129, 129, 129, "T");
  out = push_pull (h, in);
  g_assert_null (at->classifier_meta_list);
  check_object_kept (at, "T");
  check_classifier (over, 0, 1, 4, NULL, &above, 1);
  check_object_kept (over, "T red ");
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* meancls2: one label per output layer, in layer order (ours: 8.0 prepends), each kept or left out by the threshold
 * on its own; the display text gets every attached label. */
static void
test_classifier_two_layers (void)
{
  static const WantLabel both[] = { {0, 0, 3, 0.78125f, "r"}, {1, 1, 3, 1.0f, "G"} };
  static const WantLabel second = { 1, 0, 3, 1.0f, "R" };
  gchar *cfg = classifier_config ("cls-two.txt", "meancls2.onnx", 4, "r;g;b\nR;G;B\n", "classifier-threshold=0.7\n");
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om[3];

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (1);
  /* means (0.78125, 0, 0), output1 (0.21875, 1, 1): both layers */
  om[0] = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
  /* 0.5 everywhere: neither */
  om[1] = add_colour_object (in, 0, 32, 0, 128, 128, 128, "T");
  /* means (0, 0.25, 0): below; output1 (1, 0.75, 1): class 0 */
  om[2] = add_colour_object (in, 0, 64, 0, 0, 64, 0, "T");
  out = push_pull (h, in);
  check_classifier (om[0], 0, 1, 4, NULL, both, 2);
  check_object_kept (om[0], "T r G ");
  g_assert_null (om[1]->classifier_meta_list);
  check_object_kept (om[1], "T");
  check_classifier (om[2], 0, 1, 4, NULL, &second, 1);
  check_object_kept (om[2], "T R ");
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* No label file: start warns, and the classifier meta is still attached, with an empty label (ours: 8.0 attaches
 * none). */
static void
test_classifier_no_labels (void)
{
  static const WantLabel red = { 0, 0, 3, 0.78125f, "" };
  gchar *cfg = classifier_config ("cls-nolabels.txt", "meancls.onnx", 4, NULL, NULL), *texts;
  GstBus *bus = gst_bus_new ();
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, bus);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "cls-nolabels.txt:labelfile-path: (unset): classifier output gets empty labels"));
  g_free (texts);
  in = colour_batch (1);
  om = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
  out = push_pull (h, in);
  check_classifier (om, 0, 1, 4, NULL, &red, 1);
  check_object_kept (om, "T  ");
  gst_buffer_unref (out);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
}

/* An argmax past its line's labels and a layer without a line give an empty label (ours: 8.0 attaches nothing for
 * the first). A label longer than result_label is truncated there and kept whole in pResult_label (ours), and the
 * display text has it whole. */
static void
test_classifier_label_gaps (void)
{
  static const WantLabel blue[] = { {0, 2, 3, 0.99609375f, ""}, {1, 0, 3, 1.0f, ""} };
  gchar *long_label = g_strnfill (200, 'g'), *labels = g_strdup_printf ("red;%s\n", long_label);
  gchar *cfg = classifier_config ("cls-gaps.txt", "meancls2.onnx", 4, labels, NULL), *text;
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om[2];
  RdsClassifierMeta *c;
  const RdsLabelInfo *li;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (1);
  om[0] = add_colour_object (in, 0, 0, 0, 0, 0, 255, "T");
  om[1] = add_colour_object (in, 0, 32, 0, 0, 160, 0, "T");
  out = push_pull (h, in);
  check_classifier (om[0], 0, 1, 4, NULL, blue, 2);
  check_object_kept (om[0], "T   ");

  /* green: layer 0 class 1, the long label; layer 1 (1, 0.375, 1): class 0, no line */
  c = om[1]->classifier_meta_list->data;
  g_assert_cmpuint (c->num_labels, ==, 2);
  li = c->label_info_list->data;
  g_assert_cmpuint (li->result_class_id, ==, 1);
  g_assert_cmpuint (strlen (li->result_label), ==, RDS_MAX_LABEL_SIZE - 1);
  g_assert_true (strncmp (li->result_label, long_label, RDS_MAX_LABEL_SIZE - 1) == 0);
  g_assert_cmpstr (li->pResult_label, ==, long_label);
  li = c->label_info_list->next->data;
  g_assert_cmpuint (li->label_id, ==, 1);
  g_assert_cmpstr (li->result_label, ==, "");
  g_assert_null (li->pResult_label);
  text = g_strdup_printf ("T %s  ", long_label);
  check_object_kept (om[1], text);
  g_free (text);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
  g_free (labels);
  g_free (long_label);
}

/* A label file whose line count isn't the number of output layers, or a line whose label count isn't its layer's
 * class count, is one warning at start (ours), and the classifier runs all the same; a matching file warns not. */
static void
test_classifier_label_count (void)
{
  static const WantLabel red = { 0, 0, 3, 0.78125f, "red" };
  static const struct
  {
    const gchar *name, *labels, *want;
  } cases[] = {
    {"cls-count-short.txt", "red;green\n", "line 1 has 2 labels for the 3 classes of output layer "},
    {"cls-count-lines.txt", "red;green;blue\nx;y;z\n", "2 label lines for 1 output layers"},
    {"cls-count-ok.txt", RGB_LABELS, NULL},
  };

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  for (guint c = 0; c < G_N_ELEMENTS (cases); c++) {
    gchar *cfg = classifier_config (cases[c].name, "meancls.onnx", 4, cases[c].labels, NULL), *texts;
    GstBus *bus = gst_bus_new ();
    GstHarness *h = classifier_harness (cfg, bus);
    GstBuffer *in, *out;
    RdsObjectMeta *om;

    g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, (cases[c].want ? 1 : 0));
    if (cases[c].want) {
      g_assert_nonnull (strstr (texts, cases[c].name));
      g_assert_nonnull (strstr (texts, ":labelfile-path: "));
      g_assert_nonnull (strstr (texts, cases[c].want));
    }
    g_free (texts);
    in = colour_batch (1);
    om = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
    out = push_pull (h, in);
    check_classifier (om, 0, 1, 4, NULL, &red, 1);
    check_object_kept (om, "T red ");
    /* only at start */
    g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, NULL), ==, 0);
    gst_buffer_unref (out);
    harness_teardown_with_bus (h, bus);
    g_free (cfg);
  }
}

/* Two classifiers in a row both attach to the object: the second's meta after the first's (ours: 8.0 prepends), and
 * the display text grows by each one's labels (DeepStream 8.0 behaviour). */
static void
test_classifier_chained (void)
{
  static const WantLabel red = { 0, 0, 3, 0.78125f, "red" }, x = { 0, 0, 3, 0.78125f, "x" };
  gchar *cfg1 = classifier_config ("cls-chain1.txt", "meancls.onnx", 4, RGB_LABELS, NULL);
  gchar *cfg2 = classifier_config ("cls-chain2.txt", "meancls.onnx", 6, "x;y;z\n", "classifier-type=second\n");
  GstHarness *h1, *h2;
  GstBuffer *in, *out;
  RdsObjectMeta *om;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h1 = classifier_harness (cfg1, NULL);
  h2 = classifier_harness (cfg2, NULL);
  in = colour_batch (1);
  om = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
  out = push_pull (h2, push_pull (h1, in));
  check_classifier (om, 0, 2, 4, NULL, &red, 1);
  check_classifier (om, 1, 2, 6, "second", &x, 1);
  check_object_kept (om, "T red  x ");
  gst_buffer_unref (out);
  gst_harness_teardown (h2);
  gst_harness_teardown (h1);
  g_free (cfg2);
  g_free (cfg1);
}

/* parse-classifier-func-name: called once per object with that object's layers, the network and the threshold; its
 * attributes are the labels as they are (no threshold of ours: FixtureClassify returns half of it), and the display
 * text gets " " and its description (DeepStream 8.0 behaviour). */
static void
test_classifier_custom (void)
{
  static const WantLabel red = { 0, 200, 3, 0.3f, "attr0-32x32" }, green = { 0, 0, 3, 0.3f, "attr0-32x32" };
  gchar *extra = g_strdup_printf ("custom-lib-path=%s\nparse-classifier-func-name=FixtureClassify\n"
      "classifier-threshold=0.6\n", RDS_TEST_PARSER_FIXTURE);
  gchar *cfg = classifier_config ("cls-custom.txt", "meancls.onnx", 4, RGB_LABELS, extra);
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om[2];

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (2);
  om[0] = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
  om[1] = add_colour_object (in, 1, 32, 0, 0, 160, 0, NULL);
  out = push_pull (h, in);
  check_classifier (om[0], 0, 1, 4, NULL, &red, 1);
  check_object_kept (om[0], "T desc1");
  check_classifier (om[1], 0, 1, 4, NULL, &green, 1);
  check_object_kept (om[1], " desc1");
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
  g_free (extra);
}

/* A custom parser that returns false (FixtureClassifyFailDark, on red below 64): one warning per element, that object
 * (untracked) gets no classifier meta, the others theirs (ours: DeepStream 8.0 crashes). */
static void
test_classifier_custom_false (void)
{
  gchar *extra = g_strdup_printf ("custom-lib-path=%s\nparse-classifier-func-name=FixtureClassifyFailDark\n",
      RDS_TEST_PARSER_FIXTURE), *texts;
  gchar *cfg = classifier_config ("cls-false.txt", "meancls.onnx", 4, RGB_LABELS, extra);
  GstBus *bus = gst_bus_new ();
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, bus);
  for (guint b = 0; b < 2; b++) {
    GstBuffer *in = colour_batch (1), *out;
    RdsObjectMeta *dark = add_colour_object (in, 0, 0, 0, 40, 200, 0, "T");
    RdsObjectMeta *bright = add_colour_object (in, 0, 32, 0, 200, 0, 0, "T");

    out = push_pull (h, in);
    g_assert_null (dark->classifier_meta_list);
    check_object_kept (dark, "T");
    g_assert_cmpuint (g_list_length (bright->classifier_meta_list), ==, 1);
    check_object_kept (bright, "T desc1");
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "FixtureClassifyFailDark"));
  g_assert_nonnull (strstr (texts, "an object of source 0, which gets no new classifier meta "
          "(a tracked object keeps its stored result, if any)"));
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  g_free (texts);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
  g_free (extra);
}

/* A custom parser that leaves descString empty: the display text gets its labels, as for the built-in parser (ours:
 * 8.0 not observed). */
static void
test_classifier_custom_no_desc (void)
{
  static const WantLabel red = { 0, 200, 3, 0.0f, "attr0-32x32" };
  gchar *extra = g_strdup_printf ("custom-lib-path=%s\nparse-classifier-func-name=FixtureClassifyNoDesc\n",
      RDS_TEST_PARSER_FIXTURE);
  gchar *cfg = classifier_config ("cls-nodesc.txt", "meancls.onnx", 4, RGB_LABELS, extra);
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (1);
  om = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
  out = push_pull (h, in);
  check_classifier (om, 0, 1, 4, NULL, &red, 1);
  check_object_kept (om, "T attr0-32x32 ");
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
  g_free (extra);
}

/* A custom parser that throws (FixtureClassifyThrow, on every object): one warning per element, no classifier meta,
 * the labels it allocated freed (ASan), and the stream goes on (ours). */
static void
test_classifier_custom_throw (void)
{
  gchar *extra = g_strdup_printf ("custom-lib-path=%s\nparse-classifier-func-name=FixtureClassifyThrow\n",
      RDS_TEST_PARSER_FIXTURE), *texts;
  gchar *cfg = classifier_config ("cls-throw.txt", "meancls.onnx", 4, RGB_LABELS, extra);
  GstBus *bus = gst_bus_new ();
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, bus);
  for (guint b = 0; b < 2; b++) {
    GstBuffer *in = colour_batch (1), *out;
    RdsObjectMeta *om = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");

    out = push_pull (h, in);
    g_assert_null (om->classifier_meta_list);
    check_object_kept (om, "T");
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (hook_get_unit_count (h->element), ==, 2);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "FixtureClassifyThrow"));
  g_assert_nonnull (strstr (texts, "threw an exception"));
  g_assert_nonnull (strstr (texts, "which gets no new classifier meta"));
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, NULL), ==, 0);
  g_free (texts);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
  g_free (extra);
}

/* A parse-classifier-func-name the library doesn't have fails start, naming the key. */
static void
test_classifier_unknown_symbol (void)
{
  gchar *extra = g_strdup_printf ("custom-lib-path=%s\nparse-classifier-func-name=NoSuchClassifier\n",
      RDS_TEST_PARSER_FIXTURE), *texts;
  gchar *cfg = classifier_config ("cls-nosym.txt", "meancls.onnx", 4, RGB_LABELS, extra);

  RDS_TEST_REQUIRE_GPU ();
  texts = start_error (new_infer (cfg));
  g_assert_nonnull (strstr (texts, "cls-nosym.txt:parse-classifier-func-name: NoSuchClassifier: "));
  g_free (texts);
  g_free (cfg);
  g_free (extra);
}

/* The first batch of a fresh element: fresh elements one after the other in this process, each on an object of
 * another colour than the last one's, each classify their first object right and leave its crop in the network
 * input (the previous element's input and outputs, freed, are often handed out again at the same addresses). The
 * engine's warm-up runs on the element's own stream (see warm_up in rds-infer-engine.cpp). */
static void
test_classifier_fresh_elements (void)
{
  static const WantLabel want[] = {
    {0, 0, 3, 0.78125f, "red"}, {0, 1, 3, 0.625f, "green"}, {0, 2, 3, 0.99609375f, "blue"},
  };
  static const guint8 rgb[][3] = { {200, 0, 0}, {0, 160, 0}, {0, 0, 255} };
  gchar *cfg = classifier_config ("cls-fresh.txt", "meancls.onnx", 4, RGB_LABELS, NULL);

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  for (guint e = 0; e < 12; e++) {
    guint c = e % G_N_ELEMENTS (rgb);
    GstHarness *h = classifier_harness (cfg, NULL);
    gfloat *input = g_new (gfloat, 3 * 32 * 32);
    GstBuffer *in = colour_batch (1), *out;
    RdsObjectMeta *om = add_colour_object (in, 0, 0, 0, rgb[c][0], rgb[c][1], rgb[c][2], "T");

    out = push_pull (h, in);
    g_assert_true (hook_read_input (h->element, input, 3 * 32 * 32));
    for (guint ch = 0; ch < 3; ch++)
      g_assert_cmpfloat (input[ch * 32 * 32], ==, rgb[c][ch] / 256.0f);
    check_classifier (om, 0, 1, 4, NULL, &want[c], 1);
    gst_buffer_unref (out);
    g_free (input);
    gst_harness_teardown (h);
  }
  g_free (cfg);
}

/* With output-tensor-meta a classified object gets both metas; the tensor holds the raw means. */
static void
test_classifier_tensor_meta (void)
{
  static const WantLabel red = { 0, 0, 3, 0.78125f, "red" };
  gchar *cfg = classifier_config ("cls-tensor.txt", "meancls.onnx", 4, RGB_LABELS, "output-tensor-meta=1\n");
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *om;
  RdsInferTensorMeta *tm;
  const gfloat *means;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (1);
  om = add_colour_object (in, 0, 0, 0, 200, 0, 0, "T");
  out = push_pull (h, in);
  check_classifier (om, 0, 1, 4, NULL, &red, 1);
  check_object_kept (om, "T red ");
  tm = obj_tensor_meta (om);
  g_assert_nonnull (tm);
  g_assert_cmpuint (tm->unique_id, ==, 4);
  g_assert_cmpuint (tm->num_output_layers, ==, 1);
  means = tm->out_buf_ptrs_host[0];
  g_assert_cmpfloat (means[0], ==, 0.78125f);
  g_assert_cmpfloat (means[1], ==, 0.0f);
  g_assert_cmpfloat (means[2], ==, 0.0f);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* ---- gpu: the per-object cache ---- */

/* An object of GIE 1 with tracked id @id, @size x @size at (@left, @top) of frame @frame, painted (@r, @g, @b),
 * obj_label "car" and the display text @text, as add_colour_object. */
static RdsObjectMeta *
add_tracked (GstBuffer * buf, guint frame, guint64 id, guint left, guint top, guint size, guint8 r, guint8 g,
    guint8 b, const gchar * text)
{
  RdsObjectMeta *om = add_colour_object (buf, frame, left, top, r, g, b, text);

  om->object_id = id;
  om->rect_params.width = om->rect_params.height = (gfloat) size;
  paint_rect (buf, frame, left, top, size, size, r, g, b);
  return om;
}

/* Pushes a one-frame batch with tracked object @id (32x32 at the origin, painted (@r, @g, 0)) through @h; checks
 * the unit count afterwards and that the object got @want. */
static void
push_tracked (GstHarness * h, guint64 id, guint8 r, guint8 g, guint64 units, const WantLabel * want)
{
  GstBuffer *in = colour_batch (1), *out;
  RdsObjectMeta *om = add_tracked (in, 0, id, 0, 0, 32, r, g, 0, "T");

  out = push_pull (h, in);
  g_assert_cmpuint (hook_get_unit_count (h->element), ==, units);
  check_classifier (om, 0, 1, 4, NULL, want, 1);
  gst_buffer_unref (out);
}

static const WantLabel cache_red = { 0, 0, 3, 0.78125f, "red" }, cache_green = { 0, 1, 3, 0.625f, "green" };

/* Tracked objects are inferred once; later frames get an identical copy of the stored result (classifier meta,
 * labels, the display text append) and no tensor meta, also when their pixels changed. An empty result (below the
 * threshold) is cached too and not retried. Untracked objects are inferred every frame (DeepStream 8.0
 * behaviour). */
static void
test_cache_hits (void)
{
  static const WantLabel blue = { 0, 2, 3, 0.99609375f, "blue" };
  gchar *cfg = classifier_config ("cache-hits.txt", "meancls.onnx", 4, RGB_LABELS,
      "classifier-type=colour\noutput-tensor-meta=1\nclassifier-threshold=0.1\n");
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  for (guint b = 0; b < 3; b++) {
    GstBuffer *in = colour_batch (1), *out;
    /* from the second batch on id 1 is blue and id 3 bright red: the cache still serves red and nothing */
    RdsObjectMeta *t1 = add_tracked (in, 0, 1, 0, 0, 32, b ? 0 : 200, 0, b ? 255 : 0, "T");
    RdsObjectMeta *t2 = add_tracked (in, 0, 2, 32, 0, 32, 0, 160, 0, NULL);
    RdsObjectMeta *t3 = add_tracked (in, 0, 3, 64, 0, 32, b ? 200 : 10, 0, 0, "D");
    RdsObjectMeta *u = add_colour_object (in, 0, 96, 0, b ? 0 : 200, 0, b ? 255 : 0, "U");

    out = push_pull (h, in);
    g_assert_cmpuint (hook_get_unit_count (h->element), ==, 4 + b);
    check_classifier (t1, 0, 1, 4, "colour", &cache_red, 1);
    check_object_kept (t1, "T red ");
    check_classifier (t2, 0, 1, 4, "colour", &cache_green, 1);
    check_object_kept (t2, " green ");
    g_assert_null (t3->classifier_meta_list);
    check_object_kept (t3, "D");
    check_classifier (u, 0, 1, 4, "colour", b ? &blue : &cache_red, 1);
    check_object_kept (u, b ? "U blue " : "U red ");
    g_assert_nonnull (obj_tensor_meta (u));
    g_assert_true ((obj_tensor_meta (t1) != NULL) == (b == 0));
    g_assert_true ((obj_tensor_meta (t2) != NULL) == (b == 0));
    g_assert_true ((obj_tensor_meta (t3) != NULL) == (b == 0));
    gst_buffer_unref (out);
  }
  gst_harness_teardown (h);
  g_free (cfg);
}

/* Growth above 1.2 x the area at the last inference infers again (DeepStream 8.0 behaviour): 32x32 -> 34x34 (1.13)
 * is served from the cache, 36x36 (1.27) is inferred, and its result is served afterwards. */
static void
test_cache_growth (void)
{
  static const struct
  {
    guint size;
    guint8 r, g;
    guint64 units;
    const WantLabel *want;
  } steps[] = {
    {32, 200, 0, 1, &cache_red}, {34, 0, 160, 1, &cache_red}, {36, 0, 160, 2, &cache_green},
    {36, 200, 0, 2, &cache_green},
  };
  gchar *cfg = classifier_config ("cache-growth.txt", "meancls.onnx", 4, RGB_LABELS, NULL);
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  for (guint i = 0; i < G_N_ELEMENTS (steps); i++) {
    GstBuffer *in = colour_batch (1), *out;
    RdsObjectMeta *om = add_tracked (in, 0, 7, 0, 0, steps[i].size, steps[i].r, steps[i].g, 0, "T");

    out = push_pull (h, in);
    g_assert_cmpuint (hook_get_unit_count (h->element), ==, steps[i].units);
    check_classifier (om, 0, 1, 4, NULL, steps[i].want, 1);
    gst_buffer_unref (out);
  }
  gst_harness_teardown (h);
  g_free (cfg);
}

/* secondary-reinfer-interval=1: a tracked object is inferred every second frame (DeepStream 8.0 behaviour) and served
 * from the cache in between: painted red on the inferred frames and green on the others, it is always red. */
static void
test_cache_interval (void)
{
  gchar *cfg = classifier_config ("cache-interval.txt", "meancls.onnx", 4, RGB_LABELS,
      "secondary-reinfer-interval=1\n");
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  for (guint f = 0; f < 6; f++)
    push_tracked (h, 1, f % 2 ? 0 : 200, f % 2 ? 160 : 0, f / 2 + 1, &cache_red);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* A secondary detector with secondary-reinfer-interval=1: a tracked parent is inferred only on its due frames and
 * gets no children on the others, an untracked parent every frame (DeepStream 8.0 behaviour). */
static void
test_cache_detector_interval (void)
{
  static const guint64 units[] = { 2, 3, 5, 6 };
  gchar *cfg = fixeddet_config ("cache-det.txt", "secondary-reinfer-interval=1\n");
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  ensure_test_model ("fixeddet.onnx");
  load_hooks ();
  h = harness_for (new_infer (cfg), NULL, RDS_BUF_COLOR_FORMAT_RGBA, 256, 128);
  for (guint f = 0; f < G_N_ELEMENTS (units); f++) {
    GstBuffer *in = make_batch (RDS_BUF_COLOR_FORMAT_RGBA, 256, 128, 1, 1, 0), *out;
    RdsObjectMeta *parents[3];

    parents[0] = add_test_object (in, 0, 1, 0, 100, 40, 64, 64);
    parents[0]->object_id = 1;
    parents[1] = add_test_object (in, 0, 1, 0, 223.4f, 95.2f, 40, 40);
    parents[2] = add_test_object (in, 0, 2, 0, 10, 10, 40, 40);
    out = push_pull (h, in);
    g_assert_cmpuint (hook_get_unit_count (h->element), ==, units[f]);
    if (f % 2 == 0) {
      check_children (out, parents);
    } else {
      g_assert_cmpuint (count_children (nth_frame (out, 0), parents[0]), ==, 0);
      g_assert_cmpuint (count_children (nth_frame (out, 0), parents[1]), ==, 6);
    }
    gst_buffer_unref (out);
  }
  gst_harness_teardown (h);
  g_free (cfg);
}

/* classifier-async-mode=1 warns at start and runs synchronously: the results are in the same buffer, for tracked and
 * untracked objects (ours: DeepStream 8.0 attaches them one frame late, and never to untracked objects). */
static void
test_cache_async_mode (void)
{
  gchar *cfg = classifier_config ("cache-async.txt", "meancls.onnx", 4, RGB_LABELS, "classifier-async-mode=1\n"),
      *texts;
  GstBus *bus = gst_bus_new ();
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *t, *u;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, bus);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "cache-async.txt:classifier-async-mode: 1"));
  g_assert_nonnull (strstr (texts, "runs synchronously"));
  g_free (texts);
  in = colour_batch (1);
  t = add_tracked (in, 0, 1, 0, 0, 32, 200, 0, 0, "T");
  u = add_colour_object (in, 0, 32, 0, 0, 160, 0, "U");
  out = push_pull (h, in);
  check_classifier (t, 0, 1, 4, NULL, &cache_red, 1);
  check_classifier (u, 0, 1, 4, NULL, &cache_green, 1);
  gst_buffer_unref (out);
  push_tracked (h, 1, 0, 160, 2, &cache_red);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
}

/* The source's stream-reset or stream-eos clears its entries, another source's doesn't; a flush clears every one
 * (ours): the next frame infers the id again. */
static void
test_cache_events (void)
{
  gchar *cfg = classifier_config ("cache-events.txt", "meancls.onnx", 4, RGB_LABELS, NULL);
  GstHarness *h;
  GstSegment seg;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  push_tracked (h, 1, 200, 0, 1, &cache_red);
  g_assert_true (gst_harness_push_event (h, rds_event_new_stream_reset (0)));
  push_tracked (h, 1, 0, 160, 2, &cache_green);
  g_assert_true (gst_harness_push_event (h, rds_event_new_stream_eos (1)));
  g_assert_true (gst_harness_push_event (h, rds_event_new_stream_reset (1)));
  push_tracked (h, 1, 200, 0, 2, &cache_green);
  g_assert_true (gst_harness_push_event (h, rds_event_new_stream_eos (0)));
  push_tracked (h, 1, 200, 0, 3, &cache_red);
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (h, gst_event_new_segment (&seg)));
  push_tracked (h, 1, 0, 160, 4, &cache_green);
  push_tracked (h, 1, 200, 0, 4, &cache_green);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* Pushes @n one-frame batches without objects through @h. */
static void
push_empty (GstHarness * h, guint n)
{
  for (guint i = 0; i < n; i++)
    gst_buffer_unref (push_pull (h, colour_batch (1)));
}

/* An id absent for 29 batches is still served; absent for 30 it is evicted and inferred again (ours: DeepStream 8.0
 * never evicts). Batches without objects count. */
static void
test_cache_eviction (void)
{
  gchar *cfg = classifier_config ("cache-evict.txt", "meancls.onnx", 4, RGB_LABELS, NULL);
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  push_tracked (h, 1, 200, 0, 1, &cache_red);
  push_empty (h, 29);
  push_tracked (h, 1, 0, 160, 1, &cache_red);
  push_empty (h, 30);
  push_tracked (h, 1, 0, 160, 2, &cache_green);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* An object that stays in its source's frames but is filtered out (here by class) is still seen: after 35 such frames
 * it is selected again and served from the cache, while an id absent for those frames was evicted and is inferred
 * again (ours). */
static void
test_cache_filtered_kept (void)
{
  gchar *cfg = classifier_config ("cache-filtered.txt", "meancls.onnx", 4, RGB_LABELS, "operate-on-class-ids=0\n");
  GstHarness *h;
  GstBuffer *in, *out;
  RdsObjectMeta *kept, *back;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, NULL);
  in = colour_batch (1);
  kept = add_tracked (in, 0, 1, 0, 0, 32, 200, 0, 0, "T");
  back = add_tracked (in, 0, 2, 32, 0, 32, 200, 0, 0, "T");
  out = push_pull (h, in);
  g_assert_cmpuint (hook_get_unit_count (h->element), ==, 2);
  gst_buffer_unref (out);
  for (guint f = 0; f < 35; f++) {
    in = colour_batch (1);
    kept = add_tracked (in, 0, 1, 0, 0, 32, 200, 0, 0, "T");
    kept->class_id = 1;
    out = push_pull (h, in);
    g_assert_null (kept->classifier_meta_list);
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (hook_get_unit_count (h->element), ==, 2);
  in = colour_batch (1);
  kept = add_tracked (in, 0, 1, 0, 0, 32, 0, 160, 0, "T");
  back = add_tracked (in, 0, 2, 32, 0, 32, 0, 160, 0, "T");
  out = push_pull (h, in);
  g_assert_cmpuint (hook_get_unit_count (h->element), ==, 3);
  check_classifier (kept, 0, 1, 4, NULL, &cache_red, 1);
  check_classifier (back, 0, 1, 4, NULL, &cache_green, 1);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
  g_free (cfg);
}

/* A re-inference whose parser fails (FixtureClassifyFailDark, on red below 64) attaches the stored result on that
 * frame and leaves the entry as it was, so the object stays due and the next good inference replaces it (ours). */
static void
test_cache_parser_failure (void)
{
  gchar *extra = g_strdup_printf ("custom-lib-path=%s\nparse-classifier-func-name=FixtureClassifyFailDark\n",
      RDS_TEST_PARSER_FIXTURE), *texts;
  gchar *cfg = classifier_config ("cache-parser-fail.txt", "meancls.onnx", 4, RGB_LABELS, extra);
  static const struct
  {
    guint size;
    guint8 r;
    guint64 units;
    guint class_id;
  } steps[] = {
    /* inferred: red 200; grown and dark: inferred, fails, red 200 kept; grown and bright: inferred, red 120 */
    {32, 200, 1, 200}, {40, 40, 2, 200}, {40, 120, 3, 120}, {40, 40, 3, 120},
  };
  GstBus *bus = gst_bus_new ();
  GstHarness *h;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, bus);
  for (guint i = 0; i < G_N_ELEMENTS (steps); i++) {
    GstBuffer *in = colour_batch (1), *out;
    RdsObjectMeta *om = add_tracked (in, 0, 1, 0, 0, steps[i].size, steps[i].r, 200, 0, "T");
    const RdsLabelInfo *li;

    out = push_pull (h, in);
    g_assert_cmpuint (hook_get_unit_count (h->element), ==, steps[i].units);
    g_assert_cmpuint (g_list_length (om->classifier_meta_list), ==, 1);
    li = ((RdsClassifierMeta *) om->classifier_meta_list->data)->label_info_list->data;
    g_assert_cmpuint (li->result_class_id, ==, steps[i].class_id);
    check_object_kept (om, "T desc1");
    gst_buffer_unref (out);
  }
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_WARNING, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "FixtureClassifyFailDark"));
  g_free (texts);
  harness_teardown_with_bus (h, bus);
  g_free (cfg);
  g_free (extra);
}

/* A chunk that fails (the slot's surface has no valid format, so its preprocessing fails) cancels the cache requests
 * of its units: the tracked object is due again on the next batch, not stuck waiting for a result (ours). */
static void
test_cache_chunk_failure (void)
{
  gchar *cfg = classifier_config ("cache-chunk-fail.txt", "meancls.onnx", 4, RGB_LABELS, NULL), *texts;
  GstBus *bus = gst_bus_new ();
  GstHarness *h;
  GstBuffer *in;
  RdsBufSurface *s;
  RdsBufSurfaceColorFormat fmt;
  RdsObjectMeta *om;

  RDS_TEST_REQUIRE_GPU ();
  load_hooks ();
  h = classifier_harness (cfg, bus);
  in = colour_batch (1);
  om = add_tracked (in, 0, 1, 0, 0, 32, 200, 0, 0, "T");
  s = rds_buffer_get_surface (in);
  fmt = s->surfaceList[0].colorFormat;
  s->surfaceList[0].colorFormat = RDS_BUF_COLOR_FORMAT_INVALID;
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_ERROR);
  g_assert_cmpuint (pop_messages (bus, GST_MESSAGE_ERROR, &texts), ==, 1);
  g_assert_nonnull (strstr (texts, "Preprocessing failed"));
  g_free (texts);
  s->surfaceList[0].colorFormat = fmt;
  g_assert_null (om->classifier_meta_list);
  g_assert_cmpuint (hook_get_unit_count (h->element), ==, 0);
  gst_buffer_unref (in);
  push_tracked (h, 1, 200, 0, 1, &cache_red);
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
  g_test_add_func ("/cpu/infer/override-secondary", test_override_secondary);
  g_test_add_func ("/cpu/infer/start-config-errors", test_start_config_errors);
  g_test_add_func ("/cpu/infer/classifier-attach", test_classifier_attach);
  g_test_add_func ("/gpu/infer/start-errors", test_start_errors);
  g_test_add_func ("/gpu/infer/no-batch-meta", test_no_batch_meta);
  g_test_add_func ("/gpu/infer/tensor-meta", test_tensor_meta);
  g_test_add_func ("/gpu/infer/restart", test_restart);
  g_test_add_func ("/gpu/infer/demux", test_demux);
  g_test_add_func ("/gpu/infer/interval", test_interval);
  g_test_add_func ("/gpu/infer/detector", test_detector);
  g_test_add_func ("/gpu/infer/parser-failure", test_parser_failure);
  g_test_add_func ("/gpu/infer/frame-replacement-failure", test_frame_replacement_failure);
  g_test_add_func ("/gpu/infer/frame-replacement", test_frame_replacement);
  g_test_add_func ("/gpu/infer/concurrency", test_concurrency);
  g_test_add_func ("/gpu/infer-secondary/tensor-crop", test_secondary_tensor_crop);
  g_test_add_func ("/gpu/infer-secondary/chunks", test_secondary_chunks);
  g_test_add_func ("/gpu/infer-secondary/selection", test_secondary_selection);
  g_test_add_func ("/gpu/infer-secondary/detector-children", test_secondary_detector_children);
  g_test_add_func ("/gpu/infer-secondary/object-removed", test_secondary_object_removed);
  g_test_add_func ("/gpu/infer-secondary/object-replaced", test_secondary_object_replaced);
  g_test_add_func ("/gpu/infer-secondary/frame-replaced", test_secondary_frame_replaced);
  g_test_add_func ("/gpu/infer-secondary/parser-false", test_secondary_parser_false);
  g_test_add_func ("/gpu/infer-secondary/parser-false-tensor", test_secondary_parser_false_tensor);
  g_test_add_func ("/gpu/infer-secondary/pool-steady", test_secondary_pool_steady);
  g_test_add_func ("/gpu/infer-secondary/pool-trim", test_secondary_pool_trim);
  g_test_add_func ("/gpu/infer-secondary/chunk-failure", test_secondary_chunk_failure);
  g_test_add_func ("/gpu/infer-secondary/roi-ignored", test_secondary_roi_ignored);
  g_test_add_func ("/gpu/infer-secondary/classifier-classes", test_classifier_classes);
  g_test_add_func ("/gpu/infer-secondary/classifier-threshold", test_classifier_threshold);
  g_test_add_func ("/gpu/infer-secondary/classifier-two-layers", test_classifier_two_layers);
  g_test_add_func ("/gpu/infer-secondary/classifier-no-labels", test_classifier_no_labels);
  g_test_add_func ("/gpu/infer-secondary/classifier-label-gaps", test_classifier_label_gaps);
  g_test_add_func ("/gpu/infer-secondary/classifier-label-count", test_classifier_label_count);
  g_test_add_func ("/gpu/infer-secondary/classifier-chained", test_classifier_chained);
  g_test_add_func ("/gpu/infer-secondary/classifier-custom", test_classifier_custom);
  g_test_add_func ("/gpu/infer-secondary/classifier-custom-false", test_classifier_custom_false);
  g_test_add_func ("/gpu/infer-secondary/classifier-custom-no-desc", test_classifier_custom_no_desc);
  g_test_add_func ("/gpu/infer-secondary/classifier-custom-throw", test_classifier_custom_throw);
  g_test_add_func ("/gpu/infer-secondary/classifier-unknown-symbol", test_classifier_unknown_symbol);
  g_test_add_func ("/gpu/infer-secondary/classifier-tensor-meta", test_classifier_tensor_meta);
  g_test_add_func ("/gpu/infer-secondary/classifier-fresh-elements", test_classifier_fresh_elements);
  g_test_add_func ("/gpu/infer-secondary/cache-hits", test_cache_hits);
  g_test_add_func ("/gpu/infer-secondary/cache-growth", test_cache_growth);
  g_test_add_func ("/gpu/infer-secondary/cache-interval", test_cache_interval);
  g_test_add_func ("/gpu/infer-secondary/cache-detector-interval", test_cache_detector_interval);
  g_test_add_func ("/gpu/infer-secondary/cache-async-mode", test_cache_async_mode);
  g_test_add_func ("/gpu/infer-secondary/cache-events", test_cache_events);
  g_test_add_func ("/gpu/infer-secondary/cache-eviction", test_cache_eviction);
  g_test_add_func ("/gpu/infer-secondary/cache-filtered-kept", test_cache_filtered_kept);
  g_test_add_func ("/gpu/infer-secondary/cache-parser-failure", test_cache_parser_failure);
  g_test_add_func ("/gpu/infer-secondary/cache-chunk-failure", test_cache_chunk_failure);
  ret = g_test_run ();
  if (dev_alloc)
    gst_object_unref (dev_alloc);
  gst_deinit ();
  return ret;
}
