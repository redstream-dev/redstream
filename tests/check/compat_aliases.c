/* Compile-time coverage of every alias in rds/rds-compat-aliases.h: each type, function and constant is used once.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds_compat.h>

/* The unprefixed nvinfer data-type names (FLOAT, HALF, ...) are C++ only: in C they stay free. */
static const int FLOAT = 0, HALF = 1, INT8 = 2, INT32 = 3, INT64 = 4;
/* The bare tracker state names (EMPTY, ACTIVE, ...) are not aliased: they stay free too. */
static const int EMPTY = 0, ACTIVE = 1, INACTIVE = 2, TENTATIVE = 3, PROJECTED = 4, QUASIACTIVE = 5;

/* Each DeepStream type name must be the Redstream type (pointer assignment fails on a mismatch under -Werror). */
static void
test_types (void)
{
  { RdsBufSurface *r = NULL; NvBufSurface *n = r; g_assert_null (n); }
  { RdsBufSurfaceParams *r = NULL; NvBufSurfaceParams *n = r; g_assert_null (n); }
  { RdsBufSurfacePlaneParams *r = NULL; NvBufSurfacePlaneParams *n = r; g_assert_null (n); }
  { RdsBufSurfaceMappedAddr *r = NULL; NvBufSurfaceMappedAddr *n = r; g_assert_null (n); }
  { RdsBufSurfaceCreateParams *r = NULL; NvBufSurfaceCreateParams *n = r; g_assert_null (n); }
  { RdsBufSurfaceMemType *r = NULL; NvBufSurfaceMemType *n = r; g_assert_null (n); }
  { RdsBufSurfaceColorFormat *r = NULL; NvBufSurfaceColorFormat *n = r; g_assert_null (n); }
  { RdsBufSurfaceLayout *r = NULL; NvBufSurfaceLayout *n = r; g_assert_null (n); }
  { RdsBufSurfaceMemMapFlags *r = NULL; NvBufSurfaceMemMapFlags *n = r; g_assert_null (n); }
  { RdsOSD_ColorParams *r = NULL; NvOSD_ColorParams *n = r; g_assert_null (n); }
  { RdsOSD_FontParams *r = NULL; NvOSD_FontParams *n = r; g_assert_null (n); }
  { RdsOSD_TextParams *r = NULL; NvOSD_TextParams *n = r; g_assert_null (n); }
  { RdsOSD_RectParams *r = NULL; NvOSD_RectParams *n = r; g_assert_null (n); }
  { RdsOSD_LineParams *r = NULL; NvOSD_LineParams *n = r; g_assert_null (n); }
  { RdsOSD_ArrowParams *r = NULL; NvOSD_ArrowParams *n = r; g_assert_null (n); }
  { RdsOSD_CircleParams *r = NULL; NvOSD_CircleParams *n = r; g_assert_null (n); }
  { RdsOSD_MaskParams *r = NULL; NvOSD_MaskParams *n = r; g_assert_null (n); }
  { RdsOSD_Arrow_Head_Direction *r = NULL; NvOSD_Arrow_Head_Direction *n = r; g_assert_null (n); }
  { RdsBbox_Coords *r = NULL; NvBbox_Coords *n = r; g_assert_null (n); }
  { RdsBaseMeta *r = NULL; NvDsBaseMeta *n = r; g_assert_null (n); }
  { RdsBatchMeta *r = NULL; NvDsBatchMeta *n = r; g_assert_null (n); }
  { RdsFrameMeta *r = NULL; NvDsFrameMeta *n = r; g_assert_null (n); }
  { RdsObjectMeta *r = NULL; NvDsObjectMeta *n = r; g_assert_null (n); }
  { RdsClassifierMeta *r = NULL; NvDsClassifierMeta *n = r; g_assert_null (n); }
  { RdsLabelInfo *r = NULL; NvDsLabelInfo *n = r; g_assert_null (n); }
  { RdsDisplayMeta *r = NULL; NvDsDisplayMeta *n = r; g_assert_null (n); }
  { RdsUserMeta *r = NULL; NvDsUserMeta *n = r; g_assert_null (n); }
  { RdsMetaPool *r = NULL; NvDsMetaPool *n = r; g_assert_null (n); }
  { RdsMetaType *r = NULL; NvDsMetaType *n = r; g_assert_null (n); }
  { RdsMetaCopyFunc *r = NULL; NvDsMetaCopyFunc *n = r; g_assert_null (n); }
  { RdsMetaReleaseFunc *r = NULL; NvDsMetaReleaseFunc *n = r; g_assert_null (n); }
  { RdsMetaList *r = NULL; NvDsMetaList *n = r; g_assert_null (n); }
  { RdsFrameMetaList *r = NULL; NvDsFrameMetaList *n = r; g_assert_null (n); }
  { RdsObjectMetaList *r = NULL; NvDsObjectMetaList *n = r; g_assert_null (n); }
  { RdsClassifierMetaList *r = NULL; NvDsClassifierMetaList *n = r; g_assert_null (n); }
  { RdsLabelInfoList *r = NULL; NvDsLabelInfoList *n = r; g_assert_null (n); }
  { RdsUserMetaList *r = NULL; NvDsUserMetaList *n = r; g_assert_null (n); }
  { RdsDisplayMetaList *r = NULL; NvDisplayMetaList *n = r; g_assert_null (n); }
  { RdsComp_BboxInfo *r = NULL; NvDsComp_BboxInfo *n = r; g_assert_null (n); }
  { RdsMeta *r = NULL; NvDsMeta *n = r; g_assert_null (n); }
  { RdsEventType *r = NULL; GstNvEventType *n = r; g_assert_null (n); }
  { RdsBufSurfTransform_Compute *r = NULL; NvBufSurfTransform_Compute *n = r; g_assert_null (n); }
  { RdsBufSurfTransform_Flip *r = NULL; NvBufSurfTransform_Flip *n = r; g_assert_null (n); }
  { RdsBufSurfTransform_Inter *r = NULL; NvBufSurfTransform_Inter *n = r; g_assert_null (n); }
  { RdsBufSurfTransform_Error *r = NULL; NvBufSurfTransform_Error *n = r; g_assert_null (n); }
  { RdsBufSurfTransform_Transform_Flag *r = NULL; NvBufSurfTransform_Transform_Flag *n = r; g_assert_null (n); }
  { RdsBufSurfTransformRect *r = NULL; NvBufSurfTransformRect *n = r; g_assert_null (n); }
  { RdsBufSurfTransformParams *r = NULL; NvBufSurfTransformParams *n = r; g_assert_null (n); }
  { RdsBufSurfTransformConfigParams *r = NULL; NvBufSurfTransformConfigParams *n = r; g_assert_null (n); }
  { RdsInferDims *r = NULL; NvDsInferDims *n = r; g_assert_null (n); }
  { RdsInferDimsCHW *r = NULL; NvDsInferDimsCHW *n = r; g_assert_null (n); }
  { RdsInferDataType *r = NULL; NvDsInferDataType *n = r; g_assert_null (n); }
  { RdsInferLayerInfo *r = NULL; NvDsInferLayerInfo *n = r; g_assert_null (n); }
  { RdsInferNetworkInfo *r = NULL; NvDsInferNetworkInfo *n = r; g_assert_null (n); }
  { RdsInferObjectDetectionInfo *r = NULL; NvDsInferObjectDetectionInfo *n = r; g_assert_null (n); }
  { RdsInferObjectDetectionInfo *r = NULL; NvDsInferParseObjectInfo *n = r; g_assert_null (n); }
  { RdsInferTensorMeta *r = NULL; NvDsInferTensorMeta *n = r; g_assert_null (n); }
  { RdsTrackerState *r = NULL; TRACKER_STATE *n = r; g_assert_null (n); }
}

/* The transform structs keep DeepStream's field names and the documented field types, so designated initializers
 * and member assignments in ported code compile unchanged. */
static void
test_transform_structs (void)
{
  NvBufSurfTransformRect rect = { .top = 1, .left = 2, .width = 3, .height = 4 };
  NvBufSurfTransformParams params = {
    .transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_FILTER,
    .transform_flip = NvBufSurfTransform_None,
    .transform_filter = NvBufSurfTransformInter_Bilinear,
    .src_rect = &rect,
    .dst_rect = NULL,
  };
  NvBufSurfTransformConfigParams config = {
    .compute_mode = NvBufSurfTransformCompute_GPU,
    .gpu_id = 0,
    .cuda_stream = NULL,
  };

  g_assert_cmpuint (sizeof (NvBufSurfTransformRect), ==, sizeof (RdsBufSurfTransformRect));
  g_assert_cmpuint (sizeof (NvBufSurfTransformParams), ==, sizeof (RdsBufSurfTransformParams));
  g_assert_cmpuint (sizeof (NvBufSurfTransformConfigParams), ==, sizeof (RdsBufSurfTransformConfigParams));
  /* four uint32_t in the documented order */
  g_assert_cmpuint (sizeof (NvBufSurfTransformRect), ==, 4 * sizeof (guint32));
  g_assert_cmpuint (G_STRUCT_OFFSET (NvBufSurfTransformRect, top), ==, 0);
  g_assert_cmpuint (G_STRUCT_OFFSET (NvBufSurfTransformRect, left), ==, 4);
  g_assert_cmpuint (G_STRUCT_OFFSET (NvBufSurfTransformRect, width), ==, 8);
  g_assert_cmpuint (G_STRUCT_OFFSET (NvBufSurfTransformRect, height), ==, 12);
  g_assert_cmpuint (sizeof (params.transform_flag), ==, sizeof (guint32));
  g_assert_cmpuint (sizeof (config.gpu_id), ==, sizeof (gint32));
  /* signed, as documented: a negative id is representable (and rejected by SetSessionParams) */
  config.gpu_id = -1;
  g_assert_cmpint (config.gpu_id, <, 0);
  g_assert_cmpuint (sizeof (config.cuda_stream), ==, sizeof (gpointer));
  g_assert_true (params.src_rect->left == 2 && params.dst_rect == NULL);
  g_assert_cmpint (config.compute_mode, ==, RdsBufSurfTransformCompute_GPU);
}

static void
test_functions (void)
{
  gpointer nv[] = {
    (gpointer) NvBufSurfaceCreate,
    (gpointer) NvBufSurfaceDestroy,
    (gpointer) NvBufSurfaceMap,
    (gpointer) NvBufSurfaceUnMap,
    (gpointer) NvBufSurfaceSyncForCpu,
    (gpointer) NvBufSurfaceSyncForDevice,
    (gpointer) NvBufSurfaceCopy,
    (gpointer) NvBufSurfaceMemSet,
    (gpointer) nvds_create_batch_meta,
    (gpointer) nvds_destroy_batch_meta,
    (gpointer) nvds_acquire_meta_lock,
    (gpointer) nvds_release_meta_lock,
    (gpointer) nvds_acquire_frame_meta_from_pool,
    (gpointer) nvds_add_frame_meta_to_batch,
    (gpointer) nvds_remove_frame_meta_from_batch,
    (gpointer) nvds_acquire_obj_meta_from_pool,
    (gpointer) nvds_add_obj_meta_to_frame,
    (gpointer) nvds_remove_obj_meta_from_frame,
    (gpointer) nvds_acquire_classifier_meta_from_pool,
    (gpointer) nvds_add_classifier_meta_to_object,
    (gpointer) nvds_remove_classifier_meta_from_obj,
    (gpointer) nvds_acquire_label_info_meta_from_pool,
    (gpointer) nvds_add_label_info_meta_to_classifier,
    (gpointer) nvds_remove_label_info_meta_from_classifier,
    (gpointer) nvds_acquire_display_meta_from_pool,
    (gpointer) nvds_add_display_meta_to_frame,
    (gpointer) nvds_remove_display_meta_from_frame,
    (gpointer) nvds_acquire_user_meta_from_pool,
    (gpointer) nvds_add_user_meta_to_batch,
    (gpointer) nvds_add_user_meta_to_frame,
    (gpointer) nvds_add_user_meta_to_obj,
    (gpointer) nvds_remove_user_meta_from_batch,
    (gpointer) nvds_remove_user_meta_from_frame,
    (gpointer) nvds_remove_user_meta_from_object,
    (gpointer) nvds_get_user_meta_type,
    (gpointer) gst_buffer_get_nvds_batch_meta,
    (gpointer) gst_nvevent_new_pad_added,
    (gpointer) gst_nvevent_new_pad_deleted,
    (gpointer) gst_nvevent_new_stream_eos,
    (gpointer) gst_nvevent_new_stream_segment,
    (gpointer) gst_nvevent_new_stream_reset,
    (gpointer) gst_nvevent_new_stream_start,
    (gpointer) gst_nvevent_parse_pad_added,
    (gpointer) gst_nvevent_parse_pad_deleted,
    (gpointer) gst_nvevent_parse_stream_eos,
    (gpointer) gst_nvevent_parse_stream_segment,
    (gpointer) gst_nvevent_parse_stream_reset,
    (gpointer) gst_nvevent_parse_stream_start,
    (gpointer) NvBufSurfTransform,
    (gpointer) NvBufSurfTransformSetSessionParams,
    (gpointer) NvBufSurfTransformGetSessionParams,
    (gpointer) NvBufSurfTransformSetDefaultSession,
  };
  gpointer rds[] = {
    (gpointer) rds_buf_surface_create,
    (gpointer) rds_buf_surface_destroy,
    (gpointer) rds_buf_surface_map,
    (gpointer) rds_buf_surface_unmap,
    (gpointer) rds_buf_surface_sync_for_cpu,
    (gpointer) rds_buf_surface_sync_for_device,
    (gpointer) rds_buf_surface_copy,
    (gpointer) rds_buf_surface_memset,
    (gpointer) rds_create_batch_meta,
    (gpointer) rds_destroy_batch_meta,
    (gpointer) rds_acquire_meta_lock,
    (gpointer) rds_release_meta_lock,
    (gpointer) rds_acquire_frame_meta_from_pool,
    (gpointer) rds_add_frame_meta_to_batch,
    (gpointer) rds_remove_frame_meta_from_batch,
    (gpointer) rds_acquire_obj_meta_from_pool,
    (gpointer) rds_add_obj_meta_to_frame,
    (gpointer) rds_remove_obj_meta_from_frame,
    (gpointer) rds_acquire_classifier_meta_from_pool,
    (gpointer) rds_add_classifier_meta_to_object,
    (gpointer) rds_remove_classifier_meta_from_obj,
    (gpointer) rds_acquire_label_info_meta_from_pool,
    (gpointer) rds_add_label_info_meta_to_classifier,
    (gpointer) rds_remove_label_info_meta_from_classifier,
    (gpointer) rds_acquire_display_meta_from_pool,
    (gpointer) rds_add_display_meta_to_frame,
    (gpointer) rds_remove_display_meta_from_frame,
    (gpointer) rds_acquire_user_meta_from_pool,
    (gpointer) rds_add_user_meta_to_batch,
    (gpointer) rds_add_user_meta_to_frame,
    (gpointer) rds_add_user_meta_to_obj,
    (gpointer) rds_remove_user_meta_from_batch,
    (gpointer) rds_remove_user_meta_from_frame,
    (gpointer) rds_remove_user_meta_from_object,
    (gpointer) rds_get_user_meta_type,
    (gpointer) gst_buffer_get_rds_batch_meta,
    (gpointer) rds_event_new_pad_added,
    (gpointer) rds_event_new_pad_deleted,
    (gpointer) rds_event_new_stream_eos,
    (gpointer) rds_event_new_stream_segment,
    (gpointer) rds_event_new_stream_reset,
    (gpointer) rds_event_new_stream_start,
    (gpointer) rds_event_parse_pad_added,
    (gpointer) rds_event_parse_pad_deleted,
    (gpointer) rds_event_parse_stream_eos,
    (gpointer) rds_event_parse_stream_segment,
    (gpointer) rds_event_parse_stream_reset,
    (gpointer) rds_event_parse_stream_start,
    (gpointer) rds_buf_surf_transform,
    (gpointer) rds_buf_surf_transform_set_session_params,
    (gpointer) rds_buf_surf_transform_get_session_params,
    (gpointer) rds_buf_surf_transform_set_default_session,
  };
  /* The transform functions have DeepStream's exact prototypes: pointers declared with them accept our functions. */
  NvBufSurfTransform_Error (*transform) (NvBufSurface *, NvBufSurface *, NvBufSurfTransformParams *) =
      NvBufSurfTransform;
  NvBufSurfTransform_Error (*set_session) (NvBufSurfTransformConfigParams *) = NvBufSurfTransformSetSessionParams;
  NvBufSurfTransform_Error (*get_session) (NvBufSurfTransformConfigParams *) = NvBufSurfTransformGetSessionParams;
  NvBufSurfTransform_Error (*set_default) (void) = NvBufSurfTransformSetDefaultSession;

  g_assert_true ((gpointer) transform == (gpointer) rds_buf_surf_transform);
  g_assert_true ((gpointer) set_session == (gpointer) rds_buf_surf_transform_set_session_params);
  g_assert_true ((gpointer) get_session == (gpointer) rds_buf_surf_transform_get_session_params);
  g_assert_true ((gpointer) set_default == (gpointer) rds_buf_surf_transform_set_default_session);

  g_assert_cmpuint (G_N_ELEMENTS (nv), ==, G_N_ELEMENTS (rds));
  for (gsize i = 0; i < G_N_ELEMENTS (nv); i++)
    g_assert_true (nv[i] == rds[i]);
}

static void
test_constants (void)
{
  g_assert_cmpint (NVDSINFER_MAX_DIMS, ==, RDS_INFER_MAX_DIMS);
  g_assert_cmpint (FLOAT + HALF + INT8 + INT32 + INT64, ==, 10);
  g_assert_true ((gint64) NVBUF_MEM_DEFAULT == (gint64) RDS_BUF_MEM_DEFAULT);
  g_assert_true ((gint64) NVBUF_MEM_CUDA_PINNED == (gint64) RDS_BUF_MEM_HIP_PINNED);
  g_assert_true ((gint64) NVBUF_MEM_CUDA_DEVICE == (gint64) RDS_BUF_MEM_HIP_DEVICE);
  g_assert_true ((gint64) NVBUF_MEM_CUDA_UNIFIED == (gint64) RDS_BUF_MEM_HIP_UNIFIED);
  g_assert_true ((gint64) NVBUF_MEM_SURFACE_ARRAY == (gint64) RDS_BUF_MEM_SURFACE_ARRAY);
  g_assert_true ((gint64) NVBUF_MEM_HANDLE == (gint64) RDS_BUF_MEM_HANDLE);
  g_assert_true ((gint64) NVBUF_MEM_SYSTEM == (gint64) RDS_BUF_MEM_SYSTEM);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_INVALID == (gint64) RDS_BUF_COLOR_FORMAT_INVALID);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_GRAY8 == (gint64) RDS_BUF_COLOR_FORMAT_GRAY8);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_YUV420 == (gint64) RDS_BUF_COLOR_FORMAT_YUV420);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_NV12 == (gint64) RDS_BUF_COLOR_FORMAT_NV12);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_RGBA == (gint64) RDS_BUF_COLOR_FORMAT_RGBA);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_BGRA == (gint64) RDS_BUF_COLOR_FORMAT_BGRA);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_RGBx == (gint64) RDS_BUF_COLOR_FORMAT_RGBx);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_BGRx == (gint64) RDS_BUF_COLOR_FORMAT_BGRx);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_RGB == (gint64) RDS_BUF_COLOR_FORMAT_RGB);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_BGR == (gint64) RDS_BUF_COLOR_FORMAT_BGR);
  g_assert_true ((gint64) NVBUF_COLOR_FORMAT_LAST == (gint64) RDS_BUF_COLOR_FORMAT_LAST);
  g_assert_true ((gint64) NVBUF_LAYOUT_PITCH == (gint64) RDS_BUF_LAYOUT_PITCH);
  g_assert_true ((gint64) NVBUF_LAYOUT_BLOCK_LINEAR == (gint64) RDS_BUF_LAYOUT_BLOCK_LINEAR);
  g_assert_true ((gint64) NVBUF_MAP_READ == (gint64) RDS_BUF_MAP_READ);
  g_assert_true ((gint64) NVBUF_MAP_WRITE == (gint64) RDS_BUF_MAP_WRITE);
  g_assert_true ((gint64) NVBUF_MAP_READ_WRITE == (gint64) RDS_BUF_MAP_READ_WRITE);
  g_assert_true ((gint64) START_HEAD == (gint64) RDS_OSD_ARROW_START_HEAD);
  g_assert_true ((gint64) END_HEAD == (gint64) RDS_OSD_ARROW_END_HEAD);
  g_assert_true ((gint64) BOTH_HEAD == (gint64) RDS_OSD_ARROW_BOTH_HEAD);
  g_assert_true ((gint64) NVDS_INVALID_META == (gint64) RDS_INVALID_META);
  g_assert_true ((gint64) NVDS_BATCH_META == (gint64) RDS_BATCH_META);
  g_assert_true ((gint64) NVDS_FRAME_META == (gint64) RDS_FRAME_META);
  g_assert_true ((gint64) NVDS_OBJ_META == (gint64) RDS_OBJ_META);
  g_assert_true ((gint64) NVDS_DISPLAY_META == (gint64) RDS_DISPLAY_META);
  g_assert_true ((gint64) NVDS_CLASSIFIER_META == (gint64) RDS_CLASSIFIER_META);
  g_assert_true ((gint64) NVDS_LABEL_INFO_META == (gint64) RDS_LABEL_INFO_META);
  g_assert_true ((gint64) NVDS_USER_META == (gint64) RDS_USER_META);
  g_assert_true ((gint64) NVDSINFER_TENSOR_OUTPUT_META == (gint64) RDS_INFER_TENSOR_OUTPUT_META);
  /* the documented position in NvDsMetaType */
  g_assert_cmpint (RDS_INFER_TENSOR_OUTPUT_META, ==, 12);
  g_assert_true ((gint64) NVDS_TRACKER_PAST_FRAME_META == (gint64) RDS_TRACKER_PAST_FRAME_META);
  g_assert_true ((gint64) NVDS_TRACKER_BATCH_REID_META == (gint64) RDS_TRACKER_BATCH_REID_META);
  g_assert_true ((gint64) NVDS_TRACKER_OBJ_REID_META == (gint64) RDS_TRACKER_OBJ_REID_META);
  g_assert_true ((gint64) NVDS_TRACKER_TERMINATED_LIST_META == (gint64) RDS_TRACKER_TERMINATED_LIST_META);
  g_assert_true ((gint64) NVDS_TRACKER_SHADOW_LIST_META == (gint64) RDS_TRACKER_SHADOW_LIST_META);
  g_assert_true ((gint64) NVDS_OBJ_VISIBILITY == (gint64) RDS_OBJ_VISIBILITY);
  g_assert_true ((gint64) NVDS_OBJ_IMAGE_FOOT_LOCATION == (gint64) RDS_OBJ_IMAGE_FOOT_LOCATION);
  g_assert_true ((gint64) NVDS_OBJ_WORLD_FOOT_LOCATION == (gint64) RDS_OBJ_WORLD_FOOT_LOCATION);
  g_assert_true ((gint64) NVDS_OBJ_IMAGE_CONVEX_HULL == (gint64) RDS_OBJ_IMAGE_CONVEX_HULL);
  /* the tracker's documented positions in NvDsMetaType */
  g_assert_cmpint (RDS_TRACKER_PAST_FRAME_META, ==, 15);
  g_assert_cmpint (RDS_TRACKER_BATCH_REID_META, ==, 16);
  g_assert_cmpint (RDS_TRACKER_OBJ_REID_META, ==, 17);
  g_assert_cmpint (RDS_TRACKER_TERMINATED_LIST_META, ==, 18);
  g_assert_cmpint (RDS_TRACKER_SHADOW_LIST_META, ==, 19);
  g_assert_cmpint (RDS_OBJ_VISIBILITY, ==, 20);
  g_assert_cmpint (RDS_OBJ_IMAGE_FOOT_LOCATION, ==, 21);
  g_assert_cmpint (RDS_OBJ_WORLD_FOOT_LOCATION, ==, 22);
  g_assert_cmpint (RDS_OBJ_IMAGE_CONVEX_HULL, ==, 23);
  /* tracker target states: only the type name is aliased (the bare enumerators would clash with user code) */
  g_assert_cmpint (RDS_TRACKER_STATE_EMPTY, ==, 0);
  g_assert_cmpint (RDS_TRACKER_STATE_ACTIVE, ==, 1);
  g_assert_cmpint (RDS_TRACKER_STATE_INACTIVE, ==, 2);
  g_assert_cmpint (RDS_TRACKER_STATE_TENTATIVE, ==, 3);
  g_assert_cmpint (RDS_TRACKER_STATE_PROJECTED, ==, 4);
  g_assert_cmpint (RDS_TRACKER_STATE_QUASIACTIVE, ==, 5);
  g_assert_cmpuint (sizeof (TRACKER_STATE), ==, sizeof (RdsTrackerState));
  g_assert_cmpint (EMPTY + ACTIVE + INACTIVE + TENTATIVE + PROJECTED + QUASIACTIVE, ==, 15);
  g_assert_true ((gint64) NVDS_RESERVED_META == (gint64) RDS_RESERVED_META);
  g_assert_true ((gint64) NVDS_GST_CUSTOM_META == (gint64) RDS_GST_CUSTOM_META);
  g_assert_true ((gint64) NVDS_BATCH_GST_META == (gint64) RDS_BATCH_GST_META);
  g_assert_true ((gint64) NVDS_START_USER_META == (gint64) RDS_START_USER_META);
  g_assert_true ((gint64) NVDS_FORCE32_META == (gint64) RDS_FORCE32_META);
  g_assert_true ((gint64) MAX_LABEL_SIZE == (gint64) RDS_MAX_LABEL_SIZE);
  g_assert_true ((gint64) MAX_ELEMENTS_IN_DISPLAY_META == (gint64) RDS_MAX_ELEMENTS_IN_DISPLAY_META);
  g_assert_true ((gint64) MAX_USER_FIELDS == (gint64) RDS_MAX_USER_FIELDS);
  g_assert_true ((gint64) MAX_RESERVED_FIELDS == (gint64) RDS_MAX_RESERVED_FIELDS);
  g_assert_true ((gint64) UNTRACKED_OBJECT_ID == (gint64) RDS_UNTRACKED_OBJECT_ID);
  g_assert_true ((gint64) GST_NVEVENT_PAD_ADDED == (gint64) RDS_EVENT_PAD_ADDED);
  g_assert_true ((gint64) GST_NVEVENT_PAD_DELETED == (gint64) RDS_EVENT_PAD_DELETED);
  g_assert_true ((gint64) GST_NVEVENT_STREAM_EOS == (gint64) RDS_EVENT_STREAM_EOS);
  g_assert_true ((gint64) GST_NVEVENT_STREAM_SEGMENT == (gint64) RDS_EVENT_STREAM_SEGMENT);
  g_assert_true ((gint64) GST_NVEVENT_STREAM_RESET == (gint64) RDS_EVENT_STREAM_RESET);
  g_assert_true ((gint64) GST_NVEVENT_STREAM_START == (gint64) RDS_EVENT_STREAM_START);
  g_assert_true ((gint64) NvBufSurfTransformCompute_Default == (gint64) RdsBufSurfTransformCompute_Default);
  g_assert_true ((gint64) NvBufSurfTransformCompute_GPU == (gint64) RdsBufSurfTransformCompute_GPU);
  g_assert_true ((gint64) NvBufSurfTransformCompute_VIC == (gint64) RdsBufSurfTransformCompute_VIC);
  g_assert_true ((gint64) NvBufSurfTransform_None == (gint64) RdsBufSurfTransform_None);
  g_assert_true ((gint64) NvBufSurfTransform_Rotate90 == (gint64) RdsBufSurfTransform_Rotate90);
  g_assert_true ((gint64) NvBufSurfTransform_Rotate180 == (gint64) RdsBufSurfTransform_Rotate180);
  g_assert_true ((gint64) NvBufSurfTransform_Rotate270 == (gint64) RdsBufSurfTransform_Rotate270);
  g_assert_true ((gint64) NvBufSurfTransform_FlipX == (gint64) RdsBufSurfTransform_FlipX);
  g_assert_true ((gint64) NvBufSurfTransform_FlipY == (gint64) RdsBufSurfTransform_FlipY);
  g_assert_true ((gint64) NvBufSurfTransform_Transpose == (gint64) RdsBufSurfTransform_Transpose);
  g_assert_true ((gint64) NvBufSurfTransform_InvTranspose == (gint64) RdsBufSurfTransform_InvTranspose);
  g_assert_true ((gint64) NvBufSurfTransformInter_Nearest == (gint64) RdsBufSurfTransformInter_Nearest);
  g_assert_true ((gint64) NvBufSurfTransformInter_Bilinear == (gint64) RdsBufSurfTransformInter_Bilinear);
  g_assert_true ((gint64) NvBufSurfTransformInter_Algo1 == (gint64) RdsBufSurfTransformInter_Algo1);
  g_assert_true ((gint64) NvBufSurfTransformInter_Algo2 == (gint64) RdsBufSurfTransformInter_Algo2);
  g_assert_true ((gint64) NvBufSurfTransformInter_Algo3 == (gint64) RdsBufSurfTransformInter_Algo3);
  g_assert_true ((gint64) NvBufSurfTransformInter_Algo4 == (gint64) RdsBufSurfTransformInter_Algo4);
  g_assert_true ((gint64) NvBufSurfTransformInter_Default == (gint64) RdsBufSurfTransformInter_Default);
  g_assert_true ((gint64) NvBufSurfTransformError_ROI_Error == (gint64) RdsBufSurfTransformError_ROI_Error);
  g_assert_true ((gint64) NvBufSurfTransformError_Invalid_Params == (gint64) RdsBufSurfTransformError_Invalid_Params);
  g_assert_true ((gint64) NvBufSurfTransformError_Execution_Error
      == (gint64) RdsBufSurfTransformError_Execution_Error);
  g_assert_true ((gint64) NvBufSurfTransformError_Unsupported == (gint64) RdsBufSurfTransformError_Unsupported);
  g_assert_true ((gint64) NvBufSurfTransformError_Success == (gint64) RdsBufSurfTransformError_Success);
  g_assert_true ((gint64) NVBUFSURF_TRANSFORM_CROP_SRC == (gint64) RDS_BUFSURF_TRANSFORM_CROP_SRC);
  g_assert_true ((gint64) NVBUFSURF_TRANSFORM_CROP_DST == (gint64) RDS_BUFSURF_TRANSFORM_CROP_DST);
  g_assert_true ((gint64) NVBUFSURF_TRANSFORM_FILTER == (gint64) RDS_BUFSURF_TRANSFORM_FILTER);
  g_assert_true ((gint64) NVBUFSURF_TRANSFORM_FLIP == (gint64) RDS_BUFSURF_TRANSFORM_FLIP);
  g_assert_true ((gint64) NVBUFSURF_TRANSFORM_NORMALIZE == (gint64) RDS_BUFSURF_TRANSFORM_NORMALIZE);
  g_assert_true ((gint64) NVBUFSURF_TRANSFORM_ALLOW_ODD_CROP == (gint64) RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP);

  /* Values DeepStream's public reference documents and configs or apps rely on. */
  g_assert_cmpint (NVBUF_MEM_DEFAULT, ==, 0);
  g_assert_cmpint (NVBUF_MEM_CUDA_PINNED, ==, 1);
  g_assert_cmpint (NVBUF_MEM_CUDA_DEVICE, ==, 2);
  g_assert_cmpint (NVBUF_MEM_CUDA_UNIFIED, ==, 3);
  g_assert_cmpint (NVBUF_MEM_SURFACE_ARRAY, ==, 4);
  g_assert_cmpint (NVBUF_MEM_HANDLE, ==, 5);
  g_assert_cmpint (NVBUF_MEM_SYSTEM, ==, 6);
  g_assert_cmpint (MAX_LABEL_SIZE, ==, 128);
  g_assert_cmpint (MAX_ELEMENTS_IN_DISPLAY_META, ==, 16);
  g_assert_cmpint (MAX_USER_FIELDS, ==, 4);
  g_assert_cmpint (MAX_RESERVED_FIELDS, ==, 4);
  g_assert_true (UNTRACKED_OBJECT_ID == G_GUINT64_CONSTANT (0xFFFFFFFFFFFFFFFF));
  g_assert_cmpint (NVDS_START_USER_META, ==, NVDS_GST_CUSTOM_META + 4096 + 1);
  g_assert_cmpint (NvBufSurfTransformCompute_Default, ==, 0);
  g_assert_cmpint (NvBufSurfTransformCompute_GPU, ==, 1);
  g_assert_cmpint (NvBufSurfTransformCompute_VIC, ==, 2);
  g_assert_cmpint (NvBufSurfTransform_None, ==, 0);
  g_assert_cmpint (NvBufSurfTransform_Rotate90, ==, 1);
  g_assert_cmpint (NvBufSurfTransform_Rotate180, ==, 2);
  g_assert_cmpint (NvBufSurfTransform_Rotate270, ==, 3);
  g_assert_cmpint (NvBufSurfTransform_FlipX, ==, 4);
  g_assert_cmpint (NvBufSurfTransform_FlipY, ==, 5);
  g_assert_cmpint (NvBufSurfTransform_Transpose, ==, 6);
  g_assert_cmpint (NvBufSurfTransform_InvTranspose, ==, 7);
  g_assert_cmpint (NvBufSurfTransformInter_Nearest, ==, 0);
  g_assert_cmpint (NvBufSurfTransformInter_Bilinear, ==, 1);
  g_assert_cmpint (NvBufSurfTransformInter_Algo1, ==, 2);
  g_assert_cmpint (NvBufSurfTransformInter_Algo2, ==, 3);
  g_assert_cmpint (NvBufSurfTransformInter_Algo3, ==, 4);
  g_assert_cmpint (NvBufSurfTransformInter_Algo4, ==, 5);
  g_assert_cmpint (NvBufSurfTransformInter_Default, ==, 6);
  g_assert_cmpint (NvBufSurfTransformError_ROI_Error, ==, -4);
  g_assert_cmpint (NvBufSurfTransformError_Invalid_Params, ==, -3);
  g_assert_cmpint (NvBufSurfTransformError_Execution_Error, ==, -2);
  g_assert_cmpint (NvBufSurfTransformError_Unsupported, ==, -1);
  g_assert_cmpint (NvBufSurfTransformError_Success, ==, 0);
  g_assert_cmpint (NVBUFSURF_TRANSFORM_CROP_SRC, ==, 1);
  g_assert_cmpint (NVBUFSURF_TRANSFORM_CROP_DST, ==, 2);
  g_assert_cmpint (NVBUFSURF_TRANSFORM_FILTER, ==, 4);
  g_assert_cmpint (NVBUFSURF_TRANSFORM_FLIP, ==, 8);
  g_assert_cmpint (NVBUFSURF_TRANSFORM_NORMALIZE, ==, 16);
  g_assert_cmpint (NVBUFSURF_TRANSFORM_ALLOW_ODD_CROP, ==, 32);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/compat-aliases/types", test_types);
  g_test_add_func ("/compat-aliases/functions", test_functions);
  g_test_add_func ("/compat-aliases/constants", test_constants);
  g_test_add_func ("/compat-aliases/transform-structs", test_transform_structs);
  return g_test_run ();
}
