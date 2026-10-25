/* Redstream: rdsinfer. Property names follow DeepStream's public nvinfer documentation.
 *
 * An in-place GstBaseTransform on memory:RDS video: buffers go downstream unchanged (inference only adds metadata),
 * with the same caps on both pads. So far the element only negotiates and forwards buffers.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsinfer.h"

#include <rds/rds.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_infer_debug);
#define GST_CAT_DEFAULT gst_rds_infer_debug

enum
{
  PROP_0,
  PROP_CONFIG_FILE_PATH,
};

struct _GstRdsInfer
{
  GstBaseTransform parent;

  gchar *config_file_path;      /* object lock */
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

static GstFlowReturn
gst_rds_infer_transform_ip (GstBaseTransform * trans G_GNUC_UNUSED, GstBuffer * buf G_GNUC_UNUSED)
{
  return GST_FLOW_OK;
}

static void
gst_rds_infer_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsInfer *self = GST_RDS_INFER (object);

  switch (prop_id) {
    case PROP_CONFIG_FILE_PATH:
      GST_OBJECT_LOCK (self);
      g_free (self->config_file_path);
      self->config_file_path = g_value_dup_string (value);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rds_infer_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsInfer *self = GST_RDS_INFER (object);

  switch (prop_id) {
    case PROP_CONFIG_FILE_PATH:
      GST_OBJECT_LOCK (self);
      g_value_set_string (value, self->config_file_path);
      GST_OBJECT_UNLOCK (self);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_rds_infer_finalize (GObject * object)
{
  GstRdsInfer *self = GST_RDS_INFER (object);

  g_free (self->config_file_path);
  G_OBJECT_CLASS (gst_rds_infer_parent_class)->finalize (object);
}

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

  g_object_class_install_property (gobject_class, PROP_CONFIG_FILE_PATH,
      g_param_spec_string ("config-file-path", "Config file path",
          "Path to the inference configuration file (nvinfer-style key file or YAML)", NULL,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream inference", "Filter/Analyzer/Video",
      "Runs a MIGraphX model on memory:RDS batches and attaches the results as metadata", "Redstream contributors");

  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_rds_infer_transform_ip);
  trans_class->passthrough_on_same_caps = TRUE;
  trans_class->transform_ip_on_passthrough = TRUE;
}

static void
gst_rds_infer_init (GstRdsInfer * self G_GNUC_UNUSED)
{
}
