# Redstream

**GStreamer video and audio analytics on AMD GPUs: an open-source, ROCm-based functional equivalent of NVIDIA DeepStream.**

> Status: **pre-alpha.** The core library `libgstrds` and the first two elements, `rdsvideodec` and
> `rdsvideoconvert`, are in place (see "What works" below). APIs and element names will change.

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
  inference, tracking, on-screen display, analytics, encode.
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
  RGBA, BGRA, RGBx, BGRx, RGB, BGR and GRAY8 on the GPU, shaped like `NvBufSurfTransform`.
- **`rdsvideodec`** (`nvv4l2decoder`): H.264 and H.265 (8-bit 4:2:0) decoding with rocDecode into `memory:RDS` NV12,
  bit-exact with the stock VA decoders. It ranks above them, so `uridecodebin` picks it; it falls back to system
  memory for ordinary sinks.
- **`rdsvideoconvert`** (`nvvideoconvert`): conversion, scaling and cropping between `memory:RDS` and system memory,
  with `nvvideoconvert`'s property names.
- **Compat header** `rds/rds_compat.h`: DeepStream names for the above, plus runtime remapping of element names
  (`nvv4l2decoder` → `rdsvideodec`, …).

[`docs/compat-matrix.md`](docs/compat-matrix.md) lists every mapped name and property with its status. Built from
this repository (see "Getting started"; `tests/media/make-test-media.sh` generates the clip), a decode and convert
pipeline looks like this:

```bash
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA,width=640,height=640" ! fakesink sync=false
```

## How it maps to DeepStream

| DeepStream | Redstream |
|---|---|
| `nvv4l2decoder`, `nvurisrcbin` | `rdsvideodec` (rocDecode), `rdsurisrcbin` |
| `nvvideoconvert` | `rdsvideoconvert` |
| `nvstreammux` / `nvstreamdemux` | `rdsstreammux` / `rdsstreamdemux` |
| `nvinfer` | `rdsinfer` (MIGraphX) |
| `nvtracker` | `rdstracker` |
| `nvdsosd` | `rdsosd` |
| `nvmultistreamtiler` | `rdsmultistreamtiler` |
| `nvdsanalytics` | `rdsanalytics` |
| `nveglglessink`, `nv3dsink` | `rdsvideosink` |
| `nvdslogger` | `rdslogger` |
| `nvv4l2h264enc`, `nvv4l2h265enc` | `rdsh264enc`, `rdsh265enc` |
| `NvDsBatchMeta`, `NvDsObjectMeta`, … | `Rds*` + `rds/rds_compat.h` |
| TensorRT `.engine` | MIGraphX `.mxr` (compiled from ONNX, cached) |

## Roadmap

**M1: multi-stream detector**
- File/RTSP sources → rocDecode decode (`rdsvideodec`) → `rdsstreammux` → `rdsinfer` (YOLO + custom bbox parser) → `rdstracker` → `rdsosd`.
- Acceptance test: NVIDIA's `deepstream-test3` sample, ported with `#ifdef`s only, produces equivalent detections.

**M2: production-style multi-model pipeline**
- Pose detector with keypoints.
- Secondary models on object crops, with raw tensor output (ReID embeddings, attribute classifiers).
- Metadata edits in pad probes.
- ROI and line-crossing analytics, RTSP reconnect handling, and an H.264 recording branch.

## Hardware

Developed and tested on an RDNA2 APU (Ryzen 7000 iGPU, gfx1036). Any ROCm-capable GPU with a VCN video
engine should work: Radeon RDNA2/3/4, Ryzen AI APUs (Strix Point / Strix Halo). Some Instinct SKUs lack
video decode engines.

Early numbers on a 2-CU Ryzen 9 7900X iGPU:
- VA-API H.264 1080p decode: about 600 fps.
- YOLO11n 640×640 FP16 via MIGraphX: about 60 inferences/s.

## Getting started (development)

Requirements: Linux with the `amdgpu` kernel driver (`/dev/kfd` and `/dev/dri/renderD*` present), Docker.

```bash
docker build -f docker/Dockerfile.dev -t redstream:dev .
docker run -it --rm --device=/dev/kfd --device=/dev/dri \
  --group-add $(getent group render | cut -d: -f3) --group-add $(getent group video | cut -d: -f3) \
  --security-opt seccomp=unconfined -v "$PWD":/src -w /src redstream:dev

# inside the container
rocminfo | grep gfx
gst-inspect-1.0 hip
gst-launch-1.0 filesrc location=sample.mkv ! matroskademux ! h264parse ! vah264dec \
  ! hipupload ! hipconvertscale ! "video/x-raw(memory:HIPMemory),format=RGBA,width=640,height=640" \
  ! fakesink sync=false
```

Build and run the tests (inside the container):

```bash
meson setup build && meson test -C build --print-errorlogs
build/samples/rds-test3/rds-test3 --dry-run file:///a.mp4   # shows the DeepStream → Redstream element mapping
```

The image targets RDNA2 APUs by default (`HSA_OVERRIDE_GFX_VERSION=10.3.0`, `--build-arg GFX=gfx1030`).
On other GPUs, pass your gfx target and unset the override.

## Contributing

The project is in its design phase, so issues and design discussion are the most useful contributions right now.
Design discussion happens in GitHub issues. If you use AI coding agents, read [`AGENTS.md`](AGENTS.md) first.

This is a clean-room implementation: do not contribute code, headers or documentation copied from the
NVIDIA DeepStream SDK.

## License

TBD (expected: LGPL-2.1-or-later for plugins and libraries, matching GStreamer).

## Trademarks

NVIDIA and DeepStream are trademarks of NVIDIA Corporation. AMD, ROCm and Radeon are trademarks of Advanced
Micro Devices, Inc. This project is not affiliated with or endorsed by either company.
