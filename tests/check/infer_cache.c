/* rdsinfer's per-object cache: when a tracked object is due (new id, growth, the reinfer interval), what it serves
 * otherwise (an empty result too), eviction on each source's own frame count, objects present but not selected,
 * per-source clears and flushes, stale completions dropped by seq and epoch, and completions from a second thread
 * while the first one looks up.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-cache.h"

/* A result with one label of class @class_id (layer 0, 3 classes, prob 0.5), or none with @class_id < 0. */
static RdsInferClassifierResult *
make_result (gint class_id)
{
  RdsInferClassifierResult *r = rds_infer_classifier_result_new ();

  if (class_id >= 0)
    rds_infer_classifier_result_add (r, 0, (guint) class_id, 3, 0.5f, "label");
  return r;
}

/* The class of @v's first label: -1 for an empty result, -2 for none. */
static gint
class_of (const RdsInferClassifierResult * v)
{
  return !v ? -2 : v->labels->len == 0 ? -1 : (gint) g_array_index (v->labels, RdsInferClassifierLabel, 0).class_id;
}

/* Completes the due object's request @req with a result of class @class_id (-1: an empty result). */
static void
complete (RdsInferCache * c, const RdsInferCacheRequest * req, gint class_id)
{
  RdsInferClassifierResult *r = make_result (class_id);

  g_assert_true (rds_infer_cache_complete (c, req, r));
  rds_infer_classifier_result_free (r);
}

/* Looks up (@source, @id) with @area in its source's current frame: returns whether due; a due object is completed
 * at once with class @class_id (as the element does in sync mode). *@served (may be NULL) gets the stored class
 * (class_of; none when due: no copy is made then). */
static gboolean
step (RdsInferCache * c, guint source, guint64 id, gdouble area, gint class_id, gint * served)
{
  RdsInferCacheRequest req = { 0 };
  RdsInferClassifierResult *v = NULL;
  gboolean due = rds_infer_cache_lookup_or_add (c, source, id, area, &req, &v);

  if (served)
    *served = class_of (v);
  rds_infer_classifier_result_free (v);
  if (due) {
    g_assert_cmpuint (req.source_id, ==, source);
    g_assert_cmpuint (req.object_id, ==, id);
    complete (c, &req, class_id);
  }
  return due;
}

/* A new frame of @source with the one object @id in it: step. */
static gboolean
frame_step (RdsInferCache * c, guint source, guint64 id, gdouble area, gint class_id, gint * served)
{
  rds_infer_cache_begin_frame (c, source);
  return step (c, source, id, area, class_id, served);
}

/* A new id is due; afterwards it is served its stored copy (a deep copy each time), and other ids and other sources
 * have their own entries (DeepStream 8.0 behaviour). */
static void
test_new_id (void)
{
  RdsInferCache *c = rds_infer_cache_new (-1);
  RdsInferCacheRequest req = { 0 };
  RdsInferClassifierResult *v = NULL, *w = NULL;
  gint served;

  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 7, 100, &req, &v));
  g_assert_null (v);
  complete (c, &req, 2);
  rds_infer_cache_begin_frame (c, 0);
  g_assert_false (rds_infer_cache_lookup_or_add (c, 0, 7, 100, &req, &v));
  g_assert_false (rds_infer_cache_lookup_or_add (c, 0, 7, 100, &req, &w));
  g_assert_nonnull (v);
  g_assert_true (v != w && v->labels != w->labels);
  g_assert_cmpuint (v->labels->len, ==, 1);
  g_assert_cmpuint (g_array_index (v->labels, RdsInferClassifierLabel, 0).class_id, ==, 2);
  g_assert_cmpstr (g_array_index (v->labels, RdsInferClassifierLabel, 0).label, ==, "label");
  rds_infer_classifier_result_free (v);
  rds_infer_classifier_result_free (w);
  /* another id, the same id in another source */
  g_assert_true (step (c, 0, 8, 100, 1, NULL));
  rds_infer_cache_begin_frame (c, 1);
  g_assert_true (step (c, 1, 7, 100, 0, NULL));
  g_assert_false (frame_step (c, 0, 7, 100, 0, &served));
  g_assert_cmpint (served, ==, 2);
  g_assert_false (frame_step (c, 1, 7, 100, 0, &served));
  g_assert_cmpint (served, ==, 0);
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 3);
  rds_infer_cache_free (c);
}

/* Growth: due when the area is strictly above 1.2 x the area at the last inference (exactly 1.2: no; 1.2001: yes),
 * measured from that inference (100 -> 130 due; 140, 150 not; 160 due). Shrinking and moving never are and keep the
 * reference (100 -> 50 -> 61 -> 100 -> 120 not; 130 due). DeepStream 8.0 behaviour. */
static void
test_growth (void)
{
  RdsInferCache *c = rds_infer_cache_new (-1);

  g_assert_true (frame_step (c, 0, 1, 10000, 0, NULL));
  g_assert_false (frame_step (c, 0, 1, 12000, 0, NULL));
  g_assert_true (frame_step (c, 0, 1, 12001, 0, NULL));
  /* fractional: 12.5 x 5 = 62.5, x 1.2 = 75 */
  g_assert_true (frame_step (c, 0, 2, 12.5 * 5, 0, NULL));
  g_assert_false (frame_step (c, 0, 2, 15 * 5, 0, NULL));
  g_assert_true (frame_step (c, 0, 2, 75.0001, 0, NULL));

  g_assert_true (frame_step (c, 0, 3, 100, 0, NULL));
  g_assert_true (frame_step (c, 0, 3, 130, 0, NULL));
  g_assert_false (frame_step (c, 0, 3, 140, 0, NULL));
  g_assert_false (frame_step (c, 0, 3, 150, 0, NULL));
  g_assert_true (frame_step (c, 0, 3, 160, 0, NULL));

  g_assert_true (frame_step (c, 0, 4, 100, 0, NULL));
  g_assert_false (frame_step (c, 0, 4, 50, 0, NULL));
  g_assert_false (frame_step (c, 0, 4, 61, 0, NULL));
  g_assert_false (frame_step (c, 0, 4, 100, 0, NULL));
  g_assert_false (frame_step (c, 0, 4, 120, 0, NULL));
  g_assert_true (frame_step (c, 0, 4, 130, 0, NULL));
  rds_infer_cache_free (c);
}

/* The frames (of @n) on which an object of constant area is due with @interval, as a bit mask; the object is
 * selected only on the frames in @selected (a mask; 0: all) and only touched on the others. */
static guint64
due_frames (gint interval, guint n, guint64 selected)
{
  RdsInferCache *c = rds_infer_cache_new (interval);
  guint64 mask = 0;

  for (guint f = 0; f < n; f++) {
    rds_infer_cache_begin_frame (c, 0);
    if (selected && !(selected & (G_GUINT64_CONSTANT (1) << f)))
      rds_infer_cache_touch (c, 0, 1);
    else if (step (c, 0, 1, 100, 0, NULL))
      mask |= G_GUINT64_CONSTANT (1) << f;
  }
  rds_infer_cache_free (c);
  return mask;
}

/* secondary-reinfer-interval: unset never, 0 every frame, N every N + 1 frames counted from the object's own last
 * inference (DeepStream 8.0 behaviour), in the frames it was selected (ours; touches don't count). Growth restarts
 * the count. */
static void
test_interval (void)
{
  RdsInferCache *c;

  g_assert_cmphex (due_frames (-1, 12, 0), ==, 0x1);
  g_assert_cmphex (due_frames (0, 12, 0), ==, 0xfff);
  /* 1: f0, 2, 4, ...; 3: f0, 4, 8 */
  g_assert_cmphex (due_frames (1, 12, 0), ==, 0x555);
  g_assert_cmphex (due_frames (3, 12, 0), ==, 0x111);
  /* first seen on f6 with 3: f6, 10 */
  g_assert_cmphex (due_frames (3, 12, 0xfc0), ==, 0x440);
  /* selected on even frames only, interval 1: every second selection, f0, 4, 8 */
  g_assert_cmphex (due_frames (1, 12, 0x555), ==, 0x111);

  c = rds_infer_cache_new (3);
  g_assert_true (frame_step (c, 0, 1, 100, 0, NULL));
  g_assert_false (frame_step (c, 0, 1, 100, 0, NULL));
  /* growth on the second selection; the interval counts from there */
  g_assert_true (frame_step (c, 0, 1, 200, 0, NULL));
  g_assert_false (frame_step (c, 0, 1, 200, 0, NULL));
  g_assert_false (frame_step (c, 0, 1, 200, 0, NULL));
  g_assert_false (frame_step (c, 0, 1, 200, 0, NULL));
  g_assert_true (frame_step (c, 0, 1, 200, 0, NULL));
  rds_infer_cache_free (c);
}

/* An empty result is a result: served and not retried; only growth or the interval retries it (DeepStream 8.0
 * behaviour). A NULL completion (a detector's) stores no result and still counts as the inference. */
static void
test_empty_result (void)
{
  RdsInferCache *c = rds_infer_cache_new (-1);
  RdsInferCacheRequest req = { 0 };
  RdsInferClassifierResult *v = NULL;
  gint served;

  g_assert_true (frame_step (c, 0, 1, 100, -1, NULL));
  for (guint f = 1; f < 10; f++) {
    g_assert_false (frame_step (c, 0, 1, 100, 0, &served));
    g_assert_cmpint (served, ==, -1);
  }
  g_assert_true (frame_step (c, 0, 1, 121, 1, NULL));
  g_assert_false (frame_step (c, 0, 1, 121, 0, &served));
  g_assert_cmpint (served, ==, 1);

  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 2, 100, &req, NULL));
  g_assert_true (rds_infer_cache_complete (c, &req, NULL));
  rds_infer_cache_begin_frame (c, 0);
  g_assert_false (rds_infer_cache_lookup_or_add (c, 0, 2, 100, &req, &v));
  g_assert_null (v);
  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 2, 121, &req, &v));
  g_assert_null (v);
  rds_infer_cache_free (c);
}

/* A request out: the entry is PENDING, a second lookup is not due and serves the old result (ours); a due lookup
 * hands out no copy. A cancelled request leaves the entry as it was and hands back the stored result, if any: a new
 * id is due again with nothing to serve, a failed re-inference stays due and its old result is handed back (ours). */
static void
test_pending_cancel (void)
{
  RdsInferCache *c = rds_infer_cache_new (-1);
  RdsInferCacheRequest req = { 0 }, again = { 0 };
  RdsInferClassifierResult *v = NULL;
  gint served;

  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 1, 100, &req, &v));
  g_assert_false (rds_infer_cache_lookup_or_add (c, 0, 1, 100, &again, &v));
  g_assert_null (v);
  g_assert_true (rds_infer_cache_cancel (c, &req, &v));
  g_assert_null (v);
  g_assert_false (rds_infer_cache_cancel (c, &req, NULL));
  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 1, 100, &req, &v));
  g_assert_null (v);
  complete (c, &req, 2);

  /* growth: due (no copy handed out then), the old result still served while the request is out */
  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 1, 200, &req, &v));
  g_assert_null (v);
  g_assert_false (rds_infer_cache_lookup_or_add (c, 0, 1, 200, &again, &v));
  g_assert_cmpint (class_of (v), ==, 2);
  rds_infer_classifier_result_free (v);
  g_assert_true (rds_infer_cache_cancel (c, &req, &v));
  g_assert_cmpint (class_of (v), ==, 2);
  rds_infer_classifier_result_free (v);
  /* the reference area is still 100: due again, and the old result kept */
  g_assert_true (frame_step (c, 0, 1, 200, 1, &served));
  g_assert_cmpint (served, ==, -2);
  g_assert_false (frame_step (c, 0, 1, 200, 0, &served));
  g_assert_cmpint (served, ==, 1);
  /* a stale cancel hands back nothing */
  g_assert_false (rds_infer_cache_cancel (c, &req, &v));
  g_assert_null (v);
  rds_infer_cache_free (c);
}

/* Evicted after 30 absent frames of its source, not before (ours: DeepStream 8.0 never evicts). The pass runs after
 * a frame's lookups: seen on frame 1, absent on 2..30, back on 31: served; absent on 2..31: gone, due again. */
static void
test_eviction (void)
{
  RdsInferCache *c = rds_infer_cache_new (-1);

  g_assert_cmpuint (rds_infer_cache_begin_frame (c, 0), ==, 1);
  g_assert_true (step (c, 0, 1, 100, 0, NULL));
  g_assert_true (step (c, 0, 2, 100, 0, NULL));
  g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, 0);
  for (guint f = 2; f <= 30; f++) {
    g_assert_false (frame_step (c, 0, 2, 100, 0, NULL));
    g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, 0);
  }
  /* id 1 back on frame 31 after 29 absent frames */
  g_assert_false (frame_step (c, 0, 1, 100, 0, NULL));
  g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, 0);
  /* id 2 last seen on frame 30: absent on 31..60, evicted after frame 60 */
  for (guint f = 32; f < 60; f++) {
    g_assert_false (frame_step (c, 0, 1, 100, 0, NULL));
    g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, 0);
  }
  g_assert_cmpuint (rds_infer_cache_begin_frame (c, 0), ==, 60);
  g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, 1);
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 1);
  g_assert_true (frame_step (c, 0, 2, 100, 0, NULL));
  rds_infer_cache_free (c);
}

/* Each source has its own frame count (ours): a source that sends no frames keeps its entries however many frames
 * the others send, and two frames of one source in a batch count twice. */
static void
test_eviction_per_source (void)
{
  RdsInferCache *c = rds_infer_cache_new (-1);

  g_assert_true (frame_step (c, 0, 1, 100, 0, NULL));
  g_assert_true (frame_step (c, 1, 1, 100, 0, NULL));
  for (guint f = 0; f < 100; f++) {
    rds_infer_cache_begin_frame (c, 0);
    rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT);
  }
  /* source 0's entry went after 30 of its frames, source 1's (no frame since) is still there */
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 1);
  g_assert_false (frame_step (c, 1, 1, 100, 0, NULL));
  /* source 1 at frame 2; 15 batches of two source-1 frames each without the object: 30 frames */
  for (guint b = 0; b < 15; b++) {
    rds_infer_cache_begin_frame (c, 1);
    rds_infer_cache_begin_frame (c, 1);
    g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, b == 14);
  }
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 0);
  rds_infer_cache_free (c);
}

/* An object present but not selected (touched) keeps its entry however long, and isn't due when it is selected again
 * (ours); a touch creates nothing and doesn't count as a selected frame. */
static void
test_touch (void)
{
  RdsInferCache *c = rds_infer_cache_new (1);
  gint served;

  g_assert_true (frame_step (c, 0, 1, 100, 2, NULL));
  for (guint f = 0; f < 100; f++) {
    rds_infer_cache_begin_frame (c, 0);
    rds_infer_cache_touch (c, 0, 1);
    /* never seen: not created */
    rds_infer_cache_touch (c, 0, 9);
    g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, 0);
  }
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 1);
  /* interval 1: the first selection after the last inference is not due, however many touches came between */
  g_assert_false (frame_step (c, 0, 1, 100, 0, &served));
  g_assert_cmpint (served, ==, 2);
  g_assert_true (frame_step (c, 0, 1, 100, 0, NULL));
  rds_infer_cache_free (c);
}

/* Clearing a source drops only its entries and its frame count (a stream EOS or reset; ours); a flush drops every
 * one. */
static void
test_clear_flush (void)
{
  RdsInferCache *c = rds_infer_cache_new (-1);

  for (guint s = 0; s < 3; s++) {
    rds_infer_cache_begin_frame (c, s);
    for (guint64 id = 0; id < 4; id++)
      g_assert_true (step (c, s, id, 100, 0, NULL));
  }
  g_assert_cmpuint (rds_infer_cache_clear_source (c, 1), ==, 4);
  g_assert_cmpuint (rds_infer_cache_clear_source (c, 1), ==, 0);
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 8);
  g_assert_cmpuint (rds_infer_cache_begin_frame (c, 1), ==, 1);
  g_assert_true (step (c, 1, 0, 100, 0, NULL));
  g_assert_false (frame_step (c, 0, 0, 100, 0, NULL));
  g_assert_false (frame_step (c, 2, 3, 100, 0, NULL));
  rds_infer_cache_flush (c);
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 0);
  g_assert_cmpuint (rds_infer_cache_begin_frame (c, 0), ==, 1);
  g_assert_true (step (c, 0, 0, 100, 0, NULL));
  rds_infer_cache_free (c);
}

/* A completion is stored only for the entry that made the request, in the same epoch: after an eviction and the id
 * coming back (a new seq), after a flush (a new epoch), after a source clear, and for a request already completed
 * or superseded, it is dropped and changes nothing. */
static void
test_stale (void)
{
  RdsInferCache *c = rds_infer_cache_new (0);
  RdsInferCacheRequest old = { 0 }, cur = { 0 };
  RdsInferClassifierResult *r = make_result (1), *v = NULL;

  /* evicted and added again */
  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 1, 100, &old, NULL));
  for (guint f = 0; f < 30; f++)
    rds_infer_cache_begin_frame (c, 0);
  g_assert_cmpuint (rds_infer_cache_evict_absent (c, RDS_INFER_CACHE_MAX_ABSENT), ==, 1);
  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 1, 100, &cur, NULL));
  g_assert_cmpuint (cur.seq, !=, old.seq);
  g_assert_false (rds_infer_cache_complete (c, &old, r));
  g_assert_false (rds_infer_cache_cancel (c, &old, NULL));
  complete (c, &cur, 2);
  g_assert_false (rds_infer_cache_complete (c, &cur, r));
  /* interval 0: due again (no copy), and the PENDING entry serves the stored 2 */
  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 1, 100, &cur, &v));
  g_assert_null (v);
  g_assert_false (rds_infer_cache_lookup_or_add (c, 0, 1, 100, &old, &v));
  g_assert_cmpint (class_of (v), ==, 2);
  rds_infer_classifier_result_free (v);
  complete (c, &cur, 2);

  /* flushed: the same key comes back in the new epoch */
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 5, 100, &old, NULL));
  rds_infer_cache_flush (c);
  g_assert_false (rds_infer_cache_complete (c, &old, r));
  rds_infer_cache_begin_frame (c, 0);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 0, 5, 100, &cur, &v));
  g_assert_null (v);
  g_assert_cmpuint (cur.epoch, !=, old.epoch);
  g_assert_false (rds_infer_cache_complete (c, &old, r));
  /* an old request with the current seq but the old epoch: dropped by the epoch alone */
  old.seq = cur.seq;
  g_assert_false (rds_infer_cache_complete (c, &old, r));
  g_assert_false (rds_infer_cache_cancel (c, &old, NULL));
  complete (c, &cur, 0);

  /* source cleared */
  rds_infer_cache_begin_frame (c, 3);
  g_assert_true (rds_infer_cache_lookup_or_add (c, 3, 5, 100, &old, NULL));
  g_assert_cmpuint (rds_infer_cache_clear_source (c, 3), ==, 1);
  g_assert_false (rds_infer_cache_complete (c, &old, r));
  /* (0, 5) only: the flush took (0, 1) */
  g_assert_cmpuint (rds_infer_cache_get_size (c), ==, 1);
  rds_infer_classifier_result_free (r);
  rds_infer_cache_free (c);
}

#define CONCURRENT_IDS 64
#define CONCURRENT_ROUNDS 2000

typedef struct
{
  RdsInferCache *cache;
  GAsyncQueue *requests;        /* RdsInferCacheRequest *; the Worker itself ends */
  guint stored, cancelled;
} Worker;

/* Completes every other request and cancels the others (taking the stored result back, as the element does on a
 * parser failure). */
static gpointer
worker_thread (gpointer data)
{
  Worker *w = data;
  RdsInferCacheRequest *req;
  RdsInferClassifierResult *r = make_result (1);
  guint n = 0;

  while ((req = g_async_queue_pop (w->requests)) != (gpointer) w) {
    if (n++ % 2) {
      RdsInferClassifierResult *v = NULL;

      if (rds_infer_cache_cancel (w->cache, req, &v))
        w->cancelled++;
      if (v)
        g_assert_cmpuint (v->labels->len, ==, 1);
      rds_infer_classifier_result_free (v);
    } else if (rds_infer_cache_complete (w->cache, req, r)) {
      w->stored++;
    }
    g_free (req);
  }
  rds_infer_classifier_result_free (r);
  return NULL;
}

/* Completions and cancels from a second thread (as an async worker would) while this one looks up, touches, evicts,
 * clears and flushes: no deadlock, the copies handed out are whole (ASan checks them), and afterwards no entry is
 * left stuck PENDING: with interval 2 every id is due within three frames. */
static void
test_concurrent (void)
{
  Worker w = {.cache = rds_infer_cache_new (2),.requests = g_async_queue_new () };
  GThread *t = g_thread_new ("cache-worker", worker_thread, &w);
  guint sent = 0, survivors;
  gboolean due[CONCURRENT_IDS] = { FALSE };

  for (guint64 f = 0; f < CONCURRENT_ROUNDS; f++) {
    rds_infer_cache_begin_frame (w.cache, 0);
    rds_infer_cache_begin_frame (w.cache, 1);
    for (guint64 id = 0; id < CONCURRENT_IDS; id++) {
      RdsInferCacheRequest req = { 0 };
      RdsInferClassifierResult *v = NULL;

      /* ids leave and come back, so some are evicted with requests out; some are present but not selected */
      if ((id + f / 50) % 5 == 0)
        continue;
      if ((id + f) % 7 == 0) {
        rds_infer_cache_touch (w.cache, (guint) (id % 2), id);
        continue;
      }
      if (rds_infer_cache_lookup_or_add (w.cache, (guint) (id % 2), id, 100.0 + (gdouble) (f % 7), &req, &v)) {
        g_async_queue_push (w.requests, g_memdup2 (&req, sizeof (req)));
        sent++;
      }
      if (v)
        g_assert_cmpuint (v->labels->len, ==, 1);
      rds_infer_classifier_result_free (v);
    }
    rds_infer_cache_evict_absent (w.cache, RDS_INFER_CACHE_MAX_ABSENT);
    if (f % 333 == 0)
      rds_infer_cache_clear_source (w.cache, 1);
    if (f % 777 == 0)
      rds_infer_cache_flush (w.cache);
  }
  g_async_queue_push (w.requests, &w);
  g_thread_join (t);
  g_test_message ("%u requests: %u stored, %u cancelled", sent, w.stored, w.cancelled);
  g_assert_cmpuint (w.stored, >, 0);
  g_assert_cmpuint (w.cancelled, >, 0);

  /* every request was answered: no entry may stay PENDING, so each surviving id is due within three frames */
  survivors = rds_infer_cache_get_size (w.cache);
  g_assert_cmpuint (survivors, >, 0);
  for (guint f = 0; f < 3; f++) {
    rds_infer_cache_begin_frame (w.cache, 0);
    rds_infer_cache_begin_frame (w.cache, 1);
    for (guint64 id = 0; id < CONCURRENT_IDS; id++) {
      RdsInferCacheRequest req = { 0 };

      if (!due[id] && rds_infer_cache_lookup_or_add (w.cache, (guint) (id % 2), id, 100, &req, NULL)) {
        due[id] = TRUE;
        complete (w.cache, &req, 1);
      }
    }
  }
  for (guint64 id = 0; id < CONCURRENT_IDS; id++) {
    if (!due[id])
      g_test_message ("id %" G_GUINT64_FORMAT " never due: stuck PENDING", id);
    g_assert_true (due[id]);
  }
  g_async_queue_unref (w.requests);
  rds_infer_cache_free (w.cache);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/infer-cache/new-id", test_new_id);
  g_test_add_func ("/cpu/infer-cache/growth", test_growth);
  g_test_add_func ("/cpu/infer-cache/interval", test_interval);
  g_test_add_func ("/cpu/infer-cache/empty-result", test_empty_result);
  g_test_add_func ("/cpu/infer-cache/pending-cancel", test_pending_cancel);
  g_test_add_func ("/cpu/infer-cache/eviction", test_eviction);
  g_test_add_func ("/cpu/infer-cache/eviction-per-source", test_eviction_per_source);
  g_test_add_func ("/cpu/infer-cache/touch", test_touch);
  g_test_add_func ("/cpu/infer-cache/clear-flush", test_clear_flush);
  g_test_add_func ("/cpu/infer-cache/stale", test_stale);
  g_test_add_func ("/cpu/infer-cache/concurrent", test_concurrent);
  return g_test_run ();
}
