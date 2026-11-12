# rdstracker config file

How to read this page, and how to run the examples: [element reference](README.md).

The low-level config file `rdstracker` reads (`ll-config-file`): NvMultiObjectTracker's YAML, with every key's type,
default, range, the trackers that use it and its status. The element is on its own page, [rdstracker](rdstracker.md).

## Example

The repository's own configs, `samples/tracker/config_tracker_IOU.yml` and `config_tracker_NvSORT.yml`, are tuned on
a 720p street clip with YOLO11n detections. On that clip (1356 frames) the NvSORT config gives 283 tracks of 75.6
frames on average with 4 ID switches and 98.8 % of the confident detections tracked; the IOU config 426 tracks of
62.9 frames, 12 switches, 99.4 %.

A minimal IOU tracker, as a file `iou.yml`:

```yaml
%YAML:1.0
BaseConfig:
  minDetectorConfidence: 0.25
TargetManagement:
  probationAge: 3
  maxShadowTrackingAge: 30
  earlyTerminationAge: 1
DataAssociator:
  associationMatcherType: 0
  checkClassMatch: 1
  minMatchingScore4Iou: 0.1
StateEstimator:
  stateEstimatorType: 0
```

`rdstracker ll-config-file=iou.yml` runs it; [rdstracker](rdstracker.md) has a full pipeline. A `StateEstimator` with
`stateEstimatorType` 1 or 2, or an `associationMatcherType` other than 0, makes it NvSORT.

## Format and loading

- The `ll-config-file` is YAML with top-level sections (`BaseConfig:`, `TargetManagement:`, ...), each a mapping
  of scalar keys; a sequence is a list value. The OpenCV-style `%YAML:1.0` first line is accepted.
- Keys are in the tables below with the parser's types, ranges and defaults. Ranges are written as in the parser's
  key table (`0..1`, `>= 0`, `any`), not as the element pages' `A–B`. Defaults are the documented ones
  unless marked "(ours)". **supported**: used; **accepted no-op**: read and ignored, with one warning when set to
  other than its default; **ignored**: used only by a module rdstracker doesn't have, type-checked and ignored
  without a warning; **deprecated**: an old name, ignored with a warning whenever set. Values of the wrong type or
  out of range are errors in every status.
- A `VisualTracker`, `ReID`, `ObjectModelProjection` or `PoseEstimator` section gives one warning naming the
  ignored sections and the tracker the config runs as.
- An unknown key or section is a warning, not an error. Errors name the file, the section and the key:
  `FILE:SECTION.KEY: VALUE: reason`.
- `checkClassMatch` is read under `DataAssociator` (where observed DeepStream 8.0 behaviour reads it) and under
  `BaseConfig` (where the current docs put it); the same key in both with different values is an error.
- In those ignored sections, a value nested deeper than the reader accepts (the documented map form of
  `cameraModelFilepath`, keyed by sensor name) is skipped with a warning; the list form is type-checked.

The file is read, and every key checked, when the element starts (READY to PAUSED); an error there fails the state
change. Changing the config at run time is not supported: a changed file is read at the next start.

## Tracker config keys: `BaseConfig`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `minDetectorConfidence` | float | `0.0` | any | IOU, NvSORT, NvDeepSORT, NvDCF | supported | lower detections are removed before association |
| `checkClassMatch` | bool | `1` |  | IOU, NvSORT, NvDeepSORT, NvDCF | supported | also read under DataAssociator; different values in the two sections are an error |
| `useBatchNumForFrameId` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | frame numbers always come from the frame meta |

## Tracker config keys: `TargetManagement`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `preserveStreamUpdateOrder` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | IDs are always assigned in input order (streams in batch order) |
| `maxTargetsPerStream` | uint | `30` | 0..65535 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | Inactive targets count; detections over the cap are removed |
| `minIouDiff4NewTarget` | float | `0.5` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | see the "Duplicate check" row of [Tracking rules](rdstracker.md#tracking-rules) |
| `enableBboxUnClipping` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | boxes are reported as detected (observed DeepStream 8.0 behaviour for IOU and NvSORT) |
| `probationAge` | uint | `5` | >= 0 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | see the "Activation" row of [Tracking rules](rdstracker.md#tracking-rules) |
| `maxShadowTrackingAge` | uint | `38` | >= 0 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | see the "Termination" row of [Tracking rules](rdstracker.md#tracking-rules) |
| `earlyTerminationAge` | uint | `2` | >= 0 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | Tentative targets only; see the "Termination" row of [Tracking rules](rdstracker.md#tracking-rules) |
| `outputTerminatedTracks` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | tracker misc data (past-frame, shadow, terminated tracks) is not output yet |
| `outputShadowTracks` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | tracker misc data (past-frame, shadow, terminated tracks) is not output yet |
| `outputVisibility` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | object visibility is not output yet |
| `terminatedTrackFilename` | string | none |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | tracker misc data (past-frame, shadow, terminated tracks) is not output yet |
| `minTrackerConfidence` | float | `0.6` | 0..1 | NvDCF | ignored | no effect for IOU and NvSORT, whose tracker confidence is constant |
| `searchRegionPaddingScale` | int | `1` | 1..3 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |

## Tracker config keys: `TrajectoryManagement`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `useUniqueID` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | supported | the upper 32 bits of every ID: one random value per element start |
| `enableReAssoc` | bool | `0` |  | NvDCF | accepted no-op | target re-association is not supported; its other keys are ignored |
| `minMatchingScore4Overall` | float | `0.4` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minTrackletMatchingScore` | float | `0.4` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minMatchingScore4ReidSimilarity` | float | `0.8` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `matchingScoreWeight4TrackletSimilarity` | float | `1.0` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `matchingScoreWeight4ReidSimilarity` | float | `0.0` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minTrajectoryLength4Projection` | int | `20` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `prepLength4TrajectoryProjection` | int | `10` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trajectoryProjectionLength` | int | `90` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `maxAngle4TrackletMatching` | int | `40` | 0..180 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minSpeedSimilarity4TrackletMatching` | float | `0.3` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minBboxSizeSimilarity4TrackletMatching` | float | `0.6` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `maxTrackletMatchingTimeSearchRange` | int | `20` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trajectoryProjectionProcessNoiseScale` | float | `1.0` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trajectoryProjectionMeasurementNoiseScale` | float | `1.0` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trackletSpacialSearchRegionScale` | float | `0.0` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `reidExtractionInterval` | int | `0` | >= -1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `enableVanishingTrackletReconstruction` | int | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minInclusionRatio4DuplicateTrackletRemoval` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minIou4DuplicateTrackletRemoval` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minMatchRatio4ValidTrackletDetermination` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minVisibility4VanishingTrackletReconstruction` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `visibilityThreshold4VanishingTrackletDetection` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `checkClassMatchForReAssoc` | bool | none |  | NvDCF | deprecated | deprecated: use checkClassMatch |

## Tracker config keys: `DataAssociator`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `dataAssociatorType` | int | `0` | 0..0 | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | 0 (the default data associator) is the only type |
| `associationMatcherType` | int | `0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | 0 greedy, 1 cascaded (NvSORT's three stages) |
| `checkClassMatch` | bool | `1` |  | IOU, NvSORT, NvDeepSORT, NvDCF | supported | where observed DeepStream 8.0 behaviour reads it; also read under BaseConfig |
| `minMatchingScore4Overall` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `minMatchingScore4SizeSimilarity` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `minMatchingScore4Iou` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `matchingScoreWeight4SizeSimilarity` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `matchingScoreWeight4Iou` | float | `1.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `tentativeDetectorConfidence` | float | `0.5` | 0..1 | NvSORT, NvDeepSORT, NvDCF | supported | NvSORT: lower detections never create targets |
| `minMatchingScore4TentativeIou` | float | `0.0` | 0..1 | NvSORT, NvDeepSORT, NvDCF | supported | NvSORT's second and third stages |
| `minMatchingScore4VisualSimilarity` | float | `0.0` | 0..1 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |
| `matchingScoreWeight4VisualSimilarity` | float | `0.0` | 0..1 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |
| `thresholdMahalanobis` | float | `-1.0` | any | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `minMatchingScore4ReidSimilarity` | float | `0.0` | 0..1 | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `matchingScoreWeight4ReidSimilarity` | float | `0.0` | 0..1 | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `matchingScoreWeight4ReIDSimilarity` | float | `0.0` | 0..1 | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `usePrediction4Assoc` | bool | see notes (ours) |  | NvSORT, NvDeepSORT, NvDCF | supported | undocumented; associate against the predicted box; default (ours) 1 for NvSORT, 0 for IOU |

## Tracker config keys: `StateEstimator`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `stateEstimatorType` | int | `0` | 0..3 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | 3 (SIMPLE_LOC) runs as 2 (REGULAR), with a warning |
| `processNoiseVar4Loc` | float | `2.0` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `processNoiseVar4Size` | float | `1.0` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `processNoiseVar4Vel` | float | `0.1` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `measurementNoiseVar4Detector` | float | `4.0` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `measurementNoiseVar4Tracker` | float | `16.0` | >= 0 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |
| `noiseWeightVar4Loc` | float | `-0.1` | any | NvSORT, NvDeepSORT, NvDCF | supported | with `noiseWeightVar4Vel`, both above 0: noise proportional to the box height |
| `noiseWeightVar4Vel` | float | `-0.1` | any | NvSORT, NvDeepSORT, NvDCF | supported | see `noiseWeightVar4Loc` |
| `useAspectRatio` | bool | `0` |  | NvSORT, NvDeepSORT, NvDCF | supported | the filter tracks w/h instead of w |

## Tracker config keys: `VisualTracker`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `visualTrackerType` | int | `0` | 0..2 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `useColorNames` | bool | `1` |  | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `useHog` | bool | `0` |  | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `featureImgSizeLevel` | int | `2` | 1..5 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `featureFocusOffsetFactor_y` | float | `0.0` | -0.5..0.5 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `useHighPrecisionFeature` | bool | `0` |  | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `filterLr` | float | `0.075` | 0..1 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `filterChannelWeightsLr` | float | `0.1` | 0..1 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `gaussianSigma` | float | `0.75` | >= 0 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `vpiBackend4DcfTracker` | int | `1` | 1..2 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |

## Tracker config keys: `ReID`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `reidType` | int | `0` | 0..3 | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `reidFeatureSize` | int | `128` | >= 1 | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `reidHistorySize` | int | `100` | >= 1 | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `addFeatureNormalization` | bool | `0` |  | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `keepAspc` | bool | `1` |  | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `outputReidTensor` | bool | `0` |  | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |

## Tracker config keys: `ReID`, `PoseEstimator` (model inference)

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `batchSize` | int | `1` | >= 1 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `workspaceSize` | int | `20` | >= 0 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `inferDims` | int list | `[128, 64, 3]` | >= 1 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `inputOrder` | int | `1` | 0..1 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `colorFormat` | int | `0` | >= 0 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `networkMode` | int | `0` | 0..2 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `offsets` | float list | `[0.0, 0.0, 0.0]` | any | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `netScaleFactor` | float | `1.0` | >= 0 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `tltEncodedModel` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `tltModelKey` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `onnxFile` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `modelEngineFile` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `calibrationTableFile` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `useVPICropScaler` | bool | `0` |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |

## Tracker config keys: `ObjectModelProjection`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `cameraModelFilepath` | string list | none |  | any (add-on module) | ignored | 3D object model projection is not supported |
| `waistHeightRatio` | float | `0.5` | 0..1 | any (add-on module) | ignored | 3D object model projection is not supported |
| `outputFootLocation` | bool | `0` |  | any (add-on module) | ignored | 3D object model projection is not supported |
| `outputConvexHull` | bool | `1` |  | any (add-on module) | ignored | 3D object model projection is not supported |
| `maxConvexHullSize` | int | `15` | >= 0 | any (add-on module) | ignored | 3D object model projection is not supported |
| `minPoseConfidence` | float | `0.5` | 0..1 | any (add-on module) | ignored | 3D object model projection is not supported |

## Tracker config keys: `PoseEstimator`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `poseEstimatorType` | int | `0` | 0..1 | any (add-on module) | ignored | pose estimation is not supported |
| `poseInferenceInterval` | int | `-1` | >= -1 | any (add-on module) | ignored | pose estimation is not supported |
| `operateOnClassIds` | int list | `[]` | >= 0 | any (add-on module) | ignored | pose estimation is not supported |
