/* Redstream: the rdstracker tracking core (plugin-internal, not installed). Targets and their life cycle, tracking
 * IDs, per-stream state and data association, in plain C with GLib only: no GStreamer, no GPU. Boxes are in frame
 * coordinates (left, top, width, height) throughout; the core never scales them.
 *
 * Life cycle (DeepStream 8.0 behaviour): a detection that matches no target creates a Tentative target. It becomes
 * Active, and takes its ID, on the first frame where its age exceeds probationAge and it is matched. An Active
 * target that misses a frame with inference goes Inactive (not reported) and is Active again on its next match.
 * Targets terminate on shadowTrackingAge (consecutive frames with inference without a match): Active and Inactive
 * ones when it exceeds maxShadowTrackingAge, Tentative ones when it reaches earlyTerminationAge or exceeds
 * maxShadowTrackingAge. A terminated target is gone; an object that comes back becomes a new target.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TRACKER_CORE_H
#define RDS_TRACKER_CORE_H

#include "rds-tracker-config.h"

G_BEGIN_DECLS

/* The element's tracking-id-reset-mode property. */
typedef enum
{
  RDS_TRACKER_ID_RESET_NONE = 0,        /* stream reset and EOS change nothing */
  RDS_TRACKER_ID_RESET_ON_RESET = 1,    /* a stream reset terminates the stream's targets */
  RDS_TRACKER_ID_RESET_ON_EOS = 2,      /* per-stream IDs; a stream EOS terminates its targets and restarts its IDs */
  RDS_TRACKER_ID_RESET_BOTH = 3,        /* 1 and 2 together */
} RdsTrackerIdResetMode;

typedef struct
{
  RdsTrackerIdResetMode reset_mode;
  /* What a target keeps of its last matched detection (RdsTrackerTarget.last_user). With @user_copy NULL it keeps the
   * detection's user pointer as it is, which is then only an opaque cookie: the caller's detection is gone by the
   * next batch. With @user_copy set, the core calls it on every matched detection's user pointer (with
   * @user_copy_data) and keeps the copy, freeing the previous one, and the copy of a terminated target, with
   * @user_free (required with @user_copy). The element copies what it needs from the detection (label, display
   * text, component ID) this way, so a target's copy lives exactly as long as the target, whatever its ID. A
   * reported last_user is valid only until the next call that can end a target: rds_tracker_core_process,
   * _stream_reset, _stream_eos, _remove_stream, _clear or _free. */
  GCopyFunc user_copy;
  gpointer user_copy_data;
  GDestroyNotify user_free;
} RdsTrackerCoreOptions;

typedef struct
{
  gfloat x, y, w, h;
  gint class_id;
  gfloat confidence;
  gpointer user;                /* the caller's, returned in RdsTrackerTarget and in the rejected list */
} RdsTrackerDet;

typedef struct
{
  guint64 stream_id;
  gint64 frame_num;
  gboolean detection_done;      /* FALSE: no inference ran on this frame */
  gboolean reset;               /* a stream reset before this frame (as rds_tracker_core_stream_reset) */
  const RdsTrackerDet *dets;
  guint n_dets;
} RdsTrackerFrameIn;

typedef struct
{
  guint64 id;
  gfloat x, y, w, h;
  gint class_id;
  gfloat tracker_confidence;
  gpointer associated_user;     /* this frame's matched detection's user pointer; NULL: not matched this frame */
  gpointer last_user;           /* the last matched detection's user pointer, or its copy (RdsTrackerCoreOptions);
                                 * valid until the next call that can end a target */
  guint age;
} RdsTrackerTarget;

typedef struct
{
  /* On a frame with inference, the Active targets matched on it, in their detections' input order. On a frame
   * without inference, every Active target, newest ID first, with its last matched box and no associated_user. */
  const RdsTrackerTarget *targets;
  guint n_targets;
  /* The user pointers of the frame's detections that have no reported target, in input order: an invalid box (not
   * finite, or no area), below minDetectorConfidence, matched to or creating a Tentative target, duplicates, over
   * maxTargetsPerStream, and every detection of a frame without inference (DeepStream 8.0 behaviour). */
  gpointer const *rejected;
  guint n_rejected;
} RdsTrackerFrameOut;

/* Owned by the core: valid until the next rds_tracker_core_process or rds_tracker_core_free. */
typedef struct
{
  const RdsTrackerFrameOut *frames;     /* one per input frame, in the same order */
  guint n_frames;
} RdsTrackerBatchOut;

typedef struct _RdsTrackerCore RdsTrackerCore;

/* Copies what it needs from @cfg. @opts NULL: reset mode 1, no user copies. The IOU tracker and NvSORT share this
 * core; cfg->tracker_type is kept for the NvSORT path, which associates as the IOU tracker for now. */
G_GNUC_INTERNAL RdsTrackerCore *rds_tracker_core_new (const RdsTrackerConfig * cfg,
    const RdsTrackerCoreOptions * opts);
G_GNUC_INTERNAL void rds_tracker_core_free (RdsTrackerCore * core);

/* Tracks one batch: @frames in batch order, at most one per stream (a critical and an empty @out otherwise).
 * Streams are created on first sight. */
G_GNUC_INTERNAL void rds_tracker_core_process (RdsTrackerCore * core, const RdsTrackerFrameIn * frames,
    guint n_frames, RdsTrackerBatchOut * out);

/* Frees one stream's state (its targets and its ID base). */
G_GNUC_INTERNAL void rds_tracker_core_remove_stream (RdsTrackerCore * core, guint64 stream_id);
/* A stream reset: terminates the stream's targets in reset modes 1 and 3. */
G_GNUC_INTERNAL void rds_tracker_core_stream_reset (RdsTrackerCore * core, guint64 stream_id);
/* A stream EOS: in reset modes 2 and 3, terminates the stream's targets and restarts its IDs at 0. */
G_GNUC_INTERNAL void rds_tracker_core_stream_eos (RdsTrackerCore * core, guint64 stream_id);
/* Drops every stream (a flush). The ID counter continues and the useUniqueID upper half stays. */
G_GNUC_INTERNAL void rds_tracker_core_clear (RdsTrackerCore * core);

/* The number of live targets of a stream, in every state (0 for an unknown stream). */
G_GNUC_INTERNAL guint rds_tracker_core_get_n_targets (RdsTrackerCore * core, guint64 stream_id);

/* Tests only: fixes the upper 32 bits of every ID assigned from now on (useUniqueID's random value). */
G_GNUC_INTERNAL void rds_tracker_core_set_unique_id_upper (RdsTrackerCore * core, guint32 upper);

G_END_DECLS

#endif /* RDS_TRACKER_CORE_H */
