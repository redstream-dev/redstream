# rdsurisrcbin

How to read this page, and how to run the examples: [element reference](README.md).

A source bin for file and RTSP URIs: it decodes the first video stream to NV12 frames in `memory:RDS` and exposes them
on the pad `vsrc_0`. RTSP sources can reconnect when they stall or drop. File sources can loop. DeepStream
counterpart: `nvurisrcbin` ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvurisrcbin)).
Built when: always (it needs `gstreamer-rtsp`).

## Example

```sh
# a file (the clips come from tests/media/make-test-media.sh, in .cache/media)
gst-launch-1.0 rdsurisrcbin uri=file://$PWD/.cache/media/ball-720p.h264 ! fakesink sync=false

# a looped file, every second frame, with the frame rate printed
gst-launch-1.0 rdsurisrcbin uri=file://$PWD/.cache/media/smpte-1366x768.h264 file-loop=true drop-frame-interval=2 \
  ! rdslogger fps-measurement-interval-sec=2 ! fakesink sync=false

# RTSP, with the repository's test server (build/tools/rds-rtsp-serve: a 1280x720 H.264 test pattern)
build/tools/rds-rtsp-serve/rds-rtsp-serve --port 8554 &
gst-launch-1.0 rdsurisrcbin uri=rtsp://127.0.0.1:8554/test ! rdslogger fps-measurement-interval-sec=2 ! fakesink

# reconnection: the server's fault relay on 8555 cuts the connection 4 s in; -m prints the bin's messages
build/tools/rds-rtsp-serve/rds-rtsp-serve --port 8554 --proxy-port 8555 --faults 4:cut &
gst-launch-1.0 -m rdsurisrcbin uri=rtsp://127.0.0.1:8555/test select-rtp-protocol=rtp-tcp rtsp-reconnect-interval=2 \
  source-id=0 ! fakesink
```

`rds-rtsp-serve` is built only when the build finds `gstreamer-rtsp-server-1.0` (1.28 or later). The relay forwards
RTSP over TCP only, hence `select-rtp-protocol=rtp-tcp`. In the last run the cut ends the session
(a closed TCP connection is an EOS for `rtspsrc`), and the bin posts `rds-reconnect` (`reason=eos`, `attempt=1`), then
`rds-reconnected` once the new session's frames flow.

`vsrc_0` appears only once the stream is decoded, so an application links it from a `pad-added` handler. Into
`rdsstreammux`, as `nvurisrcbin` is used with `nvstreammux`:

```c
/* user_data: the mux. Source N goes to the mux's sink_N; the bin's source-id is set to N too. vsrc_0 is removed
 * going from PAUSED to READY and added again at the next start, while sink_N stays requested: reuse it then. */
static void
on_pad_added (GstElement * bin, GstPad * vsrc, gpointer user_data)
{
  GstElement *mux = user_data;
  gboolean requested = FALSE;
  gint id = -1;
  gchar *name;
  GstPad *sink;

  g_object_get (bin, "source-id", &id, NULL);
  if (id < 0)
    return;
  name = g_strdup_printf ("sink_%d", id);
  sink = gst_element_get_static_pad (mux, name);
  if (sink == NULL) {
    sink = gst_element_request_pad_simple (mux, name);
    requested = sink != NULL;
  }
  g_free (name);
  if (sink == NULL)
    return;
  if (gst_pad_link (vsrc, sink) != GST_PAD_LINK_OK && requested)
    gst_element_release_request_pad (mux, sink);
  gst_object_unref (sink);
}

/* ... */
g_object_set (bin, "uri", uri, "source-id", 0, NULL);
g_signal_connect (bin, "pad-added", G_CALLBACK (on_pad_added), mux);
```

The handler runs on a streaming thread, with the pad's caps already set. The mux takes pads while PLAYING, so the
same handler serves a source added to a running pipeline (`gst_element_sync_state_with_parent (bin)` after adding it).

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `vsrc_%u` | src | sometimes | `video/x-raw(memory:RDS)` or `video/x-raw`, format NV12, 1–16384 × 1–16384 |

- The pad really is a sometimes pad, and there is only ever `vsrc_0`. A new bin has no pad (DeepStream 8.0
  behaviour). `vsrc_0` is a ghost pad, added when the first video stream is decoded: for a file, with its first caps;
  for RTSP, when `rdsvideodec` has its first caps, so the `pad-added` handler sees them as the pad's current caps.
  Then come one stream-start, caps and segment, and the frames.
- It stays across file loops and reconnections, and goes away from PAUSED to READY (ours: DeepStream 8.0 removes it
  at NULL).
- A decoder that can't output `memory:RDS` (JPEG in a file, say) gets `rdsvideoconvert` behind it, so `vsrc_0` carries
  `memory:RDS` NV12 either way. An RTSP stream in a codec other than H.264 or H.265 gets no pad.
- There is no audio pad template: audio is never decoded or exposed.
- Rank none: `uridecodebin` and `decodebin3` never pick it.

## Properties

Names, types, ranges and defaults follow DeepStream 8.0's nvurisrcbin as it ships (gst-inspect). Where NVIDIA's pages
differ, the row says so. The pages give `select-rtp-protocol`'s default as 0, and give only an example value, with no
default, for `source-id`, `buffer-mode`, `leaky`, `max-size-buffers`, `disable-passthrough`, `rtsp-reconnect-interval`
and `rtsp-reconnect-attempts`. `simulate-fps-interval-ms` and `ipc-frame-copy` come from the 9.2 page; 8.0 lacks them.

Setting an accepted no-op property to a value other than its default logs a debug-log warning each time ("… is not
implemented; the property is accepted and has no effect"). GstBin's `message-forward` and `async-handling` are not
listed; `async-handling` starts at `true` here, as in nvurisrcbin
(GstBin's own default is `false`).

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `uri` | string | `NULL` | a URI | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | the source; NULL and the empty string both count as unset. Checked going to READY; see [Errors](#errors). `rtsp*://` URIs (rtsp, rtsps, …) go through the RTSP path, every other one through `uridecodebin3` |
| `type` | enum `GstRdsUriSrcBinType` | `auto` | `0` `auto`, `1` `uri`, `2` `rtsp` | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | the path follows the URI's scheme whatever the value. `rtsp` with a URI that isn't `rtsp*://` is an error at start (ours: DeepStream 8.0 accepts the URI and plays it) |
| `file-loop` | boolean | `false` | | any state | play a `file://` URI again from the start at its end, as one stream (see [File loop](#file-loop)). On another URI, a debug-log warning at start, and the URI plays once |
| `drop-frame-interval` | uint | `0` | 0–30 | any state | keep decoded frames 0, N, 2N, … and drop the rest, counted across file loops, durations unchanged; 0 and 1 keep every frame (DeepStream 8.0 behaviour). Dropped before any conversion; RTSP too |
| `latency` | uint | `100` | 0–4294967295 | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | milliseconds: `rtspsrc`'s `latency` (RTSP) |
| `drop-on-latency` | boolean | `true` | | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | `rtspsrc`'s `drop-on-latency` (RTSP) |
| `select-rtp-protocol` | enum `GstRdsUriSrcBinRtpProtocol` | `rtp-udp-udpmcast-tcp` | `0` `rtp-unknown` (the docs' `rtp-multi`), `1` `rtp-udp`, `2` `rtp-udp-mcast`, `4` `rtp-tcp`, `7` `rtp-udp-udpmcast-tcp`, `10` `rtp-http`, `20` `rtp-tls` | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | `rtspsrc`'s `protocols` (RTSP): UDP, UDP multicast, TCP, all three, HTTP + TCP, TLS + TCP; 0 leaves `rtspsrc`'s default (UDP, UDP multicast and TCP). DeepStream 8.0 behaviour; the docs' default is 0 |
| `udp-buffer-size` | uint | `524288` | 0–4294967295 | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | bytes: `rtspsrc`'s `udp-buffer-size` (RTSP); a value above 2147483647 is passed as 2147483647 |
| `rtsp-reconnect-interval` | uint | `0` | 0–4294967295 | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | seconds without a buffer from the RTSP session before an attempt; 0: no reconnection. See [Reconnection](#reconnection). DeepStream 8.0 behaviour; the docs' example value is 10 |
| `rtsp-reconnect-attempts` | int | `-1` | −2147483648–2147483647; negative: unlimited | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | consecutive failed attempts before giving up; a buffer resets the count (ours: any negative value is unlimited; the docs give −1 and a range from −1). The docs' example value is 4 |
| `init-rtsp-reconnect-interval` | uint | `0` | 0–4294967295 | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | seconds between retries while the first connection fails (ours: DeepStream 8.0 accepts it and does nothing); 0: retry every `rtsp-reconnect-interval` (DeepStream 8.0 behaviour); with both 0 the first failure is an error |
| `source-id` | int | `-1` | −1–2147483647 | any state | the `source-id` of the bin's messages (see [Messages](#messages)). DeepStream 8.0 behaviour; the docs' example value is 0 |
| `gpu-id` | uint | `0` | 0–4294967295 | any state | accepted no-op, without a warning for a non-default value: the decoder uses GPU 0 |
| `disable-audio` | boolean | `true` | | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | audio is never decoded or exposed, whatever the value. Files: only the first video stream is selected. RTSP: `true` refuses every stream that isn't video before SETUP; `false` sets them up and discards them inside the bin |
| `smart-record` | enum `GstRdsUriSrcBinSmartRecord` | `smart-rec-disable` | `0` `smart-rec-disable`, `1` `smart-rec-cloud`, `2` `smart-rec-multi` | any state (applied at the next start: see [When each value is read](#when-each-value-is-read)) | unsupported: anything but 0 is an error at start, "Smart record is not supported: set smart-record=0" (CORE, NOT_IMPLEMENTED) |
| `smart-rec-status` | boolean | `false` | | read-only | always `false`: smart record never runs |
| `smart-rec-dir-path` | string | `NULL` | | any state | accepted no-op |
| `smart-rec-file-prefix` | string | `"Smart_Record"` | | any state | accepted no-op |
| `smart-rec-video-cache`, `smart-rec-cache` | uint | `0` | 0–4294967295 | any state | accepted no-op |
| `smart-rec-container` | enum `GstRdsUriSrcBinSmartRecContainer` | `smart-rec-mp4` | `0` `smart-rec-mp4`, `1` `smart-rec-mkv` | any state | accepted no-op |
| `smart-rec-mode` | enum `GstRdsUriSrcBinSmartRecMode` | `smart-rec-mode-av` | `0` `smart-rec-mode-av`, `1` `smart-rec-mode-video`, `2` `smart-rec-mode-audio` | any state | accepted no-op |
| `smart-rec-default-duration` | uint | `20` | 0–4294967295 | any state | accepted no-op |
| `num-extra-surfaces` | uint | `1` | 0–4294967295 | any state | accepted no-op (the inner `rdsvideodec` keeps its own default) |
| `cudadec-memtype` | enum `GstRdsUriSrcBinCudaDecMemType` | `memtype_device` | `0` `memtype_device`, `1` `memtype_pinned`, `2` `memtype_unified` | any state | accepted no-op |
| `dec-skip-frames` | enum `GstRdsUriSrcBinSkipFrames` | `decode_all` | `0` `decode_all`, `1` `decode_non_ref`, `2` `decode_key` | any state | accepted no-op |
| `low-latency-mode` | boolean | `false` | | any state | accepted no-op |
| `ipc-buffer-timestamp-copy` | boolean | `false` | | any state | accepted no-op |
| `ipc-frame-copy` | boolean | `false` | | any state | accepted no-op; from the 9.2 page |
| `ipc-connection-attempts` | int | `-1` | −1–2147483647 | any state | accepted no-op |
| `ipc-connection-interval` | uint64 | `1000000` | the uint64 range | any state | accepted no-op; microseconds |
| `ipc-socket-path` | string | `NULL` | | any state | accepted no-op |
| `extract-sei-type5-data` | boolean | `false` | | any state | accepted no-op |
| `sei-uuid` | string | `NULL` | | any state | accepted no-op |
| `simulate-fps-interval-ms` | uint | `0` | 0–1000 | any state | accepted no-op; from the 9.2 page |
| `buffer-mode` | enum `GstRdsUriSrcBinBufferMode` | `auto` | `0` `unknown`, `1` `slave`, `2` `buffer`, `3` `auto`, `4` `synced` | any state | accepted no-op. DeepStream 8.0 behaviour; the docs' example value is 0 |
| `leaky` | enum `GstRdsUriSrcBinLeaky` | `no-leak` | `0` `no-leak`, `1` `upstream-leak`, `2` `downstream-leak` | any state | accepted no-op. DeepStream 8.0 behaviour; the docs' example value is 0 |
| `max-size-buffers` | uint | `200` | 0–4294967295 | any state | accepted no-op. DeepStream 8.0 behaviour; the docs' example value is 0 |
| `disable-passthrough` | boolean | `true` | | any state | accepted no-op. DeepStream 8.0 behaviour; the docs' example value is 0 |

## Signals

The bin defines no signal of its own. Applications use GstElement's `pad-added` to link `vsrc_0`:

| Signal | Arguments | Returns | Description |
|---|---|---|---|
| `pad-added` | `GstElement *bin`, `GstPad *pad` | `void` | GstElement's: `vsrc_0` was added, with its caps set; on a streaming thread |

## Messages

The bin posts these on the bus about reconnection:

| Message | Type | Fields | When |
|---|---|---|---|
| `rds-reconnect` | element | `source-id` (int), `attempt` (int, from 1), `reason` (string: `timeout`, `error`, `eos`) | each attempt (ours) |
| `rds-reconnected` | element | `source-id` (int) | the first frame of a new session after a reconnection (ours) |
| `attempt-exceeded` | element | `source-id` (uint, −1 as 4294967295), `attempt_exceeded` (boolean, `true`) | the attempts ran out (DeepStream 8.0's message) |
| "Reconnecting to the RTSP source, attempt N: …" | warning (STREAM, FAILED) | the GError and debug text | each attempt |
| "Could not reconnect to the RTSP source: giving up after N attempts" | warning (STREAM, FAILED) | the GError and debug text | the attempts ran out, before `attempt-exceeded` |

"Element" messages are `GST_MESSAGE_ELEMENT` with the structure named in the first column; "warning" messages are
`GST_MESSAGE_WARNING`.

## Metadata

None read or written. The frames carry no `RdsBatchMeta` until `rdsstreammux` batches them.

## Behaviour

### When each value is read

`gst-inspect-1.0` shows the properties DeepStream 8.0 marks so as "changeable only in NULL or READY"
(`GST_PARAM_MUTABLE_READY`): of those below `uri`, `type`, `latency`, `select-rtp-protocol`, `udp-buffer-size` and the
three reconnection settings, and also `drop-frame-interval`, `source-id`, `gpu-id` and many of the no-ops. The bin
accepts a change to any property in any state, without a warning; the value is used at the time below.

| Property | Read when |
|---|---|
| `uri`, `type`, `smart-record` | going from NULL to READY, where the path is chosen and the start checks run (for RTSP, `uri` also at every new session: see [Known issues and limits](#known-issues-and-limits)) |
| `latency`, `drop-on-latency`, `select-rtp-protocol`, `udp-buffer-size`, `disable-audio` | every new RTSP session: going from READY to PAUSED and at each reconnection attempt |
| `rtsp-reconnect-interval`, `rtsp-reconnect-attempts`, `init-rtsp-reconnect-interval` | going from READY to PAUSED |

`file-loop`, `drop-frame-interval` and `source-id` are read as they are used, so a change applies at once.

**Two paths.**
- **Files and other URIs** (anything but `rtsp*://`): a `uridecodebin3`, made going from NULL to READY, with only the
  first video stream selected. Its decoded pad becomes `vsrc_0`'s target: directly when the decoder outputs
  `memory:RDS` (`rdsvideodec`, which autoplugging picks for H.264 and H.265), else through `rdsvideoconvert` to
  `memory:RDS` NV12. Any further pad goes to a fakesink.
- **RTSP** (`rtsp://`, `rtsps://`, …): an RTSP session (`rtspsrc` and the RTP depayloader in a bin of their own, made
  going from READY to PAUSED), then `h264parse` or `h265parse` (`config-interval=-1`) and `rdsvideodec`. Only H.264
  and H.265 video streams are set up; another video codec is an error naming its `encoding-name`. `rtspsrc` gets a
  100 ms `teardown-timeout` (ours). The bin is live: PAUSED returns NO_PREROLL.
- Without reconnection (`rtsp-reconnect-interval` 0, the default), an `rtspsrc` error reaches the bus as it is, and
  the end of the stream (the server's EOS, or a closed TCP connection) is an EOS on `vsrc_0`.

<a id="file-loop"></a>**File loop** (DeepStream 8.0 behaviour). At the end of a `file://` URI the file plays again from
the start:
- nothing downstream sees the loop: no EOS, flush, second segment or stream-start. The one segment is open-ended;
- PTS go on from the end of the previous pass, and DTS too (ours: DeepStream 8.0 offsets PTS only);
- each pass's first buffer is DISCONT;
- a flushing seek from downstream goes through as usual;
- an EOS the application sends to the bin or the pipeline gets out as EOS and ends the loop (ours: not observed on
  DeepStream 8.0);
- if seeking back to the start fails, a WARNING message ("Could not seek back to the start of the file for
  file-loop", STREAM, FAILED) and an EOS on `vsrc_0`.

<a id="reconnection"></a>**Reconnection.** An attempt replaces the RTSP session; the parser and `rdsvideodec` stay.
Attempts run on GStreamer's thread pool, one at a time; the watchdog ticks once a second on the system clock while
PLAYING.

| Behaviour | DeepStream 8.0 | `rdsurisrcbin` |
|---|---|---|
| no buffer for `rtsp-reconnect-interval` s | an attempt (watchdog) | the same; reason `timeout` |
| an ERROR, or the server's EOS (or a closed TCP connection) | waits for the watchdog | an attempt at once (ours); reason `error` or `eos`; the EOS never leaves the bin. The EOS the application sends goes through |
| the session's ERRORs while reconnection is on | posted as WARNINGs | the same; the bin's own about a session it can't use (no video stream, a codec it can't decode) too, each a failed attempt (ours) |
| the application's EOS | not observed while the source is down | ends the stream in any state (ours): no attempt after it, EOS on `vsrc_0` through the decoder. With no `vsrc_0` yet the attempts just stop (see [Known issues and limits](#known-issues-and-limits)) |
| each attempt | a WARNING for the first two attempts only | a WARNING (STREAM, FAILED) on every attempt, and the element message `rds-reconnect` (both ours) |
| a failed attempt (an error, or no buffer within the interval) | the next attempt | the next attempt an interval after it started; nothing goes downstream |
| attempts run out (`rtsp-reconnect-attempts` N ≥ 0 failed) | the element message `attempt-exceeded`; nothing downstream, so a muxer waits for the source forever | a WARNING (ours: "Could not reconnect to the RTSP source: giving up after N attempts"), then the same message, then EOS on `vsrc_0` (ours: `rdsstreammux` ends the source and batches the others). Before any connection, the message and then an ERROR (RESOURCE, OPEN_READ; ours). No attempt after it |
| downstream after a reconnection | a FLUSH_START/STOP pair (reset-time TRUE), a new STREAM_START and a sticky reset event per attempt, then a new SEGMENT; without a muxer the running time restarts | the same stream (ours): no EOS or STREAM_START, the running time and PTS go on. The new session's frames start at a key frame (delta units before it are dropped, and the leading pictures of an open GOP after it; it is DISCONT), preceded by one FLUSH_START / FLUSH_STOP pair (reset-time FALSE) and the SEGMENT again, so `rdsstreammux` sends one `rds-stream-reset` for the source; then the element message `rds-reconnected` |
| the first connection fails | retries every `rtsp-reconnect-interval` s | retries every `init-rtsp-reconnect-interval` s if above 0 (ours), else every `rtsp-reconnect-interval` s; the attempts limit applies (running out is an ERROR); `vsrc_0` comes with the first frames, with no flush; with both 0, an ERROR |
| stopping during an attempt | about 1 ms | waits for the attempt under way; a hanging session stops within `rtspsrc`'s 100 ms `teardown-timeout` |
| a new session at another size | new CAPS on `vsrc_0`, no error, the stream goes on; without a muxer the running time restarts | a normal reconnection, with new CAPS on `vsrc_0` (`rdsvideodec` reconfigures or recreates its decoder); `rdsstreammux` takes them as any caps change and scales the frames into its fixed-size slots |
| a new session in another codec (H.264 to H.265, say) | an ERROR for every attempt: it never recovers | a normal reconnection (ours): a new parser in front of the same `rdsvideodec`, which switches its own parser to the new caps; `vsrc_0` stays, with new CAPS if the size changes |

`rds-stream-reset` is `rdsstreammux`'s downstream event for one source (`rds/rds-event.h`).

An ERROR or WARNING from a session being stopped, or from the current one while the bin stops, is dropped: it is
about the teardown. A picture of the old session that the decoder still held goes out before the reconnection's
flush (`rdsvideodec` holds the last picture until the next one arrives).

<a id="errors"></a>**Errors** going from NULL to READY, as element errors. The messages name the URI's scheme, never the
whole URI, which may hold credentials:
- no URI: "No URI: set the uri property" (RESOURCE, NOT_FOUND);
- not a valid URI: "The uri property is not a valid URI: it needs a scheme, …" (RESOURCE, NOT_FOUND);
- a scheme no source element handles: "No source element handles the scheme '…' of the uri property" (CORE,
  MISSING_PLUGIN);
- `smart-record` other than 0 (CORE, NOT_IMPLEMENTED);
- `type=rtsp` with another scheme: "type=rtsp needs an rtsp:// URI, not …://: set type=auto" (RESOURCE, SETTINGS);
- no `uridecodebin3` (made here for a non-RTSP URI): "uridecodebin3 is missing" (CORE, MISSING_PLUGIN).

Missing elements made later are element errors too (CORE, MISSING_PLUGIN, naming the element):
- `rtspsrc`, made for every RTSP session: going from READY to PAUSED and at each reconnection attempt;
- the depayloader ("rtph264depay is missing" or `rtph265depay`), the parser (`h264parse`, `h265parse`) and
  `rdsvideodec`, made at the first RTSP video pad;
- `rdsvideoconvert`, made at the first decoded pad of a file whose decoder can't output `memory:RDS`.

While playing: an RTSP source with no video stream, or in a codec other than H.264 or H.265, is an ERROR (STREAM,
CODEC_NOT_FOUND), or with reconnection on, a WARNING and a failed attempt.

## Differences from DeepStream 8.0

- Reconnection is one continuous stream downstream: no flush with reset-time, no new stream-start, the running time
  goes on (DeepStream 8.0 restarts it). Errors and the server's EOS start an attempt at once; every attempt posts a
  WARNING and `rds-reconnect`; running out of attempts ends the source with an EOS; a codec change recovers.
- `init-rtsp-reconnect-interval` works (8.0 accepts it and does nothing).
- `type=rtsp` with a non-RTSP URI is an error.
- `vsrc_0` goes away at PAUSED to READY (8.0: at NULL).
- `file-loop` offsets DTS too, and an application EOS ends the loop.
- Smart record, IPC, SEI extraction and the internal queue's settings are accepted and do nothing; `smart-record`
  other than 0 is an error.

## Known issues and limits

- One video stream per source; audio is never exposed.
- RTSP is tested against gst-rtsp-server, with a fault relay in front, not yet against real cameras.
- RTSP: H.264 and H.265 only. Files: whatever `uridecodebin3` decodes, through `rdsvideoconvert` when the decoder isn't
  `rdsvideodec`.
- `gpu-id` is not passed on: decoding and conversion run on GPU 0.
- For RTSP, `uri` is read without the start checks for every new session. A `uri` changed while PLAYING is used by the
  next reconnection attempt, and one changed in READY, after the start chose the RTSP path, is used unvalidated.
  Change `uri` in NULL only.
- An EOS the application sends before `vsrc_0` exists stops the attempts, but no EOS can reach the pipeline's sinks:
  nothing is linked yet. Wait for the EOS message with a timeout, or treat a source with no `vsrc_0` as already
  stopped.
