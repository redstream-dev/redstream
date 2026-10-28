/* Redstream: rdsinfer's MIGraphX engine. Parses an ONNX model with its input fixed to [batch, C, H, W], compiles it
 * for the GPU (FP32, or FP16 with FP32 inputs and outputs), and caches the compiled program as an .mxr file with a
 * JSON manifest next to it; a cached program is only loaded when its manifest matches the model, the device and the
 * software that would compile it now. Inputs and outputs are the caller's device buffers. Plain C API: no MIGraphX
 * or HIP types, and no C++ exception leaves it. Plugin-internal, not installed.
 *
 * Cache file: <onnx stem>_b<batch>_<C>x<H>x<W>_<gfx>_<fp32|fp16>.mxr (+ .mxr.json), in the ONNX file's directory,
 * or in $XDG_CACHE_HOME/redstream/ when that isn't writable. <gfx> is the device's own architecture (from the
 * kernel's KFD topology, so HSA_OVERRIDE_GFX_VERSION doesn't change it), e.g. gfx1036. Both files are written
 * through unique temporary files and renames, so two processes building the same entry both succeed (the last
 * rename wins). Models that keep weights in external data files are refused: the manifest hashes the .onnx only.
 *
 * Threads: an engine may be opened on one thread and used on another, but only from one thread at a time.
 * Every call sets the engine's GPU as the calling thread's current device for its duration and restores the
 * previous one.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_ENGINE_H
#define RDS_INFER_ENGINE_H

#include <glib.h>

G_BEGIN_DECLS

#define RDS_INFER_ENGINE_ERROR (rds_infer_engine_error_quark ())
G_GNUC_INTERNAL GQuark rds_infer_engine_error_quark (void);

typedef enum
{
  RDS_INFER_ENGINE_ERROR_INVALID,       /* bad options */
  RDS_INFER_ENGINE_ERROR_FILE,          /* the ONNX file can't be read */
  RDS_INFER_ENGINE_ERROR_MODEL,         /* the model can't be used as asked (encoding, input dims, batch, types) */
  RDS_INFER_ENGINE_ERROR_DEVICE,        /* no such GPU, or a HIP call failed */
  RDS_INFER_ENGINE_ERROR_MIGRAPHX,      /* a MIGraphX call failed; MIGraphX prints its own reason to stderr */
  RDS_INFER_ENGINE_ERROR_NOT_CACHED,    /* no_build and no matching program; the message says why (the build reason) */
  RDS_INFER_ENGINE_ERROR_OUTPUT,        /* output_file's directory can't be written (checked before building) */
} RdsInferEngineError;

typedef struct
{
  const gchar *onnx_file;
  const gchar *engine_file;             /* a .mxr to try first, or NULL */
  guint batch_size, gpu_id;
  gboolean fp16;
  const guint *infer_dims;              /* C, H, W, or NULL to take them from the model */
  gboolean force_rebuild;               /* never load a cached program */
  gboolean no_build;                    /* only load: fail with NOT_CACHED where a program would be built */
  /* Look up and save the program only here (+ ".json"), instead of engine_file and the cache names; NULL: unset.
   * When a build is needed and its directory can't be written, fails with OUTPUT before building. */
  const gchar *output_file;
} RdsInferEngineOptions;

typedef struct _RdsInferEngine RdsInferEngine;

/* Checks packed float32 tensor sizes before narrowing dimensions or allocating buffers. @dims are per image;
 * max_elements is G_MAXSIZE for inputs, G_MAXUINT for outputs exposed through RdsInferDims. Optional outputs
 * are written only on success; a rank-zero per-image tensor has one element. */
G_GNUC_INTERNAL gboolean rds_infer_engine_validate_tensor_size (const gsize * dims, guint n_dims,
    guint batch_size, gsize max_elements, guint * narrowed_dims, gsize * elements, gsize * bytes, GError ** error);

/* Loads a matching cached program (engine_file first, then the cache name in the model's directory, then in
 * $XDG_CACHE_HOME/redstream/) or builds one and saves it under the cache name. With no_build, the same lookup
 * decides whether the program is up to date (rds-compile). The model's input must be a float NCHW tensor whose batch
 * dimension is symbolic (or fixed at batch_size). Ends with one warm-up run. */
G_GNUC_INTERNAL RdsInferEngine *rds_infer_engine_open (const RdsInferEngineOptions * opts, GError ** error);

/* The .mxr loaded or written; NULL when a built program couldn't be saved (a warning says why). */
G_GNUC_INTERNAL const gchar *rds_infer_engine_get_program_path (RdsInferEngine * e);
/* Its manifest (program path + ".json"), or NULL with the program path. */
G_GNUC_INTERNAL const gchar *rds_infer_engine_get_manifest_path (RdsInferEngine * e);
/* FALSE: loaded from the cache. */
G_GNUC_INTERNAL gboolean rds_infer_engine_was_built (RdsInferEngine * e);
/* Why the program was built (no cached program, which manifest field differs, forced, ...); NULL when loaded. */
G_GNUC_INTERNAL const gchar *rds_infer_engine_get_build_reason (RdsInferEngine * e);
/* Warnings (strings), e.g. an ignored TensorRT engine file or a cache that couldn't be written. Never NULL. */
G_GNUC_INTERNAL GPtrArray *rds_infer_engine_get_warnings (RdsInferEngine * e);

G_GNUC_INTERNAL const gchar *rds_infer_engine_get_input_name (RdsInferEngine * e);
/* The per-image input dims; the input buffer holds batch_size * c * h * w floats, NCHW. */
G_GNUC_INTERNAL void rds_infer_engine_get_input (RdsInferEngine * e, guint * c, guint * h, guint * w);
G_GNUC_INTERNAL guint rds_infer_engine_get_n_outputs (RdsInferEngine * e);
/* Output @i's name and per-image dims (the batch dim stripped, also for a model exported with a fixed batch of
 * 1): [batch, 84, 8400] gives ndims 2, dims {84, 8400}. */
G_GNUC_INTERNAL void rds_infer_engine_get_output (RdsInferEngine * e, guint i, const gchar ** name, guint dims[8],
    guint * ndims);
/* Output @i's per-image element count (floats); its buffer holds batch_size times that. */
G_GNUC_INTERNAL gsize rds_infer_engine_get_output_elements (RdsInferEngine * e, guint i);

/* Enqueues one run on @stream (a hipStream_t; not the null stream) and returns without waiting. @input and
 * @outputs[get_n_outputs] are device buffers of batch_size images; MIGraphX orders the run after earlier work on
 * @stream, and later work on @stream after it. The buffers must stay valid until @stream has passed the run, and
 * the engine must not be freed while a run is in flight (synchronize @stream first). The program's internal
 * scratch memory is shared by all runs of one engine, so runs of one engine on different streams must not overlap:
 * use one stream per engine. */
G_GNUC_INTERNAL gboolean rds_infer_engine_run_async (RdsInferEngine * e, const float *input, float *const *outputs,
    void *stream, GError ** error);
G_GNUC_INTERNAL void rds_infer_engine_free (RdsInferEngine * e);

G_END_DECLS

#endif /* RDS_INFER_ENGINE_H */
