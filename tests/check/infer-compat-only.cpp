/* Test module for infer_abi: C++ that includes only rds/rds_compat.h (an application, not a parser). It gets
 * the C-compatible NvDsInfer* types, but neither the parser ABI (no <vector>, no exported
 * rds_infer_parser_abi_version) nor the unprefixed data-type names, which it defines for itself here, as other
 * libraries do (libjpeg's INT32, for one). Not installed.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include <rds/rds_compat.h>

#ifdef _GLIBCXX_VECTOR
#error "rds/rds_compat.h must not include <vector>"
#endif
#ifdef RDS_INFER_CUSTOM_IMPL_H
#error "rds/rds_compat.h must not include the parser ABI"
#endif

typedef long INT32;
static const int FLOAT = 1, HALF = 2, INT8 = 3, INT64 = 5;

extern "C" __attribute__ ((visibility ("default"))) int compat_only_sum (void);

extern "C" int
compat_only_sum (void)
{
  INT32 i32 = 4;
  NvDsInferLayerInfo layer = {};

  return FLOAT + HALF + INT8 + (int) i32 + INT64 + (int) layer.dataType + NVDSINFER_MAX_DIMS;
}
