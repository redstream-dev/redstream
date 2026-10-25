/* Redstream source-compatibility header for DeepStream custom bbox parsers: the replacement for
 * nvdsinfer_custom_impl.h. C++. Adds the parser ABI's DeepStream names (NvDsInferParseDetectionParams,
 * NvDsInferParseCustomFunc, CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE, and the unprefixed data-type names FLOAT, HALF,
 * INT8, INT32, INT64) to everything in rds/rds_compat.h; see docs/compat-matrix.md.
 *
 *   #ifdef USE_REDSTREAM
 *   #include <rds/rds_compat_infer.h>
 *   #else
 *   #include "nvdsinfer_custom_impl.h"
 *   #endif
 *
 * Like rds/rds-infer-custom-impl.h, it defines the weak rds_infer_parser_abi_version() that marks the library
 * as a Redstream parser: include it only in parser libraries.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_COMPAT_INFER_H
#define RDS_COMPAT_INFER_H

#ifndef __cplusplus
#error "rds/rds_compat_infer.h is C++ (the parser ABI uses std::vector)"
#endif

#include <rds/rds_compat.h>
#include <rds/rds-infer-custom-impl.h>

/* Unscoped, unprefixed enumerators on DeepStream; constants of the Redstream enum here, so comparisons and switches
 * on dataType compile warning-free. */
constexpr RdsInferDataType FLOAT = RDS_INFER_DATA_FLOAT;
constexpr RdsInferDataType HALF = RDS_INFER_DATA_HALF;
constexpr RdsInferDataType INT8 = RDS_INFER_DATA_INT8;
constexpr RdsInferDataType INT32 = RDS_INFER_DATA_INT32;
constexpr RdsInferDataType INT64 = RDS_INFER_DATA_INT64;

typedef RdsInferParseDetectionParams NvDsInferParseDetectionParams;
typedef RdsInferParseCustomFunc NvDsInferParseCustomFunc;
#define CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE RDS_CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE

#endif /* RDS_COMPAT_INFER_H */
