/* Redstream: batch, frame, object, classifier, label, display and user metadata: per-batch pools, add/remove,
 * release and teardown, and user meta types. Deep copies live in rds-meta-copy.c.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>

#include "rds-meta-private.h"

GST_DEBUG_CATEGORY (rds_meta_debug);
#define GST_CAT_DEFAULT rds_meta_debug

/* A release_func that keeps adding metas to the batch during teardown would loop forever; stop after this many
 * passes and let the pool sweep release what is left. */
#define MAX_DESTROY_PASSES 16

/* Metadata can be used without any element, so the category is created lazily. */
void
rds_meta_ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (rds_meta_debug, "rdsmeta", 0, "Redstream metadata");
    g_once_init_leave (&initialized, 1);
  }
}

/* Every element starts with an RdsBaseMeta, after its hidden header. The pool owns all elements it ever allocated
 * (all), so metas acquired but never added are still freed with the batch; free holds the released ones, zeroed
 * and ready for reuse. An element is in use exactly when its base_meta.batch_meta is set. Both arrays hold header
 * pointers. Pools are only touched under the batch's meta_mutex. */
struct _RdsMetaPool
{
  RdsMetaType meta_type;
  gsize element_size;
  GPtrArray *all;               /* owns the allocations */
  GPtrArray *free;              /* subset of all */
};

static RdsMetaElemHeader *
elem_new (gsize element_size)
{
  RdsMetaElemHeader *hdr = g_malloc0 (sizeof (RdsMetaElemHeader) + element_size);

  hdr->f.magic = RDS_META_ELEM_MAGIC;
  return hdr;
}

static RdsMetaPool *
pool_new (RdsMetaType meta_type, gsize element_size, guint prealloc)
{
  RdsMetaPool *pool = g_new0 (RdsMetaPool, 1);

  pool->meta_type = meta_type;
  pool->element_size = element_size;
  pool->all = g_ptr_array_new_full (prealloc, g_free);
  pool->free = g_ptr_array_sized_new (prealloc);
  for (guint i = 0; i < prealloc; i++) {
    RdsMetaElemHeader *hdr = elem_new (element_size);

    g_ptr_array_add (pool->all, hdr);
    g_ptr_array_add (pool->free, hdr);
  }
  return pool;
}

gpointer
rds_meta_pool_acquire (RdsMetaPool * pool, RdsBatchMeta * batch_meta)
{
  RdsMetaElemHeader *hdr;
  RdsBaseMeta *base;

  if (pool->free->len > 0) {
    hdr = g_ptr_array_steal_index_fast (pool->free, pool->free->len - 1);
  } else {
    hdr = elem_new (pool->element_size);
    g_ptr_array_add (pool->all, hdr);
  }
  base = (RdsBaseMeta *) (hdr + 1);
  base->batch_meta = batch_meta;
  base->meta_type = pool->meta_type;
  return base;
}

/* Owned members must already be freed (the release helpers). */
static void
pool_release (RdsMetaPool * pool, gpointer elem)
{
  RdsMetaElemHeader *hdr = RDS_META_ELEM_HEADER (elem);

  memset (hdr, 0, sizeof (RdsMetaElemHeader) + pool->element_size);
  hdr->f.magic = RDS_META_ELEM_MAGIC;
  g_ptr_array_add (pool->free, hdr);
}

static void
pool_free (RdsMetaPool * pool)
{
  g_ptr_array_free (pool->free, TRUE);
  g_ptr_array_free (pool->all, TRUE);
  g_free (pool);
}

/* Release helpers. Each list is detached from its owner before it is walked, so a release_func that calls back
 * into the API cannot change the list being iterated. */

void
rds_meta_release_label (RdsBatchMeta * b, RdsLabelInfo * label)
{
  g_free (label->pResult_label);
  pool_release (b->label_info_meta_pool, label);
}

void
rds_meta_release_classifier (RdsBatchMeta * b, RdsClassifierMeta * classifier)
{
  GList *labels = classifier->label_info_list;

  classifier->label_info_list = NULL;
  for (GList * l = labels; l != NULL; l = l->next)
    rds_meta_release_label (b, l->data);
  g_list_free (labels);
  pool_release (b->classifier_meta_pool, classifier);
}

void
rds_meta_release_user (RdsBatchMeta * b, RdsUserMeta * user)
{
  if (user->base_meta.release_func != NULL)
    user->base_meta.release_func (user, user->base_meta.uContext);
  pool_release (b->user_meta_pool, user);
}

/* Takes the list from *list_field (detaching it) and releases its user metas. */
static void
release_user_list (RdsBatchMeta * b, GList ** list_field)
{
  GList *list = *list_field;

  *list_field = NULL;
  for (GList * l = list; l != NULL; l = l->next)
    rds_meta_release_user (b, l->data);
  g_list_free (list);
}

void
rds_meta_release_display (RdsBatchMeta * b, RdsDisplayMeta * display)
{
  /* Every slot, not just i < num_labels, so text set beyond the count does not leak. */
  for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
    g_free (display->text_params[i].display_text);
  pool_release (b->display_meta_pool, display);
}

void
rds_meta_release_obj (RdsBatchMeta * b, RdsObjectMeta * obj)
{
  GList *classifiers = obj->classifier_meta_list;

  if (RDS_META_ELEM_HEADER (obj)->f.parented)
    RDS_BATCH_PRIV (b)->n_parented--;
  obj->classifier_meta_list = NULL;
  for (GList * l = classifiers; l != NULL; l = l->next)
    rds_meta_release_classifier (b, l->data);
  g_list_free (classifiers);
  release_user_list (b, &obj->obj_user_meta_list);
  g_free (obj->text_params.display_text);
  g_free (obj->mask_params.data);
  pool_release (b->obj_meta_pool, obj);
}

void
rds_meta_release_frame (RdsBatchMeta * b, RdsFrameMeta * frame)
{
  GList *objs = frame->obj_meta_list, *displays = frame->display_meta_list;

  frame->obj_meta_list = NULL;
  frame->display_meta_list = NULL;
  for (GList * l = objs; l != NULL; l = l->next)
    rds_meta_release_obj (b, l->data);
  g_list_free (objs);
  for (GList * l = displays; l != NULL; l = l->next)
    rds_meta_release_display (b, l->data);
  g_list_free (displays);
  release_user_list (b, &frame->frame_user_meta_list);
  pool_release (b->frame_meta_pool, frame);
}

/* Sets parent = NULL on every object in frame whose parent is gone (in the set, or equal to gone). */
static void
clear_parents_in_frame (RdsBatchMeta * b, RdsFrameMeta * frame, RdsObjectMeta * gone, GHashTable * gone_set)
{
  for (GList * l = frame->obj_meta_list; l != NULL; l = l->next) {
    RdsObjectMeta *o = l->data;

    if (o->parent == NULL)
      continue;
    if (o->parent == gone || (gone_set != NULL && g_hash_table_contains (gone_set, o->parent)))
      rds_meta_set_parent (b, o, NULL);
  }
}

/* Scans the batch's frames, plus extra if it was not added. Frames that were never added are not scanned
 * otherwise (documented in rds-meta.h). */
static void
clear_parents (RdsBatchMeta * b, RdsFrameMeta * extra, RdsObjectMeta * gone, GHashTable * gone_set)
{
  gboolean extra_seen = FALSE;

  /* Parents are only set through rds_add_obj_meta_to_frame (documented), so no counted object means no
   * object can point at the removed one(s). */
  if (RDS_BATCH_PRIV (b)->n_parented == 0)
    return;
  for (GList * l = b->frame_meta_list; l != NULL; l = l->next) {
    clear_parents_in_frame (b, l->data, gone, gone_set);
    extra_seen |= (l->data == (gpointer) extra);
  }
  if (extra != NULL && !extra_seen)
    clear_parents_in_frame (b, extra, gone, gone_set);
}

void
rds_meta_set_parent (RdsBatchMeta * b, RdsObjectMeta * obj, RdsObjectMeta * parent)
{
  RdsMetaElemHeader *hdr = RDS_META_ELEM_HEADER (obj);

  obj->parent = parent;
  if (parent != NULL && !hdr->f.parented) {
    hdr->f.parented = TRUE;
    RDS_BATCH_PRIV (b)->n_parented++;
  } else if (parent == NULL && hdr->f.parented) {
    hdr->f.parented = FALSE;
    RDS_BATCH_PRIV (b)->n_parented--;
  }
}

gboolean
rds_meta_attach (RdsBatchMeta * b, GList ** list, gpointer meta, const gchar * what, gconstpointer owner)
{
  RdsBaseMeta *base = meta;
  RdsMetaElemHeader *hdr;

  if (base->batch_meta != b) {
    GST_WARNING ("%s meta %p belongs to batch %p, not to %p's batch %p; not added", what, meta, base->batch_meta,
        owner, b);
    return FALSE;
  }
  hdr = RDS_META_ELEM_HEADER (meta);
  if (hdr->f.magic != RDS_META_ELEM_MAGIC) {
    GST_WARNING ("%s meta %p was not acquired from a pool; not added to %p", what, meta, owner);
    return FALSE;
  }
  if (hdr->f.attached) {
    GST_WARNING ("%s meta %p is already attached; not added to %p", what, meta, owner);
    return FALSE;
  }
  hdr->f.attached = TRUE;
  /* Append: insertion order is part of the contract. */
  *list = g_list_append (*list, meta);
  return TRUE;
}

gboolean
rds_meta_detach (GList ** list, gpointer meta, const gchar * what, gconstpointer owner)
{
  GList *link = g_list_find (*list, meta);

  if (link == NULL) {
    GST_WARNING ("%s meta %p is not in %p; not removed", what, meta, owner);
    return FALSE;
  }
  *list = g_list_delete_link (*list, link);
  return TRUE;
}

/* The batch an owner meta belongs to, or NULL (with a warning) for a released or foreign one. */
static RdsBatchMeta *
owner_batch (gconstpointer owner)
{
  RdsBatchMeta *b = ((const RdsBaseMeta *) owner)->batch_meta;

  if (b == NULL)
    GST_WARNING ("meta %p has no batch (released?); ignored", owner);
  return b;
}

static gpointer
acquire_from (RdsBatchMeta * b, RdsMetaPool * pool)
{
  gpointer meta;

  g_rec_mutex_lock (&b->meta_mutex);
  meta = rds_meta_pool_acquire (pool, b);
  g_rec_mutex_unlock (&b->meta_mutex);
  return meta;
}

/* Releases every element of pool still in use (batch_meta set): metas acquired but never added, and children
 * of such metas when the parent pool was swept first. Elements acquired during the sweep (by a release_func)
 * are left to pool_free. */
typedef void (*ReleaseFunc) (RdsBatchMeta * b, gpointer meta);

static void
sweep_release_frame (RdsBatchMeta * b, gpointer meta)
{
  rds_meta_release_frame (b, meta);
}

static void
sweep_release_obj (RdsBatchMeta * b, gpointer meta)
{
  rds_meta_release_obj (b, meta);
}

static void
sweep_release_classifier (RdsBatchMeta * b, gpointer meta)
{
  rds_meta_release_classifier (b, meta);
}

static void
sweep_release_label (RdsBatchMeta * b, gpointer meta)
{
  rds_meta_release_label (b, meta);
}

static void
sweep_release_display (RdsBatchMeta * b, gpointer meta)
{
  rds_meta_release_display (b, meta);
}

static void
sweep_release_user (RdsBatchMeta * b, gpointer meta)
{
  rds_meta_release_user (b, meta);
}

static void
sweep_pool (RdsBatchMeta * b, RdsMetaPool * pool, ReleaseFunc release)
{
  guint n = pool->all->len;

  for (guint i = 0; i < n; i++) {
    RdsBaseMeta *base = (RdsBaseMeta *) ((RdsMetaElemHeader *) g_ptr_array_index (pool->all, i) + 1);

    if (base->batch_meta != NULL) {
      GST_DEBUG ("releasing meta %p (type %d) that was never added", base, base->meta_type);
      release (b, base);
    }
  }
}

RdsBatchMeta *
rds_create_batch_meta (guint max_batch_size)
{
  RdsBatchMeta *self = &g_new0 (RdsBatchMetaPriv, 1)->pub;

  rds_meta_ensure_debug_category ();
  self->base_meta.batch_meta = self;
  self->base_meta.meta_type = RDS_BATCH_META;
  self->max_frames_in_batch = max_batch_size;
  g_rec_mutex_init (&self->meta_mutex);

  /* Pools grow on demand (D8). Only frames are preallocated, one per batch slot: the other pools start empty,
   * since batches are often created and destroyed per buffer (a deep copy per make_writable) and most of them
   * never use every pool. */
  self->frame_meta_pool = pool_new (RDS_FRAME_META, sizeof (RdsFrameMeta), max_batch_size);
  self->obj_meta_pool = pool_new (RDS_OBJ_META, sizeof (RdsObjectMeta), 0);
  self->classifier_meta_pool = pool_new (RDS_CLASSIFIER_META, sizeof (RdsClassifierMeta), 0);
  self->label_info_meta_pool = pool_new (RDS_LABEL_INFO_META, sizeof (RdsLabelInfo), 0);
  self->display_meta_pool = pool_new (RDS_DISPLAY_META, sizeof (RdsDisplayMeta), 0);
  self->user_meta_pool = pool_new (RDS_USER_META, sizeof (RdsUserMeta), 0);

  GST_DEBUG ("created batch meta %p, max %u frames", self, max_batch_size);
  return self;
}

void
rds_destroy_batch_meta (RdsBatchMeta * batch_meta)
{
  guint pass = 0;

  if (batch_meta == NULL)
    return;

  g_rec_mutex_lock (&batch_meta->meta_mutex);
  /* Loop: a release_func that adds to the batch-level lists (unsupported but tolerated) must not leak. */
  while (batch_meta->frame_meta_list != NULL || batch_meta->batch_user_meta_list != NULL) {
    GList *frames = batch_meta->frame_meta_list;

    if (pass++ == MAX_DESTROY_PASSES) {
      GST_WARNING ("batch %p: release functions keep adding metas during destroy; giving up after %u passes",
          batch_meta, MAX_DESTROY_PASSES);
      break;
    }
    batch_meta->frame_meta_list = NULL;
    batch_meta->num_frames_in_batch = 0;
    for (GList * l = frames; l != NULL; l = l->next)
      rds_meta_release_frame (batch_meta, l->data);
    g_list_free (frames);
    release_user_list (batch_meta, &batch_meta->batch_user_meta_list);
  }
  /* After a capped loop the lists still hold metas; drop the links, the sweep below releases the metas. */
  g_clear_pointer (&batch_meta->frame_meta_list, g_list_free);
  g_clear_pointer (&batch_meta->batch_user_meta_list, g_list_free);

  /* Sweep, parents before children, so a meta that still has an owner is released through it. */
  sweep_pool (batch_meta, batch_meta->frame_meta_pool, sweep_release_frame);
  sweep_pool (batch_meta, batch_meta->obj_meta_pool, sweep_release_obj);
  sweep_pool (batch_meta, batch_meta->classifier_meta_pool, sweep_release_classifier);
  sweep_pool (batch_meta, batch_meta->label_info_meta_pool, sweep_release_label);
  sweep_pool (batch_meta, batch_meta->display_meta_pool, sweep_release_display);
  sweep_pool (batch_meta, batch_meta->user_meta_pool, sweep_release_user);
  /* Links a release_func added during the sweep; their metas are freed with the pools. */
  g_clear_pointer (&batch_meta->frame_meta_list, g_list_free);
  g_clear_pointer (&batch_meta->batch_user_meta_list, g_list_free);
  g_rec_mutex_unlock (&batch_meta->meta_mutex);

  pool_free (batch_meta->frame_meta_pool);
  pool_free (batch_meta->obj_meta_pool);
  pool_free (batch_meta->classifier_meta_pool);
  pool_free (batch_meta->label_info_meta_pool);
  pool_free (batch_meta->display_meta_pool);
  pool_free (batch_meta->user_meta_pool);
  g_rec_mutex_clear (&batch_meta->meta_mutex);
  g_free (RDS_BATCH_PRIV (batch_meta));
}

void
rds_acquire_meta_lock (RdsBatchMeta * batch_meta)
{
  g_return_if_fail (batch_meta != NULL);
  g_rec_mutex_lock (&batch_meta->meta_mutex);
}

void
rds_release_meta_lock (RdsBatchMeta * batch_meta)
{
  g_return_if_fail (batch_meta != NULL);
  g_rec_mutex_unlock (&batch_meta->meta_mutex);
}

RdsFrameMeta *
rds_acquire_frame_meta_from_pool (RdsBatchMeta * batch_meta)
{
  RdsFrameMeta *frame;

  g_return_val_if_fail (batch_meta != NULL, NULL);
  g_rec_mutex_lock (&batch_meta->meta_mutex);
  frame = rds_meta_pool_acquire (batch_meta->frame_meta_pool, batch_meta);
  g_rec_mutex_unlock (&batch_meta->meta_mutex);
  return frame;
}

void
rds_add_frame_meta_to_batch (RdsBatchMeta * batch_meta, RdsFrameMeta * frame_meta)
{
  g_return_if_fail (batch_meta != NULL);
  g_return_if_fail (frame_meta != NULL);

  g_rec_mutex_lock (&batch_meta->meta_mutex);
  if (rds_meta_attach (batch_meta, &batch_meta->frame_meta_list, frame_meta, "frame", batch_meta)) {
    if (batch_meta->num_frames_in_batch >= batch_meta->max_frames_in_batch)
      GST_DEBUG ("batch %p: frame %u exceeds max_frames_in_batch %u", batch_meta,
          batch_meta->num_frames_in_batch + 1, batch_meta->max_frames_in_batch);
    batch_meta->num_frames_in_batch++;
  }
  g_rec_mutex_unlock (&batch_meta->meta_mutex);
}

void
rds_remove_frame_meta_from_batch (RdsBatchMeta * batch_meta, RdsFrameMeta * frame_meta)
{
  GList *link;

  g_return_if_fail (batch_meta != NULL);
  g_return_if_fail (frame_meta != NULL);

  g_rec_mutex_lock (&batch_meta->meta_mutex);
  link = g_list_find (batch_meta->frame_meta_list, frame_meta);
  if (link == NULL) {
    GST_WARNING ("frame meta %p is not in batch %p; not removed", frame_meta, batch_meta);
    g_rec_mutex_unlock (&batch_meta->meta_mutex);
    return;
  }
  batch_meta->frame_meta_list = g_list_delete_link (batch_meta->frame_meta_list, link);
  batch_meta->num_frames_in_batch--;

  if (frame_meta->obj_meta_list != NULL) {
    GHashTable *gone = g_hash_table_new (g_direct_hash, g_direct_equal);

    for (GList * l = frame_meta->obj_meta_list; l != NULL; l = l->next)
      g_hash_table_add (gone, l->data);
    clear_parents (batch_meta, NULL, NULL, gone);
    g_hash_table_unref (gone);
  }
  rds_meta_release_frame (batch_meta, frame_meta);
  g_rec_mutex_unlock (&batch_meta->meta_mutex);
}

RdsObjectMeta *
rds_acquire_obj_meta_from_pool (RdsBatchMeta * batch_meta)
{
  RdsObjectMeta *obj;

  g_return_val_if_fail (batch_meta != NULL, NULL);
  g_rec_mutex_lock (&batch_meta->meta_mutex);
  obj = rds_meta_pool_acquire (batch_meta->obj_meta_pool, batch_meta);
  obj->object_id = RDS_UNTRACKED_OBJECT_ID;
  g_rec_mutex_unlock (&batch_meta->meta_mutex);
  return obj;
}

void
rds_add_obj_meta_to_frame (RdsFrameMeta * frame_meta, RdsObjectMeta * obj_meta, RdsObjectMeta * obj_parent)
{
  RdsBatchMeta *b;

  g_return_if_fail (frame_meta != NULL);
  g_return_if_fail (obj_meta != NULL);

  if ((b = owner_batch (frame_meta)) == NULL)
    return;
  if (obj_parent != NULL && (obj_parent == obj_meta || obj_parent->base_meta.batch_meta != b)) {
    GST_WARNING ("object meta %p: invalid parent %p (itself or from another batch); parent dropped", obj_meta,
        obj_parent);
    obj_parent = NULL;
  }
  g_rec_mutex_lock (&b->meta_mutex);
  if (rds_meta_attach (b, &frame_meta->obj_meta_list, obj_meta, "object", frame_meta)) {
    rds_meta_set_parent (b, obj_meta, obj_parent);
    frame_meta->num_obj_meta++;
  }
  g_rec_mutex_unlock (&b->meta_mutex);
}

void
rds_remove_obj_meta_from_frame (RdsFrameMeta * frame_meta, RdsObjectMeta * obj_meta)
{
  RdsBatchMeta *b;
  GList *link;

  g_return_if_fail (frame_meta != NULL);
  g_return_if_fail (obj_meta != NULL);

  b = frame_meta->base_meta.batch_meta;
  if (b == NULL) {
    GST_WARNING ("frame meta %p has no batch; object %p not removed", frame_meta, obj_meta);
    return;
  }
  g_rec_mutex_lock (&b->meta_mutex);
  link = g_list_find (frame_meta->obj_meta_list, obj_meta);
  if (link == NULL) {
    GST_WARNING ("object meta %p is not in frame %p; not removed", obj_meta, frame_meta);
    g_rec_mutex_unlock (&b->meta_mutex);
    return;
  }
  frame_meta->obj_meta_list = g_list_delete_link (frame_meta->obj_meta_list, link);
  frame_meta->num_obj_meta--;
  clear_parents (b, frame_meta, obj_meta, NULL);
  rds_meta_release_obj (b, obj_meta);
  g_rec_mutex_unlock (&b->meta_mutex);
}

RdsClassifierMeta *
rds_acquire_classifier_meta_from_pool (RdsBatchMeta * batch_meta)
{
  g_return_val_if_fail (batch_meta != NULL, NULL);
  return acquire_from (batch_meta, batch_meta->classifier_meta_pool);
}

void
rds_add_classifier_meta_to_object (RdsObjectMeta * obj_meta, RdsClassifierMeta * classifier_meta)
{
  RdsBatchMeta *b;

  g_return_if_fail (obj_meta != NULL);
  g_return_if_fail (classifier_meta != NULL);
  if ((b = owner_batch (obj_meta)) == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  rds_meta_attach (b, &obj_meta->classifier_meta_list, classifier_meta, "classifier", obj_meta);
  g_rec_mutex_unlock (&b->meta_mutex);
}

void
rds_remove_classifier_meta_from_obj (RdsObjectMeta * obj_meta, RdsClassifierMeta * classifier_meta)
{
  RdsBatchMeta *b;

  g_return_if_fail (obj_meta != NULL);
  g_return_if_fail (classifier_meta != NULL);
  if ((b = owner_batch (obj_meta)) == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  if (rds_meta_detach (&obj_meta->classifier_meta_list, classifier_meta, "classifier", obj_meta))
    rds_meta_release_classifier (b, classifier_meta);
  g_rec_mutex_unlock (&b->meta_mutex);
}

RdsLabelInfo *
rds_acquire_label_info_meta_from_pool (RdsBatchMeta * batch_meta)
{
  g_return_val_if_fail (batch_meta != NULL, NULL);
  return acquire_from (batch_meta, batch_meta->label_info_meta_pool);
}

void
rds_add_label_info_meta_to_classifier (RdsClassifierMeta * classifier_meta, RdsLabelInfo * label_info_meta)
{
  RdsBatchMeta *b;

  g_return_if_fail (classifier_meta != NULL);
  g_return_if_fail (label_info_meta != NULL);
  if ((b = owner_batch (classifier_meta)) == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  if (rds_meta_attach (b, &classifier_meta->label_info_list, label_info_meta, "label info", classifier_meta))
    classifier_meta->num_labels++;
  g_rec_mutex_unlock (&b->meta_mutex);
}

void
rds_remove_label_info_meta_from_classifier (RdsClassifierMeta * classifier_meta, RdsLabelInfo * label_info_meta)
{
  RdsBatchMeta *b;

  g_return_if_fail (classifier_meta != NULL);
  g_return_if_fail (label_info_meta != NULL);
  if ((b = owner_batch (classifier_meta)) == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  if (rds_meta_detach (&classifier_meta->label_info_list, label_info_meta, "label info", classifier_meta)) {
    classifier_meta->num_labels--;
    rds_meta_release_label (b, label_info_meta);
  }
  g_rec_mutex_unlock (&b->meta_mutex);
}

RdsDisplayMeta *
rds_acquire_display_meta_from_pool (RdsBatchMeta * batch_meta)
{
  g_return_val_if_fail (batch_meta != NULL, NULL);
  return acquire_from (batch_meta, batch_meta->display_meta_pool);
}

void
rds_add_display_meta_to_frame (RdsFrameMeta * frame_meta, RdsDisplayMeta * display_meta)
{
  RdsBatchMeta *b;

  g_return_if_fail (frame_meta != NULL);
  g_return_if_fail (display_meta != NULL);
  if ((b = owner_batch (frame_meta)) == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  rds_meta_attach (b, &frame_meta->display_meta_list, display_meta, "display", frame_meta);
  g_rec_mutex_unlock (&b->meta_mutex);
}

void
rds_remove_display_meta_from_frame (RdsFrameMeta * frame_meta, RdsDisplayMeta * display_meta)
{
  RdsBatchMeta *b;

  g_return_if_fail (frame_meta != NULL);
  g_return_if_fail (display_meta != NULL);
  if ((b = owner_batch (frame_meta)) == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  if (rds_meta_detach (&frame_meta->display_meta_list, display_meta, "display", frame_meta))
    rds_meta_release_display (b, display_meta);
  g_rec_mutex_unlock (&b->meta_mutex);
}

RdsUserMeta *
rds_acquire_user_meta_from_pool (RdsBatchMeta * batch_meta)
{
  g_return_val_if_fail (batch_meta != NULL, NULL);
  return acquire_from (batch_meta, batch_meta->user_meta_pool);
}

/* owner is the batch, frame or object whose user list is *list. */
static void
add_user_meta (gconstpointer owner, GList ** list, RdsUserMeta * user_meta)
{
  RdsBatchMeta *b = owner_batch (owner);

  if (b == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  rds_meta_attach (b, list, user_meta, "user", owner);
  g_rec_mutex_unlock (&b->meta_mutex);
}

static void
remove_user_meta (gconstpointer owner, GList ** list, RdsUserMeta * user_meta)
{
  RdsBatchMeta *b = owner_batch (owner);

  if (b == NULL)
    return;
  g_rec_mutex_lock (&b->meta_mutex);
  if (rds_meta_detach (list, user_meta, "user", owner))
    rds_meta_release_user (b, user_meta);
  g_rec_mutex_unlock (&b->meta_mutex);
}

void
rds_add_user_meta_to_batch (RdsBatchMeta * batch_meta, RdsUserMeta * user_meta)
{
  g_return_if_fail (batch_meta != NULL);
  g_return_if_fail (user_meta != NULL);
  add_user_meta (batch_meta, &batch_meta->batch_user_meta_list, user_meta);
}

void
rds_add_user_meta_to_frame (RdsFrameMeta * frame_meta, RdsUserMeta * user_meta)
{
  g_return_if_fail (frame_meta != NULL);
  g_return_if_fail (user_meta != NULL);
  add_user_meta (frame_meta, &frame_meta->frame_user_meta_list, user_meta);
}

void
rds_add_user_meta_to_obj (RdsObjectMeta * obj_meta, RdsUserMeta * user_meta)
{
  g_return_if_fail (obj_meta != NULL);
  g_return_if_fail (user_meta != NULL);
  add_user_meta (obj_meta, &obj_meta->obj_user_meta_list, user_meta);
}

void
rds_remove_user_meta_from_batch (RdsBatchMeta * batch_meta, RdsUserMeta * user_meta)
{
  g_return_if_fail (batch_meta != NULL);
  g_return_if_fail (user_meta != NULL);
  remove_user_meta (batch_meta, &batch_meta->batch_user_meta_list, user_meta);
}

void
rds_remove_user_meta_from_frame (RdsFrameMeta * frame_meta, RdsUserMeta * user_meta)
{
  g_return_if_fail (frame_meta != NULL);
  g_return_if_fail (user_meta != NULL);
  remove_user_meta (frame_meta, &frame_meta->frame_user_meta_list, user_meta);
}

void
rds_remove_user_meta_from_object (RdsObjectMeta * obj_meta, RdsUserMeta * user_meta)
{
  g_return_if_fail (obj_meta != NULL);
  g_return_if_fail (user_meta != NULL);
  remove_user_meta (obj_meta, &obj_meta->obj_user_meta_list, user_meta);
}

/* Process-lifetime table: interned descriptor -> type. Never freed (LSan sees it as reachable). */
G_LOCK_DEFINE_STATIC (user_meta_types);
static GHashTable *user_meta_types;
static gint next_user_meta_type = RDS_START_USER_META;

RdsMetaType
rds_get_user_meta_type (const gchar * meta_descriptor)
{
  const gchar *key;
  gpointer value;
  RdsMetaType type;

  g_return_val_if_fail (meta_descriptor != NULL, RDS_INVALID_META);

  rds_meta_ensure_debug_category ();
  key = g_intern_string (meta_descriptor);
  G_LOCK (user_meta_types);
  if (user_meta_types == NULL)
    user_meta_types = g_hash_table_new (g_direct_hash, g_direct_equal);
  if (g_hash_table_lookup_extended (user_meta_types, key, NULL, &value)) {
    type = (RdsMetaType) GPOINTER_TO_INT (value);
  } else {
    type = (RdsMetaType) next_user_meta_type++;
    g_hash_table_insert (user_meta_types, (gpointer) key, GINT_TO_POINTER (type));
    GST_DEBUG ("user meta type %d for \"%s\"", type, key);
  }
  G_UNLOCK (user_meta_types);
  return type;
}
