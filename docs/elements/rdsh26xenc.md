# rdsh264enc, rdsh265enc

<a id="rdsh264enc"></a><a id="rdsh265enc"></a>
`rdsh264enc` encodes single frames to H.264 and `rdsh265enc` to H.265, with DeepStream 8.0's encoder properties.
Each is a bin over GStreamer's stock VA encoder (`vah264enc`, `vah265enc`, on the GPU's VCN engine): the frames,
`memory:RDS` or system memory, are converted to NV12 on the GPU, copied into system memory and encoded. DeepStream
counterparts: `nvv4l2h264enc` and `nvv4l2h265enc`
([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html)).
Built when: always. At run time they need the stock `va` plugin and a Mesa VA driver that encodes the codec: without
them the elements still register, and going to READY is an element error (see [Errors](#errors)). The two share this
page: what only one of them has is under "Only on".

## Example

```sh
# 10 s of a test pattern to H.264 in an MP4 file
gst-launch-1.0 videotestsrc num-buffers=300 ! video/x-raw,width=1280,height=720,framerate=30/1 ! rdsvideoconvert \
  ! rdsh264enc bitrate=2000000 ! h264parse ! mp4mux ! filesink location=test-h264.mp4

# the same in H.265
gst-launch-1.0 videotestsrc num-buffers=300 ! video/x-raw,width=1280,height=720,framerate=30/1 ! rdsvideoconvert \
  ! rdsh265enc bitrate=2000000 ! h265parse ! mp4mux ! filesink location=test-h265.mp4

gst-discoverer-1.0 test-h264.mp4   # 0:00:10, H.264 (Constrained Baseline Profile), 1280x720, 30/1
```

Both need a GPU and the VA encoder. `rdsvideoconvert` in front puts the frames in `memory:RDS`, as the rest of a
Redstream pipeline has them; the encoders also take system memory directly. Batches from `rdsstreammux` go through
`rdsmultistreamtiler` or `rdsstreamdemux` first.

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)` or `video/x-raw`, format NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR or GRAY8, 1–16384 × 1–16384: what `rdsvideoconvert` takes. The sizes the VA encoder takes are narrower: see "Sizes" under [Stream](#stream) |
| `src` | src | always | `rdsh264enc`: `video/x-h264`, `stream-format=byte-stream`, `alignment=au`; `rdsh265enc`: `video/x-h265`, the same |

Both are ghost pads. Inside the bin, made when it first goes to READY:

```text
rdsvideoconvert ! video/x-raw,format=NV12 ! queue ! vah264enc ! video/x-h264,stream-format=byte-stream,alignment=au
```

(`vah265enc` and `video/x-h265` for `rdsh265enc`.)

## Properties

The names, types, ranges and defaults are the ones `gst-inspect-1.0` shows for DeepStream 8.0's encoders; the enum
type names are ours. The Changeable column says what really happens; the mutability flags `gst-inspect-1.0` shows
for DeepStream 8.0 are wrong both ways (see [Differences](#differences-from-deepstream-80)):

- `bitrate`, `force-idr` and `force-intra` take effect in any state, PLAYING included.
- Every other property that does something is read when the element starts (READY to PAUSED). Set after the start,
  it is stored with a GLib warning ("… set after the start is stored and applied at the next start (NULL or READY to
  PAUSED)") and applies at the next start (DeepStream 8.0 takes some silently and ignores others).
- An accepted no-op logs one GLib warning per property the first time it gets a value other than its default ("… is
  not implemented; the property is accepted and has no effect").
- At every start the bin sets the values on the VA encoder as the rows say; a value outside VA's range is clamped to
  it.
- `constqp`, `qp-range` and `initqp` take `:`- and `,`-separated QPs, each 0–51. A malformed string is refused with
  a GLib warning naming the property ("malformed … (expected …); keeping the previous value"), and the property keeps
  its value (ours: DeepStream 8.0 crashes). NULL unsets it.

GstBin's `async-handling` and `message-forward` are not listed.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `bitrate` | uint | `4000000` | 0–4294967295 | any state | bit/s: VA's `bitrate` in kbit/s (rounded, at least 1, at most VA's 2048000). Changeable in PLAYING (DeepStream 8.0 behaviour); on VA a change there forces an IDR frame, and after a drop the rate settles above the target (ours). See [Known issues](#known-issues-and-limits) about setting it from a notify handler |
| `control-rate` | enum `GstRdsEncRateControl` | `constant_bitrate` | `0` `variable_bitrate`, `1` `constant_bitrate`, `2` `constantQP` | any state (stored with a GLib warning, applied at the next start) | VA's `rate-control`: vbr, cbr, cqp. A mode VA lacks on the device: a GLib warning at start, and VA keeps its own |
| `maxbitrate` | uint | `0` | 0–4294967295 | any state (stored with a GLib warning, applied at the next start) | bit/s: with `variable_bitrate` and above `bitrate`, VA's `target-percentage` = 100 × bitrate / maxbitrate, at least VA's 50, so a peak above twice `bitrate` gives twice `bitrate` (ours); otherwise no effect (DeepStream 8.0 behaviour) |
| `constqp` | string | `NULL` | `I:P:B` | any state (stored with a GLib warning, applied at the next start) | VA's `qpi`, `qpp`, `qpb`, used with `constantQP` only (DeepStream 8.0 behaviour) |
| `qp-range` | string | `NULL` | `minP,maxP:minI,maxI:minB,maxB` | any state (stored with a GLib warning, applied at the next start) | VA's `min-qp` and `max-qp`, from the P pair (ours: VA has one range); NULL: VA's default range |
| `vbvbufsize` | uint | `0` | 0–4294967295 | any state (stored with a GLib warning, applied at the next start) | bits: VA's `cpb-size` in kbit (0: VA's choice) |
| `idrinterval` | uint | `256` | 0–4294967295 | any state (stored with a GLib warning, applied at the next start) | VA's `key-int-max`; 0 means 256 (DeepStream 8.0 behaviour); above 1024, 1024 with a GLib warning (ours) |
| `iframeinterval` | uint | `4294967295` | 0–4294967295 | any state (stored with a GLib warning, applied at the next start) | 0 and 4294967295: no I frames between IDR frames (DeepStream 8.0 behaviour; the docs say 30). Otherwise VA's `i-frames` = ⌈idrinterval / N⌉ − 1, which VA spaces evenly: the same frames as 8.0 when N divides idrinterval (ours) |
| `force-idr` | boolean | `false` | | any state | `true` makes the next frame into the VA encoder's queue an IDR frame with parameter sets; reads back `false`. See "Forced key frames" under [Stream](#stream) |
| `force-intra` | boolean | `false` | | any state | as `force-idr`: the frame is an IDR frame too (ours: VA can't force a non-IDR intra frame) |
| `num-B-Frames` | uint | `0` | 0–2 | any state (stored with a GLib warning, applied at the next start) | VA's `b-frames`. `rdsh264enc` with a Baseline profile (from the property or from downstream caps): a GLib warning and no B frames (ours: DeepStream 8.0 hangs, and VA alone writes B slices into the Baseline stream); when downstream allows Baseline and another profile and the property's isn't allowed, B frames pick the other (ours). `rdsh265enc`: VA on VCN 3 writes no B frames whatever the value |
| `num-Ref-Frames` | uint | `0` | 0–8 | any state (stored with a GLib warning, applied at the next start) | VA's `ref-frames` when above 0; 0 keeps VA's default, 3, the number DeepStream 8.0 uses |
| `insert-aud` | boolean | `false` | | any state (stored with a GLib warning, applied at the next start) | VA's `aud`: an access unit delimiter before every frame |
| `min-force-key-unit-interval` | uint64 | `0` | the uint64 range | any state (stored with a GLib warning, applied at the next start) | nanoseconds: VA's, the shortest time between two forced key frames |
| `qos` | boolean | `false` | | any state (stored with a GLib warning, applied at the next start) | VA's `qos`: drop frames on QoS events from downstream |
| `copy-meta` | boolean | `false` | | any state (stored with a GLib warning, applied at the next start) | copy the input buffers' metas onto the encoded buffers: see [Metadata](#metadata) |
| `gpu-id` | uint | `0` | 0–4294967295 | any state (stored with a GLib warning, applied at the next start) | the inner `rdsvideoconvert`'s `gpu-id` |
| `preset-id` | uint | `1` | 1–7 | any state | accepted no-op: VA's `target-usage` is always 1 (ours: the only setting that holds CBR at the target, and the fastest) |
| `insert-sps-pps` | boolean | `false` | | any state | accepted no-op: parameter sets always precede every IDR frame (ours) |
| `initqp` | string | `NULL` | `I:P:B` | any state | accepted no-op; malformed strings are refused as for `constqp` |
| `cq` | uint | `0` | 0–51 | any state | accepted no-op |
| `aq` | uint | `0` | 0–15 | any state | accepted no-op |
| `vbvinit`, `SliceIntraRefreshInterval` | uint | `0` | 0–4294967295 | any state | accepted no-op |
| `slice-header-spacing` | uint64 | `0` | the uint64 range | any state | accepted no-op |
| `temporalaq`, `extended-colorformat`, `bit-packetization`, `enable-lossless` | boolean | `false` | | any state | accepted no-op |
| `insert-vui` | boolean | `true` | | any state | accepted no-op |
| `intra-refresh` | string | `"1,30,1"` | `enable,period,count` | any state | accepted no-op, stored as given; DeepStream 8.0 shows this default but does not apply it |
| `tuning-info-id` | enum `GstRdsEncTuningInfo` | `LowLatencyPreset` | `1` `HighQualityPreset`, `2` `LowLatencyPreset`, `3` `UltraLowLatencyPreset`, `4` `LosslessPreset` | any state | accepted no-op |
| `sei-uuid` | string | `"NVDS_CUSTOMMETA"` | | any state | accepted no-op |
| `cudaenc-memtype` | enum `GstRdsEncCudaMemType` | `memtype_device` | `0` `memtype_device`, `1` `memtype_pinned`, `2` `memtype_unified` | any state | accepted no-op |
| `capture-io-mode`, `output-io-mode` | enum `GstRdsEncIOMode` | `auto` | `0` `auto`, `2` `mmap`, `5` `dmabuf-import` | any state | accepted no-op |
| `extra-controls` | `GstStructure` | — | | any state | accepted no-op |
| `device` | string | `"/dev/v4l2-nvenc"` | | read-only | DeepStream 8.0's value; nothing opens it |
| `device-fd` | int | `-1` | −1–2147483647 | read-only | always −1 |
| `device-name` | string | `""` | | read-only | always empty |

### Only on `rdsh264enc`

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `profile` | enum `GstRdsH264EncProfile` | `Baseline` | `0` `Baseline`, `1` `Constrained-Baseline`, `2` `Main`, `4` `High`, `17` `Constrained-High`, `7` `High444` | any state (stored with a GLib warning, applied at the next start) | the preferred profile, used when downstream caps allow it; downstream caps that fix `profile` win (DeepStream 8.0 behaviour). A pad probe narrows VA's downstream caps query to it (VA takes the highest profile allowed, whatever the order of a list). Baseline and Constrained-Baseline give constrained-baseline, Main main, High and Constrained-High high (ours: 8.0 fails on the two Constrained values). `High444` is an error at start naming the profile (ours: VA has no High 4:4:4) |
| `disable-cabac` | boolean | `false` | | any state (stored with a GLib warning, applied at the next start) | VA's `cabac` = !disable-cabac: `true` codes with CAVLC |

### Only on `rdsh265enc`

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `profile` | enum `GstRdsH265EncProfile` | `Main` | `0` `Main`, `1` `Main10`, `3` `FREXT` | any state (stored with a GLib warning, applied at the next start) | `Main` gives main. `Main10` and `FREXT` are an error at start naming the profile (ours: they need P010 or 4:4:4 input, which the bin can't produce yet) |

## Metadata

- **`copy-meta=true`:** the metas of the element's input buffer (`RdsBatchMeta` included, deep-copied; metas tied to
  the memory left out) are copied onto the encoded buffer of the same frame, found by its input PTS, so B-frame
  reordering doesn't matter. An input frame's metas are kept until its output, a flush or EOS, or until 64 newer
  frames came in.
- **`copy-meta=false`:** the encoded buffers carry no `RdsBatchMeta` (DeepStream 8.0 behaviour).
- Buffer flags come from the encoder either way (ours: DeepStream 8.0 copies the input's flags too, clearing
  DELTA_UNIT on every buffer).
- **Reads:** the frame count of each input buffer, from its `RdsBatchMeta` (see "Batches" under [Stream](#stream)).

## Behaviour

Pad probes on the inner elements, not extra elements, shape the VA encoder's output. In the tables, nvv4l2h26xenc
stands for nvv4l2h264enc and nvv4l2h265enc.

### Threads and memory

The VA encoder runs on a streaming thread of its own, behind a queue of up to 3 frames (unlimited in bytes and time),
and the inner converter downloads into a plain system-memory pool of its own, which VA copies into its surfaces (ours:
more than twice the frame rate of one thread writing into VA's mapped surfaces). So the bin adds a streaming thread and
holds up to 3 converted NV12 frames in the queue (5 in the pool); the queue adds nothing to the minimum latency and
makes the maximum unbounded, as any queue without a time limit does. After a resolution change the converter writes
into VA's own surfaces instead, which is slower (see [Known issues](#known-issues-and-limits)), and into plain memory
again back at the first size.

### Stream

| Behaviour | Status | Notes |
|---|---|---|
| Profile and level in the caps | ours | the output caps carry the profile and level the stream has (DeepStream 8.0 writes "NULL"). VA picks the level from the size, frame rate and bitrate, and ignores a downstream one |
| A downstream level at or above VA's | matches nvv4l2h26xenc (DeepStream 8.0) | it is written into every SPS (`level_idc`; H.264 "1b" as `level_idc` 11 with `constraint_set3_flag` in Baseline, Main and Extended, 9 in High) and, for H.265, every VPS and SPS (`general_level_idc`), and the output caps carry it |
| A downstream level below VA's, or a list of levels | ours | not-negotiated, with a GLib warning naming both levels: the stream would claim a level its size, frame rate or bitrate exceeds |
| Filler | matches nvv4l2h26xenc (DeepStream 8.0) | filler NAL units (H.264 type 12, H.265 type 38) are removed from the output, so CBR's bitrate is a ceiling and easy content comes out smaller (8.0's output has none). VA adds them once its rate-control buffer has filled, a few seconds into a CBR stream |
| Timestamps without B frames | matches nvv4l2h26xenc (DeepStream 8.0) | VA moves PTS, DTS and the segment forward so that the first PTS is at least 1000 hours (by 1000 hours minus the first PTS, measured again after a flush); the bin removes that shift: output PTS = DTS = the input PTS, durations and the segment are the input's |
| Timestamps with B frames | ours | VA's timestamps and segment stay as they are (running time is the input's, PTS reordered, DTS increasing and below PTS), since removing the shift would put the first DTS before 0. DeepStream 8.0 writes DTS = PTS and doesn't reorder PTS. Which case applies is decided from `num-B-Frames`: see [Known issues](#known-issues-and-limits) |
| DELTA_UNIT | matches nvv4l2h26xenc (DeepStream 8.0) | cleared on intra frames (IDR and I) and set on the others (VA alone clears it on IDR frames only). The first buffer is DISCONT |
| Parameter sets | ours | every IDR frame has them, so a reader can start at any IDR frame. DeepStream 8.0 writes them once at the start, and before later IDR frames only with `insert-sps-pps` |
| Batches | ours | a buffer whose `RdsBatchMeta` has more than one frame is an element error (see [Errors](#errors)). DeepStream 8.0 encodes the first frame of the batch only, silently. Put a tiler or a demuxer first |
| Frame-rate change | ours | the output caps carry the input caps' framerate, from the first frame at a new rate (VA restarts its sequence on a frame-rate change but keeps the old framerate in its caps, and sends none when nothing else changed). The VUI timing follows: that frame is an IDR frame whose SPS has the new rate (DeepStream 8.0 keeps the old rate in the VUI and writes no new SPS) |
| Forced key frames: which frame | ours | `force-idr` and `force-intra` force the next frame out of the inner converter into the queue, so frames already queued are not the ones forced (DeepStream 8.0 forces the frame after the one being processed). The setter never waits for the stream: the request waits for that frame, then goes to the VA encoder as an upstream force-key-unit event. `min-force-key-unit-interval` applies |
| Forced key frames: the frame | ours | an IDR frame with parameter sets, for `force-intra` too, and a force-key-unit event goes downstream before it. 8.0's forced IDR frame has none unless `insert-sps-pps` |
| Forced key frames: states | matches nvv4l2h26xenc (DeepStream 8.0) | they work in PLAYING and PAUSED (8.0's flags say NULL or READY only) and read back `false` (ours: 8.0's read back `true`). In NULL or READY they do nothing, as the first frame is an IDR frame anyway |
| Force-key-unit events: from outside | ours | from either direction (an upstream one from downstream, a downstream one from upstream) they reach the VA encoder, which makes the next frame (or the frame at the event's running time) an IDR frame with parameter sets, whatever `all-headers` says. DeepStream 8.0 ignores them |
| Force-key-unit events: timing | ours | a downstream one passes the queue in order with the frames; an upstream one without a running time waits, as `force-idr` does, for the next frame into the queue, and one with a running time goes to VA at once |
| Force-key-unit events: the one out | ours | the VA encoder sends a downstream force-key-unit event before the forced frame; its timestamp is the frame's PTS, in the output's time domain (without B frames the input's, like the buffers) |
| Resolution change, H.264 | matches nvv4l2h264enc (DeepStream 8.0) | new input caps mid-stream renegotiate the inner converter and the VA encoder: the first frame at the new size is an IDR frame with parameter sets and an SPS of the new size, and new output caps precede it |
| Resolution change, H.265 | ours | as H.264; DeepStream 8.0 hangs |
| Sizes | ours | what the VA encoder takes on system memory, read from its sink template: `rdsh264enc` 128 to 4096 on each side (DeepStream 8.0: width 145 and up, up to 4096×4096); `rdsh265enc` width 130 to 8192, height 128 to 4352 (DeepStream 8.0: width 129 and up, up to 8192×8192). The sink pad's caps queries answer only that range (its template stays the converter's), and input caps outside are not-negotiated, at the start or mid-stream, with a GLib warning naming the range, once per run (VA posts nothing, and without the check the inner converter would scale the frames into the range) |
| Odd sizes | matches nvv4l2h26xenc (DeepStream 8.0) | coded as the next even size, 1281×721 as 1282×722, the SPS cropping the coded picture to it (4:2:0 crops in 2-pixel units); the output caps carry the input size |
| H.265 conformance window | ours | the SPS before every IDR frame gets the window that crops the coded picture to the input size (rounded up to even), so the stream decodes at the input size. VA on Mesa 25.2.8 pads the width to a multiple of 64 but computes the window from a multiple of 16, so 720x480 would decode 768 wide and 1366x768 1398 wide; DeepStream 8.0's streams decode at the input size. After a resolution change, from the new size |
| System-memory input | ours | `memory:RDS` and system memory both work; DeepStream 8.0 takes NVMM only |

### Errors

All element errors.

- Going to READY:
  - no VA encoder for the codec (no `va` plugin, or a driver that can't encode it): "The VA encoder 'vah264enc' is
    not available" (CORE, MISSING_PLUGIN);
  - a missing inner element, `rdsvideoconvert`, `capsfilter` or `queue` (CORE, MISSING_PLUGIN);
  - inner elements that don't link (CORE, NEGOTIATION).
- At start (READY to PAUSED): a profile the bin can't encode, `High444`, `Main10` or `FREXT`, naming it (LIBRARY,
  SETTINGS).
- While playing: a buffer with more than one frame, "<element> takes one frame per buffer: add a tiler or a demuxer
  before it" (STREAM, WRONG_TYPE), `<element>` being `rdsh264enc` or `rdsh265enc`.

Sizes outside VA's range and a downstream level below VA's are not-negotiated, not element errors.

## Differences from DeepStream 8.0

The differences, most of them DeepStream 8.0 bugs the bins don't reproduce (details in the rows above):

- **B-frame timestamps:** 8.0 writes DTS = PTS and leaves PTS in decode order; here PTS is reordered and DTS
  increases.
- **Baseline with B frames:** 8.0 hangs; here B frames are dropped with a warning (`num-B-Frames`).
- **High with B frames and I frames between IDR frames:** 8.0 hangs after 10 frames with `profile=High`,
  `num-B-Frames=2`, `iframeinterval=10` and `idrinterval=20`; here it encodes (60 frames checked, B frames
  reordered).
- **Force-key-unit events:** 8.0 ignores them; here they force an IDR frame with parameter sets.
- **H.265 resolution change:** 8.0 hangs; here the stream renegotiates, as for H.264.
- **Caps profile and level:** 8.0 writes "NULL"; here the caps carry the stream's own.
- **VUI timing after a frame-rate change:** 8.0 keeps the old rate in the VUI (and writes no new SPS); here the next
  frame is an IDR frame whose SPS has the new rate.
- **H.265 `force-intra`:** 8.0 gives an I picture in a TRAIL_R NAL unit, not a random access point, yet flagged as a
  key frame; here it is an IDR frame.
- **H.264 Constrained-Baseline and Constrained-High:** 8.0 fails to negotiate with either value; here they give
  constrained-baseline and high.
- **`copy-meta` and buffer flags:** 8.0 copies the input's flags too, clearing DELTA_UNIT on every buffer; here the
  flags are the encoder's.
- **Malformed `constqp`, `qp-range`, `initqp` and `intra-refresh` strings:** 8.0 crashes; here the QP strings are
  refused with a warning, and `intra-refresh`, a no-op, is stored as given.
- **Batches:** 8.0 silently encodes the first frame of a batch only; here a batch of more than one frame is an
  element error.
- **System memory:** 8.0 takes NVMM input only; here system memory works too.
- **Parameter sets:** 8.0 writes them once at the start, and before later IDR frames only with `insert-sps-pps`;
  here every IDR frame has them, so a reader can start at any IDR frame.
- **Property flags:** 8.0's are wrong both ways (`force-idr` and `force-intra` are flagged NULL or READY only but
  work in PLAYING; `iframeinterval`, `insert-sps-pps` and `disable-cabac` are writable there but do nothing); here
  `bitrate`, `force-idr` and `force-intra` change in PLAYING, and every other property set after the start warns
  and applies at the next one.

## Known issues and limits

- **Not zero-copy:** every frame is converted on the GPU, copied into system memory and uploaded again by the VA
  encoder: about 0.5 ms per 1080p frame for the copy alone (NV12 already on the GPU, into plain memory; release build,
  about 0.7 ms with `HSA_DISABLE_CACHE=1`) on a Ryzen 7000 iGPU, or about 1.4 ms with the RGBA to NV12 conversion
  before it (an earlier, rougher timing, the test source included). See the README's
  [Zero-copy and where it stops](../../README.md#zero-copy-and-where-it-stops).
- **GStreamer 1.28's `vah26xenc` leaks one `GstCaps` per reconfiguration** (a frame-rate or size change, or a live
  `bitrate` change), reproduced without the bins. So in a long run that changes the frame rate, the leaked caps are
  VA's. Under ASan, `element_enc` skips its subtests that reconfigure the VA encoder, and `element_enc_reconfig` runs
  them with a suppression file of their own (`tests/lsan-va-reconfig.supp.in`): the leak can only be suppressed by
  GStreamer's caps functions, which would hide the bins' own caps leaks in the other subtests.
- **GStreamer 1.28's `vah26xenc` builds its copy pool for system-memory input once per start**, so after a
  resolution change it would copy at the old size (a `gst_video_frame_copy` critical on a shrink, a partial frame on a
  grow). The bins let their converter write plain memory only at a run's first size and VA's own surfaces after a
  resize, which is slower.
- **Setting `bitrate` from a notify handler deadlocks:** a `notify` or `deep-notify` handler of the bin or its VA
  encoder that sets `bitrate`, and runs during a start or a bitrate change, deadlocks, as the lock that orders the two
  is not recursive.
- **The timestamp shift is decided from the B frames the bin asks VA for** (`num-B-Frames`, 0 with a Baseline
  profile), not the ones VA uses. When VA drops them, the output keeps VA's shift though it has no B frames: PTS and
  DTS are about 1000 hours ahead of the input's. The segment is shifted to match, so synchronization and running
  time are right, and `copy-meta` still finds each frame's metas (it matches on the output PTS minus VA's shift,
  which the bin measures either way); code that compares raw PTS, or a muxer that writes raw PTS, sees the offset.
  VA drops them for H.264 with `num-Ref-Frames=1` (0 keeps VA's 3), with `idrinterval` 1 or 2, and on drivers
  without L1 references, likely why `rdsh265enc` on VCN 3 writes none. Seen on the gfx1036 iGPU, Mesa 25.2.8:
  `num-Ref-Frames=1`, `idrinterval=1` and `idrinterval=2` gave no reordered frames with `num-B-Frames=2`, and
  `idrinterval=3` did. With `num-B-Frames=0` there is no shift, so don't ask for B frames where VA won't make them.
- **H.265 widths that are not a multiple of 64:** `vah265enc` on Mesa 25.2.8 (radeonsi, as in `redstream:dev`)
  signals them too wide (720×480 decodes 768 wide). `rdsh265enc` rewrites the SPS conformance window (see
  [Stream](#stream)), so its output decodes at the right size; bare `vah265enc` is affected.
- No P010 or 4:4:4 input yet, so no `Main10`, `FREXT` or `High444`.
