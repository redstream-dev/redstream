/* rds-compile: builds rdsinfer's compiled MIGraphX program (.mxr + .mxr.json manifest) ahead of time, so the element
 * loads it instead of compiling on its first start; trtexec's role for nvinfer's TensorRT engines. It reads the
 * config with the element's parser and builds with the element's engine set, so the programs land where the element
 * looks for them (or at -o) and the element accepts them. With dynamic-batch (ours) the set is one program per size
 * (see rds-infer-engine-set.h), and every one is built.
 *
 * Exit status: 0 built (at least one program), 1 error, 2 already up to date (the element would load every program;
 * --force rebuilds). The programs' paths go to stdout, one per line in ascending batch size (the batch-size one
 * last); the build reasons, warnings and errors to stderr.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-config.h"
#include "rds-infer-engine.h"
#include "rds-infer-engine-set.h"

#define PROG "rds-compile"

enum
{
  EXIT_BUILT = 0,
  EXIT_ERROR = 1,
  EXIT_UP_TO_DATE = 2,
};

/* An integer option and whether it was given (any value can be typed, so none can mean "unset") */
typedef struct
{
  gint value;
  gboolean set;
} IntOption;

static IntOption batch_opt, gpu_opt;

static gboolean
parse_int_option (const gchar * name, const gchar * value, gpointer data, GError ** error)
{
  IntOption *o = g_str_equal (name, "-b") || g_str_equal (name, "--batch-size") ? &batch_opt : &gpu_opt;
  gint64 v;

  (void) data;
  if (!g_ascii_string_to_signed (value, 10, G_MININT, G_MAXINT, &v, NULL)) {
    g_set_error (error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Cannot parse integer value \"%s\" for %s", value,
        name);
    return FALSE;
  }
  o->value = (gint) v;
  o->set = TRUE;
  return TRUE;
}

static void
print_warnings (GPtrArray * warnings)
{
  for (guint i = 0; i < warnings->len; i++)
    g_printerr (PROG ": warning: %s\n", (const gchar *) g_ptr_array_index (warnings, i));
}

/* The set's warnings as the element posts them: the fallback (naming --dynamic-batch when it set the key), then each
 * program's, the smaller programs' prefixed with their size */
static void
print_set_warnings (const RdsInferConfig * cfg, gboolean dynamic_opt, RdsInferEngineSet * set)
{
  const gchar *fallback = rds_infer_engine_set_get_fallback_reason (set);
  RdsInferEngine *main_engine = rds_infer_engine_set_get_main (set);
  guint n = rds_infer_engine_set_get_n (set);

  if (fallback && dynamic_opt)
    g_printerr (PROG ": warning: --dynamic-batch: %s\n", fallback);
  else if (fallback)
    g_printerr (PROG ": warning: %s:dynamic-batch: %s\n", cfg->config_path, fallback);
  for (guint p = 0; p < n; p++) {
    RdsInferEngine *e = rds_infer_engine_set_get (set, p);
    GPtrArray *warnings = rds_infer_engine_get_warnings (e);

    for (guint i = 0; i < warnings->len; i++) {
      if (e == main_engine)
        g_printerr (PROG ": warning: %s\n", (const gchar *) g_ptr_array_index (warnings, i));
      else
        g_printerr (PROG ": warning: batch %u program: %s\n", rds_infer_engine_set_get_size (set, p),
            (const gchar *) g_ptr_array_index (warnings, i));
    }
  }
}

/* Every program's path on stdout, in ascending batch size */
static void
print_paths (RdsInferEngineSet * set)
{
  for (guint p = 0; p < rds_infer_engine_set_get_n (set); p++)
    g_print ("%s\n", rds_infer_engine_get_program_path (rds_infer_engine_set_get (set, p)));
}

/* An engine error as the element reports it */
static void
print_engine_error (const RdsInferConfig * cfg, GError * err)
{
  gchar *msg = rds_infer_config_format_engine_error (cfg, err);

  g_printerr (PROG ": %s\n", msg);
  g_free (msg);
}

int
main (int argc, char **argv)
{
  gchar *config = NULL, *output = NULL;
  gboolean fp16 = FALSE, fp32 = FALSE, force = FALSE, dynamic = FALSE;
  const GOptionEntry entries[] = {
    {"config", 'c', 0, G_OPTION_ARG_FILENAME, &config, "The rdsinfer (nvinfer) config file", "CONFIG"},
    {"batch-size", 'b', 0, G_OPTION_ARG_CALLBACK, (gpointer) parse_int_option, "Override batch-size", "BATCH"},
    {"fp16", 0, 0, G_OPTION_ARG_NONE, &fp16, "Build for FP16 (network-mode=2)", NULL},
    {"fp32", 0, 0, G_OPTION_ARG_NONE, &fp32, "Build for FP32 (network-mode=0)", NULL},
    {"gpu-id", 'g', 0, G_OPTION_ARG_CALLBACK, (gpointer) parse_int_option, "Override gpu-id", "GPU"},
    {"dynamic-batch", 0, 0, G_OPTION_ARG_NONE, &dynamic,
        "Also build a program for each power of two below the batch size (dynamic-batch=1)", NULL},
    {"output", 'o', 0, G_OPTION_ARG_FILENAME, &output,
        "Write the program here (and its manifest to OUT.mxr.json) instead of the cache name; with dynamic-batch, "
        "only the batch-size one (the smaller ones keep their cache names)", "OUT.mxr"},
    {"force", 0, 0, G_OPTION_ARG_NONE, &force, "Rebuild even when the program is up to date", NULL},
    G_OPTION_ENTRY_NULL
  };
  const gchar *overridden[5];
  guint n_over = 0;
  GOptionContext *ctx;
  RdsInferConfig *cfg = NULL;
  RdsInferEngineOptions opts;
  RdsInferEngineSet *set = NULL;
  guint dims[3], n;
  GError *err = NULL;
  int ret = EXIT_ERROR;

  ctx = g_option_context_new ("- build rdsinfer's compiled model program ahead of time");
  g_option_context_add_main_entries (ctx, entries, NULL);
  g_option_context_set_description (ctx, "Prints the programs' paths, one per line, the batch-size one last.\n"
      "Exit status: 0 built (at least one program), 1 error, 2 already up to date (every program).\n");
  if (!g_option_context_parse (ctx, &argc, &argv, &err)) {
    g_printerr (PROG ": %s (see --help)\n", err->message);
    goto out;
  }
  if (argc > 1) {
    g_printerr (PROG ": unexpected argument %s (see --help)\n", argv[1]);
    goto out;
  }
  if (!config) {
    g_printerr (PROG ": -c CONFIG is required (see --help)\n");
    goto out;
  }
  if (fp16 && fp32) {
    g_printerr (PROG ": --fp16 and --fp32 exclude each other\n");
    goto out;
  }
  if (batch_opt.set && (batch_opt.value < 1 || batch_opt.value > RDS_INFER_MAX_BATCH_SIZE)) {
    g_printerr (PROG ": -b %d: the batch size must be 1..%d\n", batch_opt.value, RDS_INFER_MAX_BATCH_SIZE);
    goto out;
  }
  if (gpu_opt.set && gpu_opt.value < 0) {
    g_printerr (PROG ": -g %d: the GPU id must be >= 0\n", gpu_opt.value);
    goto out;
  }
  if (output && !g_str_has_suffix (output, ".mxr"))
    g_printerr (PROG ": warning: -o %s: not a .mxr name; the program is written there anyway\n", output);

  /* The element's steps: load, apply the overrides, validate, check the mode. */
  if (!(cfg = rds_infer_config_load (config, &err))) {
    g_printerr (PROG ": %s\n", err->message);
    goto out;
  }
  if (batch_opt.set) {
    cfg->batch_size = (guint) batch_opt.value;
    overridden[n_over++] = "batch-size";
  }
  if (fp16 || fp32) {
    cfg->network_mode = fp16 ? 2 : 0;
    overridden[n_over++] = "network-mode";
  }
  if (gpu_opt.set) {
    cfg->gpu_id = (guint) gpu_opt.value;
    overridden[n_over++] = "gpu-id";
  }
  if (dynamic) {
    cfg->dynamic_batch = TRUE;
    overridden[n_over++] = "dynamic-batch";
  }
  overridden[n_over] = NULL;
  if (!rds_infer_config_validate (cfg, overridden, &err) || !rds_infer_config_check_mode (cfg, overridden, &err)) {
    g_printerr (PROG ": %s\n", err->message);
    goto out;
  }
  print_warnings (cfg->warnings);
  if (!rds_infer_config_get_engine_options (cfg, &opts, dims, &err)) {
    g_printerr (PROG ": %s\n", err->message);
    goto out;
  }
  opts.output_file = output;

  /* Up to date: the lookup the element runs finds every program it would load. This really opens the engines (loads
   * the programs and does the warm-up runs), on purpose: the check is the element's own lookup, not a copy of it.
   * When only some programs are cached, the build below loads those again. */
  if (!force) {
    opts.no_build = TRUE;
    if ((set = rds_infer_engine_set_open (&opts, cfg->dynamic_batch, &err))) {
      print_set_warnings (cfg, dynamic, set);
      for (guint p = 0; p < rds_infer_engine_set_get_n (set); p++)
        g_printerr (PROG ": %s is up to date (--force rebuilds it)\n",
            rds_infer_engine_get_program_path (rds_infer_engine_set_get (set, p)));
      print_paths (set);
      ret = EXIT_UP_TO_DATE;
      goto out;
    }
    if (!g_error_matches (err, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_NOT_CACHED)) {
      print_engine_error (cfg, err);
      goto out;
    }
    g_clear_error (&err);
    opts.no_build = FALSE;
  }
  opts.force_rebuild = force;
  g_printerr (PROG ": compiling %s (batch %u%s, %s); this can take minutes\n", opts.onnx_file, opts.batch_size,
      cfg->dynamic_batch && opts.batch_size > 1 ? ", dynamic-batch requested" : "", opts.fp16 ? "fp16" : "fp32");
  if (!(set = rds_infer_engine_set_open (&opts, cfg->dynamic_batch, &err))) {
    print_engine_error (cfg, err);
    goto out;
  }
  print_set_warnings (cfg, dynamic, set);
  n = rds_infer_engine_set_get_n (set);
  for (guint p = 0; p < n; p++) {
    RdsInferEngine *e = rds_infer_engine_set_get (set, p);

    if (!rds_infer_engine_get_program_path (e)) {
      if (p == n - 1)
        g_printerr (PROG ": the compiled program could not be saved\n");
      else
        g_printerr (PROG ": batch %u program: the compiled program could not be saved\n",
            rds_infer_engine_set_get_size (set, p));
      goto out;
    }
  }
  for (guint p = 0; p < n; p++) {
    RdsInferEngine *e = rds_infer_engine_set_get (set, p);

    if (rds_infer_engine_was_built (e))
      g_printerr (PROG ": built %s because %s\n", rds_infer_engine_get_program_path (e),
          rds_infer_engine_get_build_reason (e));
    else
      g_printerr (PROG ": %s is up to date\n", rds_infer_engine_get_program_path (e));
  }
  print_paths (set);
  ret = EXIT_BUILT;

out:
  g_clear_pointer (&set, rds_infer_engine_set_free);
  g_clear_pointer (&cfg, rds_infer_config_free);
  g_clear_error (&err);
  g_option_context_free (ctx);
  g_free (config);
  g_free (output);
  return ret;
}
