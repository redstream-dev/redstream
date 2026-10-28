/* Redstream: when rdsurisrcbin reconnects an RTSP source: the watchdog, the attempt count and the attempt jobs.
 *
 * Contract: one RdsUriSrcReconnect per bin, owned by it. It decides when to replace the RTSP session and when to give
 * up; the bin does the work in the callbacks (posting its messages, replacing the session, ending the stream). It
 * never touches the bin's children itself.
 *
 *   - A run (rds_urisrc_reconnect_start_run, READY to PAUSED) takes the three settings. Reconnection is active while
 *     rtsp-reconnect-interval is above 0, and before the first buffer of the run also while
 *     init-rtsp-reconnect-interval is (the start-up retries); rds_urisrc_reconnect_active says so.
 *   - The bin names the current session (rds_urisrc_reconnect_set_session, an opaque token) and reports its buffers
 *     (rds_urisrc_reconnect_buffer) and its loss: an ERROR, or an EOS that the bin dropped (rds_urisrc_reconnect_lost).
 *     Reports about another session (one being replaced) are ignored, and so is a session's loss after the first.
 *   - The loss of a session that gave buffers starts an attempt at once (reason error or eos). The watchdog, a
 *     periodic system-clock id ticking every second while PLAYING, starts one when no buffer came for the interval
 *     (timeout); the interval counts from the last buffer or the last attempt's start, whichever is later, so a
 *     failed attempt (an error, or no buffer within the interval) is followed by the next one an interval after it
 *     started. Before the first buffer of the run the interval is init-rtsp-reconnect-interval if above 0, else
 *     rtsp-reconnect-interval.
 *   - The attempt count is the number of consecutive attempts without a buffer; a buffer resets it. With
 *     rtsp-reconnect-attempts N >= 0, the failure of attempt N (or the first loss, with N 0) gives up instead: no
 *     attempt after it, and the watchdog stops for the run. So does the end of the stream the application asks for
 *     (rds_urisrc_reconnect_end), which also queues a job that ends the stream (the end callback). Reconnection stays
 *     active after either (rds_urisrc_reconnect_active is still TRUE), so the session's errors while the stream
 *     drains are still warnings.
 *   - Attempts, giving up and ending run as jobs on GStreamer's thread pool (gst_element_call_async), one at a time,
 *     never on a streaming thread or the clock's; a job of an earlier run, or one that starts after
 *     rds_urisrc_reconnect_stop, does nothing. The clock callback only takes the lock, reads times and queues a job.
 *   - rds_urisrc_reconnect_stop (PAUSED to READY) stops the run: no new job, the clock id removed, and a job under way
 *     waited for (unless it is the caller: a bus sync handler that stops the pipeline from inside a job's message).
 *     A job posts its messages from its own thread, so an application's bus sync handler must not wait there for
 *     another thread that is changing the pipeline's state: that thread waits for the job in turn, and both hang
 *     (setting the state from the handler itself is fine, as above). The end and give_up callbacks push EOS from the
 *     job's thread, so stop can wait while an element downstream blocks that EOS; inside a pipeline, the state change
 *     that stops the bin also unblocks it.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_URISRC_RECONNECT_H
#define RDS_URISRC_RECONNECT_H

#include <gst/gst.h>

G_BEGIN_DECLS

typedef enum
{
  RDS_URISRC_RECONNECT_TIMEOUT,
  RDS_URISRC_RECONNECT_ERROR,
  RDS_URISRC_RECONNECT_EOS,
} RdsUriSrcReconnectReason;

/* "timeout", "error", "eos": the reason field of rds-reconnect. */
const gchar *rds_urisrc_reconnect_reason_name (RdsUriSrcReconnectReason reason);

typedef struct
{
  /* Attempt @attempt (1, 2, ...) is starting: post its messages. Called first, so a sync handler that stops the bin
   * here stops the attempt too. */
  void (*announce) (GstElement * bin, guint attempt, RdsUriSrcReconnectReason reason);
  /* Replace the session with a new one, naming it with rds_urisrc_reconnect_set_session before it starts. */
  void (*replace) (GstElement * bin);
  /* @attempts attempts failed (or none was allowed): post the messages and end the stream. */
  void (*give_up) (GstElement * bin, guint attempts);
  /* The application asked for the end of the stream (rds_urisrc_reconnect_end): end it. */
  void (*end) (GstElement * bin);
} RdsUriSrcReconnectCallbacks;

typedef struct _RdsUriSrcReconnect RdsUriSrcReconnect;

RdsUriSrcReconnect *rds_urisrc_reconnect_new (GstElement * bin, const RdsUriSrcReconnectCallbacks * callbacks);
void rds_urisrc_reconnect_free (RdsUriSrcReconnect * r);

/* A new run, with the settings in seconds (attempts < 0: unlimited); no watchdog yet. */
void rds_urisrc_reconnect_start_run (RdsUriSrcReconnect * r, guint interval, guint init_interval, gint attempts);
/* PAUSED to PLAYING: the watchdog starts (the interval counts from now). */
void rds_urisrc_reconnect_play (RdsUriSrcReconnect * r);
/* PLAYING to PAUSED: the watchdog stops. */
void rds_urisrc_reconnect_pause (RdsUriSrcReconnect * r);
/* PAUSED to READY (see above). */
void rds_urisrc_reconnect_stop (RdsUriSrcReconnect * r);

/* Whether reconnection is active now: the session's errors are warnings, and its EOS is dropped. */
gboolean rds_urisrc_reconnect_active (RdsUriSrcReconnect * r);
/* @session is the current session from now on (before it can report anything). */
void rds_urisrc_reconnect_set_session (RdsUriSrcReconnect * r, gpointer session);
/* A buffer came out of @session (any thread). */
void rds_urisrc_reconnect_buffer (RdsUriSrcReconnect * r, gpointer session);
/* @session failed (an ERROR) or ended (an EOS the bin dropped); any thread. */
void rds_urisrc_reconnect_lost (RdsUriSrcReconnect * r, gpointer session, RdsUriSrcReconnectReason reason);
/* The application's EOS (any thread): no attempt for the rest of the run, and the end callback from a job of this
 * run, after any job under way. Nothing outside a run. */
void rds_urisrc_reconnect_end (RdsUriSrcReconnect * r);

G_END_DECLS

#endif /* RDS_URISRC_RECONNECT_H */
