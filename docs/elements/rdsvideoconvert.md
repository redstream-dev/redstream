# rdsvideoconvert

How to read this page, and how to run the examples: [element reference](README.md).

Converts, scales and crops single video frames on the GPU, in `memory:RDS` or in system memory on either side.
DeepStream counterpart: `nvvideoconvert`
([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideoconvert.html)).
Built when: always.

## Example

```sh
# decode, then scale to a 640x640 RGBA memory:RDS frame (the clips come from tests/media/make-test-media.sh)
gst-launch-1.0 filesrc location=.cache/media/ball-1080p.h264 ! h264parse ! rdsvideodec ! rdsvideoconvert \
  ! "video/x-raw(memory:RDS),format=RGBA,width=640,height=640" ! fakesink sync=false

# the centre quarter of the frame, scaled bilinearly into system memory
gst-launch-1.0 filesrc location=.cache/media/ball-1080p.h264 ! h264parse ! rdsvideodec \
  ! rdsvideoconvert src-crop=480:270:960:540 interpolation-method=bilinear \
  ! video/x-raw,format=RGBA,width=640,height=360 ! fakesink sync=false
```

The output format, size and memory come from the caps downstream asks for; a capsfilter sets them.

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)` or `video/x-raw`, format NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR or GRAY8, 1–16384 × 1–16384 |
| `src` | src | always | as the sink: any of those formats and sizes, in either memory |

## Properties

`gpu-id`, `nvbuf-memory-type` and `output-buffers` set up the output pool and the GPU: they can be changed only in
NULL or READY. In PAUSED or PLAYING (or on the way there) a new value is refused with a GLib warning ("… can only be
changed in the NULL or READY state; keeping the current value"). The other properties can change in any state and
apply from the next frame. A value outside a property's range is refused by GObject with a GLib warning. The integers
of the enums are DeepStream's; only `nvbuf-memory-type`'s nicks are DeepStream's too, the others are ours.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `src-crop` | string | `""` | `left:top:width:height` | any state | the part of the input to convert; empty: all of it. See [Crops](#crops) |
| `dest-crop` | string | `""` | `left:top:width:height` | any state | where the picture goes in the output; empty: the whole frame. See [Crops](#crops) |
| `allow-odd-crop` | boolean | `true` | | any state | on NV12 and I420 output, widen an odd `dest-crop` to even coordinates; `false`: an odd one is an element error. Source rects may always be odd |
| `interpolation-method` | enum `GstRdsInterpolationMethod` | `default` | `0` `nearest`, `1` `bilinear`, `2` `algo-1`, `3` `algo-2`, `4` `algo-3`, `5` `algo-4`, `6` `default` | any state | the scaling filter: `default` and `algo-4` sample the nearest pixel, `algo-1` to `algo-3` run bilinear |
| `compute-hw` | enum `GstRdsComputeHW` | `default` | `0` `default`, `1` `gpu`, `2` `vic` | any state | `0` and `1` run on the GPU; `2` (Jetson's VIC) is an element error ("compute-hw=VIC is not available"): at start, or on the next converted frame when set while playing (passthrough frames are not affected) |
| `flip-method` | enum `GstRdsVideoFlipMethod` | `none` | `0` `none`, `1` `counterclockwise`, `2` `rotate-180`, `3` `clockwise`, `4` `horizontal-flip`, `5` `upper-right-diagonal`, `6` `vertical-flip`, `7` `upper-left-diagonal` | any state | unsupported: any value but `0` is an element error ("flip-method N is not supported yet"): at start, or on the next frame when set while playing (a flip turns passthrough off, so every frame is converted) |
| `gpu-id` | uint | `0` | 0–4294967295: a visible ROCm device | NULL, READY | the GPU that converts. One that doesn't exist fails the start: "GPU N is not available" |
| `nvbuf-memory-type` | enum `GstRdsVidConvBufMemoryType` | `nvbuf-mem-default` | `0` `nvbuf-mem-default` (HIP device memory), `1` `nvbuf-mem-cuda-pinned`, `2` `nvbuf-mem-cuda-device`, `3` `nvbuf-mem-cuda-unified` | NULL, READY | the memory of the `memory:RDS` output buffers |
| `output-buffers` | uint | `4` | 1–4294967295 | NULL, READY | the minimum number of buffers in the output pool |
| `disable-passthrough` | boolean | `false` | | any state | convert even when nothing would change |
| `contiguous-buffers` | boolean | `false` | | any state | accepted no-op: batches are always contiguous |
| `copy-hw` | enum `GstRdsCopyHWType` | `gpu` | `1` `gpu`, `2` `vic` | any state | accepted no-op: copies run on the GPU |
| `bl-output` | boolean | `false` | | any state | accepted no-op (Jetson's block-linear output): output is always pitch-linear |
| `silent` | boolean | `false` | | any state | accepted no-op |
| `qos` | boolean | `false` | | any state | GstBaseTransform's |

## Metadata

`rdsvideoconvert` reads no Redstream meta and adds none. Metas on the input buffer are copied to the output:
- `RdsMeta` (the `RdsBatchMeta` with its frames and objects), untagged metas, and metas tagged only `video` are kept.
  Object coordinates are not rescaled: they stay in the input's coordinates.
- `GstVideoMeta` is dropped (the output buffer carries its own layout), and so is `GstVideoCropMeta` (the crop is
  applied, see [Crops](#crops)).
- Any other tagged meta (size-, orientation- or memory-dependent) is dropped.

## Behaviour

**Conversion.** Each frame goes through `rds_buf_surf_transform` on the element's own HIP stream, synchronously: the
output is complete when it is pushed. What the transform computes, byte for byte (filters, BT.601 colour, odd 4:2:0
rects, alpha), is in the compatibility matrix's
[Transform semantics](../compat-matrix.md#transform-semantics).

**Passthrough.** When the input and output caps are equal (memory included), and no `src-crop`, `dest-crop` or flip is
set, buffers pass through untouched; `disable-passthrough=true` turns that off. A property change that turns
passthrough on or off takes effect from the next buffer, and renegotiates the allocation upstream.

**Caps.** Any supported format and size converts to any other, and either memory to either. When downstream allows the
input caps, they are chosen (passthrough). Otherwise the output is fixed as close to the input as downstream allows:
the nearest width, height and pixel aspect ratio, the input's format where it is allowed, and its colorimetry and
chroma siting when the output is the same format (or both are 4:2:0 YUV). The frame rate is kept: there is no rate
conversion, so a downstream that demands another rate fails to negotiate. When downstream fixes only the width or
only the height, the other keeps the input's value: the aspect ratio is not preserved.

**System memory.** A system-memory input frame is uploaded to a device staging surface, and a system-memory output
frame is converted into a staging surface and downloaded; `memory:RDS` sides are used directly. The staging surfaces
are made at the first frame and again only when the input or output video info changes. A system-memory output pool
is a video pool (downstream's when it offers one), with `GstVideoMeta` when downstream supports it.

<a id="crops"></a>**Crops.**
- Both crops are `left:top:width:height` in pixels: four decimal integers, no sign or spaces. An invalid string is
  refused with a GLib warning ("invalid src-crop … (expected left:top:width:height); keeping …"). `0:0:0:0` and the
  empty string mean no crop.
- Crops are clamped to the frame, as DeepStream documents. A crop that is empty after clamping is an element error,
  "The crop rectangle is empty after clamping it to the frame" (STREAM, FAILED).
- When the input buffer has a `GstVideoCropMeta` (from `videocrop` in passthrough, say), the conversion reads only
  that part of the surface, and `src-crop` is relative to it: offset by its x and y, clamped to its size.
- Outside `dest-crop`, the output is filled with black: Y 16 and U, V 128 for NV12 and I420, every byte 0 for GRAY8
  and the RGB formats, so **alpha is 0** outside the rect on RGBA and BGRA.

<a id="batches"></a>**Batches.** A `memory:RDS` buffer holding more than one frame (a batch from `rdsstreammux`) is an
element error when it has to be converted: "Batched input (batch size above 1) is not supported yet" (STREAM,
NOT_IMPLEMENTED). In passthrough, a batch goes through untouched.

**Errors**, all element errors, besides those above:
- `compute-hw=2` and a `flip-method` other than `0`: RESOURCE, SETTINGS (see their rows);
- a GPU or HIP stream that can't be set up at start: RESOURCE;
- a staging surface that can't be allocated: RESOURCE, NO_SPACE_LEFT;
- a buffer that can't be mapped, a failed copy between system and device memory, the black fill around `dest-crop`
  failing ("Cannot clear the output frame"), or a failed transform ("Video conversion failed"): STREAM, FAILED.

## Differences from DeepStream 8.0

- The enum nicks of `interpolation-method`, `compute-hw`, `flip-method` and `copy-hw` are ours (NVIDIA's page names
  the integers only); `interpolation-method`'s `algo-1` to `algo-4` are bilinear or nearest filters, as listed.
- `flip-method` is not implemented yet; `compute-hw=2` (VIC) is Jetson hardware and fails.
- `contiguous-buffers`, `copy-hw`, `bl-output` and `silent` are accepted and do nothing.
- One frame per buffer: a batch that needs converting is an element error (see [Batches](#batches)).

## Known issues and limits

- Batches are refused (see [Batches](#batches)): convert before `rdsstreammux`, or after `rdsstreamdemux` or a tiler.
- No flips or rotations.
- BT.601 limited-range colour only, and chroma siting is ignored (see [Transform
  semantics](../compat-matrix.md#transform-semantics)).
- When downstream fixes one dimension only, the aspect ratio is not kept.
- Object metas are carried over without rescaling their coordinates.
