/* Redstream: batch metadata and its pools.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include <gst/gst.h>

#include "rds-meta.h"

GST_DEBUG_CATEGORY_STATIC (rds_meta_debug);
#define GST_CAT_DEFAULT rds_meta_debug

/* Metadata can be used without any element, so the category is created lazily. */
static void
ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (rds_meta_debug, "rdsmeta", 0, "Redstream metadata");
    g_once_init_leave (&initialized, 1);
  }
}

/* Hidden per-element header, allocated just before every pooled meta. 16 bytes keep the meta itself aligned as
 * g_malloc would. */
typedef union
{
  struct
  {
    gboolean attached;          /* in a list: refuses double adds and adds to a second list */
    gboolean parented;          /* an object added with a parent: counted in n_parented */
  } f;
  guint8 pad[16];
} RdsMetaElemHeader;

G_STATIC_ASSERT (sizeof (RdsMetaElemHeader) % 16 == 0);

#define ELEM_HEADER(elem) (((RdsMetaElemHeader *) (elem)) - 1)

/* Private batch: the public struct comes first, so RdsBatchMeta * casts to it. */
typedef struct
{
  RdsBatchMeta pub;
  /* Attached objects added with a non-NULL parent that is still set. While it is 0, removals skip the
   * batch-wide parent scan. */
  guint n_parented;
} RdsBatchMetaPriv;

#define BATCH_PRIV(b) ((RdsBatchMetaPriv *) (b))

/* Every element starts with an RdsBaseMeta, after its hidden header. The pool owns all elements it ever allocated
 * (all), so metas acquired but never added are still freed with the batch; free holds the released ones, zeroed
 * and ready for reuse. Both arrays hold header pointers. Pools are only touched under the batch's meta_mutex. */
struct _RdsMetaPool
{
  RdsMetaType meta_type;
  gsize element_size;
  GPtrArray *all;               /* owns the allocations */
  GPtrArray *free;              /* subset of all */
};

static RdsMetaPool *
pool_new (RdsMetaType meta_type, gsize element_size, guint prealloc)
{
  RdsMetaPool *pool = g_new0 (RdsMetaPool, 1);

  pool->meta_type = meta_type;
  pool->element_size = element_size;
  pool->all = g_ptr_array_new_full (prealloc, g_free);
  pool->free = g_ptr_array_sized_new (prealloc);
  for (guint i = 0; i < prealloc; i++) {
    gpointer hdr = g_malloc0 (sizeof (RdsMetaElemHeader) + element_size);

    g_ptr_array_add (pool->all, hdr);
    g_ptr_array_add (pool->free, hdr);
  }
  return pool;
}

static gpointer
pool_acquire (RdsMetaPool * pool, RdsBatchMeta * batch_meta)
{
  RdsMetaElemHeader *hdr;
  RdsBaseMeta *base;

  if (pool->free->len > 0) {
    hdr = g_ptr_array_steal_index_fast (pool->free, pool->free->len - 1);
  } else {
    hdr = g_malloc0 (sizeof (RdsMetaElemHeader) + pool->element_size);
    g_ptr_array_add (pool->all, hdr);
  }
  base = (RdsBaseMeta *) (hdr + 1);
  base->batch_meta = batch_meta;
  base->meta_type = pool->meta_type;
  return base;
}

/* Owned members must already be freed (the release_* helpers). */
static void
pool_release (RdsMetaPool * pool, gpointer elem)
{
  RdsMetaElemHeader *hdr = ELEM_HEADER (elem);

  memset (hdr, 0, sizeof (RdsMetaElemHeader) + pool->element_size);
  g_ptr_array_add (pool->free, hdr);
}

static void
pool_free (RdsMetaPool * pool)
{
  g_ptr_array_free (pool->free, TRUE);
  g_ptr_array_free (pool->all, TRUE);
  g_free (pool);
}

/* Release helpers: free owned members and children, then return the element to its pool. Callers unlink first
 * and hold the lock. Each list is detached from its owner before it is walked, so a release_func that calls back
 * into the API cannot change the list being iterated. */

static void
release_label (RdsBatchMeta * b, RdsLabelInfo * label)
{
  g_free (label->pResult_label);
  pool_release (b->label_info_meta_pool, label);
}

static void
release_classifier (RdsBatchMeta * b, RdsClassifierMeta * classifier)
{
  GList *labels = classifier->label_info_list;

  classifier->label_info_list = NULL;
  for (GList * l = labels; l != NULL; l = l->next)
    release_label (b, l->data);
  g_list_free (labels);
  pool_release (b->classifier_meta_pool, classifier);
}

static void
release_user (RdsBatchMeta * b, RdsUserMeta * user)
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
    release_user (b, l->data);
  g_list_free (list);
}

static void
release_display (RdsBatchMeta * b, RdsDisplayMeta * display)
{
  /* Every slot, not just i < num_labels, so text set beyond the count does not leak. */
  for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
    g_free (display->text_params[i].display_text);
  pool_release (b->display_meta_pool, display);
}

static void
release_obj (RdsBatchMeta * b, RdsObjectMeta * obj)
{
  GList *classifiers = obj->classifier_meta_list;

  if (ELEM_HEADER (obj)->f.parented)
    BATCH_PRIV (b)->n_parented--;
  obj->classifier_meta_list = NULL;
  for (GList * l = classifiers; l != NULL; l = l->next)
    release_classifier (b, l->data);
  g_list_free (classifiers);
  release_user_list (b, &obj->obj_user_meta_list);
  g_free (obj->text_params.display_text);
  g_free (obj->mask_params.data);
  pool_release (b->obj_meta_pool, obj);
}

static void
release_frame (RdsBatchMeta * b, RdsFrameMeta * frame)
{
  GList *objs = frame->obj_meta_list, *displays = frame->display_meta_list;

  frame->obj_meta_list = NULL;
  frame->display_meta_list = NULL;
  for (GList * l = objs; l != NULL; l = l->next)
    release_obj (b, l->data);
  g_list_free (objs);
  for (GList * l = displays; l != NULL; l = l->next)
    release_display (b, l->data);
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
    if (o->parent == gone || (gone_set != NULL && g_hash_table_contains (gone_set, o->parent))) {
      o->parent = NULL;
      if (ELEM_HEADER (o)->f.parented) {
        ELEM_HEADER (o)->f.parented = FALSE;
        BATCH_PRIV (b)->n_parented--;
      }
    }
  }
}

static void
clear_parents (RdsBatchMeta * b, RdsFrameMeta * extra, RdsObjectMeta * gone, GHashTable * gone_set)
{
  gboolean extra_seen = FALSE;

  /* Parents are only set through rds_add_obj_meta_to_frame (documented), so no counted object means no
   * object can point at the removed one(s). */
  if (BATCH_PRIV (b)->n_parented == 0)
    return;
  for (GList * l = b->frame_meta_list; l != NULL; l = l->next) {
    clear_parents_in_frame (b, l->data, gone, gone_set);
    extra_seen |= (l->data == (gpointer) extra);
  }
  /* The frame may not have been added to the batch (yet). */
  if (extra != NULL && !extra_seen)
    clear_parents_in_frame (b, extra, gone, gone_set);
}

RdsBatchMeta *
rds_create_batch_meta (guint max_batch_size)
{
  RdsBatchMeta *self = &g_new0 (RdsBatchMetaPriv, 1)->pub;

  ensure_debug_category ();
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
  GList *frames;

  if (batch_meta == NULL)
    return;

  g_rec_mutex_lock (&batch_meta->meta_mutex);
  frames = batch_meta->frame_meta_list;
  batch_meta->frame_meta_list = NULL;
  batch_meta->num_frames_in_batch = 0;
  for (GList * l = frames; l != NULL; l = l->next)
    release_frame (batch_meta, l->data);
  g_list_free (frames);
  release_user_list (batch_meta, &batch_meta->batch_user_meta_list);
  g_rec_mutex_unlock (&batch_meta->meta_mutex);

  pool_free (batch_meta->frame_meta_pool);
  pool_free (batch_meta->obj_meta_pool);
  pool_free (batch_meta->classifier_meta_pool);
  pool_free (batch_meta->label_info_meta_pool);
  pool_free (batch_meta->display_meta_pool);
  pool_free (batch_meta->user_meta_pool);
  g_rec_mutex_clear (&batch_meta->meta_mutex);
  g_free (batch_meta);
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
  frame = pool_acquire (batch_meta->frame_meta_pool, batch_meta);
  g_rec_mutex_unlock (&batch_meta->meta_mutex);
  return frame;
}

void
rds_add_frame_meta_to_batch (RdsBatchMeta * batch_meta, RdsFrameMeta * frame_meta)
{
  g_return_if_fail (batch_meta != NULL);
  g_return_if_fail (frame_meta != NULL);

  if (frame_meta->base_meta.batch_meta != batch_meta) {
    GST_WARNING ("frame meta %p belongs to batch %p, not %p; not added", frame_meta,
        frame_meta->base_meta.batch_meta, batch_meta);
    return;
  }
  g_rec_mutex_lock (&batch_meta->meta_mutex);
  if (ELEM_HEADER (frame_meta)->f.attached) {
    GST_WARNING ("frame meta %p is already in a batch; not added again", frame_meta);
    g_rec_mutex_unlock (&batch_meta->meta_mutex);
    return;
  }
  if (batch_meta->num_frames_in_batch >= batch_meta->max_frames_in_batch)
    GST_DEBUG ("batch %p: adding frame %u exceeds max_frames_in_batch %u", batch_meta,
        batch_meta->num_frames_in_batch + 1, batch_meta->max_frames_in_batch);
  ELEM_HEADER (frame_meta)->f.attached = TRUE;
  /* Append: insertion order is part of the contract. */
  batch_meta->frame_meta_list = g_list_append (batch_meta->frame_meta_list, frame_meta);
  batch_meta->num_frames_in_batch++;
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
  release_frame (batch_meta, frame_meta);
  g_rec_mutex_unlock (&batch_meta->meta_mutex);
}

RdsObjectMeta *
rds_acquire_obj_meta_from_pool (RdsBatchMeta * batch_meta)
{
  RdsObjectMeta *obj;

  g_return_val_if_fail (batch_meta != NULL, NULL);
  g_rec_mutex_lock (&batch_meta->meta_mutex);
  obj = pool_acquire (batch_meta->obj_meta_pool, batch_meta);
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

  b = frame_meta->base_meta.batch_meta;
  if (b == NULL || obj_meta->base_meta.batch_meta != b) {
    GST_WARNING ("object meta %p and frame meta %p belong to different batches; not added", obj_meta,
        frame_meta);
    return;
  }
  if (obj_parent != NULL && (obj_parent == obj_meta || obj_parent->base_meta.batch_meta != b)) {
    GST_WARNING ("object meta %p: invalid parent %p (itself or from another batch); parent dropped", obj_meta,
        obj_parent);
    obj_parent = NULL;
  }
  g_rec_mutex_lock (&b->meta_mutex);
  if (ELEM_HEADER (obj_meta)->f.attached) {
    GST_WARNING ("object meta %p is already in a frame; not added again", obj_meta);
    g_rec_mutex_unlock (&b->meta_mutex);
    return;
  }
  ELEM_HEADER (obj_meta)->f.attached = TRUE;
  obj_meta->parent = obj_parent;
  if (obj_parent != NULL) {
    ELEM_HEADER (obj_meta)->f.parented = TRUE;
    BATCH_PRIV (b)->n_parented++;
  }
  frame_meta->obj_meta_list = g_list_append (frame_meta->obj_meta_list, obj_meta);
  frame_meta->num_obj_meta++;
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
  release_obj (b, obj_meta);
  g_rec_mutex_unlock (&b->meta_mutex);
}
