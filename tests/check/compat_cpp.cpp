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
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/compat-cpp/aliases", test_cpp_aliases);
  return g_test_run ();
}
