/* rdslogger: factory, properties, accepted no-ops, and the **PERF lines on scripted batches (cpu). The harness gives
 * the element a GstTestClock at running time 0; the element measures on a periodic clock id, so each crank of the
 * test clock is one interval boundary, and frames pushed between two cranks fall into that interval. rdslogger
 * writes with g_print, so a print handler, installed only while a logger exists (GTest prints its TAP output with
 * g_print too), captures its output.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/check/gstharness.h>
#include <gst/check/gsttestclock.h>
#include <rds/rds.h>
#include <string.h>

static GMutex out_lock;
static GString *out;
static GPrintFunc saved_print;

static void
capture_print (const gchar * s)
{
  g_mutex_lock (&out_lock);
  g_string_append (out, s);
  g_mutex_unlock (&out_lock);
}

/* The captured output so far (owned by the caller), and clears it. */
static gchar *
take_output (void)
{
  gchar *s;

  g_mutex_lock (&out_lock);
  s = g_strdup (out->str);
  g_string_truncate (out, 0);
  g_mutex_unlock (&out_lock);
  return s;
}

static void
assert_output (const gchar * expected)
{
  gchar *s = take_output ();

  g_assert_cmpstr (s, ==, expected);
  g_free (s);
}

/* A system-memory buffer with a batch meta holding one frame meta per id in @ids (n of them). */
static GstBuffer *
make_batch (const guint * ids, guint n)
{
  GstBuffer *buf = gst_buffer_new_allocate (NULL, 16, NULL);
  RdsBatchMeta *bm = rds_create_batch_meta (MAX (n, 1));

  for (guint i = 0; i < n; i++) {
    RdsFrameMeta *fm = rds_acquire_frame_meta_from_pool (bm);

    fm->batch_id = i;
    fm->pad_index = ids[i];
    fm->source_id = ids[i];
    rds_add_frame_meta_to_batch (bm, fm);
  }
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, bm));
  return buf;
}

static void
push_frames (GstHarness * h, guint id, guint n)
{
  for (guint i = 0; i < n; i++)
    g_assert_cmpint (gst_harness_push (h, make_batch (&id, 1)), ==, GST_FLOW_OK);
}

static GstHarness *
logger_new (const gchar * props)
{
  gchar *desc = g_strdup_printf ("rdslogger %s", props != NULL ? props : "");
  GstElement *e = gst_parse_launch (desc, NULL);
  GstHarness *h;

  g_assert_nonnull (e);
  g_free (take_output ());
  saved_print = g_set_print_handler (capture_print);
  h = gst_harness_new_with_element (e, "sink", "src");
  gst_object_unref (e);
  g_free (desc);
  gst_harness_set_src_caps_str (h, "video/x-raw,format=RGBA,width=4,height=4,framerate=30/1");
  /* the passthrough output is not checked: drop it */
  gst_harness_set_drop_buffers (h, TRUE);
  return h;
}

static void
logger_free (GstHarness * h)
{
  gst_harness_teardown (h);
  g_set_print_handler (saved_print);
}

/* One interval boundary: the clock jumps to the next pending id and fires it. */
static void
crank (GstHarness * h)
{
  g_assert_true (gst_harness_crank_single_clock_wait (h));
}

/* ---- tests ---- */

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdslogger", NULL);
  guint interval, sleep_time, latency;
  gboolean silent, handoffs, sync, dump, single, offset, ts, drop_alloc, qos;
  gint datarate, error_after;
  gfloat prob;
  gint64 ts_offset;
  GstBufferFlags flags;
  gchar *last;
  GParamSpec *ps;

  g_assert_nonnull (e);
  g_assert_true (GST_IS_BASE_TRANSFORM (e));
  /* defaults as gst-inspect shows them for DeepStream 8.0 */
  g_object_get (e, "fps-measurement-interval-sec", &interval, "silent", &silent, "signal-handoffs", &handoffs,
      "sync", &sync, "dump", &dump, "single-segment", &single, "check-imperfect-offset", &offset,
      "check-imperfect-timestamp", &ts, "drop-allocation", &drop_alloc, "qos", &qos, "datarate", &datarate,
      "error-after", &error_after, "drop-probability", &prob, "ts-offset", &ts_offset, "drop-buffer-flags", &flags,
      "sleep-time", &sleep_time, "latency", &latency, "last-message", &last, NULL);
  g_assert_cmpuint (interval, ==, 5);
  g_assert_true (silent);
  g_assert_true (handoffs);
  g_assert_false (sync);
  g_assert_false (dump);
  g_assert_false (single);
  g_assert_false (offset);
  g_assert_false (ts);
  g_assert_false (drop_alloc);
  g_assert_false (qos);
  g_assert_cmpint (datarate, ==, 0);
  g_assert_cmpint (error_after, ==, -1);
  g_assert_cmpfloat (prob, ==, 0);
  g_assert_cmpint (ts_offset, ==, 0);
  g_assert_cmpuint (flags, ==, 0);
  g_assert_cmpuint (sleep_time, ==, 0);
  g_assert_cmpuint (latency, ==, 0);
  g_assert_null (last);

  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "fps-measurement-interval-sec");
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->minimum, ==, 0);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (ps)->maximum, ==, G_MAXUINT);
  g_assert_false (ps->flags & GST_PARAM_MUTABLE_READY);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "last-message");
  g_assert_false (ps->flags & G_PARAM_WRITABLE);
  ps = g_object_class_find_property (G_OBJECT_GET_CLASS (e), "drop-buffer-flags");
  g_assert_true (G_PARAM_SPEC_VALUE_TYPE (ps) == GST_TYPE_BUFFER_FLAGS);
  g_assert_cmpuint (g_signal_lookup ("handoff", G_OBJECT_TYPE (e)), !=, 0);

  g_object_set (e, "fps-measurement-interval-sec", 2u, "silent", FALSE, NULL);
  g_object_get (e, "fps-measurement-interval-sec", &interval, "silent", &silent, NULL);
  g_assert_cmpuint (interval, ==, 2);
  g_assert_false (silent);
  gst_object_unref (e);
}

static void
test_caps_any (void)
{
  GstElement *e = gst_element_factory_make ("rdslogger", NULL);
  GstPad *sink = gst_element_get_static_pad (e, "sink");
  GstPad *src = gst_element_get_static_pad (e, "src");
  GstCaps *c;

  c = gst_pad_get_pad_template_caps (sink);
  g_assert_true (gst_caps_is_any (c));
  gst_caps_unref (c);
  c = gst_pad_get_pad_template_caps (src);
  g_assert_true (gst_caps_is_any (c));
  gst_caps_unref (c);
  gst_object_unref (src);
  gst_object_unref (sink);
  gst_object_unref (e);
}

static void
test_noops_warn_once (void)
{
  GstElement *e = gst_element_factory_make ("rdslogger", NULL);
  guint sleep_time;
  gboolean sync;

  /* identity's properties: accepted and stored; a value that would do something warns once per property */
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*sleep-time*no effect*");
  g_object_set (e, "sleep-time", 5u, NULL);
  g_object_set (e, "sleep-time", 7u, NULL);
  g_test_assert_expected_messages ();
  g_test_expect_message ("rdssink", G_LOG_LEVEL_WARNING, "*sync*no effect*");
  g_object_set (e, "sync", TRUE, NULL);
  g_test_assert_expected_messages ();
  /* the default value does nothing either way: no warning */
  g_object_set (e, "dump", FALSE, "error-after", -1, "drop-probability", 0.0f, "ts-offset", (gint64) 0, NULL);
  g_object_get (e, "sleep-time", &sleep_time, "sync", &sync, NULL);
  g_assert_cmpuint (sleep_time, ==, 7);
  g_assert_true (sync);
  gst_object_unref (e);
}

static void
on_handoff (G_GNUC_UNUSED GstElement * e, GstBuffer * buf, guint * n)
{
  g_assert_true (GST_IS_BUFFER (buf));
  (*n)++;
}

static void
test_handoff (void)
{
  GstHarness *h = logger_new (NULL);
  guint n = 0;

  g_signal_connect (h->element, "handoff", G_CALLBACK (on_handoff), &n);
  push_frames (h, 0, 2);
  g_assert_cmpuint (n, ==, 2);
  g_object_set (h->element, "signal-handoffs", FALSE, NULL);
  push_frames (h, 0, 1);
  g_assert_cmpuint (n, ==, 2);
  logger_free (h);
}

static void
test_passthrough (void)
{
  GstHarness *h = gst_harness_new ("rdslogger");
  GstBuffer *in, *outbuf;
  guint id = 3;

  gst_harness_set_src_caps_str (h, "application/x-anything");
  in = make_batch (&id, 1);
  GST_BUFFER_PTS (in) = 42;
  g_assert_cmpint (gst_harness_push (h, gst_buffer_ref (in)), ==, GST_FLOW_OK);
  outbuf = gst_harness_pull (h);
  g_assert_true (outbuf == in);
  g_assert_cmpuint (GST_BUFFER_PTS (outbuf), ==, 42);
  gst_buffer_unref (outbuf);
  gst_buffer_unref (in);
  gst_harness_teardown (h);
}

static void
test_first_print_after_two_intervals (void)
{
  GstHarness *h = logger_new (NULL);
  GstTestClock *clock = gst_harness_get_testclock (h);

  /* default interval 5 s: the first interval only starts the count (DeepStream 8.0 prints first ≈2 intervals in) */
  push_frames (h, 0, 10);
  crank (h);
  assert_output ("");
  g_assert_cmpuint (gst_clock_get_time (GST_CLOCK (clock)), ==, 5 * GST_SECOND);
  push_frames (h, 0, 20);
  crank (h);
  assert_output ("**PERF : FPS_0 (4.00)\t\n");
  /* then every interval: frames in the interval / its length (ours) */
  push_frames (h, 0, 7);
  crank (h);
  assert_output ("**PERF : FPS_0 (1.40)\t\n");
  crank (h);
  assert_output ("**PERF : FPS_0 (0.00)\t\n");
  gst_object_unref (clock);
  logger_free (h);
}

static void
test_two_streams (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=1");
  const guint both[] = { 3, 1 };

  /* stream 3 starts; stream 1 joins late; stream 3 ends early (rds-stream-eos) and stays listed with 0.00; the
   * streams are printed by ascending id, each followed by a tab */
  push_frames (h, 3, 5);
  crank (h);
  assert_output ("");
  push_frames (h, 3, 30);
  crank (h);
  assert_output ("**PERF : FPS_3 (30.00)\t\n");
  for (guint i = 0; i < 15; i++)
    g_assert_cmpint (gst_harness_push (h, make_batch (both, 2)), ==, GST_FLOW_OK);
  push_frames (h, 3, 15);
  crank (h);
  assert_output ("**PERF : FPS_1 (15.00)\tFPS_3 (30.00)\t\n");
  g_assert_true (gst_harness_push_event (h, rds_event_new_stream_eos (3)));
  push_frames (h, 1, 25);
  crank (h);
  assert_output ("**PERF : FPS_1 (25.00)\tFPS_3 (0.00)\t\n");
  g_assert_true (gst_harness_push_event (h, rds_event_new_stream_eos (1)));
  crank (h);
  assert_output ("**PERF : FPS_1 (0.00)\tFPS_3 (0.00)\t\n");
  logger_free (h);
}

static void
test_interval (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=2");
  GstTestClock *clock = gst_harness_get_testclock (h);

  crank (h);
  g_assert_cmpuint (gst_clock_get_time (GST_CLOCK (clock)), ==, 2 * GST_SECOND);
  push_frames (h, 0, 7);
  crank (h);
  g_assert_cmpuint (gst_clock_get_time (GST_CLOCK (clock)), ==, 4 * GST_SECOND);
  assert_output ("**PERF : FPS_0 (3.50)\t\n");

  /* a new interval while playing restarts the measurement from now: one interval to start the count, then prints */
  g_object_set (h->element, "fps-measurement-interval-sec", 1u, NULL);
  push_frames (h, 0, 3);
  crank (h);
  g_assert_cmpuint (gst_clock_get_time (GST_CLOCK (clock)), ==, 5 * GST_SECOND);
  assert_output ("");
  push_frames (h, 0, 9);
  crank (h);
  assert_output ("**PERF : FPS_0 (9.00)\t\n");
  gst_object_unref (clock);
  logger_free (h);
}

static void
test_interval_zero (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=0");
  GstTestClock *clock = gst_harness_get_testclock (h);

  /* 0 disables the measurement (ours: DeepStream 8.0 prints in a busy loop) */
  push_frames (h, 0, 30);
  g_assert_cmpuint (gst_test_clock_peek_id_count (clock), ==, 0);
  gst_test_clock_set_time (clock, 100 * GST_SECOND);
  push_frames (h, 0, 30);
  assert_output ("");
  /* and a non-zero interval set while playing starts it */
  g_object_set (h->element, "fps-measurement-interval-sec", 1u, NULL);
  g_assert_cmpuint (gst_test_clock_peek_id_count (clock), ==, 1);
  g_object_set (h->element, "fps-measurement-interval-sec", 0u, NULL);
  g_assert_cmpuint (gst_test_clock_peek_id_count (clock), ==, 0);
  gst_object_unref (clock);
  logger_free (h);
}

static void
test_no_batch_meta (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=1");

  /* a buffer without batch meta counts as one frame of stream 0 (ours) */
  crank (h);
  for (guint i = 0; i < 4; i++)
    g_assert_cmpint (gst_harness_push (h, gst_buffer_new_allocate (NULL, 16, NULL)), ==, GST_FLOW_OK);
  crank (h);
  assert_output ("**PERF : FPS_0 (4.00)\t\n");
  logger_free (h);
}

static void
test_nothing_seen (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=1");

  /* no stream seen yet: nothing printed (ours) */
  crank (h);
  crank (h);
  crank (h);
  assert_output ("");
  logger_free (h);
}

static void
test_restart (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=1");

  push_frames (h, 2, 3);
  crank (h);
  push_frames (h, 2, 3);
  crank (h);
  assert_output ("**PERF : FPS_2 (3.00)\t\n");
  /* back to READY forgets the streams and the count */
  gst_element_set_state (h->element, GST_STATE_NULL);
  gst_harness_play (h);
  push_frames (h, 0, 2);
  crank (h);
  assert_output ("");
  push_frames (h, 0, 2);
  crank (h);
  assert_output ("**PERF : FPS_0 (2.00)\t\n");
  logger_free (h);
}

static void
test_pause_resume (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=1");
  GstTestClock *clock = gst_harness_get_testclock (h);
  GstClockID id;

  crank (h);
  push_frames (h, 0, 4);
  /* paused 300 ms into the interval: no boundary pending, and no running time passes */
  gst_test_clock_set_time (clock, 1300 * GST_MSECOND);
  g_assert_cmpint (gst_element_set_state (h->element, GST_STATE_PAUSED), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpuint (gst_test_clock_peek_id_count (clock), ==, 0);
  gst_test_clock_set_time (clock, 5 * GST_SECOND);
  /* playing again: the boundary is due at the old one (2 s) plus the 3.7 s paused */
  g_assert_cmpint (gst_element_set_state (h->element, GST_STATE_PLAYING), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpuint (gst_test_clock_peek_id_count (clock), ==, 1);
  g_assert_true (gst_test_clock_peek_next_pending_id (clock, &id));
  g_assert_cmpuint (gst_clock_id_get_time (id), ==, 5700 * GST_MSECOND);
  gst_clock_id_unref (id);
  push_frames (h, 0, 2);
  crank (h);
  assert_output ("**PERF : FPS_0 (6.00)\t\n");
  gst_object_unref (clock);
  logger_free (h);
}

static void
test_flush_reset (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=1");
  GstTestClock *clock = gst_harness_get_testclock (h);
  GstSegment seg;
  GstClockID id;

  crank (h);
  push_frames (h, 0, 5);
  gst_test_clock_set_time (clock, 1500 * GST_MSECOND);
  /* flushes change nothing, even one that resets the running time (a seek, a looped clip): the boundary stays at
   * 2 s and the frames counted before the flush count */
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (h, gst_event_new_segment (&seg)));
  g_assert_true (gst_test_clock_peek_next_pending_id (clock, &id));
  g_assert_cmpuint (gst_clock_id_get_time (id), ==, 2 * GST_SECOND);
  gst_clock_id_unref (id);
  push_frames (h, 0, 3);
  crank (h);
  assert_output ("**PERF : FPS_0 (8.00)\t\n");

  push_frames (h, 0, 2);
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (h, gst_event_new_flush_stop (FALSE)));
  g_assert_true (gst_harness_push_event (h, gst_event_new_segment (&seg)));
  push_frames (h, 0, 1);
  crank (h);
  assert_output ("**PERF : FPS_0 (3.00)\t\n");
  gst_object_unref (clock);
  logger_free (h);
}

static void
test_silent_false (void)
{
  GstHarness *h = logger_new ("fps-measurement-interval-sec=1 silent=false name=log7");
  GstBuffer *buf;
  gchar *s, **lines;
  guint id = 0;

  /* clock at 2 s, buffer at 1.5 s: late by 500 ms */
  gst_harness_set_time (h, 2 * GST_SECOND);
  buf = make_batch (&id, 1);
  GST_BUFFER_PTS (buf) = 1500 * GST_MSECOND;
  g_assert_cmpint (gst_harness_push (h, buf), ==, GST_FLOW_OK);
  /* a buffer ahead of the clock: early */
  buf = make_batch (&id, 1);
  GST_BUFFER_PTS (buf) = 2250 * GST_MSECOND;
  GST_BUFFER_DURATION (buf) = 40 * GST_MSECOND;
  g_assert_cmpint (gst_harness_push (h, buf), ==, GST_FLOW_OK);
  s = take_output ();
  lines = g_strsplit (s, "\n", -1);
  g_assert_cmpuint (g_strv_length (lines), ==, 5);
  /* DeepStream 8.0's per-buffer lines, the buffer address aside */
  g_assert_true (g_str_has_prefix (lines[0], "log7 BUF_PTS : 0:00:01.500000000 element time 0:00:02.000000000 "
          "elem-buf=500.00 ms buf=0x"));
  g_assert_true (g_str_has_suffix (lines[0], " buffer_pts_to_running_time = 0:00:01.500000000"
          "rdslogger base time = 0:00:00.000000000buffer dur 99:99:99.999999999"));
  g_assert_cmpstr (lines[1], ==, "BUFFER PTS is lesser than ELEMENT TIME, IT's LATE");
  g_assert_true (g_str_has_prefix (lines[2], "log7 BUF_PTS : 0:00:02.250000000 element time 0:00:02.000000000 "
          "elem-buf=-250.00 ms buf=0x"));
  g_assert_true (g_str_has_suffix (lines[2], "buffer dur 0:00:00.040000000"));
  g_assert_cmpstr (lines[3], ==, "BUFFER PTS is greater than ELEMENT TIME, IT's EARLY");
  g_assert_cmpstr (lines[4], ==, "");
  g_strfreev (lines);
  g_free (s);

  /* silent (the default) prints only the **PERF lines */
  g_object_set (h->element, "silent", TRUE, NULL);
  push_frames (h, 0, 2);
  assert_output ("");
  logger_free (h);
}

int
main (int argc, char **argv)
{
  gint ret;

  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  out = g_string_new (NULL);

  g_test_add_func ("/cpu/logger/properties", test_properties);
  g_test_add_func ("/cpu/logger/caps-any", test_caps_any);
  g_test_add_func ("/cpu/logger/noops-warn-once", test_noops_warn_once);
  g_test_add_func ("/cpu/logger/handoff", test_handoff);
  g_test_add_func ("/cpu/logger/passthrough", test_passthrough);
  g_test_add_func ("/cpu/logger/first-print", test_first_print_after_two_intervals);
  g_test_add_func ("/cpu/logger/two-streams", test_two_streams);
  g_test_add_func ("/cpu/logger/interval", test_interval);
  g_test_add_func ("/cpu/logger/interval-zero", test_interval_zero);
  g_test_add_func ("/cpu/logger/no-batch-meta", test_no_batch_meta);
  g_test_add_func ("/cpu/logger/nothing-seen", test_nothing_seen);
  g_test_add_func ("/cpu/logger/restart", test_restart);
  g_test_add_func ("/cpu/logger/pause-resume", test_pause_resume);
  g_test_add_func ("/cpu/logger/flush-reset", test_flush_reset);
  g_test_add_func ("/cpu/logger/silent-false", test_silent_false);
  ret = g_test_run ();

  g_string_free (out, TRUE);
  gst_deinit ();
  return ret;
}
