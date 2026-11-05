/* Redstream: rdsinfer's engine set, see rds-infer-engine-set.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-engine-set.h"

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
