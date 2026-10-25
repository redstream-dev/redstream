/* Redstream: the graph inputs and outputs of an ONNX model as declared in the file (names, element types, dims),
 * read straight from the protobuf encoding. MIGraphX gives every symbolic dimension a default value when it parses
 * a model, so whether a dimension is fixed or symbolic has to come from the file. Plugin-internal, not installed.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_ONNX_H
#define RDS_INFER_ONNX_H

#include <glib.h>

G_BEGIN_DECLS

#define RDS_INFER_ONNX_FLOAT 1          /* TensorProto.DataType FLOAT */

typedef struct
{
  gchar *name;
  gint elem_type;               /* TensorProto.DataType; 0 when the type isn't a tensor or isn't given */
  gboolean has_shape;           /* FALSE: the rank is unknown */
  guint n_dims;
  gint64 *dims;                 /* n_dims entries: > 0 fixed, -1 symbolic or unknown */
  gchar **dim_params;           /* n_dims entries: the symbol name, or NULL (fixed or unnamed) */
} RdsInferOnnxValue;

typedef struct
{
  GPtrArray *inputs;            /* RdsInferOnnxValue: the graph inputs that are not initializers, in file order */
  GPtrArray *outputs;           /* RdsInferOnnxValue: the graph outputs, in file order */
  gchar *external_tensor;       /* the first initializer whose data is in an external file, or NULL */
} RdsInferOnnxInfo;

/* Reads ModelProto.graph's inputs, outputs, initializer names and whether initializers use external data from the
 * serialized model @data. Errors (RDS_INFER_ENGINE_ERROR_MODEL) say where the encoding is broken; the message
 * doesn't name the file. */
G_GNUC_INTERNAL RdsInferOnnxInfo *rds_infer_onnx_read_info (const guint8 * data, gsize len, GError ** error);
G_GNUC_INTERNAL void rds_infer_onnx_info_free (RdsInferOnnxInfo * info);

G_END_DECLS

#endif /* RDS_INFER_ONNX_H */
