/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/check/gstharness.h>
#include <rds/rds-event.h>

#define N_TYPES 6

static const RdsEventType all_types[N_TYPES] = {
  RDS_EVENT_PAD_ADDED, RDS_EVENT_PAD_DELETED, RDS_EVENT_STREAM_EOS,
  RDS_EVENT_STREAM_SEGMENT, RDS_EVENT_STREAM_RESET, RDS_EVENT_STREAM_START,
};

static const gchar *structure_names[N_TYPES] = {
  "rds-pad-added", "rds-pad-deleted", "rds-stream-eos",
  "rds-stream-segment", "rds-stream-reset", "rds-stream-start",
};

static GstEvent *
make_event (guint i, guint source_id)
{
  GstSegment segment;

  switch (i) {
    case 0:
      return rds_event_new_pad_added (source_id);
    case 1:
      return rds_event_new_pad_deleted (source_id);
    case 2:
      return rds_event_new_stream_eos (source_id);
    case 3:
      gst_segment_init (&segment, GST_FORMAT_TIME);
      segment.rate = 2.0;
      segment.start = 5 * GST_SECOND;
      return rds_event_new_stream_segment (source_id, &segment);
    case 4:
      return rds_event_new_stream_reset (source_id);
    case 5:
      return rds_event_new_stream_start (source_id, "cam-7/video");
    default:
      g_assert_not_reached ();
  }
  return NULL;
}

/* Runs parser i on ev; returns its result and the parsed source id. */
static gboolean
parse_event (guint i, GstEvent * ev, guint * source_id)
{
  GstSegment *segment = NULL;
  gchar *stream_id = NULL;

  switch (i) {
    case 0:
      return rds_event_parse_pad_added (ev, source_id);
    case 1:
      return rds_event_parse_pad_deleted (ev, source_id);
    case 2:
      return rds_event_parse_stream_eos (ev, source_id);
    case 3:
      return rds_event_parse_stream_segment (ev, source_id, &segment);
    case 4:
      return rds_event_parse_stream_reset (ev, source_id);
    case 5:
      return rds_event_parse_stream_start (ev, source_id, &stream_id);
    default:
      g_assert_not_reached ();
  }
  return FALSE;
}

static void
test_round_trip (void)
{
  for (guint i = 0; i < N_TYPES; i++) {
    GstEvent *ev = make_event (i, 7);
    guint id = 0;

    g_assert_nonnull (ev);
    /* Not a bare ==: GstEventType vs RdsEventType trips -Wenum-compare; cmpint widens both to integers. */
    g_assert_cmpint (GST_EVENT_TYPE (ev), ==, all_types[i]);
    g_assert_cmpuint ((guint) GST_EVENT_TYPE (ev) >> GST_EVENT_NUM_SHIFT, ==, 500 + i);
    g_assert_true (GST_EVENT_IS_DOWNSTREAM (ev));
    g_assert_true (GST_EVENT_IS_SERIALIZED (ev));
    g_assert_false (GST_EVENT_IS_UPSTREAM (ev));
    g_assert_false (GST_EVENT_IS_STICKY (ev));
    g_assert_true (gst_structure_has_name (gst_event_get_structure (ev), structure_names[i]));
    /* Unknown to GStreamer's name table, but must not crash. */
    g_assert_nonnull (GST_EVENT_TYPE_NAME (ev));

    g_assert_true (parse_event (i, ev, &id));
    g_assert_cmpuint (id, ==, 7);
    gst_event_unref (ev);
  }

  {
    GstEvent *ev = make_event (3, 7);
    GstSegment *segment = NULL;
    guint id = 0;

    g_assert_true (rds_event_parse_stream_segment (ev, &id, &segment));
    g_assert_cmpuint (id, ==, 7);
    g_assert_nonnull (segment);
    g_assert_cmpint (segment->format, ==, GST_FORMAT_TIME);
    g_assert_cmpfloat (segment->rate, ==, 2.0);
    g_assert_cmpuint (segment->start, ==, 5 * GST_SECOND);
    /* All outputs optional. */
    g_assert_true (rds_event_parse_stream_segment (ev, NULL, NULL));
    gst_event_unref (ev);
  }

  {
    GstEvent *ev = make_event (5, 7);
    gchar *stream_id = NULL;
    guint id = 0;

    g_assert_true (rds_event_parse_stream_start (ev, &id, &stream_id));
    g_assert_cmpuint (id, ==, 7);
    g_assert_cmpstr (stream_id, ==, "cam-7/video");
    g_assert_true (rds_event_parse_stream_start (ev, NULL, NULL));
    gst_event_unref (ev);
  }

  {
    GstEvent *ev = rds_event_new_stream_start (3, NULL);
    gchar *stream_id = (gchar *) "untouched";
    guint id = 0;

    g_assert_true (rds_event_parse_stream_start (ev, &id, &stream_id));
    g_assert_cmpuint (id, ==, 3);
    g_assert_null (stream_id);
    gst_event_unref (ev);
  }
}

static void
test_wrong_type (void)
{
  GstEvent *eos_ev = rds_event_new_stream_eos (1);
  GstEvent *stock = gst_event_new_eos ();
  GstEvent *lookalike = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
      gst_structure_new ("rds-pad-added", "source-id", G_TYPE_UINT, 9, NULL));
  guint id = 42;

  g_assert_false (rds_event_parse_pad_added (eos_ev, &id));
  g_assert_cmpuint (id, ==, 42);
  g_assert_false (rds_event_parse_stream_eos (stock, NULL));
  g_assert_false (rds_event_parse_stream_eos (stock, &id));
  g_assert_cmpuint (id, ==, 42);
  /* Our structure name on a stock custom type is not ours either: the type decides. */
  g_assert_false (rds_event_parse_pad_added (lookalike, &id));
  g_assert_cmpuint (id, ==, 42);

  /* Every parser rejects every other type, and stock events. */
  for (guint i = 0; i < N_TYPES; i++) {
    for (guint j = 0; j < N_TYPES; j++) {
      GstEvent *ev = make_event (j, 5);

      id = 42;
      g_assert_cmpint (parse_event (i, ev, &id), ==, i == j);
      g_assert_cmpuint (id, ==, i == j ? 5 : 42);
      gst_event_unref (ev);
    }
    id = 42;
    g_assert_false (parse_event (i, stock, &id));
    g_assert_cmpuint (id, ==, 42);
  }

  /* Distinct from the stock EOS. */
  g_assert_cmpint (GST_EVENT_TYPE (eos_ev), !=, GST_EVENT_EOS);

  gst_event_unref (lookalike);
  gst_event_unref (stock);
  gst_event_unref (eos_ev);
}

static const gchar *
dispatch (GstEvent * ev)
{
  switch ((guint) GST_EVENT_TYPE (ev)) {
    case RDS_EVENT_PAD_ADDED:
      return "pad-added";
    case RDS_EVENT_PAD_DELETED:
      return "pad-deleted";
    case RDS_EVENT_STREAM_EOS:
      return "stream-eos";
    case RDS_EVENT_STREAM_SEGMENT:
      return "stream-segment";
    case RDS_EVENT_STREAM_RESET:
      return "stream-reset";
    case RDS_EVENT_STREAM_START:
      return "stream-start";
    case GST_EVENT_EOS:
      return "eos";
    default:
      return "other";
  }
}

/* The documented warning-free idiom: switch on the RdsEventType cast, with a default for stock types. */
static const gchar *
dispatch_rds (GstEvent * ev)
{
  switch ((RdsEventType) GST_EVENT_TYPE (ev)) {
    case RDS_EVENT_PAD_ADDED:
      return "pad-added";
    case RDS_EVENT_PAD_DELETED:
      return "pad-deleted";
    case RDS_EVENT_STREAM_EOS:
      return "stream-eos";
    case RDS_EVENT_STREAM_SEGMENT:
      return "stream-segment";
    case RDS_EVENT_STREAM_RESET:
      return "stream-reset";
    case RDS_EVENT_STREAM_START:
      return "stream-start";
    default:
      return "other";
  }
}

static void
test_switchable (void)
{
  static const gchar *expected[N_TYPES] = {
    "pad-added", "pad-deleted", "stream-eos", "stream-segment", "stream-reset", "stream-start",
  };
  GstEvent *eos = gst_event_new_eos ();

  for (guint i = 0; i < N_TYPES; i++) {
    GstEvent *ev = make_event (i, 1);

    g_assert_cmpstr (dispatch (ev), ==, expected[i]);
    g_assert_cmpstr (dispatch_rds (ev), ==, expected[i]);
    g_assert_true ((RdsEventType) GST_EVENT_TYPE (ev) == all_types[i]);
    gst_event_unref (ev);
  }
  g_assert_cmpstr (dispatch (eos), ==, "eos");
  g_assert_cmpstr (dispatch_rds (eos), ==, "other");
  gst_event_unref (eos);
}

/* Our type but missing or wrongly typed fields: FALSE, outputs untouched. */
static void
test_malformed (void)
{
  GstEvent *no_segment = gst_event_new_custom ((GstEventType) RDS_EVENT_STREAM_SEGMENT,
      gst_structure_new ("rds-stream-segment", "source-id", G_TYPE_UINT, 3, NULL));
  GstEvent *no_source = gst_event_new_custom ((GstEventType) RDS_EVENT_PAD_ADDED,
      gst_structure_new_empty ("rds-pad-added"));
  GstEvent *bad_source = gst_event_new_custom ((GstEventType) RDS_EVENT_STREAM_EOS,
      gst_structure_new ("rds-stream-eos", "source-id", G_TYPE_STRING, "3", NULL));
  GstEvent *no_stream_id = gst_event_new_custom ((GstEventType) RDS_EVENT_STREAM_START,
      gst_structure_new ("rds-stream-start", "source-id", G_TYPE_UINT, 3, NULL));
  GstSegment *segment = (GstSegment *) 0x1;
  gchar *stream_id = (gchar *) "untouched";
  guint id = 42;

  g_assert_false (rds_event_parse_stream_segment (no_segment, &id, &segment));
  g_assert_cmpuint (id, ==, 42);
  g_assert_true (segment == (GstSegment *) 0x1);
  g_assert_false (rds_event_parse_pad_added (no_source, &id));
  g_assert_cmpuint (id, ==, 42);
  g_assert_false (rds_event_parse_stream_eos (bad_source, &id));
  g_assert_cmpuint (id, ==, 42);
  g_assert_false (rds_event_parse_stream_start (no_stream_id, &id, &stream_id));
  g_assert_cmpuint (id, ==, 42);
  g_assert_cmpstr (stream_id, ==, "untouched");

  gst_event_unref (no_stream_id);
  gst_event_unref (bad_source);
  gst_event_unref (no_source);
  gst_event_unref (no_segment);
}

/* Records, in arrival order at the harness sink pad, buffers (by offset) and our events. */
typedef struct
{
  GMutex lock;
  GPtrArray *seen;
} Trace;

static GstPadProbeReturn
record_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Trace *trace = user_data;
  gchar *item = NULL;
  guint id = 0;

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    item = g_strdup_printf ("buffer-%" G_GUINT64_FORMAT, GST_BUFFER_OFFSET (GST_PAD_PROBE_INFO_BUFFER (info)));
  } else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);

    if (rds_event_parse_pad_added (ev, &id))
      item = g_strdup_printf ("pad-added-%u", id);
    else if (rds_event_parse_stream_eos (ev, &id))
      item = g_strdup_printf ("stream-eos-%u", id);
  }
  if (item) {
    g_mutex_lock (&trace->lock);
    g_ptr_array_add (trace->seen, item);
    g_mutex_unlock (&trace->lock);
  }
  return GST_PAD_PROBE_OK;
}

static GstBuffer *
marked_buffer (guint64 offset)
{
  GstBuffer *buf = gst_buffer_new_allocate (NULL, 16, NULL);

  GST_BUFFER_OFFSET (buf) = offset;
  return buf;
}

static void
check_passes (const gchar * element)
{
  static const gchar *expected[] = {
    "buffer-0", "pad-added-1", "buffer-1", "stream-eos-1", "buffer-2",
  };
  GstHarness *h = gst_harness_new (element);
  Trace trace;
  GstEvent *ev;
  guint id = 0;

  g_mutex_init (&trace.lock);
  trace.seen = g_ptr_array_new_with_free_func (g_free);
  gst_pad_add_probe (h->sinkpad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      record_probe, &trace, NULL);

  gst_harness_set_src_caps_str (h, "video/x-raw");
  g_assert_cmpint (gst_harness_push (h, marked_buffer (0)), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (h, rds_event_new_pad_added (1)));
  g_assert_cmpint (gst_harness_push (h, marked_buffer (1)), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (h, rds_event_new_stream_eos (1)));
  g_assert_cmpint (gst_harness_push (h, marked_buffer (2)), ==, GST_FLOW_OK);

  /* Pulling the last buffer (blocking, queue has its own thread) means everything before it went through the probe. */
  for (guint64 i = 0; i < 3; i++) {
    GstBuffer *buf = gst_harness_pull (h);

    g_assert_nonnull (buf);
    g_assert_cmpuint (GST_BUFFER_OFFSET (buf), ==, i);
    gst_buffer_unref (buf);
  }

  g_mutex_lock (&trace.lock);
  g_assert_cmpuint (trace.seen->len, ==, G_N_ELEMENTS (expected));
  for (guint i = 0; i < G_N_ELEMENTS (expected); i++)
    g_assert_cmpstr (g_ptr_array_index (trace.seen, i), ==, expected[i]);
  g_mutex_unlock (&trace.lock);

  /* The event queue: stock stream-start/caps/segment, then ours, untouched. */
  for (ev = gst_harness_pull_event (h); ev && (guint) GST_EVENT_TYPE (ev) != RDS_EVENT_PAD_ADDED;
      ev = gst_harness_pull_event (h)) {
    g_assert_true (GST_EVENT_TYPE (ev) == GST_EVENT_STREAM_START || GST_EVENT_TYPE (ev) == GST_EVENT_CAPS
        || GST_EVENT_TYPE (ev) == GST_EVENT_SEGMENT);
    gst_event_unref (ev);
  }
  g_assert_nonnull (ev);
  g_assert_true (rds_event_parse_pad_added (ev, &id));
  g_assert_cmpuint (id, ==, 1);
  gst_event_unref (ev);
  ev = gst_harness_pull_event (h);
  g_assert_nonnull (ev);
  g_assert_true (rds_event_parse_stream_eos (ev, &id));
  g_assert_cmpuint (id, ==, 1);
  gst_event_unref (ev);
  g_assert_null (gst_harness_try_pull_event (h));

  gst_harness_teardown (h);
  g_ptr_array_unref (trace.seen);
  g_mutex_clear (&trace.lock);
}

static void
test_passes_elements (void)
{
  check_passes ("queue");
  check_passes ("identity");
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/event/round-trip", test_round_trip);
  g_test_add_func ("/event/wrong-type", test_wrong_type);
  g_test_add_func ("/event/switchable", test_switchable);
  g_test_add_func ("/event/malformed", test_malformed);
  g_test_add_func ("/event/passes-elements", test_passes_elements);
  return g_test_run ();
}
