/* Redstream: rdsinfer's per-object result cache for secondary mode (see rds-infer-cache.h).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-cache.h"

typedef enum
{
  ENTRY_EMPTY,
  ENTRY_PENDING,
  ENTRY_READY,
} EntryState;

typedef struct
{
  guint source_id;
  guint64 object_id;
} Key;

typedef struct
{
  Key key;                      /* the hash table's key points here */
  EntryState state;
  gboolean has_result;          /* whether a completion stored one; result may still be NULL (a detector) */
  guint64 seq;                  /* of the last request */
  RdsInferClassifierResult *result;     /* owned, NULL for none */
  gdouble area;                 /* at the last inference */
  guint64 selected;             /* frames the object was selected in, this one included */
  guint64 inferred_at;          /* the value of selected at the last inference */
  gdouble pending_area;         /* of the request out */
  guint64 pending_at;
  guint64 last_seen;            /* its source's frame counter */
} Entry;

struct _RdsInferCache
{
  GMutex lock;
  GHashTable *entries;          /* Key * -> Entry *, owned */
  GHashTable *frames;           /* source_id -> guint64 *, owned: the frames of that source begun */
  gint interval;
  guint64 next_seq, epoch;
};

static guint
key_hash (gconstpointer p)
{
  const Key *k = p;

  return (guint) (k->object_id ^ (k->object_id >> 32)) * 31u + k->source_id;
}

static gboolean
key_equal (gconstpointer a, gconstpointer b)
{
  const Key *x = a, *y = b;

  return x->source_id == y->source_id && x->object_id == y->object_id;
}

static void
entry_free (gpointer p)
{
  Entry *e = p;

  rds_infer_classifier_result_free (e->result);
  g_free (e);
}

RdsInferCache *
rds_infer_cache_new (gint reinfer_interval)
{
  RdsInferCache *cache = g_new0 (RdsInferCache, 1);

  g_mutex_init (&cache->lock);
  cache->entries = g_hash_table_new_full (key_hash, key_equal, NULL, entry_free);
  cache->frames = g_hash_table_new_full (NULL, NULL, NULL, g_free);
  cache->interval = reinfer_interval;
  return cache;
}

void
rds_infer_cache_free (RdsInferCache * cache)
{
  if (!cache)
    return;
  g_hash_table_unref (cache->entries);
  g_hash_table_unref (cache->frames);
  g_mutex_clear (&cache->lock);
  g_free (cache);
}

/* @source_id's frame counter; cache lock held. */
static guint64
source_frame (const RdsInferCache * cache, guint source_id)
{
  const guint64 *f = g_hash_table_lookup (cache->frames, GUINT_TO_POINTER (source_id));

  return f ? *f : 0;
}

guint64
rds_infer_cache_begin_frame (RdsInferCache * cache, guint source_id)
{
  guint64 *f, now;

  g_mutex_lock (&cache->lock);
  if (!(f = g_hash_table_lookup (cache->frames, GUINT_TO_POINTER (source_id)))) {
    f = g_new0 (guint64, 1);
    g_hash_table_insert (cache->frames, GUINT_TO_POINTER (source_id), f);
  }
  now = ++*f;
  g_mutex_unlock (&cache->lock);
  return now;
}

/* Whether a READY entry is due on this lookup (its selected count already counts it). */
static gboolean
ready_due (const RdsInferCache * cache, const Entry * e, gdouble area)
{
  /* area > 1.2 x the last one, exactly: 5 and 6 times a float product are exact in double */
  if (5.0 * area > 6.0 * e->area)
    return TRUE;
  return cache->interval >= 0 && e->selected - e->inferred_at >= (guint64) cache->interval + 1;
}

gboolean
rds_infer_cache_lookup_or_add (RdsInferCache * cache, guint source_id, guint64 object_id, gdouble area,
    RdsInferCacheRequest * req, RdsInferClassifierResult ** values)
{
  Key key = {.source_id = source_id,.object_id = object_id };
  gboolean due;
  Entry *e;

  g_mutex_lock (&cache->lock);
  e = g_hash_table_lookup (cache->entries, &key);
  if (!e) {
    e = g_new0 (Entry, 1);
    e->key = key;
    e->state = ENTRY_EMPTY;
    g_hash_table_insert (cache->entries, &e->key, e);
  } else {
    e->selected++;
  }
  e->last_seen = source_frame (cache, source_id);
  switch (e->state) {
    case ENTRY_PENDING:
      due = FALSE;
      break;
    case ENTRY_EMPTY:
      due = TRUE;
      break;
    default:
      due = ready_due (cache, e, area);
      break;
  }
  if (due) {
    e->state = ENTRY_PENDING;
    e->seq = ++cache->next_seq;
    e->pending_area = area;
    e->pending_at = e->selected;
    req->source_id = source_id;
    req->object_id = object_id;
    req->seq = e->seq;
    req->epoch = cache->epoch;
  }
  if (values)
    *values = !due && e->result ? rds_infer_classifier_result_copy (e->result) : NULL;
  g_mutex_unlock (&cache->lock);
  return due;
}

void
rds_infer_cache_touch (RdsInferCache * cache, guint source_id, guint64 object_id)
{
  Key key = {.source_id = source_id,.object_id = object_id };
  Entry *e;

  g_mutex_lock (&cache->lock);
  if ((e = g_hash_table_lookup (cache->entries, &key)))
    e->last_seen = source_frame (cache, source_id);
  g_mutex_unlock (&cache->lock);
}

/* The entry of @req if the request is still out (same entry, seq and epoch, PENDING); cache lock held. */
static Entry *
pending_entry (RdsInferCache * cache, const RdsInferCacheRequest * req)
{
  Key key = {.source_id = req->source_id,.object_id = req->object_id };
  Entry *e;

  if (req->epoch != cache->epoch)
    return NULL;
  e = g_hash_table_lookup (cache->entries, &key);
  return e && e->state == ENTRY_PENDING && e->seq == req->seq ? e : NULL;
}

gboolean
rds_infer_cache_complete (RdsInferCache * cache, const RdsInferCacheRequest * req,
    const RdsInferClassifierResult * values)
{
  RdsInferClassifierResult *copy = values ? rds_infer_classifier_result_copy (values) : NULL, *old = NULL;
  Entry *e;

  g_mutex_lock (&cache->lock);
  if ((e = pending_entry (cache, req))) {
    old = e->result;
    e->result = copy;
    copy = NULL;
    e->has_result = TRUE;
    e->state = ENTRY_READY;
    e->area = e->pending_area;
    e->inferred_at = e->pending_at;
  }
  g_mutex_unlock (&cache->lock);
  /* copies made and freed outside the lock */
  rds_infer_classifier_result_free (old);
  rds_infer_classifier_result_free (copy);
  return e != NULL;
}

gboolean
rds_infer_cache_cancel (RdsInferCache * cache, const RdsInferCacheRequest * req, RdsInferClassifierResult ** values)
{
  Entry *e;

  g_mutex_lock (&cache->lock);
  if ((e = pending_entry (cache, req)))
    e->state = e->has_result ? ENTRY_READY : ENTRY_EMPTY;
  if (values)
    *values = e && e->result ? rds_infer_classifier_result_copy (e->result) : NULL;
  g_mutex_unlock (&cache->lock);
  return e != NULL;
}

typedef struct
{
  const RdsInferCache *cache;
  guint64 max_absent;
  guint source_id;
} Match;

static gboolean
is_absent (gpointer key G_GNUC_UNUSED, gpointer value, gpointer data)
{
  const Entry *e = value;
  const Match *m = data;

  /* last_seen is never ahead: a source's counter only restarts when its entries go too */
  return source_frame (m->cache, e->key.source_id) - e->last_seen >= m->max_absent;
}

static gboolean
is_of_source (gpointer key G_GNUC_UNUSED, gpointer value, gpointer data)
{
  return ((const Entry *) value)->key.source_id == ((const Match *) data)->source_id;
}

guint
rds_infer_cache_evict_absent (RdsInferCache * cache, guint64 max_absent)
{
  Match m = {.cache = cache,.max_absent = max_absent };
  guint n;

  g_mutex_lock (&cache->lock);
  n = g_hash_table_foreach_remove (cache->entries, is_absent, &m);
  g_mutex_unlock (&cache->lock);
  return n;
}

guint
rds_infer_cache_clear_source (RdsInferCache * cache, guint source_id)
{
  Match m = {.source_id = source_id };
  guint n;

  g_mutex_lock (&cache->lock);
  n = g_hash_table_foreach_remove (cache->entries, is_of_source, &m);
  g_hash_table_remove (cache->frames, GUINT_TO_POINTER (source_id));
  g_mutex_unlock (&cache->lock);
  return n;
}

void
rds_infer_cache_flush (RdsInferCache * cache)
{
  g_mutex_lock (&cache->lock);
  g_hash_table_remove_all (cache->entries);
  g_hash_table_remove_all (cache->frames);
  cache->epoch++;
  g_mutex_unlock (&cache->lock);
}

guint
rds_infer_cache_get_size (RdsInferCache * cache)
{
  guint n;

  g_mutex_lock (&cache->lock);
  n = g_hash_table_size (cache->entries);
  g_mutex_unlock (&cache->lock);
  return n;
}
