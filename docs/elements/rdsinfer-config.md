# rdsinfer config file

The config file `rdsinfer` reads (`config-file-path`), with every key: its type, default, range, status and meaning.
It has nvinfer's groups and keys, so DeepStream's nvinfer config files load as they are, where the features map. The
element is on its own page, [rdsinfer](rdsinfer.md).

## Example

A primary detector on the committed test model `tests/models/fixeddet.onnx` (a 2-class detector with a constant
output, 64×64 input), as a key file saved as `fixeddet.txt` in the repository root:

```ini
[property]
onnx-file=tests/models/fixeddet.onnx
network-type=0
num-detected-classes=2
parse-bbox-func-name=RdsInferParseYolo
gie-unique-id=1

[class-attrs-all]
pre-cluster-threshold=0.5
```

The same in YAML, as `fixeddet.yml` (a file ending in `.yml` or `.yaml` is read as YAML):

```yaml
property:
  onnx-file: tests/models/fixeddet.onnx
  network-type: 0
  num-detected-classes: 2
  parse-bbox-func-name: RdsInferParseYolo
  gie-unique-id: 1
class-attrs-all:
  pre-cluster-threshold: 0.5
```

Paths in the file are relative to the file's own directory. [rdsinfer](rdsinfer.md) runs it in a pipeline. The
repository's real configs are the samples':
`samples/rds-test3/rds_test3_pgie_config.yml` (YOLO11n, which `tests/models/fetch-yolo11n.sh` fetches).

## Format and loading

The config file has nvinfer's groups: `[property]` (required), `[class-attrs-all]` and `[class-attrs-<id>]`.
- A file ending in `.yml` or `.yaml` is YAML, anything else a GLib key file. In YAML the groups are top-level
  mappings (`property:`, `class-attrs-all:`, ...), values are scalars, and a sequence (`offsets: [77.5, 21.2, 11.8]`)
  is the same as the key file's `;`-separated list (ours: the docs show the layout only by example).
- `[class-attrs-<id>]` overrides `[class-attrs-all]` key by key; the other keys of that class keep the
  `[class-attrs-all]` values (ours). An id at or above `num-detected-classes` is an error.
- Keys are in the tables below with the parser's own types, ranges and defaults; the ranges are written as in the
  parser's key table (`1..1024`, `>= 0`), not as the element pages' `A–B`; the defaults are ours (NVIDIA
  documents none). **supported**: used; **accepted no-op**: one warning per key, then ignored (values outside the
  range or the accepted values are still errors); **unsupported**: an error whenever the key is set (to a non-empty
  value). A key that is in no table is a warning, not an error: the docs list legacy keys, so a typo can't be told
  from one of those. The warnings are WARNING messages on the bus (RESOURCE, SETTINGS), posted at start.
- Errors name the file, the key (`GROUP:KEY` outside `[property]`) and the value: `FILE:KEY: VALUE: reason`.
- `onnx-file` is required, also with `model-engine-file` (a compiled program is checked against its model). For a
  detector (`network-type=0`), so are `num-detected-classes` and `parse-bbox-func-name`: nvinfer's internal resnet
  parser, which it uses when `parse-bbox-func-name` is unset, is not available, so name `RdsInferParseYolo` (built
  in) or a function in `custom-lib-path`.
- Label file (`labelfile-path`): one label per line, or all of them on one `;`-separated line; whitespace, CRs and
  trailing blank lines are ignored. Fewer labels than classes is a warning (the rest get empty labels), and so are
  more (the extra ones are ignored).
- A classifier's label file (`network-type=1`) has another form (DeepStream 8.0 behaviour): line i has the labels of
  output layer i, `;`-separated and not trimmed; one trailing `;` on a line is ignored (`a;b;` is `a`, `b`), an empty
  label elsewhere is kept (`a;;b`), and an empty line is a layer without labels. Only the CR of a CRLF line end and
  trailing blank lines are left out.
  Without `labelfile-path`, a warning: the classifier output gets empty labels (ours). A file with a line count other
  than the network's output layers, or a line whose label count isn't its layer's class count, is one warning at
  start, and its labels are used as they are (ours).
- Keys the mode or the network type doesn't use are a warning when set to something other than their default: the
  secondary-mode keys (`operate-on-*`, `input-object-*`, `secondary-reinfer-interval`, `classifier-async-mode`) in
  primary mode, the classifier keys outside classifiers, and `interval` in secondary mode (DeepStream 8.0 behaviour:
  it is ignored there). `roi-top-offset` and `roi-bottom-offset` apply in both modes (in secondary mode from the
  crop's edges, see [Secondary mode](rdsinfer.md#secondary-mode)) and never warn.

Config file paths: `onnx-file`, `model-engine-file`, `labelfile-path` and `custom-lib-path` from the file are relative
to the config file's directory (documented for YAML, ours for key files). That includes a bare `custom-lib-path`
name (no `/`): it is the file next to the config, not a library searched on the loader path. Use an absolute path
(as NVIDIA's docs ask) for a library elsewhere.

The file is read, and every key checked, when the element starts (READY to PAUSED); an error there fails the state
change. A changed file is read again only at the next start: there is no reload while playing.

## Config keys: `[property]`

### Model and files

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `onnx-file` | path | none |  | supported | the model; see [Models and compiled programs](rdsinfer.md#models-and-compiled-programs) |
| `model-engine-file` | path | none |  | supported | a compiled MIGraphX program (`.mxr`), see [Models and compiled programs](rdsinfer.md#models-and-compiled-programs) |
| `labelfile-path` | path | none |  | supported | see [Format and loading](#format-and-loading) |
| `infer-dims` | dims | none | 1..2147483647 | supported | `C;H;W` of one input image (also for `network-input-order=1`); required when the model's height or width is symbolic |
| `output-blob-names` | string list | none |  | accepted no-op | all outputs are passed to the parser and to the tensor meta |

### Network

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `batch-size` | uint | `1` (ours) | 1..1024 | supported | the program is compiled for this batch size (with `dynamic-batch`, the largest program of the set); the model's batch dimension must be symbolic or equal to it |
| `dynamic-batch` | bool | `0` (ours) |  | supported | ours (nvinfer has no such key): as the property |
| `network-mode` | int | `0` (ours) | 0..3 | supported | accepted values: 0, 2 (FP32, FP16; inputs and outputs stay FP32). INT8 (1) and BEST (3) fail at start |
| `network-type` | int | `0` (ours) | 0..100 | supported | accepted values: 0, 1, 100 (detector; classifier, in secondary mode only; no post-processing, tensor meta only). A classifier in primary mode and segmentation (2, 3) fail at start |
| `process-mode` | int | `1` (ours) | 1..2 | supported | 1 primary, 2 secondary (see [Secondary mode](rdsinfer.md#secondary-mode); classifiers run only there) |
| `gie-unique-id` | uint | `1` (ours) | >= 1 | supported | the objects' `unique_component_id` and the tensor meta's `unique_id` |
| `interval` | uint | `0` (ours) | 0..32 | supported | as the property |
| `output-tensor-meta` | bool | `0` (ours) |  | supported | as the property |

### Preprocessing

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `net-scale-factor` | float | `1.0` (ours) | > 0 | supported | input = `net-scale-factor` × (pixel − `offsets[c]`), pixels 0..255 (documented) |
| `offsets` | float list | none | >= 0 | supported | one per input channel (a count that doesn't match is an error); unset: 0 |
| `model-color-format` | int | `0` (ours) | 0..2 | supported | 0 RGB, 1 BGR, 2 GRAY (ours: the Y plane of NV12 frames, BT.601 luma of RGB frames) |
| `network-input-order` | int | `0` (ours) | 0..1 | supported | 0 NCHW, 1 NHWC |
| `maintain-aspect-ratio` | bool | `0` (ours) |  | supported | scale = min(W/w, H/h), the scaled size rounded to nearest (on crops in secondary mode: floored to even, see [Secondary mode](rdsinfer.md#secondary-mode)); padding is pixel value 0 before normalisation (ours) |
| `symmetric-padding` | bool | `0` (ours) |  | supported | with `maintain-aspect-ratio`: the scaled frame is centred (offset rounded down); without it, at the top-left |
| `scaling-filter` | int | `1` (ours) | 0..6 | supported | the `RdsBufSurfTransform_Inter` values: 1 (bilinear) and 2-4 sample bilinearly, the others nearest, as in `rds_buf_surf_transform` (nearest at pixel centres, see [Secondary mode](rdsinfer.md#secondary-mode)) |

### Detector

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `num-detected-classes` | uint | none | 1..65536 | supported | required for detectors |
| `cluster-mode` | int | `2` (ours) | 0..4 | supported | accepted values: 2, 4 (NMS, none). GroupRectangles (0), DBSCAN (1) and hybrid (3) fail at start |
| `parse-bbox-func-name` | string | none |  | supported | required for detectors: `RdsInferParseYolo` (built in) or a function in `custom-lib-path` |
| `custom-lib-path` | path | none |  | supported | a parser library, see [nvinfer custom parser ABI](../compat-matrix.md#nvinfer-custom-parser-abi-nvdsinferh-nvdsinfer_custom_implh); relative to the config file's directory, a bare name too |
| `filter-out-class-ids` | uint list | none | >= 0 | supported | objects of these classes are dropped |

### Secondary mode

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `operate-on-gie-id` | int | `-1` (ours) | >= -1 | supported | secondary mode: infer on the objects of this GIE; -1 or unset: every GIE, this element's own id included (DeepStream 8.0 behaviour); 0 is a real id |
| `operate-on-class-ids` | int list | none | >= -1 | supported | secondary mode: infer on objects of these classes; unset: every class; a `-1` in the list: every class (ours) |
| `input-object-min-width` | uint | none | >= 0 | supported | secondary mode: objects narrower are skipped; inclusive, 0: no limit (DeepStream 8.0 behaviour); above a set `input-object-max-width` is an error |
| `input-object-min-height` | uint | none | >= 0 | supported | secondary mode: as `input-object-min-width`, for the height |
| `input-object-max-width` | uint | none | >= 0 | supported | secondary mode: objects wider are skipped; inclusive, 0: no limit (DeepStream 8.0 behaviour) |
| `input-object-max-height` | uint | none | >= 0 | supported | secondary mode: as `input-object-max-width`, for the height |
| `secondary-reinfer-interval` | int | `-1` (ours) | >= -1 | supported | secondary mode: unset (-1) never reinfers an object, 0 every frame, N every N + 1 frames (DeepStream 8.0 behaviour) |

### Classifier

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `classifier-threshold` | float | `0` (ours) | >= 0 | supported | classifiers: a label needs a probability strictly above it; unset: 0 (DeepStream 8.0 behaviour) |
| `classifier-type` | string | none |  | supported | classifiers: the classifier meta's type name; letters, digits, `_` and `-` |
| `parse-classifier-func-name` | string | none |  | supported | classifiers: a parse function in `custom-lib-path` (required with it); unset: the built-in argmax parser (see [Classifier output (classifier meta)](rdsinfer.md#classifier-output-classifier-meta)) |
| `classifier-async-mode` | bool | none |  | supported | secondary classifiers: `1` runs synchronously all the same, with a warning (ours) |

### Not used here (accepted no-op or unsupported)

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `gpu-id` | uint | none | >= 0 | accepted no-op | the GPU is chosen with the element's `gpu-id` property |
| `workspace-size` | uint | none | >= 0 | accepted no-op | a TensorRT setting |
| `force-implicit-batch-dim` | bool | none |  | accepted no-op | a TensorRT setting |
| `enable-dla` | bool | none |  | accepted no-op | accepted values: 0; DLA is a Jetson unit |
| `use-dla-core` | int | none | >= 0 | accepted no-op | DLA is a Jetson unit |
| `tensor-meta-pool-size` | uint | none | >= 1 | accepted no-op | tensor output buffers are allocated as needed |
| `scaling-compute-hw` | int | none | 0..2 | accepted no-op | accepted values: 0, 1 (always the GPU); VIC (2) is a Jetson unit |
| `crop-objects-to-roi-boundary` | bool | none |  | accepted no-op | applies to preprocessed-tensor ROIs |
| `is-classifier` | bool | none |  | accepted no-op | accepted values: 0; legacy key, use `network-type=1` |
| `enable-dbscan` | bool | none |  | accepted no-op | accepted values: 0; deprecated, use `cluster-mode=2` |
| `int8-calib-file` | string | none |  | accepted no-op | INT8 only |
| `output-instance-mask` | bool | none |  | accepted no-op | accepted values: 0; instance segmentation |
| `input-tensor-from-meta` | bool | none |  | accepted no-op | accepted values: 0; preprocessed tensor input |
| `model-file` | string | none |  | unsupported | Caffe; use `onnx-file` |
| `proto-file` | string | none |  | unsupported | Caffe; use `onnx-file` |
| `mean-file` | string | none |  | unsupported | use `offsets` |
| `tlt-encoded-model` | string | none |  | unsupported | encoded TAO models; export to ONNX |
| `tlt-model-key` | string | none |  | unsupported | encoded TAO models; export to ONNX |
| `uff-input-dims` | string | none |  | unsupported | UFF; use `onnx-file` and `infer-dims` |
| `uff-input-blob-name` | string | none |  | unsupported | UFF; use `onnx-file` |
| `uff-input-order` | string | none |  | unsupported | UFF; use `network-input-order` |
| `engine-create-func-name` | string | none |  | unsupported | custom TensorRT engine builders |
| `custom-network-config` | string | none |  | unsupported | custom TensorRT engine builders |
| `layer-device-precision` | string | none |  | unsupported | a TensorRT setting; use `network-mode` |
| `output-io-formats` | string | none |  | unsupported | a TensorRT setting; inputs and outputs are FP32 |
| `parse-bbox-instance-mask-func-name` | string | none |  | unsupported | instance segmentation |
| `parse-segmentation-func-name` | string | none |  | unsupported | segmentation |
| `segmentation-threshold` | string | none |  | unsupported | segmentation |
| `segmentation-output-order` | string | none |  | unsupported | segmentation |

## Config keys: `[class-attrs-all]`, `[class-attrs-<id>]`

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `threshold` | float | none | >= 0 | supported | legacy name of `pre-cluster-threshold`, which wins if both are set |
| `pre-cluster-threshold` | float | `0.2` (ours) | >= 0 | supported | objects with a lower confidence are dropped before clustering; the default is DeepStream 8.0's |
| `post-cluster-threshold` | float | `0.0` (ours) | >= 0 | supported | objects with a lower confidence are dropped after clustering |
| `nms-iou-threshold` | float | `0.45` (ours) | >= 0 | supported | `cluster-mode=2`: a box is suppressed when its IoU with a kept box of its class is above it |
| `topk` | int | `-1` (ours) | >= -1 | supported | `cluster-mode=2`: the best K per class and frame after NMS; -1 keeps all, 0 none |
| `roi-top-offset` | uint | `0` (ours) | 0..2147483647 | supported | rows ignored at the top of the frame: objects whose top is above it are dropped, a top exactly at it is kept (DeepStream 8.0 behaviour); 0: no test. In secondary mode from the top of the crop (see [Secondary mode](rdsinfer.md#secondary-mode)) |
| `roi-bottom-offset` | uint | `0` (ours) | 0..2147483647 | supported | the same from the bottom: objects whose bottom (top + height) is below the frame's height minus it are dropped, a bottom exactly there is kept (DeepStream 8.0 behaviour); 0: no test. The frame's height or more drops every object of the class (ours: 8.0 then skips the test). In secondary mode from the bottom of the crop (see [Secondary mode](rdsinfer.md#secondary-mode)) |
| `detected-min-w` | uint | `0` (ours) | 0..2147483647 | supported | narrower objects are dropped |
| `detected-min-h` | uint | `0` (ours) | 0..2147483647 | supported | lower objects are dropped |
| `detected-max-w` | uint | `0` (ours) | 0..2147483647 | supported | wider objects are dropped; 0: no maximum |
| `detected-max-h` | uint | `0` (ours) | 0..2147483647 | supported | taller objects are dropped; 0: no maximum |
| `eps` | float | none | >= 0 | accepted no-op | DBSCAN only |
| `group-threshold` | int | none | >= 0 | accepted no-op | GroupRectangles only |
| `minBoxes` | int | none | >= 0 | accepted no-op | DBSCAN only |
| `dbscan-min-score` | float | none | >= 0 | accepted no-op | DBSCAN only |
