/* Redstream: rdsinfer, a primary detector on MIGraphX. Property names and config files follow DeepStream's public
 * nvinfer documentation.
 *
 * An in-place GstBaseTransform in passthrough mode on rdsstreammux batches: buffers go downstream unchanged, with
 * object meta (detectors) and tensor meta added to their RdsBatchMeta. Pixels are never written.
 * - start (READY -> PAUSED) loads the config file, applies the properties that were set (a property set explicitly
 *   wins over the file, also when set to its default value), and builds or loads the engine, synchronously. Any
 *   error fails the state change with a message naming the file (or the property), the key and the value.
 * - Per batch, one at a time on the element's own non-blocking stream: the frame list is read under the meta lock,
 *   then, without the lock, the preprocessing kernel writes each frame's slot (frame_meta->batch_id) of the network
 *   input and unfilled slots are zeroed, the engine runs, the outputs are copied to pinned host memory and the
 *   stream is synchronized once; the parser and the post-processing run on the CPU, and the meta lock is taken again
 *   to add the objects and tensor meta. Inferred frames get bInferDone.
 * - interval N: one batch is inferred, the next N pass untouched (bInferDone FALSE); the count restarts at start
 *   and after a flush.
 * - Output buffers are refcounted sets (rds-infer-output.h) that tensor metas keep alive, also in demuxed copies.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsinfer.h"
#include "rds-infer-config.h"
#include "rds-infer-engine.h"
#include "rds-infer-output.h"
#include "rds-infer-parse.h"
#include "rds-infer-post.h"
#include "rds-infer-preprocess.h"

#include <rds/rds.h>
#include <rds/rds-infer-meta.h>
#include <errno.h>
#include <stdarg.h>
#include <string.h>
/* after GLib/GStreamer: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>

GST_DEBUG_CATEGORY (gst_rds_infer_debug);
#define GST_CAT_DEFAULT gst_rds_infer_debug

/* Property defaults: the config file's defaults (ours, see rds-infer-config.c); they apply only when set. */
#define DEFAULT_UNIQUE_ID 1
#define DEFAULT_PROCESS_MODE 1
#define DEFAULT_INFER_ON_GIE_ID 0
#define DEFAULT_BATCH_SIZE 1
#define DEFAULT_INTERVAL 0
#define DEFAULT_GPU_ID 0

/* Object drawing defaults (ours: nvinfer documents none). */
#define BORDER_WIDTH 3
#define TEXT_FONT "Serif"
#define TEXT_FONT_SIZE 10
#define TEXT_Y_ABOVE 20

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

/* Warnings posted once per element (then logged at debug level). */
typedef enum
{
  WARN_PARSE = 1 << 0,          /* a parse function returned false or threw */
  WARN_POST_INVALID = 1 << 1,
  WARN_POST_ROTATED = 1 << 2,
  WARN_RAW_OUTPUT = 1 << 3,
} WarnOnce;

/* One frame of the batch being inferred. */
typedef struct
{
  RdsFrameMeta *frame;
  guint slot;
  RdsInferLetterbox lb;
  guint det_start, n_dets;      /* in self->dets */
} FrameJob;

struct _GstRdsInfer
{
  GstBaseTransform parent;

  /* properties (object lock); set_mask has PROP_BIT of every property set explicitly */
  gchar *config_file_path, *infer_on_class_ids, *filter_out_class_ids, *model_engine_file;
  guint unique_id, process_mode, infer_on_gie_id, batch_size, gpu_id;
  gint interval;
  gboolean output_tensor_meta, raw_output_file_write, output_instance_mask, input_tensor_meta,
      crop_objects_to_roi_boundary, clip_object_outside_roi;
  gpointer raw_output_generated_callback, raw_output_generated_userdata;
  guint32 set_mask;

  /* from start to stop (streaming thread; test hooks read it while idle) */
  RdsInferConfig *cfg;
  RdsInferEngine *engine;
  RdsInferParser *parser;
  RdsInferOutputPool *outputs;
  hipStream_t stream;
  guint gpu;
  gfloat *input;                /* batch_size network images on the GPU */
  gsize image_floats;
  guint channels;
  RdsInferNetworkInfo net;
  guint n_outputs;
  RdsInferDims *out_dims;
  const gchar **out_names;      /* the engine's */
  guint batch_count;            /* batches seen since start or the last flush */
  guint warned;                 /* WarnOnce */
  GArray *jobs;                 /* FrameJob */
  GArray *objects;              /* RdsInferObjectDetectionInfo, one frame's parser output */
  GArray *dets;                 /* RdsInferDetection, every frame's */
  GByteArray *scratch;
  RdsInferLayerInfo *layers;    /* one frame's output layers, for the parser */
  gboolean *slot_used;          /* batch_size entries: the current batch has a frame in that slot */
};

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

/* @s split at @sep into integers in [min, G_MAXUINT] (whitespace around them allowed, empty items skipped) appended
 * to @out (gint64); an empty or NULL string gives an empty list. */
static gboolean
parse_id_list (const gchar * s, const gchar * sep, gint64 min, GArray * out)
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
    ok = errno == 0 && end && !*end && v >= min && v <= G_MAXUINT;
    if (ok)
      g_array_append_val (out, v);
  }
  g_strfreev (items);
  return ok;
}

/* What start would run with: the config file with the explicitly set properties applied and validated. Property
 * warnings go into cfg->warnings. @operate_on_class_ids (gint64, may be NULL) gets infer-on-class-ids, which is only
 * checked (it applies to secondary mode). */
static RdsInferConfig *
configure (GstRdsInfer * self, GArray * operate_on_class_ids, GError ** error)
{
  RdsInferConfig *cfg = NULL;
  const gchar *overridden[16];
  guint n_over = 0;
  gchar *path, *engine_file, *on_class_ids, *filter_ids;
  guint32 set;
  guint unique_id, process_mode, batch_size, gpu_id;
  gint interval;
  gboolean tensor_meta, raw_write, instance_mask, tensor_input, crop, clip;
  gpointer raw_cb;
  GArray *ids;
  GError *err = NULL;

  GST_OBJECT_LOCK (self);
  path = g_strdup (self->config_file_path);
  engine_file = g_strdup (self->model_engine_file);
  on_class_ids = g_strdup (self->infer_on_class_ids);
  filter_ids = g_strdup (self->filter_out_class_ids);
  set = self->set_mask;
  unique_id = self->unique_id;
  process_mode = self->process_mode;
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

#define OVERRIDE(prop, key, assign) G_STMT_START { \
    if (set & PROP_BIT (prop)) { assign; overridden[n_over++] = key; } } G_STMT_END
  OVERRIDE (PROP_UNIQUE_ID, "gie-unique-id", cfg->gie_unique_id = unique_id);
  OVERRIDE (PROP_PROCESS_MODE, "process-mode", cfg->process_mode = (gint) process_mode);
  OVERRIDE (PROP_BATCH_SIZE, "batch-size", cfg->batch_size = batch_size);
  OVERRIDE (PROP_INTERVAL, "interval", cfg->interval = (guint) interval);
  OVERRIDE (PROP_GPU_ID, "gpu-id", cfg->gpu_id = gpu_id);
  OVERRIDE (PROP_OUTPUT_TENSOR_META, "output-tensor-meta", cfg->output_tensor_meta = tensor_meta);
  OVERRIDE (PROP_MODEL_ENGINE_FILE, "model-engine-file", g_free (cfg->model_engine_file);
      cfg->model_engine_file = g_strdup (engine_file));
#undef OVERRIDE
  if (set & PROP_BIT (PROP_FILTER_OUT_CLASS_IDS)) {
    ids = g_array_new (FALSE, FALSE, sizeof (gint64));
    if (!parse_id_list (filter_ids, ";", 0, ids)) {
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
    overridden[n_over++] = "filter-out-class-ids";
  }
  if (set & PROP_BIT (PROP_INFER_ON_CLASS_IDS)) {
    ids = g_array_new (FALSE, FALSE, sizeof (gint64));
    if (!parse_id_list (on_class_ids, ":", -1, ids)) {
      g_array_unref (ids);
      property_error (error, RDS_INFER_CONFIG_ERROR_INVALID, "infer-on-class-ids", on_class_ids,
          "expected class ids separated by ':', e.g. 1:2:4");
      goto fail;
    }
    if (operate_on_class_ids)
      g_array_append_vals (operate_on_class_ids, ids->data, ids->len);
    g_array_unref (ids);
  }
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
  g_assert (n_over < G_N_ELEMENTS (overridden));
  if (!rds_infer_config_validate (cfg, overridden, &err)) {
    /* errors about overridden keys name the config key: name the property instead where they differ */
    if ((set & PROP_BIT (PROP_UNIQUE_ID)) && g_str_has_prefix (err->message, "element property gie-unique-id:")) {
      gchar *msg = g_strconcat ("element property unique-id:",
          err->message + strlen ("element property gie-unique-id:"), NULL);

      g_free (err->message);
      err->message = msg;
    }
    g_propagate_error (error, err);
    goto fail;
  }

  if (set & PROP_BIT (PROP_INFER_ON_GIE_ID) || set & PROP_BIT (PROP_INFER_ON_CLASS_IDS))
    g_ptr_array_add (cfg->warnings, g_strdup ("element properties infer-on-gie-id and infer-on-class-ids: ignored: "
            "they apply to secondary mode (process-mode=2) only, which is not supported yet"));
  if (crop || clip)
    g_ptr_array_add (cfg->warnings, g_strdup ("element properties crop-objects-to-roi-boundary and "
            "clip-object-outside-roi: ignored: they apply to preprocessed-tensor ROIs, which are not supported yet"));
  /* start marks it posted (not this function: the test hook calls it too) */
  if ((raw_write || raw_cb) && !(self->warned & WARN_RAW_OUTPUT)) {
    g_ptr_array_add (cfg->warnings, g_strdup ("element properties raw-output-file-write and "
            "raw-output-generated-callback: ignored: raw output dumps are not implemented"));
  }
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
join_ids (const GArray * a, gboolean is_uint)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < a->len; i++) {
    gint64 v = is_uint ? (gint64) g_array_index (a, guint, i) : g_array_index (a, gint64, i);

    g_string_append_printf (s, "%s%" G_GINT64_FORMAT, i ? ";" : "", v);
  }
  return g_string_free (s, FALSE);
}

GstStructure *
rds_infer_test_get_config (GstElement * element, GError ** error)
{
  GArray *class_ids = g_array_new (FALSE, FALSE, sizeof (gint64));
  RdsInferConfig *cfg = configure (GST_RDS_INFER (element), class_ids, error);
  GstStructure *s = NULL;
  gchar *filter, *on_class;

  if (cfg) {
    filter = join_ids (cfg->filter_out_class_ids, TRUE);
    on_class = join_ids (class_ids, FALSE);
    s = gst_structure_new ("rdsinfer-config",
        "batch-size", G_TYPE_UINT, cfg->batch_size,
        "interval", G_TYPE_UINT, cfg->interval,
        "gie-unique-id", G_TYPE_UINT, cfg->gie_unique_id,
        "process-mode", G_TYPE_INT, cfg->process_mode,
        "gpu-id", G_TYPE_UINT, cfg->gpu_id,
        "output-tensor-meta", G_TYPE_BOOLEAN, cfg->output_tensor_meta,
        "model-engine-file", G_TYPE_STRING, cfg->model_engine_file,
        "filter-out-class-ids", G_TYPE_STRING, filter,
        "operate-on-class-ids", G_TYPE_STRING, on_class,
        "n-warnings", G_TYPE_UINT, cfg->warnings->len, NULL);
    g_free (filter);
    g_free (on_class);
    rds_infer_config_free (cfg);
  }
  g_array_unref (class_ids);
  return s;
}

/* ---- start / stop ---- */

static void
release_resources (GstRdsInfer * self)
{
  int prev = 0;
  gboolean switched = FALSE;

  if (self->stream || self->input || self->engine) {
    switched = hipGetDevice (&prev) == hipSuccess && hipSetDevice ((int) self->gpu) == hipSuccess;
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
    (void) hipSetDevice (prev);
  g_clear_pointer (&self->cfg, rds_infer_config_free);
  g_clear_pointer (&self->out_dims, g_free);
  g_clear_pointer (&self->out_names, g_free);
  g_clear_pointer (&self->layers, g_free);
  g_clear_pointer (&self->slot_used, g_free);
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
  RdsInferEngineOptions opts = {
    .onnx_file = cfg->onnx_file,
    .engine_file = cfg->model_engine_file,
    .batch_size = cfg->batch_size,
    .gpu_id = self->gpu,
    .fp16 = cfg->network_mode == 2,
  };
  GError *err = NULL;
  GPtrArray *warnings;

  if (!cfg->onnx_file) {
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_UNSUPPORTED,
        "%s:onnx-file: (unset): rdsinfer needs the ONNX model (a cached program is checked against it)",
        cfg->config_path);
    return FALSE;
  }
  if (cfg->has_infer_dims) {
    /* infer-dims is channels;height;width; an NHWC model's dims are in H, W, C order */
    dims[0] = nhwc ? cfg->infer_dims[1] : cfg->infer_dims[0];
    dims[1] = nhwc ? cfg->infer_dims[2] : cfg->infer_dims[1];
    dims[2] = nhwc ? cfg->infer_dims[0] : cfg->infer_dims[2];
    opts.infer_dims = dims;
  }
  if (!(self->engine = rds_infer_engine_open (&opts, &err))) {
    g_set_error (error, RDS_INFER_CONFIG_ERROR, RDS_INFER_CONFIG_ERROR_INVALID, "%s:onnx-file: %s",
        cfg->config_path, err->message);
    g_error_free (err);
    return FALSE;
  }
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
    for (guint k = 0; k < nd; k++)
      d->d[k] = od[k];
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
  if (!(self->cfg = configure (self, NULL, &err))) {
    post_config_error (self, err);
    g_error_free (err);
    return FALSE;
  }
  for (guint i = 0; i < self->cfg->warnings->len; i++)
    GST_ELEMENT_WARNING (self, RESOURCE, SETTINGS, ("%s", (const gchar *) g_ptr_array_index (self->cfg->warnings,
                i)), (NULL));
  GST_OBJECT_LOCK (self);
  if (self->raw_output_file_write || self->raw_output_generated_callback)
    self->warned |= WARN_RAW_OUTPUT;
  GST_OBJECT_UNLOCK (self);

  self->gpu = self->cfg->gpu_id;
  if (hipGetDevice (&prev) != hipSuccess || hipSetDevice ((int) self->gpu) != hipSuccess) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("element property gpu-id: %u: no such GPU", self->gpu), (NULL));
    release_resources (self);
    return FALSE;
  }
  herr = hipStreamCreateWithFlags (&self->stream, hipStreamNonBlocking);
  if (herr != hipSuccess) {
    self->stream = NULL;
    (void) hipSetDevice (prev);
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
  (void) hipSetDevice (prev);
  if (!ok) {
    post_config_error (self, err);
    g_error_free (err);
    release_resources (self);
    return FALSE;
  }
  self->slot_used = g_new0 (gboolean, self->cfg->batch_size);
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

static void
warn_once (GstRdsInfer * self, WarnOnce what, const gchar * msg)
{
  if (!(self->warned & what)) {
    self->warned |= what;
    GST_ELEMENT_WARNING (self, STREAM, DECODE, ("%s", msg), ("further occurrences are logged at debug level"));
  } else {
    GST_DEBUG_OBJECT (self, "%s", msg);
  }
}

/* Reads the frame list (under the meta lock) into self->jobs and self->slot_used; FALSE with an element error on a
 * bad batch. */
static gboolean
snapshot_frames (GstRdsInfer * self, RdsBatchMeta * bm, const RdsBufSurface * surf)
{
  guint batch = self->cfg->batch_size;
  gboolean ok = TRUE;

  g_array_set_size (self->jobs, 0);
  memset (self->slot_used, 0, batch * sizeof (gboolean));
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;
    FrameJob job = {.frame = fm,.slot = fm->batch_id };

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
    g_array_append_val (self->jobs, job);
  }
  rds_release_meta_lock (bm);
  return ok;
}

/* Parser and post-processing of every frame, on the host outputs of @set. */
static void
parse_frames (GstRdsInfer * self, RdsInferOutputSet * set)
{
  RdsInferConfig *cfg = self->cfg;

  g_array_set_size (self->dets, 0);
  for (guint f = 0; f < self->jobs->len; f++) {
    FrameJob *job = &g_array_index (self->jobs, FrameJob, f);
    GError *err = NULL;
    RdsInferPostFlags flags;

    job->det_start = self->dets->len;
    job->n_dets = 0;
    for (guint i = 0; i < self->n_outputs; i++) {
      gsize offset = (gsize) job->slot * self->out_dims[i].numElements;

      self->layers[i].buffer = set->host[i] + offset;
      self->layers[i].deviceBuffer = set->dev[i] + offset;
    }
    g_array_set_size (self->objects, 0);
    if (!rds_infer_parser_parse (self->parser, self->layers, self->n_outputs, &self->net, self->objects, &err)) {
      gchar *msg = g_strdup_printf ("Parser %s failed on a frame of source %u, which gets no objects: %s",
          rds_infer_parser_get_func_name (self->parser), job->frame->source_id, err->message);

      warn_once (self, WARN_PARSE, msg);
      g_free (msg);
      g_error_free (err);
      continue;
    }
    flags = rds_infer_postprocess (cfg, &job->lb, (const RdsInferObjectDetectionInfo *) self->objects->data,
        self->objects->len, self->scratch, self->dets);
    if (flags & RDS_INFER_POST_INVALID)
      warn_once (self, WARN_POST_INVALID, "The parser returned invalid objects (a class id >= "
          "num-detected-classes, a coordinate or confidence that is not finite, or an empty box); they were dropped");
    if (flags & RDS_INFER_POST_ROTATED)
      warn_once (self, WARN_POST_ROTATED, "The parser returned objects with a rotation_angle; oriented boxes are "
          "not supported, the angle is ignored");
    job->n_dets = self->dets->len - job->det_start;
  }
}

static void
add_object (GstRdsInfer * self, RdsBatchMeta * bm, RdsFrameMeta * fm, const RdsInferDetection * d)
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
  om->rect_params.border_width = BORDER_WIDTH;
  om->rect_params.border_color = (RdsOSD_ColorParams) {
  1.0, 0.0, 0.0, 1.0};
  om->rect_params.has_bg_color = 0;
  g_strlcpy (om->obj_label, label, sizeof (om->obj_label));
  tp->display_text = g_strdup (label);
  tp->x_offset = (guint) d->rect.left;
  tp->y_offset = d->rect.top > TEXT_Y_ABOVE ? (guint) (d->rect.top - TEXT_Y_ABOVE) : 0;
  tp->font_params.font_name = (gchar *) TEXT_FONT;
  tp->font_params.font_size = TEXT_FONT_SIZE;
  tp->font_params.font_color = (RdsOSD_ColorParams) {
  1.0, 1.0, 1.0, 1.0};
  tp->set_bg_clr = 1;
  tp->text_bg_clr = (RdsOSD_ColorParams) {
  0.0, 0.0, 0.0, 1.0};
  rds_add_obj_meta_to_frame (fm, om, NULL);
}

static void
attach_results (GstRdsInfer * self, RdsBatchMeta * bm, RdsInferOutputSet * set)
{
  RdsInferConfig *cfg = self->cfg;
  gboolean tensor_meta = cfg->output_tensor_meta || cfg->network_type == 100;

  rds_acquire_meta_lock (bm);
  for (guint f = 0; f < self->jobs->len; f++) {
    FrameJob *job = &g_array_index (self->jobs, FrameJob, f);

    /* A frame removed while the lock was released (by an application thread) gets nothing. This assumes such a
     * thread doesn't also add a new frame in between: one acquired from the pool could reuse the removed frame's
     * memory and would then get its results. */
    if (!g_list_find (bm->frame_meta_list, job->frame))
      continue;
    if (cfg->network_type == 0)
      for (guint i = 0; i < job->n_dets; i++)
        add_object (self, bm, job->frame, &g_array_index (self->dets, RdsInferDetection, job->det_start + i));
    if (tensor_meta) {
      RdsUserMeta *um = rds_acquire_user_meta_from_pool (bm);

      um->user_meta_data = rds_infer_tensor_meta_new (set, job->slot, cfg->gie_unique_id, &self->net,
          cfg->maintain_aspect_ratio, cfg->symmetric_padding);
      um->base_meta.meta_type = RDS_INFER_TENSOR_OUTPUT_META;
      um->base_meta.copy_func = rds_infer_tensor_meta_copy;
      um->base_meta.release_func = rds_infer_tensor_meta_release;
      rds_add_user_meta_to_frame (job->frame, um);
    }
    job->frame->bInferDone = TRUE;
  }
  rds_release_meta_lock (bm);
}

static GstFlowReturn
gst_rds_infer_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRdsInfer *self = GST_RDS_INFER (trans);
  RdsInferConfig *cfg = self->cfg;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsBufSurface *surf = rds_buffer_get_surface (buf);
  RdsInferOutputSet *set = NULL;
  GError *err = NULL;
  gint64 t0 = 0;

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
  if (!snapshot_frames (self, bm, surf))
    return GST_FLOW_ERROR;
  if (self->jobs->len == 0) {
    GST_LOG_OBJECT (self, "batch without frames");
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
  /* 1. preprocessing: each frame into its slot's image, unfilled slots zeroed */
  for (guint f = 0; f < self->jobs->len; f++) {
    FrameJob *job = &g_array_index (self->jobs, FrameJob, f);
    const RdsBufSurfaceParams *sp = &surf->surfaceList[job->slot];
    RdsPreprocessJob pj = { 0 };

    rds_infer_letterbox (self->net.width, self->net.height, sp->width, sp->height, cfg->maintain_aspect_ratio,
        cfg->symmetric_padding, &job->lb);
    (void) rds_infer_preprocess_job_set_source (&pj, sp, &job->lb);
    pj.channels = self->channels;
    pj.color = cfg->model_color_format;
    pj.nhwc = cfg->network_input_order == 1;
    pj.bilinear = rds_transform_filter_is_bilinear ((RdsBufSurfTransform_Inter) cfg->scaling_filter);
    pj.scale = cfg->net_scale_factor;
    for (guint c = 0; c < 3; c++)
      pj.offsets[c] = c < cfg->n_offsets ? cfg->offsets[c] : 0.0f;
    if (rds_infer_preprocess_launch (&pj, self->input, job->slot, self->stream) != 0) {
      GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Preprocessing failed"),
          ("slot %u: %ux%u, format %d", job->slot, sp->width, sp->height, sp->colorFormat));
      goto fail;
    }
  }
  for (guint slot = 0; slot < cfg->batch_size; slot++)
    if (!self->slot_used[slot])
      HIP_OR_FAIL (hipMemsetAsync (self->input + slot * self->image_floats, 0,
              self->image_floats * sizeof (gfloat), self->stream), "zeroing unfilled slots");

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
    parse_frames (self, set);
  attach_results (self, bm, set);
  if (t0)
    GST_DEBUG_OBJECT (self, "batch of %u frames: %.3f ms", self->jobs->len,
        (g_get_monotonic_time () - t0) / 1000.0);
  rds_infer_output_set_unref (set);
  return GST_FLOW_OK;

fail:
  /* work already queued may still read the batch or write the set: wait for it before either goes */
  (void) hipStreamSynchronize (self->stream);
  if (set)
    rds_infer_output_set_unref (set);
  return GST_FLOW_ERROR;
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
rds_infer_test_read_input (GstElement * element, gfloat * dst, gsize n_floats)
{
  GstRdsInfer *self = GST_RDS_INFER (element);
  int prev = 0;
  gboolean ok;

  if (!self->input || n_floats > self->cfg->batch_size * self->image_floats)
    return FALSE;
  if (hipGetDevice (&prev) != hipSuccess || hipSetDevice ((int) self->gpu) != hipSuccess)
    return FALSE;
  ok = hipMemcpy (dst, self->input, n_floats * sizeof (gfloat), hipMemcpyDeviceToHost) == hipSuccess;
  (void) hipSetDevice (prev);
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
      self->infer_on_gie_id = g_value_get_uint (value);
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
      g_value_set_uint (value, self->infer_on_gie_id);
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
  g_array_unref (self->jobs);
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
          "1: primary (whole frames), 2: secondary (objects; not supported yet, fails at start)", 1, 2,
          DEFAULT_PROCESS_MODE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INFER_ON_GIE_ID,
      g_param_spec_uint ("infer-on-gie-id", "Infer on GIE ID",
          "Secondary mode: infer on the objects of this unique-id (ignored in primary mode)", 0, G_MAXUINT,
          DEFAULT_INFER_ON_GIE_ID, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INFER_ON_CLASS_IDS,
      g_param_spec_string ("infer-on-class-ids", "Infer on class IDs",
          "Secondary mode: infer on objects of these classes, separated by ':' (e.g. 1:2:4; ignored in primary "
          "mode)", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_FILTER_OUT_CLASS_IDS,
      g_param_spec_string ("filter-out-class-ids", "Filter out class IDs",
          "Drop detections of these classes, separated by ';' (e.g. 1;2)", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_MODEL_ENGINE_FILE,
      g_param_spec_string ("model-engine-file", "Model engine file",
          "Compiled MIGraphX program (.mxr) to load if it matches the model; used as given, not relative to the "
          "config file", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_BATCH_SIZE,
      g_param_spec_uint ("batch-size", "Batch size", "Frames inferred together", 1, RDS_INFER_MAX_BATCH_SIZE,
          DEFAULT_BATCH_SIZE, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INTERVAL,
      g_param_spec_int ("interval", "Interval", "Batches skipped after each inferred batch", 0,
          RDS_INFER_MAX_INTERVAL, DEFAULT_INTERVAL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU ID", "GPU for preprocessing and inference", 0, G_MAXUINT, DEFAULT_GPU_ID,
          PFLAGS));
  g_object_class_install_property (gobject_class, PROP_OUTPUT_TENSOR_META,
      g_param_spec_boolean ("output-tensor-meta", "Output tensor meta",
          "Attach the output tensors to each frame as RdsInferTensorMeta user meta", FALSE, PFLAGS));
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
  self->jobs = g_array_new (FALSE, FALSE, sizeof (FrameJob));
  self->objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  self->dets = g_array_new (FALSE, FALSE, sizeof (RdsInferDetection));
  self->scratch = g_byte_array_new ();
}
