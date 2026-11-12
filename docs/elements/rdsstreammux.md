# rdsstreammux

How to read this page, and how to run the examples: [element reference](README.md).

Batches frames from several sources into one `memory:RDS` buffer with an `RdsBatchMeta`: every frame is scaled (and
colour-converted) into its own fixed-size slot of a pooled batch surface. This is DeepStream's default ("legacy")
mux; the new mux is not implemented. DeepStream counterpart: `nvstreammux`
([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreammux.html)).
Built when: always.

## Example

```sh
# two files into batches of two 1280x720 slots (the clips come from tests/media/make-test-media.sh)
gst-launch-1.0 rdsstreammux name=m batch-size=2 width=1280 height=720 ! fakesink sync=false \
  filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec ! m.sink_0 \
  filesrc location=.cache/media/smpte-1366x768.h264 ! h264parse ! rdsvideodec ! m.sink_1

# two live sources with a 40 ms timeout, aspect ratio kept, every second printed by rdslogger
gst-launch-1.0 rdsstreammux name=m batch-size=2 width=640 height=640 enable-padding=true live-source=true \
  batched-push-timeout=40000 ! rdslogger fps-measurement-interval-sec=1 ! fakesink \
  videotestsrc is-live=true num-buffers=150 ! video/x-raw,format=NV12,framerate=30/1 ! m.sink_0 \
  videotestsrc is-live=true num-buffers=75 ! video/x-raw,format=NV12,framerate=15/1 ! m.sink_1
```

In the second run `rdslogger` prints `FPS_0 (30.00)` and `FPS_1 (15.00)`: the timeout closes a batch 40 ms after
its first frame arrived, so the 30 fps source isn't held back to the 15 fps one. Without the timeout both come out at
15 fps, since a batch then waits for a frame from every source. A source added to a running pipeline (from C):

```c
/* Adds a test source as source `id`, while PLAYING or not; FALSE if sink_<id> is taken or a step fails. The id is
 * explicit: a request for sink_%u gets GstAggregator's next number (the highest so far plus one), not the lowest
 * free one, and the frame metas' source_id is that number. */
static gboolean
add_source (GstElement * pipeline, GstElement * mux, guint id)
{
  gchar *name = g_strdup_printf ("sink_%u", id);
  GstPad *sink = gst_element_get_static_pad (mux, name), *src;
  GstElement *test, *filter;
  GstCaps *caps;
  gboolean ok;

  if (sink != NULL) {           /* never request an existing sink_N: see "Known issues and limits" */
    gst_object_unref (sink);
    g_free (name);
    return FALSE;
  }
  test = gst_element_factory_make ("videotestsrc", NULL);
  filter = gst_element_factory_make ("capsfilter", NULL);
  if (test == NULL || filter == NULL) {
    if (test != NULL)
      gst_object_unref (gst_object_ref_sink (test));
    if (filter != NULL)
      gst_object_unref (gst_object_ref_sink (filter));
    g_free (name);
    return FALSE;
  }
  caps = gst_caps_from_string ("video/x-raw,format=NV12,framerate=30/1");
  g_object_set (test, "is-live", TRUE, NULL);
  g_object_set (filter, "caps", caps, NULL);
  gst_caps_unref (caps);
  gst_bin_add_many (GST_BIN (pipeline), test, filter, NULL);
  sink = gst_element_request_pad_simple (mux, name);
  g_free (name);
  src = gst_element_get_static_pad (filter, "src");
  ok = sink != NULL && gst_element_link (test, filter) && gst_pad_link (src, sink) == GST_PAD_LINK_OK
      /* downstream first: the mux pad is linked, then the source starts */
      && gst_element_sync_state_with_parent (filter) && gst_element_sync_state_with_parent (test);
  gst_object_unref (src);
  if (!ok) {
    gst_element_set_state (test, GST_STATE_NULL);
    gst_element_set_state (filter, GST_STATE_NULL);
    if (sink != NULL)
      gst_element_release_request_pad (mux, sink);
    gst_bin_remove_many (GST_BIN (pipeline), test, filter, NULL);
  }
  if (sink != NULL)
    gst_object_unref (sink);
  return ok;
}
```

The mux's `sink_N` pads stay across a restart (PLAYING to READY and back), so this needs no `pad-added` handling. For a
source bin whose pad comes and goes, as `rdsurisrcbin`'s does, link from its `pad-added` and reuse an existing `sink_N`
([rdsurisrcbin](rdsurisrcbin.md) has the snippet). To remove a source, see [Adding and removing
sources](#adding-and-removing-sources).

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink_%u` | sink | request | `video/x-raw(memory:RDS)` or `video/x-raw`, format NV12, I420, RGBA, BGRA, RGBx, BGRx, RGB, BGR or GRAY8, 1–16384 × 1–16384, one frame per buffer |
| `src` | src | always | `video/x-raw(memory:RDS)`: batches of `width`×`height` slots, with an `RdsBatchMeta` |

- **`sink_N` is source N:** the frame meta's `source_id` and `pad_index` are N. A request for `sink_%u` (or no name)
  gets one more than the highest N used so far (GstAggregator's counter), not the lowest free one. The pads are
  `GstAggregatorPad`s; their own type adds no properties.
- **Inputs:** `memory:RDS` frames are read in place; system-memory frames are uploaded through a staging surface per
  pad. A `GstVideoCropMeta` on an input is honoured: only that rect is scaled. A batch as input is an element error,
  "Batched input is not supported" (STREAM, NOT_IMPLEMENTED).
- **Output:** `width`×`height`, the format downstream picks (NV12 when it doesn't care), PAR 1/1, and the inputs'
  frame rate when they all agree, else `0/1`. Without `width` and `height` the output can't negotiate: "rdsstreammux
  needs the width and height properties (the output batch size)" (CORE, NEGOTIATION).

## Properties

Names, types and enum integers are those on NVIDIA's nvstreammux page (checked 2026-10-24). That page lists example
values, not defaults, for most properties: a default it doesn't document is ours (marked "ours" below), so it may
differ from DeepStream's. Setting an accepted no-op property logs one debug-log INFO message per element, the first
time any of them is set ("property '…' is accepted for compatibility but has no effect").

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `batch-size` | uint | `1` | 1–4294967295 | NULL, READY | frames per batch (ours: NVIDIA's range starts at 0) |
| `batched-push-timeout` | int | `-1` | −1–2147483647 | NULL, READY | microseconds before a partial batch goes out; `-1`: no timeout (ours). See [Batching and timeout](#batching-and-timeout) |
| `width`, `height` | uint | `0` | 0–16384 | NULL, READY | the slot size, **required**: `0` fails negotiation (NVIDIA's range goes to 4294967295) |
| `enable-padding` | boolean | `false` | | any state | keep the aspect ratio; the frame goes at the **top-left** of its slot (see [Scaling and padding](#scaling-and-padding)). Default ours |
| `gpu-id` | uint | `0` | 0–4294967295: a visible ROCm device | NULL, READY | the GPU that scales and holds the batches; one that doesn't exist is an element error at the first batch, "GPU N is not available" (RESOURCE, NOT_FOUND). Default ours |
| `nvbuf-memory-type` | enum `GstRdsStreamMuxMemoryType` | `nvbuf-mem-default` | `0` `nvbuf-mem-default` (HIP device memory), `1` `nvbuf-mem-cuda-pinned`, `2` `nvbuf-mem-cuda-device`, `3` `nvbuf-mem-cuda-unified` | NULL, READY | the memory of the output batches. `4` (Jetson's surface array) is not a value, so GObject refuses it. Default ours |
| `buffer-pool-size` | uint | `4` | 1–1024 | NULL, READY | batches in the output pool, allocated up front (`buffer-pool-size` × `batch-size` slots); see [Buffers and branches](#buffers-and-branches). Default ours |
| `compute-hw` | enum `GstRdsStreamMuxComputeHW` | `default` | `0` `default`, `1` `gpu`, `2` `vic` | NULL, READY | `0` and `1` run on the GPU; `2` (Jetson's VIC) is an element error at start, "compute-hw=VIC is not available" (RESOURCE, SETTINGS). Default ours |
| `interpolation-method` | enum `GstRdsStreamMuxInterpolationMethod` | `default` | `0` `nearest`, `1` `bilinear`, `2` `algo-1`, `3` `algo-2`, `4` `algo-3`, `5` `algo-4`, `6` `default` | any state | the scaling filter (integers as `NvBufSurfTransform_Inter`): `default` and `algo-4` sample the nearest pixel, `algo-1` to `algo-3` run bilinear. Default ours |
| `num-surfaces-per-frame` | uint | `1` | 1–4 | NULL, READY | only `1`; 2–4 are an element error at start (RESOURCE, SETTINGS). Default ours |
| `live-source` | boolean | `false` | | any state | changes only the batch timestamp (see [Batch timestamps](#batch-timestamps)), not the waiting. Default ours |
| `attach-sys-ts` | boolean | `true` | | any state | each frame's `ntp_timestamp` is the system (wall-clock) time in ns; `false` gives 0. RTCP sender-report time is not implemented. Default ours |
| `sync-inputs` | boolean | `false` | | NULL, READY | push a partial batch once the clock reaches its earliest frame's running time plus the latency; see [Batching and timeout](#batching-and-timeout). Default ours |
| `max-latency` | uint | `0` | 0–4294967295 | NULL, READY | nanoseconds: reported downstream in the latency query, and waited for (on top of the timeout) before a partial batch goes out in live mode. Default ours |
| `sort-batch` | boolean | `false` | | any state | slots in sink pad order; otherwise in the order the frames were collected. Default ours |
| `drop-pipeline-eos` | boolean | `false` | | any state | no EOS downstream when every source has ended; the per-source `rds-stream-eos` events still go |
| `frame-num-reset-on-eos` | boolean | `false` | | any state | a source's `frame_num` restarts at 0 after its EOS. Default ours |
| `frame-num-reset-on-stream-reset` | boolean | `false` | | any state | a source's `frame_num` restarts at 0 after a flush of that source. Default ours |
| `frame-duration` | uint64 | `18446744073709551615` | the uint64 range | any state | accepted no-op (NTP correction is not implemented); the default is −1, none |
| `failsafe-flush-count` | uint | `10` | 1–1024 | any state | accepted no-op. Range ours: not tied to `buffer-pool-size` (NVIDIA: 1 to `buffer-pool-size`, capped at one less) |
| `align-first-buffer`, `align-inputs`, `cache-buffer`, `extract-sei-sim-time`, `extract-sei-type5-data`, `drop-backward-sei`, `async-process` | boolean | `false` | | any state | accepted no-op. Default ours |
| `cache-buffer-timeout` | int | `-1` | −1–2147483647 | any state | accepted no-op; microseconds. Default ours |
| `sync-inputs-ntp` | int | `0` | 0–2147483647 | any state | accepted no-op; nanoseconds. Default ours |
| `latency` | uint64 | `0` | the uint64 range | any state | GstAggregator's. It belongs to `batched-push-timeout`: set from it (or to 0) at every start, so a value set directly has no effect |
| `min-upstream-latency` | uint64 | `0` | the uint64 range | any state | GstAggregator's |
| `start-time-selection` | enum `GstAggregatorStartTimeSelection` | `zero` | `GstAggregatorStartTimeSelection` nicks | any state | GstAggregator's |
| `start-time` | uint64 | `18446744073709551615` | the uint64 range | any state | GstAggregator's |
| `emit-signals` | boolean | `false` | | any state | GstAggregator's (its `samples-selected` signal) |

## Metadata

One `RdsBatchMeta` (`NvDsBatchMeta`) per batch, with `max_frames_in_batch` = `batch-size`, and per frame an
`RdsFrameMeta` (`NvDsFrameMeta`):

| Field | Value |
|---|---|
| `source_id`, `pad_index` | the sink pad number N |
| `batch_id` | the frame's slot in the batch |
| `frame_num` | a per-source counter from 0 (see the `frame-num-reset-*` properties) |
| `buf_pts` | the input buffer's PTS, in the source's own segment |
| `ntp_timestamp` | the system time in ns, or 0 (see `attach-sys-ts`) |
| `source_frame_width`, `source_frame_height` | the input size, after its crop meta |
| `pipeline_width`, `pipeline_height` | the slot size, `width`×`height` |

The batch's `RdsBufSurface` has one entry per slot in `surfaceList`; `numFilled` is the number of frames in the batch.

## Behaviour

<a id="scaling-and-padding"></a>**Scaling and padding.** Every frame goes through `rds_buf_surf_transform` into its slot
(the exact rules are in the compatibility matrix's ["Transform semantics"](../compat-matrix.md#transform-semantics)).
With `enable-padding`, the frame is scaled to fit with its aspect ratio kept (even width and height for NV12 and I420)
and placed at the **top-left** of the slot; the rest of the slot is black (Y 16, U/V 128 for YUV; every byte 0, alpha
included, for RGB and GRAY8). Top-left keeps the mapping from slot to source coordinates a pure scale: divide by
`pipeline_width / source_frame_width`, with no offset.

<a id="batching-and-timeout"></a>**Batching and timeout.**
- One batch takes at most one frame per sink pad. The pads are visited round robin, the first one rotating every
  batch, and a batch is closed at `batch-size` frames. A partial batch has fewer frames than `batch-size`: its
  surface's `numFilled` and its frame metas say how many.
- `batched-push-timeout` `-1` (no timeout): a batch goes out only once every source that has not ended has a frame
  queued; then at most `batch-size` of those frames go in. A stalled source therefore stalls the batch: set a timeout
  for live or unreliable sources.
- `batched-push-timeout` ≥ 0: a partial batch is due one timeout (plus `max-latency`) after its **first frame
  arrived**, whether the frames are ahead of the clock (a file decoding faster than real time) or behind it. If the
  frame arrived while the previous batch was still being built, the timer still runs from that arrival, not from when
  the previous batch went out. Unlike `sync-inputs`, the timeout ignores upstream latency. The timer can't start
  before running time 0, so with a live source the first partial batches can go out up to one upstream latency late.
- `sync-inputs=true`: a partial batch goes out once the clock reaches its earliest frame's running time plus the
  latency (`batched-push-timeout` or 0, plus `max-latency` and the upstream latency). A frame already due goes out at
  once with whatever else is queued.
- In every mode, a batch with a frame from every source goes out as soon as it is complete: frames are never held
  back to the clock.

<a id="batch-timestamps"></a>**Batch timestamps.**
- `live-source=true`: the batch PTS is the running time of the last frame copied into it (as documented).
- `live-source=false`: the running time of the earliest frame in the batch.
- Either way, batch timestamps never go backwards: a batch earlier than the previous one (a source joining late, or
  with another segment) gets the previous batch's PTS. DTS and duration are unset. The output segment is TIME from
  0, so the PTS is the running time.

**Stream events.** Downstream of the mux, a source's lifecycle travels as Redstream's custom stream events, the
counterparts of DeepStream's `gst-nvevent` events. The mux owns them: each is a serialized custom downstream event
that carries the id of the source it is about (N, the sink pad number), written `rds-<name> (N)` on these pages. Their
types, constructors and parsers are in [`rds/rds-event.h`](../../gst-libs/rds/rds-event.h), and the matrix's
["Stream events"](../compat-matrix.md#stream-events-gst-nvevent) section maps them to DeepStream's names. They go
downstream in order with the batches, after the mux's own stream-start, caps and segment:

| Event | When |
|---|---|
| `rds-pad-added (N)` | source N's **first event or frame** comes through, not when the pad is requested |
| `rds-stream-start (N)` | from that pad's STREAM_START |
| `rds-stream-segment (N)` | from that pad's SEGMENT |
| `rds-stream-eos (N)` | from that pad's EOS, after the batch with its last frame and before any later batch |
| `rds-stream-reset (N)` | before the source's first event or frame after a flush of that pad |
| `rds-pad-deleted (N)` | the pad is released, if its `rds-pad-added` went out; after any batch being built then (it may hold the pad's last frame) |

The pipeline EOS goes downstream when every source has ended, unless `drop-pipeline-eos` is set. With
`drop-pipeline-eos` and every source ended, the mux waits for a new source; its `rds-pad-added` goes out with the new
source's first data.

<a id="adding-and-removing-sources"></a>**Adding and removing sources.**
- Sink pads can be requested while PLAYING; the new source joins the next batches once its frames arrive.
- To remove a source cleanly, send EOS into its sink pad and wait for its `rds-stream-eos (N)` downstream, then
  release the pad. The EOS reaches the batching thread only after the frames queued in front of it; a pad released
  before that loses those frames and its `rds-stream-eos` (only `rds-pad-deleted` goes out).
- Releasing a sink pad sends `rds-pad-deleted` from the releasing thread. If a prerolled sink downstream is blocking
  (the pipeline PAUSED and the mux idle), that push, and so the release, blocks until the pipeline goes to PLAYING:
  release pads while PLAYING, or from a thread that may wait.

<a id="buffers-and-branches"></a>**Buffers and branches.**
- The output pool holds `buffer-pool-size` batches and never grows, so a stalled downstream back-pressures the
  sources.
- A live (or timed) mux's sink pads queue at least two frames plus a latency's worth before blocking upstream, on top
  of the one being batched: upstream buffer pools must allow for that.
- After `rdsstreamdemux`, every demuxed frame keeps its whole batch alive, and sinks keep their last buffer (`appsink`
  always, other sinks with `enable-last-sample`). So each such branch can pin one batch: set `buffer-pool-size` larger
  than the number of those branches, or the mux stalls for good. It isn't raised automatically, since that would cost
  memory in every pipeline without a demux.

**Errors** besides those above, all element errors:
- the output batch pool can't be set up: "Cannot set up the output batch pool" (RESOURCE, SETTINGS); there is no
  pool, or it can't be activated (RESOURCE, FAILED);
- a frame arrives before its caps (CORE, NEGOTIATION);
- an input or output buffer can't be mapped, an upload or a slot clear fails, the output batch is too small ("The
  output batch is too small"), or the scaling fails ("Scaling a frame into the batch failed"): STREAM, FAILED;
- a staging surface can't be allocated (RESOURCE, NO_SPACE_LEFT).

## Differences from DeepStream 8.0

- Only the default (scaling) mux; the new mux (zero-copy batches of decoder surfaces, `config-file-path`, adaptive
  batching, per-source frame limits) is not written yet.
- `width` and `height` are required (the new mux can pass sizes through).
- `enable-padding` places the frame top-left; NVIDIA's page says only that black bands keep the aspect ratio.
- `live-source=false`: the batch PTS is the earliest frame's running time; NVIDIA derives it from the frame rate of
  the first source that negotiated caps.
- `batched-push-timeout` counts from a batch's first frame, whatever the buffer timestamps; `sync-inputs` is
  specified above (NVIDIA documents only "synchronizes input frames using PTS").
- `rds-pad-added` goes out with a source's first data, as NVIDIA documents, not at the request.
- Many defaults are ours where NVIDIA's page gives none (marked in the table).

## Known issues and limits

- **Requesting an existing `sink_N` returns a broken pad.** The mux requests pads through GstAggregator's
  `request_new_pad`, which ignores the result of `gst_element_add_pad`. For a name already taken, GStreamer logs
  "already has a pad named sink_N" and "Padname sink_N is not unique … not adding", drops the new pad, and the caller
  still gets the pointer: using it gives more criticals or a crash. Never request an existing `sink_N`: check
  `gst_element_get_static_pad (mux, "sink_N")` first, as the snippets here and on the
  [rdsurisrcbin](rdsurisrcbin.md) page do (seen on GStreamer 1.28).
- One frame per input buffer; batches as input are refused.
- Partial batches still hold `batch-size` slots of memory: the pool is sized for full batches.
- No NTP from RTCP sender reports (`attach-sys-ts` gives the system time); `frame-duration` does nothing.
