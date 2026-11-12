# DeepStream compatibility matrix

What `rds/rds_compat.h` maps, and how well. Every DeepStream name here was checked against NVIDIA's public
online documentation (the Source column links the page); Redstream's implementation is independent.

Status values: **supported** (same meaning), **accepted no-op** (compiles and is ignored), **unsupported**
(compiles, then fails at run time), **different default** (supported, but the default value differs), **planned**
(aliased, element not written yet), **in progress** (partly written: the Notes say what is done). In element property
tables also: **accepted** (the value is stored and read back, and does what the Notes say rather than what DeepStream
does with it), **forwarded** (`rdsvideosink`: set on the stock sink inside it) and **ours** (a Redstream-only
property). Names Redstream does not alias at all are listed under "Not aliased" in each section: code using them does
not compile.

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
| `nvurisrcbin` | `rdsurisrcbin` | supported | decodes file and other non-RTSP URIs (uridecodebin3, first video stream only) to `memory:RDS` NV12 on `vsrc_0`, with `file-loop` and `drop-frame-interval`; RTSP URIs through `rtspsrc` and `rdsvideodec` (H.264 and H.265), with reconnection after timeouts, errors and the server's EOS; properties and reconnection behaviour below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvurisrcbin) |
| `nvv4l2decoder` | `rdsvideodec` | supported | rocDecode: H.264/H.265 8-bit 4:2:0 to NV12; rank primary+10, so `uridecodebin` picks it over the VA decoders; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvvideo4linux2), [name](https://docs.nvidia.com/deepstream/text/DS_FAQ) |
| `nvvideoconvert` | `rdsvideoconvert` | supported | single frames (batch size 1); `memory:RDS` and system memory on either side; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvvideoconvert) |
| `nvstreammux` | `rdsstreammux` | supported | the default (scaling) mux only; the new mux is not implemented yet. GstAggregator-based; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreammux) |
| `nvstreamdemux` | `rdsstreamdemux` | supported | zero-copy read-only views of the batch slots; properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreamdemux) |
| `nvinfer` | `rdsinfer` | supported | primary detector only (`process-mode=1`, `network-type` 0 or 100) on MIGraphX; nvinfer config files (key file or YAML); properties below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvinfer) |
| `nvtracker` | `rdstracker` | supported | the IOU and NvSORT trackers of NvMultiObjectTracker, on the CPU, with its YAML config files; NvDCF and NvDeepSORT configs run as NvSORT. Matches observed DeepStream 8.0 behaviour where the docs are silent; properties, config keys and behaviour below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvtracker) |
| `nvmultistreamtiler` | `rdsmultistreamtiler` | supported | NV12 and RGBA `memory:RDS` batches into one tiled frame of the same format; matches observed DeepStream 8.0 behaviour where the docs are silent; properties and behaviour below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvmultistreamtiler) |
| `nvdsosd` | `rdsosd` | supported | RGBA `memory:RDS` frames (single or batched), drawn on the GPU (`process-mode` 1, the default) or the CPU (`process-mode=0`), the same pixels either way, in the geometry of DeepStream 8.0's default GPU mode (Pango for text); masks and blur not yet; optional (`-Dosd`, needs cairo and pangocairo); properties and behaviour below | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsosd) |
| `nvdsanalytics` | `rdsanalytics` | supported | memory:RDS batches, any format; ROI filtering, overcrowding, direction (with STOPPED) and line crossing on the CPU, with DeepStream-shaped frame and object metas (`rds/rds-analytics-meta.h`, libgstrds-analytics) and nvdsanalytics config files; matches observed DeepStream 8.0 behaviour where the docs are silent, except where 8.0 is clearly broken; properties, behaviour, drawing and config keys below. The `osd-mode` overlays are display meta, drawn by `rdsosd` | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsanalytics) |
| `nvdslogger` | `rdslogger` | supported | passthrough, any caps; prints each stream's frame rate to stdout in DeepStream 8.0's `**PERF` line format; matches observed DeepStream 8.0 behaviour where the docs are silent; properties below, behaviour on [its page](elements/rdslogger.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdslogger) |
| `nveglglessink` | `rdsvideosink` | supported | display bin: `rdsvideoconvert` to system-memory BGRx, then a stock GStreamer sink (`video-sink`, default `autovideosink`); one frame per buffer; properties and behaviour below | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |
| `nv3dsink` | `rdsvideosink` | supported | as `nveglglessink` (nv3dsink's properties are a subset of nveglglessink's) | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |
| `nvv4l2h264enc` | `rdsh264enc` | supported | a bin over the stock VA encoder `vah264enc` (VCN): `rdsvideoconvert` to system-memory NV12, a queue, then VA on a thread of its own; `memory:RDS` or system-memory input, one frame per buffer; DeepStream 8.0's properties; properties and stream behaviour below | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html) |
| `nvv4l2h265enc` | `rdsh265enc` | supported | the same bin over `vah265enc`, which also fixes the SPS conformance window of widths that are not a multiple of 64; properties and stream behaviour below | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html) |

### `rdsurisrcbin` (`nvurisrcbin`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvurisrcbin))

Names, types, ranges and defaults follow DeepStream 8.0 behaviour (the element as shipped), and so does which
properties are changeable only in the NULL or READY state (`GST_PARAM_MUTABLE_READY`). Where NVIDIA's nvurisrcbin
pages differ, the Notes column says so: the pages document `select-rtp-protocol`'s default as 0, and give only an
example value, with no default, for `source-id`, `buffer-mode`, `leaky`, `max-size-buffers`, `disable-passthrough`,
`rtsp-reconnect-interval` and `rtsp-reconnect-attempts`. Two properties DeepStream 8.0 lacks come from the 9.2 page:
`simulate-fps-interval-ms` and `ipc-frame-copy`. The bin decodes URIs other than `rtsp*://` with `uridecodebin3`, and
`rtsp*://` URIs with `rtspsrc`, the RTP depayloader, `h264parse` or `h265parse` and `rdsvideodec`, and reconnects
them (see the reconnection behaviour below). "Accepted" below means the value is stored and read back and does
nothing else so far. Accepted no-ops log a warning (`GST_WARNING`, category `rdsurisrcbin`) whenever they are set to
a non-default value.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `uri` | string | NULL | supported | checked at start (NULL to READY): unset or empty, not a valid URI, or a scheme no source element handles is an error naming `uri`; the message names the scheme, never the whole URI (it may hold credentials). Decoded by `uridecodebin3`, except `rtsp*://` URIs: `rtspsrc` (made going from READY to PAUSED; the bin is live, PAUSED returns NO_PREROLL), then the H.264 or H.265 depayloader, parser (`config-interval=-1`) and `rdsvideodec`. Only H.264 and H.265 video streams are set up; another video codec (JPEG, say) is an error (STREAM, CODEC_NOT_FOUND) naming its `encoding-name`. Without reconnection, an `rtspsrc` error reaches the bus as it is, and the end of the stream (the server's EOS, or a closed TCP connection, which `rtspsrc` takes as one) is EOS on `vsrc_0` |
| `type` | enum `auto` (0), `uri` (1), `rtsp` (2) | `auto` | supported | the source follows the URI's scheme whatever the value; `rtsp` with a URI that is not `rtsp*://` is an error at start naming `type` (ours: DeepStream 8.0 accepts the URI and plays it) |
| `file-loop` | boolean | `FALSE` | supported | DeepStream 8.0 behaviour: at the end of a `file://` URI the file plays again from the start, with no EOS, flush, second SEGMENT or STREAM_START downstream; the one SEGMENT is open-ended, PTS go on from the end of the previous pass (DTS too: ours, DeepStream 8.0 offsets PTS only), and each pass's first buffer is DISCONT. A flushing seek from downstream goes through as usual. An EOS the application sends to the bin or pipeline gets out as EOS and ends the loop (ours: DeepStream 8.0 not observed). On a URI that is not `file://`: a `GST_WARNING` log at start (no WARNING message), and the URI plays once |
| `latency` | uint | `100` | supported | milliseconds: `rtspsrc`'s `latency` (RTSP) |
| `drop-on-latency` | boolean | `TRUE` | supported | `rtspsrc`'s `drop-on-latency` (RTSP), taken when the session starts (READY to PAUSED) |
| `select-rtp-protocol` | enum `rtp-unknown` (0), `rtp-udp` (1), `rtp-udp-mcast` (2), `rtp-tcp` (4), `rtp-udp-udpmcast-tcp` (7), `rtp-http` (10), `rtp-tls` (20) | `rtp-udp-udpmcast-tcp` | supported | DeepStream 8.0 behaviour; the docs' default is 0 (`rtp-multi`). `rtspsrc`'s `protocols` (RTSP): UDP, UDP multicast, TCP, all three, HTTP + TCP, TLS + TCP; 0 leaves `rtspsrc`'s default (UDP, UDP multicast and TCP). `rtspsrc` gets a 100 ms `teardown-timeout` too (ours) |
| `udp-buffer-size` | uint | `524288` | supported | bytes: `rtspsrc`'s `udp-buffer-size` (RTSP; at most G_MAXINT) |
| `rtsp-reconnect-interval` | uint | `0` | supported | seconds without a buffer from the RTSP session before an attempt; 0: no reconnection. Above 0, the session's errors and its EOS start an attempt too, at once (ours), and its ERRORs are posted as WARNINGs. DeepStream 8.0 behaviour; the docs' example value is 10 |
| `rtsp-reconnect-attempts` | int | `-1` | supported | consecutive failed attempts before giving up (a buffer resets the count); any negative value: unlimited (ours; the docs give −1 and a range from −1). DeepStream 8.0 behaviour; the docs' example value is 4 |
| `init-rtsp-reconnect-interval` | uint | `0` | supported | seconds between retries while the first connection fails (ours: DeepStream 8.0 accepts it and does nothing); 0: retry every `rtsp-reconnect-interval` (DeepStream 8.0 behaviour), and with both 0 the first failure is an ERROR |
| `drop-frame-interval` | uint 0..30 | `0` | supported | DeepStream 8.0 behaviour: N keeps decoded frames 0, N, 2N, … (counted across file loops) and drops the rest, durations unchanged; 0 and 1 keep every frame. Frames are dropped before any conversion |
| `source-id` | int −1.. | `-1` | supported | the `source-id` field of the reconnection messages (`attempt-exceeded` has it as a uint, −1 as 4294967295, as in DeepStream 8.0). DeepStream 8.0 behaviour; the docs' example value is 0 |
| `gpu-id` | uint | `0` | accepted | |
| `disable-audio` | boolean | `TRUE` | supported | audio is never decoded or exposed, whatever the value: for files only the first video stream is selected; for RTSP, TRUE refuses every stream that is not video before SETUP, and FALSE sets them up and discards them inside the bin. Taken when the session starts (READY to PAUSED). An RTSP source with no video stream is an error (STREAM, CODEC_NOT_FOUND) |
| `message-forward` | boolean | `FALSE` | supported | GstBin's own |
| `async-handling` | boolean | `TRUE` | supported | GstBin's own (GstBin's default is FALSE; nvurisrcbin's is TRUE) |
| `smart-record` | enum `smart-rec-disable` (0), `smart-rec-cloud` (1), `smart-rec-multi` (2) | `smart-rec-disable` | unsupported | anything but 0 is an error at start naming `smart-record` |
| `smart-rec-status` | boolean, read-only | `FALSE` | supported | always FALSE: smart record never runs |
| `smart-rec-dir-path` | string | NULL | accepted no-op | |
| `smart-rec-file-prefix` | string | `Smart_Record` | accepted no-op | |
| `smart-rec-video-cache`, `smart-rec-cache` | uint | `0` | accepted no-op | |
| `smart-rec-container` | enum `smart-rec-mp4` (0), `smart-rec-mkv` (1) | `smart-rec-mp4` | accepted no-op | |
| `smart-rec-mode` | enum `smart-rec-mode-av` (0), `smart-rec-mode-video` (1), `smart-rec-mode-audio` (2) | `smart-rec-mode-av` | accepted no-op | |
| `smart-rec-default-duration` | uint | `20` | accepted no-op | |
| `num-extra-surfaces` | uint | `1` | accepted no-op | |
| `cudadec-memtype` | enum `memtype_device` (0), `memtype_pinned` (1), `memtype_unified` (2) | `memtype_device` | accepted no-op | |
| `dec-skip-frames` | enum `decode_all` (0), `decode_non_ref` (1), `decode_key` (2) | `decode_all` | accepted no-op | |
| `low-latency-mode` | boolean | `FALSE` | accepted no-op | |
| `ipc-buffer-timestamp-copy` | boolean | `FALSE` | accepted no-op | |
| `ipc-frame-copy` | boolean | `FALSE` | accepted no-op | from the 9.2 page; DeepStream 8.0 does not have it |
| `ipc-connection-attempts` | int −1.. | `-1` | accepted no-op | |
| `ipc-connection-interval` | uint64 | `1000000` | accepted no-op | microseconds |
| `ipc-socket-path` | string | NULL | accepted no-op | |
| `extract-sei-type5-data` | boolean | `FALSE` | accepted no-op | |
| `sei-uuid` | string | NULL | accepted no-op | |
| `simulate-fps-interval-ms` | uint 0..1000 | `0` | accepted no-op | from the 9.2 page; DeepStream 8.0 does not have it |
| `buffer-mode` | enum `unknown` (0), `slave` (1), `buffer` (2), `auto` (3), `synced` (4) | `auto` | accepted no-op | DeepStream 8.0 behaviour; the docs' example value is 0 |
| `leaky` | enum `no-leak` (0), `upstream-leak` (1), `downstream-leak` (2) | `no-leak` | accepted no-op | DeepStream 8.0 behaviour; the docs' example value is 0 |
| `max-size-buffers` | uint | `200` | accepted no-op | DeepStream 8.0 behaviour; the docs' example value is 0 |
| `disable-passthrough` | boolean | `TRUE` | accepted no-op | DeepStream 8.0 behaviour; the docs' example value is 0 |

Pads: `vsrc_%u` (sometimes): `NV12` in `memory:RDS`, what `rdsvideodec` outputs, or in system memory. As in DeepStream
8.0 behaviour, a new bin has no pad. For a non-RTSP URI it adds the ghost pad `vsrc_0` when the first video stream is
decoded (one STREAM_START, CAPS and SEGMENT, then the frames), keeps it across file loops and removes it going from
PAUSED to READY (ours: DeepStream 8.0 removes it at NULL); there is never a second pad. A decoder that can't output
`memory:RDS` (JPEG, say) gets `rdsvideoconvert` behind it, so `vsrc_0` carries `memory:RDS` NV12 either way. For an RTSP
URI, `vsrc_0` is added when `rdsvideodec` has its first caps, so the application's `pad-added` sees them as the pad's
current caps; then one STREAM_START, CAPS and SEGMENT, and the frames. An RTSP stream in a codec other than H.264 or
H.265 gets no pad. There is no audio pad template: audio is ignored. Rank none: `uridecodebin` and `decodebin3` never
pick it.

#### `rdsurisrcbin` reconnection behaviour

An attempt replaces the RTSP session (`rtspsrc` and the depayloader); the parser and `rdsvideodec` stay. Attempts run
on GStreamer's thread pool, one at a time; the watchdog ticks once a second on the system clock while PLAYING.

| Behaviour | DeepStream 8.0 | `rdsurisrcbin` |
|---|---|---|
| no buffer for `rtsp-reconnect-interval` s | an attempt (watchdog) | the same; reason `timeout` |
| an ERROR, or the server's EOS (or a closed TCP connection) | waits for the watchdog | an attempt at once (ours); reason `error` or `eos`; the EOS never leaves the bin. The EOS the application sends goes through |
| the session's ERRORs while reconnection is on | posted as WARNINGs | the same; the bin's own about a session it can't use (no video stream, a codec it can't decode) too, each a failed attempt (ours) |
| the application's EOS | not observed while the source is down | ends the stream in any state (ours): no attempt after it, EOS on `vsrc_0` through the decoder. With no `vsrc_0` yet the attempts just stop and no EOS can reach the pipeline's sinks (nothing is linked), so the application waits for the EOS message with a timeout, or treats a source with no `vsrc_0` as already stopped |
| each attempt | a WARNING for the first two attempts only | a WARNING (STREAM, FAILED) on every attempt, and the element message `rds-reconnect`: `source-id` (int), `attempt` (int, from 1), `reason` (string) (both ours) |
| a failed attempt (an error, or no buffer within the interval) | the next attempt | the next attempt an interval after it started; nothing goes downstream |
| attempts run out (`rtsp-reconnect-attempts` N ≥ 0 failed) | the element message `attempt-exceeded`: `source-id` (uint, −1 as 4294967295), `attempt_exceeded` (boolean TRUE); nothing downstream, so a muxer waits for the source forever | a WARNING (ours), then the same message, then EOS on `vsrc_0` (ours: `rdsstreammux` ends the source and batches the others); before any connection, the message and then an ERROR (RESOURCE, OPEN_READ; ours). No attempt after it |
| downstream after a reconnection | a FLUSH_START/STOP pair (reset-time TRUE), a new STREAM_START and a sticky reset event per attempt, then a new SEGMENT; without a muxer the running time restarts | the same stream (ours): no EOS or STREAM_START, the running time and PTS go on; the new session's frames start at a key frame (delta units before it are dropped, and the leading pictures of an open GOP after it; it is DISCONT), preceded by one FLUSH_START / FLUSH_STOP pair (reset-time FALSE) and the SEGMENT again, so `rdsstreammux` sends one `rds-stream-reset` for the source; then the element message `rds-reconnected` (`source-id`, int) |
| the first connection fails | retries every `rtsp-reconnect-interval` s | retries every `init-rtsp-reconnect-interval` s if above 0 (ours), else every `rtsp-reconnect-interval` s; the attempts limit applies (running out is an ERROR); `vsrc_0` comes with the first frames, with no flush; with both 0, an ERROR |
| stopping during an attempt | about 1 ms | waits for the attempt under way; a hanging session stops within `rtspsrc`'s 100 ms `teardown-timeout` |
| a new session at another size | new CAPS on `vsrc_0`, no error, the stream goes on; without a muxer the running time restarts | a normal reconnection, with new CAPS on `vsrc_0` (`rdsvideodec` reconfigures or recreates its decoder); `rdsstreammux` takes them as any caps change and scales the frames into its fixed-size slots |
| a new session in another codec (H.264 to H.265, say) | an ERROR for every attempt: it never recovers | a normal reconnection (ours): a new parser in front of the same `rdsvideodec`, which switches its own parser to the new caps; `vsrc_0` stays, with new CAPS if the size changes |

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

### `rdsstreammux` (`nvstreammux`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreammux.html))

Names, types and enum integers as on NVIDIA's nvstreammux page (checked 2026-10-24). Only the default ("legacy")
mux is implemented: every frame is scaled into a fixed `width`×`height` slot of the batch. The new mux (zero-copy
batches of the decoder surfaces, `config-file-path`, adaptive batching, per-source frame limits) is not written yet.

NVIDIA's page lists example values, not defaults, for most properties. The Default column gives Redstream's
default; **ours** marks a default the page does not document, so it may differ from DeepStream's.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `batch-size` | uint | `1` (ours) | supported | frames per batch, 1 and up (NVIDIA's range starts at 0); changeable only in NULL or READY |
| `batched-push-timeout` | int, µs | `-1` (ours) | supported | `-1`: no timeout. See "Batching and timeout" below; changeable only in NULL or READY |
| `width`, `height` | uint | `0` | supported | the slot size, **required**: `0` fails negotiation with a message naming both properties (the new mux, which can pass sizes through, is not implemented). Range 0-16384 (NVIDIA: up to `G_MAXUINT`); changeable only in NULL or READY |
| `enable-padding` | boolean | `FALSE` (ours) | supported | keeps the aspect ratio; the frame is placed **top-left** (see below) |
| `gpu-id` | uint | `0` (ours) | supported | GPU for the scaling and the output batches; a GPU that does not exist is an error at the first batch; changeable only in NULL or READY |
| `nvbuf-memory-type` | enum | `0` (ours) | supported | `0` `nvbuf-mem-default` (HIP device memory), `1` `nvbuf-mem-cuda-pinned`, `2` `nvbuf-mem-cuda-device`, `3` `nvbuf-mem-cuda-unified`. `4` (Jetson surface array) is not a value of the enum: `g_object_set` rejects it with a warning and keeps the old value. Changeable only in NULL or READY |
| `buffer-pool-size` | uint | `4` (ours) | supported | batches in the output pool, 1-1024, allocated up front (`buffer-pool-size` × `batch-size` frames); the pool never grows, so a stalled downstream back-pressures the sources. Demux branches whose sinks keep their last buffer (appsink always, other sinks with `enable-last-sample`) pin one batch each, since a demuxed frame keeps its whole batch alive: set `buffer-pool-size` larger than the number of such branches, or the mux stalls for good. It is not raised automatically: that would cost memory in every pipeline without a demux. Changeable only in NULL or READY |
| `compute-hw` | enum | `0` (ours) | supported | `0` default (the GPU), `1` GPU; `2` (VIC) fails at start |
| `interpolation-method` | enum | `6` (ours) | supported | integers as `NvBufSurfTransform_Inter`, filters as in "Transform semantics" (`6` default and `5` `algo-4` run nearest, `2`-`4` bilinear) |
| `num-surfaces-per-frame` | uint | `1` (ours) | supported | only `1`; 2-4 fail at start |
| `live-source` | boolean | `FALSE` (ours) | supported | changes only the batch timestamp (see below), not the waiting |
| `attach-sys-ts` | boolean | `TRUE` (ours) | supported | each frame's `ntp_timestamp` is the system (wall-clock) time in ns; `FALSE` gives 0. RTCP sender-report time is not implemented |
| `sync-inputs` | boolean | `FALSE` (ours) | supported | see "Batching and timeout"; changeable only in NULL or READY |
| `max-latency` | uint, ns | `0` (ours) | supported | reported downstream in the latency query, and waited for (on top of the timeout) before a partial batch goes out in live mode; changeable only in NULL or READY |
| `sort-batch` | boolean | `FALSE` (ours) | supported | slots in sink pad order; otherwise in the order the frames were collected |
| `drop-pipeline-eos` | boolean | `FALSE` | supported | no EOS downstream when every source has ended; the per-source `stream-eos` events still go |
| `frame-num-reset-on-eos` | boolean | `FALSE` (ours) | supported | a source's `frame_num` restarts at 0 after its EOS |
| `frame-num-reset-on-stream-reset` | boolean | `FALSE` (ours) | supported | a source's `frame_num` restarts at 0 after a flush of that source |
| `frame-duration` | uint64 | `-1` (`G_MAXUINT64`) | accepted no-op | NTP correction is not implemented |
| `failsafe-flush-count` | uint | `10` (ours) | accepted no-op | range 1-1024 and not tied to `buffer-pool-size` (NVIDIA: 1 to `buffer-pool-size`, capped at one less) |
| `align-first-buffer`, `align-inputs`, `cache-buffer`, `extract-sei-sim-time`, `extract-sei-type5-data`, `drop-backward-sei`, `async-process` | boolean | `FALSE` (ours) | accepted no-op |  |
| `cache-buffer-timeout` | int, µs | `-1` (ours) | accepted no-op |  |
| `sync-inputs-ntp` | int, ns | `0` (ours) | accepted no-op |  |
| `latency`, `min-upstream-latency`, `start-time`, `start-time-selection`, `emit-signals` | | `GstAggregator`'s | Redstream only | inherited from `GstAggregator`, not on NVIDIA's page. `latency` belongs to `batched-push-timeout`: it is set from it (or to 0) at every start, so a value set directly has no effect |

Setting an accepted no-op property logs one INFO message per element (the first time any of them is set).

Pads: `sink_%u` request pads (sink pad N is source N) and `src`. Inputs: `video/x-raw(memory:RDS)` or
`video/x-raw` in system memory (uploaded through a per-pad staging surface), formats NV12, I420, RGBA, BGRA,
RGBx, BGRx, RGB, BGR and GRAY8, one frame per buffer (a batch as input is an element error). A `GstVideoCropMeta`
on an input is honoured: only that rect is scaled. Output: `video/x-raw(memory:RDS)`, `width`×`height`, the format
downstream picks (NV12 when it does not care), PAR 1/1, the inputs' framerate when they all agree, else `0/1`.

#### Frame meta

One `RdsBatchMeta` per batch with `max_frames_in_batch` = `batch-size`, and per frame: `source_id` and `pad_index`
= the sink pad number; `batch_id` = the slot; `frame_num` = a per-source counter from 0; `buf_pts` = the input
buffer's PTS (in the source's own segment); `ntp_timestamp` (see `attach-sys-ts`); `source_frame_width`/`height` =
the input size (after its crop meta); `pipeline_width`/`height` = the slot size; one surface per frame.

#### Scaling and padding

Every frame goes through `rds_buf_surf_transform` into its slot. With `enable-padding`, the frame is scaled to fit
with its aspect ratio kept (even width and height for NV12 and I420) and placed at the **top-left** of the slot; the
rest of the slot is black (Y 16, U/V 128 for YUV; every byte 0, alpha included, for RGB and GRAY8). NVIDIA's page
says only that black bands keep the aspect ratio, not where they go. Top-left keeps the mapping from slot to source
coordinates a pure scale (divide by `pipeline_width / source_frame_width`, no offset).

#### Batching and timeout

- One batch takes at most one frame per sink pad. The pads are visited round robin, the first one rotating every
  batch, and a batch is closed at `batch-size` frames.
- `batched-push-timeout` `-1` (no timeout): a batch goes out once every source that has not ended has a frame
  queued, or `batch-size` frames are collected. A stalled source therefore stalls the batch; set a timeout for live
  or unreliable sources.
- `batched-push-timeout` ≥ 0: a partial batch goes out one timeout after its **first frame arrived**, whether the
  frames are ahead of the clock (a file decoding faster than real time) or behind it, and also when that frame
  queued up while the previous batch was still waiting (it does not wait a full timeout after that batch). A live source's upstream
  latency is not added. During the first upstream-latency of running time, though, the timer cannot start before
  running time 0, so the first partial batches go out late by up to that latency.
- `sync-inputs=TRUE`: a partial batch goes out once the clock reaches its earliest frame's running time plus the
  latency (`batched-push-timeout` or 0, plus `max-latency` and the upstream latency). A frame already due goes out
  at once with whatever else is queued. As in every mode, a batch with a frame from every source goes out as soon
  as it is complete: frames are never held back to the clock. NVIDIA documents only "synchronizes input frames using
  PTS".
- A live (or timed) mux's sink pads queue at least two frames plus a latency's worth before blocking upstream,
  on top of the one being batched: upstream buffer pools must allow for that.

#### Batch timestamps

- `live-source=TRUE`: the batch PTS is the running time of the last frame copied into it (as documented).
- `live-source=FALSE`: the running time of the earliest frame in the batch. NVIDIA instead derives it from the
  framerate of the first source that negotiated caps.
- Either way, batch timestamps never go backwards: a batch earlier than the previous one (a source joining late, or
  with another segment) gets the previous batch's PTS. DTS and duration are unset. The output segment is TIME from
  0, so the PTS is the running time.

#### Stream events

The mux sends the stream events of the "Stream events" section downstream, in order with the batches, after its
own stream-start, caps and segment:

- `pad-added (N)` when source N's **first event or frame** comes through (NVIDIA: when the mux receives data from a
  new source), not when the pad is requested;
- `stream-start (N)` and `stream-segment (N)` from that pad's STREAM_START and SEGMENT;
- `stream-eos (N)` from that pad's EOS, after the batch with its last frame and before any later batch;
- `stream-reset (N)` before the source's first event or frame after a flush of that pad;
- `pad-deleted (N)` when the pad is released (only if its `pad-added` went out), after a batch being built at that
  moment (it may hold the pad's last frame).

The pipeline EOS goes downstream when every source has ended, unless `drop-pipeline-eos` is set.

Releasing a sink pad sends `pad-deleted` from the releasing thread. If a prerolled sink downstream is blocking (the
pipeline is PAUSED and the mux idle), that push, and so the release, blocks until the pipeline goes to PLAYING:
release pads while PLAYING, or from a thread that may wait.

To remove a source cleanly, send EOS into its sink pad and wait for its `stream-eos (N)` downstream before releasing
the pad: the EOS reaches the mux's batching thread only after the frames queued in front of it, and a pad released
before that loses the frames still queued and its `stream-eos` (only `pad-deleted` goes out).

### `rdsstreamdemux` (`nvstreamdemux`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreamdemux.html))

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `per-stream-eos` | boolean | `FALSE` | supported | `TRUE`: `stream-eos (N)` ends `src_N` (EOS on it) as soon as source N ends. `FALSE`: every pad gets EOS on the pipeline EOS |

Pads: `sink` (`video/x-raw(memory:RDS)` batches with an `RdsBatchMeta`, as `rdsstreammux` makes them) and `src_%u`
request pads: frames of source N go to `src_N`. Pads may be requested and released while PLAYING, from any thread,
also from a `pad-added` handler.

#### Buffers (views)

- Frames are not copied: each output buffer is a **read-only view** of the frame's slot in the batch, and the batch
  goes back to the mux's pool once every view of it is released. The view's PTS is the frame's `buf_pts` (the
  source's original timestamp), DTS is unset, and flags and duration come from the batch. It carries a one-frame
  `RdsBatchMeta` (a copy of the frame meta, `batch_id` 0).
- To write pixels (an OSD, for example), make the buffer writable (`gst_buffer_make_writable`) and map it for
  writing: the writer then gets its own copy of the frame, and the batch the other sources share stays untouched. A
  write map of the view itself fails.
- Frames stay at the mux's `width`×`height`; nothing is scaled back to the source size (as documented).
- Frames of a source with no requested pad, or whose pad has ended, are dropped.

#### Events

- Each `src_N` gets its own stream-start (the upstream stream id plus `/N`), the sink caps, and **source N's own
  segment** (from `stream-segment (N)`, else the mux's output segment): `buf_pts` is in the source's segment, so
  synchronized sinks compute the source's running times.
- Stream events for source N (`pad-added`, `pad-deleted`, `stream-start`, `stream-segment`, `stream-eos`,
  `stream-reset`) go to `src_N` only, and are dropped when it does not exist, so a per-source branch sees only its
  own source's lifecycle. `pad-deleted (N)` always ends `src_N`; a later `stream-start`, `pad-added` or
  `stream-reset` for N starts a new stream on it.
- Flushes, the pipeline EOS (to pads not ended yet) and other events go to every pad.
- Upstream from the src pads: QoS is not forwarded (one slow branch would throttle every source); a seek sent by
  several branches (same seqnum) goes upstream once; other events go upstream.

#### Flow and branches

The demux returns the combination of its pads' results: NOT_LINKED only when every pad is unlinked. As with `tee`,
FLUSHING or an error from one branch stops the streaming thread for all of them, so **block or unlink a branch
before stopping it** (a sink set to NULL while still linked returns FLUSHING, which stops every branch). Give every branch a `queue`:
otherwise the first sink to preroll blocks the others, which share the demux's streaming thread.

### `rdsmultistreamtiler` (`nvmultistreamtiler`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvmultistreamtiler))

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0 (the 8.0 page omits
`buffer-pool-size` and `interpolation-method`). Enum nicks are Redstream's.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `rows` | uint 1.. | `1` | supported | grown when the sources do not fit (see behaviour) |
| `columns` | uint 1.. | `1` | supported | as `rows` |
| `width` | uint 16.. | `1920` | supported | rounded up to a multiple of 4; a change renegotiates the output caps |
| `height` | uint 16.. | `1080` | supported | as `width` |
| `show-source` | int -1.. | `-1` | supported | a pad index: that source alone, scaled to the whole output; read on every batch |
| `square-seq-grid` | boolean | `FALSE` | supported | a ⌈√n⌉ × ⌈√n⌉ grid; `rows` and `columns` are ignored. Cells are still by pad index |
| `interpolation-method` | enum 0..6 | `6` (`default`) | supported | filters as in "Transform semantics" (`default` is nearest) |
| `buffer-pool-size` | uint 4..10 | `5` | supported | buffers in the output pool; changeable only in NULL or READY |
| `nvbuf-memory-type` | enum 0..3 | `2` (`nvbuf-mem-cuda-device`) | supported | memory of the output buffers; changeable only in NULL or READY |
| `gpu-id` | uint | `0` | supported | changeable only in NULL or READY |
| `compute-hw` | enum 0..2 | `0` | supported | `2` (VIC) fails at start; changeable only in NULL or READY |
| `custom-tile-config` | pointer, write-only | NULL | accepted no-op | reserved in DeepStream too; a non-NULL value logs one warning |
| `qos` | boolean | `FALSE` | supported | GstBaseTransform's |

Pads: `sink` and `src`, `video/x-raw(memory:RDS)` NV12 or RGBA. The output has the input's format and framerate (no
conversion) and `width` × `height`, rounded. The input batch must be in device-accessible memory (device, pinned or
unified); a system-memory batch is an element error (ours).

#### Tiling behaviour

Each row is either **matches nvmultistreamtiler (DeepStream 8.0)**: observed DeepStream 8.0 behaviour where the docs
are silent, or **ours**: where 8.0 settles nothing or is clearly broken.

| Behaviour | Status | Notes |
|---|---|---|
| Cell of a frame | matches nvmultistreamtiler (DeepStream 8.0) | the frame meta's `pad_index`, row-major from the top left; `source_id` plays no part. The pixels come from the batch slot at the frame's `batch_id` |
| Grid | matches nvmultistreamtiler (DeepStream 8.0) | n = highest pad index seen + 1, from `rds-pad-added` events. `rows` × `columns` while n fits; otherwise ⌊√n⌋ rows and ⌈n / rows⌉ columns (2: 1×2, 3: 1×3, 5: 2×3, 7: 2×4). Recomputed when a pad is added |
| Grid grown from frame metas | ours | a frame meta's pad index beyond n grows the grid too (8.0 fails with a fatal composite error on a pad index only a frame meta has) |
| Grid after a source leaves | ours | never shrunk (8.0 not observed) |
| Cell geometry | matches nvmultistreamtiler (DeepStream 8.0) | output size rounded up to a multiple of 4 (1001×701 gives 1004×704); cells ⌊W / columns⌋ × ⌊H / rows⌋, each rounded down to even, at (column·cell width, row·cell height) |
| Scaling | matches nvmultistreamtiler (DeepStream 8.0) | each frame stretched over its whole cell (no letterbox), with its own size |
| Unused cells and the strip right and below the cells | matches nvmultistreamtiler (DeepStream 8.0) | black on every output frame: NV12 Y 16, U/V 128; RGBA all bytes 0 (ours for the alpha byte, as `rdsvideoconvert`'s fill) |
| Output meta | matches nvmultistreamtiler (DeepStream 8.0) | a batch meta with one fresh frame meta (`num_frames_in_batch` and `max_frames_in_batch` 1) holding a copy of every object and display meta of the batch; its `pad_index`, `source_id`, `batch_id`, `frame_num`, `buf_pts`, `ntp_timestamp`, `source_frame_*`, `pipeline_*` and `surface_index` are 0. Frame user meta and batch user meta are not carried. Per-source counts belong before the tiler |
| Order of the merged objects | ours | by pad index, each pad's objects (then display metas) in its list order |
| Meta coordinates | matches nvmultistreamtiler (DeepStream 8.0) | `rect_params` of objects and display rects: x' = cell x + x·sx, y' = cell y + y·sy, w·sx, h·sy as floats (sx = cell width / frame width); text offsets, line and arrow end points and circle centres the same, truncated to integers; circle radius × min (sx, sy), truncated. Unchanged: `font_size`, all widths, `detector_bbox_info`, `tracker_bbox_info`, `mask_params`, ids, class, confidence, labels. Not clipped to the cell |
| A source missing from a batch | matches nvmultistreamtiler (DeepStream 8.0) | its cell keeps its last pixels and its last objects and display metas are repeated, until its `rds-stream-eos` (or `rds-pad-deleted`); then the cell is black and its meta gone. Its pixels survive a grid change (moved into the new cell) |
| Two frames of one pad in a batch | ours | both drawn, the later on top; the later one's meta is kept |
| `show-source` | matches nvmultistreamtiler (DeepStream 8.0) | by pad index: that frame stretched to the output with only its meta, scaled to the output; a pad that never had a frame or has ended: black and an empty frame meta; a value of at least rows × columns: element error on the next batch. Changes apply from the next batch |
| `show-source` on a live pad missing from the batch | ours | black and an empty frame meta, as for an ended pad (8.0 not observed); the grid would repeat such a pad's last frame instead. The grid keeps its cached cells meanwhile, so switching back to -1 shows them |
| Live changes with empty cells | ours | `rows`, `columns`, `square-seq-grid`, `width` or `height` changed while playing to values whose cells would be empty: ignored with one element warning, the current grid kept |
| Input on another GPU | ours | device memory on a GPU other than `gpu-id` is an element error naming both |
| A pad the grid cannot hold | ours | a pad index whose grid would have empty cells (narrower or lower than 2 pixels) is skipped, with one element warning, whether it comes from a frame meta or an `rds-pad-added` event (also one before the caps) (8.0: fatal composite error) |
| Empty batch | ours | no frames: a black frame with an empty frame meta, as the docs say the muxer sends one to clear the tiler's output (8.0 crashes on it, per the docs' troubleshooting page). The cached cells and meta stay: the next batch repeats absent sources again |
| Events | ours | every event goes downstream unchanged; FLUSH_STOP forgets every cached cell and meta |
| Ready on push | ours | the composite, copies and clears are synchronous on the element's own HIP stream; the output is complete when pushed |

### `rdsosd` (`nvdsosd`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsosd))

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0. Enum nicks as in DeepStream
(`process-mode`'s `MODE_VIC` is ours).

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `display-bbox` | boolean | `TRUE` | supported | object and display-meta rectangles (borders and fills) |
| `display-text` | boolean | `TRUE` | supported | object text, display-meta labels and the clock |
| `display-clock` | boolean | `FALSE` | supported | |
| `clock-font` | string | NULL | supported | a fontconfig / Pango font name; NULL: `Serif` |
| `clock-font-size` | uint 0..60 | `0` | supported | points; 0: 12. Changeable only in NULL or READY |
| `x-clock-offset`, `y-clock-offset` | uint | `0` | supported | changeable only in NULL or READY |
| `clock-color` | uint | `0` | supported | 0xRRGGBBAA; never set: opaque red; set to 0: invisible. Changeable only in NULL or READY |
| `process-mode` | enum 0..2 (`MODE_CPU`, `MODE_GPU`, `MODE_VIC`) | `1` | supported | `0` draws on the CPU; `1` on the GPU (the default, as in DeepStream 8.0); `2` (ours: DeepStream 8.0's dGPU enum stops at 1, and VIC is a Jetson unit) on the GPU too, with one debug-log warning per element. The same pixels in every mode. Changeable only in NULL or READY |
| `gpu-id` | uint | `0` | supported | the GPU the GPU modes draw on: one that is not there is an element error at start, frames in device memory on another GPU an element error; the CPU mode ignores it. Changeable only in NULL or READY |
| `display-mask` | boolean | `FALSE` | accepted no-op | masks are not drawn yet; `TRUE` logs one warning |
| `blur-bbox` | boolean | `FALSE` | accepted no-op | objects are not blurred yet; `TRUE` logs one warning. Changeable only in NULL or READY |
| `blur-on-gie-class-ids` | string | NULL | accepted no-op | as `blur-bbox` |
| `qos` | boolean | `FALSE` | supported | GstBaseTransform's |

Pads: `sink` and `src`, `video/x-raw(memory:RDS)` RGBA only (ours: DeepStream 8.0's GPU mode also takes NV12), in
place. A buffer whose memory is shared or a read-only surface view (`rdsstreamdemux` output) is drawn on a private
copy, made on the device; the batch it came from is never written. In the GPU modes the frames stay on the GPU: no
CPU staging, no sync to the CPU and back; frames in system memory are an element error. Frames with nothing to draw
(under the current switches) are not touched, and a buffer with nothing to draw passes through unmapped, without a
copy. A GPU failure (no GPU at start, a failed draw) is an element error, never a fall back to the CPU; in the CPU
mode, so is a failed map or CPU/GPU sync.

#### Drawing behaviour

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
| Huge coordinates and widths | ours | circles and arrow heads with a coordinate, radius or width beyond 2^21 (2097152) pixels are not drawn (a wider `circle_width` is taken as 2^21); `line_width` and `arrow_width` above twice the frame's larger side are clamped to it (the stroke covers the frame); lines are clipped to the frame first; lines, arrows and circles are not drawn on frames wider or taller than 2^20 (1048576) pixels; a text whose coverage mask (its glyphs' ink box clipped to the frame, a byte a pixel) is larger than the 8 MiB mask heap is not drawn, its background box is (only huge fonts on large frames); nothing is drawn on frames wider or taller than 2^21 pixels or of more than 2^20 32 × 32 tiles (32768 × 32768; rdsosd takes 16384 × 16384 at most) |
| Clock | matches nvdsosd (DeepStream 8.0) | `YYYY-MM-DD HH:MM:SS`, the process's local time read once per buffer, drawn like text without a box at the clock offsets; default font a serif; one per frame |
| A buffer without frame meta | ours | passes through untouched, except that it gets the clock on every frame when `display-clock` is on |
| Masks | ours | not drawn yet (`display-mask` accepted, one warning); 8.0 draws them only with `display-mask=1` |
| Blur | ours | not done yet (`blur-bbox`, `blur-on-gie-class-ids` accepted, one warning); 8.0 blurs in GPU mode only |
| Batched input | ours | each frame meta drawn on its own surface, the slot at its `batch_id` (8.0 draws every frame's meta on surface 0); a `batch_id` outside the batch is skipped with one element warning |
| Tracker-created objects | matches nvdsosd (DeepStream 8.0) | drawn like any other object (confidence −0.1 changes nothing); nothing is added (no id text) |
| `has_color_info`, `color_id` | ours | ignored (Jetson colour table) |
| `process-mode` | ours | `0` draws on the CPU and `1` on the GPU, byte for byte the same (DeepStream 8.0's CPU mode draws differently); `2` (VIC on Jetson) draws on the GPU, with one debug-log warning per element |
| Ready on push | ours | the GPU modes wait for the element's stream after each pass, the CPU mode's syncs are synchronous: the frame is complete when pushed |

### `rdslogger` (`nvdslogger`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdslogger.md))

Reference: [rdslogger](elements/rdslogger.md).

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0 (the 8.0 guide has no page for
the element; the 9.2 page lists `show-fps`, `show-latency` and an underscored interval name that 8.0 does not have).
DeepStream 8.0's nvdslogger has identity's properties plus the interval.

| Property | Status | Difference |
|---|---|---|
| `fps-measurement-interval-sec` | supported | `0` prints nothing (8.0 busy-loops); a change while playing restarts the measurement |
| `silent` | supported | |
| `signal-handoffs` | supported | |
| `last-message` | accepted | always NULL |
| `qos` | supported | |
| `check-imperfect-offset` | accepted no-op | a non-default value logs one warning |
| `check-imperfect-timestamp` | accepted no-op | as above |
| `datarate` | accepted no-op | as above |
| `drop-allocation` | accepted no-op | as above |
| `drop-buffer-flags` | accepted no-op | as above |
| `drop-probability` | accepted no-op | as above |
| `dump` | accepted no-op | as above |
| `error-after` | accepted no-op | as above |
| `latency` | accepted no-op | as above |
| `single-segment` | accepted no-op | as above |
| `sleep-time` | accepted no-op | as above |
| `sync` | accepted no-op | as above |
| `ts-offset` | accepted no-op | as above |

### `rdsvideosink` (`nveglglessink`, `nv3dsink`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html))

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0's nveglglessink (nv3dsink has
the same base-sink properties and the window geometry, with `window-x`/`window-y` defaulting to 0). Neither element
has a page in the 8.0 guide. "Forwarded": set on the inner sink, and on the sinks inside it when it is a bin (as
`autovideosink` is), whenever it has a writable property of that name and the value fits its range; the values are
applied when the inner sink is created and on every change. A property the inner sink lacks is stored and does nothing,
silently (ours: with `autovideosink` the real sink is known only at READY).

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `video-sink` | string | `autovideosink` | ours | a factory name (string) of the GStreamer sink that shows the frames; an element instance is not accepted; NULL or empty: `autovideosink`; an unknown name, or an element that does not link or start, keeps the current sink with a warning. Changeable only in NULL or READY |
| `sync` | boolean | `TRUE` | forwarded | |
| `qos` | boolean | `TRUE` | forwarded | |
| `max-lateness` | int64 −1.. | `5000000` | forwarded | nanoseconds |
| `processing-deadline` | uint64 | `15000000` | forwarded | nanoseconds |
| `async` | boolean | `TRUE` | forwarded | |
| `window-x`, `window-y` | uint 0..2147483647 | `10` | forwarded | nveglglessink's defaults (nv3dsink's are 0); none of the stock sinks in the dev image has them |
| `window-width`, `window-height` | uint 0..2147483647 | `0` | forwarded | 0: the video's size; writable only on sinks that have them writable (not `ximagesink`, where they are read-only) |
| `force-aspect-ratio` | boolean | `TRUE` | forwarded | |
| `create-window` | boolean | `TRUE` | forwarded | |
| `gpu-id` | uint | `0` | forwarded | also `rdsvideoconvert`'s `gpu-id` (the GPU that copies the frames). Changeable only in NULL or READY |
| `blocksize`, `enable-last-sample`, `max-bitrate`, `render-delay`, `show-preroll-frame`, `throttle-time`, `ts-offset` | as GstBaseSink | as GstBaseSink | forwarded | the base-sink properties both DeepStream sinks show |
| `rows`, `columns` | uint 1..2147483647 | `1` | accepted no-op | a non-default value logs one warning per property (what 8.0 does with them was not determined) |
| `profile` | uint | `0` | accepted no-op | as above |
| `display` | pointer | NULL | accepted no-op | as above |
| `do-transform` | boolean | `TRUE` | accepted no-op | as above |
| `winsys` | string | `x11` | accepted no-op | Jetson's nveglglessink (`x11` or `wayland`); not in 8.0's dGPU gst-inspect, so its string type and `x11` default are ours; as above |

Pads: `sink` (a ghost pad of `rdsvideoconvert`'s): `memory:RDS` or system memory, `NV12`, `I420`, `RGBA`, `BGRA`,
`RGBx`, `BGRx`, `RGB`, `BGR`, `GRAY8`. Inside: `rdsvideoconvert ! video/x-raw,format=BGRx ! videoconvert !
<video-sink>`. The stock sinks in the dev image are `ximagesink`, `kmssink`, `autovideosink` and `fpsdisplaysink`;
from a container, X11 needs `DISPLAY`, the X socket and the Xauthority file.

#### Display behaviour

Each row is either **matches nveglglessink (DeepStream 8.0)**: observed DeepStream 8.0 behaviour where the docs are
silent, or **ours**: where 8.0 settles nothing or is clearly broken. Every row is ours so far: the display path itself
differs.

| Behaviour | Status | Notes |
|---|---|---|
| Rendering path | ours | frames copied to system memory (BGRx) and shown by a stock GStreamer sink; no GL interop yet |
| Batched input | ours | a buffer with more than one frame (`numFilled` above 1, or more than one frame meta) is an element error "rdsvideosink takes one frame per buffer: add a tiler or a demuxer" (buffer lists are checked buffer by buffer) (8.0 accepts the caps, then fails the first buffer without a message, and the pipeline hangs) |
| No display | ours | the inner sink's own error reaches the bus (`ximagesink`: "Could not initialise X output"); a sink that fails to start without posting an error gets one from `rdsvideosink` naming it and the `video-sink` property (another element of the bin failing silently: "An element inside rdsvideosink failed <transition>") (8.0's nveglglessink fails the state change without a bus message; nv3dsink posts "state change failed and some element failed to post a proper error message") |
| `autovideosink` without a display | ours | it may pick `kmssink`, which can start without showing anything; set `video-sink` for a definite sink |

### `rdsinfer` (`nvinfer`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvinfer.html))

Names and types as on NVIDIA's nvinfer page (checked 2026-10-24). As documented, a property set explicitly overrides
the config file; in Redstream that holds also when it is set to its default value. All properties can be changed
only in NULL or READY: the config is read and the engine built or loaded when the element starts (READY to PAUSED),
and every error there fails the state change with a message naming the file (or `element property`), the key and
the value. Defaults are those of the config keys (**ours**, NVIDIA documents none) and apply only when the property
is set.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `config-file-path` | string | none | supported | required; `.yml`/`.yaml` is read as YAML, anything else as a key file |
| `unique-id` | uint | `1` (ours) | supported | overrides `gie-unique-id`; range 0..`G_MAXUINT` as documented (the config key starts at 1) |
| `process-mode` | uint | `1` | supported | overrides `process-mode`; `2`: secondary mode, see "Secondary mode" (classifiers run only there) |
| `infer-on-gie-id` | int | `-1` | supported | -1..`G_MAXINT` (DeepStream 8.0's type and default: -1 is every GIE); overrides `operate-on-gie-id`; a warning when set in primary mode |
| `infer-on-class-ids` | string | none | supported | colon-separated class ids (`1:2:4`; `-1` in it: every class, DeepStream 8.0 behaviour); overrides `operate-on-class-ids`; a warning when set in primary mode |
| `filter-out-class-ids` | string | none | supported | semicolon-separated class ids (`1;2`); replaces the file's list (an empty string clears it) |
| `model-engine-file` | string | none | supported | a compiled MIGraphX program (`.mxr`), loaded when its manifest matches the model and the device, else rebuilt into the cache name; used as given (a config-file value is relative to the config file). A TensorRT `.engine` is ignored with a warning. Changing it while PLAYING (on-the-fly model update) is not supported |
| `batch-size` | uint | `1` (ours) | supported | 1..1024. Primary mode: frames of a batch go to the network image of their slot (`batch_id`); a frame in a slot at or above `batch-size` is an error. Secondary mode: objects inferred together, see "Secondary mode" |
| `dynamic-batch` | boolean | `FALSE` (ours) | supported | ours (nvinfer has no such property); overrides the config key: also compile programs for the powers of two below `batch-size` (4: 1, 2, 4; 6: 1, 2, 4, 6) and run each chunk on the smallest that holds its highest image. Primary mode: the highest occupied slot (`batch_id`) decides, not the number of frames, so one frame in slot 3 runs on the batch-4 program. Secondary mode: a chunk's objects take images 0 to n-1, so the object count decides. `batch-size` keeps its meaning, the most images one run takes. A model with a fixed batch dimension falls back to the `batch-size` program alone, with a warning at start. With `batch-size` 1 it has no effect: there is no smaller program. Costs and caching: see "Models and compiled programs"; changeable only in NULL or READY |
| `interval` | int | `0` (ours) | supported | 0..32: infer one batch, skip the next `interval` (they pass with `bInferDone` FALSE and no objects); the count restarts at start and after a flush (ours); ignored in secondary mode, with a warning (DeepStream 8.0 behaviour) |
| `gpu-id` | uint | `0` | supported | the GPU for preprocessing and inference; the batch must be in GPU memory on it |
| `output-tensor-meta` | boolean | `FALSE` | supported | one `RdsInferTensorMeta` per inferred frame, or per object in secondary mode (also always with `network-type=100`), see "Tensor output meta" |
| `raw-output-file-write` | boolean | `FALSE` | accepted no-op | not implemented; a warning (once per element) when set |
| `raw-output-generated-callback` | pointer | NULL | accepted no-op | the callback's signature isn't documented; a warning (once per element) when set |
| `raw-output-generated-userdata` | pointer | NULL | accepted no-op |  |
| `output-instance-mask` | boolean | `FALSE` | unsupported | `TRUE` fails at start (instance segmentation is not implemented) |
| `input-tensor-meta` | boolean | `FALSE` | unsupported | `TRUE` fails at start (preprocessed tensor input is not implemented) |
| `crop-objects-to-roi-boundary` | boolean | `FALSE` | accepted no-op | applies to preprocessed-tensor ROIs; a warning when `TRUE` |
| `clip-object-outside-roi` | boolean | `FALSE` | accepted no-op | applies to preprocessed-tensor ROIs; a warning when `TRUE` |

Not implemented: `classifier-type`, and the TensorRT-only `strongly-typed` and `dynamic-properties`.

Config file paths: `onnx-file`, `model-engine-file`, `labelfile-path` and `custom-lib-path` from the file are relative
to the config file's directory (documented for YAML, ours for key files). That includes a bare `custom-lib-path`
name (no `/`): it is the file next to the config, not a library searched on the loader path. Use an absolute path
(as NVIDIA's docs ask) for a library elsewhere.

Pads: `sink` and `src`, `video/x-raw(memory:RDS)` NV12, RGBA, BGRA, RGBx or BGRx batches with an `RdsBatchMeta`
(from `rdsstreammux`); buffers pass through unchanged (metadata only, pixels are never written). A buffer without
batch meta is an error.

#### Detector output (object meta)

- Boxes are in the batch frame's pixels (the mux's `width`×`height`); `maintain-aspect-ratio` / `symmetric-padding`
  scaling is undone (ours: the scaled frame sits at the top-left, or centred with `symmetric-padding`).
  `detector_bbox_info.org_bbox_coords` is the unclipped box and `rect_params` the box clipped to the frame
  (documented).
- Fields set: `unique_component_id` (`gie-unique-id`), `class_id`, `confidence`, `object_id` =
  `UNTRACKED_OBJECT_ID`, `obj_label` (the label, truncated to 127 bytes), `parent` NULL. Drawing defaults (ours):
  `rect_params` border 3 px red, no background; `text_params` the label at the box's top-left, 20 px above it
  (clamped to 0), "Serif" 10, white on black.
- Frames that were inferred get `bInferDone` TRUE (primary mode). The post-processing order and NMS are under
  "Detection post-processing". A secondary detector's objects are under "Secondary mode".
- A parse function that returns `false` or throws: that frame gets no objects, and the element posts one warning
  (then logs at debug level); the stream goes on. Invalid objects from a parser (class id out of range, non-finite
  values, empty boxes) are dropped, and a non-zero `rotation_angle` is ignored: one warning each.

#### Tensor output meta

One `RdsUserMeta` per inferred frame on `frame_user_meta_list` (in secondary mode per inferred object, on its
`obj_user_meta_list`), `meta_type` `RDS_INFER_TENSOR_OUTPUT_META` (`NVDSINFER_TENSOR_OUTPUT_META`), data
`RdsInferTensorMeta`. `out_buf_ptrs_host[i]` (pinned host memory) and `out_buf_ptrs_dev[i]` point at the frame's (or
object's) slice of output layer `i`, whose dims in `output_layers_info` are those of one image, without the batch;
the `buffer` pointers in `output_layers_info` are NULL (documented: not valid there). The buffers stay valid as long as the meta exists, in
copies too (`rdsstreamdemux` outputs, `rds_batch_meta_copy`), also after the element has stopped. They are
refcounted and reused once the last meta pointing at them is gone, so `tensor-meta-pool-size` has no effect. The
element keeps as many as the most it had in use lately (during the last 256 to 512 batches, or chunks of
`batch-size` objects in secondary mode), and at least 4 (`rdsstreammux`'s default `buffer-pool-size`); the rest are
freed (ours).

#### Processing

One batch at a time, synchronously (ours; nvinfer's threading isn't documented): preprocessing and inference on the
element's own non-blocking stream, one synchronization per batch (per chunk of `batch-size` objects in secondary
mode), parsing on the CPU. Slots without a frame (a partial batch) and unused images of a chunk are zeroed, not
inferred on stale pixels (with `dynamic-batch`, those of the program that runs). The batch's metadata lock is held
only to read the frame list and to add the results, never during GPU work; results for a frame or object removed in
between are dropped, also when the metadata pool gave its address to a new one.

#### Secondary mode

`process-mode=2` infers on the objects that upstream elements added (no primary `rdsinfer` is needed). Each row is
**DeepStream 8.0 behaviour** (observed where the docs are silent) or **ours** (where 8.0 settles nothing, crashes or
is inconsistent).

Keys and properties: `process-mode` is a property and a config key; `infer-on-gie-id` and `infer-on-class-ids` are
properties that override the config keys `operate-on-gie-id` and `operate-on-class-ids`;
`input-object-min/max-width/height`, `secondary-reinfer-interval` and the classifier keys (`classifier-threshold`,
`classifier-type`, `parse-classifier-func-name`, `classifier-async-mode`) are config keys only, as in DeepStream 8.0,
which has no properties for them. Their rows are in "Config keys: `[property]`".

| Topic | Source | Behaviour |
|---|---|---|
| Which frames | DeepStream 8.0 behaviour | every frame of every batch, with `bInferDone` TRUE or FALSE; `interval` is ignored (a warning when set). `bInferDone` is never set or cleared |
| Which objects | DeepStream 8.0 behaviour | `operate-on-gie-id` (-1: every GIE, this element's own id included; 0 is a real id), `operate-on-class-ids` (empty: every class) and `input-object-min/max-width/height` (inclusive, on the float `rect_params` size, 0: no limit); objects added by this element in the same buffer are not inferred on |
| Class id -1, negative class ids | ours, DeepStream 8.0 behaviour | `-1` in `operate-on-class-ids` in the config file is every class, as the property's `-1` is in 8.0 (8.0 aborts on the config value); with a class list set, an object with a negative `class_id` is not selected (8.0 aborts); with no list it is (DeepStream 8.0 behaviour) |
| Crop | DeepStream 8.0 behaviour | the object's `rect_params` (`detector_bbox_info` and `tracker_bbox_info` are not used): origin floored, then rounded up to even (11 → 12, 43.7 → 44), size floored, then down to even, at least 2 (15.6 → 14, 1 → 2); a side under 1 pixel is skipped; overflow past the right or bottom edge is clipped and the rest stretched to the network |
| Crop outside the frame | ours | a negative origin, a box larger than the frame or wholly outside it is clipped too (8.0 fails the pipeline); a crop with less than 2 pixels left is skipped (logged at debug level). The origin is aligned before the clip |
| Crop in the network input | DeepStream 8.0 behaviour | stretched to the network, or with `maintain-aspect-ratio` scaled by min(W/w, H/h) with the scaled size floored to even (24×16 into 16×16 is 16×10), at the top-left, or with `symmetric-padding` at floor((net − exact scaled size) / 2); padding is pixel value 0 before normalisation. The crop is resampled to 8 bits, then normalised as in primary mode |
| Scaling filter | ours | `scaling-filter` defaults to 1 (bilinear) in both modes (8.0's default is nearest). Nearest samples at pixel centres, as `rds_buf_surf_transform` does: a 32-pixel crop into 16 reads columns 1, 3, 5, … (8.0 reads 0, 2, 4, …), so nearest-filter inputs can differ from 8.0's by one source pixel. `scaling-filter=3` works (bilinear; 8.0 fails the pipeline) |
| Batching | DeepStream 8.0 behaviour | objects in frame list order, then object list order, in chunks of `batch-size` that continue across the frames of a batch but never across batches: 10 objects at `batch-size` 4 run as 4, 4, 2; two batches of 5 as 4, 1 and 4, 1 |
| Chunk size | ours | the compiled program has a fixed batch of `batch-size` images: a chunk with fewer objects runs them all the same, its unused images zeroed, so a chunk costs nearly as much with few objects as when full (8.0 rebuilds the model for 1 to `batch-size` images and runs the actual count). With `dynamic-batch`, a chunk of n objects runs on the smallest program of at least n images, so smaller chunks cost less: 5 objects at `batch-size` 4 run as 4, then 1 on the batch-1 program. The results per object are the same, up to float rounding between programs |
| Tensor meta | DeepStream 8.0 behaviour | one per inferred object (a detector's parents and classified objects included), on the object's `obj_user_meta_list`; none on the frame or the batch (see "Per-object cache"). `unique_id` is the `gie-unique-id`, the layer dims are those of one object (no batch), the host and device pointers point at the object's slice, `network_info` is the network's size and `maintain_aspect_ratio` / `symmetric_padding` the config's values |
| Detector objects (children) | DeepStream 8.0 behaviour | per crop, with the class attributes' thresholds, NMS per class (never across crops), `detected-min/max-*` in frame pixels and `filter-out-class-ids`; `parent` is the cropped object, `unique_component_id` this element's `gie-unique-id`, `class_id`, `confidence`, `obj_label` and the text from the parser and label file, `object_id` untracked |
| Detector thresholds | DeepStream 8.0 behaviour | the class attributes' defaults are those of the key table in both modes: `pre-cluster-threshold` 0.2, as in 8.0 |
| Child geometry | ours | network → crop → frame pixels with one consistent mapping; `rect_params` clipped to the frame, `detector_bbox_info` the unclipped box (8.0 maps with the unaligned parent origin, so it can be a pixel off, doesn't clip, and moves negative coordinates to the parent's origin without shrinking the box). A child wholly outside its crop is dropped; one partly outside keeps its part inside the frame |
| Child order | ours | appended to the frame's object list after the existing objects, crop by crop (8.0 prepends) |
| Detector chains | DeepStream 8.0 behaviour | a second `rdsinfer` with `infer-on-gie-id` = the detector's `gie-unique-id` infers on its children |
| `roi-top-offset`, `roi-bottom-offset` | DeepStream 8.0 behaviour, ours | applied to the children with the child's class attributes, in crop pixels (frame pixels from the top of the aligned crop the parent was sampled from): a child is kept when its top − the crop's top ≥ `roi-top-offset` (DeepStream 8.0 behaviour) and its bottom − the crop's top ≤ the crop's height − `roi-bottom-offset` (ours: 8.0 compares with the frame's height minus the offset); a `roi-bottom-offset` of the crop's height or more drops every child of that crop (ours). Boundaries included, each offset only when set. The parents are never filtered (DeepStream 8.0 behaviour); no warning |
| Parser returns false | ours | one warning per element, that object gets no children but its tensor meta is still attached (as in primary mode), the other objects are unaffected (8.0 crashes) |
| Classifiers (`network-type=1`) | ours | secondary mode only for now: a classifier in primary mode fails at start; see "Classifier output (classifier meta)" and "Per-object cache" |

#### Classifier output (classifier meta)

A classifier (`network-type=1`, `process-mode=2`) attaches what it finds to each object it infers on. Each row is
**DeepStream 8.0 behaviour** (observed where the docs are silent) or **ours**.

| Topic | Source | Behaviour |
|---|---|---|
| Built-in parser | DeepStream 8.0 behaviour | without `parse-classifier-func-name`: per output layer the argmax of its values, ties to the lowest class index; no softmax (the value is the probability as it is: 200/256 gives 0.78125, an already softmaxed model's values pass through) |
| `classifier-threshold` | DeepStream 8.0 behaviour | strictly above: 0.5 rejects 0.5 and keeps 0.50390625; the default 0 gives nothing for an all-zero output. A layer below it is left out; with none left, the object gets no classifier meta at all (not an empty one) |
| Classifier meta | DeepStream 8.0 behaviour | one `RdsClassifierMeta` per object and classifier: `num_labels` the labels attached, `unique_component_id` the `gie-unique-id`, `classifier_type` the `classifier-type` (NULL if unset; interned, not owned) |
| Meta order | ours | appended to the object's `classifier_meta_list`, after the metas of earlier classifiers (8.0 prepends) |
| Labels | DeepStream 8.0 behaviour | one `RdsLabelInfo` per output layer kept: `result_label` the label, `result_class_id` the argmax, `label_id` the output layer, `result_prob` the raw value |
| Label order | ours | in output-layer order (8.0 prepends) |
| `num_classes` | ours | the layer's class count (8.0 sets 0); 0 for a custom parser's attribute whose `attributeIndex` is not an output layer |
| Long labels | ours | a label longer than `result_label` (127 bytes) is truncated there and kept whole in `pResult_label`; otherwise `pResult_label` is NULL (8.0 never sets it) |
| Label file gaps | ours | an argmax past the layer's labels, a layer without a line and no label file all attach the meta with an empty label (8.0 attaches nothing for the first and the last, an empty label for the second); start warns when there is no label file, or one whose line or label counts don't fit the output layers |
| Display text | DeepStream 8.0 behaviour | `text_params.display_text` gets `" "` and then `"<label> "` per label attached (`"T"` becomes `"T red "`, NULL becomes `" blue "`, a second classifier makes it `"T red  x "`); with a custom parser `" "` and its description (`"T desc"`); unchanged when nothing is attached. `obj_label` is never changed |
| Empty description | ours | a custom parser that returns attributes but leaves `descString` empty (length 0) attaches the classifier meta with its labels and leaves the display text unchanged (`"T"` stays `"T"`, NULL stays NULL; 8.0 attaches nothing then, not even the labels, and caches that for a tracked object). A description of one space is not empty: `"T"` becomes `"T  "` (DeepStream 8.0 behaviour) |
| Description without attributes | ours | a custom parser that sets `descString` but returns no attributes attaches nothing: no classifier meta, and the display text is unchanged (8.0 not observed) |
| Custom parser | DeepStream 8.0 behaviour | `parse-classifier-func-name` in `custom-lib-path`, called once per object with that object's output layers (dims without the batch), the network info and `classifier-threshold`; each attribute becomes a label (`attributeLabel` → `result_label`, `attributeValue` → `result_class_id`, `attributeIndex` → `label_id`, `attributeConfidence` → `result_prob`), not thresholded again. rdsinfer frees the `attributeLabel`s |
| Custom parser label | ours | a NULL `attributeLabel` is an empty label |
| Unknown parse function | DeepStream 8.0 behaviour, ours | a `parse-classifier-func-name` the library doesn't have fails start (DeepStream 8.0 behaviour); the error names the key (ours) |
| Parser returns false or throws | ours | one warning per element; that object gets no new classifier meta (a tracked object keeps its stored result, if any, see "Per-object cache"), the others are unaffected (8.0 crashes) |
| Tensor meta | DeepStream 8.0 behaviour | with `output-tensor-meta=1` a classified object gets its tensor meta too; it doesn't change the classification |
| Chained classifiers | DeepStream 8.0 behaviour | a second classifier on the same objects attaches its own meta to each |

#### Per-object cache

In secondary mode a classifier keeps each tracked object's last result, keyed by `source_id` and `object_id`, and
infers the object again only when it is due. Each row is **DeepStream 8.0 behaviour** (observed) or **ours**.

| Topic | Source | Behaviour |
|---|---|---|
| What is cached | DeepStream 8.0 behaviour | classifiers (`network-type=1`) on tracked objects only. Untracked objects (`RDS_UNTRACKED_OBJECT_ID`) are inferred every frame; tensor-only networks (`network-type=100`) infer every object every time |
| Keys | DeepStream 8.0 behaviour | `(source_id, object_id)`: the same id in two streams has two entries |
| New id | DeepStream 8.0 behaviour | inferred on its first frame; an id seen before gets its stored result back (until it is evicted) |
| Growth | DeepStream 8.0 behaviour | inferred again when the `rect_params` area is above 1.2 × the area at its last inference, strictly (1.2 × no, 1.2001 × yes); shrinking or moving never re-infers and keeps the reference area. A class change keeps the result |
| Growth comparison | ours | exact (5 × area > 6 × last area in double precision); 8.0 compares in float, which may round the other way in rare cases |
| `secondary-reinfer-interval` | DeepStream 8.0 behaviour | unset (-1): never on time; 0: every frame; N: every N + 1 frames, counted from the object's own last inference (growth restarts the count) |
| Interval count | ours | counted in the frames in which the object was selected (8.0 runs had one source and the object in every frame) |
| Cached frames | DeepStream 8.0 behaviour | the stored result is attached again: identical classifier meta and label infos, the same `display_text` append; no tensor meta, even with `output-tensor-meta=1` |
| Empty result | DeepStream 8.0 behaviour | a result below `classifier-threshold` (no labels) is cached like any other: the object gets nothing until growth or the interval infers it again |
| Parser failure | ours | the object's entry is left as it was before the inference, and on that frame the object gets the stored result (classifier meta and display text) instead of nothing, plus its tensor meta from the failed inference; a new id has no stored result, gets no classifier meta and is tried again on its next frame; an object with a stored result keeps it and stays due until an inference succeeds |
| Same id twice in a batch | ours | inferred once; the other object gets the result stored before this batch, if any (none for a new id) |
| Eviction | ours | an entry not seen for 30 frames of its own source is dropped (8.0 keeps every entry: results came back after 1000 absent frames). Each source counts its own frames, one per frame of it in a batch, so a source that sends no frames keeps its entries; seen means present in the source's frame, selected or not (an object filtered out by class, size or an empty crop is kept) |
| Stream events | ours | a source's `rds-stream-eos` or `rds-stream-reset` drops that source's entries and restarts its frame count; FLUSH_STOP and stopping the element drop every entry. A result whose request is stale (`seq` and epoch per request) is neither stored nor attached: what an object gets is always what its entry holds |
| Secondary detectors | DeepStream 8.0 behaviour | no cache of detections; with `secondary-reinfer-interval` set, a tracked parent is inferred only when due by the same rules (new id, growth, interval) and gets no children (and no tensor meta) on the other frames. Unset: every frame |
| `classifier-async-mode=1` | ours | warns at start and runs synchronously: results are in the same buffer, also for untracked objects (8.0 attaches them from the next frame on and never to untracked objects) |

#### Config files

The config file has nvinfer's groups: `[property]` (required), `[class-attrs-all]` and `[class-attrs-<id>]`.
- A file ending in `.yml` or `.yaml` is YAML, anything else a GLib key file. In YAML the groups are top-level
  mappings (`property:`, `class-attrs-all:`, ...), values are scalars, and a sequence (`offsets: [77.5, 21.2, 11.8]`)
  is the same as the key file's `;`-separated list (ours: the docs show the layout only by example).
- `[class-attrs-<id>]` overrides `[class-attrs-all]` key by key; the other keys of that class keep the
  `[class-attrs-all]` values (ours). An id at or above `num-detected-classes` is an error.
- Keys are in the tables below with the parser's own types, ranges and defaults; the defaults are ours (NVIDIA
  documents none). **supported**: used; **accepted no-op**: one warning per key, then ignored (values outside the
  range or the accepted values are still errors); **unsupported**: an error whenever the key is set (to a non-empty value). A key that is
  in no table is a warning, not an error: the docs list legacy keys, so a typo can't be told from one of those.
- Errors name the file, the key (`GROUP:KEY` outside `[property]`) and the value: `FILE:KEY: VALUE: reason`.
- `onnx-file` is required, also with `model-engine-file` (a compiled program is checked against its model). For a
  detector (`network-type=0`), so are `num-detected-classes` and `parse-bbox-func-name`: nvinfer's internal resnet
  parser, which it uses when `parse-bbox-func-name` is unset, is not available, so name `RdsInferParseYolo` (built
  in) or a function in `custom-lib-path`.
- Label file (`labelfile-path`): one label per line, or all of them on one `;`-separated line; whitespace, CRs and
  trailing blank lines are ignored. Fewer labels than classes is a warning (the rest get empty labels), and so are
  more (the extra ones are ignored).
- A classifier's label file (`network-type=1`) has another form (DeepStream 8.0 behaviour): line i has the labels of
  output layer i, `;`-separated and not trimmed; one trailing `;` on a line is ignored (`a;b;` is `a`, `b`), an empty
  label elsewhere is kept (`a;;b`), and an empty line is a layer without labels. Only the CR of a CRLF line end and
  trailing blank lines are left out.
  Without `labelfile-path`, a warning: the classifier output gets empty labels (ours). A file with a line count other
  than the network's output layers, or a line whose label count isn't its layer's class count, is one warning at
  start, and its labels are used as they are (ours).
- Keys the mode or the network type doesn't use are a warning when set to something other than their default: the
  secondary-mode keys (`operate-on-*`, `input-object-*`, `secondary-reinfer-interval`, `classifier-async-mode`) in
  primary mode, the classifier keys outside classifiers, and `interval` in secondary mode (DeepStream 8.0 behaviour:
  it is ignored there). `roi-top-offset` and `roi-bottom-offset` apply in both modes (in secondary mode from the
  crop's edges, see "Secondary mode") and never warn.

#### Config keys: `[property]`

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `onnx-file` | path | none |  | supported | the model; see "Models and compiled programs" |
| `model-engine-file` | path | none |  | supported | a compiled MIGraphX program (`.mxr`), see "Models and compiled programs" |
| `labelfile-path` | path | none |  | supported | see "Config files" |
| `infer-dims` | dims | none | 1..2147483647 | supported | `C;H;W` of one input image (also for `network-input-order=1`); required when the model's height or width is symbolic |
| `output-blob-names` | string list | none |  | accepted no-op | all outputs are passed to the parser and to the tensor meta |
| `batch-size` | uint | `1` (ours) | 1..1024 | supported | the program is compiled for this batch size (with `dynamic-batch`, the largest program of the set); the model's batch dimension must be symbolic or equal to it |
| `dynamic-batch` | bool | `0` (ours) |  | supported | ours (nvinfer has no such key): as the property |
| `network-mode` | int | `0` (ours) | 0..3 | supported | accepted values: 0, 2 (FP32, FP16; inputs and outputs stay FP32). INT8 (1) and BEST (3) fail at start |
| `network-type` | int | `0` (ours) | 0..100 | supported | accepted values: 0, 1, 100 (detector; classifier, in secondary mode only; no post-processing, tensor meta only). A classifier in primary mode and segmentation (2, 3) fail at start |
| `process-mode` | int | `1` (ours) | 1..2 | supported | 1 primary, 2 secondary (see "Secondary mode"; classifiers run only there) |
| `gie-unique-id` | uint | `1` (ours) | >= 1 | supported | the objects' `unique_component_id` and the tensor meta's `unique_id` |
| `interval` | uint | `0` (ours) | 0..32 | supported | as the property |
| `output-tensor-meta` | bool | `0` (ours) |  | supported | as the property |
| `net-scale-factor` | float | `1.0` (ours) | > 0 | supported | input = `net-scale-factor` × (pixel − `offsets[c]`), pixels 0..255 (documented) |
| `offsets` | float list | none | >= 0 | supported | one per input channel (a count that doesn't match is an error); unset: 0 |
| `model-color-format` | int | `0` (ours) | 0..2 | supported | 0 RGB, 1 BGR, 2 GRAY (ours: the Y plane of NV12 frames, BT.601 luma of RGB frames) |
| `network-input-order` | int | `0` (ours) | 0..1 | supported | 0 NCHW, 1 NHWC |
| `maintain-aspect-ratio` | bool | `0` (ours) |  | supported | scale = min(W/w, H/h), the scaled size rounded to nearest (on crops in secondary mode: floored to even, see "Secondary mode"); padding is pixel value 0 before normalisation (ours) |
| `symmetric-padding` | bool | `0` (ours) |  | supported | with `maintain-aspect-ratio`: the scaled frame is centred (offset rounded down); without it, at the top-left |
| `scaling-filter` | int | `1` (ours) | 0..6 | supported | the `RdsBufSurfTransform_Inter` values: 1 (bilinear) and 2-4 sample bilinearly, the others nearest, as in `rds_buf_surf_transform` (nearest at pixel centres, see "Secondary mode") |
| `num-detected-classes` | uint | none | 1..65536 | supported | required for detectors |
| `cluster-mode` | int | `2` (ours) | 0..4 | supported | accepted values: 2, 4 (NMS, none). GroupRectangles (0), DBSCAN (1) and hybrid (3) fail at start |
| `parse-bbox-func-name` | string | none |  | supported | required for detectors: `RdsInferParseYolo` (built in) or a function in `custom-lib-path` |
| `custom-lib-path` | path | none |  | supported | a parser library, see "nvinfer custom parser ABI"; relative to the config file's directory, a bare name too |
| `filter-out-class-ids` | uint list | none | >= 0 | supported | objects of these classes are dropped |
| `operate-on-gie-id` | int | `-1` (ours) | >= -1 | supported | secondary mode: infer on the objects of this GIE; -1 or unset: every GIE, this element's own id included (DeepStream 8.0 behaviour); 0 is a real id |
| `operate-on-class-ids` | int list | none | >= -1 | supported | secondary mode: infer on objects of these classes; unset: every class; a `-1` in the list: every class (ours) |
| `input-object-min-width` | uint | none | >= 0 | supported | secondary mode: objects narrower are skipped; inclusive, 0: no limit (DeepStream 8.0 behaviour); above a set `input-object-max-width` is an error |
| `input-object-min-height` | uint | none | >= 0 | supported | secondary mode: as `input-object-min-width`, for the height |
| `input-object-max-width` | uint | none | >= 0 | supported | secondary mode: objects wider are skipped; inclusive, 0: no limit (DeepStream 8.0 behaviour) |
| `input-object-max-height` | uint | none | >= 0 | supported | secondary mode: as `input-object-max-width`, for the height |
| `secondary-reinfer-interval` | int | `-1` (ours) | >= -1 | supported | secondary mode: unset (-1) never reinfers an object, 0 every frame, N every N + 1 frames (DeepStream 8.0 behaviour) |
| `classifier-threshold` | float | `0` (ours) | >= 0 | supported | classifiers: a label needs a probability strictly above it; unset: 0 (DeepStream 8.0 behaviour) |
| `classifier-type` | string | none |  | supported | classifiers: the classifier meta's type name; letters, digits, `_` and `-` |
| `parse-classifier-func-name` | string | none |  | supported | classifiers: a parse function in `custom-lib-path` (required with it); unset: the built-in argmax parser (see "Classifier output (classifier meta)") |
| `classifier-async-mode` | bool | none |  | supported | secondary classifiers: `1` runs synchronously all the same, with a warning (ours) |
| `gpu-id` | uint | none | >= 0 | accepted no-op | the GPU is chosen with the element's `gpu-id` property |
| `workspace-size` | uint | none | >= 0 | accepted no-op | a TensorRT setting |
| `force-implicit-batch-dim` | bool | none |  | accepted no-op | a TensorRT setting |
| `enable-dla` | bool | none |  | accepted no-op | accepted values: 0; DLA is a Jetson unit |
| `use-dla-core` | int | none | >= 0 | accepted no-op | DLA is a Jetson unit |
| `tensor-meta-pool-size` | uint | none | >= 1 | accepted no-op | tensor output buffers are allocated as needed |
| `scaling-compute-hw` | int | none | 0..2 | accepted no-op | accepted values: 0, 1 (always the GPU); VIC (2) is a Jetson unit |
| `crop-objects-to-roi-boundary` | bool | none |  | accepted no-op | applies to preprocessed-tensor ROIs |
| `is-classifier` | bool | none |  | accepted no-op | accepted values: 0; legacy key, use `network-type=1` |
| `enable-dbscan` | bool | none |  | accepted no-op | accepted values: 0; deprecated, use `cluster-mode=2` |
| `int8-calib-file` | string | none |  | accepted no-op | INT8 only |
| `output-instance-mask` | bool | none |  | accepted no-op | accepted values: 0; instance segmentation |
| `input-tensor-from-meta` | bool | none |  | accepted no-op | accepted values: 0; preprocessed tensor input |
| `model-file` | string | none |  | unsupported | Caffe; use `onnx-file` |
| `proto-file` | string | none |  | unsupported | Caffe; use `onnx-file` |
| `mean-file` | string | none |  | unsupported | use `offsets` |
| `tlt-encoded-model` | string | none |  | unsupported | encoded TAO models; export to ONNX |
| `tlt-model-key` | string | none |  | unsupported | encoded TAO models; export to ONNX |
| `uff-input-dims` | string | none |  | unsupported | UFF; use `onnx-file` and `infer-dims` |
| `uff-input-blob-name` | string | none |  | unsupported | UFF; use `onnx-file` |
| `uff-input-order` | string | none |  | unsupported | UFF; use `network-input-order` |
| `engine-create-func-name` | string | none |  | unsupported | custom TensorRT engine builders |
| `custom-network-config` | string | none |  | unsupported | custom TensorRT engine builders |
| `layer-device-precision` | string | none |  | unsupported | a TensorRT setting; use `network-mode` |
| `output-io-formats` | string | none |  | unsupported | a TensorRT setting; inputs and outputs are FP32 |
| `parse-bbox-instance-mask-func-name` | string | none |  | unsupported | instance segmentation |
| `parse-segmentation-func-name` | string | none |  | unsupported | segmentation |
| `segmentation-threshold` | string | none |  | unsupported | segmentation |
| `segmentation-output-order` | string | none |  | unsupported | segmentation |

#### Config keys: `[class-attrs-all]`, `[class-attrs-<id>]`

| Key | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|
| `threshold` | float | none | >= 0 | supported | legacy name of `pre-cluster-threshold`, which wins if both are set |
| `pre-cluster-threshold` | float | `0.2` (ours) | >= 0 | supported | objects with a lower confidence are dropped before clustering; the default is DeepStream 8.0's |
| `post-cluster-threshold` | float | `0.0` (ours) | >= 0 | supported | objects with a lower confidence are dropped after clustering |
| `nms-iou-threshold` | float | `0.45` (ours) | >= 0 | supported | `cluster-mode=2`: a box is suppressed when its IoU with a kept box of its class is above it |
| `topk` | int | `-1` (ours) | >= -1 | supported | `cluster-mode=2`: the best K per class and frame after NMS; -1 keeps all, 0 none |
| `roi-top-offset` | uint | `0` (ours) | 0..2147483647 | supported | rows ignored at the top of the frame: objects whose top is above it are dropped, a top exactly at it is kept (DeepStream 8.0 behaviour); 0: no test. In secondary mode from the top of the crop (see "Secondary mode") |
| `roi-bottom-offset` | uint | `0` (ours) | 0..2147483647 | supported | the same from the bottom: objects whose bottom (top + height) is below the frame's height minus it are dropped, a bottom exactly there is kept (DeepStream 8.0 behaviour); 0: no test. The frame's height or more drops every object of the class (ours: 8.0 then skips the test). In secondary mode from the bottom of the crop (see "Secondary mode") |
| `detected-min-w` | uint | `0` (ours) | 0..2147483647 | supported | narrower objects are dropped |
| `detected-min-h` | uint | `0` (ours) | 0..2147483647 | supported | lower objects are dropped |
| `detected-max-w` | uint | `0` (ours) | 0..2147483647 | supported | wider objects are dropped; 0: no maximum |
| `detected-max-h` | uint | `0` (ours) | 0..2147483647 | supported | taller objects are dropped; 0: no maximum |
| `eps` | float | none | >= 0 | accepted no-op | DBSCAN only |
| `group-threshold` | int | none | >= 0 | accepted no-op | GroupRectangles only |
| `minBoxes` | int | none | >= 0 | accepted no-op | DBSCAN only |
| `dbscan-min-score` | float | none | >= 0 | accepted no-op | DBSCAN only |

#### Detection post-processing

The docs list the steps but not their order. rdsinfer runs them per frame, in this order (ours), each with the
object's class attributes:

1. parser output: objects with a class id at or above `num-detected-classes`, non-finite values or an empty box are
   dropped (one warning per element), and so are those below `pre-cluster-threshold`;
2. `cluster-mode=2`: greedy NMS per class, highest confidence first (ties: the parser's order); an object is
   suppressed when its IoU with a kept one is strictly above `nms-iou-threshold`;
3. `cluster-mode=2`: `topk`;
4. `post-cluster-threshold`;
5. mapping from network-input pixels to frame pixels (undoing `maintain-aspect-ratio` / `symmetric-padding`);
6. `filter-out-class-ids`;
7. `roi-top-offset` / `roi-bottom-offset`, each when set: top ≥ `roi-top-offset` and top + height ≤ frame height −
   `roi-bottom-offset`, boundaries included (DeepStream 8.0 behaviour); a `roi-bottom-offset` of the frame height or
   more drops everything (ours);
8. `detected-min-*` / `detected-max-*`, on the unclipped size in frame pixels;
9. clipping to the frame gives `rect_params`; an object whose clipped box is empty is dropped.

The built-in `RdsInferParseYolo` (ours) reads an Ultralytics YOLOv8/YOLO11 detection output: one FP32 layer of
`[4 + classes, anchors]` per image, or transposed `[anchors, 4 + classes]` (told apart by which side is
`4 + num-detected-classes`). Rows 0-3 are centre x, centre y, width and height in network-input pixels; each anchor
gives at most one object, its best class (no objectness score). Another output layout fails at start.

#### Models and compiled programs

- Models are ONNX (`onnx-file`) with one float NCHW input (NHWC with `network-input-order=1`) whose batch dimension
  is symbolic or equal to `batch-size`. A symbolic height or width needs `infer-dims`. ONNX models that keep weights
  in external data files are not supported (an error at start): export them with the weights inside the `.onnx`.
- The first start compiles the model with MIGraphX for the batch size, input size, GPU and precision, which takes
  minutes on a small GPU (see `rds-compile` below to do it ahead of time). The compiled program is saved as
  `<onnx stem>_b<batch>_<C>x<H>x<W>_<gfx>_<fp32|fp16>.mxr` next to the ONNX file, or in
  `$XDG_CACHE_HOME/redstream/` when that directory isn't writable. `<gfx>` is the GPU's own architecture (from the
  kernel driver's topology, so `HSA_OVERRIDE_GFX_VERSION` doesn't change it), e.g. `gfx1036`.
- Each program has a manifest, `<program>.mxr.json`: the ONNX file's SHA-256, the MIGraphX and HIP runtime versions,
  the GPU architecture and `HSA_OVERRIDE_GFX_VERSION`, the precision, the input name and shape, the output shapes and
  the compile options. A program is loaded only when its manifest matches what would be compiled now; otherwise it
  is rebuilt, and the element's log says why. Both files are written through temporary files and renames.
- Lookup: `model-engine-file`, then the cache name next to the model, then in `$XDG_CACHE_HOME/redstream/`. A stale
  `model-engine-file` is never overwritten: the rebuilt program goes to the cache name, and later starts load that
  copy with a warning that `model-engine-file` doesn't match. A TensorRT `.engine` file is ignored with a warning.
- With `dynamic-batch` (ours), every program of the set is compiled or loaded at start: the `batch-size` one first,
  then the smaller ones in descending order. Each is an ordinary cache entry with its own `_b<N>_` name and
  manifest. All or nothing: a program that fails fails the start, and the error names its size (`batch 2 program:
  ...`). `model-engine-file` applies to the `batch-size` program only; the smaller ones always use their cache names.
  Each extra program costs a compile on a cold cache (YOLO11n on the gfx1036 iGPU: about 1.5 min per size) and its
  own GPU memory (about 80–115 MB each; MIGraphX 2.18 can't share weights between programs). The programs are
  different kernels, so their results agree with the `batch-size` program's only up to float rounding (YOLO11n: at
  most 1.13e-6 in confidence and 1.22e-4 px in the boxes). MIGraphX's own dynamic shapes are not used: in 2.18 they
  compile every size anyway, copy the weights for each, and don't parse YOLO11n.
- The element runs the program once on start (a warm-up), so the first batch isn't slower than the rest.

#### Pre-building programs: `rds-compile` (`trtexec`)

The element compiles the model on its first start, which takes minutes on a small GPU. `rds-compile` does it ahead
of time with the element's own config parser and engine, so the program lands where the element looks for it:

```bash
rds-compile -c CONFIG [-b BATCH] [--fp16|--fp32] [-g GPU] [--dynamic-batch] [-o OUT.mxr] [--force]
```

- `-b`, `--fp16`/`--fp32`, `-g` and `--dynamic-batch` override `batch-size`, `network-mode`, `gpu-id` and
  `dynamic-batch` (sets it to 1); `--fp16` with `--fp32` is an error. Without `-o` the program is looked up and
  written like the element does (`model-engine-file`, then the cache name next to the ONNX file, then
  `$XDG_CACHE_HOME/redstream/`). `-o` looks up and writes only that file, with its manifest next to it as
  `OUT.mxr.json` (an unwritable directory is an error before building); point `model-engine-file` at it.
- With `dynamic-batch`, every program of the set is built (one per power of two below `batch-size`, plus
  `batch-size`); those already up to date are loaded, not rebuilt. The up-to-date check is the element's own open:
  it briefly loads every program onto the GPU (YOLO11n: about 80–115 MB each). `-o` places only the `batch-size`
  program; the smaller ones always use their cache names. A model with a fixed batch dimension builds the
  `batch-size` program alone, with the element's warning.
- Prints each program's path on stdout, one per line in ascending batch size (the `batch-size` one last); the build
  reasons, warnings and errors (with the element's messages) on stderr.
- Exit status: 0 built (at least one program), 1 error, 2 already up to date (the element would load every
  program; `--force` rebuilds them all).

### `rdstracker` (`nvtracker`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvtracker))

Names, types and defaults as nvtracker reports them in DeepStream 8.0 (7.1 is the same). The properties marked
"NULL/READY" can be changed only in NULL or READY; the config file is read, and every property checked, when the
element starts (READY to PAUSED), and an error there fails the state change with a message naming the file or the
property. An accepted no-op property set to other than its default posts one warning at start, except the three
marked "silent": `tracker-width`, `tracker-height` and `gpu-id` are ordinary settings that simply have no effect on
a CPU tracker working in frame coordinates.

| Property | Type | Range | Default | Status | Notes |
|---|---|---|---|---|---|
| `ll-lib-file` | string |  | NULL | supported | NULL/READY. Only the built-in tracker exists: unset, `libnvds_nvmultiobjecttracker.so` or `librdstracker-builtin.so` (bare or as a path; an INFO log says the built-in one is used). Any other library fails at start |
| `ll-config-file` | string |  | NULL | supported | NULL/READY. An NvMultiObjectTracker YAML config (see "Tracker config files"); unset: every key's default, which is an IOU tracker |
| `tracker-width` | uint | 0..`G_MAXUINT` | `960` | accepted no-op | silent (no effect). NULL/READY. Boxes are tracked in the frame's (the mux's) pixels, so the tracker size has no effect, as observed with DeepStream 8.0's IOU and NvSORT. 0 is accepted and ignored (ours: 8.0 gives NaN boxes) |
| `tracker-height` | uint | 0..`G_MAXUINT` | `544` | accepted no-op | silent (no effect). As `tracker-width` |
| `gpu-id` | uint | 0..`G_MAXUINT` | `0` | accepted no-op | silent (no effect). NULL/READY. The tracker runs on the CPU, on the metadata only |
| `display-tracking-id` | boolean |  | `TRUE` | supported | the ID is appended to each tracked object's `display_text` (see "Behaviour"); read per batch |
| `tracking-id-reset-mode` | uint | 0..3 | `1` | supported | NULL/READY. 0: stream events change nothing; 1: a stream reset ends the stream's targets; 2: per-stream IDs, and a stream EOS ends the stream's targets and restarts its IDs at 0; 3: 1 and 2 together (see "Behaviour") |
| `tracking-surface-type` | uint | 0..`G_MAXUINT` | `0` | accepted no-op | NULL/READY. Every surface of the batch is tracked; a warning when set |
| `compute-hw` | enum | 0 default, 1 gpu, 2 vic | `0` | accepted no-op | no frames are scaled; a warning when set |
| `input-tensor-meta` | boolean |  | `FALSE` | accepted no-op | tensor meta input is not supported; a warning when set |
| `tensor-meta-gie-id` | uint | 0..`G_MAXUINT` | `0` | accepted no-op | NULL/READY. A warning when set |
| `sub-batches` | string |  | `""` | accepted no-op | one tracker instance serves every stream; a warning when set |
| `sub-batch-err-recovery-trial-cnt` | int | -1..`G_MAXINT` | `0` | accepted no-op | a warning when set |
| `user-meta-pool-size` | uint | 1..`G_MAXUINT` | `32` | accepted no-op | NULL/READY. No tracker user meta is emitted yet; a warning when set |
| `operate-on-class-ids` | string |  | NULL | supported | NULL/READY. From the DeepStream 9.x docs (8.0 and 7.1 have no such property): track only these classes, `;`-separated (`0;2`); unset or empty: every class. Objects of other classes pass untouched with `UNTRACKED_OBJECT_ID`. A token that isn't an integer fails at start |

Pads: `sink` and `src`, `video/x-raw(memory:RDS)` batches with an `RdsBatchMeta` (from `rdsstreammux`), as for
`rdsinfer`. Buffers pass through unchanged and pixels are never read: only the object meta is changed, in place,
under the batch's metadata lock. A buffer without batch meta is an error.

Trackers: the config selects **IOU** (`stateEstimatorType: 0` and `associationMatcherType: 0`) or **NvSORT**
(anything else: a Kalman filter per target and the cascaded association). NvDeepSORT and NvDCF configs run as
NvSORT without their visual tracker and Re-ID, with a warning naming the ignored sections. The tracking itself is
on the CPU: about 0.01 ms per batch on a 720p street clip (about 22 detections a frame), 0.025 ms (IOU) and
0.047 ms (NvSORT) for 4 streams × 50 objects (release build, Ryzen 9 7900X).

Not implemented: the NvDCF and NvDeepSORT trackers, Re-ID, re-association, pose estimation, 3D projection; the
tracker misc meta (past-frame data, shadow and terminated track lists, visibility); custom low-level libraries
(the NvMOT API); sub-batches; changing the config at run time.

#### Tracker config files

- The `ll-config-file` is YAML with top-level sections (`BaseConfig:`, `TargetManagement:`, ...), each a mapping
  of scalar keys; a sequence is a list value. The OpenCV-style `%YAML:1.0` first line is accepted.
- Keys are in the tables below with the parser's types, ranges and defaults. Defaults are the documented ones
  unless marked "(ours)". **supported**: used; **accepted no-op**: read and ignored, with one warning when set to
  other than its default; **ignored**: used only by a module rdstracker doesn't have, type-checked and ignored
  without a warning; **deprecated**: an old name, ignored with a warning whenever set. Values of the wrong type or
  out of range are errors in every status.
- A `VisualTracker`, `ReID`, `ObjectModelProjection` or `PoseEstimator` section gives one warning naming the
  ignored sections and the tracker the config runs as.
- An unknown key or section is a warning, not an error. Errors name the file, the section and the key:
  `FILE:SECTION.KEY: VALUE: reason`.
- `checkClassMatch` is read under `DataAssociator` (where observed DeepStream 8.0 behaviour reads it) and under
  `BaseConfig` (where the current docs put it); the same key in both with different values is an error.
- In those ignored sections, a value nested deeper than the reader accepts (the documented map form of
  `cameraModelFilepath`, keyed by sensor name) is skipped with a warning; the list form is type-checked.
- `samples/tracker/config_tracker_IOU.yml` and `config_tracker_NvSORT.yml` are our own configs, tuned on a 720p
  street clip with YOLO11n detections. On that clip (1356 frames) the NvSORT config gives 283 tracks of 75.6 frames
  on average with 4 ID switches and 98.8 % of the confident detections tracked; the IOU config 426 tracks of 62.9
  frames, 12 switches, 99.4 %.

#### Tracker config keys: `BaseConfig`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `minDetectorConfidence` | float | `0.0` | any | IOU, NvSORT, NvDeepSORT, NvDCF | supported | lower detections are removed before association |
| `checkClassMatch` | bool | `1` |  | IOU, NvSORT, NvDeepSORT, NvDCF | supported | also read under DataAssociator; different values in the two sections are an error |
| `useBatchNumForFrameId` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | frame numbers always come from the frame meta |

#### Tracker config keys: `TargetManagement`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `preserveStreamUpdateOrder` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | IDs are always assigned in input order (streams in batch order) |
| `maxTargetsPerStream` | uint | `30` | 0..65535 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | Inactive targets count; detections over the cap are removed |
| `minIouDiff4NewTarget` | float | `0.5` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | see "Duplicate check" under "Tracking behaviour" |
| `enableBboxUnClipping` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | boxes are reported as detected (observed DeepStream 8.0 behaviour for IOU and NvSORT) |
| `probationAge` | uint | `5` | >= 0 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | see "Activation" under "Tracking behaviour" |
| `maxShadowTrackingAge` | uint | `38` | >= 0 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | see "Termination" under "Tracking behaviour" |
| `earlyTerminationAge` | uint | `2` | >= 0 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | Tentative targets only; see "Termination" |
| `outputTerminatedTracks` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | tracker misc data (past-frame, shadow, terminated tracks) is not output yet |
| `outputShadowTracks` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | tracker misc data (past-frame, shadow, terminated tracks) is not output yet |
| `outputVisibility` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | object visibility is not output yet |
| `terminatedTrackFilename` | string | none |  | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | tracker misc data (past-frame, shadow, terminated tracks) is not output yet |
| `minTrackerConfidence` | float | `0.6` | 0..1 | NvDCF | ignored | no effect for IOU and NvSORT, whose tracker confidence is constant |
| `searchRegionPaddingScale` | int | `1` | 1..3 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |

#### Tracker config keys: `TrajectoryManagement`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `useUniqueID` | bool | `0` |  | IOU, NvSORT, NvDeepSORT, NvDCF | supported | the upper 32 bits of every ID: one random value per element start |
| `enableReAssoc` | bool | `0` |  | NvDCF | accepted no-op | target re-association is not supported; its other keys are ignored |
| `minMatchingScore4Overall` | float | `0.4` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minTrackletMatchingScore` | float | `0.4` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minMatchingScore4ReidSimilarity` | float | `0.8` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `matchingScoreWeight4TrackletSimilarity` | float | `1.0` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `matchingScoreWeight4ReidSimilarity` | float | `0.0` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minTrajectoryLength4Projection` | int | `20` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `prepLength4TrajectoryProjection` | int | `10` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trajectoryProjectionLength` | int | `90` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `maxAngle4TrackletMatching` | int | `40` | 0..180 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minSpeedSimilarity4TrackletMatching` | float | `0.3` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minBboxSizeSimilarity4TrackletMatching` | float | `0.6` | 0..1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `maxTrackletMatchingTimeSearchRange` | int | `20` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trajectoryProjectionProcessNoiseScale` | float | `1.0` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trajectoryProjectionMeasurementNoiseScale` | float | `1.0` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `trackletSpacialSearchRegionScale` | float | `0.0` | >= 0 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `reidExtractionInterval` | int | `0` | >= -1 | NvDCF | ignored | a re-association setting; re-association is not supported (enableReAssoc warns) |
| `enableVanishingTrackletReconstruction` | int | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minInclusionRatio4DuplicateTrackletRemoval` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minIou4DuplicateTrackletRemoval` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minMatchRatio4ValidTrackletDetermination` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `minVisibility4VanishingTrackletReconstruction` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `visibilityThreshold4VanishingTrackletDetection` | float | none | any | NvDCF | ignored | undocumented; a re-association setting; re-association is not supported (enableReAssoc warns) |
| `checkClassMatchForReAssoc` | bool | none |  | NvDCF | deprecated | deprecated: use checkClassMatch |

#### Tracker config keys: `DataAssociator`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `dataAssociatorType` | int | `0` | 0..0 | IOU, NvSORT, NvDeepSORT, NvDCF | accepted no-op | 0 (the default data associator) is the only type |
| `associationMatcherType` | int | `0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | 0 greedy, 1 cascaded (NvSORT's three stages) |
| `checkClassMatch` | bool | `1` |  | IOU, NvSORT, NvDeepSORT, NvDCF | supported | where observed DeepStream 8.0 behaviour reads it; also read under BaseConfig |
| `minMatchingScore4Overall` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `minMatchingScore4SizeSimilarity` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `minMatchingScore4Iou` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `matchingScoreWeight4SizeSimilarity` | float | `0.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `matchingScoreWeight4Iou` | float | `1.0` | 0..1 | IOU, NvSORT, NvDeepSORT, NvDCF | supported |  |
| `tentativeDetectorConfidence` | float | `0.5` | 0..1 | NvSORT, NvDeepSORT, NvDCF | supported | NvSORT: lower detections never create targets |
| `minMatchingScore4TentativeIou` | float | `0.0` | 0..1 | NvSORT, NvDeepSORT, NvDCF | supported | NvSORT's second and third stages |
| `minMatchingScore4VisualSimilarity` | float | `0.0` | 0..1 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |
| `matchingScoreWeight4VisualSimilarity` | float | `0.0` | 0..1 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |
| `thresholdMahalanobis` | float | `-1.0` | any | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `minMatchingScore4ReidSimilarity` | float | `0.0` | 0..1 | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `matchingScoreWeight4ReidSimilarity` | float | `0.0` | 0..1 | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `matchingScoreWeight4ReIDSimilarity` | float | `0.0` | 0..1 | NvDeepSORT | ignored | used only by NvDeepSORT (Re-ID), which is not supported |
| `usePrediction4Assoc` | bool | see notes (ours) |  | NvSORT, NvDeepSORT, NvDCF | supported | undocumented; associate against the predicted box; default (ours) 1 for NvSORT, 0 for IOU |

#### Tracker config keys: `StateEstimator`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `stateEstimatorType` | int | `0` | 0..3 | IOU, NvSORT, NvDeepSORT, NvDCF | supported | 3 (SIMPLE_LOC) runs as 2 (REGULAR), with a warning |
| `processNoiseVar4Loc` | float | `2.0` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `processNoiseVar4Size` | float | `1.0` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `processNoiseVar4Vel` | float | `0.1` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `measurementNoiseVar4Detector` | float | `4.0` | >= 0 | NvSORT, NvDeepSORT, NvDCF | supported |  |
| `measurementNoiseVar4Tracker` | float | `16.0` | >= 0 | NvDCF | ignored | used only by NvDCF (visual tracker), which is not supported |
| `noiseWeightVar4Loc` | float | `-0.1` | any | NvSORT, NvDeepSORT, NvDCF | supported | with `noiseWeightVar4Vel`, both above 0: noise proportional to the box height |
| `noiseWeightVar4Vel` | float | `-0.1` | any | NvSORT, NvDeepSORT, NvDCF | supported | see `noiseWeightVar4Loc` |
| `useAspectRatio` | bool | `0` |  | NvSORT, NvDeepSORT, NvDCF | supported | the filter tracks w/h instead of w |

#### Tracker config keys: `VisualTracker`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `visualTrackerType` | int | `0` | 0..2 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `useColorNames` | bool | `1` |  | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `useHog` | bool | `0` |  | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `featureImgSizeLevel` | int | `2` | 1..5 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `featureFocusOffsetFactor_y` | float | `0.0` | -0.5..0.5 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `useHighPrecisionFeature` | bool | `0` |  | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `filterLr` | float | `0.075` | 0..1 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `filterChannelWeightsLr` | float | `0.1` | 0..1 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `gaussianSigma` | float | `0.75` | >= 0 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |
| `vpiBackend4DcfTracker` | int | `1` | 1..2 | NvDCF | ignored | the visual tracker (NvDCF) is not supported |

#### Tracker config keys: `ReID`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `reidType` | int | `0` | 0..3 | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `reidFeatureSize` | int | `128` | >= 1 | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `reidHistorySize` | int | `100` | >= 1 | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `addFeatureNormalization` | bool | `0` |  | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `keepAspc` | bool | `1` |  | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |
| `outputReidTensor` | bool | `0` |  | NvDeepSORT, NvDCF | ignored | Re-ID is not supported |

#### Tracker config keys: `ReID`, `PoseEstimator` (model inference)

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `batchSize` | int | `1` | >= 1 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `workspaceSize` | int | `20` | >= 0 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `inferDims` | int list | `[128, 64, 3]` | >= 1 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `inputOrder` | int | `1` | 0..1 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `colorFormat` | int | `0` | >= 0 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `networkMode` | int | `0` | 0..2 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `offsets` | float list | `[0.0, 0.0, 0.0]` | any | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `netScaleFactor` | float | `1.0` | >= 0 | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `tltEncodedModel` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `tltModelKey` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `onnxFile` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `modelEngineFile` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `calibrationTableFile` | string | none |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |
| `useVPICropScaler` | bool | `0` |  | NvDeepSORT, NvDCF | ignored | the Re-ID and pose models are not supported |

#### Tracker config keys: `ObjectModelProjection`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `cameraModelFilepath` | string list | none |  | any (add-on module) | ignored | 3D object model projection is not supported |
| `waistHeightRatio` | float | `0.5` | 0..1 | any (add-on module) | ignored | 3D object model projection is not supported |
| `outputFootLocation` | bool | `0` |  | any (add-on module) | ignored | 3D object model projection is not supported |
| `outputConvexHull` | bool | `1` |  | any (add-on module) | ignored | 3D object model projection is not supported |
| `maxConvexHullSize` | int | `15` | >= 0 | any (add-on module) | ignored | 3D object model projection is not supported |
| `minPoseConfidence` | float | `0.5` | 0..1 | any (add-on module) | ignored | 3D object model projection is not supported |

#### Tracker config keys: `PoseEstimator`

| Key | Type | Default | Range | Trackers | Status | Notes |
|---|---|---|---|---|---|---|
| `poseEstimatorType` | int | `0` | 0..1 | any (add-on module) | ignored | pose estimation is not supported |
| `poseInferenceInterval` | int | `-1` | >= -1 | any (add-on module) | ignored | pose estimation is not supported |
| `operateOnClassIds` | int list | `[]` | >= 0 | any (add-on module) | ignored | pose estimation is not supported |

#### Tracking behaviour

The public docs describe the target life cycle and the matching, but leave many details open or contradict
themselves between versions. Each row is either **matches nvtracker (DeepStream 8.0)**: observed DeepStream 8.0
behaviour with the IOU and NvSORT trackers, or **ours**: where the docs and 8.0 settle nothing, or where 8.0 is
clearly broken. Terms: a target is **Tentative** until it is activated, then **Active** while matched and
**Inactive** (tracked but not reported) while unmatched; its **age** counts frames since it was created and its
**shadow tracking age** the consecutive frames with inference without a match.

| Behaviour | Status | Notes |
|---|---|---|
| Detections without a reported target are removed from the frame | matches nvtracker (DeepStream 8.0) | the detections of Tentative targets, those below `minDetectorConfidence`, duplicates (`minIouDiff4NewTarget`) and those over `maxTargetsPerStream` (Inactive targets count). Only objects of classes left out by `operate-on-class-ids` leave the tracker with `UNTRACKED_OBJECT_ID` |
| Which detections create targets | matches nvtracker (DeepStream 8.0) | IOU: any detection at or above `minDetectorConfidence`. NvSORT: only detections at or above `tentativeDetectorConfidence`; lower ones can only extend an Active target |
| Matched objects: boxes and fields | matches nvtracker (DeepStream 8.0) | `object_id` set; `tracker_bbox_info` is the detector's box (IOU and NvSORT, unclipped); `rect_params` is that box clipped to the frame; `confidence`, `detector_bbox_info`, `obj_label` and `unique_component_id` stay the detector's. `class_id` is the target's (with `checkClassMatch: 0` it follows the detection). NvSORT still updates its Kalman filter with the detection |
| `enableBboxUnClipping` | matches nvtracker (DeepStream 8.0) | no effect: boxes are reported as detected |
| `tracker_confidence` | matches nvtracker (DeepStream 8.0) | 0.5 on every tracked object, IOU and NvSORT (the docs, and DeepStream 7.1, give 1.0) |
| Frames without inference (`bInferDone` FALSE) | matches nvtracker (DeepStream 8.0) | every Active target is reported as a new object meta: its ID and class, `confidence` -0.1, `detector_bbox_info` all zero, `tracker_bbox_info` its last matched box (IOU) or its Kalman prediction (NvSORT), `rect_params` that box clipped. Tentative and Inactive targets are not reported. Objects already on such a frame are removed, never tracked |
| Labels of created objects | ours | `obj_label`, `display_text` and `unique_component_id` come from that target's own last matched detection (8.0 copies them from one detection per class), with rdsinfer's drawing defaults |
| Ages on frames without inference | matches nvtracker (DeepStream 8.0) | the age (and so probation) grows; the shadow tracking age doesn't |
| When IDs are assigned | matches nvtracker (DeepStream 8.0) | at activation, from 0; a Tentative target that dies takes no ID. Targets activated on the same frame are numbered in input order: streams in batch order, objects in `obj_meta_list` order |
| Activation | matches nvtracker (DeepStream 8.0) | age is 1 on the creation frame and grows by 1 every frame; a Tentative target activates on the first frame where its age exceeds `probationAge` and it is matched. `probationAge: 0` activates at once |
| Termination | matches nvtracker (DeepStream 8.0) | Active and Inactive targets end when the shadow tracking age exceeds `maxShadowTrackingAge`; Tentative ones when it reaches `earlyTerminationAge` or exceeds `maxShadowTrackingAge`. One match makes an Inactive target Active again. An object that returns after its target ended gets a new target and ID |
| `display-tracking-id` | matches nvtracker (DeepStream 8.0) | `display_text` becomes `"<display_text> <id>"`, the full 64-bit ID in decimal (rdsinfer's text is the label, so `"person 3"`); an empty text gives `" <id>"`. The string is replaced by a fresh one |
| `display-tracking-id` with a NULL `display_text` | ours | `"<id>"` (8.0 writes `"(null) <id>"`) |
| `tracking-id-reset-mode` | matches nvtracker (DeepStream 8.0) | one 32-bit counter per element, shared by every stream. 1: a stream reset ends the stream's targets; later detections go through probation again and get new IDs. 2: the lower 32 bits count per stream, from the counter's value at the stream's first target, so every stream starts at 0; a stream EOS ends the stream's targets and its count restarts at 0. 3: both |
| The ID counter is never restarted | ours | resets, EOS and flushes never restart the shared counter, so live IDs never repeat (8.0 restarts it, giving duplicate live IDs and `0xFFFFFFFF` after an underflow; 7.1 continues it, as here) |
| Size similarity | matches nvtracker (DeepStream 8.0) | the smaller area over the larger; the match score is `matchingScoreWeight4Iou`·IOU + `matchingScoreWeight4SizeSimilarity`·size similarity, and a pair must pass `minMatchingScore4Iou`, `minMatchingScore4SizeSimilarity` and `minMatchingScore4Overall` |
| Matching when all three minimums are 0 | ours | a pair also needs an IOU above 0, so a target never jumps to a disjoint box of a similar size. With any minimum set, the configured filters alone decide |
| Order of equal match scores | ours | greedy matching takes the best score first, then the lower target ID, then the earlier detection |
| NvSORT association | ours | the documented three stages: confirmed detections with Active and Inactive targets (joint score); tentative detections with the remaining Active targets (`minMatchingScore4TentativeIou`); the remaining confirmed detections with Tentative targets. Against the Kalman prediction when `usePrediction4Assoc` is 1 (default for NvSORT, ours) |
| `useUniqueID` | matches nvtracker (DeepStream 8.0) | the upper 32 bits are one random value, shared by every stream; 0 without it. Drawn again each time the element starts (ours) |
| `tracker-width` / `tracker-height` | matches nvtracker (DeepStream 8.0) | no effect on the boxes: tracking is in the frame's pixels |
| `tracker-width` / `tracker-height` 0 | ours | accepted and ignored (8.0 gives NaN boxes) |
| `checkClassMatch` | matches nvtracker (DeepStream 8.0) | 1: a target matches only detections of its class. Read under `DataAssociator` (as 8.0 does) and `BaseConfig` (as the current docs say) |
| Duplicate check (`minIouDiff4NewTarget`) | matches nvtracker (DeepStream 8.0) | a detection is a duplicate when its best IOU with any target of its stream, of any class and state, reaches the threshold |
| Duplicate check: which targets and boxes | ours | it includes targets created earlier on the same frame, and uses each target's box on this frame (for NvSORT the matched detection or the predicted box) |
| `minIouDiff4NewTarget: 0` | ours | only an overlapping detection (IOU above 0) can be a duplicate, so 0 doesn't block every new target |
| Invalid detection boxes | ours | a detection with a non-finite coordinate or a width or height of 0 or less is removed before matching |
| Which box a detection has | ours | the detector's unclipped `detector_bbox_info`; an object without one (added by an application) is tracked on its `rect_params` |
| Kalman prediction under 1 px | ours | a prediction that isn't a finite box at least 1 px wide and high is replaced by the target's last matched box |
| A box entirely outside the frame | ours | kept, with a `rect_params` of width or height 0 |
| Two frames of one source in a batch | ours | an element error. `rdsstreammux` never makes such batches; DeepStream's new streammux can (`max-same-source-frames`), which is a known limitation |
| Created objects in the frame's list | matches nvtracker (DeepStream 8.0) | matched objects keep their order; objects created for targets are put first, highest ID first |
| Kalman filter | ours | the docs describe the state and noise options, not the matrices. Constant velocity, one step per frame (also without inference), updated only on a match. Noise is fixed (`processNoiseVar4*`, `measurementNoiseVar4Detector`) or DeepSORT-style proportional to the box height; aspect-ratio terms use small constants, as in DeepSORT. Initial standard deviations 10·`noiseWeightVar4Loc`·h for position and size and 13·`noiseWeightVar4Vel`·h for velocities, fitted to observed 8.0 output: a constant-velocity object's predictions stay within 1.4 px of 8.0's (8.0's filter differs in structure, so no exact match) |
| Height-proportional noise | ours | used only when both `noiseWeightVar4Loc` and `noiseWeightVar4Vel` are above 0; otherwise the fixed noise |
| `stateEstimatorType: 3` | ours | SIMPLE_LOC runs as 2 (REGULAR), with a warning |
| Streams and events | ours | a stream is the frame meta's `source_id`, the ID `rdsstreammux`'s stream events carry. `rds-pad-deleted` frees the stream's state; FLUSH_STOP clears every stream (the counter continues). Every event goes downstream unchanged |
| Tracker misc meta | unsupported | past-frame data, shadow and terminated track lists and visibility are not emitted (8.0 emits past-frame data even without the keys that ask for it) |

#### Tracker metadata types

The tracker meta types are defined, at their documented positions, so ported code compiles; `rdstracker` doesn't
attach any of them yet (see "Tracker misc meta" above).

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NVDS_TRACKER_PAST_FRAME_META` | `RDS_TRACKER_PAST_FRAME_META` | supported | 15; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_BATCH_REID_META` | `RDS_TRACKER_BATCH_REID_META` | supported | 16; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_OBJ_REID_META` | `RDS_TRACKER_OBJ_REID_META` | supported | 17; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_TERMINATED_LIST_META` | `RDS_TRACKER_TERMINATED_LIST_META` | supported | 18; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_TRACKER_SHADOW_LIST_META` | `RDS_TRACKER_SHADOW_LIST_META` | supported | 19; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_VISIBILITY` | `RDS_OBJ_VISIBILITY` | supported | 20; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_IMAGE_FOOT_LOCATION` | `RDS_OBJ_IMAGE_FOOT_LOCATION` | supported | 21; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_WORLD_FOOT_LOCATION` | `RDS_OBJ_WORLD_FOOT_LOCATION` | supported | 22; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_IMAGE_CONVEX_HULL` | `RDS_OBJ_IMAGE_CONVEX_HULL` | supported | 23; not emitted yet | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `TRACKER_STATE` | `RdsTrackerState` | supported | the type name only. The values are `RDS_TRACKER_STATE_EMPTY` (0), `_ACTIVE` (1), `_INACTIVE` (2), `_TENTATIVE` (3), `_PROJECTED` (4) and `_QUASIACTIVE` (5, as in DeepStream 8.0); DeepStream's bare `EMPTY`, `ACTIVE`, ... are not aliased, since such macros would rewrite any identifier of that name | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvds__tracker__meta_8h.html) |

The object meta fields the tracker writes (`object_id`, `tracker_bbox_info`, `tracker_confidence`, `rect_params`,
`text_params.display_text`) are those of `RdsObjectMeta` under "Metadata"; `RDS_UNTRACKED_OBJECT_ID` is
`G_MAXUINT64`.

### `rdsanalytics` (`nvdsanalytics`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsanalytics))

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0's nvdsanalytics.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `config-file` | string | NULL | supported | required, also with `enable=FALSE`: without one, or with a file that fails, the start fails with an ERROR naming the property, the file or the key (ours: DeepStream 8.0 fails the state change too, but posts no ERROR). Settable in any state (DeepStream 8.0 behaviour): set while started, the file is loaded and checked at once and applies from the next buffer, with every stream's state reset, the cumulative counts included (DeepStream 8.0 behaviour); a file that fails then posts an ERROR, so an application that stops on ERROR stops there; otherwise the old config, its state and the property's old value stay (ours) |
| `enable` | boolean | `TRUE` | supported | NULL/READY only (DeepStream 8.0 behaviour). `FALSE`: pass-through with no meta at all, as `[property] enable=0` (DeepStream 8.0 behaviour) |
| `unique-id` | uint 0..4294967295 | `17` | supported | written to the `unique_id` of both metas; read per batch |
| `qos` | boolean | `FALSE` | accepted | GstBaseTransform's; the element never drops buffers |

#### Analytics element behaviour

Each row is **DeepStream 8.0 behaviour** (observed with scripted object meta) or **ours** (where 8.0 settles nothing
or is clearly broken).

| Behaviour | Status | Notes |
|---|---|---|
| Streams | DeepStream 8.0 behaviour | keyed by the frame meta's `pad_index`, not `source_id`; a new `pad_index` gets fresh state. `rdsstreammux` writes the sink pad's number to both, and its stream events carry the same number, so an event's id is the `pad_index` |
| Frames | DeepStream 8.0 behaviour | every frame meta of a batch is processed in batch order, partial batches and two frames of one stream included; every frame gets a frame meta, also on a stream without groups (empty maps, `objCnt` filled) |
| Object info | DeepStream 8.0 behaviour | only on objects with something to report (an ROI, overcrowding or line label, a direction or STOPPED) |
| Object box | ours (8.0 undetermined: the observed runs set both boxes equal) | the analytics use the object's `rect_params`, not `detector_bbox_info` |
| Frame size | DeepStream 8.0 behaviour | the rules are scaled from `config-width` x `config-height` to the batch surface's size, the mux output. Ours: when that size changes, the rules are scaled again and every stream's state resets, the cumulative counts included, with a warning |
| `rds-stream-reset` | ours | clears the stream's histories, STOPPED state, "already counted" flags, objCnt window and untracked warning; its cumulative line-crossing counts stay. DeepStream 8.0 ignores the event |
| `rds-pad-deleted` | ours | frees the stream's state but keeps its cumulative line-crossing counts, so a source added later on that pad starts fresh and continues them. `rds-stream-eos` alone changes nothing (a stream that restarts on its pad comes with a stream reset). A batch already in flight with that pad's frames can arrive after the event and recreate the stream's state, taking back its kept cumulative counts: harmless (ours). DeepStream 8.0 ignores these events |

#### Analytics behaviour

What the analytics compute, per frame of a stream. Each row is **DeepStream 8.0 behaviour** (observed with scripted
object meta, the docs being silent) or **ours** (where 8.0 settles nothing or is clearly broken). Terms: a stream's
**history** of an object is its list of points; **n** is the object's observations so far minus 1; **d** is the
newest point minus the history's reference.

| Behaviour | Status | Notes |
|---|---|---|
| Rule scaling | DeepStream 8.0 behaviour | every vertex scaled from `config-width` x `config-height` to the mux output, x and y separately, and truncated to an integer |
| Scaling arithmetic | ours | `v * out / cfg` in 64-bit integers, multiplied first: equal to 8.0's float result while `v * out` is under 2^24; whether 8.0 divides first is not known |
| Object point | DeepStream 8.0 behaviour | one point per object, its box's bottom centre floored: (floor(left + width/2), floor(top + height)); the rest of the box is ignored. The sums are in float (ours: 8.0's precision is unknown; its observed boxes give the same points either way) |
| Inside test | DeepStream 8.0 behaviour | the crossing number on the integer point and vertices: an edge toggles when `(yi > Py) != (yj > Py)` and Px is left of the crossing, truncated. Even-odd for self-intersecting polygons, left and top edges in, right and bottom out, whatever the vertex order |
| ROI filtering | DeepStream 8.0 behaviour | an object of the group's classes gets the label of every ROI its point is in (`roiStatus`, ` ROI:<label>` in `objStatus`) and is counted in `objInROIcnt`; with `inverse-roi=1` the objects outside, and those are counted |
| `objInROIcnt` | DeepStream 8.0 behaviour | every ROI-filtering label, then every overcrowding label, with 0 from the first frame. A group with `enable=0` adds nothing, not even its zeros |
| `class-id` | DeepStream 8.0 behaviour | unset, empty, -1 or a list holding -1: every class. Objects of other classes get none of the group's labels, and no object info for it |
| Overcrowding | DeepStream 8.0 behaviour | `ocStatus` is TRUE when the ROI's count on this frame is at least `object-threshold` and above 0 (`time-threshold` has no effect). The object's `ocStatus` lists the overcrowding ROIs it is in, whatever the status; overcrowding never appears in `objStatus` |
| `objCnt` | DeepStream 8.0 behaviour | per class, every object whatever the groups and filters: the mode of the per-frame count over the frames whose PTS is within `obj-cnt-win-in-ms` of this one, both ends included. A frame without the class counts 0. Also on streams without groups |
| `objCnt` ties | ours | the count seen most recently. 8.0 has no stable order (it chose the newest in 35 of 48 tied frames) |
| `objCnt` window | ours | frames before a class was first seen count 0 for it; at most 9000 frames; a PTS that goes back or is missing restarts the window |
| History | DeepStream 8.0 behaviour | on a stream with a direction or line-crossing group, per (pad_index, object id): one point per frame the object is in (frames it misses don't count), x clamped at 0. The window is the last 50 points; the reference is the integer-truncated mean of its oldest 2 points, or of its oldest 4 once it is full |
| One id twice in a frame | ours | each object adds its own point to the shared history |
| History eviction | ours | a history unseen for more than 30 s of PTS is dropped, so an id seen again later starts afresh and can be counted again. Without usable PTS (none, or gone back) the limit is 900 of the stream's frames, and after 7200 frames it goes whatever the PTS (a frozen PTS). Histories are swept once a second of PTS and every 30 frames, so one can outlive its age by up to a sweep. 8.0's limit is unknown (it kept a history across a 30-frame absence, as this does) |
| Direction | DeepStream 8.0 behaviour | from n = 10, when \|d\| >= 55 px: every direction rule whose vector is within 60° of d (cos >= 0.5), in config order, as ` DIR:<label> ` pieces in `dirStatus` and `objStatus`. A zero vector never matches. `mode` is ignored |
| STOPPED | DeepStream 8.0 behaviour | only with a direction group, from n = 50: when the window's population standard deviations are σx <= 20 and σy <= 0.1·√h, the object is STOPPED: ` STOPPED ` in `objStatus` and no direction |
| STOPPED limit | ours | h is the current box height (8.0's is undetermined), and 0.1 is the coefficient that best fits 8.0's runs (each measured limit within 0.5 % of it) |
| Direction `class-id` | ours | objects of other classes get no direction or STOPPED, as for the other groups (not observed on 8.0), but still build their history, which line crossing shares |
| Line crossing | DeepStream 8.0 behaviour | "past" is the side of the line the rule's vector points to. The reference must be strictly before the line, and the newest point on or past it (`loose`) or strictly past it (`balanced`, `strict`). Then d must be within the mode's angle of the vector and long enough: `loose` under 90° and 32 px from n = 3, `balanced` under 60° and 55 px from n = 10, `strict` 78 px from n = 20. Any other `mode` reads as `loose` |
| Strict angle | ours | cos > 0.8 (under 36.87°). 8.0 counted a d 36.24° off the vector and not one at exactly cos = 0.8, so its limit is in [36.24°, 36.87°) (the docs give none; 36° would count two of 8.0's crossings late) |
| `extended` | DeepStream 8.0 behaviour | 0: the crossing must hit the segment, lengthened by half the box width at each end, strictly; 1: anywhere on the infinite line |
| Segment hit | ours | with the current box's width (8.0's observed boxes kept theirs); on a slanted segment, the point where reference → newest meets the line, projected on it (8.0 was observed only on horizontal segments crossed straight down) |
| Line sides | ours | exact integer cross products: a point exactly on a slanted line is on it, and counts on that frame in `loose` mode (8.0's float slope counts it a frame later). A vector parallel to its line, a zero vector or a line of one point never counts |
| Line-crossing reference | ours | the mean of the oldest 4 points once the window is full, as for direction (8.0's runs can't tell 2 from 4 there) |
| Counting | DeepStream 8.0 behaviour | an object counts once per line, never against the vector, and never when already past the line at n <= 1. `lcStatus` and ` LC:<label> ` appear on the counting frame only; `objLCCurrCnt` is that frame's count, `objLCCumCnt` the stream's total. No STOPPED gate. Objects of other classes than `class-id`'s are not counted |
| `objStatus` | DeepStream 8.0 behaviour | ` ROI:<l>` per ROI, ` DIR:<l> ` per direction, ` LC:<l> ` per line counted, then ` STOPPED `; `""` when none |
| Untracked objects | ours | objects without a tracker id count for the ROIs, overcrowding and `objCnt`; direction and line crossing skip them, with one `GST_WARNING` per stream (again after a stream reset). 8.0 turns direction and line crossing off for the stream for good |
| Config reload | DeepStream 8.0 behaviour | every stream's state resets, the cumulative counts included (see `config-file`) |
| Stream reset and removal | ours | see `rds-stream-reset` and `rds-pad-deleted` under "Analytics element behaviour". 8.0 ignores stream events, so a source re-added on a pad inherits the old state |
| Config errors | ours | 8.0 crashes, reads garbage or silently misbehaves on: an empty value, a line crossing without exactly 8 values, an ROI with an odd count, a `config-width` or `config-height` of 0, a label used twice in a group or by both the roi-filtering and overcrowding groups of a stream. All are start errors naming the key (see "Analytics config files") |
| Class -1 in `display-obj-cnt` | ours | printed `ClassId-1`, not 8.0's `ClassId4294967295` (see "Analytics drawing behaviour") |

#### Analytics drawing behaviour

`rdsanalytics` never draws pixels: it attaches display metas (and, in `osd-mode=2`, writes the objects' text and
border) for `rdsosd` or any OSD downstream. Each row is **DeepStream 8.0 behaviour** (observed in its display-meta
dumps) or **ours**.

| Behaviour | Status | Notes |
|---|---|---|
| Elements | DeepStream 8.0 behaviour | lines and texts only, no rects, arrows or circles: every line `line_width` 2, every text `font_name` "Serif" at `display-font-size` on black (`set_bg_clr` 1, `text_bg_clr` (0,0,0,1)); coordinates truncated to integers |
| `osd-mode=0` | DeepStream 8.0 behaviour | no display meta, objects untouched (only `display-obj-cnt`'s text) |
| `osd-mode=1` | DeepStream 8.0 behaviour | ROI-filtering polygons yellow (1,1,0,1), overcrowding polygons orange (1,0.5,0,1), one line per edge with the closing edge last, label `<label>` at the first vertex; direction rules red (1,0,0,1): the vector and two arrowhead lines, label `<label>` at the start point; line-crossing rules green (0,1,0,1): the direction vector with its arrowhead, then the line, label `"<label> "` (a trailing space) at the line's second point. Only enabled groups are drawn. Objects untouched |
| Arrowheads | DeepStream 8.0 behaviour | two lines ending at the tip, 15 % of the vector's length, at −15° then +15° from the vector (200 px east from (100,650): (271,657) and (271,642) to (300,650)); a vector of zero length gives three lines on its point |
| `osd-mode=2` labels | DeepStream 8.0 behaviour | as `osd-mode=1`, with `<label>=<objInROIcnt>`, `<label> OverCrowding=<True/False>, Count=<n>` and `<label>=<objLCCumCnt>`; direction labels unchanged |
| `osd-mode=2` objects | DeepStream 8.0 behaviour | each object with an object info gets `text_params` = its objStatus (an overcrowding-only object: `""`), replacing any text it had, at (left, top − 10), white (1,1,1,1) on black, Serif at `display-font-size`. Its border turns cyan (0,1,1,1) when it has an ROI-filtering or line-crossing label on this frame; a direction, STOPPED or overcrowding alone leave it as it was. Objects without an object info are untouched |
| `display-obj-cnt=1` | DeepStream 8.0 behaviour | in every `osd-mode`, the first text: `Count for ClassId<c>=<n> ClassId<c>=<n> …` from `objCnt` (sorted by the class id as unsigned, so −1 last) at (5,5), yellow |
| Class −1 in `display-obj-cnt` | ours | printed `ClassId-1` (8.0 prints `ClassId4294967295`). With no class seen yet the text is `Count for` (8.0 not observed) |
| Order | DeepStream 8.0 behaviour | texts: `display-obj-cnt`'s, then the rules' labels; texts and lines in config order: ROI filtering, overcrowding, direction, line crossing, each group's rules in key order |
| Lines per display meta | DeepStream 8.0 behaviour | at most 15 lines per display meta, then the next one; every text in the first |
| Texts per display meta | ours | past 16 (`RDS_MAX_ELEMENTS_IN_DISPLAY_META`) texts, the next display meta (8.0 not observed with that many) |
| Display meta list | ours | a display meta is acquired only when something goes in it, and appended to `display_meta_list` in the order they fill. 8.0 adds an extra empty one after a full one and prepends them, the newest first |
| Negative coordinates | ours | 0 (an arrowhead off the frame's top or left, an object whose top is under 10 px); the fields are unsigned (8.0 not observed) |
| Drawn by `rdsosd` | ours | "Serif" is fontconfig's serif (DejaVu Serif in `redstream:dev`), not nvdsosd's font, so the glyphs and text boxes differ. Lines are drawn as `rdsosd` draws any line (its "Drawing behaviour" rows): width 2 is drawn 1 px wide, as in nvdsosd, and the diagonal arrowheads have hard edges where nvdsosd smooths them slightly |

#### Analytics config files

- The `config-file` is a GLib key file. `[property]` is required, with `enable`, `config-width` and `config-height`.
  The feature groups are `[roi-filtering-stream-<n>]`, `[overcrowding-stream-<n>]`, `[line-crossing-stream-<n>]`
  and `[direction-detection-stream-<n>]`, where `<n>` is the frame meta's `pad_index` (DeepStream 8.0 behaviour).
- The docs also spell three groups without `stream` (`[overcrowding-<n>]`, ...): such a group, and
  `[roi-filtering-<n>]`, is skipped with a log line naming it (DeepStream 8.0 behaviour). Any other unknown group is
  skipped with a log line too (ours).
- Unknown keys are ignored silently in `[property]` and with a log line in feature groups (DeepStream 8.0
  behaviour). A key of another feature (`time-threshold` outside overcrowding, `roi-<label>` in a line-crossing
  group) is an unknown key.
- Rules (`roi-<label>`, `line-crossing-<label>`, `direction-<label>`) keep their order in the file. A feature group
  without a rule is an error (DeepStream 8.0 behaviour). Lists are `;`-separated; a trailing `;` is accepted.
- Defaults are DeepStream 8.0 behaviour (observed: the docs give none) unless marked "(ours)". **supported**: used;
  **accepted no-op**: read, type-checked and ignored.
- Errors name the file, the group and the key: `FILE:[GROUP] KEY: VALUE: reason`. Besides wrong types and ranges,
  these are errors (ours, where 8.0 crashes, reads garbage or silently misbehaves): an empty value (except
  `class-id`), a `config-width` or `config-height` of 0, an ROI with an odd count or fewer than 3 points, a line
  crossing without exactly 8 values, a direction without exactly 4, negative or non-integer coordinates (fatal in
  8.0 too), an empty list item (`1;;2`), a label used twice in one group or in both the
  roi-filtering and the overcrowding group of a stream, an empty label, and a stream number that is
  not one: not a decimal number without leading zeros (`-stream-00`, `-stream-007`), or above 4294967295. So two
  spellings never name one stream. `class-id=` and `class-id=;` mean every class; `mode=` in a direction group is
  accepted.
- Key-file locale suffixes (`roi-A[fr]`) are not read: such a key is an unknown key (ours).

#### rdsanalytics config keys

| Key | Groups | Type | Default | Range | Status | Notes |
|---|---|---|---|---|---|---|
| `enable` | property | bool | required |  | supported | 0: pass-through, no meta (DeepStream 8.0 behaviour) |
| `config-width` | property | uint | required | >= 1 | supported | the frame size the rule coordinates are in; they are scaled to the mux output. 0 is an error (ours): 8.0 accepts it and every scaled coordinate becomes garbage |
| `config-height` | property | uint | required | >= 1 | supported | as config-width |
| `osd-mode` | property | uint | `2` | 0..2 | supported | 0: no overlay, 1: the rules and their labels, 2: also the counts and statuses |
| `display-font-size` | property | uint | `12` | >= 0 | supported | 0 reads as 12 (DeepStream 8.0 behaviour); no upper bound (ours) |
| `obj-cnt-win-in-ms` | property | uint | `50` (ours) | 1..1000000000 | supported | undocumented: the PTS window of objCnt (the per-class count's mode over the window); the default is ours, within the 33-100 ms measured on DeepStream 8.0 |
| `display-obj-cnt` | property | bool | `0` |  | supported | undocumented: 1 draws the per-class object counts, in every osd-mode (DeepStream 8.0 behaviour) |
| `enable` | roi-filtering, overcrowding, line-crossing, direction-detection | bool | `1` (ours) |  | supported | 0: the feature is off for the stream, and its labels are absent from the metas (DeepStream 8.0 behaviour); 8.0's behaviour without the key is unknown |
| `class-id` | roi-filtering, overcrowding, line-crossing, direction-detection | int list | none | >= -1 | supported | the classes the group's rules apply to; unset, empty or a list holding -1: every class (DeepStream 8.0 behaviour) |
| `inverse-roi` | roi-filtering | bool | `0` |  | supported | 1: the labels go on the objects outside each ROI, and those are counted (DeepStream 8.0 behaviour) |
| `object-threshold` | overcrowding | uint | `0` | >= 0 | supported | overcrowded when the ROI's count is at least this and above 0 (DeepStream 8.0 behaviour); unset flags from a count of 1, as in 8.0 |
| `time-threshold` | overcrowding | uint | `0` | >= 0 | accepted no-op | undocumented; accepted with no effect, as in DeepStream 8.0. In the other feature groups it is an unknown key |
| `mode` | line-crossing | string | `loose` |  | supported | strict, balanced or loose; any other value reads as loose (DeepStream 8.0 behaviour). Matching is case-sensitive (ours: `Strict` reads as loose; 8.0 not observed) |
| `mode` | direction-detection | string | none |  | accepted no-op | accepted and ignored, as in DeepStream 8.0 |
| `extended` | line-crossing | bool | `0` |  | supported | 0: the crossing must hit the segment, 1: the infinite line (DeepStream 8.0 behaviour) |
| `roi-<label>` | roi-filtering, overcrowding | polygon | none | >= 0 | supported | x1;y1;x2;y2;... in order around the polygon, at least 3 points; integers >= 0. A label used by both the roi-filtering and the overcrowding group of a stream is an error (ours): both go in `objInROIcnt`, where 8.0 lets one count overwrite the other. Direction and line-crossing labels are independent. Fewer than 3 points or an odd count is an error (ours): 8.0 accepts them (an ROI that never matches, a missing y read as 0) |
| `line-crossing-<label>` | line-crossing | line | none | >= 0 | supported | 8 integers >= 0: the direction vector (x1;y1;x2;y2), then the line's two ends. Another count is an error (ours): 8.0 reads a short line as garbage and ignores extra values |
| `direction-<label>` | direction-detection | vector | none | >= 0 | supported | 4 integers >= 0: the vector from (x1;y1) to (x2;y2). Another count is an error (ours): 8.0 reads a missing value as 0 |

### `rdsh264enc` (`nvv4l2h264enc`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html))

A bin: `rdsvideoconvert ! video/x-raw,format=NV12 ! queue ! vah264enc !
video/x-h264,stream-format=byte-stream,alignment=au`, made when the element first goes to READY. Names, types, ranges
and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0's nvv4l2h264enc (the enum type names are ours). Flags
(ours: 8.0's are wrong both ways): `bitrate`, `force-idr` and `force-intra` are changeable in PLAYING; every other
writable property is changeable only in NULL or READY, and is read when the element starts (READY to PAUSED). Set later,
it is stored, logs a warning and applies at the next start (DeepStream 8.0 takes some silently and ignores others). An
accepted no-op logs one warning per property the first time it gets a value other than its default. At every start the
bin sets them on `vah264enc` as the Notes say; a value outside VA's range is clamped to it. The output caps carry the
profile and level the stream has (ours: DeepStream 8.0 writes "NULL"). VA picks the level from the size, frame rate and
bitrate, and ignores a downstream one. Downstream caps that fix a `level` at or above VA's get it: it is written into
every SPS (`level_idc`; H.264 "1b" as `level_idc` 11 with `constraint_set3_flag` in Baseline, Main and Extended, 9 in
High) and, for H.265, every VPS and SPS (`general_level_idc`), and the output caps carry it (DeepStream 8.0
behaviour). A downstream level below VA's, or a list of levels, is not-negotiated with a warning naming both levels
(ours: the stream would claim a level its size, frame rate or bitrate exceeds).

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `bitrate` | uint | `4000000` | supported | bit/s: VA's `bitrate` in kbit/s (rounded, at least 1, at most VA's 2048000). Changeable in PLAYING (DeepStream 8.0 behaviour); on VA a change there forces an IDR, and after a drop the rate settles above the target (ours). Don't set it from a notify or deep-notify handler of the bin or its VA encoder: one that runs during a start or a bitrate change deadlocks, as the lock that orders the two is not recursive |
| `control-rate` | enum `variable_bitrate` (0), `constant_bitrate` (1), `constantQP` (2) | `constant_bitrate` | supported | VA's `rate-control`: vbr, cbr, cqp |
| `maxbitrate` | uint | `0` | supported | bit/s: with `variable_bitrate` and above `bitrate`, VA's `target-percentage` = 100 × bitrate / maxbitrate, at least VA's 50, so a peak above twice `bitrate` gives twice `bitrate` (ours); otherwise no effect (DeepStream 8.0 behaviour) |
| `constqp` | string | NULL | supported | `I:P:B`, each 0..51: VA's `qpi`, `qpp`, `qpb`, used with `constantQP` only (DeepStream 8.0 behaviour). A malformed string is refused with a warning naming the property, and the old value stays (ours: DeepStream 8.0 crashes); NULL unsets it |
| `qp-range` | string | NULL | supported | `minP,maxP:minI,maxI:minB,maxB`, each 0..51: VA's `min-qp` and `max-qp` from the P pair (ours: VA has one range); NULL: VA's default range. Malformed strings as `constqp` |
| `vbvbufsize` | uint | `0` | supported | bits: VA's `cpb-size` in kbit (0: VA's choice) |
| `idrinterval` | uint | `256` | supported | VA's `key-int-max`; 0 means 256 (DeepStream 8.0 behaviour); above 1024, 1024 with a warning (ours) |
| `iframeinterval` | uint | `4294967295` | supported | 0 and 4294967295: no I frames between IDR frames (DeepStream 8.0 behaviour; the docs say 30). Otherwise VA's `i-frames` = ⌈idrinterval / N⌉ − 1, which VA spaces evenly: the same frames as 8.0 when N divides idrinterval (ours) |
| `force-idr` | boolean | `FALSE` | supported | TRUE makes the next frame into the bin an IDR frame (more exactly the next one out of the inner converter into the queue in front of the VA encoder, so frames already queued are not the ones forced; ours: DeepStream 8.0 forces the frame after the one being processed), and a force-key-unit event goes downstream before it. Changeable in PLAYING (DeepStream 8.0 behaviour; its flags say NULL or READY only) and PAUSED; `min-force-key-unit-interval` applies. The IDR frame has parameter sets and the property reads back FALSE (ours: DeepStream 8.0's forced IDR frame has none unless `insert-sps-pps`, and it reads back TRUE). In NULL or READY it does nothing, as the first frame is an IDR frame anyway (ours). The setter never waits for the stream: the request waits for that frame, then goes to the VA encoder as an upstream force-key-unit event |
| `force-intra` | boolean | `FALSE` | supported | as `force-idr`: the frame is an IDR frame too (ours: VA can't force a non-IDR intra frame; DeepStream 8.0 gives an I frame, on H.265 one that is not a random access point) |
| `profile` | enum `Baseline` (0), `Constrained-Baseline` (1), `Main` (2), `High` (4), `Constrained-High` (17), `High444` (7) | `Baseline` | supported | the preferred profile, used when downstream caps allow it; downstream caps that fix `profile` win (DeepStream 8.0 behaviour). A pad probe narrows VA's downstream caps query to it (VA takes the highest profile allowed, whatever the order of a list). Baseline and Constrained-Baseline give constrained-baseline, Main main, High and Constrained-High high (ours: 8.0 fails on the two Constrained values). `High444` is an error at start (READY to PAUSED) naming the profile (ours: VA has no High 4:4:4) |
| `num-B-Frames` | uint 0..2 | `0` | supported | VA's `b-frames`. With a Baseline profile (from the property or from downstream caps): a warning and no B frames (ours: DeepStream 8.0 hangs, and VA alone writes B slices into the Baseline stream). When downstream allows Baseline and another profile and the property's isn't allowed, B frames pick the other (ours) |
| `num-Ref-Frames` | uint 0..8 | `0` | supported | VA's `ref-frames` when above 0; 0 keeps VA's default, 3, the number DeepStream 8.0 uses |
| `disable-cabac` | boolean | `FALSE` | supported | VA's `cabac` = !disable-cabac |
| `insert-aud` | boolean | `FALSE` | supported | VA's `aud` |
| `min-force-key-unit-interval` | uint64 | `0` | supported | nanoseconds: passed to the VA encoder |
| `qos` | boolean | `FALSE` | supported | passed to the VA encoder |
| `copy-meta` | boolean | `FALSE` | supported | TRUE: the metas of the element's input buffer (`RdsBatchMeta` included, deep-copied; metas tied to the memory left out) are copied onto the encoded buffer of the same frame, found by its input PTS, so B-frame reordering doesn't matter; an input frame's metas are kept until its output, a flush or EOS, or until 64 newer frames came in. FALSE: the encoded buffers carry no `RdsBatchMeta` (DeepStream 8.0 behaviour). Buffer flags come from the encoder (ours: DeepStream 8.0 copies the input's flags too, clearing DELTA_UNIT on every buffer) |
| `gpu-id` | uint | `0` | supported | the inner `rdsvideoconvert`'s `gpu-id` |
| `preset-id` | uint 1..7 | `1` | accepted no-op | VA's `target-usage` is always 1 (ours: the only setting that holds CBR at the target, and the fastest) |
| `insert-sps-pps` | boolean | `FALSE` | accepted no-op | parameter sets always precede every IDR frame (ours) |
| `initqp` | string | NULL | accepted no-op | malformed strings are refused as for `constqp` |
| `cq` | uint 0..51 | `0` | accepted no-op | |
| `aq` | uint 0..15 | `0` | accepted no-op | |
| `vbvinit`, `SliceIntraRefreshInterval` | uint | `0` | accepted no-op | |
| `slice-header-spacing` | uint64 | `0` | accepted no-op | |
| `temporalaq`, `extended-colorformat`, `bit-packetization`, `enable-lossless` | boolean | `FALSE` | accepted no-op | |
| `insert-vui` | boolean | `TRUE` | accepted no-op | |
| `intra-refresh` | string | `1,30,1` | accepted no-op | DeepStream 8.0 shows this default but does not apply it |
| `tuning-info-id` | enum `HighQualityPreset` (1), `LowLatencyPreset` (2), `UltraLowLatencyPreset` (3), `LosslessPreset` (4) | `LowLatencyPreset` | accepted no-op | |
| `sei-uuid` | string | `NVDS_CUSTOMMETA` | accepted no-op | |
| `cudaenc-memtype` | enum `memtype_device` (0), `memtype_pinned` (1), `memtype_unified` (2) | `memtype_device` | accepted no-op | |
| `capture-io-mode`, `output-io-mode` | enum `auto` (0), `mmap` (2), `dmabuf-import` (5) | `auto` | accepted no-op | |
| `extra-controls` | GstStructure | NULL | accepted no-op | |
| `device` | string, read-only | `/dev/v4l2-nvenc` | accepted no-op | DeepStream 8.0's value; nothing opens it |
| `device-fd` | int, read-only | `-1` | accepted no-op | |
| `device-name` | string, read-only | empty | accepted no-op | |

Pads: `sink` (a ghost pad of `rdsvideoconvert`'s): `memory:RDS` or system memory, `NV12`, `I420`, `RGBA`, `BGRA`,
`RGBx`, `BGRx`, `RGB`, `BGR`, `GRAY8` (ours: DeepStream 8.0 takes NVMM only); `src`: `video/x-h264`,
`stream-format=byte-stream`, `alignment=au`. Without `vah264enc` (no VA plugin, or a driver that can't encode H.264)
the element still registers, and going to READY is an element error naming `vah264enc`. Rank none.

Stream behaviour (pad probes on the inner elements):

- **Threads and memory:** the VA encoder runs on a streaming thread of its own, behind a queue of up to 3 frames
  (unlimited in bytes and time), and the inner converter downloads into a plain system-memory pool of its own, which
  VA copies into its surfaces (ours: more than twice the frame rate of one thread writing into VA's mapped surfaces).
  So the bin adds a streaming thread and holds up to 3 converted NV12 frames in the queue (5 in the pool); the queue
  adds nothing to the minimum latency and makes the maximum unbounded, as any queue without a time limit does. After
  a resolution change the converter writes into VA's own surfaces instead, which is slower (VA copies only into
  surfaces of the first size of the run), and into plain memory again back at the first size.

- **Filler:** filler NAL units (type 12) are removed from the output, so CBR's bitrate is a ceiling and easy content
  comes out smaller (DeepStream 8.0 behaviour: its output has none). VA adds them once its rate-control buffer has
  filled, a few seconds into a CBR stream.
- **Timestamps:** VA moves PTS, DTS and the segment forward so that the first PTS is at least 1000 h (by 1000 h minus
  the first PTS, measured again after a flush). Without B frames the bin removes that shift: output PTS = DTS = the
  input PTS, durations and the segment are the input's (DeepStream 8.0 behaviour). With B frames VA's timestamps and
  segment stay as they are (running time is the input's, PTS reordered, DTS increasing and below PTS), since removing
  the shift would put the first DTS before 0 (ours: DeepStream 8.0 writes DTS = PTS and doesn't reorder PTS).
- **DELTA_UNIT:** cleared on intra frames (IDR and I) and set on the others (DeepStream 8.0 behaviour; VA alone
  clears it on IDR frames only). The first buffer is DISCONT.
- **Batches:** a buffer whose `RdsBatchMeta` has more than one frame is an element error, "takes one frame per
  buffer" (ours: DeepStream 8.0 encodes the first frame of the batch only, silently). Put a tiler or a demuxer first.
- **Framerate:** the output caps carry the input caps' framerate, from the first frame at a new rate (ours: VA restarts
  its sequence on a frame-rate change but keeps the old framerate in its caps, and sends none when nothing else
  changed). The VUI timing follows: that frame is an IDR frame whose SPS has the new rate (ours: DeepStream 8.0
  keeps the old rate in the VUI).
- **Force-key-unit events:** from either direction (an upstream one from downstream, a downstream one from upstream)
  they reach the VA encoder, which makes the next frame (or the frame at the event's running time) an IDR frame with
  parameter sets, whatever `all-headers` says (ours: DeepStream 8.0 ignores them). A downstream one passes the queue in
  order with the frames; an upstream one without a running time waits, as `force-idr` does, for the next frame into the
  queue (ours), and one with a running time goes to VA at once. The VA encoder sends a downstream force-key-unit event
  before that frame; its timestamp is the frame's PTS, in the output's time domain (without B frames the input's, like
  the buffers).
- **Resolution change:** new input caps mid-stream renegotiate the inner converter and the VA encoder: the first frame
  at the new size is an IDR frame with parameter sets and an SPS of the new size, and new output caps precede it
  (DeepStream 8.0 behaviour for H.264; for H.265 ours, DeepStream 8.0 hangs).
- **Sizes:** what the VA encoder takes on system memory, read from its sink template: here 128 to 4096 on each side
  (DeepStream 8.0: width 145 and up, up to 4096×4096). The sink pad's caps queries answer only that range (its
  template stays the converter's), and input caps outside are not-negotiated, at the start or mid-stream, with a
  warning naming the range, once per run (ours: VA posts nothing, and without the check the inner converter would
  scale the frames into the range).
- **Odd sizes:** coded as the next even size, 1281×721 as 1282×722, the SPS cropping the coded picture to it
  (DeepStream 8.0 behaviour: 4:2:0 crops in 2-pixel units); the output caps carry the input size.

DeepStream 8.0 bugs the bins don't reproduce (all ours; details in the rows and items above):

- **B-frame timestamps:** 8.0 writes DTS = PTS and leaves PTS in decode order; here PTS is reordered and DTS
  increases.
- **Baseline with B frames:** 8.0 hangs; here B frames are dropped with a warning (`num-B-Frames`).
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

### `rdsh265enc` (`nvv4l2h265enc`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html))

The same bin over `vah265enc`, with `video/x-h265` on `src`, and the same properties as `rdsh264enc` (from
DeepStream 8.0's nvv4l2h265enc), except that it has no `disable-cabac` and its own `profile` enum.
Stream behaviour as `rdsh264enc` (filler NAL units are type 38; VA on VCN 3 writes no B frames, but with
`num-B-Frames` above 0 the timestamps keep VA's shift all the same), and:

- **Conformance window:** the SPS before every IDR frame gets the window that crops the coded picture to the input
  size (rounded up to even), so the stream decodes at the input size (ours: VA on this Mesa pads the width to a
  multiple of 64 but computes the window from a multiple of 16, so 720x480 decodes 768 wide and 1366x768 1398 wide;
  DeepStream 8.0's streams decode at the input size). After a resolution change, from the new size.
- **Sizes:** width 130 to 8192, height 128 to 4352 (DeepStream 8.0: width 129 and up, up to 8192×8192); outside,
  not-negotiated with the warning, as `rdsh264enc`.

| Property | Type | Default | Status | Notes |
|---|---|---|---|---|
| `bitrate`, `control-rate`, `maxbitrate`, `constqp`, `qp-range`, `vbvbufsize`, `idrinterval`, `iframeinterval`, `force-idr`, `force-intra`, `num-Ref-Frames`, `insert-aud`, `min-force-key-unit-interval`, `qos`, `copy-meta`, `gpu-id` | as `rdsh264enc` | as `rdsh264enc` | supported | as `rdsh264enc` |
| `profile` | enum `Main` (0), `Main10` (1), `FREXT` (3) | `Main` | supported | `Main` gives main. `Main10` and `FREXT` are an error at start (READY to PAUSED) naming the profile (ours: they need P010 or 4:4:4 input, which the bin can't produce yet) |
| `num-B-Frames` | uint 0..2 | `0` | supported | VA's `b-frames`; VA on VCN 3 writes no B frames in H.265 whatever the value |
| `preset-id`, `insert-sps-pps`, `initqp`, `cq`, `aq`, `vbvinit`, `SliceIntraRefreshInterval`, `slice-header-spacing`, `temporalaq`, `extended-colorformat`, `bit-packetization`, `enable-lossless`, `insert-vui`, `intra-refresh`, `tuning-info-id`, `sei-uuid`, `cudaenc-memtype`, `capture-io-mode`, `output-io-mode`, `extra-controls`, `device`, `device-fd`, `device-name` | as `rdsh264enc` | as `rdsh264enc` | accepted no-op | as `rdsh264enc` |

## Audio (on request)

DeepStream 8.0 batches and infers on audio; NVIDIA has since moved audio AI out of DeepStream (to Riva) and removed
the audio plugins in 9.2. None of it is implemented here, and it is built only if users ask for it. The two audio elements are already in the
remap table (they fail to create, naming the Redstream element, until they exist); the audio side of the mux and demux
and the audio batch buffer have no counterpart yet. The plan: `rdsinferaudio` runs its model on MIGraphX, like
`rdsinfer`, and computes the log-mel spectrogram on the GPU.

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `nvinferaudio` | `rdsinferaudio` | planned | audio inference on batched audio with batch meta attached: the `audio-transform` property selects a log-mel spectrogram (`melsdb`: FFT length, hop, window, mel bins, sample rate, power-to-dB limits), applied before the model; `audio-framesize` and `audio-hopsize` set the frame and hop size the transform uses; outputs frame meta and classifier meta. Config files in key-file format, `[property]` and `[class-attrs-all]` / `[class-attrs-<id>]` groups | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/text/DS_plugin_gst-nvinferaudio.html) |
| `nvdsaudiotemplate` | `rdsaudiotemplate` | planned | runs a custom library on single or batched S16LE/F32LE audio (`customlib-name`, `customlib-props`) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/text/DS_plugin_gst-nvdsaudiotemplate.html) |
| `nvstreammux` (audio input) | `rdsstreammux` | planned | the new mux only: mono S16LE or F32LE buffers from any number of sources, all pads with the same fixed caps (rate, format), batched into one audio buffer with batch meta; `rdsstreammux` is the default (video) mux today | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/text/DS_plugin_gst-nvstreammux2.html) |
| `nvstreamdemux` (audio batches) | `rdsstreamdemux` | planned | splits an audio batch back into per-source streams (the multi-stream audio template pipeline) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/text/DS_plugin_gst-nvdsaudiotemplate.html), [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/text/DS_plugin_gst-nvstreammux2.html) |
| `NvBufAudio` | `RdsBufAudio` | planned | the batched audio buffer the new mux outputs (the counterpart of `NvBufSurface`); its layout is not on the public pages, so ours will be designed independently | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/text/DS_plugin_gst-nvstreammux2.html) |

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

Batched crop, scale and colour conversion between surfaces, and the many-to-one composite, run by Redstream's own
HIP kernels. Types are `typedef`s, functions and enumerators are object-like macros. All five functions have
DeepStream's exact prototypes, so they can also be assigned to function pointers declared with them.

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
| `NvBufSurfTransform_Composite_Flag` | `RdsBufSurfTransform_Composite_Flag` | supported | values as documented; `composite_flag` is ignored, as in DeepStream 8.0 | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvbufsurftransform_8h.html) |
| `NVBUFSURF_TRANSFORM_COMPOSITE` | `RDS_BUFSURF_TRANSFORM_COMPOSITE` | supported | ignored | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvbufsurftransform_8h.html) |
| `NVBUFSURF_TRANSFORM_BLEND` | `RDS_BUFSURF_TRANSFORM_BLEND` | supported | ignored (no blending) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvbufsurftransform_8h.html) |
| `NVBUFSURF_TRANSFORM_COMPOSITE_FILTER` | `RDS_BUFSURF_TRANSFORM_COMPOSITE_FILTER` | supported | ignored: `composite_filter` is always read | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvbufsurftransform_8h.html) |
| `NvBufSurfTransformCompositeParams` | `RdsBufSurfTransformCompositeParams` | supported | `input_buf_count` is ignored (`src->numFilled` decides), as in DeepStream 8.0 | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvbufsurftransform_8h.html) |
| `NvBufSurfTransformComposite` | `rds_buf_surf_transform_composite` | supported | synchronous; see "Composite semantics" | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvbufsurftransform_8h.html) |

Not aliased: `NvBufSurfTransformAsync`, `NvBufSurfTransformCompositeAsync`, the blend calls and their types
(`NvBufSurfTransformCompositeBlend*`, `NvBufSurfTransformMultiInputBufCompositeBlend*`), the sync objects (`NvBufSurfTransformSyncObj*`),
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

### Composite semantics

What `rds_buf_surf_transform_composite` (`NvBufSurfTransformComposite`) does. Where NVIDIA's reference is silent it
matches observed DeepStream 8.0 behaviour; rows marked "ours" are Redstream's own choice.

| Case | Behaviour | Origin |
|---|---|---|
| Entries | the first `src->numFilled` source surfaces; entry `i` scales `src_comp_rect[i]` of source surface `i` into `dst_comp_rect[i]` of destination surface 0. Other destination surfaces are not written. `input_buf_count` and `composite_flag` are ignored | DeepStream 8.0 behaviour |
| Sampling and colour | as in "Transform semantics" (the composite runs the same kernels), with `composite_filter` as the filter; sources may differ in format and size, and each is stretched into its rect (no aspect preservation) | DeepStream 8.0 behaviour (stretch, NV12/RGBA conversion); rules ours |
| Pixels outside every destination rect | not written | DeepStream 8.0 behaviour |
| Overlapping destination rects | drawn in entry order, later entries on top | DeepStream 8.0 behaviour |
| `src_comp_rect` or `dst_comp_rect` NULL | `Invalid_Params`, destination untouched | DeepStream 8.0 behaviour |
| Destination rect partly outside the surface | success; the rect keeps its scaling and only its part inside the surface is drawn | clipping: DeepStream 8.0 behaviour; which part of the source shows: ours |
| Source rect partly outside its surface | success; the rect is cut to the surface and that part is scaled into the destination rect | clipping: DeepStream 8.0 behaviour; cut: ours |
| Rect empty, or wholly outside its surface | the entry draws nothing (success) | ours (8.0 draws a 2x2 block for a zero-size destination rect) |
| Rect wider or taller than 16384 | `ROI_Error`, destination untouched | ours |
| Odd destination rect on NV12 or I420 | left and top rounded up to even, an odd right or bottom edge rounded down unless it is the surface edge; the source is scaled onto that rect | origin: DeepStream 8.0 behaviour; odd right or bottom edge: ours |
| Odd destination rect on an RGB-family or GRAY8 surface | used as given | ours (8.0 also rounds RGBA rects to even) |
| `dst->numFilled` | not changed | ours |
| Other arguments | the transform's rules for memory types, the session GPU, formats, layouts and plane geometry (the destination needs one surface); `numFilled` 0 or above `src->batchSize`, an out-of-range filter, a NULL `dataPtr` or a source whose pixel memory (`dataPtr`, `dataSize` bytes) overlaps the destination's is `Invalid_Params` | ours |
| Execution | synchronous, on the session's stream and GPU, like `NvBufSurfTransform` | ours (no async variant) |

## OSD parameter structs

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NvOSD_ColorParams` | `RdsOSD_ColorParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_FontParams` | `RdsOSD_FontParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_TextParams` | `RdsOSD_TextParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_RectParams` | `RdsOSD_RectParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_LineParams` | `RdsOSD_LineParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_ArrowParams` | `RdsOSD_ArrowParams` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
| `NvOSD_CircleParams` | `RdsOSD_CircleParams` | supported | with `circle_width` between `bg_color` and `reserved`, as documented | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvll__osd__struct_8h.html) |
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
| `NVDSINFER_TENSOR_OUTPUT_META` | `RDS_INFER_TENSOR_OUTPUT_META` | supported | 12, its documented position in the enum | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2nvdsmeta_8h.html) |
| `NVDS_FRAME_META_NVDSANALYTICS` | `RDS_FRAME_META_ANALYTICS` | supported | 32, DeepStream 8.0's number (its position in 8.0's enum): the type of rdsanalytics' frame user meta | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvdsmeta_8h.html) |
| `NVDS_OBJ_META_NVDSANALYTICS` | `RDS_OBJ_META_ANALYTICS` | supported | 33, DeepStream 8.0's number: the type of rdsanalytics' object user meta | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvdsmeta_8h.html) |
| `NVDS_USER_FRAME_META_NVDSANALYTICS` | `RDS_FRAME_META_ANALYTICS` | supported | the enum value, as in DeepStream 8.0. Before 8.0 it was the type registered as `NVIDIA.DSANALYTICSFRAME.USER_META`; that lookup (`nvds_get_user_meta_type`) gives another type, which rdsanalytics never attaches | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvds__analytics__meta_8h.html) |
| `NVDS_USER_OBJ_META_NVDSANALYTICS` | `RDS_OBJ_META_ANALYTICS` | supported | as the frame one (`NVIDIA.DSANALYTICSOBJ.USER_META` before 8.0) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/nvds__analytics__meta_8h.html) |
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
| `nvds_copy_obj_meta_list` | `rds_copy_obj_meta_list` | supported | appends deep copies to the frame. Ours: parents are remapped within the list (a parent outside it becomes NULL) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `nvds_copy_display_meta_list` | `rds_copy_display_meta_list` | supported | appends deep copies to the frame | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsmeta_8h.html) |
| `gst_buffer_get_nvds_batch_meta` | `gst_buffer_get_rds_batch_meta` | supported |  | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html) |
| `NvDsInferTensorMeta` | `RdsInferTensorMeta` | supported | `rds/rds-infer-meta.h`; the documented fields in order; `priv_data` is rdsinfer's (it keeps the buffers alive). See the `rdsinfer` section | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferTensorMeta.html) |
| `NvDsAnalyticsFrameMeta` | `RdsAnalyticsFrameMeta` | supported | C++ struct in `rds/rds-analytics-meta.h` (for `nvds_analytics_meta.h`; library `libgstrds-analytics-1.0`, pkg-config `redstream-analytics-1.0`): `ocStatus`, `objInROIcnt`, `objLCCurrCnt`, `objLCCumCnt`, `unique_id`, `objCnt` with the documented types, in the documented order. `rds_compat.h` names it by struct tag, so it is opaque until that header is included. Ours: a C API over it (`rds_analytics_frame_meta_*`: attach, setters, getters by key, foreach). Deep-copied by `rds_batch_meta_copy`, freed with its user meta | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/structNvDsAnalyticsFrameMeta.html) |
| `NvDsAnalyticsObjInfo` | `RdsAnalyticsObjInfo` | supported | C++ struct in `rds/rds-analytics-meta.h`: `roiStatus`, `ocStatus`, `lcStatus`, `dirStatus`, `unique_id`, `objStatus` with the documented types, in the documented order. Ours: a C API over it (`rds_analytics_obj_info_*`: attach, setters, counts and indexed access) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/8.0/sdk-api/structNvDsAnalyticsObjInfo.html) |

Not aliased: `gst_buffer_add_nvds_meta` (Redstream attaches the batch with `rds_buffer_add_batch_meta`;
[ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2includes_2gstnvdsmeta_8h.html)), and
NvDs metadata types and functions Redstream doesn't implement yet (segmentation, event message, audio).

## Stream events (gst-nvevent)

Events are custom events with their own `GstEventType` numbers, so dispatching on `GST_EVENT_TYPE` works.
`rdsstreammux` sends them and `rdsstreamdemux` routes them per source (see their sections above).

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

## nvinfer custom parser ABI (nvdsinfer.h, nvdsinfer_custom_impl.h)

`rds/rds-infer.h` has the layer, network and object types (C and C++); `rds/rds-infer-custom-impl.h` has the parser
ABI (C++). Their fields keep NVIDIA's documented names and order, so a DeepStream bbox or classifier parser
recompiles unchanged. `rdsinfer` loads the parser named by `custom-lib-path` and `parse-bbox-func-name` (or
`parse-classifier-func-name` for a classifier) and calls it on every inferred frame (object, in secondary mode).

`rds/rds_compat.h` (C and C++) aliases only the C-compatible `NvDsInfer*` types below. The parser ABI names are in
`rds/rds_compat_infer.h` (C++), the replacement for `nvdsinfer_custom_impl.h`, which also includes `rds_compat.h`:

```cpp
#ifdef USE_REDSTREAM
#include <rds/rds_compat_infer.h>
#else
#include "nvdsinfer_custom_impl.h"
#endif
```

- **Build parsers in the Redstream toolchain** (libstdc++, `_GLIBCXX_USE_CXX11_ABI=1`, never `-static-libstdc++`):
  the ABI passes `std::vector`.
- Including the parser header (`rds-infer-custom-impl.h` or `rds_compat_infer.h`) defines a weak
  `rds_infer_parser_abi_version()` in the library; include it only in parser libraries. `rdsinfer` checks it when
  it loads `custom-lib-path`: a library without its own copy (one from a library it depends on does not count), or
  with another version, fails to start with a message naming the library. The symbol is exported even with
  `-fvisibility=hidden` (the parse functions then need `__attribute__ ((visibility ("default")))`); with a version
  script or `-Wl,--exclude-libs`, export it explicitly.
- The library is loaded with `RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE`: it is never unloaded (GStreamer never unloads
  plugins either), so HIP code objects, GTypes and threads in it stay valid.
- A parse function that returns `false` or throws: that image gets no detections and `rdsinfer` posts a warning
  (nothing is documented for this on DeepStream). It must not call `exit()` or `abort()`.
- The parse function is called from one thread at a time per loaded parser. `outputLayersInfo` and its buffers are
  valid only during the call. `buffer` is host memory; `deviceBuffer` (ours) is the same layer's data on the
  element's GPU (`gpu-id`); both are complete (synchronised) when the call starts.
- The unprefixed `FLOAT`, `HALF`, `INT8`, `INT32`, `INT64` are C++ only (`constexpr` constants of
  `RdsInferDataType`) and come only with `rds_compat_infer.h`; in C, use `RDS_INFER_DATA_*`.

| DeepStream name | Redstream name | Status | Notes | Source |
|---|---|---|---|---|
| `NVDSINFER_MAX_DIMS` | `RDS_INFER_MAX_DIMS` | supported | 8 | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `NvDsInferDims` | `RdsInferDims` | supported | per image: no batch dimension; `numElements` is the per-image element count | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferDims.html) |
| `NvDsInferDimsCHW` | `RdsInferDimsCHW` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferDimsCHW.html) |
| `NvDsInferDataType` | `RdsInferDataType` | supported | `RDS_INFER_DATA_FLOAT` 0, `_HALF` 1, `_INT8` 2, `_INT32` 3, `_INT64` 4 | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `FLOAT`, `HALF`, `INT8`, `INT32`, `INT64` | `RDS_INFER_DATA_FLOAT`, ... | supported | C++ only, `rds_compat_infer.h`, see above | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `NvDsInferLayerInfo` | `RdsInferLayerInfo` | supported | `buffer` is host memory; ours, appended: `deviceBuffer`, the same data on the GPU (or NULL). `bindingIndex` is the layer's index among the program's outputs | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferLayerInfo.html) |
| `NvDsInferNetworkInfo` | `RdsInferNetworkInfo` | supported |  | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferNetworkInfo.html) |
| `NvDsInferParseDetectionParams` | `RdsInferParseDetectionParams` | supported | C++ only, `rds_compat_infer.h`; with the deprecated `perClassThreshold` reference to `perClassPreclusterThreshold`; ours: a copy constructor and assignment that keep it bound to the copy's own thresholds | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferParseDetectionParams.html) |
| `NvDsInferObjectDetectionInfo` | `RdsInferObjectDetectionInfo` | supported | the DeepStream 9 layout, with `rotation_angle` (28 bytes); a non-zero angle is ignored with a warning. Coordinates in network-input pixels | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/9.1/sdk-api/structNvDsInferObjectDetectionInfo.html) |
| `NvDsInferParseObjectInfo` | `RdsInferParseObjectInfo` | supported | the same type as `RdsInferObjectDetectionInfo` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer_8h.html) |
| `NvDsInferParseCustomFunc` | `RdsInferParseCustomFunc` | supported | C++ only, `rds_compat_infer.h` | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer__custom__impl_8h.html) |
| `CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE` | `RDS_CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE` | supported | C++ only, `rds_compat_infer.h`; a mismatched signature fails to compile | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer__custom__impl_8h.html) |
| `NvDsInferAttribute` | `RdsInferAttribute` | supported | C++ only, `rds_compat_infer.h`; the parser `strdup`s `attributeLabel` and rdsinfer frees it (also after a false return or an exception); NULL is an empty label (ours) | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/structNvDsInferAttribute.html) |
| `NvDsInferClassiferParseCustomFunc` | `RdsInferClassifierParseCustomFunc` | supported | C++ only, `rds_compat_infer.h`; NVIDIA's spelling for the alias, ours spelled out; see "Classifier output (classifier meta)" | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer__custom__impl_8h.html) |
| `CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE` | `RDS_CHECK_CUSTOM_CLASSIFIER_PARSE_FUNC_PROTOTYPE` | supported | C++ only, `rds_compat_infer.h`; a mismatched signature fails to compile | [ref](https://archive.docs.nvidia.com/metropolis/deepstream/7.1/sdk-api/nvdsinfer__custom__impl_8h.html) |

Not aliased (no Redstream counterpart yet, or none on ROCm):
- the deprecated `dims` member of `NvDsInferLayerInfo`'s union (not in the public docs), `getDimsCHWFromDims`,
  `getDimsHWCFromDims`;
- `NvDsInferInstanceMaskInfo`, and the instance-mask, semantic-segmentation and engine-create parser types
  (`NvDsInferInstanceMaskParseCustomFunc`, `NvDsInferSemSegmentationParseCustomFunc`,
  `NvDsInferEngineCreateCustomFunc`) with their check macros;
  `NvDsInferInitializeInputLayers`; the TensorRT-specific functions (`NvDsInferCreateModelParser`,
  `NvDsInferPluginFactoryRuntimeGet` / `Destroy`, `NvDsInferCudaEngineGet`);
- `NvDsInferStatus` and its `NVDSINFER_*` values, `NvDsInferStatus2Str`, `NvDsInferLogLevel`;
- the `nvdsinfer_context.h` names: `NvDsInferNetworkMode`, `NvDsInferNetworkType`, `NvDsInferFormat`,
  `NvDsInferTensorOrder`, `NvDsInferClusterMode`, `NvDsInferDetectionParams`, `_NvDsInferContextInitParams`,
  `NVDSINFER_MAX_BATCH_SIZE`, `NVDSINFER_MAX_CHANNELS`, `NVDSINFER_PATH_MAX`, `NVDSINFER_MAX_STR_LENGTH` (the
  element reads the same values from its config file).
