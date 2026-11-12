# rdsinfer

How to read this page, and how to run the examples: [element reference](README.md).

Runs an ONNX model with MIGraphX on the batches from `rdsstreammux`, and attaches what it finds as metadata: a primary
or secondary detector (object meta), a secondary classifier (classifier meta, with DeepStream's per-object cache), or
any network's raw output (tensor meta). It reads nvinfer config files, and supports custom bbox and classifier parsers
through nvinfer's parser ABI. DeepStream counterpart: `nvinfer`
([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvinfer.html)).
Built when: `-Dinfer=enabled`, or `auto` (the default) when MIGraphX, libyaml and json-glib are found.

## Example

These use the repository's committed test models (`tests/models/`, see its README): `fixeddet.onnx`, a 2-class
detector whose output is the same six boxes for every image, and `meancls.onnx`, a classifier whose classes are the
mean of each colour channel.

```sh
cat > /tmp/det.txt <<EOF
[property]
onnx-file=$PWD/tests/models/fixeddet.onnx
network-type=0
num-detected-classes=2
parse-bbox-func-name=RdsInferParseYolo
gie-unique-id=1
EOF
# a classifier's label file; labelfile-path is relative to the config, so it sits next to cls.txt
printf 'red;green;blue\n' > /tmp/colours.txt
cat > /tmp/cls.txt <<EOF
[property]
onnx-file=$PWD/tests/models/meancls.onnx
network-type=1
process-mode=2
labelfile-path=colours.txt
net-scale-factor=0.00392156862745098
gie-unique-id=2
operate-on-gie-id=1
EOF

# a primary detector: the debug log prints "frame N: 6 objects"
GST_DEBUG=rdsinfer:5 gst-launch-1.0 videotestsrc num-buffers=10 ! video/x-raw,format=NV12,width=640,height=640 \
  ! m.sink_0 rdsstreammux name=m batch-size=1 width=640 height=640 \
  ! rdsinfer config-file-path=/tmp/det.txt ! fakesink 2>&1 | grep objects

# the detector, then a classifier on its objects, drawn by rdsosd
gst-launch-1.0 videotestsrc num-buffers=10 pattern=red ! video/x-raw,format=NV12,width=640,height=640 ! m.sink_0 \
  rdsstreammux name=m batch-size=1 width=640 height=640 \
  ! rdsinfer config-file-path=/tmp/det.txt ! rdsinfer config-file-path=/tmp/cls.txt batch-size=8 \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd ! fakesink
```

The first start compiles each model for its batch size, input size and GPU (seconds for these tiny models, minutes
for a real one) and caches the program next to the ONNX file: `fixeddet_b1_3x64x64_<gfx>_fp32.mxr` and
`meancls_b8_3x32x32_<gfx>_fp32.mxr` in `tests/models/` (git ignores them). Later starts load them. See [Models and
compiled programs](#models-and-compiled-programs).

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)`, format NV12, RGBA, BGRA, RGBx or BGRx, 1–16384 × 1–16384: batches with an `RdsBatchMeta` (from `rdsstreammux`) |
| `src` | src | always | as the sink |

Buffers pass through unchanged: only metadata is added, pixels are never written. A buffer without batch meta is an
element error. The batch must be in GPU memory on `gpu-id`.

## Properties

Names and types are those on NVIDIA's nvinfer page (checked 2026-10-24). As documented, a property set explicitly
overrides the config file; here that holds also when it is set to its default value. An unset property leaves the
config file's value (or the key's default). The Default column is the GParamSpec default, equal to the key's default
(**ours**, NVIDIA documents none), and takes effect only when the property is set explicitly.

Every property is read when the element starts (READY to PAUSED): the config is read and the program built or
loaded then, and an error fails the state change with a message naming the file (or `element property`), the key and
the value. GStreamer marks them all "changeable only in NULL or READY" (`GST_PARAM_MUTABLE_READY`), but the element
doesn't refuse a later change: it is stored, without a warning, and used at the next start. Changing
`model-engine-file` while PLAYING (nvinfer's on-the-fly model update) is not supported.

### Config and model

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `config-file-path` | string | `NULL` | a key file, or YAML when it ends in `.yml` or `.yaml` | any state (applied at the next start) | required; see [rdsinfer-config](rdsinfer-config.md) |
| `model-engine-file` | string | `NULL` | a path | any state (applied at the next start) | a compiled MIGraphX program (`.mxr`), loaded when its manifest matches the model and the device, else rebuilt into the cache name (see [Models and compiled programs](#models-and-compiled-programs)); used as given (a config-file value is relative to the config file). A TensorRT `.engine` is ignored with a debug-log warning |
| `unique-id` | uint | `1` | 0–4294967295 | any state (applied at the next start) | overrides `gie-unique-id`: the `unique_component_id` of the metas this element attaches (the config key starts at 1) |
| `gpu-id` | uint | `0` | 0–4294967295: a visible ROCm device | any state (applied at the next start) | the GPU for preprocessing and inference |

### Mode and selection

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `process-mode` | uint | `1` | 1–2 | any state (applied at the next start) | overrides `process-mode`: `1` primary (on whole frames), `2` secondary (on objects, see [Secondary mode](#secondary-mode); classifiers run only there) |
| `infer-on-gie-id` | int | `-1` | −1–2147483647 | any state (applied at the next start) | secondary mode: infer on objects of this `unique_component_id`; `-1` every GIE (DeepStream 8.0's type and default). Overrides `operate-on-gie-id`; a warning when set in primary mode |
| `infer-on-class-ids` | string | `NULL` | class ids separated by `:` (`1:2:4`) | any state (applied at the next start) | secondary mode: infer on objects of these classes; `-1` in the list is every class (DeepStream 8.0 behaviour). Overrides `operate-on-class-ids`; a warning when set in primary mode |
| `filter-out-class-ids` | string | `NULL` | class ids separated by `;` (`1;2`) | any state (applied at the next start) | drop the detector's objects of these classes; replaces the file's list (an empty string clears it) |
| `interval` | int | `0` | 0–32 | any state (applied at the next start) | primary mode: infer one batch, skip the next `interval` (they pass with `bInferDone` false and no objects); the count restarts at start and after a flush (ours). Ignored in secondary mode, with a warning (DeepStream 8.0 behaviour) |

### Batching

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `batch-size` | uint | `1` | 1–1024 | any state (applied at the next start) | primary mode: frames of a batch go to the network image of their slot (`batch_id`); a frame in a slot at or above `batch-size` is an error. Secondary mode: objects inferred together (see [Secondary mode](#secondary-mode)) |
| `dynamic-batch` | boolean | `false` | | any state (applied at the next start) | ours (nvinfer has no such property); see [Dynamic batching](#dynamic-batching). Overrides the config key |

### Output

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `output-tensor-meta` | boolean | `false` | | any state (applied at the next start) | attach the network's raw output as an `RdsInferTensorMeta` per inferred frame, or per object in secondary mode (always with `network-type=100`); see [Tensor output meta](#tensor-output-meta) |
| `qos` | boolean | `false` | | any state | GstBaseTransform's |

### Not implemented

The element's warnings are WARNING messages on the bus (RESOURCE, SETTINGS), posted at start. `raw-output-file-write`
and `raw-output-generated-callback` set to anything but their default post one (once per element);
`crop-objects-to-roi-boundary` and `clip-object-outside-roi` set to `true` post one at every start;
`raw-output-generated-userdata` posts none.
`output-instance-mask` and `input-tensor-meta` set to `true` fail the start. nvinfer's `classifier-type`,
`strongly-typed` and `dynamic-properties` are not there.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `raw-output-file-write` | boolean | `false` | | any state (applied at the next start) | accepted no-op |
| `raw-output-generated-callback` | `gpointer` | — | | any state (applied at the next start) | accepted no-op: the callback's signature isn't documented |
| `raw-output-generated-userdata` | `gpointer` | — | | any state (applied at the next start) | accepted no-op |
| `output-instance-mask` | boolean | `false` | | any state (applied at the next start) | unsupported: `true` fails at start (instance segmentation) |
| `input-tensor-meta` | boolean | `false` | | any state (applied at the next start) | unsupported: `true` fails at start (preprocessed tensor input) |
| `crop-objects-to-roi-boundary` | boolean | `false` | | any state (applied at the next start) | accepted no-op: applies to preprocessed-tensor ROIs |
| `clip-object-outside-roi` | boolean | `false` | | any state (applied at the next start) | accepted no-op: applies to preprocessed-tensor ROIs |

## Metadata

What the element attaches depends on `network-type`: a detector (`0`) adds object metas, a classifier (`1`)
classifier metas, and any network can add tensor metas. Each table row is either **matches nvinfer (DeepStream
8.0)**: observed DeepStream 8.0 behaviour where the docs are silent, or **ours**.

### Detector output (object meta)

- Boxes are in the batch frame's pixels (the mux's `width`×`height`); `maintain-aspect-ratio` / `symmetric-padding`
  scaling is undone (ours: the scaled frame sits at the top-left, or centred with `symmetric-padding`).
  `detector_bbox_info.org_bbox_coords` is the unclipped box and `rect_params` the box clipped to the frame (documented).
- Fields set: `unique_component_id` (`gie-unique-id`), `class_id`, `confidence`, `object_id` = `UNTRACKED_OBJECT_ID`,
  `obj_label` (the label, truncated to 127 bytes), `parent` NULL. Drawing defaults (ours): `rect_params` border 3 px
  red, no background; `text_params` the label at the box's top-left, 20 px above it (clamped to 0), "Serif" 10, white on
  black.
- Frames that were inferred get `bInferDone` TRUE (primary mode). The post-processing order and NMS are under [Detection
  post-processing](#detection-post-processing). A secondary detector's objects are under [Secondary
  mode](#secondary-mode).
- A parse function that returns `false` or throws: that frame gets no objects, and the element posts one warning (then
  logs at debug level); the stream goes on. Invalid objects from a parser (class id out of range, non-finite values,
  empty boxes) are dropped, and a non-zero `rotation_angle` is ignored: one warning each.

### Classifier output (classifier meta)

A classifier (`network-type=1`, `process-mode=2`) attaches what it finds to each object it infers on. Each row is either
**matches nvinfer (DeepStream 8.0)**: observed DeepStream 8.0 behaviour where the docs are silent, or **ours**.

| Behaviour | Status | Notes |
|---|---|---|
| Built-in parser | matches nvinfer (DeepStream 8.0) | without `parse-classifier-func-name`: per output layer the argmax of its values, ties to the lowest class index; no softmax (the value is the probability as it is: 200/256 gives 0.78125, an already softmaxed model's values pass through) |
| `classifier-threshold` | matches nvinfer (DeepStream 8.0) | strictly above: 0.5 rejects 0.5 and keeps 0.50390625; the default 0 gives nothing for an all-zero output. A layer below it is left out; with none left, the object gets no classifier meta at all (not an empty one) |
| Classifier meta | matches nvinfer (DeepStream 8.0) | one `RdsClassifierMeta` per object and classifier: `num_labels` the labels attached, `unique_component_id` the `gie-unique-id`, `classifier_type` the `classifier-type` (NULL if unset; interned, not owned) |
| Meta order | ours | appended to the object's `classifier_meta_list`, after the metas of earlier classifiers (8.0 prepends) |
| Labels | matches nvinfer (DeepStream 8.0) | one `RdsLabelInfo` per output layer kept: `result_label` the label, `result_class_id` the argmax, `label_id` the output layer, `result_prob` the raw value |
| Label order | ours | in output-layer order (8.0 prepends) |
| `num_classes` | ours | the layer's class count (8.0 sets 0); 0 for a custom parser's attribute whose `attributeIndex` is not an output layer |
| Long labels | ours | a label longer than `result_label` (127 bytes) is truncated there and kept whole in `pResult_label`; otherwise `pResult_label` is NULL (8.0 never sets it) |
| Label file gaps | ours | an argmax past the layer's labels, a layer without a line and no label file all attach the meta with an empty label (8.0 attaches nothing for the first and the last, an empty label for the second); start warns when there is no label file, or one whose line or label counts don't fit the output layers |
| Display text | matches nvinfer (DeepStream 8.0) | `text_params.display_text` gets `" "` and then `"<label> "` per label attached (`"T"` becomes `"T red "`, NULL becomes `" blue "`, a second classifier makes it `"T red  x "`); with a custom parser `" "` and its description (`"T desc"`); unchanged when nothing is attached. `obj_label` is never changed |
| Empty description | ours | a custom parser that returns attributes but leaves `descString` empty (length 0) attaches the classifier meta with its labels and leaves the display text unchanged (`"T"` stays `"T"`, NULL stays NULL; 8.0 attaches nothing then, not even the labels, and caches that for a tracked object). A description of one space is not empty: `"T"` becomes `"T  "` (DeepStream 8.0 behaviour) |
| Description without attributes | ours | a custom parser that sets `descString` but returns no attributes attaches nothing: no classifier meta, and the display text is unchanged (8.0 not observed) |
| Custom parser | matches nvinfer (DeepStream 8.0) | `parse-classifier-func-name` in `custom-lib-path`, called once per object with that object's output layers (dims without the batch), the network info and `classifier-threshold`; each attribute becomes a label (`attributeLabel` → `result_label`, `attributeValue` → `result_class_id`, `attributeIndex` → `label_id`, `attributeConfidence` → `result_prob`), not thresholded again. rdsinfer frees the `attributeLabel`s |
| Custom parser label | ours | a NULL `attributeLabel` is an empty label |
| Unknown parse function | matches nvinfer (DeepStream 8.0); ours: the message | a `parse-classifier-func-name` the library doesn't have fails start (DeepStream 8.0 behaviour); the error names the key (ours) |
| Parser returns false or throws | ours | one warning per element; that object gets no new classifier meta (a tracked object keeps its stored result, if any, see [Per-object cache](#per-object-cache)), the others are unaffected (8.0 crashes) |
| Tensor meta | matches nvinfer (DeepStream 8.0) | with `output-tensor-meta=1` a classified object gets its tensor meta too; it doesn't change the classification |
| Chained classifiers | matches nvinfer (DeepStream 8.0) | a second classifier on the same objects attaches its own meta to each |

### Tensor output meta

One `RdsUserMeta` per inferred frame on `frame_user_meta_list` (in secondary mode per inferred object, on its
`obj_user_meta_list`), `meta_type` `RDS_INFER_TENSOR_OUTPUT_META` (`NVDSINFER_TENSOR_OUTPUT_META`), data
`RdsInferTensorMeta`. `out_buf_ptrs_host[i]` (pinned host memory) and `out_buf_ptrs_dev[i]` point at the frame's (or
object's) slice of output layer `i`, whose dims in `output_layers_info` are those of one image, without the batch;
the `buffer` pointers in `output_layers_info` are NULL (documented: not valid there). The buffers stay valid as long
as the meta exists, in copies too (`rdsstreamdemux` outputs, `rds_batch_meta_copy`), also after the element has
stopped. They are refcounted and reused once the last meta pointing at them is gone, so `tensor-meta-pool-size` has
no effect. The element keeps as many as the most it had in use lately (during the last 256 to 512 batches, or chunks
of `batch-size` objects in secondary mode), and at least 4 (`rdsstreammux`'s default `buffer-pool-size`); the rest
are freed (ours).

## Config file

nvinfer's key files and YAML: groups `[property]`, `[class-attrs-all]` and `[class-attrs-<id>]`. The format, how
paths and labels are read, and every key with its type, default, range and status are on
[rdsinfer-config](rdsinfer-config.md). The file is read at start; a changed file is read again at the next start.

Custom parsers (`custom-lib-path`, `parse-bbox-func-name`, `parse-classifier-func-name`) follow nvinfer's parser ABI,
described in the compatibility matrix's
["nvinfer custom parser ABI"](../compat-matrix.md#nvinfer-custom-parser-abi-nvdsinferh-nvdsinfer_custom_implh).

## Behaviour

### Processing

One batch at a time, synchronously (ours; nvinfer's threading isn't documented): preprocessing and inference on the
element's own non-blocking stream, one synchronization per batch (per chunk of `batch-size` objects in secondary
mode), parsing on the CPU. Slots without a frame (a partial batch) and unused images of a chunk are zeroed, not
inferred on stale pixels (with `dynamic-batch`, those of the program that runs). The batch's metadata lock is held
only to read the frame list and to add the results, never during GPU work; results for a frame or object removed in
between are dropped, also when the metadata pool gave its address to a new one.

### Detection post-processing

The docs list the steps but not their order. rdsinfer runs them per frame, in this order (ours), each with the
object's class attributes:

1. parser output: objects with a class id at or above `num-detected-classes`, non-finite values or an empty box are
   dropped (one warning per element), and so are those below `pre-cluster-threshold`;
2. `cluster-mode=2`: greedy NMS per class, highest confidence first (ties: the parser's order); an object is
   suppressed when its IoU with a kept one is strictly above `nms-iou-threshold`;
3. `cluster-mode=2`: `topk`;
4. `post-cluster-threshold`;
5. mapping from network-input pixels to frame pixels (undoing `maintain-aspect-ratio` / `symmetric-padding`);
6. `filter-out-class-ids`;
7. `roi-top-offset` / `roi-bottom-offset`, each when set: top ≥ `roi-top-offset` and top + height ≤ frame height −
   `roi-bottom-offset`, boundaries included (DeepStream 8.0 behaviour); a `roi-bottom-offset` of the frame height or
   more drops everything (ours);
8. `detected-min-*` / `detected-max-*`, on the unclipped size in frame pixels;
9. clipping to the frame gives `rect_params`; an object whose clipped box is empty is dropped.

The built-in `RdsInferParseYolo` (ours) reads an Ultralytics YOLOv8/YOLO11 detection output: one FP32 layer of
`[4 + classes, anchors]` per image, or transposed `[anchors, 4 + classes]` (told apart by which side is
`4 + num-detected-classes`). Rows 0-3 are centre x, centre y, width and height in network-input pixels; each anchor
gives at most one object, its best class (no objectness score). Another output layout fails at start.

### Secondary mode

`process-mode=2` infers on the objects that upstream elements added (no primary `rdsinfer` is needed). Each row is
either **matches nvinfer (DeepStream 8.0)**: observed DeepStream 8.0 behaviour where the docs are silent, or **ours**
(where 8.0 settles nothing, crashes or is inconsistent).

Keys and properties: `process-mode` is a property and a config key; `infer-on-gie-id` and `infer-on-class-ids` are
properties that override the config keys `operate-on-gie-id` and `operate-on-class-ids`;
`input-object-min/max-width/height`, `secondary-reinfer-interval` and the classifier keys (`classifier-threshold`,
`classifier-type`, `parse-classifier-func-name`, `classifier-async-mode`) are config keys only, as in DeepStream 8.0,
which has no properties for them. Their rows are in [rdsinfer-config](rdsinfer-config.md).

| Behaviour | Status | Notes |
|---|---|---|
| Which frames | matches nvinfer (DeepStream 8.0) | every frame of every batch, with `bInferDone` TRUE or FALSE; `interval` is ignored (a warning when set). `bInferDone` is never set or cleared |
| Which objects | matches nvinfer (DeepStream 8.0) | `operate-on-gie-id` (-1: every GIE, this element's own id included; 0 is a real id), `operate-on-class-ids` (empty: every class) and `input-object-min/max-width/height` (inclusive, on the float `rect_params` size, 0: no limit); objects added by this element in the same buffer are not inferred on |
| Class id -1, negative class ids | matches nvinfer (DeepStream 8.0); ours: the config file's `-1` and negative ids with a class list | `-1` in `operate-on-class-ids` in the config file is every class, as the property's `-1` is in 8.0 (8.0 aborts on the config value); with a class list set, an object with a negative `class_id` is not selected (8.0 aborts); with no list it is (DeepStream 8.0 behaviour) |
| Crop | matches nvinfer (DeepStream 8.0) | the object's `rect_params` (`detector_bbox_info` and `tracker_bbox_info` are not used): origin floored, then rounded up to even (11 → 12, 43.7 → 44), size floored, then down to even, at least 2 (15.6 → 14, 1 → 2); a side under 1 pixel is skipped; overflow past the right or bottom edge is clipped and the rest stretched to the network |
| Crop outside the frame | ours | a negative origin, a box larger than the frame or wholly outside it is clipped too (8.0 fails the pipeline); a crop with less than 2 pixels left is skipped (logged at debug level). The origin is aligned before the clip |
| Crop in the network input | matches nvinfer (DeepStream 8.0) | stretched to the network, or with `maintain-aspect-ratio` scaled by min(W/w, H/h) with the scaled size floored to even (24×16 into 16×16 is 16×10), at the top-left, or with `symmetric-padding` at floor((net − exact scaled size) / 2); padding is pixel value 0 before normalisation. The crop is resampled to 8 bits, then normalised as in primary mode |
| Scaling filter | ours | `scaling-filter` defaults to 1 (bilinear) in both modes (8.0's default is nearest). Nearest samples at pixel centres, as `rds_buf_surf_transform` does: a 32-pixel crop into 16 reads columns 1, 3, 5, … (8.0 reads 0, 2, 4, …), so nearest-filter inputs can differ from 8.0's by one source pixel. `scaling-filter=3` works (bilinear; 8.0 fails the pipeline) |
| Batching | matches nvinfer (DeepStream 8.0) | objects in frame list order, then object list order, in chunks of `batch-size` that continue across the frames of a batch but never across batches: 10 objects at `batch-size` 4 run as 4, 4, 2; two batches of 5 as 4, 1 and 4, 1 |
| Chunk size | ours | the compiled program has a fixed batch of `batch-size` images: a chunk with fewer objects runs them all the same, its unused images zeroed, so a chunk costs nearly as much with few objects as when full (8.0 rebuilds the model for 1 to `batch-size` images and runs the actual count). With `dynamic-batch`, a chunk of n objects runs on the smallest program of at least n images, so smaller chunks cost less: 5 objects at `batch-size` 4 run as 4, then 1 on the batch-1 program. The results per object are the same, up to float rounding between programs |
| Tensor meta | matches nvinfer (DeepStream 8.0) | one per inferred object (a detector's parents and classified objects included), on the object's `obj_user_meta_list`; none on the frame or the batch (see [Per-object cache](#per-object-cache)). `unique_id` is the `gie-unique-id`, the layer dims are those of one object (no batch), the host and device pointers point at the object's slice, `network_info` is the network's size and `maintain_aspect_ratio` / `symmetric_padding` the config's values |
| Detector objects (children) | matches nvinfer (DeepStream 8.0) | per crop, with the class attributes' thresholds, NMS per class (never across crops), `detected-min/max-*` in frame pixels and `filter-out-class-ids`; `parent` is the cropped object, `unique_component_id` this element's `gie-unique-id`, `class_id`, `confidence`, `obj_label` and the text from the parser and label file, `object_id` untracked |
| Detector thresholds | matches nvinfer (DeepStream 8.0) | the class attributes' defaults are those of the key table in both modes: `pre-cluster-threshold` 0.2, as in 8.0 |
| Child geometry | ours | network → crop → frame pixels with one consistent mapping; `rect_params` clipped to the frame, `detector_bbox_info` the unclipped box (8.0 maps with the unaligned parent origin, so it can be a pixel off, doesn't clip, and moves negative coordinates to the parent's origin without shrinking the box). A child wholly outside its crop is dropped; one partly outside keeps its part inside the frame |
| Child order | ours | appended to the frame's object list after the existing objects, crop by crop (8.0 prepends) |
| Detector chains | matches nvinfer (DeepStream 8.0) | a second `rdsinfer` with `infer-on-gie-id` = the detector's `gie-unique-id` infers on its children |
| `roi-top-offset`, `roi-bottom-offset` | matches nvinfer (DeepStream 8.0); ours: the bottom offset | applied to the children with the child's class attributes, in crop pixels (frame pixels from the top of the aligned crop the parent was sampled from): a child is kept when its top − the crop's top ≥ `roi-top-offset` (DeepStream 8.0 behaviour) and its bottom − the crop's top ≤ the crop's height − `roi-bottom-offset` (ours: 8.0 compares with the frame's height minus the offset); a `roi-bottom-offset` of the crop's height or more drops every child of that crop (ours). Boundaries included, each offset only when set. The parents are never filtered (DeepStream 8.0 behaviour); no warning |
| Parser returns false | ours | one warning per element, that object gets no children but its tensor meta is still attached (as in primary mode), the other objects are unaffected (8.0 crashes) |
| Classifiers (`network-type=1`) | ours | secondary mode only for now: a classifier in primary mode fails at start; see [Classifier output (classifier meta)](#classifier-output-classifier-meta) and [Per-object cache](#per-object-cache) |

### Per-object cache

In secondary mode a classifier keeps each tracked object's last result, keyed by `source_id` and `object_id`, and infers
the object again only when it is due. Each row is either **matches nvinfer (DeepStream 8.0)**: observed DeepStream 8.0
behaviour, or **ours**.

| Behaviour | Status | Notes |
|---|---|---|
| What is cached | matches nvinfer (DeepStream 8.0) | classifiers (`network-type=1`) on tracked objects only. Untracked objects (`RDS_UNTRACKED_OBJECT_ID`) are inferred every frame; tensor-only networks (`network-type=100`) infer every object every time |
| Keys | matches nvinfer (DeepStream 8.0) | `(source_id, object_id)`: the same id in two streams has two entries |
| New id | matches nvinfer (DeepStream 8.0) | inferred on its first frame; an id seen before gets its stored result back (until it is evicted) |
| Growth | matches nvinfer (DeepStream 8.0) | inferred again when the `rect_params` area is above 1.2 × the area at its last inference, strictly (1.2 × no, 1.2001 × yes); shrinking or moving never re-infers and keeps the reference area. A class change keeps the result |
| Growth comparison | ours | exact (5 × area > 6 × last area in double precision); 8.0 compares in float, which may round the other way in rare cases |
| `secondary-reinfer-interval` | matches nvinfer (DeepStream 8.0) | unset (-1): never on time; 0: every frame; N: every N + 1 frames, counted from the object's own last inference (growth restarts the count) |
| Interval count | ours | counted in the frames in which the object was selected (8.0 runs had one source and the object in every frame) |
| Cached frames | matches nvinfer (DeepStream 8.0) | the stored result is attached again: identical classifier meta and label infos, the same `display_text` append; no tensor meta, even with `output-tensor-meta=1` |
| Empty result | matches nvinfer (DeepStream 8.0) | a result below `classifier-threshold` (no labels) is cached like any other: the object gets nothing until growth or the interval infers it again |
| Parser failure | ours | the object's entry is left as it was before the inference, and on that frame the object gets the stored result (classifier meta and display text) instead of nothing, plus its tensor meta from the failed inference; a new id has no stored result, gets no classifier meta and is tried again on its next frame; an object with a stored result keeps it and stays due until an inference succeeds |
| Same id twice in a batch | ours | inferred once; the other object gets the result stored before this batch, if any (none for a new id) |
| Eviction | ours | an entry not seen for 30 frames of its own source is dropped (8.0 keeps every entry: results came back after 1000 absent frames). Each source counts its own frames, one per frame of it in a batch, so a source that sends no frames keeps its entries; seen means present in the source's frame, selected or not (an object filtered out by class, size or an empty crop is kept) |
| Stream events | ours | a source's `rds-stream-eos` or `rds-stream-reset` drops that source's entries and restarts its frame count; FLUSH_STOP and stopping the element drop every entry. A result whose request is stale (`seq` and epoch per request) is neither stored nor attached: what an object gets is always what its entry holds |
| Secondary detectors | matches nvinfer (DeepStream 8.0) | no cache of detections; with `secondary-reinfer-interval` set, a tracked parent is inferred only when due by the same rules (new id, growth, interval) and gets no children (and no tensor meta) on the other frames. Unset: every frame |
| `classifier-async-mode=1` | ours | warns at start and runs synchronously: results are in the same buffer, also for untracked objects (8.0 attaches them from the next frame on and never to untracked objects) |

### Dynamic batching

`dynamic-batch=true` (ours) compiles a program for every power of two below `batch-size`, plus `batch-size` (4: 1, 2,
4; 6: 1, 2, 4, 6), and runs each chunk on the smallest that holds its highest image.
- Primary mode: the highest occupied slot (`batch_id`) decides, not the number of frames, so one frame in slot 3 runs
  on the batch-4 program.
- Secondary mode: a chunk's objects take images 0 to n−1, so the object count decides.
- `batch-size` keeps its meaning: the most images one run takes. With `batch-size` 1 there is no smaller program, so
  it changes nothing.
- A model with a fixed batch dimension falls back to the `batch-size` program alone, with a warning at start.
- YOLO11n with one frame in a `batch-size` 4 pipeline, on the gfx1036 iGPU: about 31 ms on the batch-1 program
  instead of 95 ms on the batch-4 one.
- The costs, compiling and caching are under [Models and compiled programs](#models-and-compiled-programs).

### Models and compiled programs

- Models are ONNX (`onnx-file`) with one float NCHW input (NHWC with `network-input-order=1`) whose batch dimension
  is symbolic or equal to `batch-size`. A symbolic height or width needs `infer-dims`. ONNX models that keep weights
  in external data files are not supported (an error at start): export them with the weights inside the `.onnx`.
- The first start compiles the model with MIGraphX for the batch size, input size, GPU and precision, which takes
  minutes on a small GPU (see `rds-compile` below to do it ahead of time). The compiled program is saved as
  `<onnx stem>_b<batch>_<C>x<H>x<W>_<gfx>_<fp32|fp16>.mxr` next to the ONNX file, or in
  `$XDG_CACHE_HOME/redstream/` when that directory isn't writable. `<gfx>` is the GPU's own architecture (from the
  kernel driver's topology, so `HSA_OVERRIDE_GFX_VERSION` doesn't change it), e.g. `gfx1036`.
- Each program has a manifest, `<program>.mxr.json`: the ONNX file's SHA-256, the MIGraphX and HIP runtime versions,
  the GPU architecture and `HSA_OVERRIDE_GFX_VERSION`, the precision, the input name and shape, the output shapes and
  the compile options. A program is loaded only when its manifest matches what would be compiled now; otherwise it
  is rebuilt, and the element's log says why. Both files are written through temporary files and renames.
- Lookup: `model-engine-file`, then the cache name next to the model, then in `$XDG_CACHE_HOME/redstream/`. A stale
  `model-engine-file` is never overwritten: the rebuilt program goes to the cache name, and later starts load that
  copy with a warning that `model-engine-file` doesn't match. A TensorRT `.engine` file is ignored with a warning.
- With `dynamic-batch` (ours), every program of the set is compiled or loaded at start: the `batch-size` one first,
  then the smaller ones in descending order. Each is an ordinary cache entry with its own `_b<N>_` name and
  manifest. All or nothing: a program that fails fails the start, and the error names its size (`batch 2 program:
  ...`). `model-engine-file` applies to the `batch-size` program only; the smaller ones always use their cache names.
  Each extra program costs a compile on a cold cache (YOLO11n on the gfx1036 iGPU: about 1.5 min per size) and its
  own GPU memory (about 80–115 MB each; MIGraphX 2.18 can't share weights between programs). The programs are
  different kernels, so their results agree with the `batch-size` program's only up to float rounding (YOLO11n: at
  most 1.13e-6 in confidence and 1.22e-4 px in the boxes). MIGraphX's own dynamic shapes are not used: in 2.18 they
  compile every size anyway, copy the weights for each, and don't parse YOLO11n.
- The element runs the program once on start (a warm-up), so the first batch isn't slower than the rest.
- Compiled programs and manifests are never deleted automatically: a rebuilt program replaces the file of the same
  name, and programs for other batch sizes, input sizes, GPUs or precisions stay until they are removed by hand. A
  rebuild first removes the old manifest, so one that fails part-way leaves the `.mxr` without a manifest, and it is
  rebuilt at the next start.

### Pre-building programs: `rds-compile` (`trtexec`)

The element compiles the model on its first start, which takes minutes on a small GPU. `rds-compile` does it ahead
of time with the element's own config parser and engine, so the program lands where the element looks for it:

```bash
rds-compile -c CONFIG [-b BATCH] [--fp16|--fp32] [-g GPU] [--dynamic-batch] [-o OUT.mxr] [--force]
```

- `-b`, `--fp16`/`--fp32`, `-g` and `--dynamic-batch` override `batch-size`, `network-mode`, `gpu-id` and
  `dynamic-batch` (sets it to 1); `--fp16` with `--fp32` is an error. Without `-o` the program is looked up and
  written like the element does (`model-engine-file`, then the cache name next to the ONNX file, then
  `$XDG_CACHE_HOME/redstream/`). `-o` looks up and writes only that file, with its manifest next to it as
  `OUT.mxr.json` (an unwritable directory is an error before building); point `model-engine-file` at it.
- With `dynamic-batch`, every program of the set is built (one per power of two below `batch-size`, plus
  `batch-size`); those already up to date are loaded, not rebuilt. The up-to-date check is the element's own open:
  it briefly loads every program onto the GPU (YOLO11n: about 80–115 MB each). `-o` places only the `batch-size`
  program; the smaller ones always use their cache names. A model with a fixed batch dimension builds the
  `batch-size` program alone, with the element's warning.
- Prints each program's path on stdout, one per line in ascending batch size (the `batch-size` one last); the build
  reasons, warnings and errors (with the element's messages) on stderr.
- Exit status: 0 built (at least one program), 1 error, 2 already up to date (the element would load every
  program; `--force` rebuilds them all).

### Errors

- **At start** (READY to PAUSED), each an element error naming the file or `element property`, the key and the value:
  a config file that can't be read (RESOURCE, NOT_FOUND), any other config or property problem (RESOURCE, SETTINGS;
  the rules are on [rdsinfer-config](rdsinfer-config.md)), a `gpu-id` with no such GPU (RESOURCE, NOT_FOUND), and a
  model that fails to compile or load, or a HIP stream that can't be made (RESOURCE, FAILED).
- **While playing**, element errors too:
  - a buffer without an `RdsBatchMeta` or a `memory:RDS` surface: "rdsinfer needs batches from rdsstreammux"
    (STREAM, FORMAT);
  - a batch not in GPU memory on `gpu-id`: "rdsinfer needs batches in GPU memory on its gpu-id (N)" (STREAM, FORMAT);
  - in primary mode a frame in a slot at or above `batch-size`: "Frame in batch slot N, but batch-size is M" (STREAM,
    FORMAT); in secondary mode a frame in a slot the batch doesn't have (STREAM, FORMAT);
  - a failed preprocessing, inference or other GPU step (RESOURCE, FAILED), and an output buffer that can't be
    allocated (RESOURCE, NO_SPACE_LEFT).
- A parser that returns false or throws is not an error: one warning per element, and the frame or object gets no
  results (see [Detector output](#detector-output-object-meta) and [Secondary mode](#secondary-mode)).

## Differences from DeepStream 8.0

- MIGraphX instead of TensorRT: ONNX models, compiled into cached `.mxr` programs with a manifest; TensorRT
  `.engine` files are ignored.
- A compiled program has a fixed batch: a partial chunk costs nearly as much as a full one, unless `dynamic-batch`
  (ours) is set. DeepStream 8.0 builds its engine for 1 to `batch-size` images and runs the actual count.
- Classifiers run in secondary mode only; primary classifiers, segmentation, instance masks and preprocessed tensor
  input are not implemented.
- nvinfer's internal resnet parser is not available: a detector names `RdsInferParseYolo` (built in) or a custom
  parser.
- Every "ours" row in the tables above, among them: undone letterbox geometry, clipping of crops outside the frame,
  child order, `classifier-async-mode` running synchronously, and the cache's eviction after 30 absent frames.

## Known issues and limits

- **Stale output from an engine's first run** (ROCm 10.1 on the Ryzen 7000 gfx1036 iGPU, run as gfx1030 through
  `HSA_OVERRIDE_GFX_VERSION=10.3.0`, a configuration AMD doesn't support). When the host is busy, a kernel's writes now
  and then don't reach the next kernel on the same stream, with no error from HIP. An engine's first run can then give
  stale or empty output: NaN tensors in the tests (3 to 13 runs out of 20, depending on the host's load), or no
  objects on the first frame (seen once in 7 runs of the tracker page's example). A rerun of the same engine is right.
  The element's warm-up runs on its own stream, which most likely only shifted the timing. The test suite sets
  `HSA_DISABLE_CACHE=1`, which avoids it, but that lowers `rds-test3`'s throughput by about 23 %, so pipelines don't.
  Whether officially supported GPUs are affected is not known yet. Workaround: `HSA_DISABLE_CACHE=1` (about 23 %
  slower).
- Compiling a real model takes minutes on a small GPU (YOLO11n FP32 at batch 1 on the gfx1036 iGPU: about 30 to 70
  s, batch 2 and 4 about 80 to 85 s): pre-build with `rds-compile`, and keep the `.mxr` caches.
- Each extra `dynamic-batch` program costs its own GPU memory (about 80–115 MB for YOLO11n; MIGraphX 2.18 can't share
  weights between programs).
- ONNX models with external weight files are not supported.
- MIGraphX gives every symbolic ONNX dimension a default (1) when it parses a model, silently; a symbolic height or
  width needs `infer-dims`.
