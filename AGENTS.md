# AGENTS.md

Guidance for AI coding agents (Claude Code, Codex, Cursor, …) working in this repository.
`CLAUDE.md` is a symlink to this file. Edit `AGENTS.md`, never the symlink.

## What this project is

**Redstream** is an open-source set of GStreamer plugins and libraries that provide a **functional equivalent of NVIDIA
DeepStream on AMD GPUs (ROCm)**. Porting an existing DeepStream application should need only small,
mechanical changes: element factory names behind an `#ifdef`, the same config files, and the same
metadata-traversal code via a compatibility header.

Status: **pre-alpha, design phase.** Nothing here is stable.

## Architecture principles

1. **Upstream GStreamer first.** Reuse what GStreamer 1.28+ already ships: the `va` plugin (VCN decode/encode),
   the `hip` plugin (`hipupload`, `hipconvertscale`, …, plus `libgsthip`), GstAnalytics meta, tensor decoders.
   Build only the AMD-specific gaps, and upstream generic pieces when it makes sense.
2. **HIP device memory is our "NVMM".** Frames stay on the GPU from decode to inference. Batched surfaces
   follow the same model as NvBufSurface (a batch of planes with pitch/offset) so user CUDA kernels port with `hipify`.
3. **MIGraphX for inference.** ONNX → MIGraphX, compiled programs cached on disk (`.mxr`), like `nvinfer` caches
   TensorRT `.engine` files. (ONNX Runtime's ROCm EP was removed in ORT 1.23; MIGraphX is the supported path.)
4. **Config compatibility.** Our elements accept the same config files and property names as their DeepStream
   counterparts (`nvinfer` YAML/TXT, new-`nvstreammux` config, tracker YAML) wherever the semantics map.
5. **Metadata shim.** A DeepStream-shaped metadata API (batch → frame → object → user/display meta, pools,
   acquire/add/remove) under our own names, plus a compat header with aliases. Also bridged to
   `GstAnalyticsRelationMeta` for interop with upstream elements.

### Planned element mapping

| DeepStream | Redstream | Notes |
|---|---|---|
| `nvurisrcbin` / `nvv4l2decoder` | `rdsurisrcbin` / upstream `vah264dec`, `vah265dec` | RTSP reconnect lives in the bin |
| `nvvideoconvert` | upstream `hipconvertscale` | |
| `nvstreammux` / `nvstreamdemux` | `rdsstreammux` / `rdsstreamdemux` | reads new-mux config file |
| `nvinfer` | `rdsinfer` | primary + secondary, custom parser ABI, tensor meta |
| `nvtracker` | `rdstracker` | IOU/SORT-class first |
| `nvdsosd` | `rdsosd` | bbox, text, lines, circles |
| `nvdsanalytics` | `rdsanalytics` | ROI, line crossing, direction, overcrowding (M2) |
| `NvDsBatchMeta` & co. | `RdsBatchMeta` & co. + `rds/rds_compat.h` | |

Naming convention: elements `rds<name>`, types `Rds<Name>`, functions `rds_<name>`. This mirrors NVIDIA's `nv`/`NvDs`/`nvds_`, so ports are near-mechanical renames.

## MVP milestones

- **M1, multi-stream detector.** File/RTSP → VA decode → `rdsstreammux` → `rdsinfer` (YOLO primary detector with
  custom bbox parser) → `rdstracker` → `rdsosd` → sink/appsink. Acceptance: NVIDIA's `deepstream-test3`
  sample ported with `#ifdef`s only, producing equivalent detections to DeepStream on the same video and model.
- **M2, production-style multi-model app.** Pose primary detector (custom parser, keypoints), secondary models in
  `process-mode=2` with `output-tensor-meta` (ReID embeddings, attribute classifiers), metadata writes in probes
  (add/remove objects, edit rects), analytics (ROI / line crossing), RTSP reconnect events, H.264 recording branch.

## Hard rules

- **Clean-room.** Never copy NVIDIA DeepStream headers, sources, samples or docs into this repo. API
  compatibility means matching names and behaviour from public documentation, written independently.
  NVIDIA samples may be used *outside* the repo as porting test targets.
- **No references to private or downstream projects.** Do not mention, link or name any user's private
  applications, customers or internal codebases in code, docs, commits, tests or fixtures. Use generic
  descriptions ("a multi-camera people-analytics app") and public sample media/models.
- **Do not bundle models or large media.** Tests download public assets (e.g. Ultralytics/ONNX zoo models,
  public sample videos) into a git-ignored cache.
- Keep changes small and scoped; follow the existing style of the surrounding code.

## Development environment

All development happens in the dev container (ROCm 10.1 + MIGraphX + GStreamer 1.28 built from source):

```bash
docker build -f docker/Dockerfile.dev -t redstream:dev .
docker run -it --rm --device=/dev/kfd --device=/dev/dri \
  --group-add $(getent group render | cut -d: -f3) --group-add $(getent group video | cut -d: -f3) \
  --security-opt seccomp=unconfined -v "$PWD":/src -w /src redstream:dev
```

Known gotchas:

- `--group-add` needs **numeric host GIDs**; the image has no `render` group.
- RDNA2 APUs (e.g. gfx1036) need `HSA_OVERRIDE_GFX_VERSION=10.3.0` (set in the image). On officially
  supported GPUs, unset it.
- ROCm 10 packages are named `amdrocm-*` and split per gfx target. MIGraphX comes from a separate AMD repo
  (`amdrocm10-migraphx`). Libraries live in `/opt/rocm/core-10.1/lib` and `/opt/rocm/extras-10/lib`;
  `migraphx-driver` is in `/opt/rocm/extras-10/bin`.
- `analyticscombiner` / `analyticssplitter` are in gst-plugins-rs and are not built in the image yet.
- On machines with an NVIDIA GPU as well, VA-API may log `pci id for fd N: 10de:...`. This is harmless; the
  AMD render node is still picked.
- MIGraphX compilation is slow on small GPUs (minutes). Always cache compiled programs.

Useful checks:

```bash
rocminfo | grep -E '^\s+Name:\s+gfx'
vainfo --display drm --device /dev/dri/renderD128
gst-inspect-1.0 hip
migraphx-driver perf model.onnx --input-dim @images 1 3 640 640 --fp16
```

## Repository layout (planned)

```
docker/            dev and CI images
gst-libs/rds/    core library: batched surfaces, metadata, config parsers, compat headers
gst/               plugins: rdsstreammux, rdsinfer, rdstracker, rdsosd, …
compat/            nvds_compat.h and the custom-parser ABI header
samples/           ported sample apps (M1: test3-equivalent)
tests/             unit tests + pipeline tests against public media
docs/              user docs: porting.md, compat-matrix.md, benchmarks.md
.agents/skills/    agent skills for this repo (.claude/skills is a symlink)
```

## Agent skills

Repo-specific skills live in `.agents/skills/<name>/SKILL.md`; `.claude/skills` is a symlink to it.
Add a skill when a workflow is repeated often enough that agents keep rediscovering it.
