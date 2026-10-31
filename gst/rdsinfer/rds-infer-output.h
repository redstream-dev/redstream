/* Redstream: rdsinfer's output buffers (plugin-internal, not installed). An output set holds one batch's outputs:
 * every output layer on the GPU (what the engine writes) and a pinned host copy. Sets are refcounted and come from a
 * pool's free list: the element takes one per inferred batch, and every tensor meta pointing into it holds a ref,
 * so a set goes back to the free list only when the last meta using it (a demuxed copy, say) is released. A new set
 * is allocated only when every set is in use (logged at debug level). Sets beyond what recent load needed are freed
 * (see RDS_INFER_OUTPUT_PEAK_WINDOW), so a burst (a secondary batch with many objects) doesn't stay allocated until
 * stop, while a steady load keeps its sets.
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

/* How many sets a pool keeps (ours): max (RDS_INFER_OUTPUT_MAX_FREE, the recent peak of sets in use). When a set is
 * released, free sets beyond that are freed, least recently used first.
 * - RDS_INFER_OUTPUT_MAX_FREE, the floor, is rdsstreammux's default buffer-pool-size: a primary rdsinfer behind it
 *   has at most that many batches, and so sets, alive downstream, so it never frees and reallocates in steady state.
 * - The recent peak is the most sets in use during the current window of RDS_INFER_OUTPUT_PEAK_WINDOW acquires or
 *   the one before. A load whose peak comes back at least once every window (a secondary element with a set per
 *   batch-size objects of each batch, say) keeps its sets and never reallocates; a one-off spike is freed once it
 *   is more than one and at most two windows old, at the next release. The window advances only on acquires, so an
 *   idle pipeline keeps a spike's sets until traffic resumes or the element stops.
 * The trade-off is in the window: longer keeps a spike's memory (device and pinned host) for longer, shorter makes
 * a load whose peaks come less often than once a window free and reallocate. 256 acquires is 256 batches in primary
 * mode (about 9-17 s at 30 batches/s before a spike goes) and 256 / (objects / batch-size) batches in secondary
 * mode; a steady load reaches its peak every batch, far more often than that. */
#define RDS_INFER_OUTPUT_MAX_FREE 4
#define RDS_INFER_OUTPUT_PEAK_WINDOW 256

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
/* Sets allocated and freed since the pool was made. */
G_GNUC_INTERNAL void rds_infer_output_pool_get_totals (RdsInferOutputPool * pool, guint64 * allocated,
    guint64 * freed);

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
