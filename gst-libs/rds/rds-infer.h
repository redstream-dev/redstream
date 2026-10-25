/* Redstream: rdsinfer's layer, network and object types (counterpart of DeepStream's nvdsinfer.h). C and C++.
 * Names and field order follow NVIDIA's public API reference; Redstream-only additions are marked "ours". The
 * custom bbox parser ABI built on them (C++) is rds/rds-infer-custom-impl.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_H
#define RDS_INFER_H

#ifdef __cplusplus
extern "C" {
#endif

#define RDS_INFER_MAX_DIMS 8

/* Element type of a layer. In C++, rds/rds_compat.h also provides the unprefixed names (FLOAT, HALF, ...). */
typedef enum
{
  RDS_INFER_DATA_FLOAT = 0,     /* 32-bit float */
  RDS_INFER_DATA_HALF = 1,      /* 16-bit float */
  RDS_INFER_DATA_INT8 = 2,
  RDS_INFER_DATA_INT32 = 3,
  RDS_INFER_DATA_INT64 = 4,
} RdsInferDataType;

/* Dimensions of one image's slice of a layer: no batch dimension, and numElements is the product of d[0..numDims)
 * (ours: the public docs leave both open). */
typedef struct
{
  unsigned int numDims;
  unsigned int d[RDS_INFER_MAX_DIMS];
  unsigned int numElements;
} RdsInferDims;

typedef struct
{
  unsigned int c;
  unsigned int h;
  unsigned int w;
} RdsInferDimsCHW;

typedef struct
{
  RdsInferDataType dataType;
  union
  {
    RdsInferDims inferDims;
  };
  int bindingIndex;             /* the layer's index among the program's inputs or outputs */
  const char *layerName;
  void *buffer;                 /* host memory: this image's slice of the layer */
  int isInput;                  /* non-zero for an input layer */
  void *deviceBuffer;           /* ours: the same slice in device memory, or NULL */
} RdsInferLayerInfo;

/* The network input size. */
typedef struct
{
  unsigned int width;
  unsigned int height;
  unsigned int channels;
} RdsInferNetworkInfo;

/* One object from a bbox parser, in network-input pixels. */
typedef struct
{
  unsigned int classId;
  float left;
  float top;
  float width;
  float height;
  float detectionConfidence;    /* 0.0 to 1.0 */
  /* Oriented boxes, in radians (the DeepStream 9 layout, so parsers that set it compile). rdsinfer has no oriented
   * boxes: it ignores a non-zero angle, with a warning. */
  float rotation_angle;
} RdsInferObjectDetectionInfo;

typedef RdsInferObjectDetectionInfo RdsInferParseObjectInfo;

#ifdef __cplusplus
}
#endif

#endif /* RDS_INFER_H */
