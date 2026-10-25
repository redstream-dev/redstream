/* Test parser module, written the way a DeepStream bbox parser is: only the NvDsInfer* names and
 * CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE, through rds/rds_compat.h (built with -DUSE_REDSTREAM). Not installed.
 * FixtureBoxes returns the boxes in infer-parser-fixture.h, FixtureThrow throws, FixtureFail returns false.
 * The box table lives in a second translation unit (infer-parser-fixture-boxes.cpp), which includes the parser
 * header too: the module links two copies of its weak version symbol.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef USE_REDSTREAM
#include <rds/rds_compat.h>
#else
#include "nvdsinfer_custom_impl.h"
#endif

#include <stdexcept>

#include "infer-parser-fixture.h"

/* From infer-parser-fixture-boxes.cpp. */
void fixture_append_boxes (std::vector<NvDsInferParseObjectInfo> &objectList);

extern "C" bool FixtureBoxes (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList);
extern "C" bool FixtureThrow (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList);
extern "C" bool FixtureFail (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList);

extern "C" bool
FixtureBoxes (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams, std::vector<NvDsInferParseObjectInfo> &objectList)
{
  (void) outputLayersInfo;
  (void) networkInfo;
  (void) detectionParams;
  fixture_append_boxes (objectList);
  return true;
}

extern "C" bool
FixtureThrow (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams, std::vector<NvDsInferParseObjectInfo> &objectList)
{
  (void) outputLayersInfo;
  (void) networkInfo;
  (void) detectionParams;
  fixture_append_boxes (objectList);
  throw std::runtime_error (RDS_TEST_FIXTURE_THROW_WHAT);
}

extern "C" bool
FixtureFail (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams, std::vector<NvDsInferParseObjectInfo> &objectList)
{
  (void) outputLayersInfo;
  (void) networkInfo;
  (void) detectionParams;
  fixture_append_boxes (objectList);
  return false;
}

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureBoxes);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureThrow);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureFail);
