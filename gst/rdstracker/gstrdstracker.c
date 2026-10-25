/* Redstream: rdstracker, a multi-object tracker. Property names follow DeepStream's public nvtracker documentation.
 *
 * An in-place GstBaseTransform in passthrough mode on rdsstreammux batches: buffers go downstream unchanged, with
 * the objects in their RdsBatchMeta given tracking IDs. Pixels are never written.
 * - Every property is accepted and stored; the tracking core reads them at start.
 * - Per batch, transform_ip hands the batch meta to the tracking core. Until the core is in, batches and their meta
 *   pass through untouched.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdstracker.h"

#include <rds/rds.h>

GST_DEBUG_CATEGORY (gst_rds_tracker_debug);
#define GST_CAT_DEFAULT gst_rds_tracker_debug

/* Property defaults, as nvtracker reports them. */
#define DEFAULT_TRACKER_WIDTH 960
#define DEFAULT_TRACKER_HEIGHT 544
#define DEFAULT_GPU_ID 0
#define DEFAULT_DISPLAY_TRACKING_ID TRUE
#define DEFAULT_TRACKING_ID_RESET_MODE 1
#define DEFAULT_TRACKING_SURFACE_TYPE 0
#define DEFAULT_COMPUTE_HW GST_RDS_TRACKER_COMPUTE_HW_DEFAULT
#define DEFAULT_INPUT_TENSOR_META FALSE
#define DEFAULT_TENSOR_META_GIE_ID 0
#define DEFAULT_SUB_BATCHES ""
#define DEFAULT_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT 0
#define DEFAULT_USER_META_POOL_SIZE 32

enum
{
  PROP_0,
  PROP_LL_LIB_FILE,
  PROP_LL_CONFIG_FILE,
  PROP_TRACKER_WIDTH,
  PROP_TRACKER_HEIGHT,
  PROP_GPU_ID,
  PROP_DISPLAY_TRACKING_ID,
  PROP_TRACKING_ID_RESET_MODE,
  PROP_TRACKING_SURFACE_TYPE,
  PROP_COMPUTE_HW,
  PROP_INPUT_TENSOR_META,
  PROP_TENSOR_META_GIE_ID,
  PROP_SUB_BATCHES,
  PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT,
  PROP_USER_META_POOL_SIZE,
  PROP_OPERATE_ON_CLASS_IDS,
};

struct _GstRdsTracker
{
  GstBaseTransform parent;

  /* properties (object lock) */
  gchar *ll_lib_file;
  gchar *ll_config_file;
  guint tracker_width;
  guint tracker_height;
  guint gpu_id;
  gboolean display_tracking_id;
  guint tracking_id_reset_mode;
  guint tracking_surface_type;
  GstRdsTrackerComputeHw compute_hw;
  gboolean input_tensor_meta;
  guint tensor_meta_gie_id;
  gchar *sub_batches;
  gint sub_batch_err_recovery_trial_cnt;
  guint user_meta_pool_size;
  gchar *operate_on_class_ids;
};

#define RDS_TRACKER_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), format = (string) { NV12, RGBA, BGRA, RGBx, BGRx }, " \
  "width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], framerate = (fraction) [ 0/1, 2147483647/1 ]"

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_TRACKER_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_TRACKER_CAPS));

GType
gst_rds_tracker_compute_hw_get_type (void)
{
  static gsize type;
  static const GEnumValue values[] = {
    {GST_RDS_TRACKER_COMPUTE_HW_DEFAULT, "Default", "default"},
    {GST_RDS_TRACKER_COMPUTE_HW_GPU, "GPU", "gpu"},
    {GST_RDS_TRACKER_COMPUTE_HW_VIC, "VIC", "vic"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsTrackerComputeHw", values);

    g_once_init_leave (&type, t);
  }
  return (GType) type;
}

G_DEFINE_TYPE (GstRdsTracker, gst_rds_tracker, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdstracker, "rdstracker", GST_RANK_NONE, GST_TYPE_RDS_TRACKER);

/* ---- streaming ---- */

/* Passthrough with transform_ip, as in rdsinfer: the buffer is never made writable. The RdsBatchMeta is shared by
 * every holder of the batch on purpose (as DeepStream's batch meta is), so the tracking core writes it in place under
 * rds_acquire_meta_lock, like rdsinfer adds its objects (see rdsinfer's header comment), and never touches pixels. */
static GstFlowReturn
gst_rds_tracker_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  /* The tracking core plugs in here: it reads the batch meta's frames and objects and writes their tracking IDs. */
  (void) trans;
  (void) buf;
  return GST_FLOW_OK;
}

/* ---- properties ---- */

/* The READY-only settings are read when streaming starts: refuse changes once streaming, including during the
 * READY -> PAUSED transition (object lock held, which also guards the state fields). */
static gboolean
check_mutable (GstRdsTracker * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_tracker_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsTracker *self = GST_RDS_TRACKER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LL_LIB_FILE:
      g_free (self->ll_lib_file);
      self->ll_lib_file = g_value_dup_string (value);
      break;
    case PROP_LL_CONFIG_FILE:
      g_free (self->ll_config_file);
      self->ll_config_file = g_value_dup_string (value);
      break;
    case PROP_TRACKER_WIDTH:
      if (check_mutable (self, pspec))
        self->tracker_width = g_value_get_uint (value);
      break;
    case PROP_TRACKER_HEIGHT:
      if (check_mutable (self, pspec))
        self->tracker_height = g_value_get_uint (value);
      break;
    case PROP_GPU_ID:
      if (check_mutable (self, pspec))
        self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_DISPLAY_TRACKING_ID:
      self->display_tracking_id = g_value_get_boolean (value);
      break;
    case PROP_TRACKING_ID_RESET_MODE:
      if (check_mutable (self, pspec))
        self->tracking_id_reset_mode = g_value_get_uint (value);
      break;
    case PROP_TRACKING_SURFACE_TYPE:
      if (check_mutable (self, pspec))
        self->tracking_surface_type = g_value_get_uint (value);
      break;
    case PROP_COMPUTE_HW:
      self->compute_hw = g_value_get_enum (value);
      break;
    case PROP_INPUT_TENSOR_META:
      self->input_tensor_meta = g_value_get_boolean (value);
      break;
    case PROP_TENSOR_META_GIE_ID:
      if (check_mutable (self, pspec))
        self->tensor_meta_gie_id = g_value_get_uint (value);
      break;
    case PROP_SUB_BATCHES:
      g_free (self->sub_batches);
      self->sub_batches = g_value_dup_string (value);
      break;
    case PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT:
      self->sub_batch_err_recovery_trial_cnt = g_value_get_int (value);
      break;
    case PROP_USER_META_POOL_SIZE:
      if (check_mutable (self, pspec))
        self->user_meta_pool_size = g_value_get_uint (value);
      break;
    case PROP_OPERATE_ON_CLASS_IDS:
      g_free (self->operate_on_class_ids);
      self->operate_on_class_ids = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_tracker_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsTracker *self = GST_RDS_TRACKER (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_LL_LIB_FILE:
      g_value_set_string (value, self->ll_lib_file);
      break;
    case PROP_LL_CONFIG_FILE:
      g_value_set_string (value, self->ll_config_file);
      break;
    case PROP_TRACKER_WIDTH:
      g_value_set_uint (value, self->tracker_width);
      break;
    case PROP_TRACKER_HEIGHT:
      g_value_set_uint (value, self->tracker_height);
      break;
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_DISPLAY_TRACKING_ID:
      g_value_set_boolean (value, self->display_tracking_id);
      break;
    case PROP_TRACKING_ID_RESET_MODE:
      g_value_set_uint (value, self->tracking_id_reset_mode);
      break;
    case PROP_TRACKING_SURFACE_TYPE:
      g_value_set_uint (value, self->tracking_surface_type);
      break;
    case PROP_COMPUTE_HW:
      g_value_set_enum (value, self->compute_hw);
      break;
    case PROP_INPUT_TENSOR_META:
      g_value_set_boolean (value, self->input_tensor_meta);
      break;
    case PROP_TENSOR_META_GIE_ID:
      g_value_set_uint (value, self->tensor_meta_gie_id);
      break;
    case PROP_SUB_BATCHES:
      g_value_set_string (value, self->sub_batches);
      break;
    case PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT:
      g_value_set_int (value, self->sub_batch_err_recovery_trial_cnt);
      break;
    case PROP_USER_META_POOL_SIZE:
      g_value_set_uint (value, self->user_meta_pool_size);
      break;
    case PROP_OPERATE_ON_CLASS_IDS:
      g_value_set_string (value, self->operate_on_class_ids);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_tracker_finalize (GObject * object)
{
  GstRdsTracker *self = GST_RDS_TRACKER (object);

  g_free (self->ll_lib_file);
  g_free (self->ll_config_file);
  g_free (self->sub_batches);
  g_free (self->operate_on_class_ids);
  G_OBJECT_CLASS (gst_rds_tracker_parent_class)->finalize (object);
}

#define PFLAGS (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)
/* changeable only in NULL or READY */
#define PFLAGS_READY (PFLAGS | GST_PARAM_MUTABLE_READY)

static void
gst_rds_tracker_class_init (GstRdsTrackerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);

  GST_DEBUG_CATEGORY_INIT (gst_rds_tracker_debug, "rdstracker", 0, "Redstream multi-object tracker");

  gobject_class->set_property = gst_rds_tracker_set_property;
  gobject_class->get_property = gst_rds_tracker_get_property;
  gobject_class->finalize = gst_rds_tracker_finalize;

  g_object_class_install_property (gobject_class, PROP_LL_LIB_FILE,
      g_param_spec_string ("ll-lib-file", "Low-level library file",
          "Path to the low-level tracker library", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_LL_CONFIG_FILE,
      g_param_spec_string ("ll-config-file", "Low-level config file",
          "Tracker configuration file (YAML)", NULL, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_TRACKER_WIDTH,
      g_param_spec_uint ("tracker-width", "Tracker width",
          "Width of the frames the tracker works on", 0, G_MAXUINT, DEFAULT_TRACKER_WIDTH, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_TRACKER_HEIGHT,
      g_param_spec_uint ("tracker-height", "Tracker height",
          "Height of the frames the tracker works on", 0, G_MAXUINT, DEFAULT_TRACKER_HEIGHT, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU ID", "GPU the tracker runs on", 0, G_MAXUINT, DEFAULT_GPU_ID,
          PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_DISPLAY_TRACKING_ID,
      g_param_spec_boolean ("display-tracking-id", "Display tracking ID",
          "Show the tracking ID in each object's label", DEFAULT_DISPLAY_TRACKING_ID, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_TRACKING_ID_RESET_MODE,
      g_param_spec_uint ("tracking-id-reset-mode", "Tracking ID reset mode",
          "When a stream's tracking IDs restart: 0 never, 1 on stream reset, 2 at end of stream, 3 both", 0, 3,
          DEFAULT_TRACKING_ID_RESET_MODE, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_TRACKING_SURFACE_TYPE,
      g_param_spec_uint ("tracking-surface-type", "Tracking surface type",
          "Surface type of the streams to track (0: default)", 0, G_MAXUINT, DEFAULT_TRACKING_SURFACE_TYPE,
          PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_COMPUTE_HW,
      g_param_spec_enum ("compute-hw", "Compute hardware",
          "Hardware for the tracker's frame scaling", GST_TYPE_RDS_TRACKER_COMPUTE_HW, DEFAULT_COMPUTE_HW,
          PFLAGS));
  g_object_class_install_property (gobject_class, PROP_INPUT_TENSOR_META,
      g_param_spec_boolean ("input-tensor-meta", "Input tensor meta",
          "Use tensor meta from an upstream preprocessing element", DEFAULT_INPUT_TENSOR_META, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_TENSOR_META_GIE_ID,
      g_param_spec_uint ("tensor-meta-gie-id", "Tensor meta GIE ID",
          "With input-tensor-meta: the unique ID of the tensor meta to use", 0, G_MAXUINT,
          DEFAULT_TENSOR_META_GIE_ID, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_SUB_BATCHES,
      g_param_spec_string ("sub-batches", "Sub-batches",
          "Split the batch into sub-batches of source IDs, tracked separately (e.g. 0,1;2,3)", DEFAULT_SUB_BATCHES,
          PFLAGS));
  g_object_class_install_property (gobject_class, PROP_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT,
      g_param_spec_int ("sub-batch-err-recovery-trial-cnt", "Sub-batch error recovery trial count",
          "Times a failed sub-batch is restarted (-1: without limit)", -1, G_MAXINT,
          DEFAULT_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT, PFLAGS));
  g_object_class_install_property (gobject_class, PROP_USER_META_POOL_SIZE,
      g_param_spec_uint ("user-meta-pool-size", "User meta pool size",
          "Size of the pool of tracker user meta", 1, G_MAXUINT, DEFAULT_USER_META_POOL_SIZE, PFLAGS_READY));
  g_object_class_install_property (gobject_class, PROP_OPERATE_ON_CLASS_IDS,
      g_param_spec_string ("operate-on-class-ids", "Operate on class IDs",
          "Track only objects of these classes, separated by ';' (e.g. 0;2); unset or empty: all classes", NULL,
          PFLAGS));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream multi-object tracker", "Filter/Video",
      "Gives the objects in memory:RDS batches persistent tracking IDs", "Redstream contributors");

  gst_type_mark_as_plugin_api (GST_TYPE_RDS_TRACKER_COMPUTE_HW, 0);

  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_rds_tracker_transform_ip);
  trans_class->passthrough_on_same_caps = TRUE;
  trans_class->transform_ip_on_passthrough = TRUE;
}

static void
gst_rds_tracker_init (GstRdsTracker * self)
{
  self->tracker_width = DEFAULT_TRACKER_WIDTH;
  self->tracker_height = DEFAULT_TRACKER_HEIGHT;
  self->gpu_id = DEFAULT_GPU_ID;
  self->display_tracking_id = DEFAULT_DISPLAY_TRACKING_ID;
  self->tracking_id_reset_mode = DEFAULT_TRACKING_ID_RESET_MODE;
  self->tracking_surface_type = DEFAULT_TRACKING_SURFACE_TYPE;
  self->compute_hw = DEFAULT_COMPUTE_HW;
  self->input_tensor_meta = DEFAULT_INPUT_TENSOR_META;
  self->tensor_meta_gie_id = DEFAULT_TENSOR_META_GIE_ID;
  self->sub_batches = g_strdup (DEFAULT_SUB_BATCHES);
  self->sub_batch_err_recovery_trial_cnt = DEFAULT_SUB_BATCH_ERR_RECOVERY_TRIAL_CNT;
  self->user_meta_pool_size = DEFAULT_USER_META_POOL_SIZE;
}
