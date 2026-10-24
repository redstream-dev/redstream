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
| `nvstreammux` | `rdsstreammux` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreammux) |
| `nvstreamdemux` | `rdsstreamdemux` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreamdemux) |
| `nvinfer` | `rdsinfer` | planned |  | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvinfer) |
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

Not aliased: `gst_buffer_add_nvds_meta` (Redstream attaches the batch with `rds_buffer_add_batch_meta`;
[ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html)), and
NvDs metadata types and functions Redstream doesn't implement yet (tensor, analytics, event message, audio).

## Stream events (gst-nvevent)

Events are custom events with their own `GstEventType` numbers, so dispatching on `GST_EVENT_TYPE` works.

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
