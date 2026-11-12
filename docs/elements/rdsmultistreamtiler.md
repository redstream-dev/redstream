# rdsmultistreamtiler

How to read this page, and how to run the examples: [element reference](README.md).

Composes a batch from `rdsstreammux` into one frame on the GPU: a rows × columns grid with one cell per source, and
one merged frame meta whose objects and display metas are scaled into the cells. DeepStream counterpart:
`nvmultistreamtiler` ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvmultistreamtiler)).
Built when: always.

## Example

```sh
# three sources in a 2x2 grid at 1280x720 (the fourth cell stays black)
gst-launch-1.0 rdsstreammux name=m batch-size=4 width=640 height=360 \
  ! rdsmultistreamtiler rows=2 columns=2 width=1280 height=720 ! fakesink sync=false \
  videotestsrc num-buffers=100 ! video/x-raw,format=NV12 ! m.sink_0 \
  videotestsrc num-buffers=100 pattern=ball ! video/x-raw,format=NV12 ! m.sink_1 \
  videotestsrc num-buffers=100 pattern=snow ! video/x-raw,format=NV12 ! m.sink_2
```

In a display pipeline the tiler goes before `rdsosd`, which draws the merged metadata on the tiled frame, and
`rdsvideosink` (see [rdsosd](rdsosd.md)).

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)`, format NV12 or RGBA, 1–16384 × 1–16384: batches with an `RdsBatchMeta` |
| `src` | src | always | `video/x-raw(memory:RDS)`, the input's format and frame rate, at `width` × `height` (rounded) |

- There is no conversion: the output has the input's format. Put `rdsvideoconvert` after the tiler for another
  format (`rdsosd` takes RGBA).
- The input batch must be in memory the GPU can reach (device, pinned or unified). A system-memory batch is an element
  error, "System-memory batches are not supported" (STREAM, NOT_IMPLEMENTED; ours). Device memory on a GPU other than
  `gpu-id` is an element error naming both (STREAM, FAILED; ours).

## Properties

Names, types, ranges and defaults are those `gst-inspect-1.0` shows for DeepStream 8.0's nvmultistreamtiler (its 8.0
page omits `buffer-pool-size` and `interpolation-method`). The enum nicks are ours. `custom-tile-config` set to a
non-NULL value logs one GLib warning per element ("custom-tile-config is reserved (as in DeepStream) and has no
effect").

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `rows`, `columns` | uint | `1` | 1–4294967295 | any state | the grid; grown when the sources don't fit (see the Grid row under [Behaviour](#behaviour)). A change applies from the next batch |
| `width` | uint | `1920` | 16–4294967295 | any state | the output width, rounded up to a multiple of 4; at most 16384 negotiates. A change renegotiates the output caps |
| `height` | uint | `1080` | 16–4294967295 | any state | the output height, as `width` |
| `show-source` | int | `-1` | −1–2147483647 | any state | a pad index: that source alone, scaled to the whole output; `-1` tiles them all. Read on every batch |
| `square-seq-grid` | boolean | `false` | | any state | a ⌈√n⌉ × ⌈√n⌉ grid for n sources; `rows` and `columns` are ignored. Cells are still by pad index |
| `interpolation-method` | enum `GstRdsTilerInterpolationMethod` | `default` | `0` `nearest`, `1` `bilinear`, `2` `algo-1`, `3` `algo-2`, `4` `algo-3`, `5` `algo-4`, `6` `default` | any state | the scaling filter: `default` and `algo-4` sample the nearest pixel, `algo-1` to `algo-3` run bilinear |
| `buffer-pool-size` | uint | `5` | 4–10 | NULL, READY | buffers in the output pool |
| `nvbuf-memory-type` | enum `GstRdsTilerMemoryType` | `nvbuf-mem-cuda-device` | `0` `nvbuf-mem-default`, `1` `nvbuf-mem-cuda-pinned`, `2` `nvbuf-mem-cuda-device`, `3` `nvbuf-mem-cuda-unified` | NULL, READY | the memory of the output buffers |
| `gpu-id` | uint | `0` | 0–4294967295: a visible ROCm device | NULL, READY | the GPU that tiles; one that doesn't exist is an element error at start, "GPU N is not available" (RESOURCE, NOT_FOUND) |
| `compute-hw` | enum `GstRdsTilerComputeHW` | `default` | `0` `default`, `1` `gpu`, `2` `vic` | NULL, READY | `0` and `1` run on the GPU; `2` (Jetson's VIC) is an element error at start, "compute-hw=VIC is not available" (RESOURCE, SETTINGS) |
| `custom-tile-config` | `gpointer` | — | | any state | accepted no-op (write-only); reserved in DeepStream too |
| `qos` | boolean | `false` | | any state | GstBaseTransform's |

## Metadata

- **Reads:** the batch's `RdsBatchMeta` (`NvDsBatchMeta`) under its meta lock: each frame meta's `pad_index` (its
  cell) and `batch_id` (its slot), its object metas and display metas.
- **Writes:** a new `RdsBatchMeta` on the output with one fresh frame meta (`num_frames_in_batch` and
  `max_frames_in_batch` 1), holding a copy of every object and display meta of the batch, scaled into its cell (see
  "Meta coordinates"). The new frame meta's `pad_index`, `source_id`, `batch_id`, `frame_num`, `buf_pts`,
  `ntp_timestamp`, `source_frame_*`, `pipeline_*` and `surface_index` are 0. Frame user meta and batch user meta are
  not carried: per-source counts belong before the tiler.

## Behaviour

Each row is either **matches nvmultistreamtiler (DeepStream 8.0)**: observed DeepStream 8.0 behaviour where the docs
are silent, or **ours**: where 8.0 settles nothing or is clearly broken. The pixels go through
`rds_buf_surf_transform_composite`; the compatibility matrix's
["Composite semantics"](../compat-matrix.md#composite-semantics) has its exact rules.

| Behaviour | Status | Notes |
|---|---|---|
| Cell of a frame | matches nvmultistreamtiler (DeepStream 8.0) | the frame meta's `pad_index`, row-major from the top left; `source_id` plays no part. The pixels come from the batch slot at the frame's `batch_id` |
| Grid | matches nvmultistreamtiler (DeepStream 8.0) | n = highest pad index seen + 1, from `rds-pad-added` events ([rdsstreammux](rdsstreammux.md) sends them). `rows` × `columns` while n fits; otherwise ⌊√n⌋ rows and ⌈n / rows⌉ columns (2: 1×2, 3: 1×3, 5: 2×3, 7: 2×4). Recomputed when a pad is added |
| Grid grown from frame metas | ours | a frame meta's pad index beyond n grows the grid too (8.0 fails with a fatal composite error on a pad index only a frame meta has) |
| Grid after a source leaves | ours | never shrunk (8.0 not observed) |
| Cell geometry | matches nvmultistreamtiler (DeepStream 8.0) | output size rounded up to a multiple of 4 (1001×701 gives 1004×704); cells ⌊W / columns⌋ × ⌊H / rows⌋, each rounded down to even, at (column·cell width, row·cell height) |
| Scaling | matches nvmultistreamtiler (DeepStream 8.0) | each frame stretched over its whole cell (no letterbox), with its own size |
| Unused cells and the strip right and below the cells | matches nvmultistreamtiler (DeepStream 8.0) | black on every output frame: NV12 Y 16, U/V 128; RGBA all bytes 0 (ours for the alpha byte, as `rdsvideoconvert`'s fill) |
| Output meta | matches nvmultistreamtiler (DeepStream 8.0) | one merged frame meta, as under [Metadata](#metadata) |
| Order of the merged objects | ours | by pad index, each pad's objects (then display metas) in its list order |
| Meta coordinates | matches nvmultistreamtiler (DeepStream 8.0) | `rect_params` of objects and display rects: x' = cell x + x·sx, y' = cell y + y·sy, w·sx, h·sy as floats (sx = cell width / frame width); text offsets, line and arrow end points and circle centres the same, truncated to integers; circle radius × min (sx, sy), truncated. Unchanged: `font_size`, all widths, `detector_bbox_info`, `tracker_bbox_info`, `mask_params`, ids, class, confidence, labels. Not clipped to the cell. Example: a 1280-wide frame in a 640-wide cell at (640, 0), so sx 0.5: x 100 becomes 640 + 100 × 0.5 = 690 |
| A source missing from a batch | matches nvmultistreamtiler (DeepStream 8.0) | its cell keeps its last pixels and its last objects and display metas are repeated, until its `rds-stream-eos` (or `rds-pad-deleted`); then the cell is black and its meta gone. Its pixels survive a grid change (moved into the new cell) |
| Two frames of one pad in a batch | ours | both drawn, the later on top; the later one's meta is kept |
| `show-source` | matches nvmultistreamtiler (DeepStream 8.0) | by pad index: that frame stretched to the output with only its meta, scaled to the output; a pad that never had a frame or has ended: black and an empty frame meta; a value of at least rows × columns: element error on the next batch ("show-source N is not a cell of the R×C grid"). Changes apply from the next batch |
| `show-source` on a live pad missing from the batch | ours | black and an empty frame meta, as for an ended pad (8.0 not observed); the grid would repeat such a pad's last frame instead. The grid keeps its cached cells meanwhile, so switching back to -1 shows them |
| Live changes with empty cells | ours | `rows`, `columns`, `square-seq-grid`, `width` or `height` changed while playing to values whose cells would be empty: ignored with one WARNING message (STREAM, FAILED), the current grid kept |
| A pad the grid can't hold | ours | a pad index whose grid would have empty cells (narrower or lower than 2 pixels) is skipped, with one WARNING message, whether it comes from a frame meta or an `rds-pad-added` event (also one before the caps) (8.0: fatal composite error) |
| Empty batch | ours | no frames: a black frame with an empty frame meta, as the docs say the muxer sends one to clear the tiler's output (8.0 crashes on it, per the docs' troubleshooting page). The cached cells and meta stay: the next batch repeats absent sources again |
| A frame meta whose `batch_id` is outside the batch | ours | skipped, with a debug-log warning |
| Events | ours | every event goes downstream unchanged; FLUSH_STOP forgets every cached cell and meta |
| Ready on push | ours | the composite, copies and clears are synchronous on the element's own HIP stream; the output is complete when pushed |

`rds-pad-added`, `rds-stream-eos` and `rds-pad-deleted` are `rdsstreammux`'s stream events for one source
(`rds/rds-event.h`).

**Errors**, all element errors: at start, the GPU (RESOURCE, NOT_FOUND), VIC (RESOURCE, SETTINGS) or a HIP stream
(RESOURCE, FAILED); a tiling or copy surface that can't be allocated (RESOURCE, NO_SPACE_LEFT); sources that can't be
laid out at all ("Cannot lay out N sources on W×H", STREAM, FAILED); a failed composite ("Tiling failed"), output
clear or copy, or a buffer that can't be mapped (STREAM, FAILED).

## Differences from DeepStream 8.0

- Cells by pad index and the √n auto-grid match DeepStream 8.0; ours grows the grid for any pad index it sees and
  never shrinks it.
- An empty batch gives a black frame (8.0 crashes); a pad index the grid can't hold is skipped with a warning (8.0:
  a fatal composite error).
- Live changes that would leave empty cells are ignored with a warning.
- System-memory batches and batches on another GPU are element errors.
- The alpha byte of the black fill is 0 on RGBA.

## Known issues and limits

- No format conversion: NV12 in, NV12 out; RGBA in, RGBA out.
- Frame and batch user metas are dropped (as in DeepStream); read them before the tiler.
- The grid never shrinks while the element runs.
