/* rdsstreammux: aggregator timing probe, factory, properties, caps, latency (cpu) and memory:RDS batching, timing,
 * stream events, EOS and dynamic pads (gpu, skipped without a ROCm device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/base/gstaggregator.h>
#include <gst/check/gstharness.h>
#include <rds/rds.h>
#include <string.h>

/* ---- GstAggregator timing probe ----
 *
 * rdsstreammux builds on GstAggregator and maps batched-push-timeout onto its live (or forced-live) timeout. This
 * test-local aggregator records every aggregate () call: when it happened, whether it was a timeout, and which
 * pads had a buffer. Two appsrcs feed it; source B stalls after 3 buffers. The tests below pin down when the batch
 * without B is produced in each mode, which is what the element's timeout design relies on. */

typedef enum
{
  NEXT_TIME_NONE,               /* no get_next_time: the aggregator never times out */
  NEXT_TIME_SIMPLE,             /* gst_aggregator_simple_get_next_time: the output position */
  NEXT_TIME_FIRST_BUFFER,       /* the running time at which the batch's first buffer was seen */
} ProbeNextTime;

typedef struct
{
  gint64 t;                     /* µs since the stall started (negative before) */
  gboolean timeout;
  guint mask;                   /* bit i: sink pad i had a buffer */
} ProbeCall;

#define PROBE_TYPE_AGG (probe_agg_get_type ())
G_DECLARE_FINAL_TYPE (ProbeAgg, probe_agg, PROBE, AGG, GstAggregator)
struct _ProbeAgg
{
  GstAggregator parent;
  ProbeNextTime next_time;
  GstClockTime frame_duration;
  GstClockTime batch_start;     /* NEXT_TIME_FIRST_BUFFER: running time, NONE while no buffer is pending */
  GMutex lock;
  gint64 stall_start;           /* monotonic µs, 0 until the stall starts */
  GArray *calls;                /* ProbeCall */
};
G_DEFINE_TYPE (ProbeAgg, probe_agg, GST_TYPE_AGGREGATOR);

static GstStaticPadTemplate probe_sink_template = GST_STATIC_PAD_TEMPLATE ("sink_%u", GST_PAD_SINK,
    GST_PAD_REQUEST, GST_STATIC_CAPS ("application/x-probe"));
static GstStaticPadTemplate probe_src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-probe"));

static guint
probe_pad_index (GstPad * pad)
{
  const gchar *name = GST_PAD_NAME (pad);

  return (guint) g_ascii_strtoull (name + strlen ("sink_"), NULL, 10);
}

static GstClockTime
probe_get_next_time (GstAggregator * agg)
{
  ProbeAgg *self = PROBE_AGG (agg);
  gboolean any = FALSE;

  if (self->next_time == NEXT_TIME_SIMPLE)
    return gst_aggregator_simple_get_next_time (agg);

  /* NEXT_TIME_FIRST_BUFFER: the timer starts when the first buffer of a batch shows up */
  GST_OBJECT_LOCK (agg);
  for (GList * l = GST_ELEMENT (agg)->sinkpads; l != NULL; l = l->next)
    any |= gst_aggregator_pad_has_buffer (GST_AGGREGATOR_PAD (l->data));
  GST_OBJECT_UNLOCK (agg);
  if (!any)
    return GST_CLOCK_TIME_NONE;
  if (!GST_CLOCK_TIME_IS_VALID (self->batch_start))
    self->batch_start = gst_element_get_current_running_time (GST_ELEMENT (agg));
  return self->batch_start;
}

static GstFlowReturn
probe_aggregate (GstAggregator * agg, gboolean timeout)
{
  ProbeAgg *self = PROBE_AGG (agg);
  ProbeCall call = {.timeout = timeout };
  GstClockTime pts = GST_CLOCK_TIME_NONE;
  gboolean all_eos = TRUE;
  GstBuffer *out;

  GST_OBJECT_LOCK (agg);
  for (GList * l = GST_ELEMENT (agg)->sinkpads; l != NULL; l = l->next) {
    GstAggregatorPad *pad = GST_AGGREGATOR_PAD (l->data);
    GstBuffer *buf = gst_aggregator_pad_pop_buffer (pad);

    if (buf != NULL) {
      call.mask |= 1u << probe_pad_index (GST_PAD (pad));
      if (!GST_CLOCK_TIME_IS_VALID (pts) || GST_BUFFER_PTS (buf) < pts)
        pts = GST_BUFFER_PTS (buf);
      gst_buffer_unref (buf);
    }
    all_eos &= gst_aggregator_pad_is_eos (pad);
  }
  GST_OBJECT_UNLOCK (agg);
  self->batch_start = GST_CLOCK_TIME_NONE;

  if (call.mask == 0)
    return all_eos ? GST_FLOW_EOS : GST_AGGREGATOR_FLOW_NEED_DATA;

  g_mutex_lock (&self->lock);
  call.t = self->stall_start != 0 ? g_get_monotonic_time () - self->stall_start : -1;
  g_array_append_val (self->calls, call);
  g_mutex_unlock (&self->lock);

  out = gst_buffer_new ();
  GST_BUFFER_PTS (out) = pts;
  GST_OBJECT_LOCK (agg);
  GST_AGGREGATOR_PAD (agg->srcpad)->segment.position = pts + self->frame_duration;
  GST_OBJECT_UNLOCK (agg);
  return gst_aggregator_finish_buffer (agg, out);
}

static GstCaps *
probe_fixate_src_caps (G_GNUC_UNUSED GstAggregator * agg, GstCaps * caps)
{
  gst_caps_unref (caps);
  return gst_caps_from_string ("application/x-probe");
}

static void
probe_agg_finalize (GObject * object)
{
  ProbeAgg *self = PROBE_AGG (object);

  g_array_unref (self->calls);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (probe_agg_parent_class)->finalize (object);
}

static void
probe_agg_class_init (ProbeAggClass * klass)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstAggregatorClass *agg_class = GST_AGGREGATOR_CLASS (klass);

  G_OBJECT_CLASS (klass)->finalize = probe_agg_finalize;
  gst_element_class_add_static_pad_template_with_gtype (element_class, &probe_sink_template,
      GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_add_static_pad_template_with_gtype (element_class, &probe_src_template,
      GST_TYPE_AGGREGATOR_PAD);
  gst_element_class_set_static_metadata (element_class, "Probe aggregator", "Generic", "Records aggregate calls",
      "Redstream contributors");
  agg_class->aggregate = probe_aggregate;
  agg_class->get_next_time = probe_get_next_time;
  agg_class->fixate_src_caps = probe_fixate_src_caps;
}

static void
probe_agg_init (ProbeAgg * self)
{
  g_mutex_init (&self->lock);
  self->calls = g_array_new (FALSE, TRUE, sizeof (ProbeCall));
  self->batch_start = GST_CLOCK_TIME_NONE;
}

typedef struct
{
  const gchar *name;
  gboolean force_live;          /* gst_aggregator_set_force_live, in a non-live pipeline */
  gboolean live;                /* is-live appsrcs, real-time pushing */
  ProbeNextTime next_time;
  GstClockTime step;            /* PTS step between frames */
  gint64 pre_stall_sleep;       /* µs to wait between B's last buffer and the stall start (A's next buffer) */
} ProbeMode;

static GstBuffer *
probe_buffer (GstClockTime pts)
{
  GstBuffer *b = gst_buffer_new ();

  GST_BUFFER_PTS (b) = pts;
  return b;
}

/* Runs one mode: A and B push 3 frames each, then (after @pre_stall_sleep) A pushes 4 more while B stalls for
 * @stall µs; then both end. Returns the recorded calls (owned by the caller). */
static GArray *
probe_run (const ProbeMode * m, gint64 stall)
{
  GstElement *pipe = gst_pipeline_new (NULL);
  GstElement *src[2], *sink = gst_element_factory_make ("fakesink", NULL);
  ProbeAgg *agg = g_object_new (PROBE_TYPE_AGG, NULL);
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg;
  GArray *calls;
  gint64 t0;

  agg->next_time = m->next_time;
  agg->frame_duration = m->step;
  if (m->next_time == NEXT_TIME_NONE)
    GST_AGGREGATOR_GET_CLASS (agg)->get_next_time = NULL;       /* class field: restored below */
  g_object_set (agg, "latency", (guint64) 40 * GST_MSECOND, NULL);
  gst_aggregator_set_force_live (GST_AGGREGATOR (agg), m->force_live);
  g_object_set (sink, "sync", FALSE, "async", FALSE, NULL);
  gst_bin_add_many (GST_BIN (pipe), GST_ELEMENT (agg), sink, NULL);
  for (guint i = 0; i < 2; i++) {
    GstCaps *caps = gst_caps_from_string ("application/x-probe");

    src[i] = gst_element_factory_make ("appsrc", NULL);
    g_object_set (src[i], "format", GST_FORMAT_TIME, "caps", caps, "is-live", m->live, "min-latency",
        (gint64) 0, "do-timestamp", m->live, NULL);
    gst_caps_unref (caps);
    gst_bin_add (GST_BIN (pipe), src[i]);
    g_assert_true (gst_element_link_pads (src[i], "src", GST_ELEMENT (agg), NULL));
  }
  g_assert_true (gst_element_link (GST_ELEMENT (agg), sink));
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  if (m->live)
    g_assert_cmpint (gst_element_get_state (pipe, NULL, NULL, 5 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);

  t0 = g_get_monotonic_time ();
  for (guint f = 0; f < 3; f++) {
    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), probe_buffer (f * m->step)), ==,
          GST_FLOW_OK);
    if (m->live)
      g_usleep (m->step / 1000);
  }
  g_usleep (m->pre_stall_sleep);
  g_mutex_lock (&agg->lock);
  agg->stall_start = g_get_monotonic_time ();
  g_mutex_unlock (&agg->lock);
  for (guint f = 3; f < 7; f++) {
    g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[0]), probe_buffer (f * m->step)), ==, GST_FLOW_OK);
    if (m->live)
      g_usleep (m->step / 1000);
  }
  while (g_get_monotonic_time () < agg->stall_start + stall)
    g_usleep (10000);
  gst_app_src_end_of_stream (GST_APP_SRC (src[1]));
  gst_app_src_end_of_stream (GST_APP_SRC (src[0]));
  msg = gst_bus_timed_pop_filtered (bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  gst_element_set_state (pipe, GST_STATE_NULL);

  calls = g_array_ref (agg->calls);
  g_test_message ("%s (pre-stall %" G_GINT64_FORMAT " ms, %" G_GINT64_FORMAT " ms after start):", m->name,
      m->pre_stall_sleep / 1000, (agg->stall_start - t0) / 1000);
  for (guint i = 0; i < calls->len; i++) {
    ProbeCall *c = &g_array_index (calls, ProbeCall, i);
    g_test_message ("  batch %u: t %+" G_GINT64_FORMAT " ms, timeout %d, pads %s%s", i, c->t / 1000, c->timeout,
        c->mask & 1 ? "A" : "", c->mask & 2 ? "B" : "");
  }
  if (m->next_time == NEXT_TIME_NONE)
    GST_AGGREGATOR_GET_CLASS (agg)->get_next_time = probe_get_next_time;
  gst_object_unref (bus);
  gst_object_unref (pipe);
  return calls;
}

/* The first call after the stall started that has A but not B. */
static const ProbeCall *
first_without_b (GArray * calls)
{
  for (guint i = 0; i < calls->len; i++) {
    ProbeCall *c = &g_array_index (calls, ProbeCall, i);
    if (c->t >= 0 && c->mask == 1)
      return c;
  }
  return NULL;
}

/* What rdsstreammux relies on (GStreamer 1.28):
 * - non-live, no timeout: aggregate () only runs once every pad has a buffer or is EOS, so a stalled source holds
 *   the batch back until it ends;
 * - forced-live with get_next_time = "running time when the batch's first buffer was seen": the partial batch
 *   comes out one latency after that buffer, whether the timestamps run ahead of the clock (fast file decode) or
 *   behind it, and once per batch.
 * (The aggregator's own gst_aggregator_simple_get_next_time waits for the clock to reach the output position, so
 * it fires late when timestamps run ahead and at once when they lag: not a usable batch timeout.) */
static void
test_aggregator_probe (void)
{
  static const ProbeMode non_live = { "non-live", FALSE, FALSE, NEXT_TIME_SIMPLE, 33 * GST_MSECOND, 0 };
  static const ProbeMode timed[] = {
    {"forced-live, timestamps ahead of the clock", TRUE, FALSE, NEXT_TIME_FIRST_BUFFER, 300 * GST_MSECOND, 0},
    {"forced-live, timestamps behind the clock", TRUE, FALSE, NEXT_TIME_FIRST_BUFFER, GST_MSECOND, 100 * 1000},
  };
  const gint64 stall = 350 * 1000;
  GArray *calls;
  const ProbeCall *c;

  calls = probe_run (&non_live, stall);
  c = first_without_b (calls);
  g_assert_nonnull (c);
  g_assert_cmpint (c->t, >=, stall - 20 * 1000);
  for (guint i = 0; i < calls->len; i++)
    g_assert_false (g_array_index (calls, ProbeCall, i).timeout);
  g_array_unref (calls);

  for (guint m = 0; m < G_N_ELEMENTS (timed); m++) {
    gint64 prev = 0;
    guint partial = 0;

    calls = probe_run (&timed[m], stall);
    /* A's 4 stalled frames come out one by one, each a timeout one latency (40 ms) after the previous batch;
     * generous upper bounds for loaded CI and sanitizers */
    for (guint i = 0; i < calls->len; i++) {
      c = &g_array_index (calls, ProbeCall, i);
      if (c->t < 0 || c->mask != 1)
        continue;
      g_assert_true (c->timeout);
      g_assert_cmpint (c->t - prev, >=, 35 * 1000);
      g_assert_cmpint (c->t - prev, <=, 250 * 1000);
      prev = c->t;
      partial++;
    }
    g_assert_cmpuint (partial, ==, 4);
    g_array_unref (calls);
  }
}

/* ---- cpu ---- */

static const gchar *formats[] = { "NV12", "I420", "RGBA", "BGRA", "RGBx", "BGRx", "RGB", "BGR", "GRAY8" };

static void
check_formats (const GstStructure * s)
{
  const GValue *list = gst_structure_get_value (s, "format");

  g_assert_nonnull (list);
  g_assert_true (GST_VALUE_HOLDS_LIST (list));
  g_assert_cmpuint (gst_value_list_get_size (list), ==, G_N_ELEMENTS (formats));
  for (guint k = 0; k < G_N_ELEMENTS (formats); k++) {
    gboolean found = FALSE;
    for (guint j = 0; j < gst_value_list_get_size (list); j++)
      found |= g_strcmp0 (g_value_get_string (gst_value_list_get_value (list, j)), formats[k]) == 0;
    g_assert_true (found);
  }
}

static void
test_factory (void)
{
  GstElementFactory *factory = gst_element_factory_find ("rdsstreammux");
  gboolean seen_sink = FALSE, seen_src = FALSE;

  g_assert_nonnull (factory);
  g_assert_cmpuint (gst_plugin_feature_get_rank (GST_PLUGIN_FEATURE (factory)), ==, GST_RANK_NONE);
  g_assert_cmpstr (gst_element_factory_get_metadata (factory, GST_ELEMENT_METADATA_KLASS), ==,
      "Codec/Muxer/Video");
  for (const GList * l = gst_element_factory_get_static_pad_templates (factory); l != NULL; l = l->next) {
    GstStaticPadTemplate *t = l->data;
    GstCaps *caps = gst_static_caps_get (&t->static_caps);

    if (t->direction == GST_PAD_SINK) {
      gboolean rds = FALSE, sys = FALSE;

      seen_sink = TRUE;
      g_assert_cmpstr (t->name_template, ==, "sink_%u");
      g_assert_cmpint (t->presence, ==, GST_PAD_REQUEST);
      for (guint i = 0; i < gst_caps_get_size (caps); i++) {
        GstCapsFeatures *f = gst_caps_get_features (caps, i);

        rds |= gst_caps_features_contains (f, RDS_CAPS_FEATURE_MEMORY_RDS);
        sys |= gst_caps_features_is_equal (f, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY);
        check_formats (gst_caps_get_structure (caps, i));
      }
      g_assert_true (rds && sys);
    } else {
      seen_src = TRUE;
      g_assert_cmpstr (t->name_template, ==, "src");
      g_assert_cmpuint (gst_caps_get_size (caps), ==, 1);
      g_assert_true (gst_caps_features_contains (gst_caps_get_features (caps, 0), RDS_CAPS_FEATURE_MEMORY_RDS));
      check_formats (gst_caps_get_structure (caps, 0));
    }
    gst_caps_unref (caps);
  }
  g_assert_true (seen_sink && seen_src);
  gst_object_unref (factory);
}

static GParamSpec *
find_prop (GstElement * e, const gchar * name, GType type)
{
  GParamSpec *p = g_object_class_find_property (G_OBJECT_GET_CLASS (e), name);

  g_assert_nonnull (p);
  g_assert_true (G_TYPE_CHECK_INSTANCE_TYPE (p, type));
  g_assert_true (p->flags & G_PARAM_READWRITE);
  return p;
}

static void
check_uint (GstElement * e, const gchar * name, guint def, guint lo, guint hi)
{
  GParamSpec *p = find_prop (e, name, G_TYPE_PARAM_UINT);

  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->default_value, ==, def);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->minimum, ==, lo);
  g_assert_cmpuint (G_PARAM_SPEC_UINT (p)->maximum, ==, hi);
}

/* An enum property: default, and a nick for each integer in lo..hi and nothing else. */
static void
check_enum (GstElement * e, const gchar * name, gint def, gint lo, gint hi)
{
  GParamSpec *p = find_prop (e, name, G_TYPE_PARAM_ENUM);
  GEnumClass *ec = G_PARAM_SPEC_ENUM (p)->enum_class;

  g_assert_cmpint (G_PARAM_SPEC_ENUM (p)->default_value, ==, def);
  g_assert_cmpuint (ec->n_values, ==, (guint) (hi - lo + 1));
  for (gint v = lo; v <= hi; v++) {
    GEnumValue *ev = g_enum_get_value (ec, v);
    g_assert_nonnull (ev);
    g_assert_nonnull (ev->value_nick);
  }
}

static void
test_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsstreammux", NULL);
  GParamSpec *p;
  gint en;

  g_assert_nonnull (e);
  check_uint (e, "batch-size", 1, 1, G_MAXUINT);
  check_uint (e, "width", 0, 0, RDS_BUF_SURFACE_MAX_DIM);
  check_uint (e, "height", 0, 0, RDS_BUF_SURFACE_MAX_DIM);
  check_uint (e, "gpu-id", 0, 0, G_MAXUINT);
  check_uint (e, "buffer-pool-size", 4, 1, 1024);
  check_uint (e, "num-surfaces-per-frame", 1, 1, 4);
  check_enum (e, "nvbuf-memory-type", 0, 0, 3);
  check_enum (e, "compute-hw", 0, 0, 2);
  check_enum (e, "interpolation-method", 6, 0, 6);
  p = find_prop (e, "enable-padding", G_TYPE_PARAM_BOOLEAN);
  g_assert_false (G_PARAM_SPEC_BOOLEAN (p)->default_value);
  p = find_prop (e, "batched-push-timeout", G_TYPE_PARAM_INT);
  g_assert_cmpint (G_PARAM_SPEC_INT (p)->default_value, ==, -1);
  g_assert_cmpint (G_PARAM_SPEC_INT (p)->minimum, ==, -1);
  g_assert_cmpint (G_PARAM_SPEC_INT (p)->maximum, ==, G_MAXINT);

  /* configs set enums by integer */
  gst_util_set_object_arg (G_OBJECT (e), "nvbuf-memory-type", "3");
  g_object_get (e, "nvbuf-memory-type", &en, NULL);
  g_assert_cmpint (en, ==, 3);
  gst_util_set_object_arg (G_OBJECT (e), "interpolation-method", "1");
  g_object_get (e, "interpolation-method", &en, NULL);
  g_assert_cmpint (en, ==, 1);
  gst_object_unref (e);
}

/* Runs @desc until it posts an error; returns it (and its debug string in @debug when not NULL). */
static GError *
pipeline_error (const gchar * desc, gchar ** debug)
{
  GError *err = NULL;
  GstElement *pipe = gst_parse_launch (desc, &err);
  GstBus *bus;
  GstMessage *msg;

  g_assert_no_error (err);
  bus = gst_element_get_bus (pipe);
  gst_element_set_state (pipe, GST_STATE_PLAYING);
  msg = gst_bus_timed_pop_filtered (bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_ERROR);
  gst_message_parse_error (msg, &err, debug);
  gst_message_unref (msg);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (bus);
  gst_object_unref (pipe);
  return err;
}

/* The scaling mux needs an output size: without one, negotiation fails with a message that says so (before any
 * GPU work, so this runs without a GPU). */
static void
test_no_size (void)
{
  static const gchar *sizes[] = { "", "width=640", "height=480" };

  for (guint i = 0; i < G_N_ELEMENTS (sizes); i++) {
    gchar *desc = g_strdup_printf ("videotestsrc num-buffers=2 ! video/x-raw,format=NV12,width=64,height=48 "
        "! m.sink_0 rdsstreammux name=m %s ! fakesink", sizes[i]);
    gchar *debug = NULL;
    GError *err = pipeline_error (desc, &debug);

    g_assert_error (err, GST_CORE_ERROR, GST_CORE_ERROR_NEGOTIATION);
    g_assert_nonnull (strstr (err->message, "width"));
    g_assert_nonnull (strstr (err->message, "height"));
    g_clear_error (&err);
    g_free (debug);
    g_free (desc);
  }
}

/* Settings we cannot honour fail at start, before any GPU work. */
static void
test_unsupported_settings (void)
{
  static const gchar *props[] = { "compute-hw=2", "num-surfaces-per-frame=2" };

  for (guint i = 0; i < G_N_ELEMENTS (props); i++) {
    gchar *desc = g_strdup_printf ("videotestsrc num-buffers=1 ! video/x-raw,format=NV12,width=64,height=48 "
        "! m.sink_0 rdsstreammux name=m width=64 height=48 %s ! fakesink", props[i]);
    GError *err = pipeline_error (desc, NULL);

    g_assert_error (err, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_SETTINGS);
    g_clear_error (&err);
    g_free (desc);
  }
}

/* ---- gpu helpers ---- */

#define RDS_NV12(w, h) "video/x-raw(memory:RDS),format=NV12,width=" #w ",height=" #h ",framerate=30/1"
#define SYS_I420(w, h) "video/x-raw,format=I420,width=" #w ",height=" #h ",framerate=30/1"
#define OUT_NV12 "video/x-raw(memory:RDS),format=NV12"

static GstBufferPool *
make_pool (const gchar * caps_str)
{
  GstBufferPool *pool = rds_buffer_pool_new ();
  GstCaps *caps = gst_caps_from_string (caps_str);
  GstStructure *config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, caps, 0, 1, 0);
  rds_buffer_pool_config_set_surface_params (config, 1, RDS_BUF_MEM_DEFAULT, 0);
  g_assert_true (gst_buffer_pool_set_config (pool, config));
  g_assert_true (gst_buffer_pool_set_active (pool, TRUE));
  gst_caps_unref (caps);
  return pool;
}

/* A memory:RDS frame from @pool with a deterministic pattern. */
static GstBuffer *
rds_frame (GstBufferPool * pool, guint32 seed, GstClockTime pts)
{
  GstBuffer *buf = NULL;
  RdsBufSurface *s;

  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &buf, NULL), ==, GST_FLOW_OK);
  s = rds_buffer_get_surface (buf);
  g_assert_nonnull (s);
  fill_random (s, 0, seed);
  s->numFilled = 1;
  GST_BUFFER_PTS (buf) = pts;
  return buf;
}

/* A system-memory frame in the default layout for @caps_str, every byte pseudo-random. */
static GstBuffer *
sys_frame (const gchar * caps_str, guint32 seed, GstClockTime pts)
{
  GstCaps *caps = gst_caps_from_string (caps_str);
  GstVideoInfo info;
  GstBuffer *buf;
  GstMapInfo map;

  g_assert_true (gst_video_info_from_caps (&info, caps));
  buf = gst_buffer_new_allocate (NULL, GST_VIDEO_INFO_SIZE (&info), NULL);
  g_assert_true (gst_buffer_map (buf, &map, GST_MAP_WRITE));
  rng_state = seed * 2654435761u + 1;
  for (gsize i = 0; i < map.size; i++)
    map.data[i] = rng_next ();
  gst_buffer_unmap (buf, &map);
  gst_caps_unref (caps);
  GST_BUFFER_PTS (buf) = pts;
  return buf;
}

/* CPU copy of a frame: surface 0 of a memory:RDS buffer, or a system-memory frame described by @caps_str. */
static void
host_of (GstBuffer * buf, const gchar * caps_str, HostImage * h)
{
  GstCaps *caps;
  GstVideoInfo info;
  GstVideoFrame f;

  if (rds_buffer_get_surface (buf) != NULL) {
    snapshot (rds_buffer_get_surface (buf), 0, h);
    return;
  }
  caps = gst_caps_from_string (caps_str);
  memset (h, 0, sizeof (*h));
  g_assert_true (gst_video_info_from_caps (&info, caps));
  g_assert_true (gst_video_frame_map (&f, &info, buf, GST_MAP_READ));
  h->im.format = rds_buf_color_format_from_video_format (GST_VIDEO_FRAME_FORMAT (&f));
  h->im.width = GST_VIDEO_FRAME_WIDTH (&f);
  h->im.height = GST_VIDEO_FRAME_HEIGHT (&f);
  h->num_planes = GST_VIDEO_FRAME_N_PLANES (&f);
  for (guint p = 0; p < h->num_planes; p++) {
    gint comp[GST_VIDEO_MAX_COMPONENTS];

    gst_video_format_info_component (f.info.finfo, p, comp);
    h->im.pitch[p] = GST_VIDEO_FRAME_PLANE_STRIDE (&f, p);
    h->rows[p] = GST_VIDEO_FRAME_COMP_HEIGHT (&f, comp[0]);
    h->row_bytes[p] = GST_VIDEO_FRAME_COMP_WIDTH (&f, comp[0]) * GST_VIDEO_FRAME_COMP_PSTRIDE (&f, comp[0]);
    h->im.plane[p] = g_memdup2 (GST_VIDEO_FRAME_PLANE_DATA (&f, p), (gsize) h->im.pitch[p] * h->rows[p]);
  }
  gst_video_frame_unmap (&f);
  gst_caps_unref (caps);
}

/* The black the mux fills padding with: Y 16, U/V 128 for YUV; 0 for GRAY8 and RGB formats (alpha too). */
static guint8
black_byte (RdsBufSurfaceColorFormat f, guint plane)
{
  if (ref_kind (f) == REF_KIND_YUV420)
    return plane == 0 ? 16 : 128;
  return 0;
}

/* Slot @slot of the batch must match the reference transform of rect @s of the input into rect @d of a black slot:
 * within 1 inside @d (the GPU rounds), exactly black outside. */
static void
assert_slot_rect (GstBuffer * batch, guint slot, const HostImage * hin, RefRect s, RefRect d)
{
  RdsBufSurface *surf = rds_buffer_get_surface (batch);
  HostImage hout, href;

  g_assert_nonnull (surf);
  snapshot (surf, slot, &hout);
  href = hout;
  for (guint p = 0; p < hout.num_planes; p++) {
    gsize size = (gsize) hout.im.pitch[p] * hout.rows[p];

    href.im.plane[p] = g_malloc (size);
    memset (href.im.plane[p], black_byte (hout.im.format, p), size);
  }
  ref_transform (&hin->im, &href.im, s, d, FALSE);
  for (guint p = 0; p < hout.num_planes; p++) {
    guint sub = (p > 0 && ref_kind (hout.im.format) == REF_KIND_YUV420) ? 2 : 1;
    guint pw = (hout.im.width + sub - 1) / sub, bpp = hout.row_bytes[p] / pw;

    for (guint y = 0; y < hout.rows[p]; y++) {
      for (guint x = 0; x < hout.row_bytes[p]; x++) {
        guint px = x / bpp * sub, py = y * sub;
        gsize at = (gsize) y * hout.im.pitch[p] + x;
        gboolean inside = px >= d.left && px < d.left + d.width && py >= d.top && py < d.top + d.height;
        guint diff = (guint) ABS ((gint) hout.im.plane[p][at] - (gint) href.im.plane[p][at]);

        if (inside ? diff > 1 : diff != 0)
          g_error ("slot %u plane %u row %u byte %u (%s): %u vs %u", slot, p, y, x, inside ? "inside" : "outside",
              hout.im.plane[p][at], href.im.plane[p][at]);
      }
    }
  }
  host_image_clear (&href);
  host_image_clear (&hout);
}

/* The whole input into rect @d of the slot. */
static void
assert_slot (GstBuffer * batch, guint slot, const HostImage * hin, RefRect d)
{
  assert_slot_rect (batch, slot, hin, (RefRect) { 0, 0, hin->im.width, hin->im.height }, d);
}

/* A mux with @n_in input harnesses on sink_0.., each with fixed caps, and its output harness with @out_caps
 * (NULL: NV12). @bus (optional) gets the element's messages. */
/* Records what leaves the mux, in order: batches ("B:<source ids in slot order>"), our stream events
 * ("added:N", "deleted:N", "start:N:<stream id>", "seg:N:<segment base in ms>", "eos:N", "reset:N") and EOS. */
typedef struct
{
  GMutex lock;
  GPtrArray *log;
} Recorder;

static GstPadProbeReturn
record_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Recorder *r = user_data;
  gchar *s = NULL;

  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
    GString *g = g_string_new ("B");

    for (GList * l = bm != NULL ? bm->frame_meta_list : NULL; l != NULL; l = l->next)
      g_string_append_printf (g, ":%u", ((RdsFrameMeta *) l->data)->source_id);
    s = g_string_free (g, FALSE);
  } else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
    GstSegment *seg = NULL;
    gchar *sid = NULL;
    guint id = 0;

    switch ((RdsEventType) GST_EVENT_TYPE (ev)) {
      case RDS_EVENT_PAD_ADDED:
        g_assert_true (rds_event_parse_pad_added (ev, &id));
        s = g_strdup_printf ("added:%u", id);
        break;
      case RDS_EVENT_PAD_DELETED:
        g_assert_true (rds_event_parse_pad_deleted (ev, &id));
        s = g_strdup_printf ("deleted:%u", id);
        break;
      case RDS_EVENT_STREAM_START:
        g_assert_true (rds_event_parse_stream_start (ev, &id, &sid));
        s = g_strdup_printf ("start:%u:%s", id, sid);
        break;
      case RDS_EVENT_STREAM_SEGMENT:
        g_assert_true (rds_event_parse_stream_segment (ev, &id, &seg));
        s = g_strdup_printf ("seg:%u:%" G_GUINT64_FORMAT, id, seg->base / GST_MSECOND);
        break;
      case RDS_EVENT_STREAM_EOS:
        g_assert_true (rds_event_parse_stream_eos (ev, &id));
        s = g_strdup_printf ("eos:%u", id);
        break;
      case RDS_EVENT_STREAM_RESET:
        g_assert_true (rds_event_parse_stream_reset (ev, &id));
        s = g_strdup_printf ("reset:%u", id);
        break;
      default:
        if (GST_EVENT_TYPE (ev) == GST_EVENT_EOS)
          s = g_strdup ("EOS");
        break;
    }
  }
  if (s != NULL) {
    g_mutex_lock (&r->lock);
    g_ptr_array_add (r->log, s);
    g_mutex_unlock (&r->lock);
  }
  return GST_PAD_PROBE_OK;
}

static void
recorder_init (Recorder * r)
{
  g_mutex_init (&r->lock);
  r->log = g_ptr_array_new_with_free_func (g_free);
}

static void
recorder_clear (Recorder * r)
{
  g_ptr_array_unref (r->log);
  g_mutex_clear (&r->lock);
}

static void
recorder_attach (Recorder * r, GstElement * mux)
{
  GstPad *src = gst_element_get_static_pad (mux, "src");

  gst_pad_add_probe (src, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, record_probe, r, NULL);
  gst_object_unref (src);
}

/* Index of the first entry equal to @what at or after @from, or -1. */
static gint
rec_find (Recorder * r, const gchar * what, gint from)
{
  gint found = -1;

  g_mutex_lock (&r->lock);
  for (guint i = MAX (from, 0); i < r->log->len && found < 0; i++)
    if (g_strcmp0 (g_ptr_array_index (r->log, i), what) == 0)
      found = (gint) i;
  g_mutex_unlock (&r->lock);
  return found;
}

static gint
rec_count (Recorder * r, const gchar * what)
{
  gint n = 0;

  for (gint i = rec_find (r, what, 0); i >= 0; i = rec_find (r, what, i + 1))
    n++;
  return n;
}

static void
rec_dump (Recorder * r)
{
  g_mutex_lock (&r->lock);
  for (guint i = 0; i < r->log->len; i++)
    g_test_message ("  [%u] %s", i, (const gchar *) g_ptr_array_index (r->log, i));
  g_mutex_unlock (&r->lock);
}

/* Waits (up to 10 s) for @what at or after @from; returns its index (fails the test if it never comes). */
static gint
rec_wait (Recorder * r, const gchar * what, gint from)
{
  gint64 end = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  gint i;

  while ((i = rec_find (r, what, from)) < 0 && g_get_monotonic_time () < end)
    g_usleep (2000);
  if (i < 0) {
    rec_dump (r);
    g_error ("'%s' never left the mux", what);
  }
  return i;
}

static void
rec_assert_before (Recorder * r, const gchar * first, const gchar * second)
{
  gint a = rec_find (r, first, 0), b = rec_find (r, second, 0);

  if (a < 0 || b < 0 || a >= b) {
    rec_dump (r);
    g_error ("expected '%s' before '%s' (%d, %d)", first, second, a, b);
  }
}

static GstHarness *
mux_harness_full (const gchar * props, const gchar * const *in_caps, guint n_in, GstHarness ** in,
    const gchar * out_caps, GstBus * bus, Recorder * rec)
{
  gchar *desc = g_strdup_printf ("rdsstreammux %s", props);
  GError *err = NULL;
  GstElement *e = gst_parse_launch (desc, &err);
  GstHarness *h;

  g_assert_no_error (err);
  gst_object_ref_sink (e);
  if (bus != NULL)
    gst_element_set_bus (e, bus);
  if (rec != NULL)
    recorder_attach (rec, e);
  h = gst_harness_new_with_element (e, NULL, "src");
  gst_harness_set_sink_caps_str (h, out_caps != NULL ? out_caps : OUT_NV12);
  for (guint i = 0; i < n_in; i++) {
    gchar *pad = g_strdup_printf ("sink_%u", i);

    in[i] = gst_harness_new_with_element (e, pad, NULL);
    gst_harness_set_src_caps_str (in[i], in_caps[i]);
    g_free (pad);
  }
  gst_object_unref (e);
  g_free (desc);
  return h;
}

static GstHarness *
mux_harness (const gchar * props, const gchar * const *in_caps, guint n_in, GstHarness ** in)
{
  return mux_harness_full (props, in_caps, n_in, in, NULL, NULL, NULL);
}

static void
mux_teardown (GstHarness * h, GstHarness ** in, guint n_in)
{
  for (guint i = 0; i < n_in; i++)
    gst_harness_teardown (in[i]);
  gst_harness_teardown (h);
}

/* The next output buffer within @timeout_ms, or NULL. */
static GstBuffer *
pull_within (GstHarness * h, guint timeout_ms)
{
  gint64 end = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
  GstBuffer *b;

  while ((b = gst_harness_try_pull (h)) == NULL && g_get_monotonic_time () < end)
    g_usleep (2000);
  return b;
}

/* The next output buffer; fails the test (instead of blocking for a minute) if none comes within 10 s. */
static GstBuffer *
pull_ok (GstHarness * h)
{
  GstBuffer *b = pull_within (h, 10000);

  g_assert_nonnull (b);
  return b;
}

static RdsFrameMeta *
frame_meta_at (RdsBatchMeta * bm, guint slot)
{
  RdsFrameMeta *fm = g_list_nth_data (bm->frame_meta_list, slot);

  g_assert_nonnull (fm);
  return fm;
}

/* ---- gpu ---- */

/* Two RDS sources of different sizes scaled into 640x480 slots: one batch per pair, each slot like the CPU
 * reference, the frame meta filled in, and (sort-batch off) slots in round-robin visiting order. */
static void
test_batch_scaling (void)
{
  const gchar *in_caps[] = { RDS_NV12 (320, 240), RDS_NV12 (640, 360) };
  GstHarness *h, *in[2];
  GstBufferPool *pools[2];
  gint64 before;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=640 height=480", in_caps, 2, in);
  pools[0] = make_pool (in_caps[0]);
  pools[1] = make_pool (in_caps[1]);
  before = g_get_real_time ();

  for (guint round = 0; round < 2; round++) {
    HostImage hin[2];
    GstBuffer *out;
    RdsBufSurface *s;
    RdsBatchMeta *bm;
    GstCaps *caps, *expected;

    for (guint i = 0; i < 2; i++) {
      GstBuffer *b = rds_frame (pools[i], 10 * round + i, (round * 33 + i * 5) * GST_MSECOND);

      host_of (b, NULL, &hin[i]);
      g_assert_cmpint (gst_harness_push (in[i], b), ==, GST_FLOW_OK);
    }
    out = pull_ok (h);
    g_assert_nonnull (out);

    caps = gst_pad_get_current_caps (h->sinkpad);
    expected = gst_caps_from_string ("video/x-raw(memory:RDS),format=NV12,width=640,height=480,framerate=30/1,"
        "pixel-aspect-ratio=1/1");
    g_assert_true (gst_caps_is_equal (caps, expected));
    gst_caps_unref (expected);
    gst_caps_unref (caps);

    s = rds_buffer_get_surface (out);
    g_assert_nonnull (s);
    g_assert_cmpuint (s->batchSize, ==, 2);
    g_assert_cmpuint (s->numFilled, ==, 2);
    /* the batch PTS is the earliest frame's (running time, segments start at 0) */
    g_assert_cmpuint (GST_BUFFER_PTS (out), ==, round * 33 * GST_MSECOND);
    g_assert_false (GST_BUFFER_DURATION_IS_VALID (out));

    bm = gst_buffer_get_rds_batch_meta (out);
    g_assert_nonnull (bm);
    g_assert_cmpuint (bm->max_frames_in_batch, ==, 2);
    g_assert_cmpuint (bm->num_frames_in_batch, ==, 2);
    for (guint slot = 0; slot < 2; slot++) {
      RdsFrameMeta *fm = frame_meta_at (bm, slot);
      /* round robin: the first pad visited rotates each batch */
      guint src = (slot + round) % 2;

      g_assert_cmpuint (fm->source_id, ==, src);
      g_assert_cmpuint (fm->pad_index, ==, src);
      g_assert_cmpuint (fm->batch_id, ==, slot);
      g_assert_cmpint (fm->frame_num, ==, (gint) round);
      g_assert_cmpuint (fm->buf_pts, ==, (round * 33 + src * 5) * GST_MSECOND);
      g_assert_cmpuint (fm->source_frame_width, ==, src == 0 ? 320 : 640);
      g_assert_cmpuint (fm->source_frame_height, ==, src == 0 ? 240 : 360);
      g_assert_cmpuint (fm->pipeline_width, ==, 640);
      g_assert_cmpuint (fm->pipeline_height, ==, 480);
      g_assert_cmpint (fm->num_surfaces_per_frame, ==, 1);
      g_assert_cmpuint (fm->surface_index, ==, 0);
      /* attach-sys-ts (on by default): system time in ns */
      g_assert_cmpuint (fm->ntp_timestamp, >=, (guint64) before * 1000);
      g_assert_cmpuint (fm->ntp_timestamp, <=, (guint64) (g_get_real_time () + 1) * 1000);
      assert_slot (out, slot, &hin[src], (RefRect) { 0, 0, 640, 480 });
    }
    host_image_clear (&hin[0]);
    host_image_clear (&hin[1]);
    gst_buffer_unref (out);
  }

  mux_teardown (h, in, 2);
  for (guint i = 0; i < 2; i++) {
    gst_buffer_pool_set_active (pools[i], FALSE);
    gst_object_unref (pools[i]);
  }
}

/* enable-padding keeps the aspect ratio: 640x360 into 640x480 lands at the top-left, black below; 320x320 into
 * 640x480 is 480x480 at the top-left, black to the right. */
static void
test_padding (void)
{
  const gchar *in_caps[] = { RDS_NV12 (640, 360), RDS_NV12 (320, 320) };
  const RefRect rects[] = { {0, 0, 640, 360}, {0, 0, 480, 480} };
  GstHarness *h, *in[2];
  GstBufferPool *pools[2];
  HostImage hin[2];
  GstBuffer *out;
  RdsBatchMeta *bm;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=640 height=480 enable-padding=true", in_caps, 2, in);
  for (guint i = 0; i < 2; i++) {
    GstBuffer *b;

    pools[i] = make_pool (in_caps[i]);
    b = rds_frame (pools[i], 7 + i, 0);
    host_of (b, NULL, &hin[i]);
    g_assert_cmpint (gst_harness_push (in[i], b), ==, GST_FLOW_OK);
  }
  out = pull_ok (h);
  bm = gst_buffer_get_rds_batch_meta (out);
  for (guint slot = 0; slot < 2; slot++) {
    guint src = frame_meta_at (bm, slot)->source_id;

    assert_slot (out, slot, &hin[src], rects[src]);
  }
  for (guint i = 0; i < 2; i++) {
    host_image_clear (&hin[i]);
    gst_buffer_pool_set_active (pools[i], FALSE);
    gst_object_unref (pools[i]);
  }
  gst_buffer_unref (out);
  mux_teardown (h, in, 2);
}

/* A system-memory I420 source next to a memory:RDS one: uploaded, converted to NV12 and scaled. */
static void
test_system_input (void)
{
  const gchar *in_caps[] = { SYS_I420 (320, 240), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  HostImage hin[2];
  GstBuffer *b, *out;
  RdsBatchMeta *bm;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=320 height=240", in_caps, 2, in);
  pool = make_pool (in_caps[1]);
  for (guint round = 0; round < 2; round++) {
    b = sys_frame (in_caps[0], 30 + round, 0);
    host_of (b, in_caps[0], &hin[0]);
    g_assert_cmpint (gst_harness_push (in[0], b), ==, GST_FLOW_OK);
    b = rds_frame (pool, 40 + round, 0);
    host_of (b, NULL, &hin[1]);
    g_assert_cmpint (gst_harness_push (in[1], b), ==, GST_FLOW_OK);

    out = pull_ok (h);
    g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
    bm = gst_buffer_get_rds_batch_meta (out);
    for (guint slot = 0; slot < 2; slot++) {
      guint src = frame_meta_at (bm, slot)->source_id;

      assert_slot (out, slot, &hin[src], (RefRect) { 0, 0, 320, 240 });
    }
    host_image_clear (&hin[0]);
    host_image_clear (&hin[1]);
    gst_buffer_unref (out);
  }
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
  mux_teardown (h, in, 2);
}

/* buffer-pool-size bounds the output pool: with every batch held downstream, the mux waits for one to come back
 * instead of allocating more. */
static void
test_pool_bounded (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  GstHarness *h, *in[1];
  GstBufferPool *pool, *out_pool;
  GstBuffer *held[2], *out;
  GstStructure *config;
  guint min = 0, max = 0, batch = 0;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("width=64 height=64 buffer-pool-size=2", in_caps, 1, in);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < 2; i++) {
    g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, i, i * GST_MSECOND)), ==, GST_FLOW_OK);
    held[i] = pull_ok (h);
    g_assert_nonnull (held[i]);
  }

  out_pool = gst_aggregator_get_buffer_pool (GST_AGGREGATOR (h->element));
  g_assert_nonnull (out_pool);
  config = gst_buffer_pool_get_config (out_pool);
  g_assert_true (gst_buffer_pool_config_get_params (config, NULL, NULL, &min, &max));
  g_assert_cmpuint (min, ==, 2);
  g_assert_cmpuint (max, ==, 2);
  g_assert_true (rds_buffer_pool_config_get_surface_params (config, &batch, NULL, NULL));
  g_assert_cmpuint (batch, ==, 1);
  gst_structure_free (config);
  gst_object_unref (out_pool);

  /* a third frame: no batch while both are held */
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 2, 2 * GST_MSECOND)), ==, GST_FLOW_OK);
  g_usleep (200 * 1000);
  g_assert_null (gst_harness_try_pull (h));
  gst_buffer_unref (held[0]);
  out = pull_ok (h);
  g_assert_nonnull (out);
  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, 2 * GST_MSECOND);
  gst_buffer_unref (out);
  gst_buffer_unref (held[1]);

  /* stopping while a batch is held, with the mux waiting on the pool, must not hang */
  held[0] = NULL;
  for (guint i = 3; i < 6; i++)
    g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, i, i * GST_MSECOND)), ==, GST_FLOW_OK);
  mux_teardown (h, in, 1);
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

/* ---- gpu: review follow-ups ---- */

static GstBufferPool *
make_pool_batch (const gchar * caps_str, guint batch_size)
{
  GstBufferPool *pool = rds_buffer_pool_new ();
  GstCaps *caps = gst_caps_from_string (caps_str);
  GstStructure *config = gst_buffer_pool_get_config (pool);

  gst_buffer_pool_config_set_params (config, caps, 0, 1, 0);
  rds_buffer_pool_config_set_surface_params (config, batch_size, RDS_BUF_MEM_DEFAULT, 0);
  g_assert_true (gst_buffer_pool_set_config (pool, config));
  g_assert_true (gst_buffer_pool_set_active (pool, TRUE));
  gst_caps_unref (caps);
  return pool;
}

static void
pool_free (GstBufferPool * pool)
{
  gst_buffer_pool_set_active (pool, FALSE);
  gst_object_unref (pool);
}

/* A flush of one source (as on a reconnect) does not stop the mux: later batches still come. */
static void
test_flush_one_pad (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  GstSegment seg;
  GstBuffer *out;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=64 height=64", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, 0)), ==, GST_FLOW_OK);
  out = pull_within (h, 5000);
  g_assert_nonnull (out);
  gst_buffer_unref (out);

  g_assert_true (gst_harness_push_event (in[0], gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_segment (&seg)));

  for (guint round = 1; round < 3; round++) {
    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 10 * round + i, round * 33 * GST_MSECOND)), ==,
          GST_FLOW_OK);
    out = pull_within (h, 5000);
    g_assert_nonnull (out);
    g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
    gst_buffer_unref (out);
  }
  mux_teardown (h, in, 2);
  pool_free (pool);
}

/* A crop meta on a memory:RDS frame selects what is scaled into the slot; the frame meta reports its size. With
 * enable-padding the aspect ratio is the crop's. */
static void
test_crop_meta (void)
{
  const gchar *in_caps[] = { RDS_NV12 (320, 240) };
  static const struct
  {
    const gchar *props;
    RefRect crop, dst;
  } cases[] = {
    {"width=320 height=240", {40, 20, 160, 120}, {0, 0, 320, 240}},
    {"width=320 height=240 enable-padding=true", {32, 100, 160, 60}, {0, 0, 320, 120}},
  };
  GstBufferPool *pool;

  RDS_TEST_REQUIRE_GPU ();
  pool = make_pool (in_caps[0]);
  for (guint c = 0; c < G_N_ELEMENTS (cases); c++) {
    GstHarness *h, *in[1];
    GstBuffer *b, *out;
    GstVideoCropMeta *crop;
    RdsFrameMeta *fm;
    HostImage hin;

    h = mux_harness (cases[c].props, in_caps, 1, in);
    b = rds_frame (pool, 50 + c, 0);
    crop = gst_buffer_add_video_crop_meta (b);
    crop->x = cases[c].crop.left;
    crop->y = cases[c].crop.top;
    crop->width = cases[c].crop.width;
    crop->height = cases[c].crop.height;
    host_of (b, NULL, &hin);
    g_assert_cmpint (gst_harness_push (in[0], b), ==, GST_FLOW_OK);
    out = pull_ok (h);
    fm = frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0);
    g_assert_cmpuint (fm->source_frame_width, ==, cases[c].crop.width);
    g_assert_cmpuint (fm->source_frame_height, ==, cases[c].crop.height);
    assert_slot_rect (out, 0, &hin, cases[c].crop, cases[c].dst);
    host_image_clear (&hin);
    gst_buffer_unref (out);
    mux_teardown (h, in, 1);
  }
  pool_free (pool);
}

typedef struct
{
  GstElement *pipe;
  gboolean done;
  GMutex lock;
  GCond cond;
} StopCtx;

static gpointer
stop_thread (gpointer data)
{
  StopCtx *ctx = data;

  gst_element_set_state (ctx->pipe, GST_STATE_NULL);
  g_mutex_lock (&ctx->lock);
  ctx->done = TRUE;
  g_cond_signal (&ctx->cond);
  g_mutex_unlock (&ctx->lock);
  return NULL;
}

/* Stopping while the mux waits for a pool buffer held downstream (an appsink nobody pulls from) completes. */
static void
test_stop_while_blocked (void)
{
  StopCtx ctx = { 0 };
  GError *err = NULL;
  GThread *t;
  gint64 end;

  RDS_TEST_REQUIRE_GPU ();
  ctx.pipe = gst_parse_launch ("videotestsrc ! video/x-raw,format=NV12,width=64,height=48,framerate=30/1 "
      "! m.sink_0 rdsstreammux name=m width=64 height=48 buffer-pool-size=2 ! appsink max-buffers=0 sync=false",
      &err);
  g_assert_no_error (err);
  g_mutex_init (&ctx.lock);
  g_cond_init (&ctx.cond);
  g_assert_cmpint (gst_element_set_state (ctx.pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_element_get_state (ctx.pipe, NULL, NULL, 10 * GST_SECOND), ==, GST_STATE_CHANGE_SUCCESS);
  g_usleep (300 * 1000);        /* both pool buffers are in the appsink by now */

  t = g_thread_new ("stop", stop_thread, &ctx);
  end = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  g_mutex_lock (&ctx.lock);
  while (!ctx.done)
    if (!g_cond_wait_until (&ctx.cond, &ctx.lock, end))
      g_error ("setting the pipeline to NULL hangs while the mux waits for a pool buffer");
  g_mutex_unlock (&ctx.lock);
  g_thread_join (t);
  gst_object_unref (ctx.pipe);
  g_cond_clear (&ctx.cond);
  g_mutex_clear (&ctx.lock);
}

/* batch-size above the number of sources: the batch carries what there is. */
static void
test_partial_batch (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  GstBuffer *out;
  RdsBufSurface *s;
  RdsBatchMeta *bm;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=3 width=64 height=64", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, 0)), ==, GST_FLOW_OK);
  out = pull_ok (h);
  s = rds_buffer_get_surface (out);
  g_assert_cmpuint (s->batchSize, ==, 3);
  g_assert_cmpuint (s->numFilled, ==, 2);
  bm = gst_buffer_get_rds_batch_meta (out);
  g_assert_cmpuint (bm->max_frames_in_batch, ==, 3);
  g_assert_cmpuint (bm->num_frames_in_batch, ==, 2);
  gst_buffer_unref (out);
  mux_teardown (h, in, 2);
  pool_free (pool);
}

/* Once every source is at EOS, EOS goes downstream. */
static void
test_eos (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  GstBuffer *out;
  gboolean eos = FALSE;
  gint64 end;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=64 height=64", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, 0)), ==, GST_FLOW_OK);
  out = pull_ok (h);
  gst_buffer_unref (out);
  /* one source ends: the other's next frame goes out alone */
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_eos ()));
  g_assert_cmpint (gst_harness_push (in[1], rds_frame (pool, 2, 33 * GST_MSECOND)), ==, GST_FLOW_OK);
  out = pull_within (h, 5000);
  g_assert_nonnull (out);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 1);
  g_assert_cmpuint (frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0)->source_id, ==, 1);
  gst_buffer_unref (out);
  g_assert_true (gst_harness_push_event (in[1], gst_event_new_eos ()));

  end = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
  while (!eos && g_get_monotonic_time () < end) {
    GstEvent *ev = gst_harness_try_pull_event (h);

    if (ev == NULL) {
      g_usleep (2000);
      continue;
    }
    eos = GST_EVENT_TYPE (ev) == GST_EVENT_EOS;
    gst_event_unref (ev);
  }
  g_assert_true (eos);
  mux_teardown (h, in, 2);
  pool_free (pool);
}

/* A system-memory source changing resolution: its staging surface follows. */
static void
test_sys_caps_change (void)
{
  const gchar *sizes[] = { SYS_I420 (320, 240), SYS_I420 (640, 480), SYS_I420 (160, 120) };
  GstHarness *h, *in[1];

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("width=160 height=120", sizes, 1, in);
  for (guint i = 0; i < G_N_ELEMENTS (sizes); i++) {
    GstBuffer *b, *out;
    HostImage hin;

    if (i > 0)
      gst_harness_set_src_caps_str (in[0], sizes[i]);
    b = sys_frame (sizes[i], 60 + i, i * GST_MSECOND);
    host_of (b, sizes[i], &hin);
    g_assert_cmpint (gst_harness_push (in[0], b), ==, GST_FLOW_OK);
    out = pull_ok (h);
    g_assert_nonnull (out);
    g_assert_cmpuint (frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0)->source_frame_width, ==,
        hin.im.width);
    assert_slot (out, 0, &hin, (RefRect) { 0, 0, 160, 120 });
    host_image_clear (&hin);
    gst_buffer_unref (out);
  }
  mux_teardown (h, in, 1);
}

static void
assert_out_framerate (GstHarness * h, gint n, gint d)
{
  GstCaps *caps = gst_pad_get_current_caps (h->sinkpad);
  gint cn = -1, cd = -1;

  g_assert_nonnull (caps);
  g_assert_true (gst_structure_get_fraction (gst_caps_get_structure (caps, 0), "framerate", &cn, &cd));
  g_assert_cmpint (cn, ==, n);
  g_assert_cmpint (cd, ==, d);
  gst_caps_unref (caps);
}

/* Output framerate: the sources' when they agree, else 0/1; renegotiated when a source changes. */
static void
test_framerate (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  const gchar *rate25 = "video/x-raw(memory:RDS),format=NV12,width=160,height=120,framerate=25/1";
  GstHarness *h, *in[2];
  GstBufferPool *pool;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=64 height=64", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  for (guint round = 0; round < 3; round++) {
    GstBuffer *out;

    if (round == 1)
      gst_harness_set_src_caps_str (in[1], rate25);
    if (round == 2)
      gst_harness_set_src_caps_str (in[0], rate25);
    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, round * GST_MSECOND)), ==, GST_FLOW_OK);
    out = pull_ok (h);
    gst_buffer_unref (out);
    if (round == 0)
      assert_out_framerate (h, 30, 1);
    else if (round == 1)
      assert_out_framerate (h, 0, 1);
    else
      assert_out_framerate (h, 25, 1);
  }
  mux_teardown (h, in, 2);
  pool_free (pool);
}

typedef struct
{
  guint n;
  hipStream_t other;
  gboolean ok;
} SessionCheck;

/* Runs on the mux's source thread right after each aggregate: the first time it installs a session of its own,
 * the second time that session must still be there (the mux restored it around its transforms). */
static GstPadProbeReturn
check_session (G_GNUC_UNUSED GstPad * pad, G_GNUC_UNUSED GstPadProbeInfo * info, gpointer user_data)
{
  SessionCheck *c = user_data;
  RdsBufSurfTransformConfigParams cfg = {.compute_mode = RdsBufSurfTransformCompute_GPU,.gpu_id = 0,
    .cuda_stream = c->other
  };
  RdsBufSurfTransformConfigParams cur;

  if (c->n == 0) {
    c->ok = rds_buf_surf_transform_set_session_params (&cfg) == RdsBufSurfTransformError_Success;
  } else if (c->n == 1) {
    c->ok &= rds_buf_surf_transform_get_session_params (&cur) == RdsBufSurfTransformError_Success
        && cur.cuda_stream == c->other && cur.gpu_id == 0 && cur.compute_mode == RdsBufSurfTransformCompute_GPU;
    /* task threads are pooled: leave the thread as we found it */
    (void) rds_buf_surf_transform_set_default_session ();
  }
  c->n++;
  return GST_PAD_PROBE_OK;
}

static void
test_session_restored (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  SessionCheck c = { 0 };
  GstHarness *h, *in[1];
  GstBufferPool *pool;
  GstPad *srcpad;

  RDS_TEST_REQUIRE_GPU ();
  g_assert_cmpint (hipStreamCreate (&c.other), ==, hipSuccess);
  h = mux_harness ("width=64 height=64", in_caps, 1, in);
  srcpad = gst_element_get_static_pad (h->element, "src");
  gst_pad_add_probe (srcpad, GST_PAD_PROBE_TYPE_BUFFER, check_session, &c, NULL);
  gst_object_unref (srcpad);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < 2; i++) {
    g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, i, i * GST_MSECOND)), ==, GST_FLOW_OK);
    gst_buffer_unref (pull_ok (h));
  }
  g_assert_cmpuint (c.n, ==, 2);
  g_assert_true (c.ok);
  mux_teardown (h, in, 1);
  pool_free (pool);
  g_assert_cmpint (hipStreamDestroy (c.other), ==, hipSuccess);
}

/* Batched input (batch size above 1) is an error. */
static void
test_batch_rejected (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  GstBus *bus = gst_bus_new ();
  GstHarness *h, *in[1];
  GstBufferPool *pool;
  GstBuffer *b = NULL;
  RdsBufSurface *s;
  GstMessage *msg;
  GError *err = NULL;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness_full ("width=64 height=64", in_caps, 1, in, NULL, bus, NULL);
  pool = make_pool_batch (in_caps[0], 2);
  g_assert_cmpint (gst_buffer_pool_acquire_buffer (pool, &b, NULL), ==, GST_FLOW_OK);
  s = rds_buffer_get_surface (b);
  s->numFilled = 2;
  gst_harness_push (in[0], b);
  msg = gst_bus_timed_pop_filtered (bus, 5 * GST_SECOND, GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  gst_message_parse_error (msg, &err, NULL);
  g_assert_error (err, GST_STREAM_ERROR, GST_STREAM_ERROR_NOT_IMPLEMENTED);
  g_clear_error (&err);
  gst_message_unref (msg);
  mux_teardown (h, in, 1);
  pool_free (pool);
  /* queued messages ref the element: drop them, or element and bus keep each other alive */
  gst_bus_set_flushing (bus, TRUE);
  gst_object_unref (bus);
}

/* enable-padding with RGBA output (black is 0 in every byte, alpha included; the picture's alpha is 255) and an
 * aspect ratio whose height rounds to an odd value: kept for RGBA, made even for 4:2:0. */
static void
test_padding_rgba_odd (void)
{
  const gchar *in_caps[] = { RDS_NV12 (300, 170) };
  static const struct
  {
    const gchar *out;
    RefRect dst;
  } cases[] = {
    {"video/x-raw(memory:RDS),format=RGBA", {0, 0, 320, 181}},
    {"video/x-raw(memory:RDS),format=NV12", {0, 0, 320, 180}},
  };
  GstBufferPool *pool;

  RDS_TEST_REQUIRE_GPU ();
  pool = make_pool (in_caps[0]);
  for (guint c = 0; c < G_N_ELEMENTS (cases); c++) {
    GstHarness *h, *in[1];
    GstBuffer *b, *out;
    HostImage hin;

    h = mux_harness_full ("width=320 height=240 enable-padding=true", in_caps, 1, in, cases[c].out, NULL, NULL);
    b = rds_frame (pool, 70 + c, 0);
    host_of (b, NULL, &hin);
    g_assert_cmpint (gst_harness_push (in[0], b), ==, GST_FLOW_OK);
    out = pull_ok (h);
    assert_slot (out, 0, &hin, cases[c].dst);
    host_image_clear (&hin);
    gst_buffer_unref (out);
    mux_teardown (h, in, 1);
  }
  pool_free (pool);
}

/* batched-push-timeout: with one source stalled, the other's frame goes out alone one timeout after it arrived. */
static void
test_timeout (void)
{
  const gchar *caps_str = "video/x-raw,format=NV12,width=64,height=48,framerate=30/1";
  GError *err = NULL;
  GstElement *pipe, *src[2], *sink;
  GstSample *sample;
  GstMessage *msg;
  GstBus *bus;
  gint64 t0, dt;

  RDS_TEST_REQUIRE_GPU ();
  pipe = gst_parse_launch ("appsrc name=a format=time ! m.sink_0 appsrc name=b format=time ! m.sink_1 "
      "rdsstreammux name=m batch-size=2 width=64 height=48 batched-push-timeout=40000 "
      "! appsink name=s sync=false", &err);
  g_assert_no_error (err);
  src[0] = gst_bin_get_by_name (GST_BIN (pipe), "a");
  src[1] = gst_bin_get_by_name (GST_BIN (pipe), "b");
  sink = gst_bin_get_by_name (GST_BIN (pipe), "s");
  for (guint i = 0; i < 2; i++) {
    GstCaps *caps = gst_caps_from_string (caps_str);

    g_object_set (src[i], "caps", caps, NULL);
    gst_caps_unref (caps);
  }
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);

  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), sys_frame (caps_str, i, 0)), ==, GST_FLOW_OK);
  sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink), 5 * GST_SECOND);
  g_assert_nonnull (sample);
  g_assert_cmpuint (rds_buffer_get_surface (gst_sample_get_buffer (sample))->numFilled, ==, 2);
  gst_sample_unref (sample);

  /* b stalls: each of a's frames goes out alone, one timeout after it arrived. The pause before the second one
   * would make a stale timer (the first batch's) fire at once. */
  for (guint f = 0; f < 2; f++) {
    if (f > 0)
      g_usleep (200 * 1000);
    t0 = g_get_monotonic_time ();
    g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[0]), sys_frame (caps_str, 2 + f,
                (1 + f) * 33 * GST_MSECOND)), ==, GST_FLOW_OK);
    sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink), 5 * GST_SECOND);
    dt = g_get_monotonic_time () - t0;
    g_assert_nonnull (sample);
    g_assert_cmpuint (rds_buffer_get_surface (gst_sample_get_buffer (sample))->numFilled, ==, 1);
    gst_sample_unref (sample);
    g_test_message ("partial batch %u after %" G_GINT64_FORMAT " ms", f, dt / 1000);
    /* generous upper bound for loaded CI and sanitizers */
    g_assert_cmpint (dt, >=, 35 * 1000);
    g_assert_cmpint (dt, <=, 400 * 1000);
  }

  for (guint i = 0; i < 2; i++)
    gst_app_src_end_of_stream (GST_APP_SRC (src[i]));
  bus = gst_element_get_bus (pipe);
  msg = gst_bus_timed_pop_filtered (bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
  g_assert_nonnull (msg);
  g_assert_cmpint (GST_MESSAGE_TYPE (msg), ==, GST_MESSAGE_EOS);
  gst_message_unref (msg);
  gst_object_unref (bus);
  gst_element_set_state (pipe, GST_STATE_NULL);
  for (guint i = 0; i < 2; i++)
    gst_object_unref (src[i]);
  gst_object_unref (sink);
  gst_object_unref (pipe);
}

/* The crop meta applies to the surface, not the caps size (videocrop in passthrough: cropped caps, full surface). */
static void
test_crop_meta_surface (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  static const RefRect crops[] = { {0, 0, 160, 120}, {160, 120, 160, 120}, {100, 60, 160, 120} };
  GstHarness *h, *in[1];
  GstBufferPool *pool;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("width=160 height=120", in_caps, 1, in);
  pool = make_pool (RDS_NV12 (320, 240));
  for (guint c = 0; c < G_N_ELEMENTS (crops); c++) {
    GstBuffer *b = rds_frame (pool, 80 + c, c * GST_MSECOND), *out;
    GstVideoCropMeta *crop = gst_buffer_add_video_crop_meta (b);
    RdsFrameMeta *fm;
    HostImage hin;

    crop->x = crops[c].left;
    crop->y = crops[c].top;
    crop->width = crops[c].width;
    crop->height = crops[c].height;
    host_of (b, NULL, &hin);
    g_assert_cmpint (gst_harness_push (in[0], b), ==, GST_FLOW_OK);
    out = pull_ok (h);
    fm = frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0);
    g_assert_cmpuint (fm->source_frame_width, ==, 160);
    g_assert_cmpuint (fm->source_frame_height, ==, 120);
    assert_slot_rect (out, 0, &hin, crops[c], (RefRect) { 0, 0, 160, 120 });
    host_image_clear (&hin);
    gst_buffer_unref (out);
  }
  mux_teardown (h, in, 1);
  pool_free (pool);
}

/* Sets up two sources with a one-batch pool, holds the first batch downstream and pushes a second pair, so the mux
 * has popped both frames and waits for a pool buffer. Returns the held batch. */
static GstBuffer *
block_in_acquire (GstHarness * h, GstHarness ** in, GstBufferPool * pool)
{
  GstBuffer *held;

  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, 0)), ==, GST_FLOW_OK);
  held = pull_ok (h);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 2 + i, 33 * GST_MSECOND)), ==, GST_FLOW_OK);
  g_usleep (100 * 1000);
  g_assert_null (gst_harness_try_pull (h));
  return held;
}

static void
push_segment (GstHarness * in)
{
  GstSegment seg;

  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_harness_push_event (in, gst_event_new_segment (&seg)));
}

typedef struct
{
  GstHarness **in;
  gboolean done;
  GMutex lock;
  GCond cond;
} FlushCtx;

/* A seek-style flush: one seqnum, and the first pad finishes START + STOP before the second pad's START. */
static gpointer
flush_thread (gpointer data)
{
  FlushCtx *ctx = data;
  guint32 seqnum = gst_util_seqnum_next ();

  for (guint i = 0; i < 2; i++) {
    GstEvent *start = gst_event_new_flush_start (), *stop = gst_event_new_flush_stop (TRUE);

    gst_event_set_seqnum (start, seqnum);
    gst_event_set_seqnum (stop, seqnum);
    gst_harness_push_event (ctx->in[i], start);
    gst_harness_push_event (ctx->in[i], stop);
  }
  g_mutex_lock (&ctx->lock);
  ctx->done = TRUE;
  g_cond_signal (&ctx->cond);
  g_mutex_unlock (&ctx->lock);
  return NULL;
}

/* An element-wide flush while the mux waits for a pool buffer held downstream: the wait gives up, the flush
 * completes, and batching resumes afterwards. */
static void
test_flush_while_blocked (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  FlushCtx ctx = { 0 };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  GstBuffer *held, *out;
  GThread *t;
  gint64 end;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=64 height=64 buffer-pool-size=1", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  held = block_in_acquire (h, in, pool);

  ctx.in = in;
  g_mutex_init (&ctx.lock);
  g_cond_init (&ctx.cond);
  t = g_thread_new ("flush", flush_thread, &ctx);
  end = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
  g_mutex_lock (&ctx.lock);
  while (!ctx.done)
    if (!g_cond_wait_until (&ctx.cond, &ctx.lock, end))
      g_error ("an element-wide flush hangs while the mux waits for a pool buffer");
  g_mutex_unlock (&ctx.lock);
  g_thread_join (t);
  g_cond_clear (&ctx.cond);
  g_mutex_clear (&ctx.lock);

  gst_buffer_unref (held);
  for (guint i = 0; i < 2; i++) {
    push_segment (in[i]);
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 4 + i, 0)), ==, GST_FLOW_OK);
  }
  out = pull_ok (h);
  /* the flushed batch never comes out: this is the new pair */
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
  g_assert_cmpuint (frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0)->buf_pts, ==, 0);
  gst_buffer_unref (out);
  mux_teardown (h, in, 2);
  pool_free (pool);
}

/* A flush of one source while the mux waits for a pool buffer, holding that source's popped frame: the batch
 * still gets the frame's running time (from the segment it arrived in, not the reset one). */
static void
test_flush_one_pad_blocked (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  GstBuffer *held, *out;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=64 height=64 buffer-pool-size=1", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  held = block_in_acquire (h, in, pool);
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_flush_stop (TRUE)));
  gst_buffer_unref (held);
  out = pull_ok (h);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, 33 * GST_MSECOND);
  gst_buffer_unref (out);

  push_segment (in[0]);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 6 + i, 66 * GST_MSECOND)), ==, GST_FLOW_OK);
  out = pull_ok (h);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
  gst_buffer_unref (out);
  mux_teardown (h, in, 2);
  pool_free (pool);
}

/* Batch timestamps never go backwards, even when a source's frame is older than the last batch. */
static void
test_pts_monotonic (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  static const guint64 in_ms[] = { 100, 50, 150 }, out_ms[] = { 100, 100, 150 };
  GstHarness *h, *in[1];
  GstBufferPool *pool;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("width=64 height=64", in_caps, 1, in);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < G_N_ELEMENTS (in_ms); i++) {
    GstBuffer *out;

    g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, i, in_ms[i] * GST_MSECOND)), ==, GST_FLOW_OK);
    out = pull_ok (h);
    g_assert_cmpuint (GST_BUFFER_PTS (out), ==, out_ms[i] * GST_MSECOND);
    /* the frame keeps its own PTS */
    g_assert_cmpuint (frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0)->buf_pts, ==, in_ms[i] * GST_MSECOND);
    gst_buffer_unref (out);
  }
  mux_teardown (h, in, 1);
  pool_free (pool);
}

/* batched-push-timeout sets the aggregator latency on every start; without a timeout it is 0 again. */
static void
test_latency_on_restart (void)
{
  GstElement *e = gst_element_factory_make ("rdsstreammux", NULL);
  guint64 latency = 1;

  g_object_set (e, "width", 64, "height", 64, "batched-push-timeout", 40000, NULL);
  g_assert_cmpint (gst_element_set_state (e, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  g_object_get (e, "latency", &latency, NULL);
  g_assert_cmpuint (latency, ==, 40 * GST_MSECOND);
  g_assert_cmpint (gst_element_set_state (e, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);

  g_object_set (e, "batched-push-timeout", -1, NULL);
  g_assert_cmpint (gst_element_set_state (e, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  g_object_get (e, "latency", &latency, NULL);
  g_assert_cmpuint (latency, ==, 0);
  g_assert_cmpint (gst_element_set_state (e, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  gst_object_unref (e);
}

/* ---- stream events, EOS, dynamic pads, remaining properties ---- */

/* The stream-id a source sent on sink_@i (the input harness's). */
static gchar *
upstream_stream_id (GstHarness * h, guint i)
{
  gchar *name = g_strdup_printf ("sink_%u", i);
  GstPad *pad = gst_element_get_static_pad (h->element, name);
  GstEvent *ev = gst_pad_get_sticky_event (pad, GST_EVENT_STREAM_START, 0);
  const gchar *sid = NULL;
  gchar *ret;

  g_assert_nonnull (ev);
  gst_event_parse_stream_start (ev, &sid);
  ret = g_strdup (sid);
  gst_event_unref (ev);
  gst_object_unref (pad);
  g_free (name);
  return ret;
}

static void
push_segment_base (GstHarness * in, GstClockTime base)
{
  GstSegment seg;

  gst_segment_init (&seg, GST_FORMAT_TIME);
  seg.base = base;
  g_assert_true (gst_harness_push_event (in, gst_event_new_segment (&seg)));
}

static void
drop_output (GstHarness * h)
{
  GstBuffer *b;

  while ((b = gst_harness_try_pull (h)) != NULL)
    gst_buffer_unref (b);
}

/* Each source's pad-added, stream-start (with the upstream stream-id) and stream-segment (with its segment) go
 * downstream before its first frame; releasing a pad sends pad-deleted. */
static void
test_stream_events (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  Recorder rec;

  RDS_TEST_REQUIRE_GPU ();
  recorder_init (&rec);
  h = mux_harness_full ("batch-size=2 width=64 height=64", in_caps, 2, in, NULL, NULL, &rec);
  pool = make_pool (in_caps[0]);
  push_segment_base (in[1], 5 * GST_SECOND);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, 0)), ==, GST_FLOW_OK);
  rec_wait (&rec, "B:0:1", 0);

  for (guint i = 0; i < 2; i++) {
    gchar *sid = upstream_stream_id (h, i);
    gchar *added = g_strdup_printf ("added:%u", i), *start = g_strdup_printf ("start:%u:%s", i, sid);
    gchar *seg = g_strdup_printf ("seg:%u:0", i);

    g_assert_cmpint (rec_count (&rec, added), ==, 1);
    rec_assert_before (&rec, added, start);
    rec_assert_before (&rec, start, seg);
    rec_assert_before (&rec, seg, "B:0:1");
    g_free (seg);
    g_free (start);
    g_free (added);
    g_free (sid);
  }
  /* the second segment of source 1 follows its first */
  g_assert_cmpint (rec_find (&rec, "seg:1:5000", rec_find (&rec, "seg:1:0", 0)), >=, 0);
  rec_assert_before (&rec, "seg:1:5000", "B:0:1");
  g_assert_cmpint (rec_find (&rec, "deleted:0", 0), <, 0);

  /* releasing the request pads (harness teardown) */
  mux_teardown (h, in, 2);
  g_assert_cmpint (rec_count (&rec, "deleted:0"), ==, 1);
  g_assert_cmpint (rec_count (&rec, "deleted:1"), ==, 1);
  pool_free (pool);
  recorder_clear (&rec);
}

/* A source's EOS becomes stream-eos after its last frame has been batched and before any later batch; the global
 * EOS follows once every source has ended, unless drop-pipeline-eos. Then a new source still gets batched. */
static void
check_stream_eos (gboolean drop)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2], *in2;
  GstBufferPool *pool;
  Recorder rec;
  gint i_eos1;

  recorder_init (&rec);
  h = mux_harness_full (drop ? "batch-size=2 width=64 height=64 drop-pipeline-eos=true" :
      "batch-size=2 width=64 height=64", in_caps, 2, in, NULL, NULL, &rec);
  pool = make_pool (in_caps[0]);
  /* source 1's last frame and its EOS are queued before source 0's frame completes the batch */
  g_assert_cmpint (gst_harness_push (in[1], rds_frame (pool, 1, 0)), ==, GST_FLOW_OK);
  g_assert_true (gst_harness_push_event (in[1], gst_event_new_eos ()));
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 0, 0)), ==, GST_FLOW_OK);
  i_eos1 = rec_wait (&rec, "eos:1", 0);
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 2, 33 * GST_MSECOND)), ==, GST_FLOW_OK);
  rec_wait (&rec, "B:0", i_eos1);
  rec_assert_before (&rec, "B:0:1", "eos:1");
  g_assert_cmpint (rec_count (&rec, "eos:1"), ==, 1);
  g_assert_cmpint (rec_find (&rec, "EOS", 0), <, 0);
  drop_output (h);

  g_assert_true (gst_harness_push_event (in[0], gst_event_new_eos ()));
  rec_wait (&rec, "eos:0", 0);
  if (!drop) {
    rec_wait (&rec, "EOS", 0);
    rec_assert_before (&rec, "eos:0", "EOS");
  } else {
    g_usleep (300 * 1000);
    g_assert_cmpint (rec_find (&rec, "EOS", 0), <, 0);
    /* the mux is still alive: a source added now gets batched */
    in2 = gst_harness_new_with_element (h->element, "sink_2", NULL);
    gst_harness_set_src_caps_str (in2, in_caps[0]);
    g_assert_cmpint (gst_harness_push (in2, rds_frame (pool, 3, 66 * GST_MSECOND)), ==, GST_FLOW_OK);
    rec_wait (&rec, "B:2", 0);
    rec_assert_before (&rec, "added:2", "B:2");
    gst_harness_teardown (in2);
    rec_wait (&rec, "deleted:2", 0);
    g_assert_cmpint (rec_find (&rec, "EOS", 0), <, 0);
  }
  mux_teardown (h, in, 2);
  pool_free (pool);
  recorder_clear (&rec);
}

static void
test_stream_eos (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_stream_eos (FALSE);
}

static void
test_drop_pipeline_eos (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_stream_eos (TRUE);
}

/* A pad requested while PLAYING joins the next batch (after its pad-added); a released one leaves the batches
 * after its pad-deleted. */
static void
test_dynamic_pads (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2], *in2;
  GstBufferPool *pool;
  Recorder rec;
  gint i_del;

  RDS_TEST_REQUIRE_GPU ();
  recorder_init (&rec);
  h = mux_harness_full ("batch-size=3 width=64 height=64 sort-batch=true", in_caps, 2, in, NULL, NULL, &rec);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, 0)), ==, GST_FLOW_OK);
  rec_wait (&rec, "B:0:1", 0);
  g_assert_cmpint (rec_find (&rec, "added:2", 0), <, 0);

  in2 = gst_harness_new_with_element (h->element, "sink_2", NULL);
  gst_harness_set_src_caps_str (in2, in_caps[0]);
  for (guint i = 0; i < 3; i++)
    g_assert_cmpint (gst_harness_push (i < 2 ? in[i] : in2, rds_frame (pool, 10 + i, 33 * GST_MSECOND)), ==,
        GST_FLOW_OK);
  rec_wait (&rec, "B:0:1:2", 0);
  rec_assert_before (&rec, "added:2", "B:0:1:2");
  drop_output (h);

  gst_harness_teardown (in2);
  i_del = rec_wait (&rec, "deleted:2", 0);
  for (guint round = 0; round < 2; round++) {
    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 20 + i, (66 + round * 33) * GST_MSECOND)), ==,
          GST_FLOW_OK);
    i_del = rec_wait (&rec, "B:0:1", i_del + 1);
    drop_output (h);
  }
  g_assert_cmpint (rec_find (&rec, "B:0:1:2", rec_find (&rec, "deleted:2", 0)), <, 0);
  mux_teardown (h, in, 2);
  pool_free (pool);
  recorder_clear (&rec);
}

static gint
pull_frame_num (GstHarness * h)
{
  GstBuffer *out = pull_ok (h);
  gint n = frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0)->frame_num;

  gst_buffer_unref (out);
  return n;
}

/* Per-source frame numbers restart after EOS (frame-num-reset-on-eos) and after a flush
 * (frame-num-reset-on-stream-reset, with rds-stream-reset before the source's next frame); otherwise they go on. */
static void
check_frame_num (gboolean on_eos, gboolean on_reset)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  gchar *props = g_strdup_printf ("width=64 height=64 frame-num-reset-on-eos=%d frame-num-reset-on-stream-reset=%d",
      on_eos, on_reset);
  GstHarness *h, *in[1];
  GstBufferPool *pool;
  Recorder rec;
  gint i_reset;

  recorder_init (&rec);
  h = mux_harness_full (props, in_caps, 1, in, NULL, NULL, &rec);
  pool = make_pool (in_caps[0]);
  for (gint f = 0; f < 2; f++) {
    g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, f, f * 33 * GST_MSECOND)), ==, GST_FLOW_OK);
    g_assert_cmpint (pull_frame_num (h), ==, f);
  }

  /* the source ends and starts again */
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_eos ()));
  rec_wait (&rec, "eos:0", 0);
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_stream_start ("second-stream")));
  push_segment (in[0]);
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 2, 0)), ==, GST_FLOW_OK);
  g_assert_cmpint (pull_frame_num (h), ==, on_eos ? 0 : 2);
  rec_wait (&rec, "start:0:second-stream", 0);

  /* a flush of the source: stream-reset before its next frame */
  g_assert_cmpint (rec_find (&rec, "reset:0", 0), <, 0);
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_flush_start ()));
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_flush_stop (TRUE)));
  push_segment (in[0]);
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 3, 0)), ==, GST_FLOW_OK);
  g_assert_cmpint (pull_frame_num (h), ==, on_reset ? 0 : (on_eos ? 1 : 3));
  i_reset = rec_wait (&rec, "reset:0", 0);
  g_assert_cmpint (rec_find (&rec, "B:0", i_reset), >, i_reset);
  g_assert_cmpint (rec_count (&rec, "reset:0"), ==, 1);

  mux_teardown (h, in, 1);
  pool_free (pool);
  recorder_clear (&rec);
  g_free (props);
}

static void
test_frame_num (void)
{
  RDS_TEST_REQUIRE_GPU ();
  check_frame_num (FALSE, FALSE);
  check_frame_num (TRUE, FALSE);
  check_frame_num (FALSE, TRUE);
}

/* attach-sys-ts=FALSE leaves ntp_timestamp at 0 (TRUE is checked in batch-scaling). */
static void
test_attach_sys_ts_off (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  GstHarness *h, *in[1];
  GstBufferPool *pool;
  GstBuffer *out;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("width=64 height=64 attach-sys-ts=false", in_caps, 1, in);
  pool = make_pool (in_caps[0]);
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 0, 0)), ==, GST_FLOW_OK);
  out = pull_ok (h);
  g_assert_cmpuint (frame_meta_at (gst_buffer_get_rds_batch_meta (out), 0)->ntp_timestamp, ==, 0);
  gst_buffer_unref (out);
  mux_teardown (h, in, 1);
  pool_free (pool);
}

/* live-source: the batch PTS is the last copied frame's; otherwise the earliest frame's. */
static void
test_live_pts (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };

  RDS_TEST_REQUIRE_GPU ();
  for (guint live = 0; live < 2; live++) {
    GstHarness *h, *in[2];
    GstBufferPool *pool;
    GstBuffer *out;

    h = mux_harness (live ? "batch-size=2 width=64 height=64 live-source=true" : "batch-size=2 width=64 height=64",
        in_caps, 2, in);
    pool = make_pool (in_caps[0]);
    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, (50 + 50 * i) * GST_MSECOND)), ==, GST_FLOW_OK);
    out = pull_ok (h);
    /* slots in visiting order: source 0, then source 1 (copied last) */
    g_assert_cmpuint (frame_meta_at (gst_buffer_get_rds_batch_meta (out), 1)->source_id, ==, 1);
    g_assert_cmpuint (GST_BUFFER_PTS (out), ==, (live ? 100 : 50) * GST_MSECOND);
    gst_buffer_unref (out);
    mux_teardown (h, in, 2);
    pool_free (pool);
  }
}

/* sort-batch: slots in pad order whatever pad the round robin visits first. */
static void
test_sort_batch (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=64 height=64 sort-batch=true", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  for (guint round = 0; round < 2; round++) {
    GstBuffer *out;
    RdsBatchMeta *bm;

    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, round * 33 * GST_MSECOND)), ==, GST_FLOW_OK);
    out = pull_ok (h);
    bm = gst_buffer_get_rds_batch_meta (out);
    for (guint slot = 0; slot < 2; slot++) {
      g_assert_cmpuint (frame_meta_at (bm, slot)->source_id, ==, slot);
      g_assert_cmpuint (frame_meta_at (bm, slot)->batch_id, ==, slot);
    }
    gst_buffer_unref (out);
  }
  mux_teardown (h, in, 2);
  pool_free (pool);
}

/* Without a timeout (and not live), a stalled source holds the batch until it ends; then the others go on. */
static void
test_stall_no_timeout (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[3];
  GstBufferPool *pool;
  GstBuffer *out;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=3 width=64 height=64", in_caps, 3, in);
  pool = make_pool (in_caps[0]);
  for (guint i = 0; i < 3; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, 0)), ==, GST_FLOW_OK);
  out = pull_ok (h);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 3);
  gst_buffer_unref (out);

  /* source 2 stalls */
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 3 + i, 33 * GST_MSECOND)), ==, GST_FLOW_OK);
  g_assert_null (pull_within (h, 300));
  g_assert_true (gst_harness_push_event (in[2], gst_event_new_eos ()));
  out = pull_ok (h);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
  gst_buffer_unref (out);
  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 5 + i, 66 * GST_MSECOND)), ==, GST_FLOW_OK);
  out = pull_ok (h);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
  gst_buffer_unref (out);
  mux_teardown (h, in, 3);
  pool_free (pool);
}

/* ---- pipelines with appsrcs (the clock runs) ---- */

#define APP_CAPS "video/x-raw,format=NV12,width=64,height=48,framerate=30/1"

typedef struct
{
  GstElement *pipe, *mux, *sink;
  GstElement *src[4];
  guint n;
} AppPipe;

/* @n appsrcs (each with @src_props) into rdsstreammux (@mux_props) into an appsink. */
static void
app_pipe_start (AppPipe * p, guint n, const gchar * src_props, const gchar * mux_props)
{
  GString *desc = g_string_new (NULL);
  GError *err = NULL;

  for (guint i = 0; i < n; i++)
    g_string_append_printf (desc, "appsrc name=s%u format=time caps=\"" APP_CAPS "\" %s ! m.sink_%u ", i, src_props,
        i);
  g_string_append_printf (desc, "rdsstreammux name=m width=64 height=48 %s ! appsink name=out sync=false", mux_props);
  p->pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  p->n = n;
  for (guint i = 0; i < n; i++) {
    gchar *name = g_strdup_printf ("s%u", i);

    p->src[i] = gst_bin_get_by_name (GST_BIN (p->pipe), name);
    g_free (name);
  }
  p->mux = gst_bin_get_by_name (GST_BIN (p->pipe), "m");
  p->sink = gst_bin_get_by_name (GST_BIN (p->pipe), "out");
  g_assert_cmpint (gst_element_set_state (p->pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
}

static void
app_push (AppPipe * p, guint i, guint32 seed, GstClockTime pts)
{
  g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (p->src[i]), sys_frame (APP_CAPS, seed, pts)), ==,
      GST_FLOW_OK);
}

/* The next batch's number of frames (fails after 5 s); @dt_us gets the wait. */
static guint
app_pull (AppPipe * p, gint64 * dt_us)
{
  gint64 t0 = g_get_monotonic_time ();
  GstSample *sample = gst_app_sink_try_pull_sample (GST_APP_SINK (p->sink), 5 * GST_SECOND);
  guint n;

  if (dt_us != NULL)
    *dt_us = g_get_monotonic_time () - t0;
  g_assert_nonnull (sample);
  n = rds_buffer_get_surface (gst_sample_get_buffer (sample))->numFilled;
  gst_sample_unref (sample);
  return n;
}

static void
app_pipe_stop (AppPipe * p)
{
  gst_element_set_state (p->pipe, GST_STATE_NULL);
  for (guint i = 0; i < p->n; i++)
    gst_object_unref (p->src[i]);
  gst_object_unref (p->mux);
  gst_object_unref (p->sink);
  gst_object_unref (p->pipe);
}

/* batched-push-timeout with three sources, one of which stops after 5 frames: every later batch carries the other
 * two and comes about one timeout after their frames. */
static void
test_stall_timeout (void)
{
  AppPipe p;

  RDS_TEST_REQUIRE_GPU ();
  app_pipe_start (&p, 3, "", "batch-size=3 batched-push-timeout=40000");
  for (guint f = 0; f < 5; f++) {
    for (guint i = 0; i < 3; i++)
      app_push (&p, i, 3 * f + i, f * 33 * GST_MSECOND);
    g_assert_cmpuint (app_pull (&p, NULL), ==, 3);
  }
  for (guint f = 5; f < 9; f++) {
    gint64 dt;

    for (guint i = 0; i < 2; i++)
      app_push (&p, i, 3 * f + i, f * 33 * GST_MSECOND);
    g_assert_cmpuint (app_pull (&p, &dt), ==, 2);
    g_test_message ("batch %u after %" G_GINT64_FORMAT " ms", f, dt / 1000);
    /* generous: loaded CI and sanitizers */
    g_assert_cmpint (dt, >=, 30 * 1000);
    g_assert_cmpint (dt, <=, 1000 * 1000);
  }
  app_pipe_stop (&p);
}

/* With a live upstream, the timeout still counts from the batch's first frame: the upstream latency (here 1 s) is
 * not added on top. (Only once the running time is past that latency: before, there is nothing to take it off.) */
static void
test_timeout_live_upstream (void)
{
  AppPipe p;

  RDS_TEST_REQUIRE_GPU ();
  app_pipe_start (&p, 2, "is-live=true do-timestamp=true min-latency=1000000000",
      "batch-size=2 batched-push-timeout=40000");
  for (guint i = 0; i < 2; i++)
    app_push (&p, i, i, GST_CLOCK_TIME_NONE);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 2);
  g_usleep (1200 * 1000);
  for (guint f = 0; f < 3; f++) {
    gint64 dt;

    g_usleep (100 * 1000);
    app_push (&p, 0, 2 + f, GST_CLOCK_TIME_NONE);
    g_assert_cmpuint (app_pull (&p, &dt), ==, 1);
    g_test_message ("partial batch %u after %" G_GINT64_FORMAT " ms", f, dt / 1000);
    g_assert_cmpint (dt, >=, 30 * 1000);
    g_assert_cmpint (dt, <=, 600 * 1000);
  }
  app_pipe_stop (&p);
}

/* sync-inputs: a partial batch waits until the clock reaches its earliest frame's running time; a frame already
 * due goes out at once, one ahead of the clock waits for it. */
static void
test_sync_inputs (void)
{
  AppPipe p;
  GstClockTime now;
  gint64 dt;

  RDS_TEST_REQUIRE_GPU ();
  app_pipe_start (&p, 2, "", "batch-size=2 sync-inputs=true");
  g_assert_cmpint (gst_element_get_state (p.pipe, NULL, NULL, 5 * GST_SECOND), !=, GST_STATE_CHANGE_FAILURE);
  /* frames due in 300 ms: both are there by then, however slow this machine is */
  now = gst_element_get_current_running_time (p.mux);
  g_assert_true (GST_CLOCK_TIME_IS_VALID (now));
  for (guint i = 0; i < 2; i++)
    app_push (&p, i, i, now + 300 * GST_MSECOND);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 2);

  /* source 1 stalls; source 0's frame is due now */
  now = gst_element_get_current_running_time (p.mux);
  app_push (&p, 0, 2, now);
  g_assert_cmpuint (app_pull (&p, &dt), ==, 1);
  g_test_message ("due frame after %" G_GINT64_FORMAT " ms", dt / 1000);
  g_assert_cmpint (dt, <=, 400 * 1000);

  /* one 600 ms ahead of the clock */
  now = gst_element_get_current_running_time (p.mux);
  app_push (&p, 0, 3, now + 600 * GST_MSECOND);
  g_assert_cmpuint (app_pull (&p, &dt), ==, 1);
  g_test_message ("early frame after %" G_GINT64_FORMAT " ms", dt / 1000);
  g_assert_cmpint (dt, >=, 500 * 1000);
  g_assert_cmpint (dt, <=, 3000 * 1000);
  app_pipe_stop (&p);
}

/* ---- cpu: remaining properties ---- */

/* max-latency is added to the latency the mux reports, on top of the timeout and the upstream latency. */
static void
test_max_latency (void)
{
  AppPipe p;
  GstPad *src;
  GstQuery *q = gst_query_new_latency ();
  GstClockTime min = 0, max = 0;
  gboolean live = FALSE;

  app_pipe_start (&p, 1, "is-live=true min-latency=200000000", "batched-push-timeout=40000 max-latency=100000000");
  g_assert_cmpint (gst_element_get_state (p.pipe, NULL, NULL, 5 * GST_SECOND), !=, GST_STATE_CHANGE_FAILURE);
  src = gst_element_get_static_pad (p.mux, "src");
  g_assert_true (gst_pad_query (src, q));
  gst_query_parse_latency (q, &live, &min, &max);
  g_assert_true (live);
  g_assert_cmpuint (min, ==, 340 * GST_MSECOND);
  gst_query_unref (q);
  gst_object_unref (src);
  app_pipe_stop (&p);
}

static void
check_bool (GstElement * e, const gchar * name, gboolean def)
{
  g_assert_cmpint (G_PARAM_SPEC_BOOLEAN (find_prop (e, name, G_TYPE_PARAM_BOOLEAN))->default_value, ==, def);
}

static void
check_int (GstElement * e, const gchar * name, gint def, gint lo, gint hi)
{
  GParamSpecInt *p = G_PARAM_SPEC_INT (find_prop (e, name, G_TYPE_PARAM_INT));

  g_assert_cmpint (p->default_value, ==, def);
  g_assert_cmpint (p->minimum, ==, lo);
  g_assert_cmpint (p->maximum, ==, hi);
}

static void
test_more_properties (void)
{
  GstElement *e = gst_element_factory_make ("rdsstreammux", NULL);
  GParamSpecUInt64 *p64;
  GError *err = NULL;
  GstElement *bad;

  check_bool (e, "live-source", FALSE);
  check_bool (e, "sync-inputs", FALSE);
  check_bool (e, "sort-batch", FALSE);
  check_bool (e, "attach-sys-ts", TRUE);
  check_bool (e, "drop-pipeline-eos", FALSE);
  check_bool (e, "frame-num-reset-on-eos", FALSE);
  check_bool (e, "frame-num-reset-on-stream-reset", FALSE);
  check_uint (e, "max-latency", 0, 0, G_MAXUINT);
  /* accepted, no effect */
  check_bool (e, "align-first-buffer", FALSE);
  check_bool (e, "align-inputs", FALSE);
  check_bool (e, "cache-buffer", FALSE);
  check_bool (e, "extract-sei-sim-time", FALSE);
  check_bool (e, "extract-sei-type5-data", FALSE);
  check_bool (e, "drop-backward-sei", FALSE);
  check_bool (e, "async-process", FALSE);
  check_int (e, "cache-buffer-timeout", -1, -1, G_MAXINT);
  check_int (e, "sync-inputs-ntp", 0, 0, G_MAXINT);
  check_uint (e, "failsafe-flush-count", 10, 1, 1024);
  p64 = G_PARAM_SPEC_UINT64 (find_prop (e, "frame-duration", G_TYPE_PARAM_UINT64));
  g_assert_cmpuint (p64->default_value, ==, G_MAXUINT64);
  g_assert_cmpuint (p64->maximum, ==, G_MAXUINT64);
  gst_object_unref (e);

  /* surface-array memory (4) is not a value of the enum */
  bad = gst_parse_launch ("rdsstreammux nvbuf-memory-type=4", &err);
  g_assert_nonnull (err);
  g_clear_error (&err);
  gst_clear_object (&bad);
}

static gint noop_logs;

static void
count_noop_log (G_GNUC_UNUSED GstDebugCategory * category, GstDebugLevel level, G_GNUC_UNUSED const gchar * file,
    G_GNUC_UNUSED const gchar * function, G_GNUC_UNUSED gint line, G_GNUC_UNUSED GObject * object,
    GstDebugMessage * message, G_GNUC_UNUSED gpointer user_data)
{
  if (level == GST_LEVEL_INFO && strstr (gst_debug_message_get (message), "no effect") != NULL)
    g_atomic_int_inc (&noop_logs);
}

/* Setting accepted no-op properties logs once per element. */
static void
test_noop_log_once (void)
{
  GstElement *e[2];

  gst_debug_set_threshold_for_name ("rdsstreammux", GST_LEVEL_INFO);
  gst_debug_add_log_function (count_noop_log, NULL, NULL);
  for (guint i = 0; i < 2; i++) {
    e[i] = gst_element_factory_make ("rdsstreammux", NULL);
    g_object_set (e[i], "align-inputs", TRUE, "cache-buffer-timeout", 100, "async-process", TRUE, NULL);
    g_assert_cmpint (g_atomic_int_get (&noop_logs), ==, (gint) i + 1);
  }
  gst_debug_remove_log_function (count_noop_log);
  gst_debug_set_threshold_for_name ("rdsstreammux", GST_LEVEL_NONE);
  for (guint i = 0; i < 2; i++)
    gst_object_unref (e[i]);
}

/* A pad released while the batch holding its last frame waits for a pool buffer: pad-deleted follows that batch. */
static void
test_release_while_blocked (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  GstBuffer *held;
  Recorder rec;
  gint i_last, i_del;

  RDS_TEST_REQUIRE_GPU ();
  recorder_init (&rec);
  h = mux_harness_full ("batch-size=2 width=64 height=64 buffer-pool-size=1", in_caps, 2, in, NULL, NULL, &rec);
  pool = make_pool (in_caps[0]);
  held = block_in_acquire (h, in, pool);
  /* the second pair (round robin: source 1 first) is popped and waits for the held batch */
  gst_harness_teardown (in[1]);
  gst_buffer_unref (held);
  i_last = rec_wait (&rec, "B:1:0", 0);
  i_del = rec_wait (&rec, "deleted:1", 0);
  if (i_del < i_last) {
    rec_dump (&rec);
    g_error ("pad-deleted came before the batch with the pad's last frame");
  }
  drop_output (h);
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 4, 66 * GST_MSECOND)), ==, GST_FLOW_OK);
  rec_wait (&rec, "B:0", i_del);
  mux_teardown (h, in, 1);
  pool_free (pool);
  recorder_clear (&rec);
}

/* Releasing a pad and requesting the same one again at once: pad-deleted before the new pad-added. */
static void
test_rerequest_pad (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool;
  Recorder rec;

  RDS_TEST_REQUIRE_GPU ();
  recorder_init (&rec);
  h = mux_harness_full ("batch-size=2 width=64 height=64 sort-batch=true", in_caps, 2, in, NULL, NULL, &rec);
  pool = make_pool (in_caps[0]);
  for (guint round = 0; round < 5; round++) {
    gint i_del, i_add;

    g_mutex_lock (&rec.lock);
    g_ptr_array_set_size (rec.log, 0);
    g_mutex_unlock (&rec.lock);
    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, i, round * 66 * GST_MSECOND)), ==, GST_FLOW_OK);
    i_add = rec_wait (&rec, "B:0:1", 0);
    drop_output (h);
    gst_harness_teardown (in[1]);
    in[1] = gst_harness_new_with_element (h->element, "sink_1", NULL);
    gst_harness_set_src_caps_str (in[1], in_caps[1]);
    for (guint i = 0; i < 2; i++)
      g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 2 + i, (round * 66 + 33) * GST_MSECOND)), ==,
          GST_FLOW_OK);
    i_add = rec_wait (&rec, "added:1", i_add + 1);
    i_del = rec_wait (&rec, "deleted:1", 0);
    if (i_del > i_add) {
      rec_dump (&rec);
      g_error ("pad-added of the re-requested pad overtook the pad-deleted of the released one");
    }
    rec_wait (&rec, "B:0:1", i_add);
    drop_output (h);
  }
  mux_teardown (h, in, 2);
  pool_free (pool);
  recorder_clear (&rec);
}

/* After READY -> PLAYING every source is announced again; a pad released without ever having been fed sends no
 * pad-deleted (it never had a pad-added). */
static void
test_restart_announce (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120) };
  GstHarness *h, *in[1];
  GstBufferPool *pool;
  GstPad *unfed;
  gchar *deleted;
  Recorder rec;

  RDS_TEST_REQUIRE_GPU ();
  recorder_init (&rec);
  h = mux_harness_full ("width=64 height=64", in_caps, 1, in, NULL, NULL, &rec);
  pool = make_pool (in_caps[0]);
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 0, 0)), ==, GST_FLOW_OK);
  rec_wait (&rec, "B:0", 0);
  drop_output (h);

  unfed = gst_element_request_pad_simple (h->element, "sink_%u");
  g_assert_nonnull (unfed);
  deleted = g_strdup_printf ("deleted:%s", GST_PAD_NAME (unfed) + strlen ("sink_"));
  gst_element_release_request_pad (h->element, unfed);
  gst_object_unref (unfed);
  g_usleep (200 * 1000);
  g_assert_cmpint (rec_find (&rec, deleted, 0), <, 0);
  g_free (deleted);

  g_assert_cmpint (gst_element_set_state (h->element, GST_STATE_READY), ==, GST_STATE_CHANGE_SUCCESS);
  g_assert_cmpint (gst_element_set_state (h->element, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_true (gst_harness_push_event (in[0], gst_event_new_stream_start ("after-restart")));
  gst_harness_set_src_caps_str (in[0], in_caps[0]);
  g_assert_cmpint (gst_harness_push (in[0], rds_frame (pool, 1, 0)), ==, GST_FLOW_OK);
  rec_wait (&rec, "B:0", rec_wait (&rec, "start:0:after-restart", 0));
  g_assert_cmpint (rec_count (&rec, "added:0"), ==, 2);
  drop_output (h);
  mux_teardown (h, in, 1);
  pool_free (pool);
  recorder_clear (&rec);
}

/* The output pool woken by an element-wide flush before the aggregator marks itself flushing (the mux sets the pool
 * flushing before chaining FLUSH_START up). Simulated by setting the pool flushing with no flush at all: the popped
 * batch is dropped, but the sink pads must not be left flushing. */
static void
test_pool_flushing_window (void)
{
  const gchar *in_caps[] = { RDS_NV12 (160, 120), RDS_NV12 (160, 120) };
  GstHarness *h, *in[2];
  GstBufferPool *pool, *out_pool;
  GstBuffer *held, *out;

  RDS_TEST_REQUIRE_GPU ();
  h = mux_harness ("batch-size=2 width=64 height=64 buffer-pool-size=1", in_caps, 2, in);
  pool = make_pool (in_caps[0]);
  held = block_in_acquire (h, in, pool);
  out_pool = gst_aggregator_get_buffer_pool (GST_AGGREGATOR (h->element));
  gst_buffer_pool_set_flushing (out_pool, TRUE);
  g_usleep (100 * 1000);
  gst_buffer_pool_set_flushing (out_pool, FALSE);
  gst_object_unref (out_pool);
  gst_buffer_unref (held);
  g_assert_null (pull_within (h, 100));

  for (guint i = 0; i < 2; i++)
    g_assert_cmpint (gst_harness_push (in[i], rds_frame (pool, 4 + i, 66 * GST_MSECOND)), ==, GST_FLOW_OK);
  out = pull_ok (h);
  g_assert_cmpuint (rds_buffer_get_surface (out)->numFilled, ==, 2);
  g_assert_cmpuint (GST_BUFFER_PTS (out), ==, 66 * GST_MSECOND);
  gst_buffer_unref (out);
  mux_teardown (h, in, 2);
  pool_free (pool);
}

/* The timer of a partial batch starts when its first frame arrived, also for a frame that queued up while the
 * previous batch was waiting: it does not wait a full timeout after that batch. */
static void
test_timer_from_arrival (void)
{
  AppPipe p;
  gint64 t1, t_done;

  RDS_TEST_REQUIRE_GPU ();
  app_pipe_start (&p, 2, "", "batch-size=2 batched-push-timeout=200000");
  for (guint i = 0; i < 2; i++)
    app_push (&p, i, i, 0);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 2);

  /* source 1 stalls; source 0 sends two frames 20 ms apart */
  app_push (&p, 0, 2, 33 * GST_MSECOND);
  g_usleep (20 * 1000);
  t1 = g_get_monotonic_time ();
  app_push (&p, 0, 3, 66 * GST_MSECOND);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 1);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 1);
  t_done = g_get_monotonic_time ();
  g_test_message ("second partial batch %" G_GINT64_FORMAT " ms after its frame", (t_done - t1) / 1000);
  /* one timeout (200 ms) after the frame arrived; waiting for the first batch and then a full timeout would be
   * about 380 ms */
  g_assert_cmpint (t_done - t1, >=, 150 * 1000);
  g_assert_cmpint (t_done - t1, <=, 300 * 1000);
  app_pipe_stop (&p);
}

/* Demuxed views pin their whole batch, and sinks keep their last buffer: four branches each holding a different
 * batch (one frame per batch here, from a different source each time) need a buffer-pool-size above four. With the
 * default (4) this pipeline stalls for good; the size is not raised automatically (it costs memory in pipelines
 * without a demux), so the application sets it. */
static void
test_pool_pinned_by_branches (void)
{
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *src[4], *sink[4];
  GError *err = NULL;

  RDS_TEST_REQUIRE_GPU ();
  for (guint i = 0; i < 4; i++)
    g_string_append_printf (desc, "appsrc name=s%u format=time caps=\"" APP_CAPS "\" ! m.sink_%u "
        "d.src_%u ! appsink name=o%u sync=false async=false enable-last-sample=true ", i, i, i, i);
  g_string_append (desc, "rdsstreammux name=m width=64 height=48 batch-size=4 batched-push-timeout=10000 "
      "buffer-pool-size=6 "
      "! rdsstreamdemux name=d");
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  for (guint i = 0; i < 4; i++) {
    gchar *name = g_strdup_printf ("s%u", i);

    src[i] = gst_bin_get_by_name (GST_BIN (pipe), name);
    g_free (name);
    name = g_strdup_printf ("o%u", i);
    sink[i] = gst_bin_get_by_name (GST_BIN (pipe), name);
    g_free (name);
  }
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  for (guint f = 0; f < 12; f++) {
    guint i = f % 4;
    GstSample *sample;

    g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), sys_frame (APP_CAPS, f, f * 10 * GST_MSECOND)),
        ==, GST_FLOW_OK);
    sample = gst_app_sink_try_pull_sample (GST_APP_SINK (sink[i]), 5 * GST_SECOND);
    if (sample == NULL)
      g_error ("frame %u never came out: the output pool is held by the sinks' last buffers", f);
    gst_sample_unref (sample);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);
  for (guint i = 0; i < 4; i++) {
    gst_object_unref (src[i]);
    gst_object_unref (sink[i]);
  }
  gst_object_unref (pipe);
}

/* A GAP event before a frame on the same source: the empty GAP buffer the aggregator queues has no arrival record
 * and is no frame. The frame's batch still goes out one timeout after the frame arrived. */
static void
test_timer_gap_then_frame (void)
{
  AppPipe p;
  GstPad *sink0;
  gint64 t_frame, t_done;

  RDS_TEST_REQUIRE_GPU ();
  app_pipe_start (&p, 2, "", "batch-size=2 batched-push-timeout=300000");
  for (guint i = 0; i < 2; i++)
    app_push (&p, i, i, 0);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 2);

  /* source 1 stalls; source 0 sends a GAP, then 50 ms later a frame */
  sink0 = gst_element_get_static_pad (p.mux, "sink_0");
  g_assert_true (gst_pad_send_event (sink0, gst_event_new_gap (33 * GST_MSECOND, 33 * GST_MSECOND)));
  g_usleep (50 * 1000);
  t_frame = g_get_monotonic_time ();
  app_push (&p, 0, 2, 66 * GST_MSECOND);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 1);
  t_done = g_get_monotonic_time ();
  g_test_message ("frame after a GAP went out %" G_GINT64_FORMAT " ms after it arrived", (t_done - t_frame) / 1000);
  /* one timeout (300 ms) after the frame; timing it from when the GAP left the queue would be about 550 ms */
  g_assert_cmpint (t_done - t_frame, >=, 250 * 1000);
  g_assert_cmpint (t_done - t_frame, <=, 450 * 1000);
  gst_object_unref (sink0);
  app_pipe_stop (&p);
}

/* A flush of a source drops its queued frame, and the timer of that frame with it: a frame sent after the flush
 * (later than one timeout) still waits a timeout for the stalled source. */
static void
test_timer_after_flush (void)
{
  AppPipe p;
  GstPad *sink0;
  GstSegment seg;
  gint64 dt;

  RDS_TEST_REQUIRE_GPU ();
  app_pipe_start (&p, 2, "", "batch-size=2 batched-push-timeout=200000");
  for (guint i = 0; i < 2; i++)
    app_push (&p, i, i, 0);
  g_assert_cmpuint (app_pull (&p, NULL), ==, 2);

  /* source 1 stalls; source 0's queued frame is flushed right away */
  sink0 = gst_element_get_static_pad (p.mux, "sink_0");
  app_push (&p, 0, 2, 33 * GST_MSECOND);
  g_usleep (5 * 1000);
  g_assert_true (gst_pad_send_event (sink0, gst_event_new_flush_start ()));
  g_assert_true (gst_pad_send_event (sink0, gst_event_new_flush_stop (TRUE)));
  gst_segment_init (&seg, GST_FORMAT_TIME);
  g_assert_true (gst_pad_send_event (sink0, gst_event_new_segment (&seg)));
  g_usleep (300 * 1000);
  app_push (&p, 0, 3, 0);
  g_assert_cmpuint (app_pull (&p, &dt), ==, 1);
  g_test_message ("frame after the flush went out after %" G_GINT64_FORMAT " ms", dt / 1000);
  g_assert_cmpint (dt, >=, 150 * 1000);
  g_assert_cmpint (dt, <=, 1000 * 1000);
  gst_object_unref (sink0);
  app_pipe_stop (&p);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/mux/aggregator-probe", test_aggregator_probe);
  g_test_add_func ("/cpu/mux/factory", test_factory);
  g_test_add_func ("/cpu/mux/properties", test_properties);
  g_test_add_func ("/cpu/mux/no-size", test_no_size);
  g_test_add_func ("/cpu/mux/unsupported-settings", test_unsupported_settings);
  g_test_add_func ("/cpu/mux/latency-on-restart", test_latency_on_restart);
  g_test_add_func ("/cpu/mux/max-latency", test_max_latency);
  g_test_add_func ("/cpu/mux/more-properties", test_more_properties);
  g_test_add_func ("/cpu/mux/noop-log-once", test_noop_log_once);

  g_test_add_func ("/gpu/mux/batch-scaling", test_batch_scaling);
  g_test_add_func ("/gpu/mux/padding", test_padding);
  g_test_add_func ("/gpu/mux/system-input", test_system_input);
  g_test_add_func ("/gpu/mux/pool-bounded", test_pool_bounded);
  g_test_add_func ("/gpu/mux/flush-one-pad", test_flush_one_pad);
  g_test_add_func ("/gpu/mux/crop-meta", test_crop_meta);
  g_test_add_func ("/gpu/mux/stop-while-blocked", test_stop_while_blocked);
  g_test_add_func ("/gpu/mux/partial-batch", test_partial_batch);
  g_test_add_func ("/gpu/mux/eos", test_eos);
  g_test_add_func ("/gpu/mux/sys-caps-change", test_sys_caps_change);
  g_test_add_func ("/gpu/mux/framerate", test_framerate);
  g_test_add_func ("/gpu/mux/session-restored", test_session_restored);
  g_test_add_func ("/gpu/mux/batch-rejected", test_batch_rejected);
  g_test_add_func ("/gpu/mux/padding-rgba-odd", test_padding_rgba_odd);
  g_test_add_func ("/gpu/mux/timeout", test_timeout);
  g_test_add_func ("/gpu/mux/crop-meta-surface", test_crop_meta_surface);
  g_test_add_func ("/gpu/mux/flush-while-blocked", test_flush_while_blocked);
  g_test_add_func ("/gpu/mux/flush-one-pad-blocked", test_flush_one_pad_blocked);
  g_test_add_func ("/gpu/mux/pts-monotonic", test_pts_monotonic);
  g_test_add_func ("/gpu/mux/stream-events", test_stream_events);
  g_test_add_func ("/gpu/mux/stream-eos", test_stream_eos);
  g_test_add_func ("/gpu/mux/drop-pipeline-eos", test_drop_pipeline_eos);
  g_test_add_func ("/gpu/mux/dynamic-pads", test_dynamic_pads);
  g_test_add_func ("/gpu/mux/frame-num", test_frame_num);
  g_test_add_func ("/gpu/mux/attach-sys-ts-off", test_attach_sys_ts_off);
  g_test_add_func ("/gpu/mux/live-pts", test_live_pts);
  g_test_add_func ("/gpu/mux/sort-batch", test_sort_batch);
  g_test_add_func ("/gpu/mux/stall-no-timeout", test_stall_no_timeout);
  g_test_add_func ("/gpu/mux/stall-timeout", test_stall_timeout);
  g_test_add_func ("/gpu/mux/timeout-live-upstream", test_timeout_live_upstream);
  g_test_add_func ("/gpu/mux/sync-inputs", test_sync_inputs);
  g_test_add_func ("/gpu/mux/release-while-blocked", test_release_while_blocked);
  g_test_add_func ("/gpu/mux/rerequest-pad", test_rerequest_pad);
  g_test_add_func ("/gpu/mux/restart-announce", test_restart_announce);
  g_test_add_func ("/gpu/mux/pool-flushing-window", test_pool_flushing_window);
  g_test_add_func ("/gpu/mux/timer-from-arrival", test_timer_from_arrival);
  g_test_add_func ("/gpu/mux/pool-pinned-by-branches", test_pool_pinned_by_branches);
  g_test_add_func ("/gpu/mux/timer-gap-then-frame", test_timer_gap_then_frame);
  g_test_add_func ("/gpu/mux/timer-after-flush", test_timer_after_flush);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_deinit ();
    return ret;
  }
}
