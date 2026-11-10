/* The analytics metas through their C API (libgstrds-analytics): attach, write, read back, deep copies through
 * rds_batch_meta_copy, and release (leaks and double frees show under ASan).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <stdlib.h>

#include <rds/rds-analytics-meta.h>
#include <rds/rds-meta.h>

static RdsUserMeta *
find_user (RdsUserMetaList * list, RdsMetaType type)
{
  RdsUserMeta *found = NULL;

  for (RdsUserMetaList * l = list; l != NULL; l = l->next) {
    RdsUserMeta *um = l->data;

    if (um->base_meta.meta_type == type) {
      g_assert_null (found);
      found = um;
    }
  }
  return found;
}

static RdsFrameMeta *
add_frame (RdsBatchMeta * b)
{
  RdsFrameMeta *f = rds_acquire_frame_meta_from_pool (b);

  rds_add_frame_meta_to_batch (b, f);
  return f;
}

static RdsObjectMeta *
add_obj (RdsFrameMeta * f)
{
  RdsObjectMeta *o = rds_acquire_obj_meta_from_pool (f->base_meta.batch_meta);

  rds_add_obj_meta_to_frame (f, o, NULL);
  return o;
}

/* A frame meta with one entry of each kind, plus a second ROI and class. */
static RdsAnalyticsFrameMeta *
fill_frame (RdsFrameMeta * f)
{
  RdsAnalyticsFrameMeta *fm = rds_analytics_frame_meta_attach (f, 17);

  g_assert_nonnull (fm);
  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "RF", 3));
  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "RF2", 0));
  g_assert_true (rds_analytics_frame_meta_set_oc_status (fm, "OC", TRUE));
  g_assert_true (rds_analytics_frame_meta_set_lc_counts (fm, "Exit", 1, G_GUINT64_CONSTANT (5000000000)));
  g_assert_true (rds_analytics_frame_meta_set_obj_count (fm, 0, 4));
  g_assert_true (rds_analytics_frame_meta_set_obj_count (fm, -1, 2));
  return fm;
}

static RdsAnalyticsObjInfo *
fill_obj (RdsObjectMeta * o)
{
  RdsAnalyticsObjInfo *oi = rds_analytics_obj_info_attach (o, 17);

  g_assert_nonnull (oi);
  g_assert_true (rds_analytics_obj_info_add_roi_status (oi, "RF"));
  g_assert_true (rds_analytics_obj_info_add_roi_status (oi, "RF2"));
  g_assert_true (rds_analytics_obj_info_add_oc_status (oi, "OC"));
  g_assert_true (rds_analytics_obj_info_add_lc_status (oi, "Exit"));
  g_assert_true (rds_analytics_obj_info_set_dir_status (oi, " DIR:South "));
  g_assert_true (rds_analytics_obj_info_set_obj_status (oi, " ROI:RF ROI:RF2 DIR:South  LC:Exit "));
  return oi;
}

static void
check_frame (const RdsAnalyticsFrameMeta * fm)
{
  guint32 n32 = 99;
  guint64 n64 = 99;
  gboolean b = FALSE;

  g_assert_cmpuint (rds_analytics_frame_meta_get_unique_id (fm), ==, 17);
  g_assert_true (rds_analytics_frame_meta_get_roi_count (fm, "RF", &n32));
  g_assert_cmpuint (n32, ==, 3);
  g_assert_true (rds_analytics_frame_meta_get_roi_count (fm, "RF2", &n32));
  g_assert_cmpuint (n32, ==, 0);
  g_assert_true (rds_analytics_frame_meta_get_oc_status (fm, "OC", &b));
  g_assert_true (b);
  g_assert_true (rds_analytics_frame_meta_get_lc_count (fm, "Exit", &n64));
  g_assert_cmpuint (n64, ==, 1);
  g_assert_true (rds_analytics_frame_meta_get_lc_cum_count (fm, "Exit", &n64));
  g_assert_cmpuint (n64, ==, G_GUINT64_CONSTANT (5000000000));
  g_assert_true (rds_analytics_frame_meta_get_obj_count (fm, 0, &n32));
  g_assert_cmpuint (n32, ==, 4);
  g_assert_true (rds_analytics_frame_meta_get_obj_count (fm, -1, &n32));
  g_assert_cmpuint (n32, ==, 2);
}

static void
check_obj (const RdsAnalyticsObjInfo * oi)
{
  g_assert_cmpuint (rds_analytics_obj_info_get_unique_id (oi), ==, 17);
  g_assert_cmpuint (rds_analytics_obj_info_n_roi_status (oi), ==, 2);
  g_assert_cmpstr (rds_analytics_obj_info_get_roi_status (oi, 0), ==, "RF");
  g_assert_cmpstr (rds_analytics_obj_info_get_roi_status (oi, 1), ==, "RF2");
  g_assert_cmpuint (rds_analytics_obj_info_n_oc_status (oi), ==, 1);
  g_assert_cmpstr (rds_analytics_obj_info_get_oc_status (oi, 0), ==, "OC");
  g_assert_cmpuint (rds_analytics_obj_info_n_lc_status (oi), ==, 1);
  g_assert_cmpstr (rds_analytics_obj_info_get_lc_status (oi, 0), ==, "Exit");
  g_assert_cmpstr (rds_analytics_obj_info_get_dir_status (oi), ==, " DIR:South ");
  g_assert_cmpstr (rds_analytics_obj_info_get_obj_status (oi), ==, " ROI:RF ROI:RF2 DIR:South  LC:Exit ");
}

static void
test_types (void)
{
  /* DeepStream 8.0's numbers */
  g_assert_cmpint (RDS_FRAME_META_ANALYTICS, ==, 32);
  g_assert_cmpint (RDS_OBJ_META_ANALYTICS, ==, 33);
}

/* The handle is the user meta's data, typed 32 with copy and release functions, and the getters read back what the
 * setters wrote. */
static void
test_frame (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *f = add_frame (b);
  RdsAnalyticsFrameMeta *fm = fill_frame (f);
  RdsUserMeta *um = find_user (f->frame_user_meta_list, RDS_FRAME_META_ANALYTICS);
  guint32 n32 = 99;
  guint64 n64 = 99;
  gboolean st = TRUE;

  g_assert_nonnull (um);
  g_assert_true (um->user_meta_data == fm);
  g_assert_true (um->base_meta.batch_meta == b);
  g_assert_nonnull (um->base_meta.copy_func);
  g_assert_nonnull (um->base_meta.release_func);
  check_frame (fm);

  /* absent keys: FALSE, and the output is left alone */
  g_assert_false (rds_analytics_frame_meta_get_roi_count (fm, "nope", &n32));
  g_assert_cmpuint (n32, ==, 99);
  g_assert_false (rds_analytics_frame_meta_get_oc_status (fm, "RF", &st));
  g_assert_true (st);
  g_assert_false (rds_analytics_frame_meta_get_lc_count (fm, "RF", &n64));
  g_assert_false (rds_analytics_frame_meta_get_lc_cum_count (fm, "RF", &n64));
  g_assert_cmpuint (n64, ==, 99);
  g_assert_false (rds_analytics_frame_meta_get_obj_count (fm, 7, &n32));
  /* a NULL output only tests for presence */
  g_assert_true (rds_analytics_frame_meta_get_roi_count (fm, "RF", NULL));

  /* setting again replaces */
  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "RF", 8));
  g_assert_true (rds_analytics_frame_meta_set_oc_status (fm, "OC", FALSE));
  g_assert_true (rds_analytics_frame_meta_set_lc_counts (fm, "Exit", 0, 6));
  g_assert_true (rds_analytics_frame_meta_get_roi_count (fm, "RF", &n32));
  g_assert_cmpuint (n32, ==, 8);
  g_assert_true (rds_analytics_frame_meta_get_oc_status (fm, "OC", &st));
  g_assert_false (st);
  g_assert_true (rds_analytics_frame_meta_get_lc_count (fm, "Exit", &n64));
  g_assert_cmpuint (n64, ==, 0);
  g_assert_true (rds_analytics_frame_meta_get_lc_cum_count (fm, "Exit", &n64));
  g_assert_cmpuint (n64, ==, 6);

  rds_destroy_batch_meta (b);
}

static void
collect_label (const gchar * label, guint64 value, gpointer user_data)
{
  GString *s = user_data;

  g_string_append_printf (s, "%s=%" G_GUINT64_FORMAT ";", label, value);
}

static void
collect_status (const gchar * label, gboolean value, gpointer user_data)
{
  GString *s = user_data;

  g_string_append_printf (s, "%s=%d;", label, value);
}

static void
collect_class (gint class_id, guint32 value, gpointer user_data)
{
  GString *s = user_data;

  g_string_append_printf (s, "%d=%u;", class_id, value);
}

static int
cmp_str (const void *a, const void *b)
{
  return g_strcmp0 (*(gchar * const *) a, *(gchar * const *) b);
}

/* The maps' iteration order is unspecified: compare sorted pieces. */
static gchar *
sorted (GString * s)
{
  gchar **parts = g_strsplit (s->str, ";", -1);
  gchar *joined;

  g_assert_cmpuint (g_strv_length (parts), >, 0);
  g_assert_cmpstr (parts[g_strv_length (parts) - 1], ==, "");
  qsort (parts, g_strv_length (parts), sizeof (gchar *), cmp_str);
  joined = g_strjoinv (";", parts);
  g_strfreev (parts);
  g_string_truncate (s, 0);
  return joined;
}

static void
test_foreach (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsAnalyticsFrameMeta *fm = fill_frame (add_frame (b));
  RdsAnalyticsFrameMeta *empty = rds_analytics_frame_meta_attach (add_frame (b), 3);
  GString *s = g_string_new (NULL);
  gchar *got;

  g_assert_true (rds_analytics_frame_meta_foreach_roi_count (fm, collect_label, s));
  got = sorted (s);
  g_assert_cmpstr (got, ==, ";RF2=0;RF=3");
  g_free (got);
  g_assert_true (rds_analytics_frame_meta_foreach_oc_status (fm, collect_status, s));
  g_assert_cmpstr (s->str, ==, "OC=1;");
  g_string_truncate (s, 0);
  g_assert_true (rds_analytics_frame_meta_foreach_lc_count (fm, collect_label, s));
  g_assert_cmpstr (s->str, ==, "Exit=1;");
  g_string_truncate (s, 0);
  g_assert_true (rds_analytics_frame_meta_foreach_lc_cum_count (fm, collect_label, s));
  g_assert_cmpstr (s->str, ==, "Exit=5000000000;");
  g_string_truncate (s, 0);
  g_assert_true (rds_analytics_frame_meta_foreach_obj_count (fm, collect_class, s));
  got = sorted (s);
  g_assert_cmpstr (got, ==, ";-1=2;0=4");
  g_free (got);

  /* empty maps call nothing */
  g_assert_true (rds_analytics_frame_meta_foreach_roi_count (empty, collect_label, s));
  g_assert_true (rds_analytics_frame_meta_foreach_oc_status (empty, collect_status, s));
  g_assert_true (rds_analytics_frame_meta_foreach_lc_count (empty, collect_label, s));
  g_assert_true (rds_analytics_frame_meta_foreach_lc_cum_count (empty, collect_label, s));
  g_assert_true (rds_analytics_frame_meta_foreach_obj_count (empty, collect_class, s));
  g_assert_cmpstr (s->str, ==, "");
  g_assert_cmpuint (rds_analytics_frame_meta_get_unique_id (empty), ==, 3);

  g_string_free (s, TRUE);
  rds_destroy_batch_meta (b);
}

static void
test_obj (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *f = add_frame (b);
  RdsObjectMeta *o = add_obj (f);
  RdsAnalyticsObjInfo *oi = fill_obj (o);
  RdsAnalyticsObjInfo *bare = rds_analytics_obj_info_attach (add_obj (f), 5);
  RdsUserMeta *um = find_user (o->obj_user_meta_list, RDS_OBJ_META_ANALYTICS);

  g_assert_nonnull (um);
  g_assert_true (um->user_meta_data == oi);
  g_assert_nonnull (um->base_meta.copy_func);
  g_assert_nonnull (um->base_meta.release_func);
  check_obj (oi);
  /* out of range: NULL */
  g_assert_null (rds_analytics_obj_info_get_roi_status (oi, 2));
  g_assert_null (rds_analytics_obj_info_get_oc_status (oi, 1));
  g_assert_null (rds_analytics_obj_info_get_lc_status (oi, 1));

  /* a fresh object info: empty lists and empty (not NULL) strings */
  g_assert_cmpuint (rds_analytics_obj_info_get_unique_id (bare), ==, 5);
  g_assert_cmpuint (rds_analytics_obj_info_n_roi_status (bare), ==, 0);
  g_assert_cmpuint (rds_analytics_obj_info_n_oc_status (bare), ==, 0);
  g_assert_cmpuint (rds_analytics_obj_info_n_lc_status (bare), ==, 0);
  g_assert_null (rds_analytics_obj_info_get_roi_status (bare, 0));
  g_assert_cmpstr (rds_analytics_obj_info_get_dir_status (bare), ==, "");
  g_assert_cmpstr (rds_analytics_obj_info_get_obj_status (bare), ==, "");

  /* the strings are replaced, the lists appended to (a repeated label is kept) */
  g_assert_true (rds_analytics_obj_info_set_dir_status (oi, ""));
  g_assert_cmpstr (rds_analytics_obj_info_get_dir_status (oi), ==, "");
  g_assert_true (rds_analytics_obj_info_add_roi_status (oi, "RF"));
  g_assert_cmpuint (rds_analytics_obj_info_n_roi_status (oi), ==, 3);
  g_assert_cmpstr (rds_analytics_obj_info_get_roi_status (oi, 2), ==, "RF");

  rds_destroy_batch_meta (b);
}

/* rds_batch_meta_copy copies both metas deeply: the copies hold the same values in their own storage. */
static void
test_copy (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (2);
  RdsFrameMeta *f = add_frame (b);
  RdsObjectMeta *o = add_obj (f);
  RdsAnalyticsFrameMeta *fm = fill_frame (f);
  RdsAnalyticsObjInfo *oi = fill_obj (o);
  RdsBatchMeta *c = rds_batch_meta_copy (b);
  RdsFrameMeta *cf;
  RdsObjectMeta *co;
  RdsUserMeta *cfu, *cou;
  RdsAnalyticsFrameMeta *cfm;
  RdsAnalyticsObjInfo *coi;
  guint32 n32;

  g_assert_nonnull (c);
  g_assert_cmpuint (g_list_length (c->frame_meta_list), ==, 1);
  cf = c->frame_meta_list->data;
  g_assert_cmpuint (g_list_length (cf->obj_meta_list), ==, 1);
  co = cf->obj_meta_list->data;
  cfu = find_user (cf->frame_user_meta_list, RDS_FRAME_META_ANALYTICS);
  cou = find_user (co->obj_user_meta_list, RDS_OBJ_META_ANALYTICS);
  g_assert_nonnull (cfu);
  g_assert_nonnull (cou);
  g_assert_true (cfu->base_meta.batch_meta == c);
  cfm = cfu->user_meta_data;
  coi = cou->user_meta_data;
  g_assert_nonnull (cfm);
  g_assert_nonnull (coi);
  g_assert_true (cfm != fm);
  g_assert_true (coi != oi);
  check_frame (cfm);
  check_obj (coi);
  /* the strings are the copy's own */
  g_assert_true (rds_analytics_obj_info_get_roi_status (coi, 0) != rds_analytics_obj_info_get_roi_status (oi, 0));
  g_assert_true (rds_analytics_obj_info_get_obj_status (coi) != rds_analytics_obj_info_get_obj_status (oi));

  /* independent: writes to the original don't show in the copy */
  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "RF", 11));
  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "New", 1));
  g_assert_true (rds_analytics_obj_info_set_obj_status (oi, "changed"));
  check_frame (cfm);
  check_obj (coi);
  g_assert_false (rds_analytics_frame_meta_get_roi_count (cfm, "New", &n32));

  /* the original goes first: the copy must not depend on it */
  rds_destroy_batch_meta (b);
  check_frame (cfm);
  check_obj (coi);
  /* and a copy of the copy */
  {
    RdsBatchMeta *cc = rds_batch_meta_copy (c);

    rds_destroy_batch_meta (c);
    cf = cc->frame_meta_list->data;
    check_frame (find_user (cf->frame_user_meta_list, RDS_FRAME_META_ANALYTICS)->user_meta_data);
    co = cf->obj_meta_list->data;
    check_obj (find_user (co->obj_user_meta_list, RDS_OBJ_META_ANALYTICS)->user_meta_data);
    rds_destroy_batch_meta (cc);
  }
}

/* Every way a meta goes away runs the release function; ASan's leak check catches a missed free and a double one. */
static void
test_release (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (4);

  for (gint i = 0; i < 4; i++) {
    RdsFrameMeta *f = add_frame (b);

    fill_frame (f);
    for (gint j = 0; j < 3; j++)
      fill_obj (add_obj (f));
  }
  /* a user meta removed on its own */
  {
    RdsFrameMeta *f = b->frame_meta_list->data;
    RdsObjectMeta *o = f->obj_meta_list->data;

    rds_remove_user_meta_from_frame (f, find_user (f->frame_user_meta_list, RDS_FRAME_META_ANALYTICS));
    g_assert_null (find_user (f->frame_user_meta_list, RDS_FRAME_META_ANALYTICS));
    rds_remove_user_meta_from_object (o, find_user (o->obj_user_meta_list, RDS_OBJ_META_ANALYTICS));
    g_assert_null (find_user (o->obj_user_meta_list, RDS_OBJ_META_ANALYTICS));
  }
  /* an object with its meta */
  {
    RdsFrameMeta *f = b->frame_meta_list->next->data;

    rds_remove_obj_meta_from_frame (f, f->obj_meta_list->data);
  }
  /* a frame with its metas and objects */
  rds_remove_frame_meta_from_batch (b, b->frame_meta_list->next->next->data);
  /* acquired and filled but never added: released with the batch */
  {
    RdsFrameMeta *f = rds_acquire_frame_meta_from_pool (b);

    fill_frame (f);
  }
  /* the rest with the batch */
  rds_destroy_batch_meta (b);
}

/* Standalone metas, owned by the caller. */
static void
test_standalone (void)
{
  RdsAnalyticsFrameMeta *fm = rds_analytics_frame_meta_new (17);
  RdsAnalyticsObjInfo *oi = rds_analytics_obj_info_new (17);
  RdsAnalyticsFrameMeta *fc;
  RdsAnalyticsObjInfo *oc;

  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "RF", 3));
  g_assert_true (rds_analytics_frame_meta_set_roi_count (fm, "RF2", 0));
  g_assert_true (rds_analytics_frame_meta_set_oc_status (fm, "OC", TRUE));
  g_assert_true (rds_analytics_frame_meta_set_lc_counts (fm, "Exit", 1, G_GUINT64_CONSTANT (5000000000)));
  g_assert_true (rds_analytics_frame_meta_set_obj_count (fm, 0, 4));
  g_assert_true (rds_analytics_frame_meta_set_obj_count (fm, -1, 2));
  fc = rds_analytics_frame_meta_copy (fm);
  rds_analytics_frame_meta_free (fm);
  check_frame (fc);
  rds_analytics_frame_meta_free (fc);
  rds_analytics_frame_meta_free (NULL);

  g_assert_true (rds_analytics_obj_info_add_roi_status (oi, "RF"));
  g_assert_true (rds_analytics_obj_info_add_roi_status (oi, "RF2"));
  g_assert_true (rds_analytics_obj_info_add_oc_status (oi, "OC"));
  g_assert_true (rds_analytics_obj_info_add_lc_status (oi, "Exit"));
  g_assert_true (rds_analytics_obj_info_set_dir_status (oi, " DIR:South "));
  g_assert_true (rds_analytics_obj_info_set_obj_status (oi, " ROI:RF ROI:RF2 DIR:South  LC:Exit "));
  oc = rds_analytics_obj_info_copy (oi);
  rds_analytics_obj_info_free (oi);
  check_obj (oc);
  rds_analytics_obj_info_free (oc);
  rds_analytics_obj_info_free (NULL);
}

/* Misuse fails with a critical and NULL or FALSE. */
static void
test_bad_args (void)
{
  RdsFrameMeta stray = { 0 };
  RdsObjectMeta stray_obj = { 0 };
  RdsAnalyticsFrameMeta *fm = rds_analytics_frame_meta_new (0);
  RdsAnalyticsObjInfo *oi = rds_analytics_obj_info_new (0);

  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*frame_meta != NULL*");
  g_assert_null (rds_analytics_frame_meta_attach (NULL, 0));
  g_test_assert_expected_messages ();
  /* a frame meta that is not from a batch */
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*batch_meta != NULL*");
  g_assert_null (rds_analytics_frame_meta_attach (&stray, 0));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*obj_meta != NULL*");
  g_assert_null (rds_analytics_obj_info_attach (NULL, 0));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*batch_meta != NULL*");
  g_assert_null (rds_analytics_obj_info_attach (&stray_obj, 0));
  g_test_assert_expected_messages ();

  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*label != NULL*");
  g_assert_false (rds_analytics_frame_meta_set_roi_count (fm, NULL, 1));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*meta != NULL*");
  g_assert_false (rds_analytics_frame_meta_set_obj_count (NULL, 0, 1));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*label != NULL*");
  g_assert_false (rds_analytics_frame_meta_get_lc_count (fm, NULL, NULL));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*func != NULL*");
  g_assert_false (rds_analytics_frame_meta_foreach_obj_count (fm, NULL, NULL));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*label != NULL*");
  g_assert_false (rds_analytics_obj_info_add_lc_status (oi, NULL));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*status != NULL*");
  g_assert_false (rds_analytics_obj_info_set_obj_status (oi, NULL));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*info != NULL*");
  g_assert_null (rds_analytics_obj_info_get_dir_status (NULL));
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*info != NULL*");
  g_assert_cmpuint (rds_analytics_obj_info_n_roi_status (NULL), ==, 0);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*meta != NULL*");
  g_assert_null (rds_analytics_frame_meta_copy (NULL));
  g_test_assert_expected_messages ();

  rds_analytics_frame_meta_free (fm);
  rds_analytics_obj_info_free (oi);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/analytics-meta/types", test_types);
  g_test_add_func ("/cpu/analytics-meta/frame", test_frame);
  g_test_add_func ("/cpu/analytics-meta/foreach", test_foreach);
  g_test_add_func ("/cpu/analytics-meta/obj", test_obj);
  g_test_add_func ("/cpu/analytics-meta/copy", test_copy);
  g_test_add_func ("/cpu/analytics-meta/release", test_release);
  g_test_add_func ("/cpu/analytics-meta/standalone", test_standalone);
  g_test_add_func ("/cpu/analytics-meta/bad-args", test_bad_args);
  return g_test_run ();
}
