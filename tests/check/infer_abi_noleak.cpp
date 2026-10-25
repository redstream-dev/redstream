/* Part of infer_abi: the parser header alone (no rds_compat.h) adds no DeepStream or unprefixed names, so a
 * parser written against the Redstream names can use them for its own things.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds-infer-custom-impl.h>

int infer_abi_noleak_sum (void);

static const int FLOAT = 1, HALF = 2, INT8 = 3, INT32 = 4, INT64 = 5;
typedef int NvDsInferLayerInfo;
typedef int NvDsInferDataType;

int
infer_abi_noleak_sum (void)
{
  NvDsInferLayerInfo a = FLOAT + HALF;
  NvDsInferDataType b = INT8 + INT32 + INT64;
  RdsInferLayerInfo layer = {};

  return a + b + (int) layer.dataType;
}
