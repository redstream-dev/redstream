/* Source compatibility of the analytics metas: DeepStream-style C++ that reads (and writes) them through the
 * DeepStream names, with nothing but the compat header and the analytics meta header.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds_compat.h>
#include <rds/rds-analytics-meta.h>

/* A probe-style walk: the frame's user metas, then each object's. */
static void
read_probe_style (NvDsFrameMeta * frame_meta, guint * frames_seen, guint * objs_seen)
{
  for (NvDsMetaList * l = frame_meta->frame_user_meta_list; l != nullptr; l = l->next) {
    NvDsUserMeta *user_meta = (NvDsUserMeta *) l->data;

    if (user_meta->base_meta.meta_type != NVDS_USER_FRAME_META_NVDSANALYTICS)
      continue;
    NvDsAnalyticsFrameMeta *meta = (NvDsAnalyticsFrameMeta *) user_meta->user_meta_data;

    g_assert_cmpuint (meta->objInROIcnt["RF"], ==, 3);
    g_assert_cmpuint (meta->objInROIcnt.size (), ==, 1);
    g_assert_true (meta->ocStatus["OC"]);
    guint lines = 0;
    for (const auto &kv : meta->objLCCumCnt) {
      g_assert_cmpstr (kv.first.c_str (), ==, "Exit");
      g_assert_cmpuint (kv.second, ==, 9);
      g_assert_cmpuint (meta->objLCCurrCnt[kv.first], ==, 1);
      lines++;
    }
    g_assert_cmpuint (lines, ==, 1);
    g_assert_cmpuint (meta->objCnt[2], ==, 5);
    g_assert_cmpuint (meta->unique_id, ==, 17);
    (*frames_seen)++;
  }
  for (NvDsMetaList * l = frame_meta->obj_meta_list; l != nullptr; l = l->next) {
    NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) l->data;

    for (NvDsMetaList * u = obj_meta->obj_user_meta_list; u != nullptr; u = u->next) {
      NvDsUserMeta *user_meta = (NvDsUserMeta *) u->data;

      if (user_meta->base_meta.meta_type != NVDS_USER_OBJ_META_NVDSANALYTICS)
        continue;
      NvDsAnalyticsObjInfo *info = (NvDsAnalyticsObjInfo *) user_meta->user_meta_data;

      g_assert_cmpuint (info->roiStatus.size (), ==, 1);
      g_assert_cmpstr (info->roiStatus[0].c_str (), ==, "RF");
      g_assert_cmpstr (info->ocStatus.at (0).c_str (), ==, "OC");
      for (const std::string &line : info->lcStatus)
        g_assert_cmpstr (line.c_str (), ==, "Exit");
      g_assert_true (info->dirStatus.empty ());
      g_assert_cmpstr (info->objStatus.c_str (), ==, " ROI:RF  LC:Exit ");
      g_assert_cmpuint (info->unique_id, ==, 17);
      (*objs_seen)++;
    }
  }
}

static void
test_probe_read (void)
{
  NvDsBatchMeta *batch_meta = nvds_create_batch_meta (1);
  NvDsFrameMeta *frame_meta = nvds_acquire_frame_meta_from_pool (batch_meta);
  NvDsObjectMeta *obj_meta = nvds_acquire_obj_meta_from_pool (batch_meta);
  guint frames_seen = 0, objs_seen = 0;

  nvds_add_frame_meta_to_batch (batch_meta, frame_meta);
  nvds_add_obj_meta_to_frame (frame_meta, obj_meta, nullptr);

  /* Written half through the C API, half through the struct: one object either way. */
  RdsAnalyticsFrameMeta *fm = rds_analytics_frame_meta_attach (frame_meta, 17);
  g_assert_nonnull (fm);
  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "RF", 3));
  g_assert_true (rds_analytics_frame_meta_set_lc_counts (fm, "Exit", 1, 9));
  fm->ocStatus["OC"] = true;
  fm->objCnt[2] = 5;

  RdsAnalyticsObjInfo *oi = rds_analytics_obj_info_attach (obj_meta, 17);
  g_assert_nonnull (oi);
  g_assert_true (rds_analytics_obj_info_add_roi_status (oi, "RF"));
  oi->ocStatus.push_back ("OC");
  oi->lcStatus.push_back ("Exit");
  oi->objStatus = " ROI:RF  LC:Exit ";

  read_probe_style (frame_meta, &frames_seen, &objs_seen);
  g_assert_cmpuint (frames_seen, ==, 1);
  g_assert_cmpuint (objs_seen, ==, 1);

  /* the C getters see the struct writes */
  gboolean oc = FALSE;
  g_assert_true (rds_analytics_frame_meta_get_oc_status (fm, "OC", &oc));
  g_assert_true (oc);
  g_assert_cmpstr (rds_analytics_obj_info_get_lc_status (oi, 0), ==, "Exit");

  /* deep copies read the same way */
  NvDsBatchMeta *copy = rds_batch_meta_copy (batch_meta);
  nvds_destroy_batch_meta (batch_meta);
  frames_seen = objs_seen = 0;
  read_probe_style ((NvDsFrameMeta *) copy->frame_meta_list->data, &frames_seen, &objs_seen);
  g_assert_cmpuint (frames_seen, ==, 1);
  g_assert_cmpuint (objs_seen, ==, 1);
  nvds_destroy_batch_meta (copy);
}

/* The DeepStream names are the Redstream ones; 8.0 makes the user meta types the enum values 32 and 33. */
static void
test_names (void)
{
  static_assert (NVDS_FRAME_META_NVDSANALYTICS == 32, "");
  static_assert (NVDS_OBJ_META_NVDSANALYTICS == 33, "");
  static_assert (NVDS_USER_FRAME_META_NVDSANALYTICS == NVDS_FRAME_META_NVDSANALYTICS, "");
  static_assert (NVDS_USER_OBJ_META_NVDSANALYTICS == NVDS_OBJ_META_NVDSANALYTICS, "");
  NvDsMetaType t = NVDS_USER_FRAME_META_NVDSANALYTICS;
  g_assert_cmpint (t, ==, RDS_FRAME_META_ANALYTICS);

  NvDsAnalyticsFrameMeta frame;
  RdsAnalyticsFrameMeta *rf = &frame;
  NvDsAnalyticsObjInfo obj;
  RdsAnalyticsObjInfo *ro = &obj;
  /* the documented field types */
  std::unordered_map<std::string, bool> &oc = rf->ocStatus;
  std::unordered_map<std::string, uint32_t> &roi = rf->objInROIcnt;
  std::unordered_map<std::string, uint64_t> &curr = rf->objLCCurrCnt;
  std::unordered_map<std::string, uint64_t> &cum = rf->objLCCumCnt;
  std::unordered_map<int, uint32_t> &cnt = rf->objCnt;
  guint &fid = rf->unique_id;
  std::vector<std::string> &rs = ro->roiStatus;
  std::vector<std::string> &os = ro->ocStatus;
  std::vector<std::string> &ls = ro->lcStatus;
  std::string &dir = ro->dirStatus;
  std::string &st = ro->objStatus;
  guint &oid = ro->unique_id;

  g_assert_true (oc.empty () && roi.empty () && curr.empty () && cum.empty () && cnt.empty ());
  g_assert_true (rs.empty () && os.empty () && ls.empty () && dir.empty () && st.empty ());
  g_assert_cmpuint (fid, ==, 0);
  g_assert_cmpuint (oid, ==, 0);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, nullptr);
  g_test_add_func ("/cpu/analytics-meta-compat/probe-read", test_probe_read);
  g_test_add_func ("/cpu/analytics-meta-compat/names", test_names);
  return g_test_run ();
}
