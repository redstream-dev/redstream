/* Redstream: rdslogger, a passthrough element that counts each stream's frames and prints their frame rates to
 * stdout, in DeepStream 8.0's nvdslogger line format:
 *
 *   **PERF : FPS_<source_id> (<fps, 2 decimals>)\t... one entry per stream, ascending ids, then a newline
 *
 * Property names, types and defaults follow nvdslogger as gst-inspect shows it for DeepStream 8.0 (identity's
 * properties plus fps-measurement-interval-sec). Behaviour the docs leave open matches DeepStream 8.0 ("DeepStream
 * 8.0 behaviour") except where marked "ours"; every such choice has a row in docs/compat-matrix.md.
 *
 * Timing: a periodic clock id on the pipeline clock fires at every interval boundary of running time (one interval
 * after PLAYING starts, then every interval). The first boundary only starts the count, so the first line comes two
 * intervals in, as in DeepStream 8.0; each later line gives the frames counted since the previous boundary divided by
 * the interval (ours: DeepStream 8.0's numbers drift after streams join or leave). Paused, the clock id is
 * unscheduled and the time left to the next boundary is kept, so the boundaries resume in running time; a flush that
 * resets the running time starts the measurement again. Lines are written with g_print, from the clock's thread.
 * A new clock is only handed out on the way to PLAYING (PAUSED to PLAYING arms the id on the current clock), so
 * there is no set_clock override.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdslogger.h"

#include <rds/rds.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_logger_debug);
#define GST_CAT_DEFAULT gst_rds_logger_debug

/* defaults as gst-inspect shows them for DeepStream 8.0 */
#define DEFAULT_FPS_INTERVAL 5
#define DEFAULT_SILENT TRUE
#define DEFAULT_SIGNAL_HANDOFFS TRUE

enum
{
  PROP_0,
  PROP_FPS_INTERVAL,
  PROP_SILENT,
  PROP_SIGNAL_HANDOFFS,
  PROP_LAST_MESSAGE,
  PROP_NOOP_FIRST,
};

/* identity's other properties, which nvdslogger also has: accepted and stored; a value that would do something logs
 * one warning per property (ours) */
enum
{
  NOOP_CHECK_IMPERFECT_OFFSET,
  NOOP_CHECK_IMPERFECT_TIMESTAMP,
  NOOP_DATARATE,
  NOOP_DROP_ALLOCATION,
  NOOP_DROP_BUFFER_FLAGS,
  NOOP_DROP_PROBABILITY,
  NOOP_DUMP,
  NOOP_ERROR_AFTER,
  NOOP_LATENCY,
  NOOP_SINGLE_SEGMENT,
  NOOP_SLEEP_TIME,
  NOOP_SYNC,
  NOOP_TS_OFFSET,
  N_NOOPS,
};

static GParamSpec *noop_specs[N_NOOPS];

enum
{
  SIGNAL_HANDOFF,
  N_SIGNALS,
};

static guint signals[N_SIGNALS];

typedef struct
{
  guint id;
  guint64 frames;               /* in the current interval */
} Stream;

struct _GstRdsLogger
{
  GstBaseTransform parent;

  /* properties (object lock) */
  gboolean silent;
  gboolean signal_handoffs;
  GValue noops[N_NOOPS];
  guint noops_warned;           /* bit per NOOP_* */

  /* the measurement (lock) */
  GMutex lock;
  guint interval;               /* seconds; 0: off */
  gboolean playing;
  GstClockID clock_id;          /* periodic, one per interval boundary; NULL when not measuring */
  GstClockTime next;            /* clock time of the next boundary, while clock_id is armed */
  GstClockTime remaining;       /* time left to the next boundary while disarmed; NONE: one interval */
  gboolean clock_failed;        /* waiting on the clock failed: not retried until the measurement restarts */
  guint ticks;                  /* boundaries since the measurement started */
  GArray *streams;              /* Stream, ascending ids; every stream seen since start, ended ones too */
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

G_DEFINE_TYPE (GstRdsLogger, gst_rds_logger, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdslogger, "rdslogger", GST_RANK_NONE, GST_TYPE_RDS_LOGGER);

/* ---- the measurement ---- */

static void
reset_counts_locked (GstRdsLogger * self)
{
  for (guint i = 0; i < self->streams->len; i++)
    g_array_index (self->streams, Stream, i).frames = 0;
}

static void
count_frame_locked (GstRdsLogger * self, guint id)
{
  guint i = 0;
  Stream s = { id, 0 };

  while (i < self->streams->len && g_array_index (self->streams, Stream, i).id < id)
    i++;
  if (i == self->streams->len || g_array_index (self->streams, Stream, i).id != id)
    g_array_insert_val (self->streams, i, s);
  g_array_index (self->streams, Stream, i).frames++;
}

/* The **PERF line for the interval just ended, or NULL before any stream was seen (ours: nothing printed). */
static gchar *
perf_line_locked (GstRdsLogger * self)
{
  GString *line;

  if (self->streams->len == 0)
    return NULL;
  line = g_string_new ("**PERF : ");
  for (guint i = 0; i < self->streams->len; i++) {
    const Stream *s = &g_array_index (self->streams, Stream, i);
    gchar fps[G_ASCII_DTOSTR_BUF_SIZE];

    /* '.' whatever the locale */
    g_ascii_formatd (fps, sizeof (fps), "%.2f", (gdouble) s->frames / self->interval);
    g_string_append_printf (line, "FPS_%u (%s)\t", s->id, fps);
  }
  g_string_append_c (line, '\n');
  return g_string_free (line, FALSE);
}

static gboolean
on_boundary (G_GNUC_UNUSED GstClock * clock, G_GNUC_UNUSED GstClockTime time, GstClockID id, gpointer user_data)
{
  GstRdsLogger *self = GST_RDS_LOGGER (user_data);
  gchar *line = NULL;

  g_mutex_lock (&self->lock);
  if (id != self->clock_id) {
    /* unscheduled meanwhile */
    g_mutex_unlock (&self->lock);
    return TRUE;
  }
  self->next += (GstClockTime) self->interval * GST_SECOND;
  /* the first boundary only starts the count (DeepStream 8.0 behaviour: the first line ≈2 intervals in) */
  if (self->ticks++ > 0)
    line = perf_line_locked (self);
  reset_counts_locked (self);
  g_mutex_unlock (&self->lock);

  if (line != NULL) {
    g_print ("%s", line);
    g_free (line);
  }
  return TRUE;
}

/* Arms the periodic clock id when playing, with a clock and a non-zero interval. */
static void
schedule_locked (GstRdsLogger * self)
{
  GstClockTime interval_ns = (GstClockTime) self->interval * GST_SECOND;
  GstClock *clock;

  if (!self->playing || self->interval == 0 || self->clock_id != NULL || self->clock_failed)
    return;
  clock = gst_element_get_clock (GST_ELEMENT (self));
  if (clock == NULL)
    return;
  self->next = gst_clock_get_time (clock) +
      (GST_CLOCK_TIME_IS_VALID (self->remaining) ? self->remaining : interval_ns);
  self->remaining = GST_CLOCK_TIME_NONE;
  GST_DEBUG_OBJECT (self, "next boundary at clock time %" GST_TIME_FORMAT ", every %u s",
      GST_TIME_ARGS (self->next), self->interval);
  self->clock_id = gst_clock_new_periodic_id (clock, self->next, interval_ns);
  if (gst_clock_id_wait_async (self->clock_id, on_boundary, gst_object_ref (self), gst_object_unref)
      != GST_CLOCK_OK) {
    GST_WARNING_OBJECT (self, "cannot wait on the clock: no frame rates printed");
    gst_clock_id_unref (self->clock_id);
    self->clock_id = NULL;
    self->clock_failed = TRUE;
  }
  gst_object_unref (clock);
}

/* Disarms the clock id, keeping the time left to the next boundary (paused, no running time passes). */
static void
unschedule_locked (GstRdsLogger * self)
{
  GstClock *clock;

  if (self->clock_id == NULL)
    return;
  clock = gst_clock_id_get_clock (self->clock_id);
  gst_clock_id_unschedule (self->clock_id);
  if (clock != NULL) {
    GstClockTime now = gst_clock_get_time (clock);

    self->remaining = self->next > now ? self->next - now : 0;
    gst_object_unref (clock);
  }
  gst_clock_id_unref (self->clock_id);
  self->clock_id = NULL;
}

/* Starts the measurement again: the next boundary one interval from now, and it only starts the count. */
static void
restart_locked (GstRdsLogger * self)
{
  unschedule_locked (self);
  self->ticks = 0;
  self->remaining = GST_CLOCK_TIME_NONE;
  self->clock_failed = FALSE;
  reset_counts_locked (self);
  schedule_locked (self);
}

/* ---- streaming ---- */

/* silent=false: DeepStream 8.0's two lines per buffer (the "rdslogger base time" label is nvdslogger's, renamed) */
static void
print_buffer_lines (GstRdsLogger * self, GstBuffer * buf)
{
  GstBaseTransform *trans = GST_BASE_TRANSFORM (self);
  GstClock *clock = gst_element_get_clock (GST_ELEMENT (self));
  GstClockTime base = gst_element_get_base_time (GST_ELEMENT (self));
  GstClockTime now = GST_CLOCK_TIME_NONE, pts = GST_BUFFER_PTS (buf), pts_rt = GST_CLOCK_TIME_NONE;
  gchar diff[G_ASCII_DTOSTR_BUF_SIZE];
  const gchar *verdict = NULL;
  gdouble diff_ms = 0;

  if (clock != NULL) {
    GstClockTime t = gst_clock_get_time (clock);

    now = t > base ? t - base : 0;
    gst_object_unref (clock);
  }
  if (GST_CLOCK_TIME_IS_VALID (pts) && trans->segment.format == GST_FORMAT_TIME)
    pts_rt = gst_segment_to_running_time (&trans->segment, GST_FORMAT_TIME, pts);
  if (GST_CLOCK_TIME_IS_VALID (now) && GST_CLOCK_TIME_IS_VALID (pts_rt)) {
    diff_ms = (gdouble) GST_CLOCK_DIFF (pts_rt, now) / GST_MSECOND;
    verdict = now > pts_rt ? "BUFFER PTS is lesser than ELEMENT TIME, IT's LATE\n" :
        "BUFFER PTS is greater than ELEMENT TIME, IT's EARLY\n";
  }
  g_ascii_formatd (diff, sizeof (diff), "%.2f", diff_ms);
  g_print ("%s BUF_PTS : %" GST_TIME_FORMAT " element time %" GST_TIME_FORMAT " elem-buf=%s ms buf=%p "
      "buffer_pts_to_running_time = %" GST_TIME_FORMAT "rdslogger base time = %" GST_TIME_FORMAT
      "buffer dur %" GST_TIME_FORMAT "\n%s", GST_OBJECT_NAME (self), GST_TIME_ARGS (pts), GST_TIME_ARGS (now), diff,
      (gpointer) buf, GST_TIME_ARGS (pts_rt), GST_TIME_ARGS (base), GST_TIME_ARGS (GST_BUFFER_DURATION (buf)),
      verdict != NULL ? verdict : "");
}

static GstFlowReturn
gst_rds_logger_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRdsLogger *self = GST_RDS_LOGGER (trans);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  gboolean silent, handoffs;

  g_mutex_lock (&self->lock);
  if (bm != NULL) {
    /* one frame per frame meta, by source_id (DeepStream 8.0 behaviour) */
    rds_acquire_meta_lock (bm);
    for (GList * l = bm->frame_meta_list; l != NULL; l = l->next)
      count_frame_locked (self, ((RdsFrameMeta *) l->data)->source_id);
    rds_release_meta_lock (bm);
  } else {
    /* no batch meta: one frame of stream 0 (ours) */
    count_frame_locked (self, 0);
  }
  /* the clock may have come after PLAYING */
  schedule_locked (self);
  g_mutex_unlock (&self->lock);

  GST_OBJECT_LOCK (self);
  silent = self->silent;
  handoffs = self->signal_handoffs;
  GST_OBJECT_UNLOCK (self);
  if (!silent)
    print_buffer_lines (self, buf);
  if (handoffs)
    g_signal_emit (self, signals[SIGNAL_HANDOFF], 0, buf);
  return GST_FLOW_OK;
}

static gboolean
gst_rds_logger_sink_event (GstBaseTransform * trans, GstEvent * event)
{
  GstRdsLogger *self = GST_RDS_LOGGER (trans);
  gboolean reset_time = FALSE;

  if (GST_EVENT_TYPE (event) == GST_EVENT_FLUSH_STOP) {
    gst_event_parse_flush_stop (event, &reset_time);
    if (reset_time) {
      /* the running time starts again: so does the measurement (streams stay listed) */
      g_mutex_lock (&self->lock);
      restart_locked (self);
      g_mutex_unlock (&self->lock);
    }
  }
  return GST_BASE_TRANSFORM_CLASS (gst_rds_logger_parent_class)->sink_event (trans, event);
}

static GstStateChangeReturn
gst_rds_logger_change_state (GstElement * element, GstStateChange transition)
{
  GstRdsLogger *self = GST_RDS_LOGGER (element);

  g_mutex_lock (&self->lock);
  switch (transition) {
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* streams are forgotten only here: an ended stream stays listed with 0.00 (DeepStream 8.0 behaviour) */
      g_array_set_size (self->streams, 0);
      self->ticks = 0;
      self->remaining = GST_CLOCK_TIME_NONE;
      self->clock_failed = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      self->playing = TRUE;
      schedule_locked (self);
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      self->playing = FALSE;
      unschedule_locked (self);
      break;
    default:
      break;
  }
  g_mutex_unlock (&self->lock);

  return GST_ELEMENT_CLASS (gst_rds_logger_parent_class)->change_state (element, transition);
}

/* ---- properties ---- */

static void
gst_rds_logger_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsLogger *self = GST_RDS_LOGGER (object);
  gboolean warn = FALSE;

  switch (prop_id) {
    case PROP_FPS_INTERVAL:
      g_mutex_lock (&self->lock);
      self->interval = g_value_get_uint (value);
      /* a new interval while playing restarts the measurement (ours) */
      restart_locked (self);
      g_mutex_unlock (&self->lock);
      return;
    default:
      break;
  }

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_SILENT:
      self->silent = g_value_get_boolean (value);
      break;
    case PROP_SIGNAL_HANDOFFS:
      self->signal_handoffs = g_value_get_boolean (value);
      break;
    default:
      if (prop_id >= PROP_NOOP_FIRST && prop_id < PROP_NOOP_FIRST + N_NOOPS) {
        guint i = prop_id - PROP_NOOP_FIRST;

        g_value_copy (value, &self->noops[i]);
        if (!g_param_value_defaults (pspec, value) && !(self->noops_warned & (1u << i))) {
          self->noops_warned |= 1u << i;
          warn = TRUE;
        }
      } else {
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      }
      break;
  }
  GST_OBJECT_UNLOCK (self);

  if (warn)
    g_warning ("%s: %s is not implemented; the property is accepted and has no effect", GST_OBJECT_NAME (self),
        pspec->name);
}

static void
gst_rds_logger_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsLogger *self = GST_RDS_LOGGER (object);

  if (prop_id == PROP_FPS_INTERVAL) {
    g_mutex_lock (&self->lock);
    g_value_set_uint (value, self->interval);
    g_mutex_unlock (&self->lock);
    return;
  }

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean (value, self->silent);
      break;
    case PROP_SIGNAL_HANDOFFS:
      g_value_set_boolean (value, self->signal_handoffs);
      break;
    case PROP_LAST_MESSAGE:
      /* ours: never set (silent=false prints to stdout instead) */
      g_value_set_string (value, NULL);
      break;
    default:
      if (prop_id >= PROP_NOOP_FIRST && prop_id < PROP_NOOP_FIRST + N_NOOPS)
        g_value_copy (&self->noops[prop_id - PROP_NOOP_FIRST], value);
      else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_logger_finalize (GObject * object)
{
  GstRdsLogger *self = GST_RDS_LOGGER (object);

  /* the clock id holds a reference: it is gone by now */
  g_assert (self->clock_id == NULL);
  for (guint i = 0; i < N_NOOPS; i++)
    g_value_unset (&self->noops[i]);
  g_array_unref (self->streams);
  g_mutex_clear (&self->lock);
  G_OBJECT_CLASS (gst_rds_logger_parent_class)->finalize (object);
}

static void
gst_rds_logger_init (GstRdsLogger * self)
{
  self->silent = DEFAULT_SILENT;
  self->signal_handoffs = DEFAULT_SIGNAL_HANDOFFS;
  for (guint i = 0; i < N_NOOPS; i++) {
    g_value_init (&self->noops[i], G_PARAM_SPEC_VALUE_TYPE (noop_specs[i]));
    g_param_value_set_default (noop_specs[i], &self->noops[i]);
  }
  g_mutex_init (&self->lock);
  self->interval = DEFAULT_FPS_INTERVAL;
  self->remaining = GST_CLOCK_TIME_NONE;
  self->streams = g_array_new (FALSE, FALSE, sizeof (Stream));
  gst_base_transform_set_passthrough (GST_BASE_TRANSFORM (self), TRUE);
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (self), TRUE);
}

static void
gst_rds_logger_class_init (GstRdsLoggerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  GST_DEBUG_CATEGORY_INIT (gst_rds_logger_debug, "rdslogger", 0, "Redstream per-stream FPS logger");

  gobject_class->set_property = gst_rds_logger_set_property;
  gobject_class->get_property = gst_rds_logger_get_property;
  gobject_class->finalize = gst_rds_logger_finalize;

  g_object_class_install_property (gobject_class, PROP_FPS_INTERVAL,
      g_param_spec_uint ("fps-measurement-interval-sec", "FPS measurement interval",
          "Seconds over which frames are counted for each frame rate printed (0: no printing)", 0, G_MAXUINT,
          DEFAULT_FPS_INTERVAL, flags));
  g_object_class_install_property (gobject_class, PROP_SILENT,
      g_param_spec_boolean ("silent", "Silent", "Print only the frame rates (FALSE: also two lines per buffer)",
          DEFAULT_SILENT, flags));
  g_object_class_install_property (gobject_class, PROP_SIGNAL_HANDOFFS,
      g_param_spec_boolean ("signal-handoffs", "Signal handoffs", "Emit \"handoff\" for every buffer",
          DEFAULT_SIGNAL_HANDOFFS, flags));
  g_object_class_install_property (gobject_class, PROP_LAST_MESSAGE,
      g_param_spec_string ("last-message", "Last message", "Accepted for compatibility; always NULL", NULL,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /* identity's, with gst-inspect's ranges and defaults for DeepStream 8.0's nvdslogger */
#define NOOP_DESC "Accepted for compatibility (identity's); has no effect"
  noop_specs[NOOP_CHECK_IMPERFECT_OFFSET] = g_param_spec_boolean ("check-imperfect-offset",
      "Check imperfect offset", NOOP_DESC, FALSE, flags);
  noop_specs[NOOP_CHECK_IMPERFECT_TIMESTAMP] = g_param_spec_boolean ("check-imperfect-timestamp",
      "Check imperfect timestamp", NOOP_DESC, FALSE, flags);
  noop_specs[NOOP_DATARATE] = g_param_spec_int ("datarate", "Data rate", NOOP_DESC, 0, G_MAXINT, 0, flags);
  noop_specs[NOOP_DROP_ALLOCATION] = g_param_spec_boolean ("drop-allocation", "Drop allocation", NOOP_DESC, FALSE,
      flags);
  noop_specs[NOOP_DROP_BUFFER_FLAGS] = g_param_spec_flags ("drop-buffer-flags", "Drop buffer flags", NOOP_DESC,
      GST_TYPE_BUFFER_FLAGS, 0, flags);
  noop_specs[NOOP_DROP_PROBABILITY] = g_param_spec_float ("drop-probability", "Drop probability", NOOP_DESC, 0, 1,
      0, flags);
  noop_specs[NOOP_DUMP] = g_param_spec_boolean ("dump", "Dump", NOOP_DESC, FALSE, flags);
  noop_specs[NOOP_ERROR_AFTER] = g_param_spec_int ("error-after", "Error after", NOOP_DESC, G_MININT, G_MAXINT, -1,
      flags);
  noop_specs[NOOP_LATENCY] = g_param_spec_uint ("latency", "Latency", NOOP_DESC, 0, G_MAXUINT, 0, flags);
  noop_specs[NOOP_SINGLE_SEGMENT] = g_param_spec_boolean ("single-segment", "Single segment", NOOP_DESC, FALSE,
      flags);
  noop_specs[NOOP_SLEEP_TIME] = g_param_spec_uint ("sleep-time", "Sleep time", NOOP_DESC, 0, G_MAXUINT, 0, flags);
  noop_specs[NOOP_SYNC] = g_param_spec_boolean ("sync", "Sync", NOOP_DESC, FALSE, flags);
  noop_specs[NOOP_TS_OFFSET] = g_param_spec_int64 ("ts-offset", "Timestamp offset", NOOP_DESC, G_MININT64,
      G_MAXINT64, 0, flags);
#undef NOOP_DESC
  for (guint i = 0; i < N_NOOPS; i++)
    g_object_class_install_property (gobject_class, PROP_NOOP_FIRST + i, noop_specs[i]);

  signals[SIGNAL_HANDOFF] = g_signal_new ("handoff", G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL,
      NULL, G_TYPE_NONE, 1, GST_TYPE_BUFFER | G_SIGNAL_TYPE_STATIC_SCOPE);

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream FPS logger", "Generic",
      "Passes data through and prints each stream's frame rate to stdout", "Redstream contributors");

  element_class->change_state = GST_DEBUG_FUNCPTR (gst_rds_logger_change_state);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_rds_logger_transform_ip);
  trans_class->sink_event = GST_DEBUG_FUNCPTR (gst_rds_logger_sink_event);
  trans_class->transform_ip_on_passthrough = TRUE;
}
