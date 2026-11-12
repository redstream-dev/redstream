# rdslogger

How to read this page, and how to run the examples: [element reference](README.md).

A passthrough element that counts each stream's frames and prints their frame rates to stdout, one `**PERF` line per
interval, in DeepStream 8.0's format. Put it after `rdsstreammux` (or anywhere after it) to see every source's rate.
DeepStream counterpart: `nvdslogger` ([ref](https://docs.nvidia.com/deepstream/text/DS_plugin_gst-nvdslogger.md)).
Built when: always.

## Example

```sh
# two live sources batched by the mux, one line a second
gst-launch-1.0 rdsstreammux name=m batch-size=2 width=1280 height=720 live-source=1 \
  ! rdslogger fps-measurement-interval-sec=1 ! fakesink \
  videotestsrc is-live=true num-buffers=150 ! video/x-raw,format=NV12,framerate=30/1 ! m.sink_0 \
  videotestsrc is-live=true num-buffers=150 ! video/x-raw,format=NV12,framerate=30/1 ! m.sink_1
```

```text
**PERF : FPS_0 (30.00)	FPS_1 (30.00)	
**PERF : FPS_0 (30.00)	FPS_1 (30.00)	
**PERF : FPS_0 (30.00)	FPS_1 (30.00)	
```

It works on any buffers, so it also runs without a GPU; without batch meta every buffer is a frame of stream 0:

```sh
gst-launch-1.0 videotestsrc is-live=true num-buffers=150 ! video/x-raw,framerate=30/1 \
  ! rdslogger fps-measurement-interval-sec=1 ! fakesink
```

The `handoff` signal gives an application every buffer, as `identity`'s does. It is emitted on the streaming thread;
the buffer is only borrowed for the call:

```c
#include <rds/rds.h>

static void
on_handoff (GstElement * logger, GstBuffer * buf, gpointer user_data)
{
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);

  if (bm != NULL) {
    rds_acquire_meta_lock (bm);
    g_print ("%u frames in this batch\n", bm->num_frames_in_batch);
    rds_release_meta_lock (bm);
  }
}

/* ... */
GstElement *logger = gst_element_factory_make ("rdslogger", NULL);

g_signal_connect (logger, "handoff", G_CALLBACK (on_handoff), NULL);
```

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | ANY |
| `src` | src | always | ANY |

Buffers pass through untouched (in place, passthrough), with their memory and metadata.

## Properties

None of the properties is tied to a state: each writable one can be set at any time (`last-message` is
read-only). Ranges are those of the GParamSpec; a value outside one is refused by GObject with a GLib warning, and
the property keeps its value.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `fps-measurement-interval-sec` | uint | `5` | 0–4294967295 | any state | seconds of running time per printed rate; `0` prints nothing. Setting it restarts the measurement, in any state and even to the same value (see Behaviour) |
| `silent` | boolean | `true` | | any state | `false` also prints two lines per buffer (see Behaviour); the `**PERF` lines are printed either way |
| `signal-handoffs` | boolean | `true` | | any state | emit `handoff` for every buffer |
| `last-message` | string | `NULL` | | read-only | always `NULL` |
| `qos` | boolean | `false` | | any state | GstBaseTransform's |
| `check-imperfect-offset`, `check-imperfect-timestamp`, `drop-allocation`, `dump`, `single-segment`, `sync` | boolean | `false` | | any state | accepted no-op (identity's) |
| `datarate` | int | `0` | 0–2147483647 | any state | accepted no-op |
| `drop-buffer-flags` | flags `GstBufferFlags` | `0` | `GstBufferFlags` nicks | any state | accepted no-op |
| `drop-probability` | float | `0` | 0–1 | any state | accepted no-op |
| `error-after` | int | `-1` | −2147483648–2147483647 | any state | accepted no-op |
| `latency`, `sleep-time` | uint | `0` | 0–4294967295 | any state | accepted no-op |
| `ts-offset` | int64 | `0` | the int64 range | any state | accepted no-op |

The no-op properties are the ones `identity` has, which DeepStream 8.0's `nvdslogger` shows too. Setting one to a value
other than its default logs one GLib warning per element and property ("… is not implemented; the property is
accepted and has no effect").

## Signals

| Signal | Arguments | Returns | Description |
|---|---|---|---|
| `handoff` | `GstElement *logger`, `GstBuffer *buffer` | `void` | every buffer, before it is pushed, when `signal-handoffs` is `true`; on the streaming thread. The buffer is not reffed for the handler (`G_SIGNAL_TYPE_STATIC_SCOPE`): ref it to keep it |

## Metadata

- **Reads:** `RdsBatchMeta` (`NvDsBatchMeta`) with its meta lock held: the `source_id` of each `RdsFrameMeta`
  (`NvDsFrameMeta`) in `frame_meta_list`. Each frame meta counts as one frame of its stream.
- **Writes:** nothing.

## Behaviour

**The `**PERF` line.** On stdout (`g_print`), one line per interval: `**PERF : `, then `FPS_<source_id> (<rate>)` and
a tab for each stream by ascending `source_id`, then a newline: `**PERF : FPS_0 (29.97)\tFPS_1 (30.00)\t`. The rate has
two decimals and always a `.`, whatever the locale. It is the number of frames counted since the previous line divided
by the interval.

**Streams.** A stream is listed from its first frame on. After its `rds-stream-eos` it stays listed, at `0.00`. Streams
are forgotten only when the element goes from READY to PAUSED again. A buffer without batch meta counts as one frame of
stream 0; until a frame has been seen, nothing is printed.

**Timing.** In PLAYING, a periodic id on the pipeline clock fires at every interval boundary of running time, the first
one interval after PLAYING starts. The first boundary only starts the count, so the first line comes two intervals in;
a run shorter than two intervals prints nothing. The lines are printed from the clock's thread.

- Paused, the id is disarmed and the time left to the next boundary is kept, so the boundaries resume in running time.
- Flushes change nothing, also when they reset the running time (a seek, a looped clip). The boundaries are kept in
  clock time, and the frames counted before the flush count in the current interval.
- Setting `fps-measurement-interval-sec` restarts the measurement, in any state and even to the value it has: one
  interval to start the count, then a line every interval. `0` stops it.
- Without a clock, nothing is printed; the measurement starts with the first buffer after a clock is set. If waiting
  on the clock fails, the element logs a debug-log warning (`GST_WARNING`) and prints nothing more until the
  measurement restarts (a new interval, or READY to PAUSED).

**`silent=false`.** Two more lines for every buffer, on the streaming thread, as DeepStream 8.0 prints them (its label
reads `nvdslogger base time`):

```text
<name> BUF_PTS : <pts> element time <running time now> elem-buf=<now − pts running time, ms> ms buf=<address> ⏎
  buffer_pts_to_running_time = <pts running time>rdslogger base time = <base time>buffer dur <duration>
BUFFER PTS is lesser than ELEMENT TIME, IT's LATE
```

The first line is wrapped here at `⏎`; it is one line, with a space where it is wrapped. There is no separator before
`rdslogger` and before `buffer dur`, as in 8.0. The second line says `LATE` when the
running time is past the buffer's, and `BUFFER PTS is greater than ELEMENT TIME, IT's EARLY` otherwise (equal counts as
early). Without a clock or a PTS, the times print as `99:99:99.999999999`, `elem-buf` as `0.00` and the second line is
left out.

## Differences from DeepStream 8.0

- **Rate value:** frames since the previous line divided by the interval. DeepStream 8.0's numbers drift above the true
  rate for a while after a stream joins or leaves.
- **`fps-measurement-interval-sec=0`:** prints nothing. DeepStream 8.0 prints `FPS_0 (0.00)` in a busy loop.
- **Interval changed while playing:** ours restarts the measurement.
- **Flushes:** ours keeps the boundaries in clock time, so a seek or a looped clip keeps the measurement going.
- **No batch meta:** ours counts one frame of stream 0. **No stream seen yet:** ours prints nothing.
- **Rate format:** ours always prints a `.`, whatever the locale.
- **`silent=false`:** only the `LATE` line was seen from DeepStream 8.0, so the `EARLY` wording is inferred; the
  fallback without a clock or PTS is ours.
- **The no-op properties** (identity's) warn once when set to something other than the default; `last-message` is
  always `NULL`, since `silent=false` prints to stdout instead.

## Known issues and limits

- No latency output. DeepStream 8.0 prints none either without `NVDS_ENABLE_LATENCY_MEASUREMENT`.
- The rates are counted per `source_id` of the frame metas, not per pad: two sources with the same `source_id` add up.
- The `**PERF` lines go to stdout only; there is no message or signal carrying the rates.
