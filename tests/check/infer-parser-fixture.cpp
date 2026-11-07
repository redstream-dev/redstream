/* Test parser module, written the way a DeepStream bbox parser is: only the NvDsInfer* names and
 * CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE, through rds/rds_compat_infer.h (built with -DUSE_REDSTREAM). Not installed.
 * FixtureBoxes returns the boxes in infer-parser-fixture.h, FixtureThrow throws, FixtureFail returns false,
 * FixtureFailDark returns false for dark images only. The classifier parsers
 * (CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE): FixtureClassify returns the attributes in
 * infer-parser-fixture.h, FixtureClassifyNoDesc the same without a descString, FixtureClassifySpaceDesc the same with
 * a descString of one space, FixtureClassifyPastLayers one attribute whose index is past the layers,
 * FixtureClassifyFail and FixtureClassifyThrow allocate a label, then return false or throw, FixtureClassifyFailDark
 * returns false for dark images only.
 * FixtureVisible (in the second file) is FixtureBoxes marked with default visibility, for the build of these
 * sources with -fvisibility=hidden.
 * The box table lives in a second translation unit (infer-parser-fixture-boxes.cpp), which includes the parser
 * header too: the module links two copies of its weak version symbol.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef USE_REDSTREAM
#include <rds/rds_compat_infer.h>
#else
#include "nvdsinfer_custom_impl.h"
#endif

#include <stdexcept>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <condition_variable>
#include <mutex>

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

/* FixtureBoxes for a bright image, false for a dark one: the first value of output layer 0 below 100 (scale2 doubles
 * its input, so a pixel value below 50 with net-scale-factor 1 and no offsets). */
extern "C" bool FixtureFailDark (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList);

extern "C" bool
FixtureFailDark (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    NvDsInferParseDetectionParams const &detectionParams, std::vector<NvDsInferParseObjectInfo> &objectList)
{
  if (outputLayersInfo.empty () || static_cast<const float *> (outputLayersInfo[0].buffer)[0] < 100.0f)
    return false;
  return FixtureBoxes (outputLayersInfo, networkInfo, detectionParams, objectList);
}

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureBoxes);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureThrow);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureFail);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureFailDark);

/* Classifier parsers, defined first and checked by the macro after (as the macro's documentation asks). */
extern "C" bool FixtureClassify (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);
extern "C" bool FixtureClassifyNoDesc (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);
extern "C" bool FixtureClassifySpaceDesc (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);
extern "C" bool FixtureClassifyPastLayers (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);
extern "C" bool FixtureClassifyFail (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);
extern "C" bool FixtureClassifyThrow (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);
extern "C" bool FixtureClassifyFailDark (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString);

/* The attributes as in infer-parser-fixture.h: one per output layer, from its first value and the call's
 * arguments. */
extern "C" bool
FixtureClassify (std::vector<NvDsInferLayerInfo> const &outputLayersInfo, NvDsInferNetworkInfo const &networkInfo,
    float classifierThreshold, std::vector<NvDsInferAttribute> &attrList, std::string &descString)
{
  for (unsigned int i = 0; i < outputLayersInfo.size (); i++) {
    const float first = static_cast<const float *> (outputLayersInfo[i].buffer)[0];
    std::string label = "attr" + std::to_string (i) + "-" + std::to_string (networkInfo.width) + "x"
        + std::to_string (networkInfo.height);
    NvDsInferAttribute attr = {};

    attr.attributeIndex = i;
    attr.attributeValue = static_cast<unsigned int> (std::lround (first * RDS_TEST_FIXTURE_ATTR_SCALE));
    attr.attributeConfidence = classifierThreshold / 2;
    attr.attributeLabel = i == 1 ? nullptr : strdup (label.c_str ());
    attrList.push_back (attr);
  }
  descString = "desc" + std::to_string (outputLayersInfo.size ());
  return true;
}

extern "C" bool
FixtureClassifyNoDesc (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString)
{
  bool ok = FixtureClassify (outputLayersInfo, networkInfo, classifierThreshold, attrList, descString);

  descString.clear ();
  return ok;
}

extern "C" bool
FixtureClassifySpaceDesc (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString)
{
  bool ok = FixtureClassify (outputLayersInfo, networkInfo, classifierThreshold, attrList, descString);

  descString = " ";
  return ok;
}

extern "C" bool
FixtureClassifyPastLayers (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString)
{
  NvDsInferAttribute attr = {};

  (void) networkInfo;
  (void) classifierThreshold;
  attr.attributeIndex = static_cast<unsigned int> (outputLayersInfo.size ());
  attr.attributeValue = 5;
  attr.attributeConfidence = 0.5f;
  attr.attributeLabel = strdup ("past");
  attrList.push_back (attr);
  descString = "past";
  return true;
}

extern "C" bool
FixtureClassifyFail (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString)
{
  FixtureClassify (outputLayersInfo, networkInfo, classifierThreshold, attrList, descString);
  return false;
}

extern "C" bool
FixtureClassifyThrow (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString)
{
  FixtureClassify (outputLayersInfo, networkInfo, classifierThreshold, attrList, descString);
  throw std::runtime_error (RDS_TEST_FIXTURE_THROW_WHAT);
}

extern "C" bool
FixtureClassifyFailDark (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, float classifierThreshold, std::vector<NvDsInferAttribute> &attrList,
    std::string &descString)
{
  if (outputLayersInfo.empty ()
      || static_cast<const float *> (outputLayersInfo[0].buffer)[0] < RDS_TEST_FIXTURE_DARK_BELOW)
    return false;
  return FixtureClassify (outputLayersInfo, networkInfo, classifierThreshold, attrList, descString);
}

CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (FixtureClassify);
CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (FixtureClassifyNoDesc);
CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (FixtureClassifySpaceDesc);
CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (FixtureClassifyPastLayers);
CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (FixtureClassifyFail);
CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (FixtureClassifyThrow);
CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE (FixtureClassifyFailDark);

/* A parser-side barrier makes frame replacement happen strictly after the snapshot and before attachment.
 * Controls live in this test module; no synchronization hooks are needed in the element. */
static std::mutex barrier_mutex;
static std::condition_variable barrier_cond;
static bool barrier_entered, barrier_released;

extern "C" void
FixtureBarrierReset ()
{
  std::lock_guard<std::mutex> lock (barrier_mutex);
  barrier_entered = barrier_released = false;
}

/* int matches gboolean in the C test driver. */
extern "C" int
FixtureBarrierWait ()
{
  std::unique_lock<std::mutex> lock (barrier_mutex);
  return barrier_cond.wait_for (lock, std::chrono::seconds (30), [] { return barrier_entered; });
}

extern "C" void
FixtureBarrierRelease ()
{
  std::lock_guard<std::mutex> lock (barrier_mutex);
  barrier_released = true;
  barrier_cond.notify_all ();
}

static void
fixture_barrier ()
{
  std::unique_lock<std::mutex> lock (barrier_mutex);
  barrier_entered = true;
  barrier_cond.notify_all ();
  /* Only the driver releases parsing: a deadline must never let it race ahead of frame replacement.
   * Meson bounds the whole test if the driver fails to release it. */
  barrier_cond.wait (lock, [] { return barrier_released; });
}

extern "C" bool
FixtureBarrierBoxes (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList)
{
  fixture_barrier ();
  return FixtureBoxes (outputLayersInfo, networkInfo, detectionParams, objectList);
}

extern "C" bool
FixtureBarrierFail (std::vector<NvDsInferLayerInfo> const &outputLayersInfo,
    NvDsInferNetworkInfo const &networkInfo, NvDsInferParseDetectionParams const &detectionParams,
    std::vector<NvDsInferParseObjectInfo> &objectList)
{
  fixture_barrier ();
  return FixtureFail (outputLayersInfo, networkInfo, detectionParams, objectList);
}

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureBarrierBoxes);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureBarrierFail);
