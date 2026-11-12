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
| `nvurisrcbin` | `rdsurisrcbin` | supported | decodes file and other non-RTSP URIs (uridecodebin3, first video stream only) to `memory:RDS` NV12 on `vsrc_0`, with `file-loop` and `drop-frame-interval`; RTSP URIs through `rtspsrc` and `rdsvideodec` (H.264 and H.265), with reconnection after timeouts, errors and the server's EOS; properties below, behaviour on [its page](elements/rdsurisrcbin.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvurisrcbin) |
| `nvv4l2decoder` | `rdsvideodec` | supported | rocDecode: H.264/H.265 8-bit 4:2:0 to NV12; rank primary+10, so `uridecodebin` picks it over the VA decoders; properties below, behaviour on [its page](elements/rdsvideodec.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvvideo4linux2), [name](https://docs.nvidia.com/deepstream/text/DS_FAQ) |
| `nvvideoconvert` | `rdsvideoconvert` | supported | single frames (batch size 1); `memory:RDS` and system memory on either side; properties below, behaviour on [its page](elements/rdsvideoconvert.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvvideoconvert) |
| `nvstreammux` | `rdsstreammux` | supported | the default (scaling) mux only; the new mux is not implemented yet. GstAggregator-based; properties below, behaviour on [its page](elements/rdsstreammux.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreammux) |
| `nvstreamdemux` | `rdsstreamdemux` | supported | zero-copy read-only views of the batch slots; properties below, behaviour on [its page](elements/rdsstreamdemux.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvstreamdemux) |
| `nvinfer` | `rdsinfer` | supported | primary and secondary detectors, secondary classifiers and tensor-output networks (`network-type` 0, 1 and 100) on MIGraphX; nvinfer config files (key file or YAML); properties below, behaviour on [its page](elements/rdsinfer.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvinfer) |
| `nvtracker` | `rdstracker` | supported | the IOU and NvSORT trackers of NvMultiObjectTracker, on the CPU, with its YAML config files; NvDCF and NvDeepSORT configs run as NvSORT. Matches observed DeepStream 8.0 behaviour where the docs are silent; properties below, config keys and behaviour on [its page](elements/rdstracker.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvtracker) |
| `nvmultistreamtiler` | `rdsmultistreamtiler` | supported | NV12 and RGBA `memory:RDS` batches into one tiled frame of the same format; matches observed DeepStream 8.0 behaviour where the docs are silent; properties below, behaviour on [its page](elements/rdsmultistreamtiler.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvmultistreamtiler) |
| `nvdsosd` | `rdsosd` | supported | RGBA `memory:RDS` frames (single or batched), drawn on the GPU (`process-mode` 1, the default) or the CPU (`process-mode=0`), the same pixels either way, in the geometry of DeepStream 8.0's default GPU mode (Pango for text); masks and blur not yet; optional (`-Dosd`, needs cairo and pangocairo); properties below, behaviour on [its page](elements/rdsosd.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsosd) |
| `nvdsanalytics` | `rdsanalytics` | supported | memory:RDS batches, any format; ROI filtering, overcrowding, direction (with STOPPED) and line crossing on the CPU, with DeepStream-shaped frame and object metas (`rds/rds-analytics-meta.h`, libgstrds-analytics) and nvdsanalytics config files; matches observed DeepStream 8.0 behaviour where the docs are silent, except where 8.0 is clearly broken; properties below, behaviour, drawing and config keys on [its page](elements/rdsanalytics.md). The `osd-mode` overlays are display meta, drawn by `rdsosd` | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsanalytics) |
| `nvdslogger` | `rdslogger` | supported | passthrough, any caps; prints each stream's frame rate to stdout in DeepStream 8.0's `**PERF` line format; matches observed DeepStream 8.0 behaviour where the docs are silent; properties below, behaviour on [its page](elements/rdslogger.md) | [ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdslogger) |
| `nveglglessink` | `rdsvideosink` | supported | display bin: `rdsvideoconvert` to system-memory BGRx, then a stock GStreamer sink (`video-sink`, default `autovideosink`); one frame per buffer; properties below, behaviour on [its page](elements/rdsvideosink.md) | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |
| `nv3dsink` | `rdsvideosink` | supported | as `nveglglessink` (nv3dsink's properties are a subset of nveglglessink's) | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html) |
| `nvv4l2h264enc` | `rdsh264enc` | supported | a bin over the stock VA encoder `vah264enc` (VCN): `rdsvideoconvert` to system-memory NV12, a queue, then VA on a thread of its own; `memory:RDS` or system-memory input, one frame per buffer; DeepStream 8.0's properties; properties and stream behaviour below | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html) |
| `nvv4l2h265enc` | `rdsh265enc` | supported | the same bin over `vah265enc`, which also fixes the SPS conformance window of widths that are not a multiple of 64; properties and stream behaviour below | [ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html) |

### `rdsurisrcbin` (`nvurisrcbin`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvurisrcbin))

Reference: [rdsurisrcbin](elements/rdsurisrcbin.md).

Names, types, ranges and defaults follow DeepStream 8.0's nvurisrcbin as it ships; the page lists where NVIDIA's
pages differ.

| Property | Status | Difference |
|---|---|---|
| `uri` | supported | |
| `type` | supported | `rtsp` with a non-RTSP URI is an error at start |
| `file-loop` | supported | DTS are offset too; an application EOS ends the loop |
| `drop-frame-interval` | supported | |
| `latency` | supported | |
| `drop-on-latency` | supported | |
| `select-rtp-protocol` | supported | default `rtp-udp-udpmcast-tcp` as DeepStream 8.0 ships it; the docs say 0 |
| `udp-buffer-size` | supported | |
| `rtsp-reconnect-interval` | supported | errors and the server's EOS start an attempt at once; the stream goes on downstream |
| `rtsp-reconnect-attempts` | supported | any negative value is unlimited; running out ends the source with EOS |
| `init-rtsp-reconnect-interval` | supported | works (DeepStream 8.0 does nothing with it) |
| `source-id` | supported | |
| `gpu-id` | accepted no-op | decoding runs on GPU 0; no warning |
| `disable-audio` | supported | audio is never exposed, whatever the value |
| `message-forward` | supported | GstBin's |
| `async-handling` | supported | GstBin's; starts `true`, as in nvurisrcbin |
| `smart-record` | unsupported | anything but 0 is an error at start |
| `smart-rec-status` | supported | always false |
| `smart-rec-dir-path` | accepted no-op | |
| `smart-rec-file-prefix` | accepted no-op | |
| `smart-rec-video-cache` | accepted no-op | |
| `smart-rec-cache` | accepted no-op | |
| `smart-rec-container` | accepted no-op | |
| `smart-rec-mode` | accepted no-op | |
| `smart-rec-default-duration` | accepted no-op | |
| `num-extra-surfaces` | accepted no-op | |
| `cudadec-memtype` | accepted no-op | |
| `dec-skip-frames` | accepted no-op | |
| `low-latency-mode` | accepted no-op | |
| `ipc-buffer-timestamp-copy` | accepted no-op | |
| `ipc-frame-copy` | accepted no-op | from the 9.2 page; DeepStream 8.0 lacks it |
| `ipc-connection-attempts` | accepted no-op | |
| `ipc-connection-interval` | accepted no-op | |
| `ipc-socket-path` | accepted no-op | |
| `extract-sei-type5-data` | accepted no-op | |
| `sei-uuid` | accepted no-op | |
| `simulate-fps-interval-ms` | accepted no-op | from the 9.2 page; DeepStream 8.0 lacks it |
| `buffer-mode` | accepted no-op | |
| `leaky` | accepted no-op | |
| `max-size-buffers` | accepted no-op | |
| `disable-passthrough` | accepted no-op | |

### `rdsvideoconvert` (`nvvideoconvert`) properties

Reference: [rdsvideoconvert](elements/rdsvideoconvert.md).

Names and integers as on NVIDIA's [nvvideoconvert page](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideoconvert.html)
(checked 2026-10-24). That page names no enum nicks except for `nvbuf-memory-type`; the other nicks are Redstream's.

| Property | Status | Difference |
|---|---|---|
| `src-crop` | supported | |
| `dest-crop` | supported | alpha is 0 outside the rect on RGBA/BGRA |
| `allow-odd-crop` | supported | |
| `interpolation-method` | supported | `algo-1` to `algo-3` run bilinear, `algo-4` nearest |
| `compute-hw` | supported | `2` (VIC) fails |
| `flip-method` | unsupported | any value but `0` fails |
| `contiguous-buffers` | accepted no-op | |
| `copy-hw` | accepted no-op | |
| `bl-output` | accepted no-op | |
| `gpu-id` | supported | |
| `nvbuf-memory-type` | supported | |
| `output-buffers` | supported | |
| `disable-passthrough` | supported | |
| `silent` | accepted no-op | |

### `rdsvideodec` (`nvv4l2decoder`) properties

Reference: [rdsvideodec](elements/rdsvideodec.md).

Names, ranges, enum integers and enum nicks as on NVIDIA's
[nvv4l2decoder page](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html)
(checked 2026-10-24). Jetson-only properties, `capture-io-mode`/`output-io-mode` and the SEI properties are not
exposed.

| Property | Status | Difference |
|---|---|---|
| `gpu-id` | supported | |
| `cudadec-memtype` | different default | `0` (device); DeepStream's is `2` (unified) |
| `num-extra-surfaces` | supported | accepts 0 (NVIDIA's page: 1-24) |
| `drop-frame-interval` | supported | accepts 0 (NVIDIA's page: 1-30) |
| `skip-frames` | supported | `1` `decode_non_ref` (Jetson-only) decodes every frame |
| `low-latency-mode` | accepted no-op | |

### `rdsstreammux` (`nvstreammux`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreammux.html))

Reference: [rdsstreammux](elements/rdsstreammux.md).

Names, types and enum integers as on NVIDIA's nvstreammux page (checked 2026-10-24); the page marks the defaults it
doesn't document as ours. Only the default ("legacy") mux is implemented.

| Property | Status | Difference |
|---|---|---|
| `batch-size` | supported | range from 1 (NVIDIA: from 0) |
| `batched-push-timeout` | supported | counts from a batch's first frame |
| `width` | supported | required |
| `height` | supported | required |
| `enable-padding` | supported | frame placed top-left |
| `gpu-id` | supported | |
| `nvbuf-memory-type` | supported | `4` (Jetson surface array) is not a value |
| `buffer-pool-size` | supported | |
| `compute-hw` | supported | `2` (VIC) fails at start |
| `interpolation-method` | supported | `algo-1` to `algo-3` run bilinear, `algo-4` nearest |
| `num-surfaces-per-frame` | supported | only `1` |
| `live-source` | supported | changes only the batch timestamp |
| `attach-sys-ts` | supported | system time; no RTCP sender-report time |
| `sync-inputs` | supported | |
| `max-latency` | supported | |
| `sort-batch` | supported | |
| `drop-pipeline-eos` | supported | |
| `frame-num-reset-on-eos` | supported | |
| `frame-num-reset-on-stream-reset` | supported | |
| `frame-duration` | accepted no-op | |
| `failsafe-flush-count` | accepted no-op | range 1-1024, not tied to `buffer-pool-size` |
| `align-first-buffer` | accepted no-op | |
| `align-inputs` | accepted no-op | |
| `cache-buffer` | accepted no-op | |
| `cache-buffer-timeout` | accepted no-op | |
| `extract-sei-sim-time` | accepted no-op | |
| `extract-sei-type5-data` | accepted no-op | |
| `drop-backward-sei` | accepted no-op | |
| `sync-inputs-ntp` | accepted no-op | |
| `async-process` | accepted no-op | |
| `latency` | ours | GstAggregator's; set from `batched-push-timeout` at every start |
| `min-upstream-latency` | ours | GstAggregator's |
| `start-time` | ours | GstAggregator's |
| `start-time-selection` | ours | GstAggregator's |
| `emit-signals` | ours | GstAggregator's |

### `rdsstreamdemux` (`nvstreamdemux`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreamdemux.html))

Reference: [rdsstreamdemux](elements/rdsstreamdemux.md).

| Property | Status | Difference |
|---|---|---|
| `per-stream-eos` | supported | |

### `rdsmultistreamtiler` (`nvmultistreamtiler`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvmultistreamtiler))

Reference: [rdsmultistreamtiler](elements/rdsmultistreamtiler.md).

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0 (the 8.0 page omits
`buffer-pool-size` and `interpolation-method`). Enum nicks are Redstream's.

| Property | Status | Difference |
|---|---|---|
| `rows` | supported | grown when the sources don't fit |
| `columns` | supported | as `rows` |
| `width` | supported | rounded up to a multiple of 4 |
| `height` | supported | as `width` |
| `show-source` | supported | |
| `square-seq-grid` | supported | |
| `interpolation-method` | supported | `algo-1` to `algo-3` run bilinear, `algo-4` nearest |
| `buffer-pool-size` | supported | |
| `nvbuf-memory-type` | supported | |
| `gpu-id` | supported | |
| `compute-hw` | supported | `2` (VIC) fails at start |
| `custom-tile-config` | accepted no-op | reserved in DeepStream too |
| `qos` | supported | GstBaseTransform's |

### `rdsosd` (`nvdsosd`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsosd))

Reference: [rdsosd](elements/rdsosd.md).

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0. Enum nicks as in DeepStream
(`process-mode`'s `MODE_VIC` is ours).

| Property | Status | Difference |
|---|---|---|
| `display-bbox` | supported | |
| `display-text` | supported | |
| `display-clock` | supported | |
| `clock-font` | supported | NULL: `Serif` |
| `clock-font-size` | supported | |
| `x-clock-offset` | supported | |
| `y-clock-offset` | supported | |
| `clock-color` | supported | |
| `process-mode` | supported | `0` CPU and `1` GPU draw the same pixels; `2` (ours) draws on the GPU |
| `gpu-id` | supported | |
| `display-mask` | accepted no-op | masks are not drawn yet |
| `blur-bbox` | accepted no-op | not blurred yet |
| `blur-on-gie-class-ids` | accepted no-op | as `blur-bbox` |
| `qos` | supported | GstBaseTransform's |

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

Reference: [rdsvideosink](elements/rdsvideosink.md).

Names, types and defaults as DeepStream 8.0's nveglglessink has them (nv3dsink's are a subset, with `window-x` and
`window-y` defaulting to 0). "Forwarded": set on the inner sink when it has the property; see the page.

| Property | Status | Difference |
|---|---|---|
| `video-sink` | ours | the stock GStreamer sink that shows the frames, by factory name; NULL or READY only |
| `sync` | forwarded | |
| `qos` | forwarded | |
| `max-lateness` | forwarded | |
| `processing-deadline` | forwarded | |
| `async` | forwarded | |
| `window-x` | forwarded | |
| `window-y` | forwarded | |
| `window-width` | forwarded | |
| `window-height` | forwarded | |
| `force-aspect-ratio` | forwarded | |
| `create-window` | forwarded | |
| `gpu-id` | forwarded | also the inner `rdsvideoconvert`'s GPU; NULL or READY only |
| `blocksize` | forwarded | |
| `enable-last-sample` | forwarded | |
| `max-bitrate` | forwarded | |
| `render-delay` | forwarded | |
| `show-preroll-frame` | forwarded | |
| `throttle-time` | forwarded | |
| `ts-offset` | forwarded | |
| `rows` | accepted no-op | a non-default value logs one warning |
| `columns` | accepted no-op | a non-default value logs one warning |
| `profile` | accepted no-op | a non-default value logs one warning |
| `display` | accepted no-op | a non-default value logs one warning |
| `do-transform` | accepted no-op | a non-default value logs one warning |
| `winsys` | accepted no-op | Jetson's; its string type and `x11` default are ours |

### `rdsinfer` (`nvinfer`) properties ([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvinfer.html))

Reference: [rdsinfer](elements/rdsinfer.md); its config file and every config key:
[rdsinfer-config](elements/rdsinfer-config.md).

Names and types as on NVIDIA's nvinfer page (checked 2026-10-24); the defaults are ours (NVIDIA documents none).
Not implemented: `classifier-type`, and the TensorRT-only `strongly-typed` and `dynamic-properties`.

| Property | Status | Difference |
|---|---|---|
| `config-file-path` | supported | required |
| `unique-id` | supported | |
| `process-mode` | supported | |
| `infer-on-gie-id` | supported | DeepStream 8.0's type and default |
| `infer-on-class-ids` | supported | |
| `filter-out-class-ids` | supported | |
| `model-engine-file` | supported | a MIGraphX `.mxr`; a TensorRT `.engine` is ignored |
| `batch-size` | supported | a compiled program runs all `batch-size` images |
| `dynamic-batch` | ours | smaller programs for partial chunks |
| `interval` | supported | |
| `gpu-id` | supported | |
| `output-tensor-meta` | supported | |
| `raw-output-file-write` | accepted no-op | |
| `raw-output-generated-callback` | accepted no-op | |
| `raw-output-generated-userdata` | accepted no-op | |
| `output-instance-mask` | unsupported | `true` fails at start |
| `input-tensor-meta` | unsupported | `true` fails at start |
| `crop-objects-to-roi-boundary` | accepted no-op | |
| `clip-object-outside-roi` | accepted no-op | |
| `qos` | supported | GstBaseTransform's |

### `rdstracker` (`nvtracker`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvtracker))

Reference: [rdstracker](elements/rdstracker.md); its config file and every config key:
[rdstracker-config](elements/rdstracker-config.md).

Names, types and defaults as nvtracker reports them in DeepStream 8.0 (7.1 is the same). The IOU and NvSORT trackers
only; NvDCF and NvDeepSORT configs run as NvSORT.

| Property | Status | Difference |
|---|---|---|
| `ll-lib-file` | supported | only the built-in tracker |
| `ll-config-file` | supported | |
| `tracker-width` | accepted no-op | tracking is in frame pixels; no warning |
| `tracker-height` | accepted no-op | as `tracker-width` |
| `gpu-id` | accepted no-op | the tracker runs on the CPU; no warning |
| `display-tracking-id` | supported | |
| `tracking-id-reset-mode` | supported | the ID counter is never restarted |
| `tracking-surface-type` | accepted no-op | |
| `compute-hw` | accepted no-op | |
| `input-tensor-meta` | accepted no-op | |
| `tensor-meta-gie-id` | accepted no-op | |
| `sub-batches` | accepted no-op | |
| `sub-batch-err-recovery-trial-cnt` | accepted no-op | |
| `user-meta-pool-size` | accepted no-op | |
| `operate-on-class-ids` | supported | from the DeepStream 9.x docs |
| `qos` | supported | GstBaseTransform's |

### `rdsanalytics` (`nvdsanalytics`) properties ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdsanalytics))

Reference: [rdsanalytics](elements/rdsanalytics.md); its config file and every config key:
[rdsanalytics-config](elements/rdsanalytics-config.md).

Names, types, ranges and defaults as `gst-inspect-1.0` shows them for DeepStream 8.0's nvdsanalytics.

| Property | Status | Difference |
|---|---|---|
| `config-file` | supported | a failing file posts an ERROR; a reload while playing keeps the old config when the new one fails |
| `enable` | supported | |
| `unique-id` | supported | |
| `qos` | accepted | GstBaseTransform's; never drops buffers |

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
`rdsstreammux` sends them and `rdsstreamdemux` routes them per source (see their pages,
[rdsstreammux](elements/rdsstreammux.md) and [rdsstreamdemux](elements/rdsstreamdemux.md)).

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
