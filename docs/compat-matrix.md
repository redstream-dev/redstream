# DeepStream compatibility matrix

What `rds/rds_compat.h` maps, and how well. Every DeepStream name here was checked against NVIDIA's public
online documentation (the Source column links the page); Redstream's implementation is independent.

Status values: **supported** (same meaning), **accepted no-op** (compiles and is ignored), **unsupported**
(compiles, then fails at run time), **different default** (supported, but the default value differs), **planned**
(aliased, element not written yet). Names Redstream does not
alias at all are listed under "Not aliased" in each section: code using them does not compile.

Functions are aliased with object-like macros, so calls and `&function` work. Where a Redstream signature differs
slightly (a `const` parameter, a `gboolean` instead of `void` result), calls compile unchanged but assigning the
function to a pointer declared with DeepStream's exact prototype does not; the Notes column flags those.

`RDS_COMPAT_NO_MACROS` turns off only the `gst_element_factory_make` / `gst_parse_launch` redirects; every alias
below stays available.

## Elements

Factory names are remapped by `gst_element_factory_make` and `gst_parse_launch` through the compat header (table in
`gst-libs/rds/rds-compat-remap.c`). Set `RDS_COMPAT_NO_REMAP` to disable remapping at run time.

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `nvurisrcbin` | `rdsurisrcbin` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvurisrcbin) |
| `nvv4l2decoder` | `rdsvideodec` | supported | rocDecode: H.264/H.265 8-bit 4:2:0 to NV12; rank primary+10, so `uridecodebin` picks it over the VA decoders; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvvideo4linux2), [name](https://docs.nvidia.com/deepstream/text/DS_FAQ) |
| `nvvideoconvert` | `rdsvideoconvert` | supported | single frames (batch size 1); `memory:RDS` and system memory on either side; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvvideoconvert) |
| `nvstreammux` | `rdsstreammux` | supported | the default (scaling) mux only; the new mux is not implemented yet. GstAggregator-based; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreammux) |
| `nvstreamdemux` | `rdsstreamdemux` | supported | zero-copy read-only views of the batch slots; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreamdemux) |
| `nvinfer` | `rdsinfer` | supported | primary detector only (`process-mode=1`, `network-type` 0 or 100) on MIGraphX; nvinfer config files (key file or YAML); properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvinfer) |
| `nvtracker` | `rdstracker` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvtracker) |
| `nvmultistreamtiler` | `rdsmultistreamtiler` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvmultistreamtiler) |
| `nvdsosd` | `rdsosd` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsosd) |
| `nvdsanalytics` | `rdsanalytics` | planned | M2 | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsanalytics) |
| `nvdslogger` | `rdslogger` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdslogger) |
| `nveglglessink` | `rdsvideosink` | planned | display sink | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |
| `nv3dsink` | `rdsvideosink` | planned | display sink | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |
| `nvv4l2h264enc` | `rdsh264enc` | planned | VA (VCN) encode, M2 | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |
| `nvv4l2h265enc` | `rdsh265enc` | planned | VA (VCN) encode, M2 | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |

### `rdsvideoconvert` (`nvvideoconvert`) properties

Names and integers as on NVIDIA's [nvvideoconvert page](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideoconvert.html)
(checked 2026-10-24). That page names no enum nicks except for `nvbuf-memory-type`; the other nicks are Redstream's.

| Property | Status | Notes |
|---|---|---|
| `src-crop` | supported | clamped to the frame (a crop that is empty after clamping is an error); when the input buffer has a `GstVideoCropMeta`, relative to that crop (offset by its x/y, clamped to its size) |
| `dest-crop` | supported | clamped to the frame (a crop that is empty after clamping is an error); the rest of the output is filled with black: Y 16, U/V 128 for YUV, every byte 0 for GRAY8 and RGB formats, so **alpha is 0** outside the rect on RGBA/BGRA |
| `allow-odd-crop` | supported | odd 4:2:0 destination rects are widened to even coordinates; without it they are an error. Source rects may always be odd |
| `interpolation-method` | supported | filters as in "Transform semantics" (`Algo1`-`Algo3` run bilinear, `Algo4` nearest) |
| `compute-hw` | supported | `2` (VIC) fails at start; set while playing, it fails on the next frame that is converted (not while in passthrough) |
| `flip-method` | unsupported | any value but `0` fails at start; set while playing, it fails on the next frame (a flip turns passthrough off) |
| `contiguous-buffers`, `copy-hw`, `bl-output` | accepted no-op | batches are always contiguous, copies run on the GPU, output is pitch-linear |
| `gpu-id` | supported | default `0`; changeable only in NULL or READY |
| `nvbuf-memory-type` | supported | memory of the `memory:RDS` output buffers: `0` `nvbuf-mem-default` (HIP device memory, the default), `1` `nvbuf-mem-cuda-pinned`, `2` `nvbuf-mem-cuda-device`, `3` `nvbuf-mem-cuda-unified`; changeable only in NULL or READY |
| `output-buffers` | supported | minimum buffers in the output pool, default `4`; changeable only in NULL or READY |
| `disable-passthrough` | supported | passthrough happens when input and output caps are equal (memory type included) and no crop or flip is set |
| `silent` | accepted no-op |  |

System-memory input or output is converted through device staging surfaces (upload, transform, download).

Known limits: a batch (batch size above 1) is an element error; when downstream fixes only the width or only the
height, the other dimension keeps the input's value (no aspect-ratio-preserving fixation); metas tagged with
anything but `video` (for example size- or orientation-dependent ones) are dropped, as are `GstVideoMeta` (the
output pool adds its own) and `GstVideoCropMeta` (the crop is applied).

### `rdsvideodec` (`nvv4l2decoder`) properties

Names, ranges, enum integers and enum nicks as on NVIDIA's
[nvv4l2decoder page](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html)
(checked 2026-10-24). Input: H.264 and H.265 byte-stream access units (`h264parse`/`h265parse` in front), 8-bit
4:2:0 only. Output: NV12 in `memory:RDS` when downstream accepts it, otherwise system memory.

| Property | Status | Notes |
|---|---|---|
| `gpu-id` | supported | changeable only in NULL or READY; a GPU that does not exist fails the start |
| `cudadec-memtype` | different default | `0` `memtype_device`, `1` `memtype_pinned`, `2` `memtype_unified`: the memory of the `memory:RDS` output surfaces. **Default `0` (device)**; DeepStream's is `2` (unified), which is slow on discrete AMD GPUs. No effect on system-memory output |
| `num-extra-surfaces` | supported | 0-24 decode surfaces on top of the stream's minimum (plus 4 of headroom that Redstream always adds) |
| `drop-frame-interval` | supported | 0-30: outputs displayed frames 0, N, 2N, ...; 0 and 1 output every frame. Counted from the start or the last flush |
| `skip-frames` | supported | `0` `decode_all`; `2` `decode_key` drops delta frames before decoding; `1` `decode_non_ref` (Jetson-only on DeepStream) decodes every frame and logs a warning |
| `low-latency-mode` | accepted no-op | pictures are always output as soon as the stream order allows |

NVIDIA's page gives `num-extra-surfaces` as 1-24 and `drop-frame-interval` as 1-30, both with default 0; Redstream
accepts 0 for both. Jetson-only properties, `capture-io-mode`/`output-io-mode` and the SEI properties are not exposed.

### `rdsstreammux` (`nvstreammux`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreammux.html))

Names, types and enum integers as on NVIDIA's nvstreammux page (checked 2026-10-24). Only the default ("legacy")
mux is implemented: every frame is scaled into a fixed `width`×`height` slot of the batch. The new mux (zero-copy
batches of the decoder surfaces, `config-file-path`, adaptive batching, per-source frame limits) is not written yet.

NVIDIA's page lists example values, not defaults, for most properties. The Default column gives Redstream's
default; **ours** marks a default the page does not document, so it may differ from DeepStream's.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `batch-size` | uint | `1` (ours) | supported | frames per batch, 1 and up (NVIDIA's range starts at 0); changeable only in NULL or READY |
| `batched-push-timeout` | int, µs | `-1` (ours) | supported | `-1`: no timeout. See "Batching and timeout" below; changeable only in NULL or READY |
| `width`, `height` | uint | `0` | supported | the slot size, **required**: `0` fails negotiation with a message naming both properties (the new mux, which can pass sizes through, is not implemented). Range 0-16384 (NVIDIA: up to `G_MAXUINT`); changeable only in NULL or READY |
| `enable-padding` | boolean | `FALSE` (ours) | supported | keeps the aspect ratio; the frame is placed **top-left** (see below) |
| `gpu-id` | uint | `0` (ours) | supported | GPU for the scaling and the output batches; a GPU that does not exist is an error at the first batch; changeable only in NULL or READY |
| `nvbuf-memory-type` | enum | `0` (ours) | supported | `0` `nvbuf-mem-default` (HIP device memory), `1` `nvbuf-mem-cuda-pinned`, `2` `nvbuf-mem-cuda-device`, `3` `nvbuf-mem-cuda-unified`. `4` (Jetson surface array) is not a value of the enum: `g_object_set` rejects it with a warning and keeps the old value. Changeable only in NULL or READY |
| `buffer-pool-size` | uint | `4` (ours) | supported | batches in the output pool, 1-1024, allocated up front (`buffer-pool-size` × `batch-size` frames); the pool never grows, so a stalled downstream back-pressures the sources. Demux branches whose sinks keep their last buffer (appsink always, other sinks with `enable-last-sample`) pin one batch each, since a demuxed frame keeps its whole batch alive: set `buffer-pool-size` larger than the number of such branches, or the mux stalls for good. It is not raised automatically: that would cost memory in every pipeline without a demux. Changeable only in NULL or READY |
| `compute-hw` | enum | `0` (ours) | supported | `0` default (the GPU), `1` GPU; `2` (VIC) fails at start |
| `interpolation-method` | enum | `6` (ours) | supported | integers as `NvBufSurfTransform_Inter`, filters as in "Transform semantics" (`6` default and `5` `algo-4` run nearest, `2`-`4` bilinear) |
| `num-surfaces-per-frame` | uint | `1` (ours) | supported | only `1`; 2-4 fail at start |
| `live-source` | boolean | `FALSE` (ours) | supported | changes only the batch timestamp (see below), not the waiting |
| `attach-sys-ts` | boolean | `TRUE` (ours) | supported | each frame's `ntp_timestamp` is the system (wall-clock) time in ns; `FALSE` gives 0. RTCP sender-report time is not implemented |
| `sync-inputs` | boolean | `FALSE` (ours) | supported | see "Batching and timeout"; changeable only in NULL or READY |
| `max-latency` | uint, ns | `0` (ours) | supported | reported downstream in the latency query, and waited for (on top of the timeout) before a partial batch goes out in live mode; changeable only in NULL or READY |
| `sort-batch` | boolean | `FALSE` (ours) | supported | slots in sink pad order; otherwise in the order the frames were collected |
| `drop-pipeline-eos` | boolean | `FALSE` | supported | no EOS downstream when every source has ended; the per-source `stream-eos` events still go |
| `frame-num-reset-on-eos` | boolean | `FALSE` (ours) | supported | a source's `frame_num` restarts at 0 after its EOS |
| `frame-num-reset-on-stream-reset` | boolean | `FALSE` (ours) | supported | a source's `frame_num` restarts at 0 after a flush of that source |
| `frame-duration` | uint64 | `-1` (`G_MAXUINT64`) | accepted no-op | NTP correction is not implemented |
| `failsafe-flush-count` | uint | `10` (ours) | accepted no-op | range 1-1024 and not tied to `buffer-pool-size` (NVIDIA: 1 to `buffer-pool-size`, capped at one less) |
| `align-first-buffer`, `align-inputs`, `cache-buffer`, `extract-sei-sim-time`, `extract-sei-type5-data`, `drop-backward-sei`, `async-process` | boolean | `FALSE` (ours) | accepted no-op |  |
| `cache-buffer-timeout` | int, µs | `-1` (ours) | accepted no-op |  |
| `sync-inputs-ntp` | int, ns | `0` (ours) | accepted no-op |  |
| `latency`, `min-upstream-latency`, `start-time`, `start-time-selection`, `emit-signals` | | `GstAggregator`'s | Redstream only | inherited from `GstAggregator`, not on NVIDIA's page. `latency` belongs to `batched-push-timeout`: it is set from it (or to 0) at every start, so a value set directly has no effect |

Setting an accepted no-op property logs one INFO message per element (the first time any of them is set).

Pads: `sink_%u` request pads (sink pad N is source N) and `src`. Inputs: `video/x-raw(memory:RDS)` or
`video/x-raw` in system memory (uploaded through a per-pad staging surface), formats NV12, I420, RGBA, BGRA,
RGBx, BGRx, RGB, BGR and GRAY8, one frame per buffer (a batch as input is an element error). A `GstVideoCropMeta`
on an input is honoured: only that rect is scaled. Output: `video/x-raw(memory:RDS)`, `width`×`height`, the format
downstream picks (NV12 when it does not care), PAR 1/1, the inputs' framerate when they all agree, else `0/1`.

#### Frame meta

One `RdsBatchMeta` per batch with `max_frames_in_batch` = `batch-size`, and per frame: `source_id` and `pad_index`
= the sink pad number; `batch_id` = the slot; `frame_num` = a per-source counter from 0; `buf_pts` = the input
buffer's PTS (in the source's own segment); `ntp_timestamp` (see `attach-sys-ts`); `source_frame_width`/`height` =
the input size (after its crop meta); `pipeline_width`/`height` = the slot size; one surface per frame.

#### Scaling and padding

Every frame goes through `rds_buf_surf_transform` into its slot. With `enable-padding`, the frame is scaled to fit
with its aspect ratio kept (even width and height for NV12 and I420) and placed at the **top-left** of the slot; the
rest of the slot is black (Y 16, U/V 128 for YUV; every byte 0, alpha included, for RGB and GRAY8). NVIDIA's page
says only that black bands keep the aspect ratio, not where they go. Top-left keeps the mapping from slot to source
coordinates a pure scale (divide by `pipeline_width / source_frame_width`, no offset).

#### Batching and timeout

- One batch takes at most one frame per sink pad. The pads are visited round robin, the first one rotating every
  batch, and a batch is closed at `batch-size` frames.
- `batched-push-timeout` `-1` (no timeout): a batch goes out once every source that has not ended has a frame
  queued, or `batch-size` frames are collected. A stalled source therefore stalls the batch; set a timeout for live
  or unreliable sources.
- `batched-push-timeout` ≥ 0: a partial batch goes out one timeout after its **first frame arrived**, whether the
  frames are ahead of the clock (a file decoding faster than real time) or behind it, and also when that frame
  queued up while the previous batch was still waiting (it does not wait a full timeout after that batch). A live source's upstream
  latency is not added. During the first upstream-latency of running time, though, the timer cannot start before
  running time 0, so the first partial batches go out late by up to that latency.
- `sync-inputs=TRUE`: a partial batch goes out once the clock reaches its earliest frame's running time plus the
  latency (`batched-push-timeout` or 0, plus `max-latency` and the upstream latency). A frame already due goes out
  at once with whatever else is queued. As in every mode, a batch with a frame from every source goes out as soon
  as it is complete: frames are never held back to the clock. NVIDIA documents only "synchronizes input frames using
  PTS".
- A live (or timed) mux's sink pads queue at least two frames plus a latency's worth before blocking upstream,
  on top of the one being batched: upstream buffer pools must allow for that.

#### Batch timestamps

- `live-source=TRUE`: the batch PTS is the running time of the last frame copied into it (as documented).
- `live-source=FALSE`: the running time of the earliest frame in the batch. NVIDIA instead derives it from the
  framerate of the first source that negotiated caps.
- Either way, batch timestamps never go backwards: a batch earlier than the previous one (a source joining late, or
  with another segment) gets the previous batch's PTS. DTS and duration are unset. The output segment is TIME from
  0, so the PTS is the running time.

#### Stream events

The mux sends the stream events of the "Stream events" section downstream, in order with the batches, after its
own stream-start, caps and segment:

- `pad-added (N)` when source N's **first event or frame** comes through (NVIDIA: when the mux receives data from a
  new source), not when the pad is requested;
- `stream-start (N)` and `stream-segment (N)` from that pad's STREAM_START and SEGMENT;
- `stream-eos (N)` from that pad's EOS, after the batch with its last frame and before any later batch;
- `stream-reset (N)` before the source's first event or frame after a flush of that pad;
- `pad-deleted (N)` when the pad is released (only if its `pad-added` went out), after a batch being built at that
  moment (it may hold the pad's last frame).

The pipeline EOS goes downstream when every source has ended, unless `drop-pipeline-eos` is set.

Releasing a sink pad sends `pad-deleted` from the releasing thread. If a prerolled sink downstream is blocking (the
pipeline is PAUSED and the mux idle), that push, and so the release, blocks until the pipeline goes to PLAYING:
release pads while PLAYING, or from a thread that may wait.

To remove a source cleanly, send EOS into its sink pad and wait for its `stream-eos (N)` downstream before releasing
the pad: the EOS reaches the mux's batching thread only after the frames queued in front of it, and a pad released
before that loses the frames still queued and its `stream-eos` (only `pad-deleted` goes out).

### `rdsstreamdemux` (`nvstreamdemux`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreamdemux.html))

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `per-stream-eos` | boolean | `FALSE` | supported | `TRUE`: `stream-eos (N)` ends `src_N` (EOS on it) as soon as source N ends. `FALSE`: every pad gets EOS on the pipeline EOS |

Pads: `sink` (`video/x-raw(memory:RDS)` batches with an `RdsBatchMeta`, as `rdsstreammux` makes them) and `src_%u`
request pads: frames of source N go to `src_N`. Pads may be requested and released while PLAYING, from any thread,
also from a `pad-added` handler.

#### Buffers (views)

- Frames are not copied: each output buffer is a **read-only view** of the frame's slot in the batch, and the batch
  goes back to the mux's pool once every view of it is released. The view's PTS is the frame's `buf_pts` (the
  source's original timestamp), DTS is unset, and flags and duration come from the batch. It carries a one-frame
  `RdsBatchMeta` (a copy of the frame meta, `batch_id` 0).
- To write pixels (an OSD, for example), make the buffer writable (`gst_buffer_make_writable`) and map it for
  writing: the writer then gets its own copy of the frame, and the batch the other sources share stays untouched. A
  write map of the view itself fails.
- Frames stay at the mux's `width`×`height`; nothing is scaled back to the source size (as documented).
- Frames of a source with no requested pad, or whose pad has ended, are dropped.

#### Events

- Each `src_N` gets its own stream-start (the upstream stream id plus `/N`), the sink caps, and **source N's own
  segment** (from `stream-segment (N)`, else the mux's output segment): `buf_pts` is in the source's segment, so
  synchronized sinks compute the source's running times.
- Stream events for source N (`pad-added`, `pad-deleted`, `stream-start`, `stream-segment`, `stream-eos`,
  `stream-reset`) go to `src_N` only, and are dropped when it does not exist, so a per-source branch sees only its
  own source's lifecycle. `pad-deleted (N)` always ends `src_N`; a later `stream-start`, `pad-added` or
  `stream-reset` for N starts a new stream on it.
- Flushes, the pipeline EOS (to pads not ended yet) and other events go to every pad.
- Upstream from the src pads: QoS is not forwarded (one slow branch would throttle every source); a seek sent by
  several branches (same seqnum) goes upstream once; other events go upstream.

#### Flow and branches

The demux returns the combination of its pads' results: NOT_LINKED only when every pad is unlinked. As with `tee`,
FLUSHING or an error from one branch stops the streaming thread for all of them, so **block or unlink a branch
before stopping it** (a sink set to NULL while still linked returns FLUSHING, which stops every branch). Give every branch a `queue`:
otherwise the first sink to preroll blocks the others, which share the demux's streaming thread.

### `rdsinfer` (`nvinfer`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvinfer.html))

Names and types as on NVIDIA's nvinfer page (checked 2026-10-24). As documented, a property set explicitly overrides
the config file; in Redstream that holds also when it is set to its default value. All properties can be changed
only in NULL or READY: the config is read and the engine built or loaded when the element starts (READY to PAUSED),
and every error there fails the state change with a message naming the file (or `element property`), the key and
the value. Defaults are those of the config keys (**ours**, NVIDIA documents none) and apply only when the property
is set.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `config-file-path` | string | none | supported | required; `.yml`/`.yaml` is read as YAML, anything else as a key file |
| `unique-id` | uint | `1` (ours) | supported | overrides `gie-unique-id`; range 0..`G_MAXUINT` as documented (the config key starts at 1) |
| `process-mode` | uint | `1` | supported | `1` only; `2` (secondary) fails at start naming the property |
| `infer-on-gie-id` | uint | `0` | accepted no-op | secondary mode only; a warning when set |
| `infer-on-class-ids` | string | none | accepted no-op | secondary mode only: colon-separated class ids (`1:2:4`, `-1` allowed), checked at start, then ignored with a warning |
| `filter-out-class-ids` | string | none | supported | semicolon-separated class ids (`1;2`); replaces the file's list (an empty string clears it) |
| `model-engine-file` | string | none | supported | a compiled MIGraphX program (`.mxr`), loaded when its manifest matches the model and the device, else rebuilt into the cache name; used as given (a config-file value is relative to the config file). A TensorRT `.engine` is ignored with a warning. Changing it while PLAYING (on-the-fly model update) is not supported |
| `batch-size` | uint | `1` (ours) | supported | 1..1024. Frames of a batch go to the network image of their slot (`batch_id`); a frame in a slot at or above `batch-size` is an error |
| `interval` | int | `0` (ours) | supported | 0..32: infer one batch, skip the next `interval` (they pass with `bInferDone` FALSE and no objects); the count restarts at start and after a flush (ours) |
| `gpu-id` | uint | `0` | supported | the GPU for preprocessing and inference; the batch must be in GPU memory on it |
| `output-tensor-meta` | boolean | `FALSE` | supported | one `RdsInferTensorMeta` per inferred frame (also always with `network-type=100`), see "Tensor output meta" |
| `raw-output-file-write` | boolean | `FALSE` | accepted no-op | not implemented; a warning (once per element) when set |
| `raw-output-generated-callback` | pointer | NULL | accepted no-op | the callback's signature isn't documented; a warning (once per element) when set |
| `raw-output-generated-userdata` | pointer | NULL | accepted no-op |  |
| `output-instance-mask` | boolean | `FALSE` | unsupported | `TRUE` fails at start (instance segmentation is not implemented) |
| `input-tensor-meta` | boolean | `FALSE` | unsupported | `TRUE` fails at start (preprocessed tensor input is not implemented) |
| `crop-objects-to-roi-boundary` | boolean | `FALSE` | accepted no-op | applies to preprocessed-tensor ROIs; a warning when `TRUE` |
| `clip-object-outside-roi` | boolean | `FALSE` | accepted no-op | applies to preprocessed-tensor ROIs; a warning when `TRUE` |

Not implemented: `classifier-type`, and the TensorRT-only `strongly-typed` and `dynamic-properties`.

Config file paths: `onnx-file`, `model-engine-file`, `labelfile-path` and `custom-lib-path` from the file are relative
to the config file's directory (documented for YAML, ours for key files). That includes a bare `custom-lib-path`
name (no `/`): it is the file next to the config, not a library searched on the loader path. Use an absolute path
(as NVIDIA's docs ask) for a library elsewhere.

Pads: `sink` and `src`, `video/x-raw(memory:RDS)` NV12, RGBA, BGRA, RGBx or BGRx batches with an `RdsBatchMeta`
(from `rdsstreammux`); buffers pass through unchanged (metadata only, pixels are never written). A buffer without
batch meta is an error.

#### Detector output (object meta)

- Boxes are in the batch frame's pixels (the mux's `width`×`height`); `maintain-aspect-ratio` / `symmetric-padding`
  scaling is undone (ours: the scaled frame sits at the top-left, or centred with `symmetric-padding`).
  `detector_bbox_info.org_bbox_coords` is the unclipped box and `rect_params` the box clipped to the frame
  (documented).
- Fields set: `unique_component_id` (`gie-unique-id`), `class_id`, `confidence`, `object_id` =
  `UNTRACKED_OBJECT_ID`, `obj_label` (the label, truncated to 127 bytes), `parent` NULL. Drawing defaults (ours):
  `rect_params` border 3 px red, no background; `text_params` the label at the box's top-left, 20 px above it
  (clamped to 0), "Serif" 10, white on black.
- Frames that were inferred get `bInferDone` TRUE. Post-processing order and NMS are described with the config keys.
- A parse function that returns `false` or throws: that frame gets no objects, and the element posts one warning
  (then logs at debug level); the stream goes on. Invalid objects from a parser (class id out of range, non-finite
  values, empty boxes) are dropped, and a non-zero `rotation_angle` is ignored: one warning each.

#### Tensor output meta

One `RdsUserMeta` per inferred frame on `frame_user_meta_list`, `meta_type` `RDS_INFER_TENSOR_OUTPUT_META`
(`NVDSINFER_TENSOR_OUTPUT_META`), data `RdsInferTensorMeta`. `out_buf_ptrs_host[i]` (pinned host memory) and
`out_buf_ptrs_dev[i]` point at the frame's slice of output layer `i`; the `buffer` pointers in
`output_layers_info` are NULL (documented: not valid there). The buffers stay valid as long as the meta exists, in
copies too (`rdsstreamdemux` outputs, `rds_batch_meta_copy`), also after the element has stopped. They are
refcounted and reused once the last meta pointing at them is gone, so `tensor-meta-pool-size` has no effect.

#### Processing

One batch at a time, synchronously (ours; nvinfer's threading isn't documented): preprocessing and inference on the
element's own non-blocking stream, one synchronization per batch, parsing on the CPU. Slots without a frame (a
partial batch) are zeroed, not inferred on stale pixels. The batch's metadata lock is held only to read the frame
list and to add the results, never during GPU work.

#### Pre-building programs: `rds-compile` (`trtexec`)

The element compiles the model on its first start, which takes minutes on a small GPU. `rds-compile` does it ahead
of time with the element's own config parser and engine, so the program lands where the element looks for it:

```bash
rds-compile -c CONFIG [-b BATCH] [--fp16|--fp32] [-g GPU] [-o OUT.mxr] [--force]
```

- `-b`, `--fp16`/`--fp32` and `-g` override `batch-size`, `network-mode` and `gpu-id`; `--fp16` with `--fp32` is an
  error. Without `-o` the program is looked up and written like the element does (`model-engine-file`, then the
  cache name next to the ONNX file, then `$XDG_CACHE_HOME/redstream/`). `-o` looks up and writes only that file,
  with its manifest next to it as `OUT.mxr.json`; point `model-engine-file` at it.
- Prints the program's path on stdout; the build reason, warnings and errors (with the element's messages) on
  stderr.
- Exit status: 0 built, 1 error, 2 already up to date (the element would load it; `--force` rebuilds).

## NvBufSurface

Types are `typedef`s, functions and enumerators are object-like macros (taking a function's address works).

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NvBufSurface` | `RdsBufSurface` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceParams` | `RdsBufSurfaceParams` | supported | `bufferDesc` unused | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfacePlaneParams` | `RdsBufSurfacePlaneParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceMappedAddr` | `RdsBufSurfaceMappedAddr` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceCreateParams` | `RdsBufSurfaceCreateParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceMemType` | `RdsBufSurfaceMemType` | supported | integer values 0-6 match the documented order (configs pass `nvbuf-memory-type=N`) | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceColorFormat` | `RdsBufSurfaceColorFormat` | supported | only the formats below; integer values are not DeepStream's, use the names | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceLayout` | `RdsBufSurfaceLayout` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceMemMapFlags` | `RdsBufSurfaceMemMapFlags` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MEM_DEFAULT` | `RDS_BUF_MEM_DEFAULT` | supported | HIP device memory | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MEM_CUDA_PINNED` | `RDS_BUF_MEM_HIP_PINNED` | supported | hipHostMalloc | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MEM_CUDA_DEVICE` | `RDS_BUF_MEM_HIP_DEVICE` | supported | hipMalloc | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MEM_CUDA_UNIFIED` | `RDS_BUF_MEM_HIP_UNIFIED` | supported | hipMallocManaged | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MEM_SURFACE_ARRAY` | `RDS_BUF_MEM_SURFACE_ARRAY` | unsupported | Jetson only: rejected at create time | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MEM_HANDLE` | `RDS_BUF_MEM_HANDLE` | unsupported | Jetson only: rejected at create time | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MEM_SYSTEM` | `RDS_BUF_MEM_SYSTEM` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_INVALID` | `RDS_BUF_COLOR_FORMAT_INVALID` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_GRAY8` | `RDS_BUF_COLOR_FORMAT_GRAY8` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_YUV420` | `RDS_BUF_COLOR_FORMAT_YUV420` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_NV12` | `RDS_BUF_COLOR_FORMAT_NV12` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_RGBA` | `RDS_BUF_COLOR_FORMAT_RGBA` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_BGRA` | `RDS_BUF_COLOR_FORMAT_BGRA` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_RGBx` | `RDS_BUF_COLOR_FORMAT_RGBx` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_BGRx` | `RDS_BUF_COLOR_FORMAT_BGRx` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_RGB` | `RDS_BUF_COLOR_FORMAT_RGB` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_BGR` | `RDS_BUF_COLOR_FORMAT_BGR` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_COLOR_FORMAT_LAST` | `RDS_BUF_COLOR_FORMAT_LAST` | supported | end marker of the Redstream enum, not DeepStream's value | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_LAYOUT_PITCH` | `RDS_BUF_LAYOUT_PITCH` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_LAYOUT_BLOCK_LINEAR` | `RDS_BUF_LAYOUT_BLOCK_LINEAR` | unsupported | rejected at create time | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MAP_READ` | `RDS_BUF_MAP_READ` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MAP_WRITE` | `RDS_BUF_MAP_WRITE` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NVBUF_MAP_READ_WRITE` | `RDS_BUF_MAP_READ_WRITE` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceCreate` | `rds_buf_surface_create` | supported | `params` is `const`, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceDestroy` | `rds_buf_surface_destroy` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceMap` | `rds_buf_surface_map` | supported | device memory maps a pinned staging mirror (map, sync_for_cpu, ..., sync_for_device, unmap) | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceUnMap` | `rds_buf_surface_unmap` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceSyncForCpu` | `rds_buf_surface_sync_for_cpu` | supported | Synchronous, on the calling thread's per-thread stream: does not wait for work queued on other streams; synchronize producers first. | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceSyncForDevice` | `rds_buf_surface_sync_for_device` | supported | Synchronous, on the calling thread's per-thread stream: does not wait for work queued on other streams; synchronize producers first. | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceCopy` | `rds_buf_surface_copy` | supported | `src` is `const`, but assigning it to a pointer with DeepStream's exact prototype does not compile. Synchronous, on the calling thread's per-thread stream: does not wait for work queued on other streams; synchronize producers first. | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |
| `NvBufSurfaceMemSet` | `rds_buf_surface_memset` | supported | Synchronous, on the calling thread's per-thread stream: does not wait for work queued on other streams; synchronize producers first. | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvbufsurface_8h.html) |

Not aliased: `NVBUF_MEM_CUDA_ARRAY` (no Redstream counterpart), the other `NvBufSurfaceColorFormat` values, and the rest of the
`NvBufSurface*` API (EGL, CUDA-specific calls). The transform API has its own section below.

## NvBufSurfTransform

Batched crop, scale and colour conversion between surfaces, run by Redstream's own HIP kernels. Types are
`typedef`s, functions and enumerators are object-like macros. All four functions have DeepStream's exact prototypes,
so they can also be assigned to function pointers declared with them.

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NvBufSurfTransform_Compute` | `RdsBufSurfTransform_Compute` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformCompute_Default` | `RdsBufSurfTransformCompute_Default` | supported | runs on the GPU | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformCompute_GPU` | `RdsBufSurfTransformCompute_GPU` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformCompute_VIC` | `RdsBufSurfTransformCompute_VIC` | unsupported | Jetson engine: `NvBufSurfTransformSetSessionParams` returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Flip` | `RdsBufSurfTransform_Flip` | supported | values 0-7 as documented; only `None` runs | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_None` | `RdsBufSurfTransform_None` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Rotate90` | `RdsBufSurfTransform_Rotate90` | unsupported | with `NVBUFSURF_TRANSFORM_FLIP` set, the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Rotate180` | `RdsBufSurfTransform_Rotate180` | unsupported | with `NVBUFSURF_TRANSFORM_FLIP` set, the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Rotate270` | `RdsBufSurfTransform_Rotate270` | unsupported | with `NVBUFSURF_TRANSFORM_FLIP` set, the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_FlipX` | `RdsBufSurfTransform_FlipX` | unsupported | with `NVBUFSURF_TRANSFORM_FLIP` set, the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_FlipY` | `RdsBufSurfTransform_FlipY` | unsupported | with `NVBUFSURF_TRANSFORM_FLIP` set, the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Transpose` | `RdsBufSurfTransform_Transpose` | unsupported | with `NVBUFSURF_TRANSFORM_FLIP` set, the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_InvTranspose` | `RdsBufSurfTransform_InvTranspose` | unsupported | with `NVBUFSURF_TRANSFORM_FLIP` set, the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Inter` | `RdsBufSurfTransform_Inter` | supported | values 0-6 as documented | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformInter_Nearest` | `RdsBufSurfTransformInter_Nearest` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformInter_Bilinear` | `RdsBufSurfTransformInter_Bilinear` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformInter_Algo1` | `RdsBufSurfTransformInter_Algo1` | supported | runs as bilinear (one warning per process) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformInter_Algo2` | `RdsBufSurfTransformInter_Algo2` | supported | runs as bilinear (one warning per process) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformInter_Algo3` | `RdsBufSurfTransformInter_Algo3` | supported | runs as bilinear (one warning per process) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformInter_Algo4` | `RdsBufSurfTransformInter_Algo4` | supported | runs as nearest | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformInter_Default` | `RdsBufSurfTransformInter_Default` | supported | nearest | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Error` | `RdsBufSurfTransform_Error` | supported | values -4..0 as documented | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformError_ROI_Error` | `RdsBufSurfTransformError_ROI_Error` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformError_Invalid_Params` | `RdsBufSurfTransformError_Invalid_Params` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformError_Execution_Error` | `RdsBufSurfTransformError_Execution_Error` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformError_Unsupported` | `RdsBufSurfTransformError_Unsupported` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformError_Success` | `RdsBufSurfTransformError_Success` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform_Transform_Flag` | `RdsBufSurfTransform_Transform_Flag` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NVBUFSURF_TRANSFORM_CROP_SRC` | `RDS_BUFSURF_TRANSFORM_CROP_SRC` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NVBUFSURF_TRANSFORM_CROP_DST` | `RDS_BUFSURF_TRANSFORM_CROP_DST` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NVBUFSURF_TRANSFORM_FILTER` | `RDS_BUFSURF_TRANSFORM_FILTER` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NVBUFSURF_TRANSFORM_FLIP` | `RDS_BUFSURF_TRANSFORM_FLIP` | supported | only with `NvBufSurfTransform_None` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NVBUFSURF_TRANSFORM_NORMALIZE` | `RDS_BUFSURF_TRANSFORM_NORMALIZE` | unsupported | the transform returns `Unsupported` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NVBUFSURF_TRANSFORM_ALLOW_ODD_CROP` | `RDS_BUFSURF_TRANSFORM_ALLOW_ODD_CROP` | supported | see "Odd crops" below | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformRect` | `RdsBufSurfTransformRect` | supported | never clamped: a rect outside its surface is `ROI_Error` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformParams` | `RdsBufSurfTransformParams` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformConfigParams` | `RdsBufSurfTransformConfigParams` | supported | `cuda_stream` is a `gpointer` holding a `hipStream_t` (NULL: the default stream); C++ code reading it back needs a cast | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransform` | `rds_buf_surf_transform` | supported | synchronous; BT.601 limited range only; see "Transform semantics" | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformSetSessionParams` | `rds_buf_surf_transform_set_session_params` | supported | per thread | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformGetSessionParams` | `rds_buf_surf_transform_get_session_params` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |
| `NvBufSurfTransformSetDefaultSession` | `rds_buf_surf_transform_set_default_session` | supported | GPU 0, default stream | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html) |

Not aliased: `NvBufSurfTransformAsync`, the composite and blend calls and their types (`NvBufSurfTransformComposite*`,
`NvBufSurfTransformMultiInputBufCompositeBlend*`, `NvBufSurfTransform_Composite_Flag` and its
`NVBUFSURF_TRANSFORM_COMPOSITE*`/`BLEND` flags), the sync objects (`NvBufSurfTransformSyncObj*`),
`NvBufSurfTransform_ColorParams` and `NVBUFSURF_MAX_SYNCOBJ` ([ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvbufsurftransform_8h_source.html)).

### Behaviour differences from DeepStream

| Area | DeepStream | Redstream |
|---|---|---|
| Compute engine | GPU or VIC (Jetson) | GPU only; VIC is `Unsupported` |
| Flips and rotations | all eight `NvBufSurfTransform_Flip` values | `None` only; others are `Unsupported` |
| `NVBUFSURF_TRANSFORM_NORMALIZE` | supported | `Unsupported` |
| `Algo1`-`Algo3` filters | cubic, super-sampling, Lanczos (GPU) | bilinear, with a one-time warning |
| Colour conversion | several matrices and ranges | BT.601 limited range only |
| Execution | synchronous call plus an async variant with sync objects | synchronous only |
| Memory | CUDA and Jetson surface types | HIP device, default, pinned and unified; system memory is `Unsupported` |

### Transform semantics

What `rds_buf_surf_transform` (`NvBufSurfTransform`) does, precisely enough to predict every output byte. The GPU
tests compare the kernels with an independent CPU implementation of these rules.

**Calls and sessions**

- Session parameters belong to the calling thread. A thread that never set them, or called
  `NvBufSurfTransformSetDefaultSession`, uses GPU 0 and the default (null) stream. `SetSessionParams` rejects
  `VIC` (`Unsupported`), an unknown compute mode, a negative `gpu_id` or one at or beyond the number of visible
  devices (`Invalid_Params`). With no visible device at all, any non-negative `gpu_id` is stored and the transform
  itself then returns `Execution_Error`.
- The call is synchronous: the work is queued on the session stream of the session GPU, and the stream is
  synchronized before the call returns, so the destination is ready to use (or push downstream) immediately.
- Same-format, same-size copies may run as plain 2D memory copies instead of the kernel; the bytes are identical.

**Arguments**

- The first `src->numFilled` surfaces are transformed, surface `i` of the source into surface `i` of the
  destination. `numFilled` must be at least 1 and no larger than either batch size; otherwise `Invalid_Params`.
  `dst->numFilled` is set to the same count, only on success. On an error before the GPU work starts, the
  destination is not touched at all.
- Both surfaces must be reachable from the GPU: default, HIP device, pinned or unified memory. System memory on
  either side is `Unsupported`. Device memory (default, device) whose `gpuId` differs from the session GPU is
  `Invalid_Params`; pinned and unified memory work from any GPU.
- Formats: GRAY8, YUV420 (I420), NV12, RGBA, BGRA, RGBx, BGRx, RGB, BGR, pitch-linear layout, in any combination.
  Another format or the block-linear layout is `Unsupported`. A surface larger than 16384 in either dimension, of
  size 0, with a filled entry whose `dataPtr` is NULL, or whose plane count, pitches or plane heights can't hold
  the format, is `Invalid_Params`.
- `src_rect` and `dst_rect` are arrays with one rectangle per filled surface. Each is read only when its flag
  (`CROP_SRC`, `CROP_DST`) is set; without the flag the whole surface is used. A flag with a NULL array is
  `Invalid_Params`. A rectangle that is empty or reaches past its surface is `ROI_Error`: the transform never clamps
  (`rdsvideoconvert` clamps its `src-crop`/`dest-crop` properties itself, as `nvvideoconvert` documents).
- Destination pixels outside the destination rectangle (after the 4:2:0 widening described under "Odd crops") are
  never written.
- `transform_flip` is read only when `FLIP` is set, and must then be `None`. `transform_filter` is read only when
  `FILTER` is set (an out-of-range value is then `Invalid_Params`); without the flag the filter is nearest.
- `NORMALIZE` is `Unsupported`.
- Source and destination must not overlap: in-place transforms are not supported. A filled entry whose source and
  destination share the same `dataPtr` is `Invalid_Params`; other overlaps are not detected and give undefined
  output.
- When several problems apply, the first in this order is reported: batch counts; VIC, `NORMALIZE`, a flip; memory
  types; the GPU of device memory; formats and layouts; sizes and plane geometry; source then destination
  rectangles; odd 4:2:0 destination rectangles; the filter value; overlapping source and destination. For example, system memory with an odd
  destination rectangle is `Unsupported`, not `Invalid_Params`.

**Sampling (filters)**

- `Nearest`, `Default` and `Algo4` sample the nearest pixel; `Bilinear` and `Algo1`-`Algo3` interpolate bilinearly.
- Coordinates are continuous, with pixel `i` covering `[i, i + 1)`. Destination pixel `xd` of a destination
  rectangle starting at `dl` with width `dw` looks at source position `xs = sl + (xd - dl + 0.5) * sw / dw`, where
  `sl` and `sw` describe the source rectangle (likewise vertically). Pixel centres map to pixel centres, so a
  same-size nearest transform is an exact copy.
- Nearest takes pixel `floor(xs)`, computed in exact integer arithmetic (`sl + ((2k + 1) * sw) / (2 * dw)` with
  `k = xd - dl`), so the choice never depends on floating-point rounding.
- Bilinear interpolates between the two pixels around `xs - 0.5`, in `float`.
- Every full-resolution sample index (luma, RGB, GRAY8) is clamped to the source rectangle, so those pixels are
  never read outside it, even at its edges. Subsampled chroma has its own clamp range (see "Colour"): with an odd
  source rectangle, the edge chroma samples also cover a luma pixel just outside it.

**Colour**

- YUV and RGB convert with the BT.601 limited-range matrix, both ways:
  - `R = 1.164 (Y - 16) + 1.596 (V - 128)`, `G = 1.164 (Y - 16) - 0.392 (U - 128) - 0.813 (V - 128)`,
    `B = 1.164 (Y - 16) + 2.017 (U - 128)`;
  - `Y = 16 + 0.257 R + 0.504 G + 0.098 B`, `U = 128 - 0.148 R - 0.291 G + 0.439 B`,
    `V = 128 + 0.439 R - 0.368 G - 0.071 B`.
- Values stay in `float` until they are written: samples are converted unrounded, and averaged chroma is averaged
  before rounding. Each written byte is `floor(v + 0.5)` clamped to 0..255.
- YUV 4:2:0 to YUV 4:2:0 scales each plane at its own resolution: the chroma plane is treated as an image with the
  rectangles halved, so chroma position is `sl / 2 + (c - dl / 2 + 0.5) * sw / dw` for chroma column `c` (`dl` is
  even, see "Odd crops"), clamped like the other chroma samples below. A same-size nearest NV12 to I420 transform
  is therefore an exact repack.
- RGB to YUV 4:2:0 converts each pixel of a 2x2 block to YUV, writes the four Y values, and writes the mean of the
  block's U and V.
- YUV 4:2:0 to RGB: chroma is sampled at half the luma position, `xs / 2` (nearest `floor(xs / 2)`,
  bilinear around `xs / 2 - 0.5`), clamped to the chroma samples that cover the source rectangle,
  `floor(sl / 2)` to `ceil((sl + sw) / 2) - 1`. This places chroma at the centre of each 2x2 block. H.264 and HEVC
  streams usually signal left-sited chroma (`chroma_sample_loc_type` 0), a quarter of a luma pixel to the left;
  Redstream ignores that offset for now.
- GRAY8 is the luma plane when the other side is YUV: GRAY8 to YUV copies Y and sets U = V = 128, YUV to GRAY8 keeps
  Y. When the other side is RGB, GRAY8 is full-range intensity: GRAY8 to RGB sets R = G = B = gray, RGB to GRAY8
  writes `0.299 R + 0.587 G + 0.114 B`.
- Alpha is sampled (and, with bilinear, interpolated) like the colour channels only from RGBA/BGRA to RGBA/BGRA;
  any other source writes alpha 255. The padding byte of RGBx and
  BGRx is always written as 255.

**Odd crops on 4:2:0 destinations**

- A 2x2 chroma block can't be half-written, so a destination rectangle on an NV12 or I420 surface is widened to
  even coordinates: its left and top edges are rounded down to even, its right and bottom edges rounded up to even
  and clamped to the surface. The source is scaled onto the widened rectangle.
- Without `ALLOW_ODD_CROP`, a destination rectangle with an odd edge on a 4:2:0 surface is `Invalid_Params`, except
  a right or bottom edge that lies on the surface's own odd edge, which counts as even. With the flag, any rectangle
  is widened as above. Source rectangles may always be odd: sampling is continuous.
- On a surface with an odd width or height, the last column or row of 2x2 blocks is partial: only the in-bounds luma
  pixels are written, the chroma sample is still written, and for an RGB source its U and V are the mean over the
  in-bounds pixels only.

## OSD parameter structs

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NvOSD_ColorParams` | `RdsOSD_ColorParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_FontParams` | `RdsOSD_FontParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_TextParams` | `RdsOSD_TextParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_RectParams` | `RdsOSD_RectParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_LineParams` | `RdsOSD_LineParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_ArrowParams` | `RdsOSD_ArrowParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_CircleParams` | `RdsOSD_CircleParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_MaskParams` | `RdsOSD_MaskParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_Arrow_Head_Direction` | `RdsOSD_Arrow_Head_Direction` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvBbox_Coords` | `RdsBbox_Coords` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `START_HEAD` | `RDS_OSD_ARROW_START_HEAD` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `END_HEAD` | `RDS_OSD_ARROW_END_HEAD` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `BOTH_HEAD` | `RDS_OSD_ARROW_BOTH_HEAD` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |

## Metadata

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NvDsBaseMeta` | `RdsBaseMeta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsBatchMeta` | `RdsBatchMeta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsFrameMeta` | `RdsFrameMeta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsObjectMeta` | `RdsObjectMeta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsClassifierMeta` | `RdsClassifierMeta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsLabelInfo` | `RdsLabelInfo` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsDisplayMeta` | `RdsDisplayMeta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsUserMeta` | `RdsUserMeta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsMetaPool` | `RdsMetaPool` | supported | opaque in Redstream: pools are reached only through the acquire functions | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsMetaType` | `RdsMetaType` | supported | Redstream reserves 8..4095 for its own components | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsMetaCopyFunc` | `RdsMetaCopyFunc` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html) |
| `NvDsMetaReleaseFunc` | `RdsMetaReleaseFunc` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html) |
| `NvDsMetaList` | `RdsMetaList` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsFrameMetaList` | `RdsFrameMetaList` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsObjectMetaList` | `RdsObjectMetaList` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsClassifierMetaList` | `RdsClassifierMetaList` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsLabelInfoList` | `RdsLabelInfoList` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsUserMetaList` | `RdsUserMetaList` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDisplayMetaList` | `RdsDisplayMetaList` | supported | DeepStream's name has no "Ds" | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsComp_BboxInfo` | `RdsComp_BboxInfo` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NvDsMeta` | `RdsMeta` | supported | only `meta`, `meta_data`, `user_data`, `meta_type` | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html) |
| `NVDS_INVALID_META` | `RDS_INVALID_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_BATCH_META` | `RDS_BATCH_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_FRAME_META` | `RDS_FRAME_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_OBJ_META` | `RDS_OBJ_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_DISPLAY_META` | `RDS_DISPLAY_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_CLASSIFIER_META` | `RDS_CLASSIFIER_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_LABEL_INFO_META` | `RDS_LABEL_INFO_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_USER_META` | `RDS_USER_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDSINFER_TENSOR_OUTPUT_META` | `RDS_INFER_TENSOR_OUTPUT_META` | supported | 12, its documented position in the enum | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_RESERVED_META` | `RDS_RESERVED_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_GST_CUSTOM_META` | `RDS_GST_CUSTOM_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html) |
| `NVDS_BATCH_GST_META` | `RDS_BATCH_GST_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html) |
| `NVDS_START_USER_META` | `RDS_START_USER_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_FORCE32_META` | `RDS_FORCE32_META` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `MAX_LABEL_SIZE` | `RDS_MAX_LABEL_SIZE` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `MAX_ELEMENTS_IN_DISPLAY_META` | `RDS_MAX_ELEMENTS_IN_DISPLAY_META` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `MAX_USER_FIELDS` | `RDS_MAX_USER_FIELDS` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `MAX_RESERVED_FIELDS` | `RDS_MAX_RESERVED_FIELDS` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `UNTRACKED_OBJECT_ID` | `RDS_UNTRACKED_OBJECT_ID` | supported | defined only if not already defined | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_create_batch_meta` | `rds_create_batch_meta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_destroy_batch_meta` | `rds_destroy_batch_meta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_acquire_meta_lock` | `rds_acquire_meta_lock` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_release_meta_lock` | `rds_release_meta_lock` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_acquire_frame_meta_from_pool` | `rds_acquire_frame_meta_from_pool` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_frame_meta_to_batch` | `rds_add_frame_meta_to_batch` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_frame_meta_from_batch` | `rds_remove_frame_meta_from_batch` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_acquire_obj_meta_from_pool` | `rds_acquire_obj_meta_from_pool` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_obj_meta_to_frame` | `rds_add_obj_meta_to_frame` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_obj_meta_from_frame` | `rds_remove_obj_meta_from_frame` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_acquire_classifier_meta_from_pool` | `rds_acquire_classifier_meta_from_pool` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_classifier_meta_to_object` | `rds_add_classifier_meta_to_object` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_classifier_meta_from_obj` | `rds_remove_classifier_meta_from_obj` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_acquire_label_info_meta_from_pool` | `rds_acquire_label_info_meta_from_pool` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_label_info_meta_to_classifier` | `rds_add_label_info_meta_to_classifier` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_label_info_meta_from_classifier` | `rds_remove_label_info_meta_from_classifier` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_acquire_display_meta_from_pool` | `rds_acquire_display_meta_from_pool` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_display_meta_to_frame` | `rds_add_display_meta_to_frame` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_display_meta_from_frame` | `rds_remove_display_meta_from_frame` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_acquire_user_meta_from_pool` | `rds_acquire_user_meta_from_pool` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_user_meta_to_batch` | `rds_add_user_meta_to_batch` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_user_meta_to_frame` | `rds_add_user_meta_to_frame` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_add_user_meta_to_obj` | `rds_add_user_meta_to_obj` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_user_meta_from_batch` | `rds_remove_user_meta_from_batch` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_user_meta_from_frame` | `rds_remove_user_meta_from_frame` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_remove_user_meta_from_object` | `rds_remove_user_meta_from_object` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `nvds_get_user_meta_type` | `rds_get_user_meta_type` | supported | takes `const gchar *`, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `gst_buffer_get_nvds_batch_meta` | `gst_buffer_get_rds_batch_meta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html) |
| `NvDsInferTensorMeta` | `RdsInferTensorMeta` | supported | `rds/rds-infer-meta.h`; the documented fields in order; `priv_data` is rdsinfer's (it keeps the buffers alive). See the `rdsinfer` section | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferTensorMeta.html) |

Not aliased: `gst_buffer_add_nvds_meta` (Redstream attaches the batch with `rds_buffer_add_batch_meta`;
[ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html)), and
NvDs metadata types and functions Redstream doesn't implement yet (segmentation, analytics, event message, audio).

## Stream events (gst-nvevent)

Events are custom events with their own `GstEventType` numbers, so dispatching on `GST_EVENT_TYPE` works.
`rdsstreammux` sends them and `rdsstreamdemux` routes them per source (see their sections above).

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `GstNvEventType` | `RdsEventType` | supported | cast `GST_EVENT_TYPE (ev)` to it before a `switch` or `==`, as on DeepStream | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `GST_NVEVENT_PAD_ADDED` | `RDS_EVENT_PAD_ADDED` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `GST_NVEVENT_PAD_DELETED` | `RDS_EVENT_PAD_DELETED` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `GST_NVEVENT_STREAM_EOS` | `RDS_EVENT_STREAM_EOS` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `GST_NVEVENT_STREAM_SEGMENT` | `RDS_EVENT_STREAM_SEGMENT` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `GST_NVEVENT_STREAM_RESET` | `RDS_EVENT_STREAM_RESET` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `GST_NVEVENT_STREAM_START` | `RDS_EVENT_STREAM_START` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_new_pad_added` | `rds_event_new_pad_added` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_new_pad_deleted` | `rds_event_new_pad_deleted` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_new_stream_eos` | `rds_event_new_stream_eos` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_new_stream_segment` | `rds_event_new_stream_segment` | supported | takes `const GstSegment *`, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_new_stream_reset` | `rds_event_new_stream_reset` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_new_stream_start` | `rds_event_new_stream_start` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_parse_pad_added` | `rds_event_parse_pad_added` | supported | returns `gboolean` (DeepStream: `void`); calls compile unchanged, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_parse_pad_deleted` | `rds_event_parse_pad_deleted` | supported | returns `gboolean` (DeepStream: `void`); calls compile unchanged, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_parse_stream_eos` | `rds_event_parse_stream_eos` | supported | returns `gboolean` (DeepStream: `void`); calls compile unchanged, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_parse_stream_segment` | `rds_event_parse_stream_segment` | supported | returns `gboolean` (DeepStream: `void`); calls compile unchanged, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_parse_stream_reset` | `rds_event_parse_stream_reset` | supported | returns `gboolean` (DeepStream: `void`); calls compile unchanged, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |
| `gst_nvevent_parse_stream_start` | `rds_event_parse_stream_start` | supported | returns `gboolean` (DeepStream: `void`); calls compile unchanged, but assigning it to a pointer with DeepStream's exact prototype does not compile | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gst-nvevent_8h.html) |

Not aliased: `GST_NVEVENT_UPDATE_CAPS` and `gst_nvevent_new_update_caps` (no Redstream counterpart yet).

## nvinfer custom parser ABI (nvdsinfer.h, nvdsinfer_custom_impl.h)

`rds/rds-infer.h` has the layer, network and object types (C and C++); `rds/rds-infer-custom-impl.h` has the parser
ABI (C++). Their fields keep NVIDIA's documented names and order, so a DeepStream bbox parser recompiles unchanged.
`rdsinfer` loads the parser named by `custom-lib-path` and `parse-bbox-func-name` and calls it on every inferred frame.

`rds/rds_compat.h` (C and C++) aliases only the C-compatible `NvDsInfer*` types below. The parser ABI names are in
`rds/rds_compat_infer.h` (C++), the replacement for `nvdsinfer_custom_impl.h`, which also includes `rds_compat.h`:

```cpp
#ifdef USE_REDSTREAM
#include <rds/rds_compat_infer.h>
#else
#include "nvdsinfer_custom_impl.h"
#endif
```

- **Build parsers in the Redstream toolchain** (libstdc++, `_GLIBCXX_USE_CXX11_ABI=1`, never `-static-libstdc++`):
  the ABI passes `std::vector`.
- Including the parser header (`rds-infer-custom-impl.h` or `rds_compat_infer.h`) defines a weak
  `rds_infer_parser_abi_version()` in the library; include it only in parser libraries. `rdsinfer` checks it when
  it loads `custom-lib-path`: a library without its own copy (one from a library it depends on does not count), or
  with another version, fails to start with a message naming the library. The symbol is exported even with
  `-fvisibility=hidden` (the parse functions then need `__attribute__ ((visibility ("default")))`); with a version
  script or `-Wl,--exclude-libs`, export it explicitly.
- The library is loaded with `RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE`: it is never unloaded (GStreamer never unloads
  plugins either), so HIP code objects, GTypes and threads in it stay valid.
- A parse function that returns `false` or throws: that image gets no detections and `rdsinfer` posts a warning
  (nothing is documented for this on DeepStream). It must not call `exit()` or `abort()`.
- The parse function is called from one thread at a time per loaded parser. `outputLayersInfo` and its buffers are
  valid only during the call. `buffer` is host memory; `deviceBuffer` (ours) is the same layer's data on the
  element's GPU (`gpu-id`); both are complete (synchronised) when the call starts.
- The unprefixed `FLOAT`, `HALF`, `INT8`, `INT32`, `INT64` are C++ only (`constexpr` constants of
  `RdsInferDataType`) and come only with `rds_compat_infer.h`; in C, use `RDS_INFER_DATA_*`.

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NVDSINFER_MAX_DIMS` | `RDS_INFER_MAX_DIMS` | supported | 8 | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `NvDsInferDims` | `RdsInferDims` | supported | per image: no batch dimension; `numElements` is the per-image element count | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferDims.html) |
| `NvDsInferDimsCHW` | `RdsInferDimsCHW` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferDimsCHW.html) |
| `NvDsInferDataType` | `RdsInferDataType` | supported | `RDS_INFER_DATA_FLOAT` 0, `_HALF` 1, `_INT8` 2, `_INT32` 3, `_INT64` 4 | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `FLOAT`, `HALF`, `INT8`, `INT32`, `INT64` | `RDS_INFER_DATA_FLOAT`, ... | supported | C++ only, `rds_compat_infer.h`, see above | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `NvDsInferLayerInfo` | `RdsInferLayerInfo` | supported | `buffer` is host memory; ours, appended: `deviceBuffer`, the same data on the GPU (or NULL). `bindingIndex` is the layer's index among the program's outputs | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferLayerInfo.html) |
| `NvDsInferNetworkInfo` | `RdsInferNetworkInfo` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferNetworkInfo.html) |
| `NvDsInferParseDetectionParams` | `RdsInferParseDetectionParams` | supported | C++ only, `rds_compat_infer.h`; with the deprecated `perClassThreshold` reference to `perClassPreclusterThreshold`; ours: a copy constructor and assignment that keep it bound to the copy's own thresholds | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferParseDetectionParams.html) |
| `NvDsInferObjectDetectionInfo` | `RdsInferObjectDetectionInfo` | supported | the DeepStream 9 layout, with `rotation_angle` (28 bytes); a non-zero angle is ignored with a warning. Coordinates in network-input pixels | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/9.1/sdk-api/structNvDsInferObjectDetectionInfo.html) |
| `NvDsInferParseObjectInfo` | `RdsInferParseObjectInfo` | supported | the same type as `RdsInferObjectDetectionInfo` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `NvDsInferParseCustomFunc` | `RdsInferParseCustomFunc` | supported | C++ only, `rds_compat_infer.h` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer__custom__impl_8h.html) |
| `CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE` | `RDS_CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE` | supported | C++ only, `rds_compat_infer.h`; a mismatched signature fails to compile | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer__custom__impl_8h.html) |

Not aliased (no Redstream counterpart yet, or none on ROCm):
- the deprecated `dims` member of `NvDsInferLayerInfo`'s union (not in the public docs), `getDimsCHWFromDims`,
  `getDimsHWCFromDims`;
- `NvDsInferInstanceMaskInfo`, `NvDsInferAttribute`, and the instance-mask, classifier, semantic-segmentation and
  engine-create parser types (`NvDsInferInstanceMaskParseCustomFunc`, `NvDsInferClassiferParseCustomFunc`,
  `NvDsInferSemSegmentationParseCustomFunc`, `NvDsInferEngineCreateCustomFunc`) with their check macros;
  `NvDsInferInitializeInputLayers`; the TensorRT-specific functions (`NvDsInferCreateModelParser`,
  `NvDsInferPluginFactoryRuntimeGet` / `Destroy`, `NvDsInferCudaEngineGet`);
- `NvDsInferStatus` and its `NVDSINFER_*` values, `NvDsInferStatus2Str`, `NvDsInferLogLevel`;
- the `nvdsinfer_context.h` names: `NvDsInferNetworkMode`, `NvDsInferNetworkType`, `NvDsInferFormat`,
  `NvDsInferTensorOrder`, `NvDsInferClusterMode`, `NvDsInferDetectionParams`, `_NvDsInferContextInitParams`,
  `NVDSINFER_MAX_BATCH_SIZE`, `NVDSINFER_MAX_CHANNELS`, `NVDSINFER_PATH_MAX`, `NVDSINFER_MAX_STR_LENGTH` (the
  element reads the same values from its config file).
