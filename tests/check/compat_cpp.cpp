/* Source-compatibility check: DeepStream-style C++ compiled against rds/rds_compat.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds_compat.h>

static void
test_cpp_aliases (void)
{
  NvDsBatchMeta *batch_meta = nvds_create_batch_meta (1);
  NvDsFrameMeta *frame_meta = nvds_acquire_frame_meta_from_pool (batch_meta);
  NvDsObjectMeta *obj_meta = nvds_acquire_obj_meta_from_pool (batch_meta);
  NvBufSurfaceCreateParams params = {};
  NvBufSurface *surf = nullptr;
  GstEvent *ev = gst_nvevent_new_pad_added (7);
  guint source_id = 0;

  nvds_add_frame_meta_to_batch (batch_meta, frame_meta);
  obj_meta->rect_params.border_color = NvOSD_ColorParams { 0.0, 1.0, 0.0, 1.0 };
  g_strlcpy (obj_meta->obj_label, "person", MAX_LABEL_SIZE);
  nvds_add_obj_meta_to_frame (frame_meta, obj_meta, nullptr);
  g_assert_cmpuint (g_list_length (frame_meta->obj_meta_list), ==, 1);
  nvds_destroy_batch_meta (batch_meta);

  params.width = 64;
  params.height = 32;
  params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  params.layout = NVBUF_LAYOUT_PITCH;
  params.memType = NVBUF_MEM_SYSTEM;
  g_assert_cmpint (NvBufSurfaceCreate (&surf, 1, &params), ==, 0);
  g_assert_cmpint (NvBufSurfaceDestroy (surf), ==, 0);

  switch ((GstNvEventType) GST_EVENT_TYPE (ev)) {
    case GST_NVEVENT_PAD_ADDED:
      gst_nvevent_parse_pad_added (ev, &source_id);
      break;
    default:
      g_assert_not_reached ();
  }
  g_assert_cmpuint (source_id, ==, 7);
  gst_event_unref (ev);

  NvOSD_Arrow_Head_Direction head = BOTH_HEAD;
  g_assert_cmpint (head, ==, RDS_OSD_ARROW_BOTH_HEAD);

  /* the tracker state type alias, with Redstream's enumerators */
  TRACKER_STATE state = RDS_TRACKER_STATE_ACTIVE;
  g_assert_cmpint (state, ==, 1);
}

/* DeepStream-style transform and composite calls compiled as C++. The source batch is empty (numFilled = 0), so
 * the calls fail validation and never reach the GPU: this test needs no device. */
static void
test_cpp_transform (void)
{
  NvBufSurfaceCreateParams create_params = {};
  NvBufSurface *src = nullptr, *dst = nullptr;
  NvBufSurfTransformConfigParams config = {};
  NvBufSurfTransformConfigParams got = {};
  NvBufSurfTransformParams transform_params = {};
  NvBufSurfTransformRect src_rect = { 0, 0, 32, 16 };

  config.compute_mode = NvBufSurfTransformCompute_GPU;
  config.gpu_id = 0;
  config.cuda_stream = nullptr;
  g_assert_cmpint (NvBufSurfTransformSetSessionParams (&config), ==, NvBufSurfTransformError_Success);
  g_assert_cmpint (NvBufSurfTransformGetSessionParams (&got), ==, NvBufSurfTransformError_Success);
  g_assert_cmpint (got.compute_mode, ==, NvBufSurfTransformCompute_GPU);

  create_params.width = 64;
  create_params.height = 32;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_NV12;
  create_params.layout = NVBUF_LAYOUT_PITCH;
  create_params.memType = NVBUF_MEM_SYSTEM;
  g_assert_cmpint (NvBufSurfaceCreate (&src, 1, &create_params), ==, 0);
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  g_assert_cmpint (NvBufSurfaceCreate (&dst, 1, &create_params), ==, 0);
  src->numFilled = 0;

  transform_params.transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_FILTER;
  transform_params.transform_flip = NvBufSurfTransform_None;
  transform_params.transform_filter = NvBufSurfTransformInter_Bilinear;
  transform_params.src_rect = &src_rect;

  NvBufSurfTransform_Error err = NvBufSurfTransform (src, dst, &transform_params);
  g_assert_cmpint (err, ==, NvBufSurfTransformError_Invalid_Params);
  g_assert_cmpuint (dst->numFilled, ==, 0);

  /* the composite, rejected the same way */
  NvBufSurfTransformRect dst_rect = { 0, 0, 32, 16 };
  NvBufSurfTransformCompositeParams composite_params = {};
  composite_params.composite_flag = NVBUFSURF_TRANSFORM_COMPOSITE;
  composite_params.input_buf_count = 1;
  composite_params.src_comp_rect = &src_rect;
  composite_params.dst_comp_rect = &dst_rect;
  composite_params.composite_filter = NvBufSurfTransformInter_Bilinear;
  err = NvBufSurfTransformComposite (src, dst, &composite_params);
  g_assert_cmpint (err, ==, NvBufSurfTransformError_Invalid_Params);

  g_assert_cmpint (NvBufSurfaceDestroy (src), ==, 0);
  g_assert_cmpint (NvBufSurfaceDestroy (dst), ==, 0);
  g_assert_cmpint (NvBufSurfTransformSetDefaultSession (), ==, NvBufSurfTransformError_Success);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/compat-cpp/aliases", test_cpp_aliases);
  g_test_add_func ("/compat-cpp/transform", test_cpp_transform);
  return g_test_run ();
}
