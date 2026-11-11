/* rdsh264enc / rdsh265enc key-frame requests (rds-enc-keyunits) on their own: a request goes to the next frame into
 * the queue, buffer lists count all their frames, several requests can share a frame, a flush puts the requests given
 * to queued frames back in front of the waiting ones in order, and clear drops everything.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-keyunits.h"

/* A request told apart by its count. */
static GstEvent *
request (guint count)
{
  GstStructure *s = gst_structure_new ("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, "count", G_TYPE_UINT,
      count, "running-time", G_TYPE_UINT64, GST_CLOCK_TIME_NONE, NULL);

  return gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM, s);
}

static guint
count_of (GstEvent * ev)
{
  guint c = G_MAXUINT;

  g_assert_true (gst_structure_get_uint (gst_event_get_structure (ev), "count", &c));
  return c;
}

/* Takes the next frame out and checks its requests are @n, with the counts in @counts, in order. */
static void
check_out (RdsEncKeyUnits * k, guint n, const guint * counts)
{
  GList *got = rds_enc_key_units_take_due (k);

  g_assert_cmpuint (g_list_length (got), ==, n);
  for (guint i = 0; i < n; i++)
    g_assert_cmpuint (count_of (g_list_nth_data (got, i)), ==, counts[i]);
  g_list_free_full (got, (GDestroyNotify) gst_event_unref);
}

/* A request made while frames are queued applies to the next frame in, not to the queued ones. */
static void
test_next_frame (void)
{
  RdsEncKeyUnits k;
  const guint one[] = { 1 };

  rds_enc_key_units_init (&k);
  /* nothing asked: nothing taken */
  rds_enc_key_units_frames_in (&k, 1);
  check_out (&k, 0, NULL);
  /* frames 1 and 2 queued, then the request: frame 3 */
  rds_enc_key_units_frames_in (&k, 1);
  rds_enc_key_units_frames_in (&k, 1);
  rds_enc_key_units_request (&k, request (1));
  rds_enc_key_units_frames_in (&k, 1);
  check_out (&k, 0, NULL);      /* frame 1 */
  check_out (&k, 0, NULL);      /* frame 2 */
  check_out (&k, 1, one);       /* frame 3 */
  check_out (&k, 0, NULL);      /* none left */
  g_assert_null (rds_enc_key_units_clear (&k));
}

/* A buffer list counts all its frames; a request waiting before it goes to its first frame. */
static void
test_buffer_lists (void)
{
  RdsEncKeyUnits k;
  const guint one[] = { 1 }, two[] = { 2 };

  rds_enc_key_units_init (&k);
  rds_enc_key_units_frames_in (&k, 3);
  rds_enc_key_units_request (&k, request (1));
  rds_enc_key_units_frames_in (&k, 4);  /* frames 3..6 */
  rds_enc_key_units_request (&k, request (2));
  rds_enc_key_units_frames_in (&k, 0);  /* an empty list gives no frame */
  rds_enc_key_units_frames_in (&k, 1);  /* frame 7 */
  for (guint i = 0; i < 3; i++)
    check_out (&k, 0, NULL);
  check_out (&k, 1, one);
  for (guint i = 4; i < 7; i++)
    check_out (&k, 0, NULL);
  check_out (&k, 1, two);
  g_assert_null (rds_enc_key_units_clear (&k));
}

/* Requests made before the same frame all go to it, oldest first. */
static void
test_same_frame (void)
{
  RdsEncKeyUnits k;
  const guint all[] = { 1, 2, 3 };

  rds_enc_key_units_init (&k);
  rds_enc_key_units_request (&k, request (1));
  rds_enc_key_units_request (&k, request (2));
  rds_enc_key_units_request (&k, request (3));
  rds_enc_key_units_frames_in (&k, 1);
  check_out (&k, 3, all);
  g_assert_null (rds_enc_key_units_clear (&k));
}

/* A flush: requests given to dropped frames wait again, in front of the ones still waiting and in their order; both
 * counts restart, so the first frame after the flush gets them all. */
static void
test_flush (void)
{
  RdsEncKeyUnits k;
  const guint one[] = { 1 }, rest[] = { 2, 3, 4 };

  rds_enc_key_units_init (&k);
  rds_enc_key_units_request (&k, request (1));
  rds_enc_key_units_frames_in (&k, 1);  /* frame 0: 1 */
  rds_enc_key_units_request (&k, request (2));
  rds_enc_key_units_frames_in (&k, 1);  /* frame 1: 2 */
  rds_enc_key_units_request (&k, request (3));
  rds_enc_key_units_frames_in (&k, 1);  /* frame 2: 3 */
  rds_enc_key_units_request (&k, request (4));  /* waiting */
  check_out (&k, 1, one);       /* frame 0 reached VA; 1 and 2 are dropped by the flush */
  rds_enc_key_units_flush (&k);
  rds_enc_key_units_frames_in (&k, 1);
  check_out (&k, 3, rest);
  check_out (&k, 0, NULL);
  g_assert_null (rds_enc_key_units_clear (&k));
}

/* clear drops the waiting and the due requests (no leak: the events are freed) and restarts the counts. */
static void
test_clear (void)
{
  RdsEncKeyUnits k;
  GstEvent *kept = request (9);
  GList *dropped;

  rds_enc_key_units_init (&k);
  rds_enc_key_units_request (&k, request (1));
  rds_enc_key_units_frames_in (&k, 2);
  rds_enc_key_units_request (&k, gst_event_ref (kept));
  dropped = rds_enc_key_units_clear (&k);
  g_assert_cmpuint (g_list_length (dropped), ==, 2);
  g_list_free_full (dropped, (GDestroyNotify) gst_event_unref);
  g_assert_cmpint (GST_MINI_OBJECT_REFCOUNT_VALUE (kept), ==, 1);
  gst_event_unref (kept);
  /* counts restarted: the next frame in is frame 0 again */
  rds_enc_key_units_request (&k, request (5));
  rds_enc_key_units_frames_in (&k, 1);
  {
    const guint five[] = { 5 };

    check_out (&k, 1, five);
  }
  g_list_free_full (rds_enc_key_units_clear (&k), (GDestroyNotify) gst_event_unref);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/enc-keyunits/next-frame", test_next_frame);
  g_test_add_func ("/cpu/enc-keyunits/buffer-lists", test_buffer_lists);
  g_test_add_func ("/cpu/enc-keyunits/same-frame", test_same_frame);
  g_test_add_func ("/cpu/enc-keyunits/flush", test_flush);
  g_test_add_func ("/cpu/enc-keyunits/clear", test_clear);
  return g_test_run ();
}
