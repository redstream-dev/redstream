/* Second translation unit of the test parser module (see infer-parser-fixture.cpp): it includes the parser header
 * as well, so the module links two weak definitions of rds_infer_parser_abi_version. FixtureVisible is exported
 * also when the module is built with -fvisibility=hidden.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef USE_REDSTREAM
#include <rds/rds_compat_infer.h>
#else
#include "nvdsinfer_custom_impl.h"
#endif

#include "infer-parser-fixture.h"

void fixture_append_boxes (std::vector<NvDsInferParseObjectInfo> &objectList);

void
fixture_append_boxes (std::vector<NvDsInferParseObjectInfo> &objectList)
{
  for (const RdsTestFixtureBox &b : rds_test_fixture_boxes) {
    NvDsInferParseObjectInfo o = {};

    o.classId = b.class_id;
    o.left = b.left;
    o.top = b.top;
    o.width = b.width;
    o.height = b.height;
    o.detectionConfidence = b.confidence;
    objectList.push_back (o);
  }
}

extern "C" __attribute__ ((visibility ("default"))) bool FixtureVisible (
    std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams, std::vector<NvDsInferParseObjectInfo> &objectList);

extern "C" __attribute__ ((visibility ("default"))) bool
FixtureVisible (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams, std::vector<NvDsInferParseObjectInfo> &objectList)
{
  (void) outputLayersInfo;
  (void) networkInfo;
  (void) detectionParams;
  fixture_append_boxes (objectList);
  return true;
}

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureVisible);
