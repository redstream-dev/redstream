# AGENTS.md

Guidance for AI coding agents (Claude Code, Codex, Cursor, …) working in this repository.
`CLAUDE.md` is a symlink to this file. Edit `AGENTS.md`, never the symlink.

## What this project is

**Redstream** is an open-source set of GStreamer plugins and libraries that provide a **functional equivalent of NVIDIA
DeepStream on AMD GPUs (ROCm)**. Porting an existing DeepStream application should need only small,
mechanical changes: element factory names behind an `#ifdef`, the same config files, and the same
metadata-traversal code via a compatibility header.

Status: **pre-alpha.** Phase 0 spikes and the walking skeleton are done; the libgstrds core is in place (surfaces,
`memory:RDS`, pools, metadata, events, the `rds_buf_surf_transform` GPU transform). The first elements exist:
`rdsvideodec` (rocDecode H.264/H.265 decode) and `rdsvideoconvert` (convert, scale, crop). Nothing here is stable.

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
| `nvurisrcbin` | `rdsurisrcbin` | RTSP reconnect lives in the bin |
| `nvv4l2decoder` | `rdsvideodec` | rocDecode-backed: stock VA output is tiled on AMD, so it is not used for analytics |
| `nvvideoconvert` | `rdsvideoconvert` | convert, scale and crop on `memory:RDS`; copies stock VA input via a linear path |
| `nvmultistreamtiler` | `rdsmultistreamtiler` | |
| `nvstreammux` / `nvstreamdemux` | `rdsstreammux` / `rdsstreamdemux` | reads new-mux config file |
| `nvinfer` | `rdsinfer` | primary + secondary, custom parser ABI, tensor meta |
| `nvtracker` | `rdstracker` | IOU/SORT-class first |
| `nvdsosd` | `rdsosd` | bbox, text, lines, circles |
| `nvdsanalytics` | `rdsanalytics` | ROI, line crossing, direction, overcrowding (M2) |
| `nveglglessink` / `nv3dsink` | `rdsvideosink` | display sink |
| `nvdslogger` | `rdslogger` | per-stream FPS |
| `nvv4l2h264enc` / `nvv4l2h265enc` | `rdsh264enc` / `rdsh265enc` | VA (VCN) encode with DeepStream property names (M2) |
| `NvDsBatchMeta` & co. | `RdsBatchMeta` & co. + `rds/rds_compat.h` | |

The authoritative runtime mapping is the alias table in `gst-libs/rds/rds-compat-remap.c`.

Naming convention: elements `rds<name>`, types `Rds<Name>`, functions `rds_<name>`. This mirrors NVIDIA's `nv`/`NvDs`/`nvds_`, so ports are near-mechanical renames.

## MVP milestones

- **M1, multi-stream detector.** File/RTSP → rocDecode decode (`rdsvideodec`) → `rdsstreammux` → `rdsinfer` (YOLO primary detector with
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
- MIGraphX compilation is slow on small GPUs (minutes; FP16 even slower). Always cache compiled programs (`.mxr`).
- Build HIP code with the GPU attached, or pass `--offload-arch` explicitly. Otherwise hipcc targets the wrong arch
  and kernels fail with "device kernel image is invalid".
- rocDecode must be fed whole NAL units or access units (`h264parse ! ...,alignment=au`). Split NAL units decode
  incorrectly. `rdsvideodec`'s sink caps require `alignment=au`, so a parser in front negotiates it.
- rocDecode decode surfaces of 1 MiB or less (Mesa slab-allocates them) lose their first decoded picture unless they
  are exported to HIP before the first decode. `rdsvideodec` maps every surface right after creating the decoder
  and again after `rocDecReconfigureDecoder`; keep that if you touch decoder setup.
- Stock `vah26xdec` exports tiled NV12 DMABufs (AMD modifier `0x0200000000401b03`). Never read them as linear.
- Run docker with `--user $(id -u):$(id -g)` so build outputs aren't root-owned. From zsh, wrap `$RUN`-style
  variables in `bash -c` (zsh doesn't word-split them).
- Include GLib/GStreamer **before** HIP: HIP defines `__noinline__` as a macro, which breaks `gmacros.h`. Public
  headers never include HIP (device pointers are `gpointer`); only `rds-hip-private.h` does.
- `hipHostMalloc` (pinned) memory shows up in the process's VmRSS, not in `hipMemGetInfo`.

Useful checks:

```bash
rocminfo | grep -E '^\s+Name:\s+gfx'
vainfo --display drm --device /dev/dri/renderD128
gst-inspect-1.0 hip
migraphx-driver perf model.onnx --input-dim @images 1 3 640 640 --fp16
```

## Repository layout

```
docker/            dev image (Dockerfile.dev)
gst-libs/rds/      libgstrds-1.0 (umbrella header rds.h):
  rds-buf-surface    RdsBufSurface: system/device/pinned/unified memory, pinned staging mirror for CPU access
  rds-memory         memory:RDS allocator (map returns the RdsBufSurface)
  rds-buffer-pool    RdsBufferPool, rds_propose_allocation / rds_decide_allocation
  rds-meta*, rds-osd-types  batch/frame/object/user/display meta, pools, deep copy
  rds-gst-meta       RdsMeta GstMeta on buffers (gst_buffer_get_rds_batch_meta)
  rds-event          stream events (pad added/deleted, stream start/EOS/segment/reset)
  rds_compat.h       DeepStream compat header (+ rds-compat-aliases.h, rds-compat-remap.c)
  rds-transform      rds_buf_surf_transform: batched crop/scale/colour conversion (NvBufSurfTransform-shaped)
  rds-transform-kernels.hip  its HIP kernels (amdclang++, linked in as an object)
  *-private.h        internal headers, not installed (rds-hip-private.h is the only HIP include)
gst/rdsvideo/      libgstrdsvideo plugin: rdsvideodec (rocDecode, gstrdsvideodec.c), rdsvideoconvert
                   (gstrdsvideoconvert.c), shared helpers in rds-video-util
docs/              compat-matrix.md: what the compat header and the elements map, with status and sources
samples/rds-test3/ clean-room test3-style multi-stream sample (walking skeleton, has --dry-run)
tests/check/       GLib unit and element tests (suites cpu/gpu); test clips are encoded in-process
                   (rds-test-media.h, needs the VA encoder)      tests/samples/  sample-level tests
tests/media/       make-test-media.sh: generated H.264 clips (into git-ignored .cache/)
spikes/            Phase 0 experiments (rocdec-hip, migraphx-devptr); reference code, not built by meson
.agents/skills/    agent skills for this repo (.claude/skills is a symlink)
```

Planned next: more plugins in `gst/`, `docs/porting.md`, `docs/benchmarks.md`.

## Build and test

Inside the dev container:

```bash
meson setup build          # once; then `meson setup build --reconfigure` after meson.build changes
meson test -C build --print-errorlogs
meson test -C build --suite cpu  # GPU-free subset (gpu tests also skip themselves without a device);
                                 # building still needs the ROCm headers and libamdhip64
tests/media/make-test-media.sh   # generates .cache/media/*.h264 for manual runs, benchmarks and spikes
                                 # (the tests encode their own clips)
# run our elements from the build tree
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse \
  ! rdsvideodec ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA,width=640,height=640" ! fakesink sync=false

# ASan + UBSan build; tests/lsan.supp hides HIP runtime leaks
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson test -C build-asan --print-errorlogs
```

- Warnings are errors (`warning_level=2`, `werror`).
- HIP kernels (`*.hip`) are compiled by `amdclang++` in a meson `custom_target` for the targets in
  `-Doffload_arch=` (default `gfx10-3-generic,gfx11-generic,gfx12-generic`; e.g. `-Doffload_arch=gfx1030`), then
  linked into the C library as objects. A build for the wrong arch fails at kernel launch, not silently.
- With `-Drocm_path` other than `/opt/rocm`, the library and plugins get an install rpath to `<rocm_path>/lib`;
  `/opt/rocm/lib` is expected to be on the system linker path.
- Element tests load plugins from `build/gst` (`GST_PLUGIN_PATH_1_0`) next to the system plugins, with a private
  registry in `build/tests/registry.bin`. ASan builds scan plugins in-process (`GST_REGISTRY_FORK=no`), because
  `gst-plugin-scanner` can't load sanitized plugins. ASan runs with `use_sigaltstack=0` (Mesa's LLVM, loaded by the
  VA driver, installs its own alternate signal stack), and only the soak test (`pipeline_soak`, its own build of
  `pipeline_decode_convert.c`) shrinks the ASan quarantine so its VmRSS check stays meaningful. Tests that need private symbols link `rds_internal_dep`, a
  static copy of libgstrds. Such a test must never also link `rds_dep` or load one of our plugins in-process
  (the plugin pulls in the shared libgstrds): two copies of the library register the same GTypes twice. They run
  with `internal_test_env`: no plugin paths and their own registry, so even an in-process scan loads no plugin.
- Library symbols are hidden by default. Mark public API with `RDS_API` (from `rds/rds-prelude.h`).
- Put new tests next to the existing ones and register them in `tests/meson.build`, in the `cpu` or `gpu` suite.
- **Ready-on-push:** a producer synchronizes its GPU work before pushing a `memory:RDS` buffer; downstream never
  waits on a stream. Library copies and memsets are synchronous, on the calling thread's per-thread stream
  (`hipStreamPerThread`), so they never wait for unrelated work on the legacy null stream.
- Transform sessions are per thread and streaming threads are shared: elements set the transform session before
  each call and restore the previous one after (`rds_buf_surf_transform_get_session_params` first).
- `memory:RDS` is not `NO_SHARE`: shallow buffer copies share the surface. Pixel writers must own the memory
  exclusively (a WRITE map of shared memory gets a private deep copy).
- Dispatch our stream events on the cast type: `switch ((RdsEventType) GST_EVENT_TYPE (ev))`. An uncast switch
  triggers `-Wswitch`, as on DeepStream.
- Use TDD: write the failing test first.

## Agent skills

Repo-specific skills live in `.agents/skills/<name>/SKILL.md`; `.claude/skills` is a symlink to it.
Add a skill when a workflow is repeated often enough that agents keep rediscovering it.
