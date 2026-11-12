# Redstream

**GStreamer video and audio analytics on AMD GPUs: an open-source, ROCm-based functional equivalent of NVIDIA DeepStream.**

> Status: **pre-alpha.** The first milestone's pipeline runs end to end on an AMD GPU: decode → batch → detect →
> track → analytics → tile → on-screen display → window or encoded file, from files or RTSP streams, with the core
> library `libgstrds`, fourteen elements (`rdsurisrcbin`, `rdsvideodec`, `rdsvideoconvert`, `rdsstreammux`,
> `rdsstreamdemux`, `rdsinfer`, `rdstracker`, `rdsanalytics`, `rdsmultistreamtiler`, `rdsosd`, `rdslogger`,
> `rdsvideosink`, `rdsh264enc`, `rdsh265enc`) and the `rds-compile` tool (see "What works" below). APIs and element
> names will change.

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

Each element has a reference page in [`docs/elements/`](docs/elements/README.md): its pads, properties, metadata,
behaviour, how it differs from DeepStream 8.0, and its known issues.

- **Sources and decode.** [`rdsurisrcbin`](docs/elements/rdsurisrcbin.md) (`nvurisrcbin`) plays file and RTSP URIs, with
  `nvurisrcbin`'s properties; it reconnects RTSP sources without ending the stream, also when they come back at another
  size or in another codec (tested against gst-rtsp-server with a fault relay, not yet against real cameras).
  [`rdsvideodec`](docs/elements/rdsvideodec.md) (`nvv4l2decoder`) decodes H.264 and H.265 with rocDecode into
  `memory:RDS`, bit-exact with the VA decoders, and [`rdsvideoconvert`](docs/elements/rdsvideoconvert.md)
  (`nvvideoconvert`) converts, scales and crops on the GPU.
- **Batching.** [`rdsstreammux`](docs/elements/rdsstreammux.md) (`nvstreammux`, the default mux) batches any number
  of sources into fixed-size GPU slots, with DeepStream's stream events and sources added or removed while playing;
  [`rdsstreamdemux`](docs/elements/rdsstreamdemux.md) (`nvstreamdemux`) splits a batch again without copying.
- **Inference.** [`rdsinfer`](docs/elements/rdsinfer.md) (`nvinfer`) runs ONNX models on MIGraphX from nvinfer
  config files ([keys](docs/elements/rdsinfer-config.md)): primary and secondary detectors, secondary classifiers with
  the per-object cache, tensor output, and DeepStream custom parser libraries recompiled against
  `rds/rds_compat_infer.h`. It runs YOLO11n on four 720p sources at about 50 fps on a small iGPU (see
  [Hardware](#hardware)), and with `dynamic-batch` a partial batch costs less (one frame of four: about 31 ms
  instead of 95 ms). `rds-compile` (`trtexec`) compiles its programs ahead of time.
- **Tracking and analytics.** [`rdstracker`](docs/elements/rdstracker.md) (`nvtracker`) runs the IOU and NvSORT
  trackers on the CPU from NvMultiObjectTracker configs ([keys](docs/elements/rdstracker-config.md)); our tuned
  configs are in `samples/tracker/`. [`rdsanalytics`](docs/elements/rdsanalytics.md) (`nvdsanalytics`) does ROI
  filtering, overcrowding, direction and line crossing from nvdsanalytics configs
  ([keys](docs/elements/rdsanalytics-config.md)), with DeepStream-shaped metas.
- **Display and encode.** [`rdsmultistreamtiler`](docs/elements/rdsmultistreamtiler.md) (`nvmultistreamtiler`)
  tiles a batch into one frame; [`rdsosd`](docs/elements/rdsosd.md) (`nvdsosd`) draws boxes, labels, display meta
  and the clock, on the GPU by default; [`rdsvideosink`](docs/elements/rdsvideosink.md) (`nveglglessink`,
  `nv3dsink`) shows frames through a stock sink; [`rdslogger`](docs/elements/rdslogger.md) (`nvdslogger`) prints
  `**PERF` lines; [`rdsh264enc` and `rdsh265enc`](docs/elements/rdsh26xenc.md) (`nvv4l2h264enc`, `nvv4l2h265enc`)
  encode through GStreamer's VA encoders, with DeepStream 8.0's properties.
- **Libraries.** `libgstrds` has the batched GPU surfaces (`RdsBufSurface`, the `memory:RDS` caps feature), buffer
  pools, DeepStream-shaped metadata (batch, frame, object, user and display meta) and stream events.
  `rds_buf_surf_transform` does batched crop, scale (nearest, bilinear) and colour conversion between NV12, I420,
  RGBA, BGRA, RGBx, BGRx, RGB, BGR and GRAY8 on the GPU, shaped like `NvBufSurfTransform`, and
  `rds_buf_surf_transform_composite` composes many surfaces into one (`NvBufSurfTransformComposite`).
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
`--output FILE` writes the tiled, annotated frames to a file instead of a window: `.mp4` (`qtmux`) or `.mkv`
(`matroskamux`), through `rdsvideoconvert`, `rdsh264enc` and `h264parse`; `--codec h265` uses `rdsh265enc` and
`h265parse`. It wins over `--no-display`, and the file is finished at the end of the streams or on Ctrl-C:

```bash
GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test3/rds-test3 --output out.mp4 \
  --tracker-config samples/tracker/config_tracker_NvSORT.yml file://$PWD/.cache/media/crosswalk-720p.h264
```

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

`rdsanalytics` goes after the tracker. It reads nvdsanalytics config files, attaches DeepStream 8.0's frame and
object metas (user meta types 32 and 33; C++ code reads them as on DeepStream, C code through a C API), and with
`osd-mode` 1 or 2 adds the rules, counts and statuses as display meta for `rdsosd`. Where DeepStream 8.0 is clearly
broken it does the sane thing instead. [Its page](docs/elements/rdsanalytics.md) has the metas, the behaviour and
every such choice; [rdsanalytics-config](docs/elements/rdsanalytics-config.md) has the config keys.

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
| `nvv4l2h264enc`, `nvv4l2h265enc` | `rdsh264enc`, `rdsh265enc` | works (bins over GStreamer's VA-API encoders; frames go through system memory for now, see "Zero-copy and where it stops") |
| Audio batching in `nvstreammux` / `nvstreamdemux` | audio in `rdsstreammux` / `rdsstreamdemux` | on request |
| `nvinferaudio` | `rdsinferaudio` (MIGraphX) | on request |
| `nvdsaudiotemplate` | `rdsaudiotemplate` | on request |
| `NvDsBatchMeta`, `NvDsObjectMeta`, … | `Rds*` + `rds/rds_compat.h` | works |
| TensorRT `.engine` | MIGraphX `.mxr` (compiled from ONNX, cached) | works |

### Zero-copy and where it stops

From decode to the last analytics element, frames stay in GPU memory (`memory:RDS`): `rdsvideodec` decodes into
GPU surfaces, and batching, inference, tracking, analytics, tiling and on-screen display all work on them in place
or GPU to GPU. Frames leave the GPU only where a stock GStreamer element takes over:

- **Encoding (`rdsh264enc`, `rdsh265enc`).** These are bins over GStreamer's VA-API encoders (`vah264enc`,
  `vah265enc`). Those encoders take NV12 in system memory or AMD-tiled VA surfaces, but Redstream's GPU surfaces are
  linear, so the bins convert to NV12 on the GPU and copy each frame into plain system memory (on an APU the
  "system memory" is the same DRAM); the VA encoder then uploads it. On the Ryzen 7000 iGPU at 1080p the copy alone
  takes about 0.5 ms per frame (release build), the RGBA to NV12 conversion and the copy together about 1.4 ms (a
  rougher timing). The encode runs on a thread of its own, so it overlaps the next frame's conversion. A zero-copy
  path, with the GPU writing straight into VA surfaces, is still planned behind the same elements.
- **Display (`rdsvideosink`).** It converts on the GPU, then hands system-memory frames to a stock video sink.
- **`rdsosd process-mode=0`.** That mode draws on the CPU by design. The default mode draws on the GPU.

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
- Done: H.264 and H.265 encoding (`rdsh264enc`, `rdsh265enc`) for a recording branch, with `rds-test3 --output`.
- Next: sliced inference (SAHI-style tiles of large frames, reusing the secondary-mode crops).
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
- `rdsh264enc` / `rdsh265enc` at 1080p, 4 Mbit/s CBR, release build, `gst-launch-1.0` from `videotestsrc` uploaded
  to the GPU into a fakesink: about 205 fps H.264 and about 215 fps H.265, the VA encoder's own limit with its copy
  from system memory. With a parser after it (the 3000-frame soak test): 194–207 fps.

### Known issues

These two were seen with ROCm 10.1 on the gfx1036 iGPU, which runs as gfx1030 through `HSA_OVERRIDE_GFX_VERSION`, a
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
  on an idle host they never failed. `HSA_DISABLE_CACHE=1` avoids it (0 of 80 runs, with `HSA_ENABLE_SDMA=0` as
  well, as the test suite runs) but costs about 23 % of `rds-test3`'s throughput, so only the test suite sets it.
  On affected systems, `process-mode=0` (drawing on the CPU) doesn't have the fault.

Element-specific issues are listed on each element's page in [`docs/elements/`](docs/elements/README.md), under
"Known issues and limits" (the VA encoders' ones on [rdsh26xenc](docs/elements/rdsh26xenc.md)).

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
build/tools/rds-compile -c config.txt   # pre-builds rdsinfer's MIGraphX program, like trtexec (docs/elements)
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
