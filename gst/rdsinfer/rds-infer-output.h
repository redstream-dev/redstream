/* Redstream: rdsinfer's output buffers (plugin-internal, not installed). An output set holds one batch's outputs:
 * every output layer on the GPU (what the engine writes) and a pinned host copy. Sets are refcounted and come from a
 * pool's free list: the element takes one per inferred batch, and every tensor meta pointing into it holds a ref,
 * so a set goes back to the free list only when the last meta using it (a demuxed copy, say) is released. The free
 * list is unbounded but reused: a new set is allocated only when every set is in use (logged at debug level), which
 * with a fixed upstream batch pool levels off at a few sets.
 *
 * Each set in use holds a ref on its pool, and the pool frees the sets when its last ref goes, so a meta that
 * outlives the element (or its stop) keeps its buffers valid. Refs and unrefs are thread-safe; the last unref may
 * happen on any thread (it switches to the pool's GPU to free memory and restores the thread's device).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_OUTPUT_H
#define RDS_INFER_OUTPUT_H

#include <glib.h>
#include <rds/rds-infer.h>
#include <rds/rds-infer-meta.h>

G_BEGIN_DECLS

typedef struct _RdsInferOutputPool RdsInferOutputPool;

typedef struct
{
  RdsInferOutputPool *pool;
  gint refcount;
  gfloat **dev;                 /* per output: batch_size images on the GPU */
  gfloat **host;                /* per output: the same in pinned host memory */
} RdsInferOutputSet;

/* @dims: each output's per-image dims (numElements set); @names: its names (copied). */
G_GNUC_INTERNAL RdsInferOutputPool *rds_infer_output_pool_new (guint gpu_id, guint batch_size, guint n_outputs,
    const RdsInferDims * dims, const gchar * const *names);
G_GNUC_INTERNAL void rds_infer_output_pool_unref (RdsInferOutputPool * pool);
/* A set from the free list, or a new one (NULL with @error if the allocation fails). Call with the pool's GPU
 * current. */
G_GNUC_INTERNAL RdsInferOutputSet *rds_infer_output_pool_acquire (RdsInferOutputPool * pool, GError ** error);
/* Sets allocated (in use or free) and sets in the free list. */
G_GNUC_INTERNAL void rds_infer_output_pool_get_counts (RdsInferOutputPool * pool, guint * n_sets, guint * n_free);

G_GNUC_INTERNAL RdsInferOutputSet *rds_infer_output_set_ref (RdsInferOutputSet * set);
G_GNUC_INTERNAL void rds_infer_output_set_unref (RdsInferOutputSet * set);

/* A tensor meta for image @image of @set (it takes a ref on the set, kept in priv_data). One allocation, freed by
 * rds_infer_tensor_meta_release. */
G_GNUC_INTERNAL RdsInferTensorMeta *rds_infer_tensor_meta_new (RdsInferOutputSet * set, guint image,
    guint unique_id, const RdsInferNetworkInfo * network_info, gboolean maintain_aspect_ratio,
    gboolean symmetric_padding);
/* The user meta callbacks (RdsMetaCopyFunc, RdsMetaReleaseFunc): data is the RdsUserMeta. */
G_GNUC_INTERNAL gpointer rds_infer_tensor_meta_copy (gpointer data, gpointer user_data);
G_GNUC_INTERNAL void rds_infer_tensor_meta_release (gpointer data, gpointer user_data);

G_END_DECLS

#endif /* RDS_INFER_OUTPUT_H */
