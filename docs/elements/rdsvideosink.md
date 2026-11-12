# rdsvideosink

A display sink for single frames: it converts them on the GPU, copies them to system memory and shows them with a
stock GStreamer video sink (`autovideosink` by default), under the DeepStream sinks' property names. DeepStream
counterparts: `nveglglessink` and `nv3dsink`
([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/sdk-api/sources_2apps_2apps-common_2includes_2deepstream__config_8h.html)).
Built when: always.

## Example

```sh
# a window (needs a display; from a container, the X11 flags of docker run: DISPLAY, the X socket, XAUTHORITY)
gst-launch-1.0 videotestsrc num-buffers=300 ! video/x-raw,width=1280,height=720 ! rdsvideoconvert \
  ! "video/x-raw(memory:RDS),format=RGBA" ! rdsvideosink

# no display: the same pipeline into fakesink
gst-launch-1.0 videotestsrc num-buffers=100 ! video/x-raw,width=1280,height=720 ! rdsvideoconvert \
  ! "video/x-raw(memory:RDS),format=RGBA" ! rdsvideosink video-sink=fakesink sync=false
```

Both need a GPU (`rdsvideoconvert`). Raw `.h264` from `filesrc` has no timestamps: use `sync=false` with it, or the
sink drops most frames as late. Batches from `rdsstreammux` go through `rdsmultistreamtiler` or `rdsstreamdemux`
first; a batch of one frame (`batch-size=1`) is shown as it is.

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)` or `video/x-raw`, format NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR or GRAY8, 1–16384 × 1–16384: what `rdsvideoconvert` takes |

The pad is a ghost pad of the inner `rdsvideoconvert`'s sink pad. Inside the bin:

```text
rdsvideoconvert ! video/x-raw,format=BGRx ! videoconvert ! <video-sink>
```

## Properties

**Forwarded** properties are kept by `rdsvideosink` and set on the inner sink, and on the sinks inside it when it is
a bin (as `autovideosink` is): whenever the inner sink has a writable, not construct-only property of that name and
the value converts to its type and fits its range. They are applied when the inner sink is created, when
`autovideosink` adds the sink it picked (at READY), and on every change. A property the inner sink lacks, or a value
it can't take, is stored and does nothing, silently. `gpu-id` is also set on the inner `rdsvideoconvert`.

Setting an accepted no-op property to a value other than its default logs one GLib warning per element and property
("… is not implemented; the property is accepted and has no effect").

`video-sink` and `gpu-id` can be changed only in NULL or READY. In PAUSED or PLAYING (or on the way there) a new value
is refused with a GLib warning ("… can only be changed in the NULL or READY state; keeping the current value"), and
the property keeps its value. The other properties can be set in any state. A value outside a property's range is
refused by GObject with a GLib warning. GstBin's `async-handling` and `message-forward` are not listed.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `video-sink` | string | `"autovideosink"` | a sink factory name | NULL, READY | the stock sink that shows the frames (ours); see "Changing `video-sink`" |
| `sync` | boolean | `true` | | any state | forwarded: sync on the clock |
| `qos` | boolean | `true` | | any state | forwarded: QoS events upstream |
| `max-lateness` | int64 | `5000000` | −1–9223372036854775807 (−1: unlimited) | any state | forwarded: nanoseconds a buffer may be late before it is dropped |
| `processing-deadline` | uint64 | `15000000` | the uint64 range | any state | forwarded: nanoseconds |
| `async` | boolean | `true` | | any state | forwarded: go to PAUSED asynchronously |
| `window-x`, `window-y` | uint | `10` | 0–2147483647 | any state | forwarded: the window position (nveglglessink's defaults; nv3dsink's are 0). None of the stock sinks in the dev image has them |
| `window-width`, `window-height` | uint | `0` | 0–2147483647 | any state | forwarded: the window size, 0 for the video's. Read-only on `ximagesink`, so not set there |
| `force-aspect-ratio` | boolean | `true` | | any state | forwarded: keep the frame's aspect ratio |
| `create-window` | boolean | `true` | | any state | forwarded |
| `gpu-id` | uint | `0` | 0–4294967295 | NULL, READY | forwarded, and set on the inner `rdsvideoconvert`: the GPU that converts and copies the frames |
| `blocksize` | uint | `4096` | the uint range | any state | forwarded (GstBaseSink's) |
| `enable-last-sample` | boolean | `true` | | any state | forwarded (GstBaseSink's) |
| `max-bitrate` | uint64 | `0` | the uint64 range | any state | forwarded (GstBaseSink's); 0: no limit |
| `render-delay` | uint64 | `0` | the uint64 range | any state | forwarded (GstBaseSink's): nanoseconds |
| `show-preroll-frame` | boolean | `true` | | any state | forwarded (GstBaseSink's) |
| `throttle-time` | uint64 | `0` | the uint64 range | any state | forwarded (GstBaseSink's): nanoseconds; 0: off |
| `ts-offset` | int64 | `0` | the int64 range | any state | forwarded (GstBaseSink's): nanoseconds |
| `rows`, `columns` | uint | `1` | 1–2147483647 | any state | accepted no-op |
| `profile` | uint | `0` | 0–4294967295 | any state | accepted no-op |
| `display` | `gpointer` | — | | any state | accepted no-op |
| `do-transform` | boolean | `true` | | any state | accepted no-op |
| `winsys` | string | `"x11"` | any string | any state | accepted no-op; Jetson's nveglglessink has it, with `x11` or `wayland` (not 8.0's dGPU one), so its string type and default are ours |

## Metadata

- **Reads:** for each buffer, the frame count: the surface's `numFilled`, or the number of `RdsFrameMeta`
  (`NvDsFrameMeta`) in its `RdsBatchMeta` (`NvDsBatchMeta`, under its meta lock) if that is larger.
- **Writes:** nothing. The sink shows pixels only: draw the metadata with `rdsosd` first.

## Behaviour

**Display path.** `rdsvideoconvert` converts the frame to BGRx on the GPU and copies it to system memory;
`videoconvert` passes it through (or converts, when the inner sink wants another format); the inner sink shows it.

**Changing `video-sink`** (in NULL or READY):
- The current inner sink is set to NULL and removed. A new element of the named factory is made, linked after
  `videoconvert`, given every forwarded value and brought to the bin's state.
- NULL or an empty string means `autovideosink`. Only a factory name is accepted, not an element instance.
- If there is no such factory, or the new sink does not link or change state, the previous sink is put back, the
  property keeps its old value, and a GLib warning says so ("no element '…', or it does not link or start; keeping
  the video sink '…'").

**Batches.** A buffer with more than one frame (a surface `numFilled` above 1, or more than one frame meta) is an
element error: "rdsvideosink takes one frame per buffer: add a tiler or a demuxer" (STREAM, WRONG_TYPE). A buffer list
is checked buffer by buffer.

**Errors.**
- Going from NULL to READY without one of its elements (`rdsvideoconvert`, `capsfilter`, `videoconvert`, or a sink
  for `video-sink`) is an element error, "rdsvideosink is missing an element" (CORE, MISSING_PLUGIN).
- No display: the inner sink's own error reaches the bus (`ximagesink`: "Could not initialise X output").
- A state change up that fails without any error from inside the bin gets one from `rdsvideosink`: "The video sink
  '…' could not start: check the display (DISPLAY), or choose another sink with the video-sink property" when the
  inner sink is the child that failed, or no child can be named; else "An element inside
  rdsvideosink failed <transition>" (CORE, STATE_CHANGE).

## Differences from DeepStream 8.0

- **Rendering:** frames are copied to system memory and shown by a stock GStreamer sink; there is no GL interop yet.
  `video-sink` (ours) picks that sink.
- **Batched input** is an element error. DeepStream 8.0 accepts the caps, then fails the first buffer without a
  message, and the pipeline hangs.
- **No display** gives a bus error. DeepStream 8.0's nveglglessink fails the state change without a bus message, and
  nv3dsink posts "state change failed and some element failed to post a proper error message".
- **Properties the inner sink lacks** are stored silently (with `autovideosink`, the real sink is known only at
  READY). nveglglessink's `rows`, `columns`, `profile`, `display` and `do-transform` are accepted no-ops (what 8.0 does
  with them was not determined), and `winsys` is Jetson's.

## Known issues and limits

- Every frame is copied from the GPU to system memory: no zero-copy display yet.
- `autovideosink` without a display may pick `kmssink`, which can start without showing anything (seen in the
  `redstream:dev` container with no X11 flags). Set `video-sink=ximagesink` to get a clear error instead, or pass the
  X11 flags (`DISPLAY`, the X socket, `XAUTHORITY`).
- `ximagesink` in the container warns that MIT-SHM is unavailable (no shared memory with the host X server) and falls
  back to plain X11 puts: harmless.
- The stock sinks in the dev image (`ximagesink`, `kmssink`, `autovideosink`, `fpsdisplaysink`) have no `window-x`
  or `window-y`, and `ximagesink`'s `window-width` and `window-height` are read-only, so those values do nothing
  there.
