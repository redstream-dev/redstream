/* Redstream: when rdsurisrcbin reconnects an RTSP source (see rds-urisrc-reconnect.h for the contract).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-urisrc-reconnect.h"

GST_DEBUG_CATEGORY_EXTERN (gst_rds_uri_src_bin_debug);
#define GST_CAT_DEFAULT gst_rds_uri_src_bin_debug

/* the watchdog's period (DeepStream 8.0 behaviour: the interval is checked once a second) */
#define TICK GST_SECOND

struct _RdsUriSrcReconnect
{
  GstElement *bin;              /* not a ref: the bin owns this */
  RdsUriSrcReconnectCallbacks callbacks;

  GMutex job_lock;              /* held by a running job: one at a time */
  GMutex lock;
  GCond job_done;
  /* (lock) */
  guint run;                    /* the run's number: a job queued in another run does nothing */
  gint64 interval, init_interval;       /* us */
  gint max_attempts;            /* < 0: unlimited */
  gboolean stopped;             /* no run, or the run is stopping: no new job */
  gboolean connected;           /* a buffer came in this run */
  gboolean gave_up;             /* or the stream ended: no attempt for the rest of the run */
  gpointer session;             /* the current session, a token never dereferenced (NULL: none counts) */
  gboolean live;                /* it gave a buffer */
  gboolean failed;              /* it failed before giving one: the next attempt's reason is failed_reason */
  RdsUriSrcReconnectReason failed_reason;
  guint attempts;               /* consecutive attempts without a buffer */
  gint64 last;                  /* monotonic us: the last buffer, the last attempt's start, or PLAYING */
  gboolean queued;              /* a job of this run is queued or running */
  RdsUriSrcReconnectReason reason;      /* the queued job's */
  GThread *job_thread;          /* the thread running a job */
  GstClockID tick;              /* the watchdog, while PLAYING */
};

typedef struct
{
  RdsUriSrcReconnect *r;        /* alive: gst_element_call_async holds a ref on the bin, which owns it */
  guint run;
  gboolean end;                 /* the application's end of the stream, else an attempt or giving up */
} Job;

const gchar *
rds_urisrc_reconnect_reason_name (RdsUriSrcReconnectReason reason)
{
  switch (reason) {
    case RDS_URISRC_RECONNECT_ERROR:
      return "error";
    case RDS_URISRC_RECONNECT_EOS:
      return "eos";
    default:
      return "timeout";
  }
}

RdsUriSrcReconnect *
rds_urisrc_reconnect_new (GstElement * bin, const RdsUriSrcReconnectCallbacks * callbacks)
{
  RdsUriSrcReconnect *r = g_new0 (RdsUriSrcReconnect, 1);

  r->bin = bin;
  r->callbacks = *callbacks;
  r->stopped = TRUE;
  g_mutex_init (&r->job_lock);
  g_mutex_init (&r->lock);
  g_cond_init (&r->job_done);
  return r;
}

void
rds_urisrc_reconnect_free (RdsUriSrcReconnect * r)
{
  g_warn_if_fail (r->tick == NULL);
  g_mutex_clear (&r->job_lock);
  g_mutex_clear (&r->lock);
  g_cond_clear (&r->job_done);
  g_free (r);
}

/* (lock) Whether no further attempt is allowed. */
static gboolean
limit_reached (RdsUriSrcReconnect * r)
{
  return r->max_attempts >= 0 && r->attempts >= (guint) r->max_attempts;
}

/* (lock) Reconnection is on: always with an interval, and before the first buffer with an initial interval. */
static gboolean
active (RdsUriSrcReconnect * r)
{
  return r->interval > 0 || (!r->connected && r->init_interval > 0);
}

/* (lock) The interval the watchdog waits; 0: none. */
static gint64
current_interval (RdsUriSrcReconnect * r)
{
  if (!r->connected && r->init_interval > 0)
    return r->init_interval;
  return r->interval;
}

/* GStreamer's thread pool, holding job_lock: the application's end of the stream. */
static void
run_end (GstElement * bin, Job * job)
{
  RdsUriSrcReconnect *r = job->r;

  g_mutex_lock (&r->lock);
  if (r->stopped || r->run != job->run) {
    g_mutex_unlock (&r->lock);
    return;
  }
  r->job_thread = g_thread_self ();
  g_mutex_unlock (&r->lock);

  GST_INFO_OBJECT (bin, "the application ends the stream");
  r->callbacks.end (bin);

  g_mutex_lock (&r->lock);
  r->job_thread = NULL;
  g_cond_broadcast (&r->job_done);
  g_mutex_unlock (&r->lock);
}

/* GStreamer's thread pool, holding job_lock: an attempt, or giving up. */
static void
run_attempt (GstElement * bin, Job * job)
{
  RdsUriSrcReconnect *r = job->r;
  RdsUriSrcReconnectReason reason;
  gboolean give_up, go;
  guint attempt;

  g_mutex_lock (&r->lock);
  /* a job of an earlier run, or one queued just before the stream ended */
  if (r->stopped || r->gave_up || r->run != job->run) {
    g_mutex_unlock (&r->lock);
    return;
  }
  give_up = limit_reached (r);
  if (give_up)
    r->gave_up = TRUE;
  else
    r->attempts++;
  attempt = r->attempts;
  reason = r->reason;
  /* the next attempt an interval from here: a refused one ends at once, and does not bring the next one closer */
  r->last = g_get_monotonic_time ();
  /* the session being replaced no longer counts: its late errors are not this attempt's */
  r->session = NULL;
  r->live = r->failed = FALSE;
  r->job_thread = g_thread_self ();
  g_mutex_unlock (&r->lock);

  if (give_up) {
    GST_INFO_OBJECT (bin, "giving up after %u reconnection attempts", attempt);
    r->callbacks.give_up (bin, attempt);
  } else {
    GST_INFO_OBJECT (bin, "reconnection attempt %u (%s)", attempt, rds_urisrc_reconnect_reason_name (reason));
    r->callbacks.announce (bin, attempt, reason);
    /* a sync handler may have stopped the bin while the messages were posted */
    g_mutex_lock (&r->lock);
    go = !r->stopped && r->run == job->run;
    g_mutex_unlock (&r->lock);
    if (go)
      r->callbacks.replace (bin);
  }

  g_mutex_lock (&r->lock);
  r->job_thread = NULL;
  if (r->run == job->run)
    r->queued = FALSE;
  g_cond_broadcast (&r->job_done);
  g_mutex_unlock (&r->lock);
}

static void
run_job (GstElement * bin, gpointer user_data)
{
  Job *job = user_data;

  /* one job at a time: an end queued while an attempt runs waits for it */
  g_mutex_lock (&job->r->job_lock);
  if (job->end)
    run_end (bin, job);
  else
    run_attempt (bin, job);
  g_mutex_unlock (&job->r->job_lock);
}

/* (lock) Queues a job for @reason, unless one is; TRUE when the caller must call queue_job after unlocking. */
static gboolean
want_job (RdsUriSrcReconnect * r, RdsUriSrcReconnectReason reason)
{
  if (r->queued)
    return FALSE;
  r->queued = TRUE;
  r->reason = reason;
  return TRUE;
}

static void
queue_job (RdsUriSrcReconnect * r, guint run, gboolean end)
{
  Job *job = g_new (Job, 1);

  job->r = r;
  job->run = run;
  job->end = end;
  gst_element_call_async (r->bin, run_job, job, g_free);
}

/* The clock's thread, once a second while PLAYING: never blocks. */
static gboolean
tick (G_GNUC_UNUSED GstClock * clock, G_GNUC_UNUSED GstClockTime time, G_GNUC_UNUSED GstClockID id,
    gpointer user_data)
{
  RdsUriSrcReconnect *r = user_data;
  gint64 interval, age;
  gboolean queue = FALSE;
  guint run;

  g_mutex_lock (&r->lock);
  interval = current_interval (r);
  if (r->stopped || r->gave_up || r->queued || interval == 0) {
    g_mutex_unlock (&r->lock);
    return TRUE;
  }
  age = g_get_monotonic_time () - r->last;
  GST_DEBUG_OBJECT (r->bin, "%" G_GINT64_FORMAT " ms since the last buffer or attempt (interval %" G_GINT64_FORMAT
      " ms)", age / 1000, interval / 1000);
  /* also an attempt that failed with an error while it was starting, when it was the last one allowed */
  if (age >= interval || (r->failed && limit_reached (r)))
    queue = want_job (r, r->failed ? r->failed_reason : RDS_URISRC_RECONNECT_TIMEOUT);
  run = r->run;
  g_mutex_unlock (&r->lock);
  if (queue)
    queue_job (r, run, FALSE);
  return TRUE;
}

static void
release_bin (gpointer user_data)
{
  gst_object_unref (((RdsUriSrcReconnect *) user_data)->bin);
}

void
rds_urisrc_reconnect_start_run (RdsUriSrcReconnect * r, guint interval, guint init_interval, gint attempts)
{
  g_mutex_lock (&r->lock);
  r->run++;
  r->interval = (gint64) interval * G_USEC_PER_SEC;
  r->init_interval = (gint64) init_interval * G_USEC_PER_SEC;
  r->max_attempts = attempts;
  r->stopped = r->connected = r->gave_up = r->live = r->failed = r->queued = FALSE;
  r->session = NULL;
  r->attempts = 0;
  r->last = g_get_monotonic_time ();
  g_mutex_unlock (&r->lock);
}

void
rds_urisrc_reconnect_play (RdsUriSrcReconnect * r)
{
  GstClock *clock;
  GstClockID id;

  g_mutex_lock (&r->lock);
  if (r->stopped || r->tick != NULL || (r->interval == 0 && r->init_interval == 0)) {
    g_mutex_unlock (&r->lock);
    return;
  }
  r->last = g_get_monotonic_time ();
  clock = gst_system_clock_obtain ();
  id = r->tick = gst_clock_new_periodic_id (clock, gst_clock_get_time (clock) + TICK, TICK);
  gst_object_unref (clock);
  gst_object_ref (r->bin);
  gst_clock_id_wait_async (id, tick, r, release_bin);
  g_mutex_unlock (&r->lock);
}

void
rds_urisrc_reconnect_pause (RdsUriSrcReconnect * r)
{
  GstClockID id;

  g_mutex_lock (&r->lock);
  id = g_steal_pointer (&r->tick);
  g_mutex_unlock (&r->lock);
  if (id != NULL) {
    /* a tick under way may still queue a job: the job does nothing once the run is stopped */
    gst_clock_id_unschedule (id);
    gst_clock_id_unref (id);
  }
}

void
rds_urisrc_reconnect_stop (RdsUriSrcReconnect * r)
{
  g_mutex_lock (&r->lock);
  r->stopped = TRUE;
  while (r->job_thread != NULL && r->job_thread != g_thread_self ())
    g_cond_wait (&r->job_done, &r->lock);
  g_mutex_unlock (&r->lock);
  rds_urisrc_reconnect_pause (r);
}

gboolean
rds_urisrc_reconnect_active (RdsUriSrcReconnect * r)
{
  gboolean on;

  g_mutex_lock (&r->lock);
  on = active (r);
  g_mutex_unlock (&r->lock);
  return on;
}

void
rds_urisrc_reconnect_end (RdsUriSrcReconnect * r)
{
  gboolean queue;
  guint run;

  g_mutex_lock (&r->lock);
  queue = !r->stopped;
  if (queue)
    r->gave_up = TRUE;
  run = r->run;
  g_mutex_unlock (&r->lock);
  if (queue)
    queue_job (r, run, TRUE);
}

void
rds_urisrc_reconnect_set_session (RdsUriSrcReconnect * r, gpointer session)
{
  g_mutex_lock (&r->lock);
  r->session = session;
  r->live = r->failed = FALSE;
  g_mutex_unlock (&r->lock);
}

void
rds_urisrc_reconnect_buffer (RdsUriSrcReconnect * r, gpointer session)
{
  g_mutex_lock (&r->lock);
  if (session == r->session) {
    if (!r->live) {
      GST_DEBUG_OBJECT (r->bin, "first buffer from the session");
      r->live = r->connected = TRUE;
      r->attempts = 0;
    }
    r->last = g_get_monotonic_time ();
  }
  g_mutex_unlock (&r->lock);
}

void
rds_urisrc_reconnect_lost (RdsUriSrcReconnect * r, gpointer session, RdsUriSrcReconnectReason reason)
{
  gboolean queue = FALSE;
  guint run;

  g_mutex_lock (&r->lock);
  if (session != r->session || session == NULL || r->stopped || r->gave_up || !active (r)) {
    g_mutex_unlock (&r->lock);
    return;
  }
  /* once: its later errors say nothing new */
  r->session = NULL;
  if (r->live) {
    /* a working session: at once (ours; DeepStream 8.0 waits for the watchdog) */
    GST_INFO_OBJECT (r->bin, "the RTSP session ended (%s)", rds_urisrc_reconnect_reason_name (reason));
    queue = want_job (r, reason);
  } else {
    /* an attempt (or the first connection) failed: the next attempt an interval after it started, or giving up now */
    GST_INFO_OBJECT (r->bin, "the RTSP session failed before its first buffer (%s)",
        rds_urisrc_reconnect_reason_name (reason));
    r->failed = TRUE;
    r->failed_reason = reason;
    if (limit_reached (r))
      queue = want_job (r, reason);
  }
  r->live = FALSE;
  run = r->run;
  g_mutex_unlock (&r->lock);
  if (queue)
    queue_job (r, run, FALSE);
}
