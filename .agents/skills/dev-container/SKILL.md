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

Use `-it` only for interactive shells; agents should use one-off `bash -c` commands.
Add `--user $(id -u):$(id -g) -e HOME=/tmp` so outputs aren't root-owned. From zsh, run the whole thing via
`bash -c '...'`, because zsh doesn't word-split `$RUN`.

## Build and test

```bash
$RUN bash -c 'meson setup build --reconfigure 2>/dev/null || meson setup build; meson test -C build --print-errorlogs'
$RUN tests/media/make-test-media.sh          # .cache/media/{ball-720p,smpte-1366x768,ball-1080p}.h264

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
| `rdsvideodec`/`rdsvideoconvert`: `no element` | set `GST_PLUGIN_PATH_1_0=build/gst` (the build tree's plugins) |
| first frame of each rocDecode surface all zeros (small clips) | surfaces ≤ 1 MiB must be mapped before the first decode; `rdsvideodec` does it, a bare rocDecode program must too |
| VA plugin has no `vah264dec` on the host (Fedora) | host Mesa lacks patented codecs → `mesa-va-drivers-freeworld`; the container is fine |
