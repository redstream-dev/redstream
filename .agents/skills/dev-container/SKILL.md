---
name: dev-container
description: Use when building, running, testing or benchmarking anything in this repo - how to start the redstream:dev ROCm/GStreamer container with GPU access, run pipelines and MIGraphX on the AMD GPU, and fix the common environment errors.
---

# Dev container

All builds and tests run inside `redstream:dev` (ROCm 10.1 + MIGraphX + GStreamer 1.28 from source).

## Build / run

```bash
docker build -f docker/Dockerfile.dev -t redstream:dev .        # add --build-arg GFX=gfx1100 etc. for other GPUs

RUN="docker run --rm --device=/dev/kfd --device=/dev/dri \
  --group-add $(getent group render | cut -d: -f3) --group-add $(getent group video | cut -d: -f3) \
  --security-opt seccomp=unconfined -v $PWD:/src -w /src redstream:dev"

$RUN bash -c 'rocminfo | grep -E "^\s+Name:\s+gfx"'          # non-interactive one-off command
```

The image's last layer installs `libyaml-dev` (rdsinfer's and rdstracker's YAML configs) and `fonts-dejavu-core`
(distinct Sans, Serif and Mono families for rdsosd's tests), after the ~20-minute GStreamer build, so
adding packages there keeps the GStreamer layer cached; a rebuild after editing it takes seconds.

Use `-it` only for interactive shells; agents should use one-off `bash -c` commands.
Add `--user $(id -u):$(id -g) -e HOME=/tmp` so outputs aren't root-owned. From zsh, run the whole thing via
`bash -c '...'`, because zsh doesn't word-split `$RUN`.

## Build and test

```bash
$RUN bash -c 'meson setup build --reconfigure 2>/dev/null || meson setup build; meson test -C build --print-errorlogs'
$RUN tests/media/make-test-media.sh          # .cache/media/{ball-720p,smpte-1366x768,ball-1080p}.h264
tests/models/fetch-yolo11n.sh               # on the HOST, not in $RUN (it starts its own python container; needs
                                            # network): YOLO11n + reference data into .cache/models (AGPL: never commit)
tests/models/fetch-tracker-clip.sh          # on the HOST too (Docker, network and the GPU): the CC0 street clip,
                                            # VP8 decoded in a python container, vah264enc in redstream:dev, into
                                            # .cache/media/crosswalk-720p.h264 (never commit it)
# the YOLO11n soak takes ~4 min, the tracker soak ~6 min, the OSD soak ~6.5 min; leave the soaks out of repeat runs
$RUN meson test -C build --no-suite soak --print-errorlogs

# ASan + UBSan (tests/lsan.supp, applied by meson test, hides HIP runtime leaks)
$RUN bash -c 'meson setup build-asan --reconfigure -Db_sanitize=address,undefined -Db_lundef=false 2>/dev/null \
  || meson setup build-asan -Db_sanitize=address,undefined -Db_lundef=false; meson test -C build-asan --print-errorlogs'

# No GPU: drop the --device/--group-add flags. gpu-suite tests SKIP; or run only the cpu suite
docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v $PWD:/src -w /src redstream:dev meson test -C build --suite cpu
```

## Smoke tests

```bash
$RUN vainfo --display drm --device /dev/dri/renderD128         # expect H264/HEVC VLD entrypoints
$RUN gst-inspect-1.0 hip                                       # hipupload, hipconvertscale, ...
$RUN bash -c 'gst-launch-1.0 filesrc location=$VIDEO ! matroskademux ! h264parse ! vah264dec \
  ! hipupload ! hipconvertscale ! "video/x-raw(memory:HIPMemory),format=RGBA,width=640,height=640" ! fakesink sync=false'
$RUN migraphx-driver perf model.onnx --input-dim @images 1 3 640 640 --fp16 --iterations 50
```

Redstream's own elements, from the build tree (`build/gst`; needs a build and the clips from
`tests/media/make-test-media.sh`):

```bash
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst gst-inspect-1.0 rdsvideodec | grep Rank'   # primary + 10 (266)
# decode + convert on the GPU, memory:RDS all the way
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse \
  ! rdsvideodec ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA,width=640,height=640" ! fakesink sync=false'
# download to system memory, bilinear scaling
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/ball-1080p.h264 ! h264parse \
  ! rdsvideodec ! rdsvideoconvert interpolation-method=1 ! video/x-raw,format=RGBA,width=640,height=360 \
  ! fakesink sync=false'
# two sources -> rdsstreammux (1280x720 slots, batch of 2) -> rdsstreamdemux, a queue per branch
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 rdsstreammux name=m batch-size=2 width=1280 height=720 \
  ! rdsstreamdemux name=d \
  filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec ! m.sink_0 \
  filesrc location=.cache/media/smpte-1366x768.h264 ! h264parse ! rdsvideodec ! m.sink_1 \
  d.src_0 ! queue ! fakesink sync=false  d.src_1 ! queue ! fakesink sync=false'
# two sources -> rdsstreammux -> rdsinfer (YOLO11n; needs fetch-yolo11n.sh). The first run compiles batch 2 (~85 s
# on a small iGPU), later runs load the cached .mxr; rdsinfer:5 logs the object count per frame
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst GST_DEBUG=rdsinfer:5 gst-launch-1.0 rdsstreammux name=m batch-size=2 \
  width=1280 height=720 ! rdsinfer config-file-path=samples/rds-test3/rds_test3_pgie_config.yml batch-size=2 \
  ! fakesink sync=false filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec ! m.sink_0 \
  filesrc location=.cache/media/smpte-1366x768.h264 ! h264parse ! rdsvideodec ! m.sink_1'
# rds-test3 end to end (mux -> infer -> NvSORT tracker -> tiler -> OSD) on the street clip (needs
# fetch-tracker-clip.sh): one count line per frame and source; --no-display uses fakesink sync=false
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst build/samples/rds-test3/rds-test3 --no-display \
  --tracker-config samples/tracker/config_tracker_NvSORT.yml \
  file:///src/.cache/media/crosswalk-720p.h264 file:///src/.cache/media/crosswalk-720p.h264 | tail'
# the same in a window (autovideosink -> ximagesink): add the X11 flags to docker run, drop --no-display
#   -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix -e XAUTHORITY=/tmp/.xauth -v $XAUTHORITY:/tmp/.xauth:ro
# one source, decode to display with gst-launch (X11 flags as above; sync=false: raw .h264 has no timestamps)
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 filesrc location=.cache/media/crosswalk-720p.h264 \
  ! h264parse ! rdsvideodec ! m.sink_0 rdsstreammux name=m batch-size=1 width=1280 height=720 \
  ! rdsinfer config-file-path=samples/rds-test3/rds_test3_pgie_config.yml batch-size=1 \
  ! rdstracker ll-config-file=samples/tracker/config_tracker_NvSORT.yml \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd ! rdsvideosink sync=false'
# per-stream frame rates: put rdslogger after the mux (**PERF lines every fps-measurement-interval-sec, default 5)
# pre-build a program (exit 2: already up to date)
$RUN build/tools/rds-compile -c samples/rds-test3/rds_test3_pgie_config.yml -b 4
# autoplugging: -v shows GstRdsVideoDec inside the decodebin
$RUN bash -c 'GST_PLUGIN_PATH_1_0=build/gst gst-launch-1.0 -v uridecodebin uri=file:///src/.cache/media/ball-720p.h264 \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! fakesink sync=false | grep -o "GstRdsVideoDec:[a-z0-9]*"'
```

## Common errors

| Symptom | Fix |
|---|---|
| `Unable to find group render` | pass numeric GIDs to `--group-add` (see `RUN` above) |
| `rocminfo` shows no GPU agent / HSA errors on an APU | `HSA_OVERRIDE_GFX_VERSION=10.3.0` (RDNA2), `11.0.0` (RDNA3) |
| `libhiprtc.so.7: cannot open shared object` | `/opt/rocm/core-10.1/lib` missing from `ld.so.conf.d` → run `ldconfig` |
| MIGraphX parse error showing `{1, 3, 1, 1}` input | model has dynamic dims → `--input-dim @<input> N C H W` |
| MIGraphX "hangs" for minutes | it is compiling/tuning; normal on small GPUs. Run long jobs in the background and cache `.mxr` |
| `pci id for fd N: 10de:...` spam | harmless, VA probing the NVIDIA card on mixed-GPU hosts |
| `device kernel image is invalid` | HIP binary was built without the GPU attached → build inside a GPU container or pass `--offload-arch=gfx1030` |
| rocDecode wrong frames / `ROCDEC_RUNTIME_ERROR` mid-stream | feed whole NAL units or access units, not byte chunks |
| `rdsvideodec`/`rdsvideoconvert`/`rdsstreammux`: `no element` | set `GST_PLUGIN_PATH_1_0=build/gst` (the build tree's plugins) |
| first frame of each rocDecode surface all zeros (small clips) | surfaces ≤ 1 MiB must be mapped before the first decode; `rdsvideodec` does it, a bare rocDecode program must too |
| mux/demux pipeline hangs (in preroll or after a few frames), no error | a demux branch without a `queue`, or more branches pinning batches than `rdsstreammux`'s `buffer-pool-size` (leave it unset) |
| test process crashes at exit in libamdhip64 TLS destructors | the test `main()` must end with `gst_deinit ()` |
| YOLO11n tests `SKIP` ("run tests/models/fetch-yolo11n.sh") | run the fetch script on the host; outputs go to `.cache/models` |
| `pipeline_tracker` `SKIP`s; `tracker_replay` skips its clip case | run `tests/models/fetch-tracker-clip.sh` on the host; the replay needs a `pipeline_tracker` run after the last change to the tracker core's sources (its saved detections are stamped with their hash) |
| `vp8dec`/VP8 not found in `redstream:dev` | expected: the image can't decode VP8; `fetch-tracker-clip.sh` decodes it in a python container |
| a compat-matrix lint fails (`/cpu/*-config/compat-matrix`, `/cpu/elements/compat-matrix`) | a config key or element property changed: copy the expected row from the test's message into `docs/compat-matrix.md` (and fill in its status and notes) |
| `rdsosd`: `no element` while the other elements load | the build found no cairo/pangocairo (`-Dosd` is auto): the meson setup log says so |
| `rdsvideosink` runs but no window appears (headless or no X11 flags) | `autovideosink` picked `kmssink`, which can start without showing anything: pass the X11 flags (`DISPLAY`, the X socket, `XAUTHORITY`), or set `video-sink=ximagesink` to get a clear error instead |
| `Could not initialise X output` / `cannot open display` | the X11 flags are missing from `docker run`, or `$XAUTHORITY` is unset on the host |
| `ximagesink`: "A lot of buffers are being dropped" | raw `.h264` from `filesrc` has no timestamps: use `rdsvideosink sync=false` |
| `MIT-SHM` / XShm unavailable warnings from `ximagesink` | harmless in the container (no shared memory with the host X server); it falls back to plain X11 puts |
| MIOpen errors about `/dev/null/.config/miopen` in a GLib test | `G_TEST_OPTION_ISOLATE_DIRS` sets `HOME=/dev/null`: set `MIOPEN_USER_DB_PATH`, `MIOPEN_CUSTOM_CACHE_DIR` and `AMD_COMGR_CACHE_DIR` to writable dirs |
| ASan: MIGraphX can't load `libmigraphx_gpu.so` | ASan's `dlopen` ignores MIGraphX's `$ORIGIN` RUNPATH; `meson test` sets `LD_LIBRARY_PATH` (do the same when running a binary by hand) |
| MIGraphX error says only "Failed to call function" | the real reason is printed on stderr just before it |
| `rdsinfer`/`rds-compile` rebuild a program every time | read the "built … because …" message (`GST_DEBUG=rdsinfer:4`); the `.mxr.json` manifest field it names differs |
| VA plugin has no `vah264dec` on the host (Fedora) | host Mesa lacks patented codecs → `mesa-va-drivers-freeworld`; the container is fine |
