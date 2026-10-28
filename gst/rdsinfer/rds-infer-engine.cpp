/* Redstream: rdsinfer's MIGraphX engine and its compiled-program cache. No C++ exception leaves this file: every
 * MIGraphX call that can throw runs under RDS_MGX_CHECK, which turns the exception into a GError naming the call.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
/* GLib before HIP: HIP defines __noinline__, which breaks gmacros.h. */
#include "rds-infer-engine.h"
#include "rds-infer-onnx.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <hip/hip_runtime_api.h>
#include <migraphx/migraphx.hpp>
#include <migraphx/version.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <exception>
#include <memory>
#include <string>
#include <vector>

G_DEFINE_QUARK (rds-infer-engine-error-quark, rds_infer_engine_error)

gboolean
rds_infer_engine_validate_tensor_size (const gsize * dims, guint n_dims, guint batch_size,
    gsize max_elements, guint * narrowed_dims, gsize * elements, gsize * bytes, GError ** error)
{
  gsize count = 1;

  for (guint i = 0; i < n_dims; i++) {
    if (dims[i] == 0 || dims[i] > G_MAXUINT) {
      g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
          "tensor dimension %u (%" G_GSIZE_FORMAT ") must be nonzero and fit in guint", i, dims[i]);
      return FALSE;
    }
    if (count > G_MAXSIZE / dims[i] || count > max_elements / dims[i]) {
      g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
          "tensor element count exceeds the supported limit (%" G_GSIZE_FORMAT ")", max_elements);
      return FALSE;
    }
    count *= dims[i];
  }
  if (count > max_elements || batch_size == 0 || count > G_MAXSIZE / batch_size
      || count * batch_size > G_MAXSIZE / sizeof (gfloat)) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "tensor batch size %u or packed float32 byte size exceeds the supported limit", batch_size);
    return FALSE;
  }
  if (narrowed_dims)
    for (guint i = 0; i < n_dims; i++)
      narrowed_dims[i] = (guint) dims[i];
  if (elements)
    *elements = count;
  if (bytes)
    *bytes = count * batch_size * sizeof (gfloat);
  return TRUE;
}

/* Bump when the manifest's fields or their meaning change: older manifests then no longer match. */
#define MANIFEST_FORMAT 1
/* The MIGraphX headers this was built against (the library has no runtime version query); its soname carries
 * major.minor, so a different minor version doesn't load. */
#define MIGRAPHX_VERSION_STRING G_STRINGIFY (MIGRAPHX_VERSION_MAJOR) "." G_STRINGIFY (MIGRAPHX_VERSION_MINOR) "." \
    G_STRINGIFY (MIGRAPHX_VERSION_PATCH) "-" MIGRAPHX_VERSION_TWEAK
/* The compile options, fixed: recorded in the manifest. fast_math and exhaustive_tune have MIGraphX's defaults. */
#define COMPILE_FAST_MATH true
#define COMPILE_EXHAUSTIVE_TUNE false

#define MAX_DIMS 8

struct EngineOutput
{
  std::string name;             /* the ONNX graph output's name */
  std::string param;            /* main:#output_N */
  migraphx::shape shape;        /* with the batch dim */
  guint dims[MAX_DIMS];         /* per image */
  guint ndims;
  gsize elements;               /* per image, validated to fit RdsInferDims.numElements */
  gsize bytes;                  /* checked batch size in bytes */
};

struct _RdsInferEngine
{
  migraphx::program program;
  guint batch, gpu_id, c, h, w;
  gsize input_bytes;
  std::string input_name;
  migraphx::shape input_shape;
  std::vector<EngineOutput> outputs;
  gchar *program_path, *manifest_path, *build_reason;
  gboolean built;
  GPtrArray *warnings;
};

/* Runs @f (MIGraphX calls); an exception becomes a RDS_INFER_ENGINE_ERROR_MIGRAPHX naming @call. The C++ API's
 * own exceptions only say "Failed to call function": MIGraphX prints the real reason to stderr. */
template <class F>
static gboolean
mgx_try (const char *call, GError ** error, F && f)
{
  try {
    f ();
    return TRUE;
  }
  catch (const std::exception & ex) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MIGRAPHX,
        "%s failed: %s (MIGraphX prints the cause on stderr)", call, ex.what ());
  }
  catch (...) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MIGRAPHX, "%s failed: unknown exception",
        call);
  }
  return FALSE;
}

#define RDS_MGX_CHECK(call, error, ...) mgx_try (call, error, [&] () { __VA_ARGS__; })

static gboolean
hip_check (hipError_t err, const char *call, GError ** error)
{
  if (err == hipSuccess)
    return TRUE;
  g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_DEVICE, "%s failed: %s", call,
      hipGetErrorString (err));
  return FALSE;
}

#define HIP_CHECK(call, error) hip_check ((call), #call, error)

/* Makes gpu_id the calling thread's device for the scope and restores the previous one. */
class DeviceScope
{
public:
  explicit DeviceScope (guint gpu_id)
  {
    have_prev = hipGetDevice (&prev) == hipSuccess;
    err = hipSetDevice ((int) gpu_id);
  }
  ~DeviceScope ()
  {
    if (have_prev)
      (void) hipSetDevice (prev);
  }
  hipError_t err;

private:
  int prev = 0;
  bool have_prev;
};

static gboolean
device_scope_ok (const DeviceScope & scope, guint gpu_id, GError ** error)
{
  if (scope.err == hipSuccess)
    return TRUE;
  g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_DEVICE, "hipSetDevice (%u) failed: %s", gpu_id,
      hipGetErrorString (scope.err));
  return FALSE;
}

/* ----- the device's identity ----- */

typedef struct
{
  gchar *gfx;                   /* the device's own architecture, e.g. gfx1036 */
  gchar *arch;                  /* HIP's gcnArchName, with feature suffixes (what programs are compiled for) */
  gint hip_runtime;
  gchar *hsa_override;          /* HSA_OVERRIDE_GFX_VERSION, or NULL */
} DeviceInfo;

static void
device_info_clear (DeviceInfo * d)
{
  g_free (d->gfx);
  g_free (d->arch);
  g_free (d->hsa_override);
}

/* The real architecture from the KFD topology (gfx_target_version, e.g. 100306 for gfx1036), which
 * HSA_OVERRIDE_GFX_VERSION doesn't change, unlike gcnArchName. The node is matched by PCI location. NULL when not
 * found. */
static gchar *
kfd_gfx_name (const hipDeviceProp_t * prop)
{
  const gchar *base = "/sys/class/kfd/kfd/topology/nodes";
  GDir *dir = g_dir_open (base, 0, NULL);
  const gchar *node;
  gchar *result = NULL;

  if (!dir)
    return NULL;
  while (!result && (node = g_dir_read_name (dir))) {
    gchar *path = g_build_filename (base, node, "properties", NULL), *contents = NULL;
    guint64 version = 0, location = G_MAXUINT64, domain = 0;    /* no domain property: domain 0 */

    if (g_file_get_contents (path, &contents, NULL, NULL)) {
      gchar **lines = g_strsplit (contents, "\n", -1);

      for (gchar ** l = lines; *l; l++) {
        gchar **kv = g_strsplit (*l, " ", 2);

        if (kv[0] && kv[1]) {
          if (strcmp (kv[0], "gfx_target_version") == 0)
            version = g_ascii_strtoull (kv[1], NULL, 10);
          else if (strcmp (kv[0], "location_id") == 0)
            location = g_ascii_strtoull (kv[1], NULL, 10);
          else if (strcmp (kv[0], "domain") == 0)
            domain = g_ascii_strtoull (kv[1], NULL, 10);
        }
        g_strfreev (kv);
      }
      g_strfreev (lines);
    }
    /* location_id is bus << 8 | device << 3 | function. */
    if (version != 0 && domain == (guint64) prop->pciDomainID && (location >> 8) == (guint64) prop->pciBusID
        && ((location >> 3) & 0x1f) == (guint64) prop->pciDeviceID)
      result = g_strdup_printf ("gfx%u%u%x", (guint) (version / 10000), (guint) (version / 100 % 100),
          (guint) (version % 100));
    g_free (contents);
    g_free (path);
  }
  g_dir_close (dir);
  return result;
}

static gboolean
device_info_get (guint gpu_id, DeviceInfo * d, GError ** error)
{
  hipDeviceProp_t prop;

  memset (d, 0, sizeof *d);
  if (!HIP_CHECK (hipGetDeviceProperties (&prop, (int) gpu_id), error)
      || !HIP_CHECK (hipRuntimeGetVersion (&d->hip_runtime), error))
    return FALSE;
  d->arch = g_strdup (prop.gcnArchName);
  d->gfx = kfd_gfx_name (&prop);
  if (!d->gfx)
    d->gfx = g_strndup (d->arch, strcspn (d->arch, ":"));
  d->hsa_override = g_strdup (g_getenv ("HSA_OVERRIDE_GFX_VERSION"));
  return TRUE;
}

/* ----- the manifest ----- */

static JsonNode *
shape_node (const std::vector<size_t> &lens)
{
  JsonArray *a = json_array_new ();
  JsonNode *n = json_node_new (JSON_NODE_ARRAY);

  for (size_t l : lens)
    json_array_add_int_element (a, (gint64) l);
  json_node_take_array (n, a);
  return n;
}

/* Everything a cached program must match; the outputs are added when a program is written. */
static JsonObject *
manifest_key (const gchar * sha256, const DeviceInfo * dev, gboolean fp16, guint batch, const gchar * input_name,
    const std::vector<size_t> &input_lens)
{
  JsonObject *o = json_object_new (), *input = json_object_new (), *opts = json_object_new ();

  json_object_set_int_member (o, "format", MANIFEST_FORMAT);
  json_object_set_string_member (o, "onnx_sha256", sha256);
  json_object_set_string_member (o, "migraphx_version", MIGRAPHX_VERSION_STRING);
  json_object_set_int_member (o, "hip_runtime_version", dev->hip_runtime);
  json_object_set_string_member (o, "gfx", dev->gfx);
  json_object_set_string_member (o, "arch", dev->arch);
  if (dev->hsa_override)
    json_object_set_string_member (o, "hsa_override_gfx_version", dev->hsa_override);
  else
    json_object_set_null_member (o, "hsa_override_gfx_version");
  json_object_set_string_member (o, "precision", fp16 ? "fp16" : "fp32");
  json_object_set_int_member (o, "batch_size", batch);
  json_object_set_string_member (input, "name", input_name);
  json_object_set_member (input, "shape", shape_node (input_lens));
  json_object_set_object_member (o, "input", input);
  json_object_set_boolean_member (opts, "offload_copy", FALSE);
  json_object_set_boolean_member (opts, "fast_math", COMPILE_FAST_MATH);
  json_object_set_boolean_member (opts, "exhaustive_tune", COMPILE_EXHAUSTIVE_TUNE);
  json_object_set_object_member (o, "compile_options", opts);
  return o;
}

static gchar *
node_text (JsonNode * n)
{
  return n ? json_to_string (n, FALSE) : g_strdup ("nothing");
}

/* NULL when every member of @key is in @stored with the same value, else the first difference. */
static gchar *
manifest_mismatch (JsonObject * key, JsonObject * stored)
{
  GList *members = json_object_get_members (key);
  gchar *reason = NULL;

  for (GList * l = members; l && !reason; l = l->next) {
    const gchar *name = (const gchar *) l->data;
    JsonNode *want = json_object_get_member (key, name), *have = json_object_get_member (stored, name);

    if (!have || !json_node_equal (want, have)) {
      gchar *w = node_text (want), *h = node_text (have);

      reason = g_strdup_printf ("%s is %s, expected %s", name, h, w);
      g_free (w);
      g_free (h);
    }
  }
  g_list_free (members);
  return reason;
}

/* ----- files ----- */

static gboolean
fsync_path (const gchar * path)
{
  int fd = g_open (path, O_RDONLY, 0);
  gboolean ok;

  if (fd < 0)
    return FALSE;
  ok = fsync (fd) == 0;
  close (fd);
  return ok;
}

static void
set_errno_error (GError ** error, int e, const gchar * what, const gchar * path)
{
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (e), "can't %s %s: %s", what, path, g_strerror (e));
}

/* Saves the program to @path and then the manifest to PATH.json. The program goes to a unique temporary file
 * (PATH.XXXXXX) that is synced and renamed, then the directory is synced; the manifest is written the same way by
 * g_file_set_contents_full. So a manifest only ever describes a complete program, and when two processes build the
 * same entry, the last rename wins with a complete pair. */
static gboolean
save_program (const migraphx::program & program, JsonObject * manifest, const gchar * path, GError ** error)
{
  gchar *tmp = g_strconcat (path, ".XXXXXX", NULL), *json_path = g_strconcat (path, ".json", NULL);
  gchar *dir = g_path_get_dirname (path), *text;
  JsonNode *root = json_node_new (JSON_NODE_OBJECT);
  JsonGenerator *gen = json_generator_new ();
  gboolean ok = FALSE, have_tmp = FALSE;
  gsize len;
  int fd;

  /* A manifest left over from an older program must not describe the new one if we stop half-way. */
  if (g_unlink (json_path) != 0 && errno != ENOENT) {
    set_errno_error (error, errno, "remove", json_path);
    goto out;
  }
  if ((fd = g_mkstemp_full (tmp, O_RDWR, 0644)) < 0) {
    set_errno_error (error, errno, "create", tmp);
    goto out;
  }
  close (fd);
  have_tmp = TRUE;
  if (!RDS_MGX_CHECK ("migraphx::save", error, migraphx::save (program, tmp)))
    goto out;
  if (!fsync_path (tmp)) {
    set_errno_error (error, errno, "sync", tmp);
    goto out;
  }
  if (g_rename (tmp, path) != 0) {
    set_errno_error (error, errno, "rename to", path);
    goto out;
  }
  have_tmp = FALSE;
  if (!fsync_path (dir)) {
    set_errno_error (error, errno, "sync", dir);
    goto out;
  }
  json_node_set_object (root, manifest);
  json_generator_set_root (gen, root);
  json_generator_set_pretty (gen, TRUE);
  text = json_generator_to_data (gen, &len);
  ok = g_file_set_contents_full (json_path, text, len,
      (GFileSetContentsFlags) (G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE), 0644, error);
  g_free (text);

out:
  if (have_tmp)
    g_unlink (tmp);
  g_object_unref (gen);
  json_node_unref (root);
  g_free (dir);
  g_free (json_path);
  g_free (tmp);
  return ok;
}

/* The directory exists (made if needed) and a file can be created in it. */
static gboolean
dir_writable (const gchar * dir)
{
  return g_mkdir_with_parents (dir, 0755) == 0 && access (dir, W_OK | X_OK) == 0;
}

/* ----- the program's parameters ----- */

static gchar *
lens_text (const std::vector<size_t> &lens)
{
  GString *s = g_string_new ("[");

  for (size_t i = 0; i < lens.size (); i++)
    g_string_append_printf (s, "%s%" G_GSIZE_FORMAT, i ? ", " : "", (gsize) lens[i]);
  g_string_append_c (s, ']');
  return g_string_free (s, FALSE);
}

/* Fills e->input_shape and e->outputs from the compiled program: one float input of the expected shape and float
 * outputs main:#output_0..N-1 whose first dim is the batch. @names: the ONNX graph outputs. */
static gboolean
read_parameters (RdsInferEngine * e, const std::vector<size_t> &input_lens, const std::vector<std::string> &names,
    GError ** error)
{
  migraphx::program_parameter_shapes shapes;
  std::vector<const char *> params;
  guint n_outputs = 0;

  if (!RDS_MGX_CHECK ("migraphx::program::get_parameter_shapes", error,
          shapes = e->program.get_parameter_shapes (); params = shapes.names ()))
    return FALSE;
  for (const char *p : params)
    if (g_str_has_prefix (p, "main:#output_"))
      n_outputs++;
  if (n_outputs == 0 || params.size () != n_outputs + 1) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "the compiled program has %" G_GSIZE_FORMAT " parameters for %u outputs; expected one input",
        (gsize) params.size (), n_outputs);
    return FALSE;
  }
  e->outputs.clear ();
  e->outputs.resize (n_outputs);
  for (const char *p : params) {
    migraphx::shape s;
    std::vector<size_t> lens;
    migraphx_shape_datatype_t type;
    gboolean standard;
    gchar *text;
    guint i;

    if (!RDS_MGX_CHECK ("migraphx::shape", error,
            s = shapes[p]; lens = s.lengths (); type = s.type (); standard = s.standard ()))
      return FALSE;
    text = lens_text (lens);
    if (type != migraphx_shape_float_type || !standard) {
      g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
          "parameter %s %s is not a packed float32 tensor", p, text);
      g_free (text);
      return FALSE;
    }
    if (!g_str_has_prefix (p, "main:#output_")) {
      if (p != e->input_name || lens != input_lens) {
        gchar *want = lens_text (input_lens);

        g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
            "the compiled program's input is %s %s, expected %s %s", p, text, e->input_name.c_str (), want);
        g_free (want);
        g_free (text);
        return FALSE;
      }
      e->input_shape = s;
      g_free (text);
      continue;
    }
    i = (guint) g_ascii_strtoull (p + strlen ("main:#output_"), NULL, 10);
    if (i >= n_outputs || !e->outputs[i].param.empty () || lens.empty () || lens[0] != e->batch
        || lens.size () > MAX_DIMS + 1) {
      g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
          "unexpected output parameter %s %s (the first dimension must be the batch size %u, at most %u more)", p,
          text, e->batch, MAX_DIMS);
      g_free (text);
      return FALSE;
    }
    g_free (text);
    guint dims[MAX_DIMS];
    gsize elements, bytes;
    guint ndims = (guint) lens.size () - 1;

    if (!rds_infer_engine_validate_tensor_size (lens.data () + 1, ndims, e->batch, G_MAXUINT, dims,
            &elements, &bytes, error))
      return FALSE;
    EngineOutput & o = e->outputs[i];
    o.param = p;
    o.shape = s;
    /* MIGraphX keeps the graph's output order. */
    if (n_outputs == names.size ())
      o.name = names[i];
    else
      o.name = "output_" + std::to_string (i);
    o.ndims = ndims;
    memcpy (o.dims, dims, ndims * sizeof (guint));
    o.elements = elements;
    o.bytes = bytes;
  }
  return TRUE;
}

static JsonNode *
outputs_node (RdsInferEngine * e)
{
  JsonArray *a = json_array_new ();
  JsonNode *n = json_node_new (JSON_NODE_ARRAY);

  for (const EngineOutput & o:e->outputs) {
    JsonObject *obj = json_object_new ();
    std::vector<size_t> lens { e->batch };

    for (guint d = 0; d < o.ndims; d++)
      lens.push_back (o.dims[d]);
    json_object_set_string_member (obj, "name", o.name.c_str ());
    json_object_set_member (obj, "shape", shape_node (lens));
    json_array_add_object_element (a, obj);
  }
  json_node_take_array (n, a);
  return n;
}

/* ----- open ----- */

/* The input dims [batch, C, H, W] from the model and the options. */
static gboolean
input_lens_get (const RdsInferOnnxValue * in, const RdsInferEngineOptions * opts, std::vector<size_t> &lens,
    GError ** error)
{
  static const char *const axis[] = { "batch", "channels", "height", "width" };

  if (in->elem_type != RDS_INFER_ONNX_FLOAT) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "input %s: element type %d is not float32", in->name, in->elem_type);
    return FALSE;
  }
  if (!in->has_shape || in->n_dims != 4) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "input %s: expected 4 dimensions (batch, channels, height, width), the model declares %s", in->name,
        in->has_shape ? "another rank" : "no shape");
    return FALSE;
  }
  lens.assign (4, 0);
  if (in->dims[0] > 0 && (guint64) in->dims[0] != opts->batch_size) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "input %s: the model's batch size is fixed at %" G_GINT64_FORMAT ", not %u; export the model with a "
        "dynamic batch dimension", in->name, in->dims[0], opts->batch_size);
    return FALSE;
  }
  lens[0] = opts->batch_size;
  for (guint i = 1; i < 4; i++) {
    gint64 model = in->dims[i];

    if (opts->infer_dims) {
      if (model > 0 && (guint64) model != opts->infer_dims[i - 1]) {
        g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
            "input %s: infer-dims %u;%u;%u don't match the model's fixed %s %" G_GINT64_FORMAT, in->name,
            opts->infer_dims[0], opts->infer_dims[1], opts->infer_dims[2], axis[i], model);
        return FALSE;
      }
      lens[i] = opts->infer_dims[i - 1];
    } else if (model > 0) {
      if ((guint64) model > G_MAXSIZE) {
        g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
            "input %s: dimension %u exceeds the supported size", in->name, i);
        return FALSE;
      }
      lens[i] = (size_t) model;
    } else {
      g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
          "input %s: dimension %u (%s) is %s%s%s in the model; set infer-dims", in->name, i, axis[i],
          in->dim_params[i] ? "symbolic (\"" : "not fixed", in->dim_params[i] ? in->dim_params[i] : "",
          in->dim_params[i] ? "\")" : "");
      return FALSE;
    }
  }
  return TRUE;
}

/* Loads @path if its manifest matches @key and the program has the expected parameters. FALSE with *reason set
 * otherwise. */
static gboolean
try_load (RdsInferEngine * e, const gchar * path, JsonObject * key, const std::vector<size_t> &input_lens,
    const std::vector<std::string> &names, gchar ** reason)
{
  gchar *json_path = g_strconcat (path, ".json", NULL), *mismatch;
  JsonParser *parser = NULL;
  JsonNode *root;
  GError *err = NULL;
  gboolean ok = FALSE;

  if (!g_file_test (path, G_FILE_TEST_IS_REGULAR)) {
    *reason = g_strdup_printf ("%s doesn't exist", path);
    goto out;
  }
  if (!g_file_test (json_path, G_FILE_TEST_IS_REGULAR)) {
    *reason = g_strdup_printf ("%s has no manifest (%s)", path, json_path);
    goto out;
  }
  parser = json_parser_new ();
  if (!json_parser_load_from_mapped_file (parser, json_path, &err)) {
    *reason = g_strdup_printf ("can't read the manifest %s: %s", json_path, err->message);
    g_error_free (err);
    goto out;
  }
  root = json_parser_get_root (parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT (root)) {
    *reason = g_strdup_printf ("the manifest %s is not a JSON object", json_path);
    goto out;
  }
  mismatch = manifest_mismatch (key, json_node_get_object (root));
  if (mismatch) {
    *reason = g_strdup_printf ("%s doesn't match: %s", json_path, mismatch);
    g_free (mismatch);
    goto out;
  }
  if (!RDS_MGX_CHECK ("migraphx::load", &err, e->program = migraphx::load (path))
      || !read_parameters (e, input_lens, names, &err)) {
    *reason = g_strdup_printf ("can't use %s: %s", path, err->message);
    g_error_free (err);
    goto out;
  }
  /* The program's outputs must be the manifest's: a manifest copied next to another program doesn't match. */
  {
    JsonNode *want = outputs_node (e), *have = json_object_get_member (json_node_get_object (root), "outputs");

    if (!have || !json_node_equal (want, have)) {
      gchar *w = node_text (want), *h = node_text (have);

      *reason = g_strdup_printf ("%s doesn't match: outputs is %s, the program has %s", json_path, h, w);
      g_free (w);
      g_free (h);
    } else {
      ok = TRUE;
    }
    json_node_unref (want);
  }

out:
  if (parser)
    g_object_unref (parser);
  g_free (json_path);
  return ok;
}

static gboolean
build_program (RdsInferEngine * e, const gchar * onnx, gsize onnx_len, const gchar * model_dir,
    const std::vector<size_t> &input_lens, gboolean fp16, GError ** error)
{
  /* Models whose initializers use external data are refused before this, but the reader scans initializers only:
   * external tensors elsewhere (Constant node attributes, sparse initializers, subgraphs) are resolved against the
   * model's directory, as they would be for a file path. */
  return RDS_MGX_CHECK ("migraphx::parse_onnx_buffer", error,
      migraphx::onnx_options o;
      o.set_input_parameter_shape (e->input_name, input_lens);
      o.set_external_data_path (model_dir);
      e->program = migraphx::parse_onnx_buffer (onnx, onnx_len, o))
      && (!fp16 || RDS_MGX_CHECK ("migraphx::quantize_fp16", error, migraphx::quantize_fp16 (e->program)))
      && RDS_MGX_CHECK ("migraphx::program::compile", error,
      migraphx::compile_options co;
      co.set_offload_copy (false);
      co.set_fast_math (COMPILE_FAST_MATH);
      co.set_exhaustive_tune_flag (COMPILE_EXHAUSTIVE_TUNE);
      e->program.compile (migraphx::target ("gpu"), co));
}

static gboolean
run_async (RdsInferEngine * e, const float *input, float *const *outputs, hipStream_t stream, GError ** error)
{
  return RDS_MGX_CHECK ("migraphx::program::run_async", error,
      migraphx::program_parameters params;
      params.add (e->input_name.c_str (), migraphx::argument (e->input_shape, const_cast < float *>(input)));
      for (size_t i = 0; i < e->outputs.size (); i++)
        params.add (e->outputs[i].param.c_str (), migraphx::argument (e->outputs[i].shape, outputs[i]));
      e->program.run_async (params, stream));
}

/* One run on zeros, on a stream of our own, synchronized: the first run after loading takes longer. */
static gboolean
warm_up (RdsInferEngine * e, GError ** error)
{
  std::vector<float *> outs (e->outputs.size (), nullptr);
  float *input = nullptr;
  hipStream_t stream = nullptr;
  gsize in_bytes = e->input_bytes;
  gboolean ok = HIP_CHECK (hipStreamCreateWithFlags (&stream, hipStreamNonBlocking), error)
      && HIP_CHECK (hipMalloc ((void **) &input, in_bytes), error)
      && HIP_CHECK (hipMemsetAsync (input, 0, in_bytes, stream), error);

  for (size_t i = 0; ok && i < outs.size (); i++)
    ok = HIP_CHECK (hipMalloc ((void **) &outs[i], e->outputs[i].bytes), error);
  ok = ok && run_async (e, input, outs.data (), stream, error)
      && HIP_CHECK (hipStreamSynchronize (stream), error);
  for (float *o : outs)
    if (o)
      (void) hipFree (o);
  if (input)
    (void) hipFree (input);
  if (stream)
    (void) hipStreamDestroy (stream);
  return ok;
}

static gchar *
file_sha256 (const gchar * data, gsize len)
{
  return g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) data, len);
}

/* What engine_open owns while it runs; freed on every return, also when an exception unwinds. */
struct OpenState
{
  gchar *onnx = NULL, *sha256 = NULL, *dir = NULL, *stem = NULL, *name = NULL, *xdg_dir = NULL;
  gchar *candidates[2] = { NULL, NULL };
  gsize onnx_len = 0;
  RdsInferOnnxInfo *info = NULL;
  DeviceInfo dev = { NULL, NULL, 0, NULL };
  JsonObject *key = NULL;

  ~OpenState ()
  {
    if (key)
      json_object_unref (key);
    for (gchar * c : candidates)
      g_free (c);
    g_free (xdg_dir);
    g_free (name);
    g_free (stem);
    g_free (dir);
    g_free (sha256);
    rds_infer_onnx_info_free (info);
    g_free (onnx);
    device_info_clear (&dev);
  }
};

struct EngineFree
{
  void operator () (RdsInferEngine * e) const
  {
    rds_infer_engine_free (e);
  }
};

/* A GError from @err, prefixed with the model file, in RDS_INFER_ENGINE_ERROR_@code. */
static void
model_error (GError ** error, RdsInferEngineError code, const gchar * onnx_file, GError * err)
{
  g_set_error (error, RDS_INFER_ENGINE_ERROR, code, "%s: %s", onnx_file, err->message);
  g_error_free (err);
}

static RdsInferEngine *
engine_open (const RdsInferEngineOptions * opts, GError ** error)
{
  OpenState st;
  std::unique_ptr < RdsInferEngine, EngineFree > e;
  RdsInferOnnxValue *in;
  std::vector<size_t> input_lens;
  guint input_dims[3];
  gsize input_bytes;
  std::vector<std::string> names;
  std::string reason;
  /* With output_file, only that file is looked up and written. */
  const gchar *engine_file = opts && !opts->output_file ? opts->engine_file : NULL;
  gboolean need_build = TRUE, is_trt;
  int n_devices = 0;
  GError *err = NULL;

  if (!opts || !opts->onnx_file || opts->batch_size == 0
      || (opts->infer_dims && (!opts->infer_dims[0] || !opts->infer_dims[1] || !opts->infer_dims[2]))) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID,
        "invalid engine options: %s", !opts || !opts->onnx_file ? "no ONNX file" : opts->batch_size == 0 ?
        "batch size 0" : "an infer-dims value is 0");
    return NULL;
  }
  if (opts->force_rebuild && opts->no_build) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID,
        "invalid engine options: force_rebuild and no_build together");
    return NULL;
  }
  if (!HIP_CHECK (hipGetDeviceCount (&n_devices), error))
    return NULL;
  if (opts->gpu_id >= (guint) n_devices) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_DEVICE,
        "gpu-id %u: no such GPU (%d ROCm device%s visible)", opts->gpu_id, n_devices, n_devices == 1 ? "" : "s");
    return NULL;
  }
  DeviceScope scope (opts->gpu_id);
  if (!device_scope_ok (scope, opts->gpu_id, error) || !device_info_get (opts->gpu_id, &st.dev, error))
    return NULL;

  if (!g_file_get_contents (opts->onnx_file, &st.onnx, &st.onnx_len, &err)) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_FILE, "%s", err->message);
    g_error_free (err);
    return NULL;
  }
  if (!(st.info = rds_infer_onnx_read_info ((const guint8 *) st.onnx, st.onnx_len, &err))) {
    model_error (error, RDS_INFER_ENGINE_ERROR_MODEL, opts->onnx_file, err);
    return NULL;
  }
  /* The manifest hashes the .onnx file only, so weights in other files could change unnoticed. */
  if (st.info->external_tensor) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "%s: initializer \"%s\" keeps its data in an external file; models with external data are not supported "
        "(export the model with its weights inside the .onnx file)", opts->onnx_file, st.info->external_tensor);
    return NULL;
  }
  if (st.info->inputs->len != 1) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL,
        "%s: the model has %u inputs; only models with one input are supported", opts->onnx_file,
        st.info->inputs->len);
    return NULL;
  }
  in = (RdsInferOnnxValue *) g_ptr_array_index (st.info->inputs, 0);
  if (!input_lens_get (in, opts, input_lens, &err)
      || !rds_infer_engine_validate_tensor_size (input_lens.data () + 1, 3, opts->batch_size, G_MAXSIZE,
          input_dims, NULL, &input_bytes, &err)) {
    model_error (error, RDS_INFER_ENGINE_ERROR_MODEL, opts->onnx_file, err);
    return NULL;
  }
  for (guint i = 0; i < st.info->outputs->len; i++)
    names.push_back (((RdsInferOnnxValue *) g_ptr_array_index (st.info->outputs, i))->name);

  e.reset (new _RdsInferEngine ());
  e->warnings = g_ptr_array_new_with_free_func (g_free);
  e->batch = opts->batch_size;
  e->gpu_id = opts->gpu_id;
  e->c = input_dims[0];
  e->h = input_dims[1];
  e->w = input_dims[2];
  e->input_bytes = input_bytes;
  e->input_name = in->name;

  st.sha256 = file_sha256 (st.onnx, st.onnx_len);
  st.key = manifest_key (st.sha256, &st.dev, opts->fp16, opts->batch_size, in->name, input_lens);

  /* The cache name, in the model's directory and in the user cache directory. */
  st.dir = g_path_get_dirname (opts->onnx_file);
  st.stem = g_path_get_basename (opts->onnx_file);
  if (g_str_has_suffix (st.stem, ".onnx"))
    st.stem[strlen (st.stem) - strlen (".onnx")] = '\0';
  st.name = g_strdup_printf ("%s_b%u_%ux%ux%u_%s_%s.mxr", st.stem, opts->batch_size, e->c, e->h, e->w, st.dev.gfx,
      opts->fp16 ? "fp16" : "fp32");
  st.xdg_dir = g_build_filename (g_get_user_cache_dir (), "redstream", NULL);
  st.candidates[0] = g_build_filename (st.dir, st.name, NULL);
  st.candidates[1] = g_build_filename (st.xdg_dir, st.name, NULL);

  is_trt = engine_file && g_str_has_suffix (engine_file, ".engine");
  if (is_trt)
    g_ptr_array_add (e->warnings, g_strdup_printf ("model-engine-file %s is a TensorRT engine; ignored, the "
            "compiled program comes from the ONNX model", engine_file));
  if (opts->force_rebuild) {
    reason = "rebuild forced";
  } else {
    std::vector<const gchar *> paths;
    std::string engine_file_reason;

    if (opts->output_file) {
      paths.push_back (opts->output_file);
    } else {
      if (engine_file && !is_trt)
        paths.push_back (engine_file);
      paths.push_back (st.candidates[0]);
      paths.push_back (st.candidates[1]);
    }
    for (size_t i = 0; i < paths.size () && need_build; i++) {
      gchar *why = NULL;

      /* engine_file may name a cache file itself. */
      if (i > 0 && engine_file && strcmp (paths[i], engine_file) == 0)
        continue;
      if (try_load (e.get (), paths[i], st.key, input_lens, names, &why)) {
        need_build = FALSE;
        e->program_path = g_strdup (paths[i]);
      } else {
        if (i == 0 && paths[0] == engine_file)
          engine_file_reason = why;
        /* The first candidate, and any other that exists, explain why nothing was loaded. */
        if (i == 0 || g_file_test (paths[i], G_FILE_TEST_EXISTS)) {
          if (!reason.empty ())
            reason += "; ";
          reason += why;
        }
      }
      g_free (why);
    }
    if (!need_build && !engine_file_reason.empty ())
      g_ptr_array_add (e->warnings, g_strdup_printf ("model-engine-file %s doesn't match (%s); using %s",
              engine_file, engine_file_reason.c_str (), e->program_path));
  }

  if (need_build && opts->no_build) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_NOT_CACHED, "no matching compiled program: %s",
        reason.c_str ());
    return NULL;
  }
  if (need_build) {
    const gchar *targets[2] = { st.candidates[0], st.candidates[1] };
    guint n_targets = 2;
    gboolean saved = FALSE;

    if (opts->output_file) {
      gchar *d = g_path_get_dirname (opts->output_file);
      gboolean ok = dir_writable (d);

      /* Fail before the (slow) build, not after it. */
      if (!ok)
        g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_OUTPUT, "can't write %s: %s is not "
            "writable", opts->output_file, d);
      g_free (d);
      if (!ok)
        return NULL;
      targets[0] = opts->output_file;
      n_targets = 1;
    }
    e->built = TRUE;
    e->build_reason = g_strdup (reason.c_str ());
    if (!build_program (e.get (), st.onnx, st.onnx_len, st.dir, input_lens, opts->fp16, error)
        || !read_parameters (e.get (), input_lens, names, error))
      return NULL;
    json_object_set_member (st.key, "outputs", outputs_node (e.get ()));
    for (guint i = 0; i < n_targets && !saved; i++) {
      gchar *d = g_path_get_dirname (targets[i]);

      if (dir_writable (d)) {
        if (save_program (e->program, st.key, targets[i], &err)) {
          saved = TRUE;
          e->program_path = g_strdup (targets[i]);
        } else {
          g_ptr_array_add (e->warnings, g_strdup_printf ("can't save the compiled program: %s", err->message));
          g_clear_error (&err);
        }
      }
      g_free (d);
    }
    if (!saved && !opts->output_file)
      g_ptr_array_add (e->warnings, g_strdup_printf ("the compiled program is not cached: neither %s nor %s is "
              "writable", st.dir, st.xdg_dir));
  }
  if (e->program_path)
    e->manifest_path = g_strconcat (e->program_path, ".json", NULL);
  if (!warm_up (e.get (), error))
    return NULL;
  return e.release ();
}

RdsInferEngine *
rds_infer_engine_open (const RdsInferEngineOptions * opts, GError ** error)
{
  /* MIGraphX calls are checked one by one; this catches the rest (std::bad_alloc from our own containers). What
   * engine_open owns is freed by its destructors on the way out. */
  try {
    return engine_open (opts, error);
  }
  catch (const std::exception & ex) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MIGRAPHX, "engine open failed: %s",
        ex.what ());
  }
  catch (...) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MIGRAPHX,
        "engine open failed: unknown exception");
  }
  return NULL;
}

const gchar *
rds_infer_engine_get_program_path (RdsInferEngine * e)
{
  return e->program_path;
}

const gchar *
rds_infer_engine_get_manifest_path (RdsInferEngine * e)
{
  return e->manifest_path;
}

gboolean
rds_infer_engine_was_built (RdsInferEngine * e)
{
  return e->built;
}

const gchar *
rds_infer_engine_get_build_reason (RdsInferEngine * e)
{
  return e->build_reason;
}

GPtrArray *
rds_infer_engine_get_warnings (RdsInferEngine * e)
{
  return e->warnings;
}

const gchar *
rds_infer_engine_get_input_name (RdsInferEngine * e)
{
  return e->input_name.c_str ();
}

void
rds_infer_engine_get_input (RdsInferEngine * e, guint * c, guint * h, guint * w)
{
  *c = e->c;
  *h = e->h;
  *w = e->w;
}

guint
rds_infer_engine_get_n_outputs (RdsInferEngine * e)
{
  return (guint) e->outputs.size ();
}

void
rds_infer_engine_get_output (RdsInferEngine * e, guint i, const gchar ** name, guint dims[8], guint * ndims)
{
  g_return_if_fail (i < e->outputs.size ());
  const EngineOutput & o = e->outputs[i];

  if (name)
    *name = o.name.c_str ();
  if (dims)
    memcpy (dims, o.dims, o.ndims * sizeof (guint));
  if (ndims)
    *ndims = o.ndims;
}

gsize
rds_infer_engine_get_output_elements (RdsInferEngine * e, guint i)
{
  g_return_val_if_fail (i < e->outputs.size (), 0);
  return e->outputs[i].elements;
}

gboolean
rds_infer_engine_run_async (RdsInferEngine * e, const float *input, float *const *outputs, void *stream,
    GError ** error)
{
  if (!stream || !input || !outputs) {
    g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID, "run_async: %s",
        !stream ? "no stream (the null stream is not used)" : "no input or output buffers");
    return FALSE;
  }
  for (size_t i = 0; i < e->outputs.size (); i++)
    if (!outputs[i]) {
      g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_INVALID, "run_async: output %"
          G_GSIZE_FORMAT " has no buffer", (gsize) i);
      return FALSE;
    }
  DeviceScope scope (e->gpu_id);
  return device_scope_ok (scope, e->gpu_id, error) && run_async (e, input, outputs, (hipStream_t) stream, error);
}

void
rds_infer_engine_free (RdsInferEngine * e)
{
  if (!e)
    return;
  {
    /* The program's GPU resources are released on its device. A failure here is dropped: the engine never logs,
     * and teardown has no caller to hand an error to. */
    DeviceScope scope (e->gpu_id);
    GError *err = NULL;

    if (!RDS_MGX_CHECK ("migraphx::program::~program", &err, e->program = migraphx::program ()))
      g_error_free (err);
  }
  g_free (e->program_path);
  g_free (e->manifest_path);
  g_free (e->build_reason);
  g_ptr_array_unref (e->warnings);
  delete e;
}
