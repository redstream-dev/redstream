/* Source-compatibility check: DeepStream-style code compiled against rds/rds_compat.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#define USE_REDSTREAM 1
#include <rds/rds_compat.h>

/* A probe-style walker, as an app would write it. */
static guint
count_objects (GstBuffer * buf, gint class_id)
{
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buf);
  guint n = 0;

  for (NvDsMetaList * lf = batch_meta->frame_meta_list; lf != NULL; lf = lf->next) {
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) lf->data;
    for (NvDsMetaList * lo = frame_meta->obj_meta_list; lo != NULL; lo = lo->next) {
      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) lo->data;
      if (obj_meta->class_id == class_id)
        n++;
    }
  }
  return n;
}

static void
test_meta_api (void)
{
  NvDsBatchMeta *batch_meta = nvds_create_batch_meta (1);
  NvDsFrameMeta *frame_meta = nvds_acquire_frame_meta_from_pool (batch_meta);
  NvDsObjectMeta *obj_meta;
  NvDsDisplayMeta *display_meta;
  NvDsUserMeta *user_meta;
  GstBuffer *buf = gst_buffer_new ();

  nvds_add_frame_meta_to_batch (batch_meta, frame_meta);
  for (gint i = 0; i < 3; i++) {
    obj_meta = nvds_acquire_obj_meta_from_pool (batch_meta);
    obj_meta->class_id = i == 2 ? 0 : 2;
    obj_meta->object_id = UNTRACKED_OBJECT_ID;
    obj_meta->rect_params.border_color = (NvOSD_ColorParams) {1.0, 0.0, 0.0, 1.0};
    g_strlcpy (obj_meta->obj_label, "car", MAX_LABEL_SIZE);
    nvds_add_obj_meta_to_frame (frame_meta, obj_meta, NULL);
  }

  display_meta = nvds_acquire_display_meta_from_pool (batch_meta);
  NvOSD_TextParams *txt = &display_meta->text_params[0];
  txt->display_text = g_strdup ("Vehicles: 2");
  txt->font_params.font_name = (gchar *) "Serif";
  display_meta->num_labels = 1;
  g_assert_cmpuint (MAX_ELEMENTS_IN_DISPLAY_META, ==, 16);
  nvds_add_display_meta_to_frame (frame_meta, display_meta);

  user_meta = nvds_acquire_user_meta_from_pool (batch_meta);
  user_meta->base_meta.meta_type = nvds_get_user_meta_type ((gchar *) "EXAMPLE.COMPAT.USER_META");
  g_assert_cmpint (user_meta->base_meta.meta_type, >=, NVDS_START_USER_META);
  nvds_add_user_meta_to_frame (frame_meta, user_meta);

  rds_buffer_add_batch_meta (buf, batch_meta);      /* done by the mux in a real pipeline */
  g_assert_cmpuint (count_objects (buf, 2), ==, 2);

  nvds_acquire_meta_lock (batch_meta);
  nvds_remove_obj_meta_from_frame (frame_meta, (NvDsObjectMeta *) frame_meta->obj_meta_list->data);
  nvds_release_meta_lock (batch_meta);
  g_assert_cmpuint (count_objects (buf, 2), ==, 1);
  gst_buffer_unref (buf);
}

static void
test_surface_api (void)
{
  NvBufSurfaceCreateParams params = {
    .width = 320, .height = 240, .colorFormat = NVBUF_COLOR_FORMAT_NV12,
    .layout = NVBUF_LAYOUT_PITCH, .memType = NVBUF_MEM_SYSTEM,
  };
  NvBufSurface *surf = NULL;

  g_assert_cmpint (NVBUF_MEM_CUDA_UNIFIED, ==, 3);  /* integers used by nvbuf-memory-type configs */
  g_assert_cmpint (NvBufSurfaceCreate (&surf, 1, &params), ==, 0);
  g_assert_cmpint (NvBufSurfaceMemSet (surf, 0, -1, 0x10), ==, 0);
  g_assert_cmpint (NvBufSurfaceMap (surf, 0, 0, NVBUF_MAP_READ), ==, 0);
  g_assert_cmpint (NvBufSurfaceSyncForCpu (surf, 0, 0), ==, 0);
  g_assert_cmpuint (((guint8 *) surf->surfaceList[0].mappedAddr.addr[0])[0], ==, 0x10);
  g_assert_cmpint (NvBufSurfaceUnMap (surf, 0, 0), ==, 0);
  g_assert_cmpint (NvBufSurfaceDestroy (surf), ==, 0);
}

static void
test_event_api (void)
{
  GstEvent *ev = gst_nvevent_new_stream_eos (4);
  guint source_id = 0;

  /* Cast as DeepStream apps must: an uncast switch on GstEventType trips -Wswitch (same on DeepStream). */
  switch ((GstNvEventType) GST_EVENT_TYPE (ev)) {
    case GST_NVEVENT_STREAM_EOS:
      gst_nvevent_parse_stream_eos (ev, &source_id);
      break;
    case GST_NVEVENT_PAD_ADDED:
    case GST_NVEVENT_PAD_DELETED:
    default:
      g_assert_not_reached ();
  }
  g_assert_cmpuint (source_id, ==, 4);
  gst_event_unref (ev);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/compat/meta-api", test_meta_api);
  g_test_add_func ("/compat/surface-api", test_surface_api);
  g_test_add_func ("/compat/event-api", test_event_api);
  return g_test_run ();
}
