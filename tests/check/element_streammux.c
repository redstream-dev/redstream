/* rdsstreammux: aggregator timing probe, factory, properties, caps (cpu) and memory:RDS batching (gpu, skipped
 * without a ROCm device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-surface.h"

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
      g_assert_cmpint (c->t - prev, <=, 150 * 1000);
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

/* Slot @slot of the batch must match the reference transform of the whole input into rect @d of a black slot
 * (YUV black: Y 16, U/V 128), exactly black outside @d. */
static void
assert_slot (GstBuffer * batch, guint slot, const HostImage * hin, RefRect d)
{
  RdsBufSurface *s = rds_buffer_get_surface (batch);
  RefRect src = { 0, 0, hin->im.width, hin->im.height };
  HostImage hout, href;

  g_assert_nonnull (s);
  snapshot (s, slot, &hout);
  href = hout;
  for (guint p = 0; p < hout.num_planes; p++) {
    gsize size = (gsize) hout.im.pitch[p] * hout.rows[p];

    href.im.plane[p] = g_malloc (size);
    memset (href.im.plane[p], p == 0 ? 16 : 128, size);
  }
  ref_transform (&hin->im, &href.im, src, d, FALSE);
  if (d.width == hout.im.width && d.height == hout.im.height) {
    g_assert_cmpuint (max_diff (&hout, &href, TRUE), <=, 1);
  } else {
    /* inside the rect within 1 (the GPU rounds), outside exactly black */
    for (guint p = 0; p < hout.num_planes; p++) {
      guint sub = p > 0 ? 2 : 1;
      for (guint y = 0; y < hout.rows[p]; y++) {
        for (guint x = 0; x < hout.row_bytes[p]; x++) {
          guint px = (hout.num_planes == 2 && p == 1 ? x / 2 : x) * sub, py = y * sub;
          gsize at = (gsize) y * hout.im.pitch[p] + x;
          gboolean inside = px >= d.left && px < d.left + d.width && py >= d.top && py < d.top + d.height;
          guint diff = (guint) ABS ((gint) hout.im.plane[p][at] - (gint) href.im.plane[p][at]);

          if (inside ? diff > 1 : diff != 0)
            g_error ("slot %u plane %u row %u byte %u (%s): %u vs %u", slot, p, y, x, inside ? "inside" : "outside",
                hout.im.plane[p][at], href.im.plane[p][at]);
        }
      }
    }
  }
  host_image_clear (&href);
  host_image_clear (&hout);
}

/* A mux with @n_in input harnesses on sink_0.., each with fixed caps, and its output harness. */
static GstHarness *
mux_harness (const gchar * props, const gchar * const *in_caps, guint n_in, GstHarness ** in)
{
  gchar *desc = g_strdup_printf ("rdsstreammux %s", props);
  GError *err = NULL;
  GstElement *e = gst_parse_launch (desc, &err);
  GstHarness *h;

  g_assert_no_error (err);
  gst_object_ref_sink (e);
  h = gst_harness_new_with_element (e, NULL, "src");
  gst_harness_set_sink_caps_str (h, OUT_NV12);
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

static void
mux_teardown (GstHarness * h, GstHarness ** in, guint n_in)
{
  for (guint i = 0; i < n_in; i++)
    gst_harness_teardown (in[i]);
  gst_harness_teardown (h);
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
    out = gst_harness_pull (h);
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
  out = gst_harness_pull (h);
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

    out = gst_harness_pull (h);
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
    held[i] = gst_harness_pull (h);
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
  out = gst_harness_pull (h);
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

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/mux/factory", test_factory);
  g_test_add_func ("/cpu/mux/properties", test_properties);
  g_test_add_func ("/cpu/mux/no-size", test_no_size);
  g_test_add_func ("/cpu/mux/unsupported-settings", test_unsupported_settings);
  g_test_add_func ("/gpu/mux/aggregator-probe", test_aggregator_probe);
  g_test_add_func ("/gpu/mux/batch-scaling", test_batch_scaling);
  g_test_add_func ("/gpu/mux/padding", test_padding);
  g_test_add_func ("/gpu/mux/system-input", test_system_input);
  g_test_add_func ("/gpu/mux/pool-bounded", test_pool_bounded);
  return g_test_run ();
}
