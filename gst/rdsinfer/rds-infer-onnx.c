/* Redstream: a minimal reader for the protobuf encoding of an ONNX ModelProto, for the graph's inputs and outputs.
 * Only the fields on the path ModelProto.graph -> GraphProto.{input, output, initializer} -> ValueInfoProto.type ->
 * TypeProto.tensor_type.{elem_type, shape} -> TensorShapeProto.dim -> {dim_value, dim_param}, and the
 * initializers' TensorProto.{name, external_data, data_location}, are decoded; every
 * other field is skipped by its wire type. Field numbers are from onnx.proto (ONNX IR, all versions).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-onnx.h"
#include "rds-infer-engine.h"

#include <string.h>

/* ModelProto */
#define MODEL_GRAPH 7
/* GraphProto */
#define GRAPH_INITIALIZER 5
#define GRAPH_INPUT 11
#define GRAPH_OUTPUT 12
/* TensorProto */
#define TENSOR_NAME 8
#define TENSOR_EXTERNAL_DATA 13
#define TENSOR_DATA_LOCATION 14
#define DATA_LOCATION_EXTERNAL 1
/* ValueInfoProto */
#define VALUE_NAME 1
#define VALUE_TYPE 2
/* TypeProto */
#define TYPE_TENSOR 1
/* TypeProto.Tensor */
#define TENSOR_TYPE_ELEM_TYPE 1
#define TENSOR_TYPE_SHAPE 2
/* TensorShapeProto */
#define SHAPE_DIM 1
/* TensorShapeProto.Dimension */
#define DIM_VALUE 1
#define DIM_PARAM 2

enum
{
  WIRE_VARINT = 0,
  WIRE_I64 = 1,
  WIRE_LEN = 2,
  WIRE_I32 = 5,
};

typedef struct
{
  const guint8 *p, *end;
} Reader;

/* One field: its number, wire type, and value (a varint, or the bytes of a length-delimited field). */
typedef struct
{
  guint64 number;
  guint wire;
  guint64 varint;
  Reader bytes;
} Field;

static gboolean
read_varint (Reader * r, guint64 * out)
{
  guint64 v = 0;

  for (guint shift = 0; shift < 64; shift += 7) {
    guint8 b;

    if (r->p >= r->end)
      return FALSE;
    b = *r->p++;
    v |= (guint64) (b & 0x7f) << shift;
    if (!(b & 0x80)) {
      *out = v;
      return TRUE;
    }
  }
  return FALSE;                 /* more than 10 bytes */
}

/* FALSE at the end of @r (with *ok TRUE) or on a malformed field (*ok FALSE). */
static gboolean
next_field (Reader * r, Field * f, gboolean * ok)
{
  guint64 tag, len;

  *ok = TRUE;
  if (r->p >= r->end)
    return FALSE;
  *ok = FALSE;
  if (!read_varint (r, &tag) || (tag >> 3) == 0)
    return FALSE;
  f->number = tag >> 3;
  f->wire = tag & 7;
  switch (f->wire) {
    case WIRE_VARINT:
      if (!read_varint (r, &f->varint))
        return FALSE;
      break;
    case WIRE_I64:
    case WIRE_I32:
      len = f->wire == WIRE_I64 ? 8 : 4;
      if ((guint64) (r->end - r->p) < len)
        return FALSE;
      r->p += len;
      break;
    case WIRE_LEN:
      if (!read_varint (r, &len) || len > (guint64) (r->end - r->p))
        return FALSE;
      f->bytes.p = r->p;
      f->bytes.end = r->p + len;
      r->p += len;
      break;
    default:                   /* groups (3, 4) are not used by ONNX; 6 and 7 don't exist */
      return FALSE;
  }
  *ok = TRUE;
  return TRUE;
}

static gchar *
field_string (const Field * f)
{
  return g_strndup ((const gchar *) f->bytes.p, f->bytes.end - f->bytes.p);
}

static void
value_free (RdsInferOnnxValue * v)
{
  g_free (v->name);
  g_free (v->dims);
  if (v->dim_params) {
    for (guint i = 0; i < v->n_dims; i++)
      g_free (v->dim_params[i]);
    g_free (v->dim_params);
  }
  g_free (v);
}

static gboolean
read_dim (Reader r, GArray * dims, GPtrArray * params)
{
  Field f;
  gboolean ok;
  gint64 value = -1;
  gchar *param = NULL;

  while (next_field (&r, &f, &ok)) {
    if (f.number == DIM_VALUE && f.wire == WIRE_VARINT) {
      value = (gint64) f.varint > 0 ? (gint64) f.varint : -1;
      g_clear_pointer (&param, g_free);
    } else if (f.number == DIM_PARAM && f.wire == WIRE_LEN) {
      g_free (param);
      param = field_string (&f);
      value = -1;
    }
  }
  if (!ok) {
    g_free (param);
    return FALSE;
  }
  g_array_append_val (dims, value);
  g_ptr_array_add (params, param);
  return TRUE;
}

static gboolean
read_shape (Reader r, RdsInferOnnxValue * v)
{
  GArray *dims = g_array_new (FALSE, FALSE, sizeof (gint64));
  GPtrArray *params = g_ptr_array_new_with_free_func (g_free);
  Field f;
  gboolean ok;

  while (next_field (&r, &f, &ok))
    if (f.number == SHAPE_DIM && f.wire == WIRE_LEN && !read_dim (f.bytes, dims, params)) {
      ok = FALSE;
      break;
    }
  if (ok) {
    v->has_shape = TRUE;
    v->n_dims = dims->len;
    v->dims = (gint64 *) g_array_free (dims, FALSE);
    g_ptr_array_set_free_func (params, NULL);
    v->dim_params = (gchar **) g_ptr_array_free (params, FALSE);
  } else {
    g_array_free (dims, TRUE);
    g_ptr_array_free (params, TRUE);
  }
  return ok;
}

static gboolean
read_type (Reader r, RdsInferOnnxValue * v)
{
  Field f, g;
  gboolean ok, ok2 = TRUE;

  while (next_field (&r, &f, &ok)) {
    if (f.number != TYPE_TENSOR || f.wire != WIRE_LEN)
      continue;
    while (ok2 && next_field (&f.bytes, &g, &ok2)) {
      if (g.number == TENSOR_TYPE_ELEM_TYPE && g.wire == WIRE_VARINT) {
        v->elem_type = (gint) g.varint;
      } else if (g.number == TENSOR_TYPE_SHAPE && g.wire == WIRE_LEN) {
        /* A repeated shape field replaces the previous one. */
        g_clear_pointer (&v->dims, g_free);
        if (v->dim_params) {
          for (guint i = 0; i < v->n_dims; i++)
            g_free (v->dim_params[i]);
          g_clear_pointer (&v->dim_params, g_free);
        }
        v->n_dims = 0;
        if (!read_shape (g.bytes, v))
          ok2 = FALSE;
      }
    }
    if (!ok2)
      return FALSE;
  }
  return ok;
}

static RdsInferOnnxValue *
read_value (Reader r)
{
  RdsInferOnnxValue *v = g_new0 (RdsInferOnnxValue, 1);
  Field f;
  gboolean ok;

  while (next_field (&r, &f, &ok)) {
    if (f.number == VALUE_NAME && f.wire == WIRE_LEN) {
      g_free (v->name);
      v->name = field_string (&f);
    } else if (f.number == VALUE_TYPE && f.wire == WIRE_LEN && !read_type (f.bytes, v)) {
      ok = FALSE;
      break;
    }
  }
  if (!ok || !v->name) {
    value_free (v);
    return NULL;
  }
  return v;
}

/* Adds the initializer's name to @names; sets *external to it (once) if its data is in an external file. */
static gboolean
read_initializer (Reader r, GHashTable * names, gchar ** external)
{
  Field f;
  gboolean ok, is_external = FALSE;
  gchar *name = NULL;

  while (next_field (&r, &f, &ok)) {
    if (f.number == TENSOR_NAME && f.wire == WIRE_LEN) {
      g_free (name);
      name = field_string (&f);
    } else if ((f.number == TENSOR_EXTERNAL_DATA && f.wire == WIRE_LEN)
        || (f.number == TENSOR_DATA_LOCATION && f.wire == WIRE_VARINT && f.varint == DATA_LOCATION_EXTERNAL)) {
      is_external = TRUE;
    }
  }
  if (!ok) {
    g_free (name);
    return FALSE;
  }
  if (is_external && !*external)
    *external = g_strdup (name ? name : "");
  if (name)
    g_hash_table_add (names, name);
  return TRUE;
}

RdsInferOnnxInfo *
rds_infer_onnx_read_info (const guint8 * data, gsize len, GError ** error)
{
  Reader model = { data, data + len }, graph = { NULL, NULL };
  GPtrArray *inputs = g_ptr_array_new_with_free_func ((GDestroyNotify) value_free);
  GPtrArray *outputs = g_ptr_array_new_with_free_func ((GDestroyNotify) value_free);
  GHashTable *initializers = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  RdsInferOnnxInfo *info;
  gchar *external = NULL;
  const gchar *what = NULL;
  gboolean ok, have_graph = FALSE;
  Field f;

  /* A repeated graph field would be merged by protobuf; models have one, so the last one is used. */
  while (next_field (&model, &f, &ok))
    if (f.number == MODEL_GRAPH && f.wire == WIRE_LEN) {
      graph = f.bytes;
      have_graph = TRUE;
    }
  if (!ok) {
    what = "malformed ModelProto";
    goto fail;
  }
  if (!have_graph) {
    what = "no graph";
    goto fail;
  }
  while (next_field (&graph, &f, &ok)) {
    if (f.wire != WIRE_LEN)
      continue;
    if (f.number == GRAPH_INPUT || f.number == GRAPH_OUTPUT) {
      RdsInferOnnxValue *v = read_value (f.bytes);

      if (!v) {
        what = f.number == GRAPH_INPUT ? "malformed graph input" : "malformed graph output";
        goto fail;
      }
      g_ptr_array_add (f.number == GRAPH_INPUT ? inputs : outputs, v);
    } else if (f.number == GRAPH_INITIALIZER && !read_initializer (f.bytes, initializers, &external)) {
      what = "malformed initializer";
      goto fail;
    }
  }
  if (!ok) {
    what = "malformed GraphProto";
    goto fail;
  }

  /* Before IR version 4, initializers are listed as graph inputs too. */
  for (guint i = inputs->len; i > 0; i--) {
    RdsInferOnnxValue *v = g_ptr_array_index (inputs, i - 1);

    if (g_hash_table_contains (initializers, v->name))
      g_ptr_array_remove_index (inputs, i - 1);
  }
  g_hash_table_unref (initializers);
  info = g_new (RdsInferOnnxInfo, 1);
  info->inputs = inputs;
  info->outputs = outputs;
  info->external_tensor = external;
  return info;

fail:
  g_set_error (error, RDS_INFER_ENGINE_ERROR, RDS_INFER_ENGINE_ERROR_MODEL, "not a valid ONNX model: %s", what);
  g_free (external);
  g_hash_table_unref (initializers);
  g_ptr_array_unref (inputs);
  g_ptr_array_unref (outputs);
  return NULL;
}

void
rds_infer_onnx_info_free (RdsInferOnnxInfo * info)
{
  if (!info)
    return;
  g_ptr_array_unref (info->inputs);
  g_ptr_array_unref (info->outputs);
  g_free (info->external_tensor);
  g_free (info);
}
