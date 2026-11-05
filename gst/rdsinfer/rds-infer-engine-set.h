/* Redstream: rdsinfer's engine set (plugin-internal, not installed): the programs one model is compiled into. A
 * compiled MIGraphX program has a fixed batch size, so with dynamic-batch (ours) rdsinfer compiles one program for
 * each power of two below batch-size and one for batch-size, and runs each chunk on the smallest that holds it.
 * Without it, the set is one program of batch-size, as before.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_ENGINE_SET_H
#define RDS_INFER_ENGINE_SET_H

#include "rds-infer-config.h"
#include "rds-infer-engine.h"

#include <glib.h>

G_BEGIN_DECLS

/* The most sizes a set has: 1, 2, 4, ..., 512 and a batch-size up to RDS_INFER_MAX_BATCH_SIZE. */
#define RDS_INFER_ENGINE_SET_MAX_SIZES 11

/* Writes the program batch sizes for @batch_size to @sizes, in ascending order, and returns how many (at least 1):
 * with @dynamic, the powers of two below @batch_size, then @batch_size; without it, @batch_size alone. Asserted:
 * @batch_size is 1..RDS_INFER_MAX_BATCH_SIZE, @sizes is not NULL and the sizes fit in its @max entries
 * (RDS_INFER_ENGINE_SET_MAX_SIZES always do). */
G_GNUC_INTERNAL guint rds_infer_engine_set_sizes (guint batch_size, gboolean dynamic, guint sizes[], guint max);

typedef struct _RdsInferEngineSet RdsInferEngineSet;

/* Opens one engine for each of rds_infer_engine_set_sizes (@opts->batch_size, @dynamic): the batch-size program
 * first, with @opts as given, then the smaller ones in descending order with the same options but no engine_file or
 * output_file (their normal cache names). Every program must have the batch-size one's per-image input and outputs
 * (else RDS_INFER_ENGINE_ERROR_MODEL). All or nothing: when one fails, those opened are freed and, with more than
 * one size, the error is prefixed with the failing size ("batch 2 program: ..."). With @dynamic and a model whose
 * batch dimension is fixed, the set is the batch-size program alone and rds_infer_engine_set_get_fallback_reason
 * says why; without onnx_file, the open fails as rds_infer_engine_open's does. The engines share the caller's
 * buffers: a batch-major NCHW buffer of batch-size images holds a valid tensor of any fewer. */
G_GNUC_INTERNAL RdsInferEngineSet *rds_infer_engine_set_open (const RdsInferEngineOptions * opts, gboolean dynamic,
    GError ** error);
/* Why @dynamic was not honoured (see rds_infer_engine_set_open), or NULL; phrased to follow the key name
 * ("needs a model with ..."). */
G_GNUC_INTERNAL const gchar *rds_infer_engine_set_get_fallback_reason (RdsInferEngineSet * set);
/* The batch-size program: the one whose input, outputs, paths and build status describe the set. */
G_GNUC_INTERNAL RdsInferEngine *rds_infer_engine_set_get_main (RdsInferEngineSet * set);
/* How many programs (at least 1); index i in ascending batch size, the last is the main one. */
G_GNUC_INTERNAL guint rds_infer_engine_set_get_n (RdsInferEngineSet * set);
G_GNUC_INTERNAL guint rds_infer_engine_set_get_size (RdsInferEngineSet * set, guint i);
G_GNUC_INTERNAL RdsInferEngine *rds_infer_engine_set_get (RdsInferEngineSet * set, guint i);
/* The index of the smallest program that holds @n images; asserted: 1 <= @n <= batch-size. */
G_GNUC_INTERNAL guint rds_infer_engine_set_pick (RdsInferEngineSet * set, guint n);
/* Frees every engine (no run may be in flight on any of them). NULL is allowed. */
G_GNUC_INTERNAL void rds_infer_engine_set_free (RdsInferEngineSet * set);

G_END_DECLS

#endif /* RDS_INFER_ENGINE_SET_H */
