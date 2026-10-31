/* Redstream: rdsinfer's per-object result cache for secondary mode (plugin-internal, not installed): which tracked
 * objects are due for inference on this frame, and a classifier's last result for the others. C, no GPU, no
 * GStreamer element types, no parsing.
 *
 * Entries are keyed by (source_id, object_id); untracked objects (RDS_UNTRACKED_OBJECT_ID) never come here, the
 * element infers them every time. An entry is
 * - EMPTY: no result stored (only after a failed or dropped first request): due on its next lookup;
 * - PENDING: a request (its seq) is out; a stored result, if any, is still served meanwhile;
 * - READY: a result is stored (a classifier's labels, which may be none: an empty result is a result).
 * Results are values (RdsInferClassifierResult, deep-copied in and out), never metadata pointers. A detector's
 * entry stores no result (NULL): it only tracks when the object is due.
 *
 * Locking: the cache has its own mutex, held only around its own reads and writes (no GPU work, GStreamer calls or
 * parsing under it). Every function but _new and _free may be called from any thread, concurrently; _free must not
 * race with any other call. A caller that holds a batch meta lock may call in; the cache never takes a batch meta
 * lock (lock order: batch meta lock, then cache lock). Copies of results: rds_infer_cache_lookup_or_add and _cancel
 * copy the stored result out under the cache lock; rds_infer_cache_complete copies its input before taking the lock
 * and frees the replaced result after releasing it (removals free entries under the lock).
 *
 * Staleness: a request carries (key, seq, epoch). rds_infer_cache_complete and _cancel act only if the entry still
 * exists, is still PENDING on that seq (seqs are never reused, so an entry evicted and added again has a new one)
 * and the cache's epoch is unchanged (rds_infer_cache_flush bumps it); otherwise the completion is stale and dropped.
 * Each rule is marked "DeepStream 8.0 behaviour" (measured) or "ours".
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_CACHE_H
#define RDS_INFER_CACHE_H

#include <glib.h>

#include "rds-infer-parse.h"

G_BEGIN_DECLS

/* Frames of its source an entry may go unseen before rds_infer_cache_evict_absent drops it (ours: DeepStream 8.0
 * never evicts). Only frames of that source count: a source that stops sending frames without a stream-eos or
 * stream-reset keeps its entries until a flush or stop (rdsstreammux sends a stream-eos when a pad is released). */
#define RDS_INFER_CACHE_MAX_ABSENT 30

typedef struct _RdsInferCache RdsInferCache;

/* A request handed out by rds_infer_cache_lookup_or_add for a due object, and handed back on completion. */
typedef struct
{
  guint source_id;
  guint64 object_id;
  guint64 seq;
  guint64 epoch;
} RdsInferCacheRequest;

/* An empty cache. @reinfer_interval is secondary-reinfer-interval: -1 never re-infers on time, 0 every frame, N
 * every N + 1 frames. */
G_GNUC_INTERNAL RdsInferCache *rds_infer_cache_new (gint reinfer_interval);
/* NULL is allowed. */
G_GNUC_INTERNAL void rds_infer_cache_free (RdsInferCache * cache);

/* The clock: one frame counter per source. A frame of @source_id begins: its counter ticks (once per frame, so a
 * batch with two frames of a source ticks it twice); returns the new value. The lookups and touches of the frame's
 * objects that follow are on this frame. A source that never began a frame is at 0. */
G_GNUC_INTERNAL guint64 rds_infer_cache_begin_frame (RdsInferCache * cache, guint source_id);

/* The object (@source_id, @object_id) with box area @area (rect_params width x height) was selected in its source's
 * current frame. Marks the entry seen on that frame and returns whether the object is due for inference:
 * - a new key (DeepStream 8.0 behaviour): added PENDING, due;
 * - a PENDING entry: not due, its request is still out (the same key twice before a completion: ours);
 * - an EMPTY entry: due (ours: only a failed or stale first request leaves one);
 * - a READY entry: due when @area > 1.2 x the area at its last inference, strictly and exactly (5 @area > 6 last;
 *   DeepStream 8.0 behaviour, which compares in float: ours where float rounding would differ), or by the interval:
 *   on its (N + 1)th selected frame after its last inference (DeepStream 8.0 behaviour; counted in the frames the
 *   object was selected, ours). Shrinking or moving never makes it due and never changes the reference area
 *   (DeepStream 8.0 behaviour).
 * When due, the entry goes PENDING with a new seq and *@req is filled; *@req is untouched otherwise. The area and
 * frame of the request become the entry's "last inference" only on completion.
 * @values (may be NULL): when the object is not due, set to a copy of the stored result (free it with
 * rds_infer_classifier_result_free), or NULL when none is stored; when it is due, NULL (no copy is made: a failed
 * request gets the stored result back from rds_infer_cache_cancel). */
G_GNUC_INTERNAL gboolean rds_infer_cache_lookup_or_add (RdsInferCache * cache, guint source_id, guint64 object_id,
    gdouble area, RdsInferCacheRequest * req, RdsInferClassifierResult ** values);
/* The object (@source_id, @object_id) is in its source's current frame but not selected (filtered out by class or
 * size, or an empty crop): marks its entry seen, if it has one, so it is not evicted while it stays (ours). Creates
 * nothing and counts no selected frame. */
G_GNUC_INTERNAL void rds_infer_cache_touch (RdsInferCache * cache, guint source_id, guint64 object_id);
/* Stores a copy of @values (NULL: no result, for a detector) as the entry's result, READY, with the request's area
 * and frame as its last inference. FALSE, nothing changed, when the request is stale: the caller then attaches
 * nothing, so what an object gets is always what its entry holds. An empty result is stored like any other and
 * serves until growth or the interval (DeepStream 8.0 behaviour). */
G_GNUC_INTERNAL gboolean rds_infer_cache_complete (RdsInferCache * cache, const RdsInferCacheRequest * req,
    const RdsInferClassifierResult * values);
/* The request failed (a parser error): the entry is left as it was before it, READY with its old result or EMPTY
 * without one, so the object is due again on its next lookup only if it was before (ours). @values (may be NULL):
 * set to a copy of the stored result, or NULL (none stored, or stale), for the caller to attach instead (ours).
 * FALSE when the request is stale. */
G_GNUC_INTERNAL gboolean rds_infer_cache_cancel (RdsInferCache * cache, const RdsInferCacheRequest * req,
    RdsInferClassifierResult ** values);
/* Drops every entry not seen for @max_absent frames of its source or more (the source's counter - last seen >=
 * @max_absent); returns how many. The element runs it once per batch, after the batch's lookups and touches, with
 * RDS_INFER_CACHE_MAX_ABSENT (ours). */
G_GNUC_INTERNAL guint rds_infer_cache_evict_absent (RdsInferCache * cache, guint64 max_absent);
/* Drops every entry of @source_id and its frame counter (its stream ended or was reset; ours); returns how many
 * entries. */
G_GNUC_INTERNAL guint rds_infer_cache_clear_source (RdsInferCache * cache, guint source_id);
/* Drops every entry and frame counter and bumps the epoch, so every request out is stale (a flush; ours). */
G_GNUC_INTERNAL void rds_infer_cache_flush (RdsInferCache * cache);
/* The number of entries. */
G_GNUC_INTERNAL guint rds_infer_cache_get_size (RdsInferCache * cache);

G_END_DECLS

#endif /* RDS_INFER_CACHE_H */
