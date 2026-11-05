/* Redstream: rdsinfer's engine set (plugin-internal, not installed): the programs one model is compiled into. A
 * compiled MIGraphX program has a fixed batch size, so with dynamic-batch (ours) rdsinfer compiles one program for
 * each power of two below batch-size and one for batch-size, and runs each chunk on the smallest that holds it.
 * Without it, the set is one program of batch-size, as before.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_ENGINE_SET_H
#define RDS_INFER_ENGINE_SET_H

#include "rds-infer-config.h"

#include <glib.h>

G_BEGIN_DECLS

/* The most sizes a set has: 1, 2, 4, ..., 512 and a batch-size up to RDS_INFER_MAX_BATCH_SIZE. */
#define RDS_INFER_ENGINE_SET_MAX_SIZES 11

/* Writes the program batch sizes for @batch_size to @sizes, in ascending order, and returns how many (at least 1):
 * with @dynamic, the powers of two below @batch_size, then @batch_size; without it, @batch_size alone. Asserted:
 * @batch_size is 1..RDS_INFER_MAX_BATCH_SIZE, @sizes is not NULL and the sizes fit in its @max entries
 * (RDS_INFER_ENGINE_SET_MAX_SIZES always do). */
G_GNUC_INTERNAL guint rds_infer_engine_set_sizes (guint batch_size, gboolean dynamic, guint sizes[], guint max);

G_END_DECLS

#endif /* RDS_INFER_ENGINE_SET_H */
