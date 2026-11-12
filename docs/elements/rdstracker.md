# rdstracker

How to read this page, and how to run the examples: [element reference](README.md).

Gives the objects of each stream a tracking ID: the IOU and NvSORT trackers of NvMultiObjectTracker, on the CPU,
configured by its YAML files. It works on the metadata only and writes the IDs and the tracked boxes into the object
metas, under the batch's metadata lock. DeepStream counterpart: `nvtracker`
([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvtracker)).
Built when: `-Dtracker=enabled`, or `auto` (the default) when libyaml is found.

## Example

The detector here is the committed test model `tests/models/fixeddet.onnx`, which finds the same six boxes in every
frame, so the tracker reports six tracks once their probation is over:

```sh
cat > /tmp/det.txt <<EOF
[property]
onnx-file=$PWD/tests/models/fixeddet.onnx
network-type=0
num-detected-classes=2
parse-bbox-func-name=RdsInferParseYolo
EOF

# "frame 3: 6 tracks reported" in the debug log (probationAge 3); boxes and IDs drawn by rdsosd
GST_DEBUG=rdstracker:5 gst-launch-1.0 videotestsrc num-buffers=10 ! video/x-raw,format=NV12,width=640,height=640 \
  ! m.sink_0 rdsstreammux name=m batch-size=1 width=640 height=640 ! rdsinfer config-file-path=/tmp/det.txt \
  ! rdstracker ll-config-file=samples/tracker/config_tracker_NvSORT.yml \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd ! fakesink 2>&1 | grep "tracks reported"
```

Without `ll-config-file` it runs an IOU tracker with every key's default. The repository's
`samples/tracker/config_tracker_IOU.yml` and `config_tracker_NvSORT.yml` are tuned on a 720p street clip.

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)`, format NV12, RGBA, BGRA, RGBx or BGRx, 1–16384 × 1–16384: batches with an `RdsBatchMeta` (from `rdsstreammux`) |
| `src` | src | always | as the sink |

Buffers pass through unchanged and pixels are never read: only the object metas change, in place, under the batch's
metadata lock. A buffer without batch meta is an element error.

## Properties

Names, types and defaults are those nvtracker reports in DeepStream 8.0 (7.1 is the same). The config file is read,
and every property checked, when the element starts (READY to PAUSED); an error there fails the state change with a
message naming the file or the property.

An accepted no-op property set to anything but its default posts one WARNING message at every start, except
`tracker-width`, `tracker-height` and `gpu-id`: those are ordinary settings that simply have no effect on a CPU
tracker working in frame coordinates, so they post nothing.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `ll-lib-file` | string | `NULL` | the built-in tracker only | NULL, READY | unset, `libnvds_nvmultiobjecttracker.so` or `librdstracker-builtin.so` (bare or as a path; a debug-log INFO message says the built-in one is used). Any other library fails at start |
| `ll-config-file` | string | `NULL` | an NvMultiObjectTracker YAML file | NULL, READY | the tracker's config ([rdstracker-config](rdstracker-config.md)); unset: every key's default, which is an IOU tracker |
| `tracker-width` | uint | `960` | 0–4294967295 | NULL, READY | accepted no-op, silent: boxes are tracked in the frame's (the mux's) pixels, as observed with DeepStream 8.0's IOU and NvSORT. 0 is accepted and ignored (ours: 8.0 gives NaN boxes) |
| `tracker-height` | uint | `544` | 0–4294967295 | NULL, READY | accepted no-op, silent: as `tracker-width` |
| `gpu-id` | uint | `0` | 0–4294967295 | NULL, READY | accepted no-op, silent: the tracker runs on the CPU, on the metadata only |
| `display-tracking-id` | boolean | `true` | | any state | append the ID to each tracked object's `display_text` (see [Tracking rules](#tracking-rules)); read per batch |
| `tracking-id-reset-mode` | uint | `1` | 0–3 | NULL, READY | 0: stream events change nothing; 1: a stream reset ends the stream's targets; 2: per-stream IDs, and a stream EOS ends the stream's targets and restarts its IDs at 0; 3: 1 and 2 together (see [Tracking rules](#tracking-rules)) |
| `operate-on-class-ids` | string | `NULL` | class ids separated by `;` (`0;2`) | NULL, READY | from the DeepStream 9.x docs (8.0 and 7.1 have no such property): track only these classes; unset or empty: every class. Objects of other classes pass untouched with `UNTRACKED_OBJECT_ID`. A token that isn't an integer fails at start |
| `tracking-surface-type` | uint | `0` | 0–4294967295 | NULL, READY | accepted no-op: every surface of the batch is tracked |
| `compute-hw` | enum `GstRdsTrackerComputeHw` | `default` | `0` `default`, `1` `gpu`, `2` `vic` | any state | accepted no-op: no frames are scaled |
| `input-tensor-meta` | boolean | `false` | | any state | accepted no-op: tensor meta input is not supported |
| `tensor-meta-gie-id` | uint | `0` | 0–4294967295 | NULL, READY | accepted no-op |
| `sub-batches` | string | `""` | | any state | accepted no-op: one tracker instance serves every stream |
| `sub-batch-err-recovery-trial-cnt` | int | `0` | −1–2147483647 | any state | accepted no-op |
| `user-meta-pool-size` | uint | `32` | 1–4294967295 | NULL, READY | accepted no-op: no tracker user meta is emitted yet |
| `qos` | boolean | `false` | | any state | GstBaseTransform's |

## Metadata

- **Reads**, per frame of the `RdsBatchMeta` (`NvDsBatchMeta`): the frame meta's `source_id` (the stream) and
  `bInferDone`, and each `RdsObjectMeta`'s `class_id`, `confidence`, `detector_bbox_info` (or `rect_params` for an
  object without one), and `obj_label`, `display_text` and `unique_component_id`, which are copied to label the
  objects it creates on frames without inference.
- **Writes** into each tracked object meta: `object_id`, `class_id` (the target's; with `checkClassMatch: 0` it
  follows the detection), `tracker_bbox_info`, `tracker_confidence`, `rect_params` and `text_params.display_text`
  (with `display-tracking-id`); untracked objects keep `RDS_UNTRACKED_OBJECT_ID` (`G_MAXUINT64`). It removes the
  detections of targets that aren't reported, and on frames without inference adds an object meta for every Active
  target. The rules are under [Tracking rules](#tracking-rules).
- **Tracker meta types:** defined, at their documented positions, so ported code compiles; `rdstracker` doesn't
  attach any of them yet (no past-frame data, shadow or terminated track lists, or visibility).

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NVDS_TRACKER_PAST_FRAME_META` | `RDS_TRACKER_PAST_FRAME_META` | supported | 15; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_BATCH_REID_META` | `RDS_TRACKER_BATCH_REID_META` | supported | 16; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_OBJ_REID_META` | `RDS_TRACKER_OBJ_REID_META` | supported | 17; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_TERMINATED_LIST_META` | `RDS_TRACKER_TERMINATED_LIST_META` | supported | 18; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_SHADOW_LIST_META` | `RDS_TRACKER_SHADOW_LIST_META` | supported | 19; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_VISIBILITY` | `RDS_OBJ_VISIBILITY` | supported | 20; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_IMAGE_FOOT_LOCATION` | `RDS_OBJ_IMAGE_FOOT_LOCATION` | supported | 21; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_WORLD_FOOT_LOCATION` | `RDS_OBJ_WORLD_FOOT_LOCATION` | supported | 22; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_IMAGE_CONVEX_HULL` | `RDS_OBJ_IMAGE_CONVEX_HULL` | supported | 23; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `TRACKER_STATE` | `RdsTrackerState` | supported | the type name only. The values are `RDS_TRACKER_STATE_EMPTY` (0), `_ACTIVE` (1), `_INACTIVE` (2), `_TENTATIVE` (3), `_PROJECTED` (4) and `_QUASIACTIVE` (5, as in DeepStream 8.0); DeepStream's bare `EMPTY`, `ACTIVE`, ... are not aliased, since such macros would rewrite any identifier of that name | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvds__tracker__meta_8h.html) |

## Config file

NvMultiObjectTracker's YAML (`ll-config-file`): the sections and every key with its type, default, range, the
trackers that use it and its status are on [rdstracker-config](rdstracker-config.md). The file is read at start.

## Behaviour

### Trackers

The config selects **IOU** (`stateEstimatorType: 0` and `associationMatcherType: 0`) or **NvSORT**
(anything else: a Kalman filter per target and the cascaded association). NvDeepSORT and NvDCF configs run as NvSORT
without their visual tracker and Re-ID, with a warning naming the ignored sections. The tracking itself is on the CPU:
about 0.01 ms per batch on a 720p street clip (about 22 detections a frame), 0.025 ms (IOU) and 0.047 ms (NvSORT) for 4
streams × 50 objects (release build, Ryzen 9 7900X).

### Tracking rules

The public docs describe the target life cycle and the matching, but leave many details open or
contradict themselves between versions. Each row is either **matches nvtracker (DeepStream 8.0)**: observed DeepStream
8.0 behaviour with the IOU and NvSORT trackers, or **ours**: where the docs and 8.0 settle nothing, or where 8.0 is
clearly broken. Terms: a target is **Tentative** until it is activated, then **Active** while matched and **Inactive**
(tracked but not reported) while unmatched; its **age** counts frames since it was created and its **shadow tracking
age** the consecutive frames with inference without a match.

| Behaviour | Status | Notes |
|---|---|---|
| Detections without a reported target are removed from the frame | matches nvtracker (DeepStream 8.0) | the detections of Tentative targets, those below `minDetectorConfidence`, duplicates (`minIouDiff4NewTarget`) and those over `maxTargetsPerStream` (Inactive targets count). Only objects of classes left out by `operate-on-class-ids` leave the tracker with `UNTRACKED_OBJECT_ID` |
| Which detections create targets | matches nvtracker (DeepStream 8.0) | IOU: any detection at or above `minDetectorConfidence`. NvSORT: only detections at or above `tentativeDetectorConfidence`; lower ones can only extend an Active target |
| Matched objects: boxes and fields | matches nvtracker (DeepStream 8.0) | `object_id` set; `tracker_bbox_info` is the detector's box (IOU and NvSORT, unclipped); `rect_params` is that box clipped to the frame; `confidence`, `detector_bbox_info`, `obj_label` and `unique_component_id` stay the detector's. `class_id` is the target's (with `checkClassMatch: 0` it follows the detection). NvSORT still updates its Kalman filter with the detection |
| `enableBboxUnClipping` | matches nvtracker (DeepStream 8.0) | no effect: boxes are reported as detected |
| `tracker_confidence` | matches nvtracker (DeepStream 8.0) | 0.5 on every tracked object, IOU and NvSORT (the docs, and DeepStream 7.1, give 1.0) |
| Frames without inference (`bInferDone` FALSE) | matches nvtracker (DeepStream 8.0) | every Active target is reported as a new object meta: its ID and class, `confidence` -0.1, `detector_bbox_info` all zero, `tracker_bbox_info` its last matched box (IOU) or its Kalman prediction (NvSORT), `rect_params` that box clipped. Tentative and Inactive targets are not reported. Objects already on such a frame are removed, never tracked |
| Labels of created objects | ours | `obj_label`, `display_text` and `unique_component_id` come from that target's own last matched detection (8.0 copies them from one detection per class), with rdsinfer's drawing defaults |
| Ages on frames without inference | matches nvtracker (DeepStream 8.0) | the age (and so probation) grows; the shadow tracking age doesn't |
| When IDs are assigned | matches nvtracker (DeepStream 8.0) | at activation, from 0; a Tentative target that dies takes no ID. Targets activated on the same frame are numbered in input order: streams in batch order, objects in `obj_meta_list` order |
| Activation | matches nvtracker (DeepStream 8.0) | age is 1 on the creation frame and grows by 1 every frame; a Tentative target activates on the first frame where its age exceeds `probationAge` and it is matched. `probationAge: 0` activates at once |
| Termination | matches nvtracker (DeepStream 8.0) | Active and Inactive targets end when the shadow tracking age exceeds `maxShadowTrackingAge`; Tentative ones when it reaches `earlyTerminationAge` or exceeds `maxShadowTrackingAge`. One match makes an Inactive target Active again. An object that returns after its target ended gets a new target and ID |
| `display-tracking-id` | matches nvtracker (DeepStream 8.0) | `display_text` becomes `"<display_text> <id>"`, the full 64-bit ID in decimal (rdsinfer's text is the label, so `"person 3"`); an empty text gives `" <id>"`. The string is replaced by a fresh one |
| `display-tracking-id` with a NULL `display_text` | ours | `"<id>"` (8.0 writes `"(null) <id>"`) |
| `tracking-id-reset-mode` | matches nvtracker (DeepStream 8.0) | one 32-bit counter per element, shared by every stream. 1: a stream reset ends the stream's targets; later detections go through probation again and get new IDs. 2: the lower 32 bits count per stream, from the counter's value at the stream's first target, so every stream starts at 0; a stream EOS ends the stream's targets and its count restarts at 0. 3: both |
| The ID counter is never restarted | ours | resets, EOS and flushes never restart the shared counter, so live IDs never repeat (8.0 restarts it, giving duplicate live IDs and `0xFFFFFFFF` after an underflow; 7.1 continues it, as here) |
| Size similarity | matches nvtracker (DeepStream 8.0) | the smaller area over the larger; the match score is `matchingScoreWeight4Iou`·IOU + `matchingScoreWeight4SizeSimilarity`·size similarity, and a pair must pass `minMatchingScore4Iou`, `minMatchingScore4SizeSimilarity` and `minMatchingScore4Overall` |
| Matching when all three minimums are 0 | ours | a pair also needs an IOU above 0, so a target never jumps to a disjoint box of a similar size. With any minimum set, the configured filters alone decide |
| Order of equal match scores | ours | greedy matching takes the best score first, then the lower target ID, then the earlier detection |
| NvSORT association | ours | the documented three stages: confirmed detections with Active and Inactive targets (joint score); tentative detections with the remaining Active targets (`minMatchingScore4TentativeIou`); the remaining confirmed detections with Tentative targets. Against the Kalman prediction when `usePrediction4Assoc` is 1 (default for NvSORT, ours) |
| `useUniqueID` | matches nvtracker (DeepStream 8.0) | the upper 32 bits are one random value, shared by every stream; 0 without it. Drawn again each time the element starts (ours) |
| `tracker-width` / `tracker-height` | matches nvtracker (DeepStream 8.0) | no effect on the boxes: tracking is in the frame's pixels |
| `tracker-width` / `tracker-height` 0 | ours | accepted and ignored (8.0 gives NaN boxes) |
| `checkClassMatch` | matches nvtracker (DeepStream 8.0) | 1: a target matches only detections of its class. Read under `DataAssociator` (as 8.0 does) and `BaseConfig` (as the current docs say) |
| Duplicate check (`minIouDiff4NewTarget`) | matches nvtracker (DeepStream 8.0) | a detection is a duplicate when its best IOU with any target of its stream, of any class and state, reaches the threshold |
| Duplicate check: which targets and boxes | ours | it includes targets created earlier on the same frame, and uses each target's box on this frame (for NvSORT the matched detection or the predicted box) |
| `minIouDiff4NewTarget: 0` | ours | only an overlapping detection (IOU above 0) can be a duplicate, so 0 doesn't block every new target |
| Invalid detection boxes | ours | a detection with a non-finite coordinate or a width or height of 0 or less is removed before matching |
| Which box a detection has | ours | the detector's unclipped `detector_bbox_info`; an object without one (added by an application) is tracked on its `rect_params` |
| Kalman prediction under 1 px | ours | a prediction that isn't a finite box at least 1 px wide and high is replaced by the target's last matched box |
| A box entirely outside the frame | ours | kept, with a `rect_params` of width or height 0 |
| Two frames of one source in a batch | ours | an element error. `rdsstreammux` never makes such batches; DeepStream's new streammux can (`max-same-source-frames`), which is a known limitation |
| Created objects in the frame's list | matches nvtracker (DeepStream 8.0) | matched objects keep their order; objects created for targets are put first, highest ID first |
| Kalman filter | ours | the docs describe the state and noise options, not the matrices. Constant velocity, one step per frame (also without inference), updated only on a match. Noise is fixed (`processNoiseVar4*`, `measurementNoiseVar4Detector`) or DeepSORT-style proportional to the box height; aspect-ratio terms use small constants, as in DeepSORT. Initial standard deviations 10·`noiseWeightVar4Loc`·h for position and size and 13·`noiseWeightVar4Vel`·h for velocities, fitted to observed 8.0 output: a constant-velocity object's predictions stay within 1.4 px of 8.0's (8.0's filter differs in structure, so no exact match) |
| Height-proportional noise | ours | used only when both `noiseWeightVar4Loc` and `noiseWeightVar4Vel` are above 0; otherwise the fixed noise |
| `stateEstimatorType: 3` | ours | SIMPLE_LOC runs as 2 (REGULAR), with a warning |
| Streams and events | ours | a stream is the frame meta's `source_id`, the ID `rdsstreammux`'s stream events carry. `rds-pad-deleted` frees the stream's state; FLUSH_STOP clears every stream (the counter continues). Every event goes downstream unchanged |

`rds-pad-deleted`, `rds-stream-eos` and `rds-stream-reset` are `rdsstreammux`'s stream events for one source
([rdsstreammux](rdsstreammux.md), `rds/rds-event.h`).

### Errors

All element errors.

- At start:
  - config-file errors: a file that can't be read (RESOURCE, NOT_FOUND), or any other error in it (RESOURCE,
    SETTINGS, naming the file, the section and the key; [rdstracker-config](rdstracker-config.md) has the rules);
  - an `ll-lib-file` other than the built-in tracker (RESOURCE, SETTINGS);
  - an `operate-on-class-ids` token that isn't an integer (RESOURCE, SETTINGS).
- While playing:
  - a buffer without batch meta (STREAM, FORMAT);
  - two frames of one source in a batch (STREAM, FORMAT).

## Differences from DeepStream 8.0

- Only the IOU and NvSORT trackers; NvDCF and NvDeepSORT configs run as NvSORT, without their visual tracker and
  Re-ID, with a warning naming the ignored sections.
- The shared ID counter is never restarted, so live IDs never repeat (8.0 restarts it, giving duplicate live IDs).
- Every "ours" row of the tracking rules, among them: the labels of created objects, a NULL `display_text`, matching
  when all minimums are 0, and the handling of invalid boxes.

## Known issues and limits

- Not implemented: the NvDCF and NvDeepSORT trackers, Re-ID, re-association, pose estimation, 3D projection; the
  tracker misc meta (past-frame data, shadow and terminated track lists, visibility; 8.0 emits past-frame data even
  without the keys that ask for it); custom low-level libraries (the NvMOT API); sub-batches; changing the config
  at run time.
- Two frames of one source in a batch are an element error; `rdsstreammux` never makes such batches.
