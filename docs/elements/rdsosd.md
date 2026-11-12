# rdsosd

How to read this page, and how to run the examples: [element reference](README.md).

Draws object boxes and labels, display meta (rectangles, text, lines, arrows, circles) and the clock on RGBA
`memory:RDS` frames, in place, in the geometry of DeepStream 8.0's default GPU mode. It draws on the GPU by default,
so the frames never leave it, or on the CPU with `process-mode=0`: the same pixels either way, with Pango for the
text. DeepStream counterpart: `nvdsosd` ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsosd)).
Built when: `-Dosd=enabled`, or `auto` (the default) when cairo and pangocairo are found; without them the element
and its tests are left out, with a meson message.

## Example

```sh
# two sources tiled, converted to RGBA, the clock drawn on the GPU; process-mode=0 draws on the CPU
gst-launch-1.0 rdsstreammux name=m batch-size=2 width=640 height=360 ! rdsmultistreamtiler width=1280 height=360 \
  ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" ! rdsosd display-clock=true ! fakesink sync=false \
  videotestsrc num-buffers=100 ! video/x-raw,format=NV12 ! m.sink_0 \
  videotestsrc num-buffers=100 pattern=ball ! video/x-raw,format=NV12 ! m.sink_1

# one file, the clock at 24 points, shown through rdsvideosink (fakesink here: no display needed)
gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec ! m.sink_0 \
  rdsstreammux name=m batch-size=1 width=1280 height=720 ! rdsvideoconvert ! "video/x-raw(memory:RDS),format=RGBA" \
  ! rdsosd display-clock=true clock-font-size=24 x-clock-offset=20 y-clock-offset=20 \
  ! rdsvideosink video-sink=fakesink sync=false
```

These draw only the clock: boxes and labels come from the object metas an `rdsinfer` and `rdstracker` before it
write, or from display meta an application adds. The README's [`rds-test3` pipeline](../../README.md#see-it) shows a
full chain.

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)`, format RGBA, 1–16384 × 1–16384, single frames or batches |
| `src` | src | always | as the sink: the same buffers, drawn in place |

- RGBA only (ours: DeepStream 8.0's GPU mode also takes NV12); anything else is an element error, "rdsosd draws on
  RGBA surfaces only" (STREAM, FORMAT).
- A buffer whose memory is shared, or a read-only surface view (`rdsstreamdemux` output), is drawn on a private copy
  made on the device; the batch it came from is never written.
- In the GPU modes the frames stay on the GPU: no CPU staging, no sync to the CPU and back. Frames in system memory
  are then an element error ("process-mode N draws on frames the GPU can reach"), and so are frames in device memory
  on another GPU than `gpu-id`.
- Frames with nothing to draw (under the current switches) are not touched, and a buffer with nothing to draw passes
  through unmapped, without a copy.

## Properties

Names, types, ranges and defaults are those `gst-inspect-1.0` shows for DeepStream 8.0's nvdsosd; the enum nicks are
DeepStream's (`process-mode`'s `MODE_VIC` is ours). `display-mask`, `blur-bbox` and `blur-on-gie-class-ids` log one
GLib warning per element and property when they are set to something that would do anything ("…; the property is
accepted and has no effect").

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `display-bbox` | boolean | `true` | | any state | draw object and display-meta rectangles (borders and fills) |
| `display-text` | boolean | `true` | | any state | draw object text, display-meta labels and the clock |
| `display-clock` | boolean | `false` | | any state | draw the local time on every frame |
| `clock-font` | string | `NULL` | a fontconfig / Pango font name | any state | NULL: `Serif` |
| `clock-font-size` | uint | `0` | 0–60 | NULL, READY | points; 0: 12 |
| `x-clock-offset`, `y-clock-offset` | uint | `0` | 0–4294967295 | NULL, READY | the clock's position in pixels |
| `clock-color` | uint | `0` | 0–4294967295, as 0xRRGGBBAA | NULL, READY | the default is 0, but until the property is set the clock is opaque red (DeepStream 8.0 behaviour); setting 0 makes it invisible |
| `process-mode` | enum `GstRdsOsdMode` | `MODE_GPU` | `0` `MODE_CPU`, `1` `MODE_GPU`, `2` `MODE_VIC` | NULL, READY | `0` draws on the CPU; `1` on the GPU (the default, as in DeepStream 8.0); `2` (ours: DeepStream 8.0's dGPU enum stops at 1, and VIC is a Jetson unit) on the GPU too, with one debug-log warning per element. The same pixels in every mode |
| `gpu-id` | uint | `0` | 0–4294967295: a visible ROCm device | NULL, READY | the GPU the GPU modes draw on; one that isn't there is an element error at start, "GPU N is not available" (RESOURCE, NOT_FOUND). The CPU mode ignores it |
| `display-mask` | boolean | `false` | | any state | accepted no-op: masks are not drawn yet; `true` logs the warning |
| `blur-bbox` | boolean | `false` | | NULL, READY | accepted no-op: objects are not blurred yet; `true` logs the warning |
| `blur-on-gie-class-ids` | string | `NULL` | | NULL, READY | accepted no-op, as `blur-bbox`; a non-NULL value logs the warning |
| `qos` | boolean | `false` | | any state | GstBaseTransform's |

## Metadata

- **Reads**, under the batch's meta lock: each `RdsFrameMeta` (`NvDsFrameMeta`) of the `RdsBatchMeta`
  (`NvDsBatchMeta`): its `batch_id` (the slot it is drawn on), its object metas' `rect_params` and `text_params`, and
  its display metas' `rect_params`, `text_params`, `line_params`, `arrow_params` and `circle_params` (the first
  `num_*` entries, at most 16 of each). The structs are the compatibility matrix's
  ["OSD parameter structs"](../compat-matrix.md#osd-parameter-structs) (`RdsOSD_RectParams` and so on).
- **Writes:** no meta; pixels only.

## Behaviour

Each row is either **matches nvdsosd (DeepStream 8.0)**: observed DeepStream 8.0 behaviour in its default GPU mode
where the docs are silent, or **ours**: where 8.0 settles nothing or is clearly broken.

| Behaviour | Status | Notes |
|---|---|---|
| Rectangle borders | matches nvdsosd (DeepStream 8.0) | hard edges; each float field truncated; with L = left, R = L + width and bw = `border_width`, the left and top bands are [L − ⌊(bw−1)/2⌋, L + ⌈(bw−1)/2⌉], the right and bottom bands [R − ⌈(bw−1)/2⌉, R + ⌊(bw−1)/2⌋] (bw 3 at L 100: 99..101); `border_width` 0 draws no border |
| Rectangles partly outside the frame | matches nvdsosd (DeepStream 8.0) | clipped, never moved into the frame (DeepStream's CPU mode clamps them) |
| `border_width` above 32 (`MAX_BORDER_WIDTH`) | ours | drawn 32 wide with one element warning (8.0 stops the pipeline) |
| Rectangle fills | matches nvdsosd (DeepStream 8.0) | `has_bg_color`: `bg_color` strictly inside the border bands (bw 0: L + 1 .. R − 1) |
| Alpha | matches nvdsosd (DeepStream 8.0) | a·src + (1 − a)·dst per channel, truncated (red at 0.5 over 128 gives 191, 64, 64); applied to borders too (the docs say GPU mode ignores border alpha; 8.0 applies it); alpha byte blended with 255 |
| A rect with a NaN field | ours | not drawn |
| Text position | matches nvdsosd (DeepStream 8.0) | the background box's top left is (`x_offset` + 1, `y_offset` + 1); the text is inset by the padding; not moved into the frame, clipped at its edges |
| Text box padding | ours | `font_size` / 2 pixels left and right, `font_size` / 4 above and below, around Pango's logical extents (8.0: ≈10 and ≈5 at size 20; size 20 boxes ≈40 px tall) |
| Fonts | matches nvdsosd (DeepStream 8.0) | `font_name` honoured (a fontconfig / Pango name); unknown: fontconfig's fallback, a sans; `font_size` in points at 96 dpi; `font_size` 0, NULL or empty text: nothing drawn, not even the box; `\n` starts a new line; antialiased glyphs |
| NULL or empty `font_name` | ours | `Sans` (8.0's GPU mode crashes on NULL) |
| Text encoding | ours | `display_text` is UTF-8, shaped by Pango (any script the installed fonts cover); invalid UTF-8 is drawn with U+FFFD for the bad bytes (8.0 not observed) |
| Font sizes above 1024 | ours | drawn at 1024 points |
| Switches | matches nvdsosd (DeepStream 8.0) | `display-bbox=0` hides object and display-meta rects only; `display-text=0` hides object text, display-meta labels and the clock |
| Draw order | matches nvdsosd (DeepStream 8.0) | all rects (objects, then display metas), all text (objects, then labels), lines, arrows, circles, the clock; within a kind in list order, later on top. The order of arrows against circles is ours |
| Display meta limits | matches nvdsosd (DeepStream 8.0) | at most 16 entries of each kind read, a larger `num_*` ignored; every display meta of the frame drawn |
| Lines and arrow shafts | matches nvdsosd (DeepStream 8.0) | hard edges, ⌊(w−1)/2⌋ pixels on each side of the segment (width 6 is drawn 5 wide), ends extended as far; clipped at the frame. Diagonals have hard edges (8.0 smooths them slightly); width 0 draws 1 pixel (ours) |
| Arrow heads | matches nvdsosd (DeepStream 8.0) | an open V at (x1, y1) for `START_HEAD` and (x2, y2) for `END_HEAD`, its wings 22 px back and 7 px to each side (measured), drawn with the shaft's width |
| `BOTH_HEAD` | ours | a head at both ends (8.0 draws one head, or nothing in CPU mode) |
| Other `arrow_head` values, zero-length arrows | ours | the shaft only |
| Circles | matches nvdsosd (DeepStream 8.0) | a ring straddling the radius: `circle_width` w covers the pixels at distances r − ⌊w/2⌋ .. r + ⌈w/2⌉ from the centre (w 3, r 60: 59..62); `has_bg_color` fills inside the ring with `bg_color`; clipped at the frame |
| `circle_width` 0 | matches nvdsosd (DeepStream 8.0) | still outlined, 3 px centred on the radius (r 60: 59..61), so zeroed circles stay visible |
| Huge coordinates and widths | ours | drawn only up to the size limits under [Known issues and limits](#known-issues-and-limits) |
| Clock | matches nvdsosd (DeepStream 8.0) | `YYYY-MM-DD HH:MM:SS`, the process's local time read once per buffer, drawn like text without a box at the clock offsets; default font a serif; one per frame |
| A buffer without frame meta | ours | passes through untouched, except that it gets the clock on every frame when `display-clock` is on |
| Masks | ours | not drawn yet (`display-mask` accepted, one warning); 8.0 draws them only with `display-mask=1` |
| Blur | ours | not done yet (`blur-bbox`, `blur-on-gie-class-ids` accepted, one warning); 8.0 blurs in GPU mode only |
| Batched input | ours | each frame meta drawn on its own surface, the slot at its `batch_id` (8.0 draws every frame's meta on surface 0); a `batch_id` outside the batch is skipped with one element warning |
| Tracker-created objects | matches nvdsosd (DeepStream 8.0) | drawn like any other object (confidence −0.1 changes nothing); nothing is added (no id text) |
| `has_color_info`, `color_id` | ours | ignored (Jetson colour table) |
| `process-mode` | ours | `0` draws on the CPU and `1` on the GPU, byte for byte the same (DeepStream 8.0's CPU mode draws differently); `2` (VIC on Jetson) draws on the GPU, with one debug-log warning per element |
| Ready on push | ours | the GPU modes wait for the element's stream after each pass, the CPU mode's syncs are synchronous: the frame is complete when pushed |

**Errors**, all element errors and never a fall back to the CPU: at start, no GPU (RESOURCE, NOT_FOUND) or no GPU
drawing set up (RESOURCE, FAILED); a failed GPU draw ("Cannot draw on the GPU", RESOURCE, FAILED); in the CPU mode a
frame that can't be mapped or synced between the GPU and the CPU (STREAM, FAILED); a buffer that can't be mapped for
drawing (STREAM, FAILED).

## Differences from DeepStream 8.0

- RGBA only; DeepStream 8.0's GPU mode also takes NV12.
- The CPU mode draws exactly what the GPU mode draws; DeepStream 8.0's CPU mode draws differently.
- Batched input is drawn per surface (8.0 draws every frame's meta on surface 0).
- DeepStream 8.0 bugs not copied: the GPU-mode crash on a NULL `font_name` (ours: `Sans`), `border_width` above 32
  stopping the pipeline (ours: drawn 32 wide with a warning), `BOTH_HEAD` drawing one head or none (ours: both).
- Masks and blur are not drawn yet.

## Known issues and limits

- **GPU faults under heavy host load** (ROCm 10.1 on the Ryzen 7000 gfx1036 iGPU, run as gfx1030 through
  `HSA_OVERRIDE_GFX_VERSION=10.3.0`, a configuration AMD doesn't support). When the host is busy (CPU and memory
  bandwidth), a kernel's writes now and then don't reach the next kernel on the same stream, with no error from HIP.
  The GPU modes (`process-mode` 1 and 2, the default) can then read a stale command block and fault the GPU:
  `Memory access fault` in `rds_osd_upload_kernel` or `rds_osd_draw_kernel`, then the process aborts. Under a
  synthetic load (parallel `dd bs=64M` and busy loops) `element_osd` failed in 30 of 38 runs; on an idle host it never
  failed. With the test suite's environment (`HSA_DISABLE_CACHE=1` and `HSA_ENABLE_SDMA=0`) it failed in 0 of 80
  runs. The CPU mode doesn't have the fault. Whether officially supported GPUs are affected is not known yet.
  Workarounds: `process-mode=0`, or `HSA_DISABLE_CACHE=1` (about 23 % slower).
- The first GPU pass in a fresh process needs the drawing kernels loaded up front: with HIP's lazy kernel loading
  (ROCm 10.1), a first launch could run before the command copy queued in front of it. `rdsosd` loads them when it
  sets up drawing, so this is handled; it matters for code that changes that setup.
- **Size limits** (ours):
  - circles and arrow heads with a coordinate, radius or width beyond 2^21 (2097152) pixels are not drawn; a wider
    `circle_width` is taken as 2^21;
  - `line_width` and `arrow_width` above twice the frame's larger side are clamped to it (the stroke covers the
    frame); lines are clipped to the frame first;
  - lines, arrows and circles are not drawn on frames wider or taller than 2^20 (1048576) pixels;
  - a text whose coverage mask (its glyphs' ink box clipped to the frame, a byte a pixel) is larger than the 8 MiB
    mask heap is not drawn, but its background box is (only huge fonts on large frames);
  - nothing is drawn on frames wider or taller than 2^21 pixels, or of more than 2^20 32 × 32 tiles (32768 × 32768);
    `rdsosd` takes 16384 × 16384 at most anyway.
- Masks (`display-mask`) and blur (`blur-bbox`, `blur-on-gie-class-ids`) are not implemented.
- Optional at build time: without cairo and pangocairo, `rdsosd` is not built.
