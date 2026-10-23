/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds-buffer-pool.h>

#define RDS_CAPS "video/x-raw(memory:RDS), format=NV12, width=320, height=240"
#define SYS_CAPS "video/x-raw, format=NV12, width=320, height=240"

static GstQuery *
new_query (const gchar * caps_str, gboolean need_pool)
{
  GstCaps *caps = gst_caps_from_string (caps_str);
  GstQuery *q = gst_query_new_allocation (caps, need_pool);

  gst_caps_unref (caps);
  return q;
}

static void
test_propose_rds (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *pool = NULL;
  guint size, min, max, batch;
  RdsBufSurfaceMemType mt;
  GstStructure *config;

  g_assert_true (rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 3));
  g_assert_true (gst_query_find_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL));
  g_assert_true (gst_query_find_allocation_meta (q, GST_VIDEO_CROP_META_API_TYPE, NULL));
  g_assert_cmpuint (gst_query_get_n_allocation_pools (q), ==, 1);
  gst_query_parse_nth_allocation_pool (q, 0, &pool, &size, &min, &max);
  g_assert_true (RDS_IS_BUFFER_POOL (pool));
  g_assert_cmpuint (size, ==, sizeof (RdsBufSurface));
  g_assert_cmpuint (min, ==, 3);
  config = gst_buffer_pool_get_config (pool);
  g_assert_true (rds_buffer_pool_config_get_surface_params (config, &batch, &mt, NULL));
  g_assert_cmpuint (batch, ==, 1);
  g_assert_cmpint (mt, ==, RDS_BUF_MEM_SYSTEM);
  gst_structure_free (config);
  gst_object_unref (pool);
  gst_query_unref (q);
}

static void
test_propose_system_caps (void)
{
  GstQuery *q = new_query (SYS_CAPS, TRUE);

  g_assert_true (rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2));
  g_assert_cmpuint (gst_query_get_n_allocation_pools (q), ==, 0);   /* upstream allocates system memory */
  g_assert_true (gst_query_find_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL));
  gst_query_unref (q);
}

static void
test_decide_reuses_matching (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *proposed = NULL, *decided;

  rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2);
  gst_query_parse_nth_allocation_pool (q, 0, &proposed, NULL, NULL, NULL);
  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 4, 0);
  g_assert_true (decided == proposed);

  guint min, max;
  gst_query_parse_nth_allocation_pool (q, 0, NULL, NULL, &min, &max);
  g_assert_cmpuint (min, ==, 4);                                    /* MAX (2, 4) */
  g_assert_cmpuint (max, ==, 0);
  g_assert_true (gst_buffer_pool_set_active (decided, TRUE));
  gst_buffer_pool_set_active (decided, FALSE);
  gst_object_unref (decided);
  gst_object_unref (proposed);
  gst_query_unref (q);
}

static void
test_decide_replaces_foreign_or_mismatched (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *plain = gst_buffer_pool_new (), *decided, *first = NULL;

  gst_query_add_allocation_pool (q, plain, 4096, 1, 0);
  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2, 8);
  g_assert_true (RDS_IS_BUFFER_POOL (decided));
  guint min, max;
  gst_query_parse_nth_allocation_pool (q, 0, &first, NULL, &min, &max);
  g_assert_true (first == decided);                                  /* written back as pool 0 */
  g_assert_cmpuint (min, ==, 2);                                     /* MAX (1, 2) */
  g_assert_cmpuint (max, ==, 8);                                     /* no limit downstream, ours is 8 */
  gst_object_unref (first);
  gst_object_unref (decided);
  gst_object_unref (plain);
  gst_query_unref (q);

  /* An RDS pool with a different batch size is not reused. */
  q = new_query (RDS_CAPS, TRUE);
  rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2);
  gst_query_parse_nth_allocation_pool (q, 0, &first, NULL, NULL, NULL);
  decided = rds_decide_allocation (q, 4, RDS_BUF_MEM_SYSTEM, 0, 2, 0);
  g_assert_true (decided != first);
  gst_object_unref (first);
  gst_object_unref (decided);
  gst_query_unref (q);
}

static void
test_decide_empty_query (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *decided = rds_decide_allocation (q, 2, RDS_BUF_MEM_SYSTEM, 0, 2, 4);
  guint min, max;

  g_assert_nonnull (decided);
  g_assert_cmpuint (gst_query_get_n_allocation_pools (q), ==, 1);
  gst_query_parse_nth_allocation_pool (q, 0, NULL, NULL, &min, &max);
  g_assert_cmpuint (min, ==, 2);
  g_assert_cmpuint (max, ==, 4);
  gst_object_unref (decided);
  gst_query_unref (q);
}

static void
test_decide_skips_active (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *proposed = NULL, *decided;

  rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1);
  gst_query_parse_nth_allocation_pool (q, 0, &proposed, NULL, NULL, NULL);
  g_assert_true (gst_buffer_pool_set_active (proposed, TRUE));     /* in use elsewhere: cannot be reconfigured */
  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1, 0);
  g_assert_nonnull (decided);
  g_assert_true (decided != proposed);
  g_assert_true (gst_buffer_pool_set_active (proposed, FALSE));
  gst_object_unref (decided);
  gst_object_unref (proposed);
  gst_query_unref (q);
}

static void
test_decide_video_meta_option (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *decided;
  GstStructure *config;

  /* Without GstVideoMeta in the query, the pool is not asked to add it. */
  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1, 0);
  config = gst_buffer_pool_get_config (decided);
  g_assert_false (gst_buffer_pool_config_has_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META));
  gst_structure_free (config);
  gst_object_unref (decided);
  gst_query_unref (q);

  q = new_query (RDS_CAPS, TRUE);
  gst_query_add_allocation_meta (q, GST_VIDEO_META_API_TYPE, NULL);
  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1, 0);
  config = gst_buffer_pool_get_config (decided);
  g_assert_true (gst_buffer_pool_config_has_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META));
  gst_structure_free (config);
  gst_object_unref (decided);
  gst_query_unref (q);
}

static void
test_no_caps (void)
{
  GstQuery *q = gst_query_new_allocation (NULL, TRUE);

  g_assert_false (rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1));
  g_assert_null (rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1, 0));
  gst_query_unref (q);
}

/* Proposes a SYSTEM/GPU 0/batch 1 pool, then decides with other params: the proposed pool must not be reused. */
static void
assert_not_reused (guint batch, RdsBufSurfaceMemType mem_type, guint gpu_id)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *proposed = NULL, *decided;

  rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2);
  gst_query_parse_nth_allocation_pool (q, 0, &proposed, NULL, NULL, NULL);
  decided = rds_decide_allocation (q, batch, mem_type, gpu_id, 2, 0);
  g_assert_nonnull (decided);
  g_assert_true (decided != proposed);
  gst_object_unref (decided);
  gst_object_unref (proposed);
  gst_query_unref (q);
}

static void
test_decide_param_mismatch (void)
{
  assert_not_reused (1, RDS_BUF_MEM_SYSTEM, 1);                     /* GPU id */
  assert_not_reused (1, RDS_BUF_MEM_HIP_PINNED, 0);                 /* memory type */
  assert_not_reused (4, RDS_BUF_MEM_SYSTEM, 0);                     /* batch size */
}

/* Decides against a query holding one pool-less entry min/max; checks the single resulting entry. */
static void
assert_counts (guint qmin, guint qmax, guint min_buffers, guint max_buffers, guint want_min, guint want_max)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *decided, *first = NULL;
  guint min, max;

  gst_query_add_allocation_pool (q, NULL, 0, qmin, qmax);
  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, min_buffers, max_buffers);
  g_assert_nonnull (decided);
  g_assert_cmpuint (gst_query_get_n_allocation_pools (q), ==, 1);
  gst_query_parse_nth_allocation_pool (q, 0, &first, NULL, &min, &max);
  g_assert_true (first == decided);
  g_assert_cmpuint (min, ==, want_min);
  g_assert_cmpuint (max, ==, want_max);
  gst_object_unref (first);
  gst_object_unref (decided);
  gst_query_unref (q);
}

static void
test_decide_counts (void)
{
  assert_counts (3, 7, 1, 0, 3, 7);       /* NULL pool entry replaced, its counts kept */
  assert_counts (1, 10, 1, 4, 1, 4);      /* our max is tighter */
  assert_counts (2, 3, 5, 0, 5, 5);       /* max raised to min */
}

static void
test_decide_moves_match_to_front (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *plain = gst_buffer_pool_new (), *proposed = NULL, *decided, *first = NULL;

  gst_query_add_allocation_pool (q, plain, 4096, 1, 0);
  rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2);           /* index 1 */
  gst_query_parse_nth_allocation_pool (q, 1, &proposed, NULL, NULL, NULL);
  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2, 0);
  g_assert_true (decided == proposed);
  g_assert_cmpuint (gst_query_get_n_allocation_pools (q), ==, 1);     /* no duplicate entry */
  gst_query_parse_nth_allocation_pool (q, 0, &first, NULL, NULL, NULL);
  g_assert_true (first == decided);
  gst_object_unref (first);
  gst_object_unref (decided);
  gst_object_unref (proposed);
  gst_object_unref (plain);
  gst_query_unref (q);
}

static void
test_decide_retries_failed_reuse (void)
{
  GstQuery *q = new_query (RDS_CAPS, TRUE);
  GstBufferPool *proposed = NULL, *decided;
  GstBuffer *outstanding = NULL;

  rds_propose_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1);
  gst_query_parse_nth_allocation_pool (q, 0, &proposed, NULL, NULL, NULL);
  /* Inactive but with a buffer still out: it matches, yet set_config refuses. */
  g_assert_true (gst_buffer_pool_set_active (proposed, TRUE));
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (proposed, &outstanding, NULL), ==, GST_FLOW_OK);
  g_assert_true (gst_buffer_pool_set_active (proposed, FALSE));
  g_assert_false (gst_buffer_pool_is_active (proposed));

  decided = rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 2, 0);
  g_assert_nonnull (decided);
  g_assert_true (RDS_IS_BUFFER_POOL (decided));
  g_assert_true (decided != proposed);

  gst_buffer_unref (outstanding);
  gst_object_unref (decided);
  gst_object_unref (proposed);
  gst_query_unref (q);
}

static void
test_decide_system_caps (void)
{
  GstQuery *q = new_query (SYS_CAPS, TRUE);

  g_assert_null (rds_decide_allocation (q, 1, RDS_BUF_MEM_SYSTEM, 0, 1, 0));
  g_assert_cmpuint (gst_query_get_n_allocation_pools (q), ==, 0);
  gst_query_unref (q);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/allocation/propose-rds", test_propose_rds);
  g_test_add_func ("/allocation/propose-system-caps", test_propose_system_caps);
  g_test_add_func ("/allocation/decide-reuses-matching", test_decide_reuses_matching);
  g_test_add_func ("/allocation/decide-replaces", test_decide_replaces_foreign_or_mismatched);
  g_test_add_func ("/allocation/decide-empty-query", test_decide_empty_query);
  g_test_add_func ("/allocation/decide-skips-active", test_decide_skips_active);
  g_test_add_func ("/allocation/decide-video-meta-option", test_decide_video_meta_option);
  g_test_add_func ("/allocation/no-caps", test_no_caps);
  g_test_add_func ("/allocation/decide-param-mismatch", test_decide_param_mismatch);
  g_test_add_func ("/allocation/decide-counts", test_decide_counts);
  g_test_add_func ("/allocation/decide-moves-match-to-front", test_decide_moves_match_to_front);
  g_test_add_func ("/allocation/decide-retries-failed-reuse", test_decide_retries_failed_reuse);
  g_test_add_func ("/allocation/decide-system-caps", test_decide_system_caps);
  return g_test_run ();
}
