/* Redstream: the rdstracker config parser. One table lists every documented NvMultiObjectTracker config key with its
 * section, type, range, default, status and the tracker types that use it; it drives parsing, the warnings and the
 * compat matrix. The YAML file is read into a GKeyFile first (sections become groups).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tracker-config.h"
#include "rds-config-parse.h"
#include "rds-keyfile-yaml.h"

#include <stdarg.h>
#include <string.h>

G_DEFINE_QUARK (rds-tracker-config-error-quark, rds_tracker_config_error)

typedef enum
{
  SEC_BASE,
  SEC_TARGET,
  SEC_TRAJECTORY,
  SEC_ASSOC,
  SEC_STATE,
  SEC_VISUAL,
  SEC_REID,
  SEC_MODEL,                    /* not a section of its own: the model inference keys, under ReID and PoseEstimator */
  SEC_PROJECTION,
  SEC_POSE,
  N_SECTIONS,
} Section;

static const gchar *const section_names[] = {
  [SEC_BASE] = "BaseConfig",[SEC_TARGET] = "TargetManagement",[SEC_TRAJECTORY] = "TrajectoryManagement",
  [SEC_ASSOC] = "DataAssociator",[SEC_STATE] = "StateEstimator",[SEC_VISUAL] = "VisualTracker",[SEC_REID] = "ReID",
  [SEC_MODEL] = "ReID, PoseEstimator",[SEC_PROJECTION] = "ObjectModelProjection",[SEC_POSE] = "PoseEstimator",
};

/* Sections of modules rdstracker doesn't have: their keys are type-checked, and one warning names them. */
static const Section ignored_sections[] = { SEC_VISUAL, SEC_REID, SEC_PROJECTION, SEC_POSE };

typedef enum
{
  TYPE_BOOL,                    /* 0/1 or true/false */
  TYPE_INT,
  TYPE_UINT,
  TYPE_FLOAT,
  TYPE_STRING,
  TYPE_INT_LIST,                /* a YAML sequence (';'-joined by the reader); checked only, no field */
  TYPE_FLOAT_LIST,
  TYPE_STRING_LIST,
} KeyType;

static const gchar *const type_names[] = {
  [TYPE_BOOL] = "bool",[TYPE_INT] = "int",[TYPE_UINT] = "uint",[TYPE_FLOAT] = "float",[TYPE_STRING] = "string",
  [TYPE_INT_LIST] = "int list",[TYPE_FLOAT_LIST] = "float list",[TYPE_STRING_LIST] = "string list",
};

/* The tracker types that use a key; none: an add-on module, usable with any tracker. */
enum
{
  T_IOU = 1 << 0,
  T_NVSORT = 1 << 1,
  T_NVDEEPSORT = 1 << 2,
  T_NVDCF = 1 << 3,
};
#define T_ALL (T_IOU | T_NVSORT | T_NVDEEPSORT | T_NVDCF)
#define T_KF (T_NVSORT | T_NVDEEPSORT | T_NVDCF)
#define T_REID (T_NVDEEPSORT | T_NVDCF)

typedef struct
{
  const gchar *name;
  Section section;
  KeyType type;
  RdsTrackerKeyStatus status;
  gdouble min, max;             /* numbers, and the elements of number lists */
  const gchar *def;             /* default as written in the file (lists as "[a, b]"); NULL: none / unset */
  gboolean def_ours;            /* the documentation gives no default */
  gssize field;                 /* offset + 1 into RdsTrackerConfig; 0: not stored */
  guint trackers;
  const gchar *note;
} KeyInfo;

#define F(f) (G_STRUCT_OFFSET (RdsTrackerConfig, f) + 1)
#define FIELD(type, base, k) G_STRUCT_MEMBER (type, base, (k)->field - 1)
#define SUP RDS_TRACKER_KEY_SUPPORTED
#define NOOP RDS_TRACKER_KEY_NOOP
#define IGN RDS_TRACKER_KEY_IGNORED
#define DEPR RDS_TRACKER_KEY_DEPRECATED
#define UNIT .min = 0,.max = 1
#define ANY .min = -G_MAXFLOAT,.max = G_MAXFLOAT
#define NONNEG_F .min = 0,.max = G_MAXFLOAT
#define NONNEG_I .min = 0,.max = G_MAXINT

#define NOTE_DCF "used only by NvDCF (visual tracker), which is not supported"
#define NOTE_DEEPSORT "used only by NvDeepSORT (Re-ID), which is not supported"
#define NOTE_REASSOC "a re-association setting; re-association is not supported (enableReAssoc warns)"
#define NOTE_MISC "tracker misc data (past-frame, shadow, terminated tracks) is not output yet"
#define NOTE_VISUAL "the visual tracker (NvDCF) is not supported"
#define NOTE_REID "Re-ID is not supported"
#define NOTE_MODEL "the Re-ID and pose models are not supported"
#define NOTE_PROJECTION "3D object model projection is not supported"
#define NOTE_POSE "pose estimation is not supported"

/* Ranges and defaults are the documented ones unless a comment says "ours". */
static const KeyInfo keys[] = {
  /* BaseConfig */
  /* ours: any value (the documentation gives no range) */
  {.name = "minDetectorConfidence",.section = SEC_BASE,.type = TYPE_FLOAT,.status = SUP, ANY,.def = "0.0",
      .field = F (min_detector_confidence),.trackers = T_ALL},
  {.name = "checkClassMatch",.section = SEC_BASE,.type = TYPE_BOOL,.status = SUP,.def = "1",
        .field = F (check_class_match),.trackers = T_ALL,
      .note = "also read under DataAssociator; different values in the two sections are an error"},
  {.name = "useBatchNumForFrameId",.section = SEC_BASE,.type = TYPE_BOOL,.status = NOOP,.def = "0",
      .trackers = T_ALL,.note = "frame numbers always come from the frame meta"},

  /* TargetManagement */
  {.name = "preserveStreamUpdateOrder",.section = SEC_TARGET,.type = TYPE_BOOL,.status = NOOP,.def = "0",
      .trackers = T_ALL,.note = "IDs are always assigned in input order (streams in batch order)"},
  {.name = "maxTargetsPerStream",.section = SEC_TARGET,.type = TYPE_UINT,.status = SUP,.min = 0,.max = 65535,
      .def = "30",.field = F (max_targets_per_stream),.trackers = T_ALL},
  {.name = "minIouDiff4NewTarget",.section = SEC_TARGET,.type = TYPE_FLOAT,.status = SUP, UNIT,.def = "0.5",
      .field = F (min_iou_diff4new_target),.trackers = T_ALL},
  {.name = "enableBboxUnClipping",.section = SEC_TARGET,.type = TYPE_BOOL,.status = NOOP,.def = "0",
        .trackers = T_ALL,
      .note = "boxes are reported as detected (observed DeepStream 8.0 behaviour for IOU and NvSORT)"},
  {.name = "probationAge",.section = SEC_TARGET,.type = TYPE_UINT,.status = SUP, NONNEG_I,.def = "5",
      .field = F (probation_age),.trackers = T_ALL},
  {.name = "maxShadowTrackingAge",.section = SEC_TARGET,.type = TYPE_UINT,.status = SUP, NONNEG_I,.def = "38",
      .field = F (max_shadow_tracking_age),.trackers = T_ALL},
  {.name = "earlyTerminationAge",.section = SEC_TARGET,.type = TYPE_UINT,.status = SUP, NONNEG_I,.def = "2",
      .field = F (early_termination_age),.trackers = T_ALL},
  {.name = "outputTerminatedTracks",.section = SEC_TARGET,.type = TYPE_BOOL,.status = NOOP,.def = "0",
      .trackers = T_ALL,.note = NOTE_MISC},
  {.name = "outputShadowTracks",.section = SEC_TARGET,.type = TYPE_BOOL,.status = NOOP,.def = "0",
      .trackers = T_ALL,.note = NOTE_MISC},
  {.name = "outputVisibility",.section = SEC_TARGET,.type = TYPE_BOOL,.status = NOOP,.def = "0",
      .trackers = T_ALL,.note = "object visibility is not output yet"},
  {.name = "terminatedTrackFilename",.section = SEC_TARGET,.type = TYPE_STRING,.status = NOOP,
      .trackers = T_ALL,.note = NOTE_MISC},
  {.name = "minTrackerConfidence",.section = SEC_TARGET,.type = TYPE_FLOAT,.status = IGN, UNIT,.def = "0.6",
        .trackers = T_NVDCF,
      .note = "no effect for IOU and NvSORT, whose tracker confidence is constant"},
  {.name = "searchRegionPaddingScale",.section = SEC_TARGET,.type = TYPE_INT,.status = IGN,.min = 1,.max = 3,
      .def = "1",.trackers = T_NVDCF,.note = NOTE_DCF},

  /* TrajectoryManagement */
  {.name = "useUniqueID",.section = SEC_TRAJECTORY,.type = TYPE_BOOL,.status = SUP,.def = "0",
      .field = F (use_unique_id),.trackers = T_ALL},
  {.name = "enableReAssoc",.section = SEC_TRAJECTORY,.type = TYPE_BOOL,.status = NOOP,.def = "0",
      .trackers = T_NVDCF,.note = "target re-association is not supported; its other keys are ignored"},
  {.name = "minMatchingScore4Overall",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.4",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "minTrackletMatchingScore",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.4",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "minMatchingScore4ReidSimilarity",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.8",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "matchingScoreWeight4TrackletSimilarity",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN,
      UNIT,.def = "1.0",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "matchingScoreWeight4ReidSimilarity",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.0",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "minTrajectoryLength4Projection",.section = SEC_TRAJECTORY,.type = TYPE_INT,.status = IGN, NONNEG_I,
      .def = "20",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "prepLength4TrajectoryProjection",.section = SEC_TRAJECTORY,.type = TYPE_INT,.status = IGN, NONNEG_I,
      .def = "10",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "trajectoryProjectionLength",.section = SEC_TRAJECTORY,.type = TYPE_INT,.status = IGN, NONNEG_I,
      .def = "90",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "maxAngle4TrackletMatching",.section = SEC_TRAJECTORY,.type = TYPE_INT,.status = IGN,.min = 0,
      .max = 180,.def = "40",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "minSpeedSimilarity4TrackletMatching",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.3",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "minBboxSizeSimilarity4TrackletMatching",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN,
      UNIT,.def = "0.6",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "maxTrackletMatchingTimeSearchRange",.section = SEC_TRAJECTORY,.type = TYPE_INT,.status = IGN,
      NONNEG_I,.def = "20",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "trajectoryProjectionProcessNoiseScale",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN,
      NONNEG_F,.def = "1.0",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "trajectoryProjectionMeasurementNoiseScale",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,
      .status = IGN, NONNEG_F,.def = "1.0",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "trackletSpacialSearchRegionScale",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN,
      NONNEG_F,.def = "0.0",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  {.name = "reidExtractionInterval",.section = SEC_TRAJECTORY,.type = TYPE_INT,.status = IGN,.min = -1,
      .max = G_MAXINT,.def = "0",.trackers = T_NVDCF,.note = NOTE_REASSOC},
  /* undocumented: in a published NvDCF sample config only; permissive types and no range, so such configs load
   * quietly */
  {.name = "enableVanishingTrackletReconstruction",.section = SEC_TRAJECTORY,.type = TYPE_INT,.status = IGN,
      .min = G_MININT,.max = G_MAXINT,.trackers = T_NVDCF,.note = "undocumented; " NOTE_REASSOC},
  {.name = "minInclusionRatio4DuplicateTrackletRemoval",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,
      .status = IGN, ANY,.trackers = T_NVDCF,.note = "undocumented; " NOTE_REASSOC},
  {.name = "minIou4DuplicateTrackletRemoval",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN, ANY,
      .trackers = T_NVDCF,.note = "undocumented; " NOTE_REASSOC},
  {.name = "minMatchRatio4ValidTrackletDetermination",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,.status = IGN,
      ANY,.trackers = T_NVDCF,.note = "undocumented; " NOTE_REASSOC},
  {.name = "minVisibility4VanishingTrackletReconstruction",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,
      .status = IGN, ANY,.trackers = T_NVDCF,.note = "undocumented; " NOTE_REASSOC},
  {.name = "visibilityThreshold4VanishingTrackletDetection",.section = SEC_TRAJECTORY,.type = TYPE_FLOAT,
      .status = IGN, ANY,.trackers = T_NVDCF,.note = "undocumented; " NOTE_REASSOC},
  {.name = "checkClassMatchForReAssoc",.section = SEC_TRAJECTORY,.type = TYPE_BOOL,.status = DEPR,
      .trackers = T_NVDCF,.note = "deprecated: use checkClassMatch"},

  /* DataAssociator */
  {.name = "dataAssociatorType",.section = SEC_ASSOC,.type = TYPE_INT,.status = NOOP,.min = 0,.max = 0,
      .def = "0",.trackers = T_ALL,.note = "0 (the default data associator) is the only type"},
  {.name = "associationMatcherType",.section = SEC_ASSOC,.type = TYPE_INT,.status = SUP,.min = 0,.max = 1,
      .def = "0",.field = F (association_matcher_type),.trackers = T_ALL},
  {.name = "checkClassMatch",.section = SEC_ASSOC,.type = TYPE_BOOL,.status = SUP,.def = "1",
        .field = F (check_class_match),.trackers = T_ALL,
      .note = "where observed DeepStream 8.0 behaviour reads it; also read under BaseConfig"},
  {.name = "minMatchingScore4Overall",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = SUP, UNIT,.def = "0.0",
      .field = F (min_matching_score4overall),.trackers = T_ALL},
  {.name = "minMatchingScore4SizeSimilarity",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = SUP, UNIT,
      .def = "0.0",.field = F (min_matching_score4size_similarity),.trackers = T_ALL},
  {.name = "minMatchingScore4Iou",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = SUP, UNIT,.def = "0.0",
      .field = F (min_matching_score4iou),.trackers = T_ALL},
  {.name = "matchingScoreWeight4SizeSimilarity",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = SUP, UNIT,
      .def = "0.0",.field = F (matching_score_weight4size_similarity),.trackers = T_ALL},
  {.name = "matchingScoreWeight4Iou",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = SUP, UNIT,.def = "1.0",
      .field = F (matching_score_weight4iou),.trackers = T_ALL},
  {.name = "tentativeDetectorConfidence",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = SUP, UNIT,
      .def = "0.5",.field = F (tentative_detector_confidence),.trackers = T_KF},
  {.name = "minMatchingScore4TentativeIou",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = SUP, UNIT,
      .def = "0.0",.field = F (min_matching_score4tentative_iou),.trackers = T_KF},
  {.name = "minMatchingScore4VisualSimilarity",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.0",.trackers = T_NVDCF,.note = NOTE_DCF},
  {.name = "matchingScoreWeight4VisualSimilarity",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.0",.trackers = T_NVDCF,.note = NOTE_DCF},
  /* > 0 means set; any value is accepted */
  {.name = "thresholdMahalanobis",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = IGN, ANY,.def = "-1.0",
      .trackers = T_NVDEEPSORT,.note = NOTE_DEEPSORT},
  {.name = "minMatchingScore4ReidSimilarity",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.0",.trackers = T_NVDEEPSORT,.note = NOTE_DEEPSORT},
  {.name = "matchingScoreWeight4ReidSimilarity",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.0",.trackers = T_NVDEEPSORT,.note = NOTE_DEEPSORT},
  /* the documentation's table spells it this way, its sample configs as above */
  {.name = "matchingScoreWeight4ReIDSimilarity",.section = SEC_ASSOC,.type = TYPE_FLOAT,.status = IGN, UNIT,
      .def = "0.0",.trackers = T_NVDEEPSORT,.note = NOTE_DEEPSORT},
  /* Undocumented (it appears in the NvSORT sample config). Ours: a bool, defaulting to 1 for NvSORT and 0 for IOU
   * (set after the tracker type is known). */
  {.name = "usePrediction4Assoc",.section = SEC_ASSOC,.type = TYPE_BOOL,.status = SUP,.def_ours = TRUE,
        .field = F (use_prediction4assoc),.trackers = T_KF,
      .note = "undocumented; associate against the predicted box; default (ours) 1 for NvSORT, 0 for IOU"},

  /* StateEstimator */
  {.name = "stateEstimatorType",.section = SEC_STATE,.type = TYPE_INT,.status = SUP,.min = 0,.max = 3,.def = "0",
        .field = F (state_estimator_type),.trackers = T_ALL,
      .note = "3 (SIMPLE_LOC) runs as 2 (REGULAR), with a warning"},
  {.name = "processNoiseVar4Loc",.section = SEC_STATE,.type = TYPE_FLOAT,.status = SUP, NONNEG_F,.def = "2.0",
      .field = F (process_noise_var4loc),.trackers = T_KF},
  {.name = "processNoiseVar4Size",.section = SEC_STATE,.type = TYPE_FLOAT,.status = SUP, NONNEG_F,.def = "1.0",
      .field = F (process_noise_var4size),.trackers = T_KF},
  {.name = "processNoiseVar4Vel",.section = SEC_STATE,.type = TYPE_FLOAT,.status = SUP, NONNEG_F,.def = "0.1",
      .field = F (process_noise_var4vel),.trackers = T_KF},
  {.name = "measurementNoiseVar4Detector",.section = SEC_STATE,.type = TYPE_FLOAT,.status = SUP, NONNEG_F,
      .def = "4.0",.field = F (measurement_noise_var4detector),.trackers = T_KF},
  {.name = "measurementNoiseVar4Tracker",.section = SEC_STATE,.type = TYPE_FLOAT,.status = IGN, NONNEG_F,
      .def = "16.0",.trackers = T_NVDCF,.note = NOTE_DCF},
  /* > 0 means set; any value is accepted */
  {.name = "noiseWeightVar4Loc",.section = SEC_STATE,.type = TYPE_FLOAT,.status = SUP, ANY,.def = "-0.1",
      .field = F (noise_weight_var4loc),.trackers = T_KF},
  {.name = "noiseWeightVar4Vel",.section = SEC_STATE,.type = TYPE_FLOAT,.status = SUP, ANY,.def = "-0.1",
      .field = F (noise_weight_var4vel),.trackers = T_KF},
  {.name = "useAspectRatio",.section = SEC_STATE,.type = TYPE_BOOL,.status = SUP,.def = "0",
      .field = F (use_aspect_ratio),.trackers = T_KF},

  /* VisualTracker */
  {.name = "visualTrackerType",.section = SEC_VISUAL,.type = TYPE_INT,.status = IGN,.min = 0,.max = 2,.def = "0",
      .trackers = T_NVDCF,.note = NOTE_VISUAL},
  {.name = "useColorNames",.section = SEC_VISUAL,.type = TYPE_BOOL,.status = IGN,.def = "1",.trackers = T_NVDCF,
      .note = NOTE_VISUAL},
  {.name = "useHog",.section = SEC_VISUAL,.type = TYPE_BOOL,.status = IGN,.def = "0",.trackers = T_NVDCF,
      .note = NOTE_VISUAL},
  {.name = "featureImgSizeLevel",.section = SEC_VISUAL,.type = TYPE_INT,.status = IGN,.min = 1,.max = 5,
      .def = "2",.trackers = T_NVDCF,.note = NOTE_VISUAL},
  {.name = "featureFocusOffsetFactor_y",.section = SEC_VISUAL,.type = TYPE_FLOAT,.status = IGN,.min = -0.5,
      .max = 0.5,.def = "0.0",.trackers = T_NVDCF,.note = NOTE_VISUAL},
  {.name = "useHighPrecisionFeature",.section = SEC_VISUAL,.type = TYPE_BOOL,.status = IGN,.def = "0",
      .trackers = T_NVDCF,.note = NOTE_VISUAL},
  /* ours: learning rates 0..1, sigma >= 0 */
  {.name = "filterLr",.section = SEC_VISUAL,.type = TYPE_FLOAT,.status = IGN, UNIT,.def = "0.075",
      .trackers = T_NVDCF,.note = NOTE_VISUAL},
  {.name = "filterChannelWeightsLr",.section = SEC_VISUAL,.type = TYPE_FLOAT,.status = IGN, UNIT,.def = "0.1",
      .trackers = T_NVDCF,.note = NOTE_VISUAL},
  {.name = "gaussianSigma",.section = SEC_VISUAL,.type = TYPE_FLOAT,.status = IGN, NONNEG_F,.def = "0.75",
      .trackers = T_NVDCF,.note = NOTE_VISUAL},
  {.name = "vpiBackend4DcfTracker",.section = SEC_VISUAL,.type = TYPE_INT,.status = IGN,.min = 1,.max = 2,
      .def = "1",.trackers = T_NVDCF,.note = NOTE_VISUAL},

  /* ReID; ours: sizes >= 1 */
  {.name = "reidType",.section = SEC_REID,.type = TYPE_INT,.status = IGN,.min = 0,.max = 3,.def = "0",
      .trackers = T_REID,.note = NOTE_REID},
  {.name = "reidFeatureSize",.section = SEC_REID,.type = TYPE_INT,.status = IGN,.min = 1,.max = G_MAXINT,
      .def = "128",.trackers = T_REID,.note = NOTE_REID},
  {.name = "reidHistorySize",.section = SEC_REID,.type = TYPE_INT,.status = IGN,.min = 1,.max = G_MAXINT,
      .def = "100",.trackers = T_REID,.note = NOTE_REID},
  {.name = "addFeatureNormalization",.section = SEC_REID,.type = TYPE_BOOL,.status = IGN,.def = "0",
      .trackers = T_REID,.note = NOTE_REID},
  {.name = "keepAspc",.section = SEC_REID,.type = TYPE_BOOL,.status = IGN,.def = "1",.trackers = T_REID,
      .note = NOTE_REID},
  {.name = "outputReidTensor",.section = SEC_REID,.type = TYPE_BOOL,.status = IGN,.def = "0",.trackers = T_REID,
      .note = NOTE_REID},

  /* Model inference, under ReID and PoseEstimator. Ours: batchSize >= 1, colorFormat >= 0, inferDims >= 1,
   * netScaleFactor >= 0, offsets any. */
  {.name = "batchSize",.section = SEC_MODEL,.type = TYPE_INT,.status = IGN,.min = 1,.max = G_MAXINT,.def = "1",
      .trackers = T_REID,.note = NOTE_MODEL},
  {.name = "workspaceSize",.section = SEC_MODEL,.type = TYPE_INT,.status = IGN, NONNEG_I,.def = "20",
      .trackers = T_REID,.note = NOTE_MODEL},
  {.name = "inferDims",.section = SEC_MODEL,.type = TYPE_INT_LIST,.status = IGN,.min = 1,.max = G_MAXINT,
      .def = "[128, 64, 3]",.trackers = T_REID,.note = NOTE_MODEL},
  {.name = "inputOrder",.section = SEC_MODEL,.type = TYPE_INT,.status = IGN,.min = 0,.max = 1,.def = "1",
      .trackers = T_REID,.note = NOTE_MODEL},
  {.name = "colorFormat",.section = SEC_MODEL,.type = TYPE_INT,.status = IGN, NONNEG_I,.def = "0",
      .trackers = T_REID,.note = NOTE_MODEL},
  {.name = "networkMode",.section = SEC_MODEL,.type = TYPE_INT,.status = IGN,.min = 0,.max = 2,.def = "0",
      .trackers = T_REID,.note = NOTE_MODEL},
  {.name = "offsets",.section = SEC_MODEL,.type = TYPE_FLOAT_LIST,.status = IGN, ANY,.def = "[0.0, 0.0, 0.0]",
      .trackers = T_REID,.note = NOTE_MODEL},
  {.name = "netScaleFactor",.section = SEC_MODEL,.type = TYPE_FLOAT,.status = IGN, NONNEG_F,.def = "1.0",
      .trackers = T_REID,.note = NOTE_MODEL},
  {.name = "tltEncodedModel",.section = SEC_MODEL,.type = TYPE_STRING,.status = IGN,.trackers = T_REID,
      .note = NOTE_MODEL},
  {.name = "tltModelKey",.section = SEC_MODEL,.type = TYPE_STRING,.status = IGN,.trackers = T_REID,
      .note = NOTE_MODEL},
  {.name = "onnxFile",.section = SEC_MODEL,.type = TYPE_STRING,.status = IGN,.trackers = T_REID,
      .note = NOTE_MODEL},
  {.name = "modelEngineFile",.section = SEC_MODEL,.type = TYPE_STRING,.status = IGN,.trackers = T_REID,
      .note = NOTE_MODEL},
  {.name = "calibrationTableFile",.section = SEC_MODEL,.type = TYPE_STRING,.status = IGN,.trackers = T_REID,
      .note = NOTE_MODEL},
  {.name = "useVPICropScaler",.section = SEC_MODEL,.type = TYPE_BOOL,.status = IGN,.def = "0",
      .trackers = T_REID,.note = NOTE_MODEL},

  /* ObjectModelProjection. cameraModelFilepath is a list per stream; its other documented form, a mapping keyed by
   * sensor name, nests deeper than the YAML reader accepts. Ours: ratios and confidences 0..1. */
  {.name = "cameraModelFilepath",.section = SEC_PROJECTION,.type = TYPE_STRING_LIST,.status = IGN,
      .note = NOTE_PROJECTION},
  {.name = "waistHeightRatio",.section = SEC_PROJECTION,.type = TYPE_FLOAT,.status = IGN, UNIT,.def = "0.5",
      .note = NOTE_PROJECTION},
  {.name = "outputFootLocation",.section = SEC_PROJECTION,.type = TYPE_BOOL,.status = IGN,.def = "0",
      .note = NOTE_PROJECTION},
  {.name = "outputConvexHull",.section = SEC_PROJECTION,.type = TYPE_BOOL,.status = IGN,.def = "1",
      .note = NOTE_PROJECTION},
  {.name = "maxConvexHullSize",.section = SEC_PROJECTION,.type = TYPE_INT,.status = IGN, NONNEG_I,.def = "15",
      .note = NOTE_PROJECTION},
  {.name = "minPoseConfidence",.section = SEC_PROJECTION,.type = TYPE_FLOAT,.status = IGN, UNIT,.def = "0.5",
      .note = NOTE_PROJECTION},

  /* PoseEstimator (plus the model inference keys); ours: class ids >= 0 */
  {.name = "poseEstimatorType",.section = SEC_POSE,.type = TYPE_INT,.status = IGN,.min = 0,.max = 1,.def = "0",
      .note = NOTE_POSE},
  {.name = "poseInferenceInterval",.section = SEC_POSE,.type = TYPE_INT,.status = IGN,.min = -1,.max = G_MAXINT,
      .def = "-1",.note = NOTE_POSE},
  {.name = "operateOnClassIds",.section = SEC_POSE,.type = TYPE_INT_LIST,.status = IGN, NONNEG_I,.def = "[]",
      .note = NOTE_POSE},
};

typedef struct
{
  const gchar *path;
  RdsTrackerConfig *cfg;
  GHashTable *warned;           /* qualified keys already warned about */
  gboolean has_use_prediction;
  gchar *check_class_match[2];  /* the raw values under BaseConfig and DataAssociator; NULL: not set */
} LoadCtx;

/* Is @k valid in the file section @sec? The model inference keys are valid under ReID and PoseEstimator. */
static gboolean
in_section (const KeyInfo * k, Section sec)
{
  return k->section == sec || (k->section == SEC_MODEL && (sec == SEC_REID || sec == SEC_POSE));
}

static gboolean G_GNUC_PRINTF (6, 7)
key_error (GError ** error, const gchar * path, const gchar * section, const gchar * key, const gchar * value,
    const gchar * fmt, ...)
{
  gchar *reason;
  va_list ap;

  va_start (ap, fmt);
  reason = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_TRACKER_CONFIG_ERROR, RDS_TRACKER_CONFIG_ERROR_INVALID, "%s:%s.%s: %s: %s", path, section,
      key, value, reason);
  g_free (reason);
  return FALSE;
}

/* "FILE:WHAT: message", at most once per @what and load. */
static void G_GNUC_PRINTF (3, 4)
warn_once (LoadCtx * ctx, const gchar * what, const gchar * fmt, ...)
{
  gchar *msg;
  va_list ap;

  if (!g_hash_table_add (ctx->warned, g_strdup (what)))
    return;
  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_ptr_array_add (ctx->cfg->warnings, g_strdup_printf ("%s:%s: %s", ctx->path, what, msg));
  g_free (msg);
}

static gboolean
is_number_type (KeyType type)
{
  return type == TYPE_INT || type == TYPE_UINT || type == TYPE_FLOAT || type == TYPE_INT_LIST
      || type == TYPE_FLOAT_LIST;
}

static gboolean
is_integer_type (KeyType type)
{
  return type == TYPE_INT || type == TYPE_UINT || type == TYPE_INT_LIST;
}

static gboolean
is_list_type (KeyType type)
{
  return type == TYPE_INT_LIST || type == TYPE_FLOAT_LIST || type == TYPE_STRING_LIST;
}

/* Parses one number of @k (a scalar or a list element) and checks its range; on failure @reason says why. */
static gboolean
parse_number (const KeyInfo * k, const gchar * s, gdouble * out, gchar ** reason)
{
  return rds_config_parse_number (s, is_integer_type (k->type), k->min, FALSE, k->max, out, reason);
}

/* Parses @raw (as the YAML reader gives it: lists ';'-joined) as @k's type; with @base, stores a scalar into k's
 * field. @num receives the value of a bool or a scalar number. On failure @reason says why. */
static gboolean
parse_value (const KeyInfo * k, const gchar * raw, gpointer base, gdouble * num, gchar ** reason)
{
  gchar *s = g_strstrip (g_strdup (raw)), **items = NULL;
  gboolean ok = TRUE;
  gdouble v = 0;

  switch (k->type) {
    case TYPE_BOOL:{
      gboolean b = FALSE;

      if (!(ok = rds_config_parse_bool (s, &b)))
        *reason = g_strdup ("not a boolean (use 0/1 or true/false)");
      else if (base)
        FIELD (gboolean, base, k) = b;
      v = b;
      break;
    }
    case TYPE_INT:
    case TYPE_UINT:
    case TYPE_FLOAT:
      if (!(ok = parse_number (k, s, &v, reason)) || !base)
        break;
      if (k->type == TYPE_INT)
        FIELD (gint, base, k) = (gint) v;
      else if (k->type == TYPE_UINT)
        FIELD (guint, base, k) = (guint) v;
      else
        FIELD (gfloat, base, k) = (gfloat) v;
      break;
    case TYPE_STRING:
    case TYPE_STRING_LIST:
      break;
    case TYPE_INT_LIST:
    case TYPE_FLOAT_LIST:{
      gdouble item;

      /* an empty list is "" */
      items = *s ? g_strsplit (s, ";", -1) : g_new0 (gchar *, 1);
      for (guint i = 0; ok && items[i]; i++)
        ok = parse_number (k, g_strstrip (items[i]), &item, reason);
      break;
    }
  }
  g_strfreev (items);
  g_free (s);
  *num = v;
  return ok;
}

/* A default as the YAML reader would give it: "[a, b]" becomes "a;b". */
static gchar *
default_raw (const KeyInfo * k)
{
  gchar *inner, **items, *raw;

  if (!is_list_type (k->type) || !g_str_has_prefix (k->def, "[") || !g_str_has_suffix (k->def, "]"))
    return g_strdup (k->def);
  inner = g_strndup (k->def + 1, strlen (k->def) - 2);
  items = g_strsplit (inner, ",", -1);
  for (guint i = 0; items[i]; i++)
    g_strstrip (items[i]);
  raw = g_strjoinv (";", items);
  g_strfreev (items);
  g_free (inner);
  return raw;
}

/* Parses @k's default (into @base when given); rds_tracker_config_check_table makes sure every default parses. */
static gboolean
parse_default (const KeyInfo * k, gpointer base, gdouble * num, gchar ** reason)
{
  gchar *raw = default_raw (k);
  gboolean ok = parse_value (k, raw, base, num, reason);

  g_free (raw);
  return ok;
}

static void
apply_defaults (RdsTrackerConfig * cfg)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    gchar *reason = NULL;
    gdouble v;

    if (keys[i].status != RDS_TRACKER_KEY_SUPPORTED || !keys[i].def)
      continue;
    if (!parse_default (&keys[i], cfg, &v, &reason))
      g_error ("bad default for %s.%s: %s", section_names[keys[i].section], keys[i].name, reason);
  }
}

/* Is @raw (valid for @k) its default? A string's default is empty. */
static gboolean
is_default (const KeyInfo * k, const gchar * raw, gdouble v)
{
  gdouble def;
  gchar *reason = NULL;

  if (k->type == TYPE_STRING || is_list_type (k->type) || !k->def)
    return !*raw;
  if (!parse_default (k, NULL, &def, &reason)) {
    g_free (reason);
    return FALSE;
  }
  return v == def;
}

/* Parses the keys of the file section @sec (group @group): known keys in table order, then a warning for each
 * unknown one. */
static gboolean
parse_section (LoadCtx * ctx, GKeyFile * kf, const gchar * group, Section sec, GError ** error)
{
  gchar **names;

  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gchar *raw, *reason = NULL, *qkey;
    gdouble v;

    if (!in_section (k, sec) || !g_key_file_has_key (kf, group, k->name, NULL))
      continue;
    /* the YAML reader stores escaped strings, which always read back; the raw value is a fallback */
    if (!(raw = g_key_file_get_string (kf, group, k->name, NULL)))
      raw = g_key_file_get_value (kf, group, k->name, NULL);
    g_strstrip (raw);
    if (!parse_value (k, raw, k->status == RDS_TRACKER_KEY_SUPPORTED ? ctx->cfg : NULL, &v, &reason)) {
      key_error (error, ctx->path, group, k->name, raw, "%s", reason);
      g_free (reason);
      g_free (raw);
      return FALSE;
    }
    qkey = g_strconcat (group, ".", k->name, NULL);
    if (k->status == RDS_TRACKER_KEY_DEPRECATED
        || (k->status == RDS_TRACKER_KEY_NOOP && !is_default (k, raw, v)))
      warn_once (ctx, qkey, "%s: ignored: %s", raw, k->note);
    if (k->field == F (use_prediction4assoc))
      ctx->has_use_prediction = TRUE;
    if (k->field == F (check_class_match)) {
      g_free (ctx->check_class_match[sec == SEC_ASSOC]);
      ctx->check_class_match[sec == SEC_ASSOC] = g_strdup (raw);
    }
    g_free (qkey);
    g_free (raw);
  }

  names = g_key_file_get_keys (kf, group, NULL, NULL);
  for (guint i = 0; names && names[i]; i++) {
    gboolean known = FALSE;

    for (guint j = 0; !known && j < G_N_ELEMENTS (keys); j++)
      known = in_section (&keys[j], sec) && !strcmp (keys[j].name, names[i]);
    if (!known) {
      gchar *qkey = g_strconcat (group, ".", names[i], NULL);

      warn_once (ctx, qkey, "unknown key, ignored");
      g_free (qkey);
    }
  }
  g_strfreev (names);
  return TRUE;
}

static gboolean
parse_file (LoadCtx * ctx, GKeyFile * kf, GError ** error)
{
  RdsTrackerConfig *cfg = ctx->cfg;
  gchar **groups = g_key_file_get_groups (kf, NULL);
  gboolean ok = TRUE, present[N_SECTIONS] = { FALSE };
  GString *ignored = g_string_new (NULL);

  for (guint i = 0; ok && groups[i]; i++) {
    Section sec = N_SECTIONS;

    for (guint s = 0; s < N_SECTIONS; s++)
      if (s != SEC_MODEL && !strcmp (groups[i], section_names[s]))
        sec = s;
    if (sec == N_SECTIONS) {
      warn_once (ctx, groups[i], "unknown section, ignored");
      continue;
    }
    present[sec] = TRUE;
    ok = parse_section (ctx, kf, groups[i], sec, error);
  }
  g_strfreev (groups);

  /* the same checkClassMatch wherever it is set (both sections parse it as a bool, so these parse) */
  if (ok && ctx->check_class_match[0] && ctx->check_class_match[1]) {
    gboolean base = FALSE, assoc = FALSE;

    rds_config_parse_bool (ctx->check_class_match[0], &base);
    rds_config_parse_bool (ctx->check_class_match[1], &assoc);
    if (base != assoc)
      ok = key_error (error, ctx->path, section_names[SEC_ASSOC], "checkClassMatch", ctx->check_class_match[1],
          "differs from BaseConfig.checkClassMatch (%s); set it in one section", ctx->check_class_match[0]);
  }
  if (!ok)
    goto out;

  if (cfg->state_estimator_type == 3) {
    warn_once (ctx, "StateEstimator.stateEstimatorType",
        "3: SIMPLE_LOC is not supported, the regular state estimator (2) runs instead");
    cfg->state_estimator_type = 2;
  }
  cfg->tracker_type = cfg->state_estimator_type == 0 && cfg->association_matcher_type == 0
      ? RDS_TRACKER_TYPE_IOU : RDS_TRACKER_TYPE_NVSORT;
  if (!ctx->has_use_prediction)
    cfg->use_prediction4assoc = cfg->tracker_type == RDS_TRACKER_TYPE_NVSORT;

  for (guint i = 0; i < G_N_ELEMENTS (ignored_sections); i++)
    if (present[ignored_sections[i]])
      g_string_append_printf (ignored, "%s%s", ignored->len ? ", " : "", section_names[ignored_sections[i]]);
  if (ignored->len)
    warn_once (ctx, ignored->str, "ignored: rdstracker has no visual tracker, Re-ID, 3D model projection or pose "
        "estimation; the config runs as %s", rds_tracker_type_name (cfg->tracker_type));
out:
  g_string_free (ignored, TRUE);
  return ok;
}

RdsTrackerConfig *
rds_tracker_config_load (const gchar * path, GError ** error)
{
  RdsTrackerConfig *cfg;
  GKeyFile *kf = NULL;
  LoadCtx ctx = { 0 };
  GError *err = NULL;
  gboolean ok = TRUE;
  GPtrArray *warnings = g_ptr_array_new_with_free_func (g_free);
  const gchar *lenient[G_N_ELEMENTS (ignored_sections) + 1] = { NULL };

  /* the sections rdstracker ignores may nest deeper than the reader takes (cameraModelFilepath keyed by sensor
   * name): those values are skipped with a warning */
  for (guint i = 0; i < G_N_ELEMENTS (ignored_sections); i++)
    lenient[i] = section_names[ignored_sections[i]];
  if (path && !(kf = rds_keyfile_from_yaml_full (path, lenient, warnings, &err))) {
    g_ptr_array_unref (warnings);
    if (err->domain == G_FILE_ERROR)
      g_set_error (error, RDS_TRACKER_CONFIG_ERROR, RDS_TRACKER_CONFIG_ERROR_FILE, "%s: can't read the config: %s",
          path, err->message);
    else                        /* the YAML reader names the file and line */
      g_set_error_literal (error, RDS_TRACKER_CONFIG_ERROR, RDS_TRACKER_CONFIG_ERROR_PARSE, err->message);
    g_error_free (err);
    return NULL;
  }

  cfg = g_new0 (RdsTrackerConfig, 1);
  cfg->warnings = warnings;
  apply_defaults (cfg);
  cfg->tracker_type = RDS_TRACKER_TYPE_IOU;
  ctx.path = path;
  ctx.cfg = cfg;
  ctx.warned = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  if (kf) {
    ok = parse_file (&ctx, kf, error);
    g_key_file_unref (kf);
  }
  g_hash_table_unref (ctx.warned);
  g_free (ctx.check_class_match[0]);
  g_free (ctx.check_class_match[1]);
  if (!ok)
    g_clear_pointer (&cfg, rds_tracker_config_free);
  return cfg;
}

void
rds_tracker_config_free (RdsTrackerConfig * cfg)
{
  if (!cfg)
    return;
  if (cfg->warnings)
    g_ptr_array_unref (cfg->warnings);
  g_free (cfg);
}

const gchar *
rds_tracker_type_name (RdsTrackerType type)
{
  return type == RDS_TRACKER_TYPE_IOU ? "IOU" : "NvSORT";
}

static gboolean
is_type_max (const KeyInfo * k)
{
  if (k->type == TYPE_FLOAT || k->type == TYPE_FLOAT_LIST)
    return k->max == G_MAXFLOAT;
  return k->max == G_MAXINT;
}

static gchar *
trackers_text (guint trackers)
{
  static const gchar *const names[] = { "IOU", "NvSORT", "NvDeepSORT", "NvDCF" };
  GString *str = g_string_new (NULL);

  if (!trackers)
    return g_string_free (g_string_append (str, "any (add-on module)"), FALSE);
  for (guint i = 0; i < G_N_ELEMENTS (names); i++)
    if (trackers & (1u << i))
      g_string_append_printf (str, "%s%s", str->len ? ", " : "", names[i]);
  return g_string_free (str, FALSE);
}

void
rds_tracker_config_foreach_key (RdsTrackerConfigKeyFunc func, gpointer user_data)
{
  g_return_if_fail (func != NULL);

  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gchar min[G_ASCII_DTOSTR_BUF_SIZE], max[G_ASCII_DTOSTR_BUF_SIZE], *range = NULL;
    gchar *trackers = trackers_text (k->trackers);
    RdsTrackerKeyDesc desc = {
      .section = section_names[k->section],
      .name = k->name,
      .type = type_names[k->type],
      .status = k->status,
      .default_value = k->def,
      .default_ours = k->def_ours,
      .trackers = trackers,
      .note = k->note,
    };

    if (is_number_type (k->type)) {
      rds_config_format_number (min, k->min);
      rds_config_format_number (max, k->max);
      if ((k->min == -G_MAXFLOAT || k->min == G_MININT) && is_type_max (k))
        range = g_strdup ("any");
      else if (is_type_max (k))
        range = g_strdup_printf (">= %s", min);
      else
        range = g_strdup_printf ("%s..%s", min, max);
    }
    desc.range = range;
    func (&desc, user_data);
    g_free (range);
    g_free (trackers);
  }
}

static gboolean G_GNUC_PRINTF (3, 4)
table_error (GError ** error, const KeyInfo * k, const gchar * fmt, ...)
{
  gchar *msg;
  va_list ap;

  va_start (ap, fmt);
  msg = g_strdup_vprintf (fmt, ap);
  va_end (ap);
  g_set_error (error, RDS_TRACKER_CONFIG_ERROR, RDS_TRACKER_CONFIG_ERROR_INVALID, "key table: %s.%s: %s",
      section_names[k->section], k->name, msg);
  g_free (msg);
  return FALSE;
}

gboolean
rds_tracker_config_check_table (GError ** error)
{
  for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
    const KeyInfo *k = &keys[i];
    gboolean scalar = k->type == TYPE_BOOL || k->type == TYPE_INT || k->type == TYPE_UINT
        || k->type == TYPE_FLOAT;

    /* unique per file section: the model inference keys share ReID and PoseEstimator */
    for (guint j = 0; j < i; j++)
      for (guint s = 0; s < N_SECTIONS; s++)
        if (s != SEC_MODEL && in_section (k, s) && in_section (&keys[j], s) && !strcmp (keys[j].name, k->name))
          return table_error (error, k, "listed twice in %s", section_names[s]);
    if ((k->status == RDS_TRACKER_KEY_SUPPORTED) != (k->field > 0))
      return table_error (error, k, "supported keys need a field, and only they have one");
    if (k->field > 0 && !scalar)
      return table_error (error, k, "only scalars have a field");
    if (k->status != RDS_TRACKER_KEY_SUPPORTED && !k->note)
      return table_error (error, k, "keys that are not supported need a note");
    if (k->status == RDS_TRACKER_KEY_SUPPORTED && !k->def && !(k->def_ours && k->note))
      return table_error (error, k, "supported keys need a default, or a note saying how ours is chosen");
    if (k->status == RDS_TRACKER_KEY_NOOP && !k->def && k->type != TYPE_STRING)
      return table_error (error, k, "no-op keys need a default to compare with");
    if (is_number_type (k->type) && k->min > k->max)
      return table_error (error, k, "a number needs min <= max");
    if (!is_number_type (k->type) && (k->min != 0 || k->max != 0))
      return table_error (error, k, "only numbers have a range");
    if (!k->trackers && k->section != SEC_PROJECTION && k->section != SEC_POSE)
      return table_error (error, k, "only add-on modules are used by no tracker type in particular");
    if (k->def) {
      gchar *reason = NULL;
      gdouble v;

      if (k->type == TYPE_STRING)
        return table_error (error, k, "strings have no default");
      if (!parse_default (k, NULL, &v, &reason)) {
        table_error (error, k, "default \"%s\": %s", k->def, reason);
        g_free (reason);
        return FALSE;
      }
    }
  }
  return TRUE;
}
