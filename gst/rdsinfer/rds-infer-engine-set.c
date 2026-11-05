/* Redstream: rdsinfer's engine set, see rds-infer-engine-set.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-engine-set.h"

#include <string.h>

G_STATIC_ASSERT ((1u << (RDS_INFER_ENGINE_SET_MAX_SIZES - 1)) >= RDS_INFER_MAX_BATCH_SIZE);

guint
rds_infer_engine_set_sizes (guint batch_size, gboolean dynamic, guint sizes[], guint max)
{
  guint n = 0;

  g_assert (batch_size >= 1 && batch_size <= RDS_INFER_MAX_BATCH_SIZE);
  g_assert (sizes != NULL);

  if (dynamic)
    for (guint size = 1; size < batch_size; size *= 2) {
      g_assert (n < max);
      sizes[n++] = size;
    }
  g_assert (n < max);
  sizes[n++] = batch_size;
  return n;
}

struct _RdsInferEngineSet
{
  guint n, sizes[RDS_INFER_ENGINE_SET_MAX_SIZES];
  RdsInferEngine *engines[RDS_INFER_ENGINE_SET_MAX_SIZES];
  gchar *fallback_reason;
};

/* "AxBxC" for @n_dims @dims (empty for none). */
static gchar *
dims_string (const guint * dims, guint n_dims)
{
  GString *s = g_string_new (NULL);

  for (guint i = 0; i < n_dims; i++)
    g_string_append_printf (s, i ? "x%u" : "%u", dims[i]);
  return g_string_free (s, FALSE);
}

/* Whether @e's per-image input and outputs are @main_e's; if not, says which differs in @error. */
static gboolean
same_tensors (RdsInferEngine * main_e, RdsInferEngine * e, GError ** error)
{
  guint mc, mh, mw, c, h, w, n = rds_infer_engine_get_n_outputs (main_e);

  rds_infer_engine_get_input (main_e, &mc, &mh, &mw);
  rds_infer_engine_get_input (e, &c, &h, &w);
  if (mc != c || mh != h || mw != w || g_strcmp0 (rds_infer_engine_get_input_name (main_e),
          rds_infer_engine_get_input_name (e)) != 0) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "input %s %ux%ux%u differs from the batch-size program's %s %ux%ux%u", rds_infer_engine_get_input_name (e),
        c, h, w, rds_infer_engine_get_input_name (main_e), mc, mh, mw);
    return FALSE;
  }
  if (rds_infer_engine_get_n_outputs (e) != n) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "%u outputs, the batch-size program has %u", rds_infer_engine_get_n_outputs (e), n);
    return FALSE;
  }
  for (guint i = 0; i < n; i++) {
    const gchar *mname, *name;
    guint mdims[8], dims[8], mnd, nd;

    rds_infer_engine_get_output (main_e, i, &mname, mdims, &mnd);
    rds_infer_engine_get_output (e, i, &name, dims, &nd);
    if (g_strcmp0 (mname, name) != 0 || mnd != nd || memcmp (mdims, dims, nd * sizeof (guint)) != 0) {
      gchar *ms = dims_string (mdims, mnd), *s = dims_string (dims, nd);

      g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
          "output %u %s [%s] differs from the batch-size program's %s [%s]", i, name, s, mname, ms);
      g_free (ms);
      g_free (s);
      return FALSE;
    }
  }
  return TRUE;
}

RdsInferEngineSet *
rds_infer_engine_set_open (const RdsInferEngineOptions * opts, gboolean dynamic, GError ** error)
{
  RdsInferEngineSet *set;
  gchar *fallback = NULL;

  g_return_val_if_fail (opts != NULL, NULL);
  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  /* batch-size 1 has nothing smaller; an invalid batch-size or no onnx_file is the engine's to refuse. */
  if (opts->batch_size < 2 || opts->batch_size > RDS_INFER_MAX_BATCH_SIZE || !opts->onnx_file)
    dynamic = FALSE;
  if (dynamic) {
    gboolean symbolic;
    GError *err = NULL;

    if (!rds_infer_engine_model_has_symbolic_batch (opts->onnx_file, &symbolic, &err)) {
      g_prefix_error (&err, "dynamic-batch: ");
      g_propagate_error (error, err);
      return NULL;
    }
    if (!symbolic) {
      fallback = g_strdup_printf ("needs a model with a symbolic batch dimension, and %s fixes it: running "
          "batch-size %u only", opts->onnx_file, opts->batch_size);
      dynamic = FALSE;
    }
  }

  set = g_new0 (RdsInferEngineSet, 1);
  set->fallback_reason = fallback;
  /* Without dynamic the set is batch-size alone, which may be out of the size rule's range: take one size, set it. */
  set->n = rds_infer_engine_set_sizes (dynamic ? opts->batch_size : 1, dynamic, set->sizes, G_N_ELEMENTS (set->sizes));
  set->sizes[set->n - 1] = opts->batch_size;
  for (guint i = set->n; i-- > 0;) {
    RdsInferEngineOptions o = *opts;
    GError *err = NULL;

    o.batch_size = set->sizes[i];
    if (i != set->n - 1)
      o.engine_file = o.output_file = NULL;
    set->engines[i] = rds_infer_engine_open (&o, &err);
    if (set->engines[i] && i != set->n - 1 && !same_tensors (set->engines[set->n - 1], set->engines[i], &err)) {
      rds_infer_engine_free (set->engines[i]);
      set->engines[i] = NULL;
    }
    if (!set->engines[i]) {
      g_assert (err != NULL);
      if (set->n > 1)
        g_prefix_error (&err, "batch %u program: ", set->sizes[i]);
      g_propagate_error (error, err);
      rds_infer_engine_set_free (set);
      return NULL;
    }
  }
  return set;
}

const gchar *
rds_infer_engine_set_get_fallback_reason (RdsInferEngineSet * set)
{
  return set->fallback_reason;
}

RdsInferEngine *
rds_infer_engine_set_get_main (RdsInferEngineSet * set)
{
  return set->engines[set->n - 1];
}

guint
rds_infer_engine_set_get_n (RdsInferEngineSet * set)
{
  return set->n;
}

guint
rds_infer_engine_set_get_size (RdsInferEngineSet * set, guint i)
{
  g_assert (i < set->n);
  return set->sizes[i];
}

RdsInferEngine *
rds_infer_engine_set_get (RdsInferEngineSet * set, guint i)
{
  g_assert (i < set->n);
  return set->engines[i];
}

guint
rds_infer_engine_set_pick (RdsInferEngineSet * set, guint n)
{
  guint i = 0;

  g_assert (n >= 1 && n <= set->sizes[set->n - 1]);
  while (set->sizes[i] < n)
    i++;
  return i;
}

void
rds_infer_engine_set_free (RdsInferEngineSet * set)
{
  if (!set)
    return;
  for (guint i = 0; i < set->n; i++)
    if (set->engines[i])
      rds_infer_engine_free (set->engines[i]);
  g_free (set->fallback_reason);
  g_free (set);
}
