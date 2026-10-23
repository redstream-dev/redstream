/* Redstream: metadata internals shared by rds-meta.c and rds-meta-copy.c. Not installed.
 * Everything here expects the batch's meta_mutex to be held unless noted.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_META_PRIVATE_H
#define RDS_META_PRIVATE_H

#include <gst/gst.h>
#include "rds-meta.h"

G_BEGIN_DECLS

/* "RDSE": set by the pool on every element it allocates, checked before the flags are trusted. A meta that did
 * not come from a pool is refused. Reading the 16 bytes before such a pointer can itself be out of bounds; the
 * magic only makes the common misuse (a meta allocated by the app) fail loudly. */
#define RDS_META_ELEM_MAGIC 0x52445345u

/* Hidden per-element header, allocated just before every pooled meta. 16 bytes keep the meta itself aligned as
 * g_malloc would. */
typedef union
{
  struct
  {
    guint32 magic;
    gboolean attached;          /* in a list: refuses double adds and adds to a second list */
    gboolean parented;          /* an object added with a parent: counted in n_parented */
  } f;
  guint8 pad[16];
} RdsMetaElemHeader;

G_STATIC_ASSERT (sizeof (RdsMetaElemHeader) % 16 == 0);

#define RDS_META_ELEM_HEADER(elem) (((RdsMetaElemHeader *) (elem)) - 1)

/* Private batch: the public struct comes first, so RdsBatchMeta * casts to it. This is why an RdsBatchMeta must
 * come from rds_create_batch_meta. */
typedef struct
{
  RdsBatchMeta pub;
  /* Attached objects added with a non-NULL parent that is still set. While it is 0, removals skip the
   * batch-wide parent scan. */
  guint n_parented;
} RdsBatchMetaPriv;

#define RDS_BATCH_PRIV(b) ((RdsBatchMetaPriv *) (b))

G_GNUC_INTERNAL extern GstDebugCategory *rds_meta_debug;
/* Safe without the lock; call before logging from code that can run before any batch exists. */
G_GNUC_INTERNAL void rds_meta_ensure_debug_category (void);

/* A fresh zeroed element with base_meta.batch_meta and meta_type set, and a clean header. */
G_GNUC_INTERNAL gpointer rds_meta_pool_acquire (RdsMetaPool * pool, RdsBatchMeta * batch_meta);

/* Appends meta to *list when it is a pool element of b in no list yet; otherwise warns and returns FALSE. owner
 * and what are for the log. */
G_GNUC_INTERNAL gboolean rds_meta_attach (RdsBatchMeta * b, GList ** list, gpointer meta, const gchar * what,
    gconstpointer owner);
/* Unlinks meta from *list (the caller then releases it); warns and returns FALSE if it is not there. */
G_GNUC_INTERNAL gboolean rds_meta_detach (GList ** list, gpointer meta, const gchar * what, gconstpointer owner);
/* Sets obj->parent and keeps the parented flag and n_parented right. parent must be NULL or from b. */
G_GNUC_INTERNAL void rds_meta_set_parent (RdsBatchMeta * b, RdsObjectMeta * obj, RdsObjectMeta * parent);

/* Release helpers: free owned members and children, then return the element to its pool. The caller unlinks
 * first. */
G_GNUC_INTERNAL void rds_meta_release_label (RdsBatchMeta * b, RdsLabelInfo * label);
G_GNUC_INTERNAL void rds_meta_release_classifier (RdsBatchMeta * b, RdsClassifierMeta * classifier);
G_GNUC_INTERNAL void rds_meta_release_user (RdsBatchMeta * b, RdsUserMeta * user);
G_GNUC_INTERNAL void rds_meta_release_display (RdsBatchMeta * b, RdsDisplayMeta * display);
G_GNUC_INTERNAL void rds_meta_release_obj (RdsBatchMeta * b, RdsObjectMeta * obj);
G_GNUC_INTERNAL void rds_meta_release_frame (RdsBatchMeta * b, RdsFrameMeta * frame);

G_END_DECLS

#endif /* RDS_META_PRIVATE_H */
