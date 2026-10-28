# AGENTS.md

Guidance for AI coding agents (Claude Code, Codex, Cursor, …) working in this repository.
`CLAUDE.md` is a symlink to this file. Edit `AGENTS.md`, never the symlink.

## What this project is

**Redstream** is an open-source set of GStreamer plugins and libraries that provide a **functional equivalent of NVIDIA
DeepStream on AMD GPUs (ROCm)**. Porting an existing DeepStream application should need only small,
mechanical changes: element factory names behind an `#ifdef`, the same config files, and the same
metadata-traversal code via a compatibility header.

Status: **pre-alpha.** M1's pipeline runs end to end from files (decode → mux → infer → tracker → tiler → OSD →
display, `samples/rds-test3`). The libgstrds core is in place (surfaces, `memory:RDS`, pools, metadata, events,
read-only surface views, the `rds_buf_surf_transform` GPU transform and its many-to-one composite). The elements so
far: `rdsvideodec` (rocDecode H.264/H.265 decode), `rdsvideoconvert` (convert, scale, crop),
`rdsstreammux` (the default scaling mux: batches of fixed-size slots), `rdsstreamdemux` (zero-copy per-source
views of the batch), `rdsinfer` (MIGraphX primary detector: nvinfer configs, custom bbox parsers, object and
tensor meta), `rdstracker` (the IOU and NvSORT trackers on the CPU, with NvMultiObjectTracker YAML configs),
`rdsmultistreamtiler` (batches into one tiled frame), `rdsosd` (boxes, text, lines, arrows, circles and the clock,
drawn on the GPU by default or on the CPU, the same pixels, Pango for text), `rdslogger` (per-stream FPS lines) and
`rdsvideosink` (display through a stock GStreamer sink). Nothing here is stable.

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
| `nvstreammux` / `nvstreamdemux` | `rdsstreammux` / `rdsstreamdemux` | default (scaling) mux done; the zero-copy new mux and its config file come later |
| `nvinfer` | `rdsinfer` | primary + secondary, custom parser ABI, tensor meta |
| `nvtracker` | `rdstracker` | IOU and NvSORT done (CPU); NvDCF / NvDeepSORT later |
| `nvdsosd` | `rdsosd` | bbox, text, lines, circles |
| `nvdsanalytics` | `rdsanalytics` | ROI, line crossing, direction, overcrowding (M2) |
| `nveglglessink` / `nv3dsink` | `rdsvideosink` | display sink |
| `nvdslogger` | `rdslogger` | per-stream FPS |
| `nvv4l2h264enc` / `nvv4l2h265enc` | `rdsh264enc` / `rdsh265enc` | VA (VCN) encode with DeepStream property names (M2) |
| `nvstreammux` / `nvstreamdemux` (audio) | `rdsstreammux` / `rdsstreamdemux` | later: batches of mono S16LE/F32LE audio, as the new mux does |
| `nvinferaudio` | `rdsinferaudio` | later: audio classifier on MIGraphX, log-mel spectrogram (`audio-transform`) on the GPU |
| `nvdsaudiotemplate` | `rdsaudiotemplate` | later: custom-library hook for single or batched audio |
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
  public sample videos) into a git-ignored cache. The one exception is the README's demo, `docs/media/demo.gif`
  (from the CC0 street clip); keep it small and replace it rather than adding more.
- **Licences.** Code is LGPL-2.1-or-later; `samples/` is MIT. Every new source, build or script file starts with
  an `SPDX-License-Identifier` line for its licence (MIT under `samples/`). Never add code under another licence.
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
  YOLO11n FP32 at batch 1 on the gfx1036 iGPU: the first compile on a cold user cache (comgr's kernel cache in
  `$XDG_CACHE_HOME`, MIOpen's in `$HOME/.config/miopen`) takes ~70 s (~40 s with only comgr's cache cold), ~30 s
  once both are warm; batch 2 and 4 take ~80-85 s. Loading a cached program takes ~350 ms. `rdsinfer` and
  `rds-compile` cache programs as `<onnx stem>_b<N>_<C>x<H>x<W>_<gfx>_<fp32|fp16>.mxr` next to the ONNX file (else in
  `$XDG_CACHE_HOME/redstream/`), with a `.mxr.json` manifest (model hash, MIGraphX and HIP versions, device,
  shapes, options) that must match for the program to load. Tests reuse these caches: never delete them casually,
  and never run two YOLO11n tests in parallel (`is_parallel : false`, generous `timeout`s).
- MIGraphX gives every symbolic ONNX dimension a default (1) when it parses a model, silently. Whether a dimension is
  fixed or symbolic comes from our own reader (`rds-infer-onnx`), which reads the file's declared input dims.
- MIGraphX's C++ exceptions always say "Failed to call function"; the real reason is on stderr. Error messages name
  the MIGraphX call instead (`RDS_MGX_CHECK` in `rds-infer-engine.cpp`).
- MIGraphX `dlopen`s its GPU target (`libmigraphx_gpu.so`) through its own `$ORIGIN` RUNPATH, which ASan's `dlopen`
  interceptor defeats: ASan test runs set `LD_LIBRARY_PATH` to MIGraphX's `migraphx/lib` (done in
  `tests/meson.build`).
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
- With `HSA_OVERRIDE_GFX_VERSION` set, HIP's `gcnArchName` reports the overridden target (gfx1030 on a gfx1036).
  The device's own architecture is the KFD topology's `gfx_target_version`; the engine's cache names use that one.
- ROCm 10.1 shares `GPU_MAX_HW_QUEUES` (default 4) hardware queues between all HIP streams. With more streams than
  queues (per-thread streams on many threads, as in a pipeline), an SDMA copy now and then faults: `Memory access
  fault by GPU node-1 ... on address 0x401000`, then SIGABRT; the address is none of ours, and the faulting copy
  (pageable host → device, `hipMemcpy2DAsync`) was valid. `HSA_ENABLE_SDMA=0` (copies by blit kernels) avoids it at
  no measured cost on the gfx1036 iGPU, so `tests/meson.build` sets it for every test; `GPU_MAX_HW_QUEUES=16` avoids
  it too but slowed `rds-test3` from ~45 to ~29 fps. Seen in `memory_hip`'s concurrent views test (~2 % of runs
  under `meson test`, whose `MALLOC_PERTURB_` changes the timing); no standalone HIP reproducer yet.
- HIP loads a library's kernels lazily, at their first launch (`HIP_ENABLE_DEFERRED_LOADING`, on by default). In
  ROCm 10.1 the launch that loads the code object is not ordered after the work queued before it on the same stream:
  `rdsosd`'s first pass copies its commands to the device (`hipMemcpyAsync`) and launches the kernel on its
  non-blocking stream, and the kernel read the device block before the copy landed and drew nothing, with no error
  (`element_osd_first_*`, each a fresh process). With `HIP_ENABLE_DEFERRED_LOADING=0`, `AMD_SERIALIZE_KERNEL=3` or a
  stream sync between the copy and the launch it drew correctly; `HIP_LAUNCH_BLOCKING=1` and SDMA copies did not
  help. `rds_osd_gpu_new` loads its kernels up front with `hipFuncGetAttributes`; do the same for a kernel whose first
  launch follows other work on its stream.
- MIOpen (MIGraphX's convolutions) keeps its databases under `$HOME/.config/miopen`. GLib tests with
  `G_TEST_OPTION_ISOLATE_DIRS` set `HOME=/dev/null`, so such tests point `MIOPEN_USER_DB_PATH` and
  `MIOPEN_CUSTOM_CACHE_DIR` somewhere writable, and `AMD_COMGR_CACHE_DIR` too, since the isolated
  `XDG_CACHE_HOME` is fresh for each test (`infer_engine` uses the build directory).

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
  rds-memory         memory:RDS allocator (map returns the RdsBufSurface); read-only surface views of one batch
                     slot (rds_buffer_new_surface_view)
  rds-buffer-pool    RdsBufferPool, rds_propose_allocation / rds_decide_allocation
  rds-meta*, rds-osd-types  batch/frame/object/user/display meta, pools, deep copy
  rds-gst-meta       RdsMeta GstMeta on buffers (gst_buffer_get_rds_batch_meta)
  rds-event          stream events (pad added/deleted, stream start/EOS/segment/reset)
  rds_compat.h       DeepStream compat header (+ rds-compat-aliases.h, rds-compat-remap.c); rds_compat_infer.h
                     (C++) adds the custom parser ABI names, for parser libraries only
  rds-infer*.h       rdsinfer types (C), its tensor output meta (rds-infer-meta.h) and the custom bbox parser ABI
                     (rds-infer-custom-impl.h, C++); headers only
  rds-transform      rds_buf_surf_transform: batched crop/scale/colour conversion (NvBufSurfTransform-shaped), and
                     rds_buf_surf_transform_composite (many surfaces into one, NvBufSurfTransformComposite-shaped)
  rds-transform-kernels.hip  its HIP kernels (amdclang++, linked in as an object); their device helpers
                     (sampling, BT.601 colour) are in rds-kernel-common.h, shared with rdsinfer's kernel
  *-private.h        internal headers, not installed (rds-hip-private.h is the only HIP include)
gst/rdsvideo/      libgstrdsvideo plugin: rdsvideodec (rocDecode, gstrdsvideodec.c), rdsvideoconvert
                   (gstrdsvideoconvert.c), shared helpers in rds-video-util
gst/rdsmux/        libgstrdsmux plugin: rdsstreammux (GstAggregator, gstrdsstreammux.c) and rdsstreamdemux
                   (gstrdsstreamdemux.c)
gst/rdsinfer/      libgstrdsinfer plugin: rdsinfer (gstrdsinfer.c; refcounted output sets and tensor meta in
                   rds-infer-output; test hooks exported for element_infer). rds-infer-config parses
                   nvinfer key-file/YAML configs from one key table; rds-infer-parse loads custom parser libraries
                   (dlopen, ABI version check) and has the built-in YOLO parser; rds-infer-post does the detection
                   post-processing (letterbox geometry, NMS, ROI/size filters); rds-infer-engine compiles ONNX with
                   MIGraphX and caches the program (.mxr + .mxr.json manifest), reading the model's declared input
                   dims with the small protobuf reader rds-infer-onnx. These are in the static rdsinfer-core library
                   (C and C++: link it with link_language 'cpp'), shared with the tests; the letterbox +
                   normalisation kernel (rds-infer-preprocess.hip) is the static rdsinfer-preprocess; config corpus
                   in tests/check/infer-configs, test parser modules built from tests/check/infer-parser-*. Optional
                   (-Dinfer, default auto): needs MIGraphX (-Dmigraphx_path, default <rocm_path>/extras-10), libyaml
                   and json-glib
gst/rdstiler/      libgstrdstiler plugin: rdsmultistreamtiler (gstrdsmultistreamtiler.c: a canvas of the last tiled
                   grid, per-pad cached meta, composite with rds_buf_surf_transform_composite). rds-tiler-layout is the
                   layout core (grid, cells, meta scaling; GLib only), the static rdstiler-core the tests link. Always
                   built
gst/rdsosd/        libgstrdsosd plugin: rdsosd (gstrdsosd.c: in place on RGBA memory:RDS, each frame meta drawn on its
                   batch_id slot; process-mode 1 (default) and 2 plan the batch under the meta lock and draw it with
                   rds-osd-gpu, no CPU staging; 0 syncs each slot to the CPU, draws it there and syncs it back; views
                   and shared memory are drawn on a private copy; test hooks exported for element_osd).
                   rds-osd-plan is the draw planner (a batch's metas -> draw commands in draw order, text rasterized by
                   Pango into A8 masks cached in an 8 MiB heap, split into passes when a batch's masks overflow it,
                   commands binned into 32x32 tiles for the GPU; GLib, Cairo and Pango only); rds-osd-draw draws a
                   frame on the CPU from its plan; rds-osd-shapes.h the commands and their per-pixel coverage and blend
                   (plain C that also compiles as HIP device code: exact integer coverage, no FMA in the blend);
                   together the static rdsosd-core the tests link. rds-osd-gpu draws a pass on the GPU, byte for byte
                   as the CPU path (one upload a pass, a block per tile; kernel in rds-osd-gpu.hip, built with
                   -ffp-contract=off), the static rdsosd-gpu. Optional (-Dosd, default auto): needs cairo and
                   pangocairo
gst/rdssink/       libgstrdssink plugin: rdslogger (gstrdslogger.c: passthrough, counts frames per source_id and prints
                   the **PERF line on a periodic pipeline-clock id) and rdsvideosink (gstrdsvideosink.c: a bin of
                   rdsvideoconvert, BGRx videoconvert and the video-sink factory; forwards the DeepStream sink
                   properties the inner sink has; refuses batches). Always built
gst/common/        static rds-plugin-common, shared by the plugins: rds-keyfile-yaml (YAML config -> GKeyFile, with the
                   OpenCV-style "%YAML:1.0" line), rds-config-parse (number and bool parsing for the key tables),
                   rds-object-style.h (the drawing defaults of the object metas rdsinfer and rdstracker create).
                   Built when libyaml is found
gst/rdstracker/    libgstrdstracker plugin: rdstracker (gstrdstracker.c: write-back of IDs and boxes, created and
                   removed objects, stream events). rds-tracker-config parses NvMultiObjectTracker YAML configs from
                   one key table; rds-tracker-core is the tracking core (targets, life cycle, IDs, greedy and NvSORT
                   cascaded matching; GLib only, no GStreamer or GPU); rds-tracker-kf the Kalman filter. These three
                   are the static rdstracker-core library, which the tests link directly. Optional (-Dtracker,
                   default auto): needs only libyaml, not MIGraphX
docs/              compat-matrix.md: what the compat header and the elements map, with status and sources (its rdsinfer
                   and rdstracker config-key tables must match the key tables: the infer_config and tracker_config
                   tests check every row; element_compat_matrix checks the tiler, OSD, logger and sink property
                   tables against the elements' properties)
samples/rds-test3/ clean-room test3-style multi-stream sample (end to end: per-frame counts, tiler, OSD, display;
                   --dry-run, --no-display, --pgie-config, --tracker-config)
samples/tracker/   our IOU and NvSORT low-level tracker configs for rdstracker (tuned on the street clip)
tools/rds-compile/ rds-compile: pre-builds rdsinfer's program with its config parser and engine (exit 2: up to date)
tools/rds-rtsp-serve/ rds-rtsp-serve: a test pattern or a looped file over RTSP, with a TCP fault relay in front (freeze,
                   cut, refuse on a schedule). Its server and relay (rtsp-test-server, gst-rtsp-server on its own thread
                   and context) are the static rtsp-test-server library the tests link (its shared option gives every
                   client one media, so a later client joins mid-GOP); without gst-rtsp-server a stub whose constructor
                   returns NULL, so the RTSP tests skip
tests/check/       GLib unit, element and pipeline tests (suites cpu/gpu); test clips are encoded in-process
                   (rds-test-media.h, needs the VA encoder); soak memory probes in rds-test-soak.h
tests/samples/     sample-level tests
tests/tools/       tool tests (shell scripts)
tests/media/       make-test-media.sh: generated H.264 clips (into git-ignored .cache/)
tests/models/      committed tiny ONNX test models (make-test-models.py); fetch-yolo11n.sh exports YOLO11n and its
                   test reference data (street photo, Ultralytics detections, parity tensors) into .cache/models
                   (host, Docker; AGPL, never committed); fetch-tracker-clip.sh transcodes a CC0 street clip for
                   the tracker tests into .cache/media (host, Docker, GPU)
spikes/            design spikes (rocdec-hip, migraphx-devptr); reference code, not built by meson
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
tests/models/fetch-yolo11n.sh    # ON THE HOST (Docker + network): exports YOLO11n and its reference data into
                                 # .cache/models; without it the YOLO11n tests skip
tests/models/fetch-tracker-clip.sh  # ON THE HOST (Docker + network + GPU): the street clip for pipeline_tracker
# run our elements from the build tree
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse \
  ! rdsvideodec ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA,width=640,height=640" ! fakesink sync=false
# two sources batched by rdsstreammux and split again by rdsstreamdemux (a queue per branch)
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 rdsstreammux name=m batch-size=2 width=1280 height=720 \
  ! rdsstreamdemux name=d \
  filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec ! m.sink_0 \
  filesrc location=.cache/media/smpte-1366x768.h264 ! h264parse ! rdsvideodec ! m.sink_1 \
  d.src_0 ! queue ! fakesink sync=false  d.src_1 ! queue ! fakesink sync=false
# the whole M1 chain on the street clip (needs YOLO11n and the clip): counts per frame, tiled frames in a window;
# --no-display runs to fakesink sync=false. A window from the container needs the X11 flags on docker run:
#   -e DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix -e XAUTHORITY=/tmp/.xauth -v $XAUTHORITY:/tmp/.xauth:ro
GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test3/rds-test3 --tracker-config samples/tracker/config_tracker_NvSORT.yml \
  file://$PWD/.cache/media/crosswalk-720p.h264 file://$PWD/.cache/media/crosswalk-720p.h264
# one source, decode to display (sync=false: raw .h264 has no timestamps)
GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/crosswalk-720p.h264 ! h264parse \
  ! rdsvideodec ! m.sink_0 rdsstreammux name=m batch-size=1 width=1280 height=720 \
  ! rdsinfer config-file-path=samples/rds-test3/rds_test3_pgie_config.yml batch-size=1 \
  ! rdstracker ll-config-file=samples/tracker/config_tracker_NvSORT.yml \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd ! rdsvideosink sync=false

# ASan + UBSan build; tests/lsan.supp hides HIP runtime leaks
meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false
meson test -C build-asan --print-errorlogs

# optimized build: no CI yet, so check one before merging (GLib macros expand differently under __OPTIMIZE__)
meson setup build-rel -Dbuildtype=release
meson test -C build-rel --print-errorlogs
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
  VA driver, installs its own alternate signal stack). The soak tests (`pipeline_soak`, `pipeline_mux_soak`,
  `pipeline_infer_soak`, `pipeline_tracker_soak`, `pipeline_osd_soak`) check host memory growth on VmRSS, or under
  ASan on ASan's live heap (`__sanitizer_get_current_allocated_bytes`, which leaves out the quarantine, so ASan keeps
  its default options); the normal build's VmRSS check covers memory that bypasses malloc (HIP pinned memory). They
  are also in the `soak` suite; `pipeline_infer_soak` takes about 4 min with YOLO11n (3000 batches of 4 at ~50 fps on
  a small iGPU, also under ASan), `pipeline_tracker_soak` about 6 min with the street clip (4068 batches of 4) and
  `pipeline_osd_soak` (the same through the tiler and rdsosd) about 6.5 min, so
  `meson test --no-suite soak` leaves the soaks out. Their memory probes live in `tests/check/rds-test-soak.h`: host
  memory, and GPU memory from this process's DRM fdinfo (`drm-total-vram` + `drm-total-gtt` of its render-node fds,
  where HIP allocations are accounted), so other processes on the GPU don't skew it; `hipMemGetInfo` is device-wide.
- Element and pipeline test `main()`s end with `gst_deinit ()` after `g_test_run ()`. Without it, a test process can
  crash at exit in libamdhip64's thread-local destructors on a GStreamer task-pool thread (seen under parallel ASan
  load).
- Tests that need private symbols link `rds_internal_dep`, a static copy of libgstrds. Such a test must never also
  link `rds_dep` or load one of our plugins in-process (the plugin pulls in the shared libgstrds): two copies of the
  library register the same GTypes twice. They run with `internal_test_env`: no plugin paths and their own
  registry, so even an in-process scan loads no plugin.
- Library symbols are hidden by default. Mark public API with `RDS_API` (from `rds/rds-prelude.h`).
- **C++** is used only where MIGraphX and the parser ABI need it (`rds-infer-engine.cpp`, `rds-infer-parse.cpp`), in
  the same layout as the C code. No exception crosses into C: every C-callable function catches at its boundary and
  turns the exception into a `GError` (or a failed parse), and calls into user parser libraries are wrapped in
  try/catch. Executables that link `rdsinfer-core` need `link_language : 'cpp'`.
- **Test models.** The committed `tests/models/scale2*.onnx` are our own tiny models (`make-test-models.py`), so the
  element tests run offline and compile in seconds. YOLO11n is AGPL-3.0: `tests/models/fetch-yolo11n.sh` exports it
  (pinned versions and hashes, `PYTHONHASHSEED=0` for a reproducible export) into the git-ignored `.cache/models`,
  with its test reference data. Never commit it or anything derived from it; tests that need it `g_test_skip` with
  a message naming the script.
- Put new tests next to the existing ones and register them in `tests/meson.build`, in the `cpu` or `gpu` suite.
- The compat-matrix lints (`/cpu/infer-config/compat-matrix`, `/cpu/tracker-config/compat-matrix`) run only in
  builds with rdsinfer and rdstracker respectively: a build without MIGraphX, such as a hosted CI runner, doesn't
  check the rdsinfer key tables in `docs/compat-matrix.md`. A new or changed config key needs its row there; the
  test's message prints the expected row. `/cpu/elements/compat-matrix` does the same for the property tables of
  `rdsmultistreamtiler`, `rdsosd` (when built), `rdslogger` and `rdsvideosink`: every property except those of
  GstObject, GstElement and GstBin needs a row (GstBaseTransform's `qos` included), and a row naming no property
  fails too.
- **Tracker, tiler, OSD, logger and sink behaviour.** Where NVIDIA's docs are silent, `rdstracker`,
  `rdsmultistreamtiler`, `rdsosd`, `rdslogger` and `rdsvideosink` match observed DeepStream 8.0 behaviour, except where
  8.0 is clearly broken; there they do the sane thing. Code comments say "DeepStream 8.0 behaviour" or "ours", and
  every such choice has a row in the compat matrix's behaviour tables ("Tracking behaviour", "Tiling behaviour",
  "Drawing behaviour", "Logging behaviour", "Display behaviour"): keep them in step.
- **The street clip** (`tests/models/fetch-tracker-clip.sh`, CC0 from Wikimedia Commons) is VP8, which nothing in
  `redstream:dev` decodes: the script decodes it with a pinned PyAV in a throwaway Python container and encodes it
  with `vah264enc` in the dev image. The encoding depends on the GPU and driver (another hash is only a warning), and
  `pipeline_tracker`'s budgets come from a run on the encoding with the recorded hash. `pipeline_tracker` saves the
  clip's detections for `tracker_replay`, stamped with a hash of the `rdstracker-core` sources: a change to the
  tracking code makes the replay skip until `pipeline_tracker` runs again.
- **Ready-on-push:** a producer synchronizes its GPU work before pushing a `memory:RDS` buffer; downstream never
  waits on a stream. Library copies, memsets and staging syncs (`sync_for_cpu`/`sync_for_device`) are synchronous,
  on the calling thread's per-thread stream (`hipStreamPerThread`), so they don't wait for other streams' work. The
  per-thread stream is still a blocking stream: it synchronizes with the legacy null stream, and so does the default
  transform session (`cuda_stream` NULL, which runs on the null stream). Elements create their own non-blocking
  stream (`hipStreamNonBlocking`) and set it in their transform session; never queue element work on the null
  stream.
- Transform sessions are per thread and streaming threads are shared: elements set the transform session before
  each call and restore the previous one after (`rds_buf_surf_transform_get_session_params` first).
- `memory:RDS` is not `NO_SHARE`: shallow buffer copies share the surface. Pixel writers must own the memory
  exclusively (a WRITE map of shared memory gets a private deep copy).
- **Surface views are read-only.** `rds_buffer_new_surface_view` (one slot of a batch, what `rdsstreamdemux` pushes)
  makes `GST_MEMORY_FLAG_READONLY` memory that refs the batch buffer. A writer (an OSD, say) calls
  `gst_buffer_make_writable`, then `gst_buffer_map (GST_MAP_WRITE)`: GStreamer swaps in `gst_memory_copy` of the
  view, an owned single surface, and the shared batch stays untouched. `gst_memory_map (WRITE)` on the view itself
  fails. Never replace or remove the memory of a buffer that has views. Views of one batch slot share the batch's
  staging mirror: map and sync a slot from one thread at a time.
- **Demux branches:** every `rdsstreamdemux` branch needs its own `queue` (the branches share the demux's streaming
  thread; the first sink to preroll blocks the rest). Each demuxed view pins its whole batch until released, and
  sinks keep their last buffer, so N branches can pin N batches: set `rdsstreammux`'s `buffer-pool-size` (default 4)
  larger than the number of branches whose sinks keep their last buffer, or the mux stalls. As with `tee`,
  FLUSHING from one branch stops all of them: block or unlink a branch before setting it to NULL. To remove a source,
  wait for its `rds-stream-eos` downstream before releasing its mux pad (releasing right after sending EOS into the
  pad drops that stream-eos). A sink added while PLAYING needs `async=false`.
- Dispatch our stream events on the cast type: `switch ((RdsEventType) GST_EVENT_TYPE (ev))`. An uncast switch
  triggers `-Wswitch`, as on DeepStream.
- Use TDD: write the failing test first.

## Agent skills

Repo-specific skills live in `.agents/skills/<name>/SKILL.md`; `.claude/skills` is a symlink to it.
Add a skill when a workflow is repeated often enough that agents keep rediscovering it.
