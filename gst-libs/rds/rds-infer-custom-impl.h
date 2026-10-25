/* Redstream: the custom bbox parser ABI of rdsinfer (counterpart of DeepStream's nvdsinfer_custom_impl.h). C++.
 *
 * A parser library is a shared object with one or more functions of type RdsInferParseCustomFunc, with C linkage.
 * rdsinfer loads the library named by custom-lib-path and calls the function named by parse-bbox-func-name once
 * per inferred image. Names and field order follow NVIDIA's public API reference, so a DeepStream parser builds
 * unchanged against rds/rds_compat.h (which adds the NvDsInfer* names).
 *
 * Build parsers with the toolchain and C++ standard library ABI Redstream was built with (libstdc++,
 * _GLIBCXX_USE_CXX11_ABI=1): the ABI passes std::vector. Including this header defines
 * rds_infer_parser_abi_version() in the library (a weak symbol, so any number of files may include it), which
 * rdsinfer checks when it loads the library. A library built with -fvisibility=hidden still exports it, but its
 * parse functions need __attribute__ ((visibility ("default"))) to be found.
 *
 * The parse function must not let an exception escape: rdsinfer catches it, drops that image's detections and
 * posts a warning, as it does for a false return.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_INFER_CUSTOM_IMPL_H
#define RDS_INFER_CUSTOM_IMPL_H

#ifndef __cplusplus
#error "rds/rds-infer-custom-impl.h is C++ (the parser ABI uses std::vector)"
#endif

#include <type_traits>
#include <vector>

#include <rds/rds-infer.h>

/* Bumped on any change to the types or the call contract. */
#define RDS_INFER_PARSER_ABI_VERSION 1u

/* The detection settings from the config file, for the parser to apply. */
struct RdsInferParseDetectionParams
{
  /* Classes 0 to numClassesConfigured - 1 (num-detected-classes): output no other class ids. */
  unsigned int numClassesConfigured = 0;
  /* Per class (pre-cluster-threshold): output only objects whose confidence is at least this. */
  std::vector<float> perClassPreclusterThreshold;
  /* Per class (post-cluster-threshold), applied by rdsinfer after clustering. */
  std::vector<float> perClassPostclusterThreshold;
  /* Deprecated name of perClassPreclusterThreshold. */
  std::vector<float> &perClassThreshold = perClassPreclusterThreshold;

  /* Ours: copies keep perClassThreshold bound to their own thresholds (the implicit copy would bind it to the
   * source's, and the reference member would delete assignment). */
  RdsInferParseDetectionParams () = default;
  RdsInferParseDetectionParams (const RdsInferParseDetectionParams &other)
      : numClassesConfigured (other.numClassesConfigured),
        perClassPreclusterThreshold (other.perClassPreclusterThreshold),
        perClassPostclusterThreshold (other.perClassPostclusterThreshold)
  {
  }
  RdsInferParseDetectionParams &operator= (const RdsInferParseDetectionParams &other)
  {
    numClassesConfigured = other.numClassesConfigured;
    perClassPreclusterThreshold = other.perClassPreclusterThreshold;
    perClassPostclusterThreshold = other.perClassPostclusterThreshold;
    return *this;
  }
};

/* Parses one image's output layers and appends the objects found to objectList. Return false on failure. */
typedef bool (*RdsInferParseCustomFunc) (std::vector<RdsInferLayerInfo> const &outputLayersInfo,
    RdsInferNetworkInfo const &networkInfo, RdsInferParseDetectionParams const &detectionParams,
    std::vector<RdsInferObjectDetectionInfo> &objectList);

/* Use after defining a parse function: fails to compile unless the function has the RdsInferParseCustomFunc
 * signature, and declares it extern "C". */
#define RDS_CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(customParseFunc) \
  extern "C" bool customParseFunc (std::vector<RdsInferLayerInfo> const &outputLayersInfo, \
      RdsInferNetworkInfo const &networkInfo, RdsInferParseDetectionParams const &detectionParams, \
      std::vector<RdsInferObjectDetectionInfo> &objectList); \
  static_assert (std::is_same<decltype (&customParseFunc), RdsInferParseCustomFunc>::value, \
      #customParseFunc " is not an RdsInferParseCustomFunc");

/* rdsinfer itself defines RDS_INFER_NO_PARSER_ABI_SYMBOL: it loads parsers and is not one. */
#ifndef RDS_INFER_NO_PARSER_ABI_SYMBOL
extern "C" __attribute__ ((weak, visibility ("default"))) unsigned int rds_infer_parser_abi_version (void);

extern "C" __attribute__ ((weak, visibility ("default"))) unsigned int
rds_infer_parser_abi_version (void)
{
  return RDS_INFER_PARSER_ABI_VERSION;
}
#endif

#endif /* RDS_INFER_CUSTOM_IMPL_H */
