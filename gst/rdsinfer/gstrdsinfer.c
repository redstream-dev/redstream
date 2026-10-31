/* Redstream: rdsinfer, a primary or secondary detector on MIGraphX. Property names and config files follow
 * DeepStream's public nvinfer documentation.
 *
 * An in-place GstBaseTransform in passthrough mode on rdsstreammux batches: buffers go downstream unchanged, with
 * object meta (detectors) and tensor meta added to their RdsBatchMeta. Pixels are never written.
 * - start (READY -> PAUSED) loads the config file, applies the properties that were set (a property set explicitly
 *   wins over the file, also when set to its default value), and builds or loads the engine, synchronously. Any
 *   error fails the state change with a message naming the file (or the property), the key and the value.
 * - Per batch, one at a time on the element's own non-blocking stream: the frame list is read under the meta lock
 *   into inference units, each a source rect of a frame's slot with its letterbox:
 *   - primary mode: one per frame, the whole slot, into image frame_meta->batch_id; one chunk per batch;
 *   - secondary mode (process-mode=2): one per selected object with a non-empty crop (rds-infer-units.h), in frame
 *     list order, then object list order, in chunks of batch-size that continue across the frames of a batch, unit
 *     i of a chunk into image i (DeepStream 8.0 behaviour). No hard limit: the unit array is reused and grows.
 *   Units run in chunks of at most batch-size: without the lock, the preprocessing kernel writes each unit's image
 *   of the network input and the chunk's unused images are zeroed, the engine runs (always batch-size images), the
 *   outputs are copied to pinned host memory of the chunk's output set and the stream is synchronized once; the
 *   parser and the post-processing run on the CPU, and the meta lock is taken again to add the chunk's results,
 *   which go to frames that are still there (and in secondary mode objects: RdsMetaRef). Inferred frames get
 *   bInferDone in primary mode; secondary mode doesn't touch it (DeepStream 8.0 behaviour), and attaches to the
 *   object: its tensor meta, and a detector's children with the object as parent.
 * - interval N (primary mode): one batch is inferred, the next N pass untouched (bInferDone FALSE); the count
 *   restarts at start and after a flush. Secondary mode infers every batch (DeepStream 8.0 behaviour).
 * - Output buffers are refcounted sets (rds-infer-output.h) that tensor metas keep alive, also in demuxed copies:
 *   one set per chunk, so with tensor meta a secondary batch holds one set per batch-size objects while its metas
 *   live. The pool reuses free sets and keeps as many as the recent peak in use, at least
 *   RDS_INFER_OUTPUT_MAX_FREE (rds-infer-output.h): the rest are freed when a set is released (on whichever thread
 *   drops the last meta).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsinfer-private.h"
#include "rds-infer-preprocess.h"
#include "rds-infer-units.h"
#include "rds-object-style.h"

#include <rds/rds-infer-meta.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include "rds-infer-device.h"

GST_DEBUG_CATEGORY (gst_rds_infer_debug);
#define GST_CAT_DEFAULT gst_rds_infer_debug

/* Property defaults: the config file's defaults (ours, see rds-infer-config.c); they apply only when set. */
#define DEFAULT_UNIQUE_ID 1
#define DEFAULT_PROCESS_MODE 1
#define DEFAULT_INFER_ON_GIE_ID -1     /* every GIE, as DeepStream 8.0's */
#define DEFAULT_BATCH_SIZE 1
#define DEFAULT_INTERVAL 0
#define DEFAULT_GPU_ID 0

enum
{
  PROP_0,
  PROP_CONFIG_FILE_PATH,
  PROP_UNIQUE_ID,
  PROP_PROCESS_MODE,
  PROP_INFER_ON_GIE_ID,
  PROP_INFER_ON_CLASS_IDS,
  PROP_FILTER_OUT_CLASS_IDS,
  PROP_MODEL_ENGINE_FILE,
  PROP_BATCH_SIZE,
  PROP_INTERVAL,
  PROP_GPU_ID,
  PROP_OUTPUT_TENSOR_META,
  PROP_RAW_OUTPUT_FILE_WRITE,
  PROP_RAW_OUTPUT_GENERATED_CALLBACK,
  PROP_RAW_OUTPUT_GENERATED_USERDATA,
  PROP_OUTPUT_INSTANCE_MASK,
  PROP_INPUT_TENSOR_META,
  PROP_CROP_OBJECTS_TO_ROI_BOUNDARY,
  PROP_CLIP_OBJECT_OUTSIDE_ROI,
  N_PROPS
};

G_STATIC_ASSERT (N_PROPS <= 32);
#define PROP_BIT(p) (1u << (p))

#define RDS_INFER_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), format = (string) { NV12, RGBA, BGRA, RGBx, BGRx }, " \
  "width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_INFER_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_INFER_CAPS));

G_DEFINE_TYPE (GstRdsInfer, gst_rds_infer, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdsinfer, "rdsinfer", GST_RANK_NONE, GST_TYPE_RDS_INFER);

/* ---- configuration ---- */

static gboolean G_GNUC_PRINTF (5, 6)
property_error (GError ** error, gint code, const gchar * prop, const gchar * value, const gchar * fmt, ...)
{
  gchar *reason;
  va_list ap;

  va_start (ap, fmt);
  reason = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_INFER_CONFIG_ERROR, code, "element property %s: %s: %s", prop, value, reason);
  g_free (reason);
  return FALSE;
}

/* @s split at @sep into integers in [min, max] (whitespace around them allowed, empty items skipped) appended to @out
 * (gint64); an empty or NULL string gives an empty list. */
static gboolean
parse_id_list (const gchar * s, const gchar * sep, gint64 min, gint64 max, GArray * out)
{
  gchar **items;
  gboolean ok = TRUE;

  if (!s)
    return TRUE;
  items = g_strsplit (s, sep, -1);
  for (guint i = 0; ok && items[i]; i++) {
    gchar *item = g_strstrip (items[i]), *end = NULL;
    gint64 v;

    if (!*item)
      continue;
    errno = 0;
    v = g_ascii_strtoll (item, &end, 10);
    ok = errno == 0 && end && !*end && v >= min && v <= max;
    if (ok)
      g_array_append_val (out, v);
  }
  g_strfreev (items);
  return ok;
}

/* What secondary mode ignores, after the config warned about it: interval (DeepStream 8.0 behaviour) and the ROI
 * offsets, which postprocessing would apply from a crop's top and bottom edges (ours). */
static void
ignore_in_secondary_mode (RdsInferConfig * cfg)
{
  cfg->interval = 0;
  for (guint i = 0; i < cfg->num_detected_classes; i++) {
    cfg->class_attrs[i].roi_top_offset = 0;
    cfg->class_attrs[i].roi_bottom_offset = 0;
  }
}

/* What start would run with: the config file with the explicitly set properties applied and validated. Property
 * warnings go into cfg->warnings, after the config's warnings about keys the final mode doesn't use. */
static RdsInferConfig *
configure (GstRdsInfer * self, GError ** error)
{
  RdsInferConfig *cfg = NULL;
  /* at most one config key per property, and the terminating NULL */
  const gchar *overridden[N_PROPS + 1];
  guint n_over = 0;
  gchar *path, *engine_file, *on_class_ids, *filter_ids;
  guint32 set;
  guint unique_id, process_mode, batch_size, gpu_id;
  gint on_gie_id, interval;
  gboolean tensor_meta, raw_write, instance_mask, tensor_input, crop, clip;
  gpointer raw_cb;
  GArray *ids;

  GST_OBJECT_LOCK (self);
  path = g_strdup (self->config_file_path);
  engine_file = g_strdup (self->model_engine_file);
  on_class_ids = g_strdup (self->infer_on_class_ids);
  filter_ids = g_strdup (self->filter_out_class_ids);
  set = self->set_mask;
  unique_id = self->unique_id;
  process_mode = self->process_mode;
  on_gie_id = self->infer_on_gie_id;
  batch_size = self->batch_size;
  gpu_id = self->gpu_id;
  interval = self->interval;
  tensor_meta = self->output_tensor_meta;
  raw_write = self->raw_output_file_write;
  raw_cb = self->raw_output_generated_callback;
  instance_mask = self->output_instance_mask;
  tensor_input = self->input_tensor_meta;
  crop = self->crop_objects_to_roi_boundary;
  clip = self->clip_object_outside_roi;
  GST_OBJECT_UNLOCK (self);

  if (!path || !*path) {
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID,
        "element property config-file-path: (unset): rdsinfer needs a config file");
    goto out;
  }
  if (!(cfg = rds_infer_config_load (path, error)))
    goto out;

#define ADD_OVERRIDDEN(key) G_STMT_START { \
    g_assert (n_over < G_N_ELEMENTS (overridden) - 1); overridden[n_over++] = key; } G_STMT_END
#define OVERRIDE(prop, key, assign) G_STMT_START { \
    if (set & PROP_BIT (prop)) { assign; ADD_OVERRIDDEN (key); } } G_STMT_END
  OVERRIDE (PROP_UNIQUE_ID, "gie-unique-id", cfg->gie_unique_id = unique_id);
  OVERRIDE (PROP_PROCESS_MODE, "process-mode", cfg->process_mode = (gint) process_mode);
  OVERRIDE (PROP_INFER_ON_GIE_ID, "operate-on-gie-id", cfg->operate_on_gie_id = on_gie_id);
  OVERRIDE (PROP_BATCH_SIZE, "batch-size", cfg->batch_size = batch_size);
  OVERRIDE (PROP_INTERVAL, "interval", cfg->interval = (guint) interval);
  OVERRIDE (PROP_GPU_ID, "gpu-id", cfg->gpu_id = gpu_id);
  OVERRIDE (PROP_OUTPUT_TENSOR_META, "output-tensor-meta", cfg->output_tensor_meta = tensor_meta);
  OVERRIDE (PROP_MODEL_ENGINE_FILE, "model-engine-file", g_free (cfg->model_engine_file);
      cfg->model_engine_file = g_strdup (engine_file));
#undef OVERRIDE
  if (set & PROP_BIT (PROP_FILTER_OUT_CLASS_IDS)) {
    ids = g_array_new (FALSE, FALSE, sizeof (gint64));
    if (!parse_id_list (filter_ids, ";", 0, G_MAXUINT, ids)) {
      g_array_unref (ids);
      property_error (error, RDS_INFER_CONFIG_ERROR_INVALID, "filter-out-class-ids", filter_ids,
          "expected class ids separated by ';', e.g. 1;2");
      goto fail;
    }
    g_array_set_size (cfg->filter_out_class_ids, 0);
    for (guint i = 0; i < ids->len; i++) {
      guint id = (guint) g_array_index (ids, gint64, i);

      g_array_append_val (cfg->filter_out_class_ids, id);
    }
    g_array_unref (ids);
    ADD_OVERRIDDEN ("filter-out-class-ids");
  }
  if (set & PROP_BIT (PROP_INFER_ON_CLASS_IDS)) {
    ids = g_array_new (FALSE, FALSE, sizeof (gint64));
    if (!parse_id_list (on_class_ids, ":", -1, G_MAXINT, ids)) {
      g_array_unref (ids);
      property_error (error, RDS_INFER_CONFIG_ERROR_INVALID, "infer-on-class-ids", on_class_ids,
          "expected class ids separated by ':', e.g. 1:2:4");
      goto fail;
    }
    g_array_set_size (cfg->operate_on_class_ids, 0);
    for (guint i = 0; i < ids->len; i++) {
      gint id = (gint) g_array_index (ids, gint64, i);

      g_array_append_val (cfg->operate_on_class_ids, id);
    }
    rds_infer_config_normalize_class_ids (cfg->operate_on_class_ids);
    g_array_unref (ids);
    ADD_OVERRIDDEN ("operate-on-class-ids");
  }
#undef ADD_OVERRIDDEN
  if (instance_mask) {
    property_error (error, RDS_INFER_CONFIG_ERROR_UNSUPPORTED, "output-instance-mask", "1",
        "instance segmentation is not supported yet");
    goto fail;
  }
  if (tensor_input) {
    property_error (error, RDS_INFER_CONFIG_ERROR_UNSUPPORTED, "input-tensor-meta", "1",
        "preprocessed tensor input is not supported yet");
    goto fail;
  }
  overridden[n_over] = NULL;
  if (!rds_infer_config_validate (cfg, overridden, error) || !rds_infer_config_check_mode (cfg, overridden, error))
    goto fail;

  /* properties at their default value don't warn */
  if (cfg->process_mode != 2 && ((set & PROP_BIT (PROP_INFER_ON_GIE_ID) && on_gie_id != DEFAULT_INFER_ON_GIE_ID)
          || (set & PROP_BIT (PROP_INFER_ON_CLASS_IDS) && cfg->operate_on_class_ids->len > 0)))
    g_ptr_array_add (cfg->warnings, g_strdup ("element properties infer-on-gie-id and infer-on-class-ids: ignored: "
            "they apply to secondary mode (process-mode=2) only"));
  if (cfg->process_mode == 2 && set & PROP_BIT (PROP_INTERVAL) && interval != 0)
    g_ptr_array_add (cfg->warnings, g_strdup_printf ("element property interval: %d: ignored in secondary mode "
            "(process-mode=2)", interval));
  if (crop || clip)
    g_ptr_array_add (cfg->warnings, g_strdup ("element properties crop-objects-to-roi-boundary and "
            "clip-object-outside-roi: ignored: they apply to preprocessed-tensor ROIs, which are not supported yet"));
  /* start marks it posted (not this function: the test hook calls it too) */
  if ((raw_write || raw_cb) && !(self->warned & WARN_RAW_OUTPUT)) {
    g_ptr_array_add (cfg->warnings, g_strdup ("element properties raw-output-file-write and "
            "raw-output-generated-callback: ignored: raw output dumps are not implemented"));
  }
  /* after the warnings about them: the config start runs with, and the test hook reports */
  if (cfg->process_mode == 2)
    ignore_in_secondary_mode (cfg);
  goto out;

fail:
  g_clear_pointer (&cfg, rds_infer_config_free);
out:
  g_free (path);
  g_free (engine_file);
  g_free (on_class_ids);
  g_free (filter_ids);
  return cfg;
}

static gchar *
join_warnings (const GPtrArray * warnings)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < warnings->len; i++)
    g_string_append_printf (s, "%s%s", i ? "\n" : "", (const gchar *) g_ptr_array_index (warnings, i));
  return g_string_free (s, FALSE);
}

static gchar *
join_ids (const GArray * a, gboolean is_uint)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < a->len; i++) {
    gint64 v = is_uint ? (gint64) g_array_index (a, guint, i) : (gint64) g_array_index (a, gint, i);

    g_string_append_printf (s, "%s%" G_GINT64_FORMAT, i ? ";" : "", v);
  }
  return g_string_free (s, FALSE);
}

GstStructure *
rds_infer_test_get_config (GstElement * element, GError ** error)
{
  RdsInferConfig *cfg = configure (GST_RDS_INFER (element), error);
  GstStructure *s = NULL;
  gchar *filter, *on_class, *warnings;

  if (cfg) {
    filter = join_ids (cfg->filter_out_class_ids, TRUE);
    on_class = join_ids (cfg->operate_on_class_ids, FALSE);
    warnings = join_warnings (cfg->warnings);
    s = gst_structure_new ("rdsinfer-config",
        "batch-size", G_TYPE_UINT, cfg->batch_size,
        "interval", G_TYPE_UINT, cfg->interval,
        "gie-unique-id", G_TYPE_UINT, cfg->gie_unique_id,
        "process-mode", G_TYPE_INT, cfg->process_mode,
        "gpu-id", G_TYPE_UINT, cfg->gpu_id,
        "output-tensor-meta", G_TYPE_BOOLEAN, cfg->output_tensor_meta,
        "model-engine-file", G_TYPE_STRING, cfg->model_engine_file,
        "filter-out-class-ids", G_TYPE_STRING, filter,
        "operate-on-gie-id", G_TYPE_INT, cfg->operate_on_gie_id,
        "operate-on-class-ids", G_TYPE_STRING, on_class,
        "n-warnings", G_TYPE_UINT, cfg->warnings->len,
        "warnings", G_TYPE_STRING, warnings, NULL);
    g_free (filter);
    g_free (on_class);
    g_free (warnings);
    rds_infer_config_free (cfg);
  }
  return s;
}

/* ---- start / stop ---- */

static void
release_resources (GstRdsInfer * self)
{
  int prev = 0;
  gboolean switched = FALSE;

  if (self->stream || self->input || self->engine) {
    switched = rds_infer_device_enter (self->gpu, &prev);
    if (self->stream)
      (void) hipStreamSynchronize (self->stream);
  }
  /* the streaming thread has stopped (the base class stops after deactivating the pads), so no parser call or
   * engine run is still going */
  g_clear_pointer (&self->parser, rds_infer_parser_free);
  g_clear_pointer (&self->engine, rds_infer_engine_free);
  /* sets still referenced by metas stay alive, with the pool */
  g_clear_pointer (&self->outputs, rds_infer_output_pool_unref);
  if (self->input)
    (void) hipFree (self->input);
  self->input = NULL;
  if (self->stream)
    (void) hipStreamDestroy (self->stream);
  self->stream = NULL;
  if (switched)
    rds_infer_device_restore (prev);
  g_clear_pointer (&self->cfg, rds_infer_config_free);
  g_clear_pointer (&self->out_dims, g_free);
  g_clear_pointer (&self->out_names, g_free);
  g_clear_pointer (&self->layers, g_free);
  g_clear_pointer (&self->slot_used, g_free);
  g_clear_pointer (&self->image_used, g_free);
  self->n_outputs = 0;
}

static void
post_config_error (GstRdsInfer * self, GError * err)
{
  if (err->domain == RDS_INFER_CONFIG_ERROR && err->code == RDS_INFER_CONFIG_ERROR_FILE)
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("%s", err->message), (NULL));
  else
    GST_ELEMENT_ERROR (self, RESOURCE, SETTINGS, ("%s", err->message), (NULL));
}

/* Opens the engine and checks its input against the preprocessing settings; errors name the config key. */
static gboolean
open_engine (GstRdsInfer * self, GError ** error)
{
  RdsInferConfig *cfg = self->cfg;
  gboolean nhwc = cfg->network_input_order == 1;
  guint dims[3], d0, d1, d2, c, h, w;
  RdsInferEngineOptions opts;
  GError *err = NULL;
  GPtrArray *warnings;

  if (!rds_infer_config_get_engine_options (cfg, &opts, dims, error))
    return FALSE;
  if (!(self->engine = rds_infer_engine_open (&opts, &err))) {
    gchar *msg = rds_infer_config_format_engine_error (cfg, err);

    g_set_error_literal (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID, msg);
    g_free (msg);
    g_error_free (err);
    return FALSE;
  }
  /* tests/tools/rds_compile.sh matches these two messages. */
  if (rds_infer_engine_was_built (self->engine))
    GST_INFO_OBJECT (self, "built %s because %s", GST_STR_NULL (rds_infer_engine_get_program_path (self->engine)),
        rds_infer_engine_get_build_reason (self->engine));
  else
    GST_INFO_OBJECT (self, "loaded %s", rds_infer_engine_get_program_path (self->engine));
  warnings = rds_infer_engine_get_warnings (self->engine);
  for (guint i = 0; i < warnings->len; i++)
    GST_ELEMENT_WARNING (self, RESOURCE, SETTINGS, ("%s", (const gchar *) g_ptr_array_index (warnings, i)), (NULL));

  rds_infer_engine_get_input (self->engine, &d0, &d1, &d2);
  c = nhwc ? d2 : d0;
  h = nhwc ? d0 : d1;
  w = nhwc ? d1 : d2;
  self->channels = cfg->model_color_format == RDS_PREPROCESS_GRAY ? 1 : 3;
  if (c != self->channels) {
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID,
        "%s:model-color-format: %d: the model's input %s has %u channels (%ux%ux%u%s), this format needs %u",
        cfg->config_path, cfg->model_color_format, rds_infer_engine_get_input_name (self->engine), c, d0, d1, d2,
        nhwc ? ", NHWC" : "", self->channels);
    return FALSE;
  }
  self->net.width = w;
  self->net.height = h;
  self->net.channels = c;
  self->image_floats = (gsize) c * h * w;

  self->n_outputs = rds_infer_engine_get_n_outputs (self->engine);
  self->out_dims = g_new0 (RdsInferDims, self->n_outputs);
  self->out_names = g_new0 (const gchar *, self->n_outputs + 1);
  self->layers = g_new0 (RdsInferLayerInfo, self->n_outputs);
  for (guint i = 0; i < self->n_outputs; i++) {
    guint nd = 0, od[RDS_INFER_MAX_DIMS] = { 0 };
    RdsInferDims *d = &self->out_dims[i];

    rds_infer_engine_get_output (self->engine, i, &self->out_names[i], od, &nd);
    d->numDims = nd;
    /* The engine validated every dimension and the per-image count against guint, and the complete
     * batch against gsize, before exposing this output. Keep those counts for allocation and metadata. */
    memcpy (d->d, od, nd * sizeof (guint));
    d->numElements = (guint) rds_infer_engine_get_output_elements (self->engine, i);
    self->layers[i].dataType = RDS_INFER_DATA_FLOAT;
    self->layers[i].inferDims = *d;
    self->layers[i].bindingIndex = (int) i;
    self->layers[i].layerName = self->out_names[i];
  }
  return TRUE;
}

static gboolean
open_parser (GstRdsInfer * self, GError ** error)
{
  RdsInferConfig *cfg = self->cfg;
  guint n = cfg->num_detected_classes;
  gfloat *pre = g_new (gfloat, n), *post = g_new (gfloat, n);
  GError *err = NULL;
  gboolean ok;

  if (cfg->custom_lib_path)
    self->parser = rds_infer_parser_open (cfg->custom_lib_path, cfg->parse_bbox_func_name, &err);
  else
    self->parser = rds_infer_parser_open_builtin (cfg->parse_bbox_func_name, &err);
  if (!self->parser) {
    if (cfg->custom_lib_path)
      g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID, "%s:custom-lib-path: %s",
          cfg->config_path, err->message);
    else
      g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID, "%s:parse-bbox-func-name: %s: %s",
          cfg->config_path, cfg->parse_bbox_func_name, err->message);
    g_error_free (err);
    g_free (pre);
    g_free (post);
    return FALSE;
  }
  for (guint i = 0; i < n; i++) {
    pre[i] = cfg->class_attrs[i].pre_cluster_threshold;
    post[i] = cfg->class_attrs[i].post_cluster_threshold;
  }
  rds_infer_parser_set_detection_params (self->parser, n, pre, post);
  g_free (pre);
  g_free (post);
  ok = rds_infer_parser_check_layers (self->parser, self->layers, self->n_outputs, &err);
  if (!ok) {
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID, "%s:parse-bbox-func-name: %s: %s",
        cfg->config_path, cfg->parse_bbox_func_name, err->message);
    g_error_free (err);
  }
  return ok;
}

static gboolean
gst_rds_infer_start (GstBaseTransform * trans)
{
  GstRdsInfer *self = GST_RDS_INFER (trans);
  GError *err = NULL;
  int prev = 0;
  hipError_t herr;
  gboolean ok;

  self->batch_count = 0;
  self->unit_count = 0;
  if (!(self->cfg = configure (self, &err))) {
    post_config_error (self, err);
    g_error_free (err);
    return FALSE;
  }
  /* the config accepts classifiers (in secondary mode, which check_mode made sure of); the element doesn't run them
   * yet. network-type comes from the file only. */
  if (self->cfg->network_type == 1) {
    g_set_error (&err, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_UNSUPPORTED,
        "%s:network-type: 1: classifiers are not implemented yet", self->cfg->config_path);
    post_config_error (self, err);
    g_error_free (err);
    release_resources (self);
    return FALSE;
  }
  for (guint i = 0; i < self->cfg->warnings->len; i++)
    GST_ELEMENT_WARNING (self, RESOURCE, SETTINGS, ("%s", (const gchar *) g_ptr_array_index (self->cfg->warnings,
                i)), (NULL));
  self->secondary = self->cfg->process_mode == 2;
  GST_OBJECT_LOCK (self);
  if (self->raw_output_file_write || self->raw_output_generated_callback)
    self->warned |= WARN_RAW_OUTPUT;
  GST_OBJECT_UNLOCK (self);

  self->gpu = self->cfg->gpu_id;
  if (!rds_infer_device_enter (self->gpu, &prev)) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("element property gpu-id: %u: no such GPU", self->gpu), (NULL));
    release_resources (self);
    return FALSE;
  }
  herr = hipStreamCreateWithFlags (&self->stream, hipStreamNonBlocking);
  if (herr != hipSuccess) {
    self->stream = NULL;
    rds_infer_device_restore (prev);
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot create a HIP stream on GPU %u", self->gpu),
        ("hipStreamCreateWithFlags: %s", hipGetErrorString (herr)));
    release_resources (self);
    return FALSE;
  }
  ok = open_engine (self, &err) && (self->cfg->network_type != 0 || open_parser (self, &err));
  if (ok) {
    herr = hipMalloc ((void **) &self->input, self->cfg->batch_size * self->image_floats * sizeof (gfloat));
    if (herr != hipSuccess) {
      self->input = NULL;
      g_set_error (&err, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID,
          "can't allocate the network input (%u x %" G_GSIZE_FORMAT " floats): %s", self->cfg->batch_size,
          self->image_floats, hipGetErrorString (herr));
      ok = FALSE;
    }
  }
  rds_infer_device_restore (prev);
  if (!ok) {
    post_config_error (self, err);
    g_error_free (err);
    release_resources (self);
    return FALSE;
  }
  self->slot_used = g_new0 (gboolean, self->cfg->batch_size);
  self->image_used = g_new0 (gboolean, self->cfg->batch_size);
  self->outputs = rds_infer_output_pool_new (self->gpu, self->cfg->batch_size, self->n_outputs, self->out_dims,
      self->out_names);
  GST_INFO_OBJECT (self, "network %ux%ux%u, batch %u, %u outputs, interval %u", self->net.channels,
      self->net.height, self->net.width, self->cfg->batch_size, self->n_outputs, self->cfg->interval);
  return TRUE;
}

static gboolean
gst_rds_infer_stop (GstBaseTransform * trans)
{
  release_resources (GST_RDS_INFER (trans));
  return TRUE;
}

static gboolean
gst_rds_infer_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP)
    GST_RDS_INFER (trans)->batch_count = 0;
  return GST_BASE_TRANSFORM_CLASS (gst_rds_infer_parent_class)->sink_event (trans, event);
}

/* ---- per batch ---- */

#define HIP_OR_FAIL(call, what) G_STMT_START {                                              \
    hipError_t _e = (call);                                                                  \
    if (_e != hipSuccess) {                                                                  \
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("GPU error in %s", what),                  \
          ("%s: %s", #call, hipGetErrorString (_e)));                                        \
      goto fail;                                                                             \
    }                                                                                        \
  } G_STMT_END

void
gst_rds_infer_warn_once (GstRdsInfer * self, WarnOnce what, const gchar * msg)
{
  if (!(self->warned & what)) {
    self->warned |= what;
    GST_ELEMENT_WARNING (self, STREAM, DECODE, ("%s", msg), ("further occurrences are logged at debug level"));
  } else {
    GST_DEBUG_OBJECT (self, "%s", msg);
  }
}

/* Primary mode: reads the frame list (under the meta lock) into self->units, one unit per frame (the whole slot, as
 * image batch_id), and self->slot_used; FALSE with an element error on a bad batch. */
static gboolean
snapshot_frames (GstRdsInfer * self, RdsBatchMeta * bm, const RdsBufSurface * surf)
{
  RdsInferConfig *cfg = self->cfg;
  guint batch = cfg->batch_size;
  gboolean ok = TRUE;

  g_array_set_size (self->units, 0);
  memset (self->slot_used, 0, batch * sizeof (gboolean));
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    InferUnit unit = {.frame = rds_meta_ref (fm),.source_id = fm->source_id,.slot = fm->batch_id,
      .image = fm->batch_id };

    if (fm->batch_id >= batch || fm->batch_id >= surf->batchSize) {
      ok = FALSE;
      GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("Frame in batch slot %u, but batch-size is %u", fm->batch_id,
              batch), ("the batch surface has %u slots; set rdsinfer's batch-size to the mux's", surf->batchSize));
      break;
    }
    if (self->slot_used[fm->batch_id]) {
      GST_DEBUG_OBJECT (self, "second frame in slot %u ignored", fm->batch_id);
      continue;
    }
    self->slot_used[fm->batch_id] = TRUE;
    unit.src.width = surf->surfaceList[fm->batch_id].width;
    unit.src.height = surf->surfaceList[fm->batch_id].height;
    rds_infer_letterbox (self->net.width, self->net.height, unit.src.width, unit.src.height,
        cfg->maintain_aspect_ratio, cfg->symmetric_padding, &unit.lb);
    g_array_append_val (self->units, unit);
  }
  rds_release_meta_lock (bm);
  return ok;
}

/* Parser and post-processing of the units @first .. @first + @n - 1 (a chunk), on the host outputs of its @set; in
 * secondary mode per crop (thresholds and NMS within the crop), then to frame pixels. */
static void
parse_units (GstRdsInfer * self, const RdsBufSurface * surf, RdsInferOutputSet * set, guint first, guint n)
{
  RdsInferConfig *cfg = self->cfg;

  g_array_set_size (self->dets, 0);
  for (guint u = first; u < first + n; u++) {
    InferUnit *unit = &g_array_index (self->units, InferUnit, u);
    GError *err = NULL;
    RdsInferPostFlags flags;

    unit->det_start = self->dets->len;
    unit->n_dets = 0;
    for (guint i = 0; i < self->n_outputs; i++) {
      gsize offset = (gsize) unit->image * self->out_dims[i].numElements;

      self->layers[i].buffer = set->host[i] + offset;
      self->layers[i].deviceBuffer = set->dev[i] + offset;
    }
    g_array_set_size (self->objects, 0);
    /* only this unit loses its objects (ours in secondary mode, where DeepStream 8.0 crashes) */
    if (!rds_infer_parser_parse (self->parser, self->layers, self->n_outputs, &self->net, self->objects, &err)) {
      gchar *msg = g_strdup_printf ("Parser %s failed on %s of source %u, which gets no %s: %s",
          rds_infer_parser_get_func_name (self->parser), self->secondary ? "an object" : "a frame", unit->source_id,
          self->secondary ? "children" : "objects", err->message);

      gst_rds_infer_warn_once (self, WARN_PARSE, msg);
      g_free (msg);
      g_error_free (err);
      continue;
    }
    flags = rds_infer_postprocess (cfg, &unit->lb, (const RdsInferObjectDetectionInfo *) self->objects->data,
        self->objects->len, self->scratch, self->dets);
    if (flags & RDS_INFER_POST_INVALID)
      gst_rds_infer_warn_once (self, WARN_POST_INVALID, "The parser returned invalid objects (a class id >= "
          "num-detected-classes, a coordinate or confidence that is not finite, or an empty box); they were dropped");
    if (flags & RDS_INFER_POST_ROTATED)
      gst_rds_infer_warn_once (self, WARN_POST_ROTATED, "The parser returned objects with a rotation_angle; "
          "oriented boxes are not supported, the angle is ignored");
    if (self->secondary)
      gst_rds_infer_map_children (self, unit, &surf->surfaceList[unit->slot]);
    unit->n_dets = self->dets->len - unit->det_start;
  }
}

/* @parent: the object a secondary detector cropped, NULL in primary mode. The child is appended (ours, as everywhere
 * in our metadata library; DeepStream 8.0 prepends). */
void
gst_rds_infer_add_object (GstRdsInfer * self, RdsBatchMeta * bm, RdsFrameMeta * fm, const RdsInferDetection * d,
    RdsObjectMeta * parent)
{
  RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);
  const gchar *label = self->cfg->labels[d->class_id];
  RdsOSD_TextParams *tp = &om->text_params;

  om->unique_component_id = (gint) self->cfg->gie_unique_id;
  om->class_id = (gint) d->class_id;
  om->confidence = d->confidence;
  om->object_id = RDS_UNTRACKED_OBJECT_ID;
  om->detector_bbox_info.org_bbox_coords.left = d->box.left;
  om->detector_bbox_info.org_bbox_coords.top = d->box.top;
  om->detector_bbox_info.org_bbox_coords.width = d->box.width;
  om->detector_bbox_info.org_bbox_coords.height = d->box.height;
  om->rect_params.left = d->rect.left;
  om->rect_params.top = d->rect.top;
  om->rect_params.width = d->rect.width;
  om->rect_params.height = d->rect.height;
  rds_object_meta_set_drawing_defaults (om);
  g_strlcpy (om->obj_label, label, sizeof (om->obj_label));
  tp->display_text = g_strdup (label);
  rds_add_obj_meta_to_frame (fm, om, parent);
}

/* A tensor meta for image @image of @set, in a user meta from @bm's pool. */
RdsUserMeta *
gst_rds_infer_new_tensor_user_meta (GstRdsInfer * self, RdsBatchMeta * bm, RdsInferOutputSet * set, guint image)
{
  RdsInferConfig *cfg = self->cfg;
  RdsUserMeta *um = rds_acquire_user_meta_from_pool (bm);

  um->user_meta_data = rds_infer_tensor_meta_new (set, image, cfg->gie_unique_id, &self->net,
      cfg->maintain_aspect_ratio, cfg->symmetric_padding);
  um->base_meta.meta_type = RDS_INFER_TENSOR_OUTPUT_META;
  um->base_meta.copy_func = rds_infer_tensor_meta_copy;
  um->base_meta.release_func = rds_infer_tensor_meta_release;
  return um;
}

/* The results of the units @first .. @first + @n - 1 (a chunk); their tensor metas hold a ref on the chunk's @set
 * and name their image in it. Primary mode: the unit is the frame: objects, tensor meta and bInferDone per frame. */
static void
attach_frame_results (GstRdsInfer * self, RdsBatchMeta * bm, RdsInferOutputSet * set, guint first, guint n)
{
  RdsInferConfig *cfg = self->cfg;
  gboolean tensor_meta = cfg->output_tensor_meta || cfg->network_type == 100;

  rds_acquire_meta_lock (bm);
  for (guint u = first; u < first + n; u++) {
    InferUnit *unit = &g_array_index (self->units, InferUnit, u);
    RdsFrameMeta *fm = rds_meta_ref_resolve (bm->frame_meta_list, &unit->frame);

    /* A frame removed while the lock was released gets nothing, even when the pool reused its address. */
    if (!fm)
      continue;
    if (cfg->network_type == 0) {
      for (guint i = 0; i < unit->n_dets; i++)
        gst_rds_infer_add_object (self, bm, fm, &g_array_index (self->dets, RdsInferDetection, unit->det_start + i),
            NULL);
      GST_DEBUG_OBJECT (self, "source %u frame %d: %u objects", fm->source_id, fm->frame_num,
          unit->n_dets);
    }
    if (tensor_meta)
      rds_add_user_meta_to_frame (fm, gst_rds_infer_new_tensor_user_meta (self, bm, set, unit->image));
    fm->bInferDone = TRUE;
  }
  rds_release_meta_lock (bm);
}

/* Infers the units @first .. @first + @n - 1 (n <= batch-size, distinct images below batch-size) as one engine run
 * and attaches their results; FALSE with an element error. Each chunk has its own output set from the pool; the
 * chunk's tensor metas keep it alive after this returns. */
static gboolean
infer_chunk (GstRdsInfer * self, RdsBatchMeta * bm, const RdsBufSurface * surf, guint first, guint n)
{
  RdsInferConfig *cfg = self->cfg;
  RdsInferOutputSet *set = NULL;
  GError *err = NULL;

  GST_DEBUG_OBJECT (self, "chunk of %u units (%u..%u of %u)", n, first, first + n - 1, self->units->len);
  /* 1. preprocessing: each unit's source rect into its image, the unused images zeroed */
  memset (self->image_used, 0, cfg->batch_size * sizeof (gboolean));
  for (guint u = first; u < first + n; u++) {
    InferUnit *unit = &g_array_index (self->units, InferUnit, u);
    const RdsBufSurfaceParams *sp = &surf->surfaceList[unit->slot];
    RdsPreprocessJob pj = { 0 };

    g_assert (unit->image < cfg->batch_size && !self->image_used[unit->image]);
    self->image_used[unit->image] = TRUE;
    (void) rds_infer_preprocess_job_set_source (&pj, sp, &unit->src, &unit->lb);
    pj.channels = self->channels;
    pj.color = cfg->model_color_format;
    pj.nhwc = cfg->network_input_order == 1;
    pj.bilinear = rds_transform_filter_is_bilinear ((RdsBufSurfTransform_Inter) cfg->scaling_filter);
    pj.scale = cfg->net_scale_factor;
    for (guint c = 0; c < 3; c++)
      pj.offsets[c] = c < cfg->n_offsets ? cfg->offsets[c] : 0.0f;
    if (rds_infer_preprocess_launch (&pj, self->input, unit->image, self->stream) != 0) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Preprocessing failed"),
          ("slot %u: %ux%u, format %d, rect %u,%u %ux%u", unit->slot, sp->width, sp->height, sp->colorFormat,
              unit->src.left, unit->src.top, unit->src.width, unit->src.height));
      goto fail;
    }
  }
  for (guint image = 0; image < cfg->batch_size; image++)
    if (!self->image_used[image])
      HIP_OR_FAIL (hipMemsetAsync (self->input + image * self->image_floats, 0,
              self->image_floats * sizeof (gfloat), self->stream), "zeroing unused images");

  /* 2. inference and the copy to the host, one sync */
  if (!(set = rds_infer_output_pool_acquire (self->outputs, &err))) {
    GST_ELEMENT_ERROR (self, RESOURCE, NO_SPACE_LEFT, ("%s", err->message), (NULL));
    g_error_free (err);
    goto fail;
  }
  if (!rds_infer_engine_run_async (self->engine, self->input, set->dev, self->stream, &err)) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Inference failed"), ("%s", err->message));
    g_error_free (err);
    goto fail;
  }
  for (guint i = 0; i < self->n_outputs; i++)
    HIP_OR_FAIL (hipMemcpyAsync (set->host[i], set->dev[i],
            (gsize) cfg->batch_size * self->out_dims[i].numElements * sizeof (gfloat), hipMemcpyDeviceToHost,
            self->stream), "copying the outputs");
  HIP_OR_FAIL (hipStreamSynchronize (self->stream), "inference");

  /* 3. parsing on the CPU, then the metadata under the lock */
  if (cfg->network_type == 0)
    parse_units (self, surf, set, first, n);
  if (self->secondary)
    gst_rds_infer_attach_object_results (self, bm, set, first, n);
  else
    attach_frame_results (self, bm, set, first, n);
  self->unit_count += n;
  rds_infer_output_set_unref (set);
  return TRUE;

fail:
  /* work already queued may still read the batch or write the set: wait for it before either goes */
  (void) hipStreamSynchronize (self->stream);
  if (set)
    rds_infer_output_set_unref (set);
  return FALSE;
}

static GstFlowReturn
gst_rds_infer_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRdsInfer *self = GST_RDS_INFER (trans);
  RdsInferConfig *cfg = self->cfg;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsBufSurface *surf = rds_buffer_get_surface (buf);
  gint64 t0 = 0;
  gboolean ok;

  if (!bm || !surf) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("rdsinfer needs batches from rdsstreammux"),
        ("the buffer has no %s", !bm ? "RdsBatchMeta" : "memory:RDS surface"));
    return GST_FLOW_ERROR;
  }
  if (self->batch_count++ % (cfg->interval + 1) != 0) {
    GST_LOG_OBJECT (self, "batch skipped (interval %u)", cfg->interval);
    return GST_FLOW_OK;
  }
  if (surf->memType == RDS_BUF_MEM_SYSTEM || surf->gpuId != self->gpu) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("rdsinfer needs batches in GPU memory on its gpu-id (%u)", self->gpu),
        ("memory type %d on GPU %u", surf->memType, surf->gpuId));
    return GST_FLOW_ERROR;
  }
  ok = self->secondary ? gst_rds_infer_snapshot_objects (self, bm, surf) : snapshot_frames (self, bm, surf);
  if (!ok)
    return GST_FLOW_ERROR;
  if (self->units->len == 0) {
    GST_LOG_OBJECT (self, "batch without %s", self->secondary ? "objects to infer on" : "frames");
    return GST_FLOW_OK;
  }
  if (gst_debug_category_get_threshold (GST_CAT_DEFAULT) >= GST_LEVEL_DEBUG)
    t0 = g_get_monotonic_time ();

  /* Set on every batch and not restored: the streaming thread only runs our GPU work while it is in here, and
   * whatever ran on it before may have left another device current. */
  if (hipSetDevice ((int) self->gpu) != hipSuccess) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("GPU %u is not available", self->gpu), (NULL));
    return GST_FLOW_ERROR;
  }
  /* primary mode has at most batch-size units (one per slot): one chunk; secondary mode never chunks across
   * buffers (DeepStream 8.0 behaviour) */
  for (guint first = 0; first < self->units->len; first += cfg->batch_size)
    if (!infer_chunk (self, bm, surf, first, MIN (cfg->batch_size, self->units->len - first)))
      return GST_FLOW_ERROR;
  if (t0)
    GST_DEBUG_OBJECT (self, "batch of %u units: %.3f ms", self->units->len, (g_get_monotonic_time () - t0) / 1000.0);
  return GST_FLOW_OK;
}

/* ---- test hooks ---- */

gboolean
rds_infer_test_get_output_sets (GstElement * element, guint * n_sets, guint * n_free)
{
  GstRdsInfer *self = GST_RDS_INFER (element);

  if (!self->outputs)
    return FALSE;
  rds_infer_output_pool_get_counts (self->outputs, n_sets, n_free);
  return TRUE;
}

gboolean
rds_infer_test_get_output_set_totals (GstElement * element, guint64 * allocated, guint64 * freed)
{
  GstRdsInfer *self = GST_RDS_INFER (element);

  if (!self->outputs)
    return FALSE;
  rds_infer_output_pool_get_totals (self->outputs, allocated, freed);
  return TRUE;
}

guint64
rds_infer_test_get_unit_count (GstElement * element)
{
  return GST_RDS_INFER (element)->unit_count;
}

gboolean
rds_infer_test_read_input (GstElement * element, gfloat * dst, gsize n_floats)
{
  GstRdsInfer *self = GST_RDS_INFER (element);
  int prev = 0;
  gboolean ok;

  if (!self->input || n_floats > self->cfg->batch_size * self->image_floats)
    return FALSE;
  if (!rds_infer_device_enter (self->gpu, &prev))
    return FALSE;
  ok = hipMemcpy (dst, self->input, n_floats * sizeof (gfloat), hipMemcpyDeviceToHost) == hipSuccess;
  rds_infer_device_restore (prev);
  return ok;
}

/* ---- properties ---- */

static void
gst_rds_infer_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsInfer *self = GST_RDS_INFER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_CONFIG_FILE_PATH:
      g_free (self->config_file_path);
      self->config_file_path = g_value_dup_string (value);
      break;
    case PROP_UNIQUE_ID:
      self->unique_id = g_value_get_uint (value);
      break;
    case PROP_PROCESS_MODE:
      self->process_mode = g_value_get_uint (value);
      break;
    case PROP_INFER_ON_GIE_ID:
      self->infer_on_gie_id = g_value_get_int (value);
      break;
    case PROP_INFER_ON_CLASS_IDS:
      g_free (self->infer_on_class_ids);
      self->infer_on_class_ids = g_value_dup_string (value);
      break;
    case PROP_FILTER_OUT_CLASS_IDS:
      g_free (self->filter_out_class_ids);
      self->filter_out_class_ids = g_value_dup_string (value);
      break;
    case PROP_MODEL_ENGINE_FILE:
      g_free (self->model_engine_file);
      self->model_engine_file = g_value_dup_string (value);
      break;
    case PROP_BATCH_SIZE:
      self->batch_size = g_value_get_uint (value);
      break;
    case PROP_INTERVAL:
      self->interval = g_value_get_int (value);
      break;
    case PROP_GPU_ID:
      self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_OUTPUT_TENSOR_META:
      self->output_tensor_meta = g_value_get_boolean (value);
      break;
    case PROP_RAW_OUTPUT_FILE_WRITE:
      self->raw_output_file_write = g_value_get_boolean (value);
      break;
    case PROP_RAW_OUTPUT_GENERATED_CALLBACK:
      self->raw_output_generated_callback = g_value_get_pointer (value);
      break;
    case PROP_RAW_OUTPUT_GENERATED_USERDATA:
      self->raw_output_generated_userdata = g_value_get_pointer (value);
      break;
    case PROP_OUTPUT_INSTANCE_MASK:
      self->output_instance_mask = g_value_get_boolean (value);
      break;
    case PROP_INPUT_TENSOR_META:
      self->input_tensor_meta = g_value_get_boolean (value);
      break;
    case PROP_CROP_OBJECTS_TO_ROI_BOUNDARY:
      self->crop_objects_to_roi_boundary = g_value_get_boolean (value);
      break;
    case PROP_CLIP_OBJECT_OUTSIDE_ROI:
      self->clip_object_outside_roi = g_value_get_boolean (value);
      break;
    default:
      GST_OBJECT_UNLOCK (self);
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      return;
  }
  self->set_mask |= PROP_BIT (prop_id);
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_infer_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsInfer *self = GST_RDS_INFER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_CONFIG_FILE_PATH:
      g_value_set_string (value, self->config_file_path);
      break;
    case PROP_UNIQUE_ID:
      g_value_set_uint (value, self->unique_id);
      break;
    case PROP_PROCESS_MODE:
      g_value_set_uint (value, self->process_mode);
      break;
    case PROP_INFER_ON_GIE_ID:
      g_value_set_int (value, self->infer_on_gie_id);
      break;
    case PROP_INFER_ON_CLASS_IDS:
      g_value_set_string (value, self->infer_on_class_ids);
      break;
    case PROP_FILTER_OUT_CLASS_IDS:
      g_value_set_string (value, self->filter_out_class_ids);
      break;
    case PROP_MODEL_ENGINE_FILE:
      g_value_set_string (value, self->model_engine_file);
      break;
    case PROP_BATCH_SIZE:
      g_value_set_uint (value, self->batch_size);
      break;
    case PROP_INTERVAL:
      g_value_set_int (value, self->interval);
      break;
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_OUTPUT_TENSOR_META:
      g_value_set_boolean (value, self->output_tensor_meta);
      break;
    case PROP_RAW_OUTPUT_FILE_WRITE:
      g_value_set_boolean (value, self->raw_output_file_write);
      break;
    case PROP_RAW_OUTPUT_GENERATED_CALLBACK:
      g_value_set_pointer (value, self->raw_output_generated_callback);
      break;
    case PROP_RAW_OUTPUT_GENERATED_USERDATA:
      g_value_set_pointer (value, self->raw_output_generated_userdata);
      break;
    case PROP_OUTPUT_INSTANCE_MASK:
      g_value_set_boolean (value, self->output_instance_mask);
      break;
    case PROP_INPUT_TENSOR_META:
      g_value_set_boolean (value, self->input_tensor_meta);
      break;
    case PROP_CROP_OBJECTS_TO_ROI_BOUNDARY:
      g_value_set_boolean (value, self->crop_objects_to_roi_boundary);
      break;
    case PROP_CLIP_OBJECT_OUTSIDE_ROI:
      g_value_set_boolean (value, self->clip_object_outside_roi);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_infer_finalize (GObject * object)
{
  GstRdsInfer *self = GST_RDS_INFER (object);

  release_resources (self);
  g_free (self->config_file_path);
  g_free (self->infer_on_class_ids);
  g_free (self->filter_out_class_ids);
  g_free (self->model_engine_file);
  g_array_unref (self->units);
  g_array_unref (self->objects);
  g_array_unref (self->dets);
  g_byte_array_unref (self->scratch);
  G_OBJECT_CLASS (gst_rds_infer_parent_class)->finalize (object);
}

#define PFLAGS (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)

static void
gst_rds_infer_class_init (GstRdsInferClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_rds_infer_debug, "rdsinfer", 0, "Redstream MIGraphX inference");

  gobject_class->set_property = gst_rds_infer_set_property;
  gobject_class->get_property = gst_rds_infer_get_property;
  gobject_class->finalize = gst_rds_infer_finalize;

  /* Every property set explicitly overrides the config file, even when set to its default value. Ranges are the
   * config keys' (rds-infer-config.c), except unique-id, which starts at 0 as documented for the property. */
  g_object_class_install_property (gobject_class, PROP_CONFIG_FILE_PATH,
      g_param_spec_string ("config-file-path", "Config file path",
          "Path to the inference configuration file (nvinfer-style key file or YAML)", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_UNIQUE_ID,
      g_param_spec_uint ("unique-id", "Unique ID",
          "ID of this element in the metadata it adds (overrides gie-unique-id)", 0, G_MAXUINT,
          DEFAULT_UNIQUE_ID, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_PROCESS_MODE,
      g_param_spec_uint ("process-mode", "Process mode",
          "1: primary (whole frames), 2: secondary (the objects upstream elements found; classifiers are not "
          "implemented yet and fail at start)", 1, 2,
          DEFAULT_PROCESS_MODE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INFER_ON_GIE_ID,
      g_param_spec_int ("infer-on-gie-id", "Infer on GIE ID",
          "Secondary mode: infer on the objects of this unique-id, -1 for every one (overrides operate-on-gie-id; "
          "ignored in primary mode)", -1, G_MAXINT, DEFAULT_INFER_ON_GIE_ID, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INFER_ON_CLASS_IDS,
      g_param_spec_string ("infer-on-class-ids", "Infer on class IDs",
          "Secondary mode: infer on objects of these classes, separated by ':' (e.g. 1:2:4; -1: every class; "
          "overrides operate-on-class-ids; ignored in primary mode)", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_FILTER_OUT_CLASS_IDS,
      g_param_spec_string ("filter-out-class-ids", "Filter out class IDs",
          "Drop detections of these classes, separated by ';' (e.g. 1;2)", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_MODEL_ENGINE_FILE,
      g_param_spec_string ("model-engine-file", "Model engine file",
          "Compiled MIGraphX program (.mxr) to load if it matches the model; used as given, not relative to the "
          "config file", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_BATCH_SIZE,
      g_param_spec_uint ("batch-size", "Batch size", "Frames (primary mode) or objects (secondary mode) inferred "
          "together", 1, RDS_INFER_MAX_BATCH_SIZE,
          DEFAULT_BATCH_SIZE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INTERVAL,
      g_param_spec_int ("interval", "Interval", "Batches skipped after each inferred batch", 0,
          RDS_INFER_MAX_INTERVAL, DEFAULT_INTERVAL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU ID", "GPU for preprocessing and inference", 0, G_MAXUINT, DEFAULT_GPU_ID,
          PFLAGS));
  g_object_class_install_property (gobject_class, PROP_OUTPUT_TENSOR_META,
      g_param_spec_boolean ("output-tensor-meta", "Output tensor meta",
          "Attach the output tensors to each frame (primary mode) or object (secondary mode) as "
          "RdsInferTensorMeta user meta", FALSE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_RAW_OUTPUT_FILE_WRITE,
      g_param_spec_boolean ("raw-output-file-write", "Raw output file write",
          "Not implemented: accepted with a warning", FALSE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_RAW_OUTPUT_GENERATED_CALLBACK,
      g_param_spec_pointer ("raw-output-generated-callback", "Raw output generated callback",
          "Not implemented: accepted with a warning", PFLAGS));
  g_object_class_install_property (gobject_class, PROP_RAW_OUTPUT_GENERATED_USERDATA,
      g_param_spec_pointer ("raw-output-generated-userdata", "Raw output generated user data",
          "User data for raw-output-generated-callback (not implemented)", PFLAGS));
  g_object_class_install_property (gobject_class, PROP_OUTPUT_INSTANCE_MASK,
      g_param_spec_boolean ("output-instance-mask", "Output instance mask",
          "Instance segmentation masks (not supported yet: TRUE fails at start)", FALSE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INPUT_TENSOR_META,
      g_param_spec_boolean ("input-tensor-meta", "Input tensor meta",
          "Use preprocessed tensors from upstream (not supported yet: TRUE fails at start)", FALSE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_CROP_OBJECTS_TO_ROI_BOUNDARY,
      g_param_spec_boolean ("crop-objects-to-roi-boundary", "Crop objects to ROI boundary",
          "Preprocessed-tensor ROIs only (not supported yet: ignored with a warning)", FALSE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_CLIP_OBJECT_OUTSIDE_ROI,
      g_param_spec_boolean ("clip-object-outside-roi", "Clip objects outside ROI",
          "Preprocessed-tensor ROIs only (not supported yet: ignored with a warning)", FALSE, PFLAGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream inference", "Filter/Analyzer/Video",
      "Runs a MIGraphX model on memory:RDS batches and attaches the results as metadata", "Redstream contributors");

  trans_class->start = GST_DEBUG_FUNCPTR (gst_rds_infer_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_rds_infer_stop);
  trans_class->sink_event = GST_DEBUG_FUNCPTR (gst_rds_infer_sink_event);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_rds_infer_transform_ip);
  trans_class->passthrough_on_same_caps = TRUE;
  trans_class->transform_ip_on_passthrough = TRUE;
}

static void
gst_rds_infer_init (GstRdsInfer * self)
{
  self->unique_id = DEFAULT_UNIQUE_ID;
  self->process_mode = DEFAULT_PROCESS_MODE;
  self->infer_on_gie_id = DEFAULT_INFER_ON_GIE_ID;
  self->batch_size = DEFAULT_BATCH_SIZE;
  self->interval = DEFAULT_INTERVAL;
  self->gpu_id = DEFAULT_GPU_ID;
  self->units = g_array_new (FALSE, FALSE, sizeof (InferUnit));
  self->objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  self->dets = g_array_new (FALSE, FALSE, sizeof (RdsInferDetection));
  self->scratch = g_byte_array_new ();
}
