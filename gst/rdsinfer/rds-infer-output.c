/* Redstream: rdsinfer's refcounted output sets and the tensor meta that points into them.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-output.h"

#include <gst/gst.h>
/* after GLib/GStreamer: HIP defines __noinline__ */
#include <hip/hip_runtime_api.h>

GST_DEBUG_CATEGORY_EXTERN (gst_rds_infer_debug);
#define GST_CAT_DEFAULT gst_rds_infer_debug

struct _RdsInferOutputPool
{
  gint refcount;                /* the owner's, plus one per set in use */
  GMutex lock;                  /* free_sets, n_sets */
  GQueue free_sets;
  guint n_sets;
  guint gpu_id, batch_size, n_outputs;
  RdsInferDims *dims;
  gchar **names;
};

static gsize
output_bytes (const RdsInferOutputPool * pool, guint i)
{
  return (gsize) pool->batch_size * pool->dims[i].numElements * sizeof (gfloat);
}

/* Frees @set's memory on the pool's GPU (any thread: the caller's device is restored). */
static void
set_free (RdsInferOutputPool * pool, RdsInferOutputSet * set)
{
  int prev = 0;
  gboolean switched = hipGetDevice (&prev) == hipSuccess && hipSetDevice ((int) pool->gpu_id) == hipSuccess;

  for (guint i = 0; i < pool->n_outputs; i++) {
    if (set->dev[i])
      (void) hipFree (set->dev[i]);
    if (set->host[i])
      (void) hipHostFree (set->host[i]);
  }
  if (switched)
    (void) hipSetDevice (prev);
  g_free (set->dev);
  g_free (set->host);
  g_free (set);
}

RdsInferOutputPool *
rds_infer_output_pool_new (guint gpu_id, guint batch_size, guint n_outputs, const RdsInferDims * dims,
    const gchar * const *names)
{
  RdsInferOutputPool *pool = g_new0 (RdsInferOutputPool, 1);

  pool->refcount = 1;
  g_mutex_init (&pool->lock);
  g_queue_init (&pool->free_sets);
  pool->gpu_id = gpu_id;
  pool->batch_size = batch_size;
  pool->n_outputs = n_outputs;
  pool->dims = g_memdup2 (dims, n_outputs * sizeof (RdsInferDims));
  pool->names = g_new0 (gchar *, n_outputs + 1);
  for (guint i = 0; i < n_outputs; i++)
    pool->names[i] = g_strdup (names[i]);
  return pool;
}

static RdsInferOutputPool *
pool_ref (RdsInferOutputPool * pool)
{
  g_atomic_int_inc (&pool->refcount);
  return pool;
}

void
rds_infer_output_pool_unref (RdsInferOutputPool * pool)
{
  RdsInferOutputSet *set;

  if (!pool || !g_atomic_int_dec_and_test (&pool->refcount))
    return;
  /* the last ref: no set is in use any more, they are all in the free list */
  while ((set = g_queue_pop_head (&pool->free_sets)))
    set_free (pool, set);
  g_mutex_clear (&pool->lock);
  g_free (pool->dims);
  g_strfreev (pool->names);
  g_free (pool);
}

RdsInferOutputSet *
rds_infer_output_pool_acquire (RdsInferOutputPool * pool, GError ** error)
{
  RdsInferOutputSet *set;
  guint n_sets;

  g_mutex_lock (&pool->lock);
  set = g_queue_pop_head (&pool->free_sets);
  g_mutex_unlock (&pool->lock);
  if (!set) {
    set = g_new0 (RdsInferOutputSet, 1);
    set->dev = g_new0 (gfloat *, pool->n_outputs);
    set->host = g_new0 (gfloat *, pool->n_outputs);
    for (guint i = 0; i < pool->n_outputs; i++) {
      hipError_t err = hipMalloc ((void **) &set->dev[i], output_bytes (pool, i));

      if (err == hipSuccess)
        err = hipHostMalloc ((void **) &set->host[i], output_bytes (pool, i), hipHostMallocDefault);
      if (err != hipSuccess) {
        g_set_error (error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NO_SPACE_LEFT,
            "can't allocate %" G_GSIZE_FORMAT " bytes for output %s: %s", output_bytes (pool, i),
            pool->names[i], hipGetErrorString (err));
        set_free (pool, set);
        return NULL;
      }
    }
    g_mutex_lock (&pool->lock);
    n_sets = ++pool->n_sets;
    g_mutex_unlock (&pool->lock);
    GST_DEBUG ("all output sets in use: allocated set %u", n_sets);
  }
  set->pool = pool_ref (pool);
  set->refcount = 1;
  return set;
}

void
rds_infer_output_pool_get_counts (RdsInferOutputPool * pool, guint * n_sets, guint * n_free)
{
  g_mutex_lock (&pool->lock);
  *n_sets = pool->n_sets;
  *n_free = pool->free_sets.length;
  g_mutex_unlock (&pool->lock);
}

RdsInferOutputSet *
rds_infer_output_set_ref (RdsInferOutputSet * set)
{
  g_atomic_int_inc (&set->refcount);
  return set;
}

void
rds_infer_output_set_unref (RdsInferOutputSet * set)
{
  RdsInferOutputPool *pool;

  if (!set || !g_atomic_int_dec_and_test (&set->refcount))
    return;
  pool = set->pool;
  set->pool = NULL;
  g_mutex_lock (&pool->lock);
  g_queue_push_head (&pool->free_sets, set);
  g_mutex_unlock (&pool->lock);
  rds_infer_output_pool_unref (pool);
}

/* One block: the meta, then the layer infos, then the host and device pointer arrays. */
static gsize
tensor_meta_size (guint n)
{
  return sizeof (RdsInferTensorMeta) + n * sizeof (RdsInferLayerInfo) + 2 * n * sizeof (void *);
}

static void
tensor_meta_link (RdsInferTensorMeta * tm)
{
  guint n = tm->num_output_layers;

  tm->output_layers_info = (RdsInferLayerInfo *) (tm + 1);
  tm->out_buf_ptrs_host = (void **) (tm->output_layers_info + n);
  tm->out_buf_ptrs_dev = tm->out_buf_ptrs_host + n;
}

RdsInferTensorMeta *
rds_infer_tensor_meta_new (RdsInferOutputSet * set, guint image, guint unique_id,
    const RdsInferNetworkInfo * network_info, gboolean maintain_aspect_ratio, gboolean symmetric_padding)
{
  RdsInferOutputPool *pool = set->pool;
  RdsInferTensorMeta *tm = g_malloc0 (tensor_meta_size (pool->n_outputs));

  tm->unique_id = unique_id;
  tm->num_output_layers = pool->n_outputs;
  tensor_meta_link (tm);
  for (guint i = 0; i < pool->n_outputs; i++) {
    RdsInferLayerInfo *l = &tm->output_layers_info[i];
    gsize offset = (gsize) image * pool->dims[i].numElements;

    l->dataType = RDS_INFER_DATA_FLOAT;
    l->inferDims = pool->dims[i];
    l->bindingIndex = (int) i;
    l->layerName = pool->names[i];      /* the pool lives as long as the set */
    tm->out_buf_ptrs_host[i] = set->host[i] + offset;
    tm->out_buf_ptrs_dev[i] = set->dev[i] + offset;
  }
  tm->gpu_id = (gint) pool->gpu_id;
  tm->priv_data = rds_infer_output_set_ref (set);
  tm->network_info = *network_info;
  tm->maintain_aspect_ratio = maintain_aspect_ratio;
  tm->symmetric_padding = symmetric_padding;
  return tm;
}

gpointer
rds_infer_tensor_meta_copy (gpointer data, gpointer user_data G_GNUC_UNUSED)
{
  const RdsInferTensorMeta *src = ((RdsUserMeta *) data)->user_meta_data;
  RdsInferTensorMeta *tm;

  if (!src)
    return NULL;
  /* same buffers: the copy points into the same set and holds its own ref */
  tm = g_memdup2 (src, tensor_meta_size (src->num_output_layers));
  tensor_meta_link (tm);
  rds_infer_output_set_ref (tm->priv_data);
  return tm;
}

void
rds_infer_tensor_meta_release (gpointer data, gpointer user_data G_GNUC_UNUSED)
{
  RdsUserMeta *um = data;
  RdsInferTensorMeta *tm = um->user_meta_data;

  if (!tm)
    return;
  rds_infer_output_set_unref (tm->priv_data);
  g_free (tm);
  um->user_meta_data = NULL;
}
