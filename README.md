# Redstream

**GStreamer video and audio analytics on AMD GPUs: an open-source, ROCm-based functional equivalent of NVIDIA DeepStream.**

> Status: **pre-alpha.** The first milestone's pipeline runs end to end on an AMD GPU: decode → batch → detect →
> track → analytics → tile → on-screen display → window, from files or RTSP streams, with the core library
> `libgstrds`, twelve elements (`rdsurisrcbin`, `rdsvideodec`, `rdsvideoconvert`, `rdsstreammux`, `rdsstreamdemux`,
> `rdsinfer`, `rdstracker`, `rdsanalytics`, `rdsmultistreamtiler`, `rdsosd`, `rdslogger`, `rdsvideosink`) and the
> `rds-compile` tool (see "What works" below). APIs and element names will change.

![A street clip with detection boxes, class labels and track IDs drawn by rdsosd](docs/media/demo.gif)

*A 720p street clip (CC0, Wikimedia Commons) through `rdsvideodec` → `rdsstreammux` → `rdsinfer` (YOLO11n) →
`rdstracker` (NvSORT) → `rdsosd` → `rdsvideosink` on a Ryzen 7000 iGPU; the command is under "See it" below.*

## Why

NVIDIA DeepStream is the de-facto toolkit for multi-stream video analytics pipelines, but it only runs
on NVIDIA hardware. AMD GPUs and APUs have capable video engines (VCN) and a maturing compute stack (ROCm,
HIP, MIGraphX), but there is no equivalent end-to-end analytics framework.

Upstream GStreamer has recently gained many of the building blocks: VA-API decode, a HIP plugin,
analytics metadata, tensor decoders and batching. What's missing is the AMD-specific glue
(zero-copy decode→GPU, GPU-resident batching, a MIGraphX inference element) and a DeepStream-compatible
surface so existing applications can move over without a rewrite.

## Goals

- **Functional parity** with the core of DeepStream: multi-stream decode, batching, primary and secondary
  inference, tracking, on-screen display, analytics, encode, and audio inference.
- **Easy porting.** An existing DeepStream app should port with only small, mechanical changes:
  - element names swapped behind an `#ifdef`
  - the same `nvinfer` / `nvstreammux` / tracker config files
  - the same metadata-traversal code through a compatibility header
- **GPU-resident pipelines.** Frames stay in GPU memory from decode to inference.
- **Upstream-friendly.** Built on GStreamer 1.28+ and GstAnalytics. Generic pieces are contributed upstream.

Non-goals (for now): binary compatibility with DeepStream, Triton (`nvinferserver`), TAO encrypted models,
Jetson-specific hardware blocks.

## What works

- **`libgstrds`:** batched GPU surfaces (`RdsBufSurface`, the `memory:RDS` caps feature), buffer pools,
  DeepStream-shaped metadata (batch, frame, object, user and display meta) and stream events.
- **`rds_buf_surf_transform`:** batched crop, scale (nearest, bilinear) and colour conversion between NV12, I420,
  RGBA, BGRA, RGBx, BGRx, RGB, BGR and GRAY8 on the GPU, shaped like `NvBufSurfTransform`; and
  `rds_buf_surf_transform_composite`, many surfaces into one (`NvBufSurfTransformComposite`).
- **`rdsurisrcbin`** (`nvurisrcbin`): a source bin for file and RTSP URIs, decoded with `rdsvideodec` to
  `memory:RDS` NV12 on `vsrc_0`, with `nvurisrcbin`'s properties: `file-loop`, `drop-frame-interval`, RTSP latency
  and transport, and reconnection (`rtsp-reconnect-interval`, `rtsp-reconnect-attempts`,
  `init-rtsp-reconnect-interval`). An RTSP source that stalls, drops the connection or refuses it is reconnected
  without ending the stream (same pad, running time going on), also when it comes back at another size or in another
  codec; the reconnection messages are DeepStream 8.0's, plus a few of ours. Tested against gst-rtsp-server (with a
  fault relay), not yet against real cameras.
- **`rdsvideodec`** (`nvv4l2decoder`): H.264 and H.265 (8-bit 4:2:0) decoding with rocDecode into `memory:RDS` NV12,
  bit-exact with the stock VA decoders. It ranks above them, so `uridecodebin` picks it; it falls back to system
  memory for ordinary sinks.
- **`rdsvideoconvert`** (`nvvideoconvert`): conversion, scaling and cropping between `memory:RDS` and system memory,
  with `nvvideoconvert`'s property names.
- **`rdsstreammux`** (`nvstreammux`, the default mux): batches frames from any number of sources into one
  `memory:RDS` buffer, each frame scaled into a fixed `width`×`height` slot (optionally with aspect-ratio padding),
  with per-frame metadata, `batched-push-timeout`, live sources, DeepStream's stream events (pad added/deleted,
  per-stream EOS, …) and sources added or removed while playing. The zero-copy "new" mux and its config file are not
  implemented yet.
- **`rdsstreamdemux`** (`nvstreamdemux`): splits a batch back into per-source streams without copying: each output
  buffer is a read-only view of its slot (a writer gets its own copy), with the source's own timestamps, segment and
  stream events.
- **`rdsinfer`** (`nvinfer`): a primary or secondary detector, or a secondary classifier, on MIGraphX. It reads
  nvinfer config files (key file or YAML), runs ONNX models (YOLO11n tested) on whole batches on the GPU, and
  attaches object meta and, optionally, tensor output meta. Bounding boxes come from the built-in YOLO parser or from
  a DeepStream custom bbox parser library, recompiled against `rds/rds_compat_infer.h`. Secondary mode
  (`process-mode=2`) runs detectors, classifiers (classifier meta, with the built-in argmax parser or a DeepStream
  custom classifier parser) and tensor-output models on the crops of objects that upstream elements found.
  A compiled MIGraphX program has a fixed batch, so a partial batch normally costs as much as a full one. With
  `dynamic-batch=1` (ours, off by default) `rdsinfer` also compiles programs for the powers of two below
  `batch-size` and runs each batch or chunk on the smallest that holds it: YOLO11n with one frame in a
  `batch-size` 4 pipeline takes about 31 ms instead of 95 ms. It costs one compile per size on a cold cache and about
  80–115 MB of GPU memory per extra program. In primary mode the highest occupied slot decides, not the number of
  frames: one frame in slot 3 still runs on the batch-4 program.
- **`rdstracker`** (`nvtracker`): gives detected objects persistent IDs with the IOU and NvSORT trackers of
  NvMultiObjectTracker, on the CPU, reading the same YAML low-level config files (NvDCF and NvDeepSORT configs run
  as NvSORT). Where NVIDIA's docs leave behaviour open, it follows observed DeepStream 8.0 behaviour, and the compat
  matrix lists every point where it differs. Our own tuned configs are in `samples/tracker/`.
- **`rdsanalytics`** (`nvdsanalytics`): ROI filtering, overcrowding, direction (with the stopped state) and line
  crossing on the tracked objects, on the CPU, from nvdsanalytics config files, with DeepStream-shaped frame and
  object metas and the `osd-mode` overlays as display meta (see "Analytics" below).
- **`rdsmultistreamtiler`** (`nvmultistreamtiler`): composes a batch into one rows × columns frame on the GPU and
  merges the sources' metadata into it, scaled into the tiles, as DeepStream 8.0 does; `show-source` shows one
  source alone.
- **`rdsosd`** (`nvdsosd`): draws object boxes and labels, and display meta (rectangles, text, lines, arrows,
  circles) and the clock, in the geometry of DeepStream 8.0's GPU mode. It draws on the GPU by default, so frames
  never leave it, or on the CPU with `process-mode=0`, the same pixels either way (Pango renders the text). Masks
  and blur are not drawn yet.
- **`rdslogger`** (`nvdslogger`): prints each stream's frame rate in DeepStream's `**PERF` line format.
- **`rdsvideosink`** (`nveglglessink`, `nv3dsink`): shows frames in a window through a stock GStreamer sink
  (`autovideosink` by default), with the DeepStream sinks' property names.
- **`rds-compile`** (`trtexec`): compiles and caches `rdsinfer`'s MIGraphX program ahead of time.
- **Compat header** `rds/rds_compat.h`: DeepStream names for the above, plus runtime remapping of element names
  (`nvurisrcbin` → `rdsurisrcbin`, `nvv4l2decoder` → `rdsvideodec`, `nvstreammux` → `rdsstreammux`, `nvinfer` →
  `rdsinfer`, `nvtracker` → `rdstracker`, `nvdsanalytics` → `rdsanalytics`, `nvdsosd` → `rdsosd`, …).

[`docs/compat-matrix.md`](docs/compat-matrix.md) lists every mapped name and property with its status. Built from
this repository (see "Getting started"; `tests/media/make-test-media.sh` generates the clips), a decode and convert
pipeline looks like this:

```bash
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA,width=640,height=640" ! fakesink sync=false
```

Two sources batched on the GPU and split again, one `queue` per output branch:

```bash
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 rdsstreammux name=m batch-size=2 width=1280 height=720 \
  ! rdsstreamdemux name=d \
  filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec ! m.sink_0 \
  filesrc location=.cache/media/smpte-1366x768.h264 ! h264parse ! rdsvideodec ! m.sink_1 \
  d.src_0 ! queue ! fakesink sync=false  d.src_1 ! queue ! fakesink sync=false
```

Two sources batched and run through YOLO11n. Fetch the model first (see "Getting started"); the first run compiles
it for batch 2, which takes a minute or two on a small GPU, and later runs load the cached program.
`GST_DEBUG=rdsinfer:5` logs the number of objects in each frame:

```bash
GST_PLUGIN_PATH_1_0=build/gst GST_DEBUG=rdsinfer:5 gst-launch-1.0 rdsstreammux name=m batch-size=2 width=1280 \
  height=720 ! rdsinfer config-file-path=samples/rds-test3/rds_test3_pgie_config.yml batch-size=2 ! fakesink sync=false \
  filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec ! m.sink_0 \
  filesrc location=.cache/media/smpte-1366x768.h264 ! h264parse ! rdsvideodec ! m.sink_1
```

The generated test clips (a bouncing ball, SMPTE bars) contain nothing YOLO11n detects, so expect 0 objects per
frame: this checks that the pipeline runs, not what it finds.

### See it

Fetch YOLO11n and the street clip (see "Getting started"). The `rds-test3` sample is shaped like DeepStream's
`deepstream-test3`: N sources → `rdsstreammux` → `rdsinfer` → `rdstracker` → `rdsmultistreamtiler` →
`rdsvideoconvert` → `rdsosd` → `rdsvideosink`. It prints each source's object counts per frame and shows the sources
tiled, with boxes and track IDs, in a window:

```bash
GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test3/rds-test3 \
  --tracker-config samples/tracker/config_tracker_NvSORT.yml \
  file://$PWD/.cache/media/crosswalk-720p.h264 file://$PWD/.cache/media/crosswalk-720p.h264
```

`--no-display` runs to a fakesink as fast as it can; `GST_DEBUG=rdstracker:5` adds the tracker's per-frame log.
From the dev container, a window needs the X11 socket and credentials; add these to `docker run`:

```bash
-e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix -e XAUTHORITY=/tmp/.xauth -v $XAUTHORITY:/tmp/.xauth:ro
```

One source through the whole chain with `gst-launch-1.0` (`sync=false`: a raw `.h264` file has no timestamps, so a
synchronizing sink would drop most frames):

```bash
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/crosswalk-720p.h264 ! h264parse \
  ! rdsvideodec ! m.sink_0 rdsstreammux name=m batch-size=1 width=1280 height=720 \
  ! rdsinfer config-file-path=samples/rds-test3/rds_test3_pgie_config.yml batch-size=1 \
  ! rdstracker ll-config-file=samples/tracker/config_tracker_NvSORT.yml \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd ! rdsvideosink sync=false
```

On that clip (56 s of a busy crosswalk, YOLO11n detections) the NvSORT sample config keeps 98.8 % of the confident
detections tracked, with 4 ID switches over 283 tracks; the tracker itself takes about 0.01 ms per batch on the CPU.
Four copies of the clip through the whole `rds-test3` chain run at about 44–46 fps on the iGPU below (`--no-display`):
the detector sets the pace, and the tiler and OSD add about 2.5 ms per batch (2.1 ms tiling, 0.4 ms drawing; release
build).

`--analytics-config FILE` adds `rdsanalytics` after the tracker (DeepStream's `nvdsanalytics`, with its config
format), and each count line then ends with the frame's ROI counts and the line crossings counted so far:

```bash
GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test3/rds-test3 \
  --tracker-config samples/tracker/config_tracker_NvSORT.yml \
  --analytics-config samples/rds-test3/rds_test3_analytics_config.txt \
  file://$PWD/.cache/media/crosswalk-720p.h264
# source 0 frame 499: objects 20 (person: 12, cars: 2) roi Crosswalk=6 lc Eastbound=14
```

The sample config, written for the street clip (the same rules for sources 0 to 3), has an ROI over the middle of the
near street counting people, a vertical line in the middle of the frame counting people and cars going east, and a
direction rule for people going west; `osd-mode=2` draws the rules, the counts and each object's status. On the clip,
29 objects cross the line, and the ROI has people on 1325 of the 1356 frames.

The `rds-test2` sample adds a secondary classifier, shaped like DeepStream's `deepstream-test2`: after the tracker,
YOLO11n-cls (ImageNet classes) runs in `rdsinfer`'s secondary mode on the cars, motorcycles, buses and trucks of at
least 64×64 pixels, and its label is appended to each vehicle's text (ImageNet has no plain "car" class: expect
labels such as `minivan` or `sports_car`). Each tracked vehicle is classified once and keeps its label while it is
tracked (inferred again only when its box grows by more than 1.2 times), as DeepStream does. It prints each source's
object, vehicle and labelled counts per frame, then the most frequent labels. It needs YOLO11n-cls as well (see
"Getting started"):

```bash
GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test2/rds-test2 \
  file://$PWD/.cache/media/crosswalk-720p.h264 file://$PWD/.cache/media/crosswalk-720p.h264
```

With four copies of the clip (four URIs) it runs at about 39 fps (`--no-display`, release build). The classifier runs
on about one batch in ten: of the 9576 times a vehicle was selected (once per vehicle and frame), 608 were inferred
and the rest, 93.7 %, were served from the cache. Classifying every vehicle on every frame
(`secondary-reinfer-interval: 0`) slows the sample to about 32 fps.

### Analytics

`rdsanalytics` (`nvdsanalytics`) goes after the tracker. It reads nvdsanalytics config files
(`[property]` with `config-width` and `config-height`, then `[roi-filtering-stream-<n>]`, `[overcrowding-stream-<n>]`,
`[direction-detection-stream-<n>]` and `[line-crossing-stream-<n>]` groups, `<n>` the stream's mux pad), scales the
rules to the mux output and, per frame, attaches the frame meta (`objInROIcnt`, `ocStatus`, `objLCCurrCnt`,
`objLCCumCnt`, `objCnt`) and, on each object with something to report, the object meta (`roiStatus`, `ocStatus`,
`lcStatus`, `dirStatus`, `objStatus`), as user metas of DeepStream 8.0's types 32 and 33. `osd-mode` 1 and 2 add
the rules, counts and statuses as display meta for `rdsosd`.

The metas are DeepStream's C++ structs, with the same field names and types, in `rds/rds-analytics-meta.h`
(library `redstream-analytics-1.0`, which replaces `nvds_analytics_meta.h`), so C++ code reads them as on
DeepStream (`m->objInROIcnt["RF"]`); C code uses its C API (`rds_analytics_frame_meta_get_roi_count`,
`rds_analytics_frame_meta_foreach_lc_cum_count`, `rds_analytics_obj_info_get_obj_status`, …).

Where the docs are silent it follows observed DeepStream 8.0 behaviour, except where 8.0 is clearly broken: objects
without a tracker id no longer turn off direction and line crossing for their stream, a config that 8.0 would crash
on or misread (empty values, short lines, `config-width=0`, duplicate labels, …) fails at start naming the key, and
stream resets clear a stream's state. The compat matrix lists every such choice.

```bash
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/crosswalk-720p.h264 ! h264parse \
  ! rdsvideodec ! m.sink_0 rdsstreammux name=m batch-size=1 width=1280 height=720 \
  ! rdsinfer config-file-path=samples/rds-test3/rds_test3_pgie_config.yml batch-size=1 \
  ! rdstracker ll-config-file=samples/tracker/config_tracker_NvSORT.yml \
  ! rdsanalytics config-file=samples/rds-test3/rds_test3_analytics_config.txt \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd ! rdsvideosink sync=false
```

## How it maps to DeepStream

| DeepStream | Redstream | Status |
|---|---|---|
| `nvv4l2decoder` | `rdsvideodec` (rocDecode) | works |
| `nvurisrcbin` | `rdsurisrcbin` | works (file and RTSP URIs, RTSP reconnection) |
| `nvvideoconvert` | `rdsvideoconvert` | works |
| `nvstreammux` / `nvstreamdemux` | `rdsstreammux` / `rdsstreamdemux` | works (video; the default, scaling mux. DeepStream's zero-copy "new mux" mode is planned) |
| `nvinfer` | `rdsinfer` (MIGraphX) | works (primary and secondary detectors, secondary classifiers, tensor output; primary classifiers and segmentation planned) |
| `nvtracker` | `rdstracker` | works (IOU, NvSORT) |
| `nvdsosd` | `rdsosd` | works |
| `nvmultistreamtiler` | `rdsmultistreamtiler` | works |
| `nvdsanalytics` | `rdsanalytics` | works (ROI filtering, overcrowding, direction, line crossing) |
| `nveglglessink`, `nv3dsink` | `rdsvideosink` | works |
| `nvdslogger` | `rdslogger` | works |
| `nvv4l2h264enc`, `nvv4l2h265enc` | `rdsh264enc`, `rdsh265enc` | planned |
| Audio batching in `nvstreammux` / `nvstreamdemux` | audio in `rdsstreammux` / `rdsstreamdemux` | on request |
| `nvinferaudio` | `rdsinferaudio` (MIGraphX) | on request |
| `nvdsaudiotemplate` | `rdsaudiotemplate` | on request |
| `NvDsBatchMeta`, `NvDsObjectMeta`, … | `Rds*` + `rds/rds_compat.h` | works |
| TensorRT `.engine` | MIGraphX `.mxr` (compiled from ONNX, cached) | works |

## Roadmap

**M1: multi-stream detector**
- File/RTSP sources → rocDecode decode (`rdsvideodec`) → `rdsstreammux` → `rdsinfer` (YOLO + custom bbox parser) → `rdstracker` → `rdsosd`.
- Done: the pipeline runs end to end from files and RTSP streams (with reconnection; tested against gst-rtsp-server),
  with tiling, on-screen display and a display sink. Left: the ported `deepstream-test3`.
- Acceptance test: NVIDIA's `deepstream-test3` sample, ported with `#ifdef`s only, produces equivalent detections.

**M2: production-style multi-model pipeline**
- Done: secondary inference (`process-mode=2`): detectors, classifiers and tensor-output models on object crops, with
  DeepStream's per-object classifier cache and the `rds-test2` sample.
- Done: `rdsanalytics` (ROI filtering, overcrowding, direction and line crossing), with `rds-test3
  --analytics-config`.
- Next: sliced inference (SAHI-style tiles of large frames, reusing the secondary-mode crops), then the H.264 encoder
  for a recording branch.
- Pose detector with keypoints.
- Metadata edits in pad probes.

**On request: audio analytics**

DeepStream dropped its audio support (8.0 points to NVIDIA Riva; 9.2 removed the audio plugins), so these come
only if users ask for them:
- Audio streams batched by `rdsstreammux` and split again by `rdsstreamdemux`, as DeepStream's new mux does.
- `rdsinferaudio` (`nvinferaudio`): audio classification on MIGraphX, with the log-mel spectrogram front end on the
  GPU, and the same config files.
- The audio batch buffer and its metadata (frame and classifier meta), and `rdsaudiotemplate` for custom audio
  processing libraries.

## Hardware

Only one GPU has been tested so far: the RDNA2 iGPU of a Ryzen 7000 desktop CPU (gfx1036). Other ROCm-capable GPUs
with a VCN video engine (Radeon RDNA2/3/4, Ryzen AI APUs such as Strix Point and Strix Halo) are expected to work,
but are untested; reports, good or bad, are welcome in the issues. Some Instinct SKUs lack video decode engines.

Early numbers on a 2-CU Ryzen 9 7900X iGPU:
- VA-API H.264 1080p decode: about 600 fps.
- YOLO11n 640×640 FP16 via MIGraphX: about 60 inferences/s.
- `rdsinfer` with YOLO11n FP32, four 720p sources at batch 4 (decode, batching, inference and post-processing):
  about 50 fps; about 48 fps with `rdstracker` (NvSORT) after it; about 44–46 fps through the whole `rds-test3`
  chain (tracker, tiler and OSD, to a fakesink); about 39 fps for `rds-test2` (YOLO11n-cls on the vehicles as well).
- YOLO11n-cls FP32 at 224×224, secondary mode with `batch-size` 16: about 26 ms per run with 4 crops and about 29 ms
  with 16 (the compiled program has a fixed batch, so fewer crops save almost nothing unless `dynamic-batch` is
  on).
- `rdsosd` at 1280×720 with about 60 objects, release build: about 0.3 ms per frame on the GPU (about 1.2 ms with
  `process-mode=0`, the CPU drawing and the GPU↔CPU syncs included). On 1920×1080 frames from `rdsstreamdemux`,
  which it draws on a private copy: about 0.9 ms on the GPU, about 7 ms on the CPU (the copy's CPU staging).

### Known issues

Both were seen with ROCm 10.1 on the gfx1036 iGPU, which runs as gfx1030 through `HSA_OVERRIDE_GFX_VERSION`, a
configuration AMD does not support. Whether officially supported GPUs are affected is not known yet.

- **SDMA copy faults.** With more HIP streams than hardware queues, which a multi-stream pipeline easily has, an
  SDMA copy can occasionally fault and abort the process with `Memory access fault by GPU ... on address 0x401000`.
  Set `HSA_ENABLE_SDMA=0` to have copies done by blit kernels instead; on the gfx1036 iGPU this costs nothing
  measurable (about 44 fps for `rds-test3` either way). Raising `GPU_MAX_HW_QUEUES` also avoids it but costs
  throughput (about 29 fps with 16 queues). The test suite sets `HSA_ENABLE_SDMA=0`.
- **`rdsosd` GPU faults under heavy host load.** When the host is busy (CPU and memory bandwidth), a kernel's writes
  now and then don't reach the next kernel on the same stream, with no error from HIP. Most elements then see wrong
  pixels at worst. `rdsosd`'s GPU drawing (`process-mode` 1 and 2, the default) can instead read a stale command
  block and fault the GPU: `Memory access fault` in `rds_osd_upload_kernel` or `rds_osd_draw_kernel`, then the
  process aborts. Under a synthetic load (parallel `dd bs=64M` and busy loops) the OSD tests failed in 30 of 38 runs;
  on an idle host they never failed. `HSA_DISABLE_CACHE=1` avoids it (0 of 80 runs) but costs about 23 % of
  `rds-test3`'s throughput, so only the test suite sets it. On affected systems, `process-mode=0` (drawing on the
  CPU) is not exposed.

## Getting started (development)

Requirements: Linux with the `amdgpu` kernel driver (`/dev/kfd` and `/dev/dri/renderD*` present), Docker.

```bash
docker build -f docker/Dockerfile.dev -t redstream:dev .
docker run -it --rm --device=/dev/kfd --device=/dev/dri \
  --group-add $(getent group render | cut -d: -f3) --group-add $(getent group video | cut -d: -f3) \
  --security-opt seccomp=unconfined -v "$PWD":/src -w /src redstream:dev

# inside the container: check that the GPU and the video engine are visible
rocminfo | grep gfx
vainfo --display drm --device /dev/dri/renderD128
```

Build and run the tests (inside the container):

```bash
meson setup build && meson test -C build --print-errorlogs
```

Then a first check with Redstream's own elements, on a clip generated with the VA encoder: decode with rocDecode and
convert to RGBA on the GPU.

```bash
tests/media/make-test-media.sh   # writes .cache/media/*.h264
GST_PLUGIN_PATH_1_0=build/gst gst-inspect-1.0 rdsvideodec
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA,width=640,height=640" ! fakesink sync=false
```

The YOLO11n tests and the detector example need the model, exported on your machine (on the host: it needs Docker
and network access). YOLO11n is AGPL-3.0, so it goes into the git-ignored `.cache/models/` and is never committed;
the tests that need it skip without it. See [`tests/models/README.md`](tests/models/README.md).

```bash
tests/models/fetch-yolo11n.sh
```

The tracker tests and the tracking example use a public-domain (CC0) street clip from Wikimedia Commons, transcoded
to 720p H.264 on your machine (on the host: it needs Docker, network access and the GPU). It goes into the
git-ignored `.cache/media/` and is never committed; the tests that need it skip without it.

```bash
tests/models/fetch-tracker-clip.sh
```

`rds-test2` and the classifier tests also need YOLO11n-cls (AGPL-3.0 too), exported the same way with an ImageNet
label file; then run the sample on the street clip (`--no-display` runs to a fakesink):

```bash
tests/models/fetch-yolo11n-cls.sh
GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test2/rds-test2 --no-display \
  file://$PWD/.cache/media/crosswalk-720p.h264
```

More tools from the build tree:

RTSP: `rds-rtsp-serve` serves a test pattern over RTSP, with a fault relay in front of it (here the connection is cut
10 s in); `rds-test3` reconnects to it (`rtsp-reconnect-interval` 2) and prints the reconnection as it goes. Only
RTSP over TCP goes through the relay, hence `--select-rtp-protocol 4`. `rds-test3`'s default detector is YOLO11n
(`tests/models/fetch-yolo11n.sh`). Ctrl-C stops `rds-test3`; `kill %1` then stops the server.

```bash
build/tools/rds-rtsp-serve/rds-rtsp-serve --proxy-port 8555 --faults 10:cut &   # rtsp://127.0.0.1:8555/test
GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test3/rds-test3 --no-display --rtsp-reconnect-interval 2 \
  --select-rtp-protocol 4 rtsp://127.0.0.1:8555/test rtsp://127.0.0.1:8554/test
kill %1
```

```bash
build/samples/rds-test3/rds-test3 --dry-run file:///a.mp4   # shows the DeepStream → Redstream element mapping
build/tools/rds-compile -c config.txt   # pre-builds rdsinfer's MIGraphX program, like trtexec (docs/compat-matrix.md)
```

The image targets RDNA2 APUs by default (`HSA_OVERRIDE_GFX_VERSION=10.3.0`, `--build-arg GFX=gfx1030`).
On other GPUs, pass your gfx target and unset the override.

## Contributing

Redstream is pre-alpha, and issues and pull requests are welcome: bug reports, results on GPUs we have not tested,
DeepStream behaviour we get wrong, and fixes or new elements. For anything large, open an issue first so the design
can be discussed there. [`CONTRIBUTING.md`](CONTRIBUTING.md) has the ground rules; if you use AI coding agents, read
[`AGENTS.md`](AGENTS.md) first.

This is a clean-room implementation: do not contribute code, headers or documentation copied from the
NVIDIA DeepStream SDK. Commits are signed off under the Developer Certificate of Origin; see
[`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

The libraries, plugins, tools and tests are licensed under the GNU Lesser General Public License, version 2.1 or
(at your option) any later version ([`LICENSE`](LICENSE)), as GStreamer is. Applications that use Redstream through
its shared libraries and plugins may have any licence; changes to Redstream itself that you distribute must be
published under the same terms.

The samples in [`samples/`](samples) (code and configs) are under the MIT licence ([`samples/LICENSE`](samples/LICENSE)),
so you can copy them into your own applications freely.

Each source file names its licence in an `SPDX-License-Identifier` line. Files without one (the test config fixtures)
are under the LGPL like the rest of the tree outside `samples/`.

## Trademarks

NVIDIA and DeepStream are trademarks of NVIDIA Corporation. AMD, ROCm and Radeon are trademarks of Advanced
Micro Devices, Inc. This project is not affiliated with or endorsed by either company.
