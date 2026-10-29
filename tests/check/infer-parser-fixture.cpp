/* Test parser module, written the way a DeepStream bbox parser is: only the NvDsInfer* names and
 * CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE, through rds/rds_compat_infer.h (built with -DUSE_REDSTREAM). Not installed.
 * FixtureBoxes returns the boxes in infer-parser-fixture.h, FixtureThrow throws, FixtureFail returns false.
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

CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureBoxes);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureThrow);
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE (FixtureFail);

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
