/* Redstream: deep copies of batch and frame metadata.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>

#include "rds-meta-private.h"

#define GST_CAT_DEFAULT rds_meta_debug

/* Struct assignment that keeps the destination's base_meta (batch_meta, meta_type and callbacks from the pool).
 * The hidden element header lives before the struct, so it is never touched. */
#define COPY_KEEP_BASE(dst, src) G_STMT_START {      \
    RdsBaseMeta keep_base_ = (dst)->base_meta;       \
    *(dst) = *(src);                                 \
    (dst)->base_meta = keep_base_;                   \
  } G_STMT_END

static RdsUserMeta *
copy_user (RdsUserMeta * src, RdsBatchMeta * dst)
{
  static gsize warned = 0;
  RdsUserMeta *um;

  if (src->base_meta.copy_func == NULL) {
    /* Copying the pointer would make both batches free it. */
    if (g_once_init_enter (&warned)) {
      GST_WARNING ("user meta %p (type %d) has no copy_func; skipped in copies (warned once)", src,
          src->base_meta.meta_type);
      g_once_init_leave (&warned, 1);
    }
    return NULL;
  }
  um = rds_meta_pool_acquire (dst->user_meta_pool, dst);
  um->base_meta.meta_type = src->base_meta.meta_type;
  um->base_meta.uContext = src->base_meta.uContext;
  um->base_meta.copy_func = src->base_meta.copy_func;
  um->base_meta.release_func = src->base_meta.release_func;
  um->user_meta_data = src->base_meta.copy_func (src, src->base_meta.uContext);
  return um;
}

static void
copy_user_list (GList * src_list, RdsBatchMeta * dst, GList ** dst_list, gconstpointer owner)
{
  for (GList * l = src_list; l != NULL; l = l->next) {
    RdsUserMeta *um = copy_user (l->data, dst);

    if (um != NULL && !rds_meta_attach (dst, dst_list, um, "user", owner))
      rds_meta_release_user (dst, um);
  }
}

static RdsLabelInfo *
copy_label (RdsLabelInfo * src, RdsBatchMeta * dst)
{
  RdsLabelInfo *label = rds_meta_pool_acquire (dst->label_info_meta_pool, dst);

  COPY_KEEP_BASE (label, src);
  label->pResult_label = g_strdup (src->pResult_label);
  return label;
}

static RdsClassifierMeta *
copy_classifier (RdsClassifierMeta * src, RdsBatchMeta * dst)
{
  RdsClassifierMeta *c = rds_meta_pool_acquire (dst->classifier_meta_pool, dst);

  COPY_KEEP_BASE (c, src);
  c->label_info_list = NULL;
  c->num_labels = 0;
  for (GList * l = src->label_info_list; l != NULL; l = l->next) {
    RdsLabelInfo *label = copy_label (l->data, dst);

    if (rds_meta_attach (dst, &c->label_info_list, label, "label info", c))
      c->num_labels++;
    else
      rds_meta_release_label (dst, label);
  }
  return c;
}

/* parent is left NULL: the frame copy remaps it once all objects exist. */
static RdsObjectMeta *
copy_obj (RdsObjectMeta * src, RdsBatchMeta * dst)
{
  RdsObjectMeta *o = rds_meta_pool_acquire (dst->obj_meta_pool, dst);

  COPY_KEEP_BASE (o, src);
  o->parent = NULL;
  o->classifier_meta_list = NULL;
  o->obj_user_meta_list = NULL;
  o->text_params.display_text = g_strdup (src->text_params.display_text);
  o->mask_params.data = (src->mask_params.data != NULL && src->mask_params.size > 0) ?
      g_memdup2 (src->mask_params.data, src->mask_params.size) : NULL;
  if (o->mask_params.data == NULL)
    o->mask_params.size = 0;

  for (GList * l = src->classifier_meta_list; l != NULL; l = l->next) {
    RdsClassifierMeta *c = copy_classifier (l->data, dst);

    if (!rds_meta_attach (dst, &o->classifier_meta_list, c, "classifier", o))
      rds_meta_release_classifier (dst, c);
  }
  copy_user_list (src->obj_user_meta_list, dst, &o->obj_user_meta_list, o);
  return o;
}

static RdsDisplayMeta *
copy_display (RdsDisplayMeta * src, RdsBatchMeta * dst)
{
  RdsDisplayMeta *d = rds_meta_pool_acquire (dst->display_meta_pool, dst);

  COPY_KEEP_BASE (d, src);
  for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
    d->text_params[i].display_text = g_strdup (src->text_params[i].display_text);
  return d;
}

/* Both locks held. Returns a complete frame in dst that is not yet added to dst's frame list. Objects get no
 * parent; each source object -> copy pair is recorded in map, for remap_parents. */
static RdsFrameMeta *
copy_frame (RdsFrameMeta * src, RdsBatchMeta * dst, GHashTable * map)
{
  RdsFrameMeta *f = rds_meta_pool_acquire (dst->frame_meta_pool, dst);

  COPY_KEEP_BASE (f, src);
  f->obj_meta_list = NULL;
  f->display_meta_list = NULL;
  f->frame_user_meta_list = NULL;
  f->num_obj_meta = 0;

  /* Every object, in order, without parents. */
  for (GList * l = src->obj_meta_list; l != NULL; l = l->next) {
    RdsObjectMeta *o = copy_obj (l->data, dst);

    if (rds_meta_attach (dst, &f->obj_meta_list, o, "object", f)) {
      f->num_obj_meta++;
      g_hash_table_insert (map, l->data, o);
    } else {
      rds_meta_release_obj (dst, o);
    }
  }
  for (GList * l = src->display_meta_list; l != NULL; l = l->next) {
    RdsDisplayMeta *d = copy_display (l->data, dst);

    if (!rds_meta_attach (dst, &f->display_meta_list, d, "display", f))
      rds_meta_release_display (dst, d);
  }
  copy_user_list (src->frame_user_meta_list, dst, &f->frame_user_meta_list, f);
  return f;
}

/* dst's lock held. Second pass, once every object the map covers exists: links each copy of src_objs to the copy
 * of its source parent, through the internal path so n_parented stays right. A parent the map does not cover
 * (a frame that was not copied, or an object in no list) becomes NULL. */
static void
remap_parents (RdsBatchMeta * dst, GList * src_objs, GHashTable * map)
{
  for (GList * l = src_objs; l != NULL; l = l->next) {
    RdsObjectMeta *so = l->data;
    RdsObjectMeta *o = g_hash_table_lookup (map, so);
    RdsObjectMeta *parent = so->parent != NULL ? g_hash_table_lookup (map, so->parent) : NULL;

    if (o != NULL && parent != NULL)
      rds_meta_set_parent (dst, o, parent);
  }
}

/* dst's lock held. Adds the copied frame, or releases it (with everything under it) if that fails. */
static gboolean
add_copied_frame (RdsBatchMeta * dst, RdsFrameMeta * f)
{
  if (!rds_meta_attach (dst, &dst->frame_meta_list, f, "frame", dst)) {
    rds_meta_release_frame (dst, f);
    return FALSE;
  }
  dst->num_frames_in_batch++;
  return TRUE;
}

RdsFrameMeta *
rds_copy_frame_meta_to_batch (RdsFrameMeta * src, RdsBatchMeta * dst)
{
  RdsBatchMeta *sb;
  RdsFrameMeta *f;
  GHashTable *map;

  g_return_val_if_fail (src != NULL, NULL);
  g_return_val_if_fail (dst != NULL, NULL);

  sb = src->base_meta.batch_meta;
  if (sb == NULL) {
    GST_WARNING ("frame meta %p has no batch (released?); not copied", src);
    return NULL;
  }
  g_rec_mutex_lock (&sb->meta_mutex);
  g_rec_mutex_lock (&dst->meta_mutex);
  /* A local map: parents in other frames of src's batch become NULL. */
  map = g_hash_table_new (g_direct_hash, g_direct_equal);
  f = copy_frame (src, dst, map);
  remap_parents (dst, src->obj_meta_list, map);
  g_hash_table_unref (map);
  if (!add_copied_frame (dst, f))
    f = NULL;
  g_rec_mutex_unlock (&dst->meta_mutex);
  g_rec_mutex_unlock (&sb->meta_mutex);
  return f;
}

RdsBatchMeta *
rds_batch_meta_copy (RdsBatchMeta * src)
{
  RdsBatchMeta *dst;
  GHashTable *map;
  gboolean ok = TRUE;

  g_return_val_if_fail (src != NULL, NULL);

  g_rec_mutex_lock (&src->meta_mutex);
  dst = rds_create_batch_meta (src->max_frames_in_batch);
  memcpy (dst->misc_batch_info, src->misc_batch_info, sizeof dst->misc_batch_info);
  memcpy (dst->reserved, src->reserved, sizeof dst->reserved);
  g_rec_mutex_lock (&dst->meta_mutex);

  /* One map for the whole batch, and the parent pass after every frame exists, so a parent in another frame
   * (earlier or later) is remapped too. */
  map = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (GList * l = src->frame_meta_list; l != NULL && ok; l = l->next)
    ok = add_copied_frame (dst, copy_frame (l->data, dst, map));
  if (ok) {
    for (GList * l = src->frame_meta_list; l != NULL; l = l->next)
      remap_parents (dst, ((RdsFrameMeta *) l->data)->obj_meta_list, map);
    copy_user_list (src->batch_user_meta_list, dst, &dst->batch_user_meta_list, dst);
  }
  g_hash_table_unref (map);
  g_rec_mutex_unlock (&dst->meta_mutex);
  g_rec_mutex_unlock (&src->meta_mutex);

  if (!ok) {
    /* Defensive: adding a freshly copied frame cannot fail today. If it ever does, a partial copy is worse than
     * none (callers would silently lose frames), so it is discarded without leaking. */
    GST_WARNING ("copy of batch %p failed; discarding the partial copy", src);
    rds_destroy_batch_meta (dst);
    return NULL;
  }
  return dst;
}
