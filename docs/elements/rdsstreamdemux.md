# rdsstreamdemux

How to read this page, and how to run the examples: [element reference](README.md).

Splits the batches `rdsstreammux` makes back into one stream per source, without copying: each output buffer is a
read-only view of the frame's slot in the batch, with the source's own timestamps, segment and stream events.
DeepStream counterpart: `nvstreamdemux`
([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvstreamdemux.html)).
Built when: always.

## Example

```sh
# two sources batched by the mux and split again; a queue on every branch
gst-launch-1.0 rdsstreammux name=m batch-size=2 width=1280 height=720 ! rdsstreamdemux name=d \
  videotestsrc num-buffers=100 ! video/x-raw,format=NV12,width=1280,height=720 ! m.sink_0 \
  videotestsrc num-buffers=100 pattern=ball ! video/x-raw,format=NV12,width=640,height=480 ! m.sink_1 \
  d.src_0 ! queue ! fakesink sync=false \
  d.src_1 ! queue ! fakesink sync=false
```

The mux needs a GPU. The `src_%u` pads are request pads: `d.src_0` in a launch line requests the pad of source 0.

An application requests `src_N` for source N (its `source_id`, the mux's `sink_N`), and can do so while PLAYING,
from any thread, also from a `pad-added` handler that adds the source. A branch added while PLAYING needs a sink with
`async=false`:

```c
/* a branch for source `id`: src_<id>, a queue, a sink */
static gboolean
add_branch (GstElement * pipeline, GstElement * demux, guint id)
{
  gchar *name = g_strdup_printf ("src_%u", id);
  GstPad *src = gst_element_request_pad_simple (demux, name);
  GstElement *queue = gst_element_factory_make ("queue", NULL);
  GstElement *sink = gst_element_factory_make ("fakesink", NULL);
  GstPad *queue_sink;
  gboolean ok = FALSE;

  g_free (name);
  if (src == NULL || queue == NULL || sink == NULL) {   /* a bad name, src_<id> exists, or no such factory */
    if (src != NULL)
      gst_element_release_request_pad (demux, src);
    g_clear_object (&src);
    if (queue != NULL)
      gst_object_unref (gst_object_ref_sink (queue));
    if (sink != NULL)
      gst_object_unref (gst_object_ref_sink (sink));
    return FALSE;
  }
  g_object_set (sink, "async", FALSE, NULL);
  gst_bin_add_many (GST_BIN (pipeline), queue, sink, NULL);
  /* the branch runs before src_<id> is linked: a push into elements still in NULL would return FLUSHING, and
   * the demux would stop every branch */
  if (gst_element_link (queue, sink) && gst_element_sync_state_with_parent (sink)
      && gst_element_sync_state_with_parent (queue)) {
    queue_sink = gst_element_get_static_pad (queue, "sink");
    ok = gst_pad_link (src, queue_sink) == GST_PAD_LINK_OK;
    gst_object_unref (queue_sink);
  }
  if (!ok) {
    gst_element_release_request_pad (demux, src);
    gst_element_set_state (queue, GST_STATE_NULL);
    gst_element_set_state (sink, GST_STATE_NULL);
    gst_bin_remove_many (GST_BIN (pipeline), queue, sink, NULL);
  }
  gst_object_unref (src);       /* the demux keeps its own reference while the pad exists */
  return ok;
}
```

To remove a branch, block or unlink `src_N` first, then set its elements to NULL and give the pad back with
`gst_element_release_request_pad`. A sink set to NULL while still linked returns FLUSHING, which stops every branch
(see Flow).

## Pads

The stream events below, written `rds-stream-eos (N)`, `rds-pad-deleted (N)` and so on, are `rdsstreammux`'s custom
downstream events; each carries the id of the source it is about. Their types and parsers are in `rds/rds-event.h`;
[rdsstreammux](rdsstreammux.md) lists when each is sent.

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-raw(memory:RDS)`: batches with an `RdsBatchMeta`, as `rdsstreammux` makes them |
| `src_%u` | src | request | `video/x-raw(memory:RDS)`, the sink caps: one frame per buffer, at the mux's `width`×`height` |

- `src_N` carries the frames of source N. A request with a name names N (`src_3`); a request for `src_%u` (or with
  no name) gets the lowest free N.
- A name that doesn't match `src_%u` gets no pad (NULL): GStreamer itself rejects it before the demux sees it, with a
  critical from `gst_element_request_pad` given the template and the name, silently from
  `gst_element_request_pad_simple`. A number too large for a `guint` gets NULL and a debug-log warning from the demux.
- An N that already has a pad gets no pad either; GStreamer logs a critical for that request ("… already has a pad
  named src_N"), so check before you request.
- Releasing a pad waits for a push in progress on it: pushes hold the pad's stream lock.
- The demux answers no ALLOCATION query on its sink pad: its outputs are views of the batch, so it has no pool to
  propose.

## Properties

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `per-stream-eos` | boolean | `false` | | any state | `true`: `rds-stream-eos (N)` ends `src_N` (an EOS on it) as soon as source N ends. `false`: a pad gets EOS on the pipeline EOS, or on `rds-pad-deleted (N)` |

## Metadata

- **Reads:** the batch's `RdsBatchMeta` (`NvDsBatchMeta`), under its meta lock: for each `RdsFrameMeta`
  (`NvDsFrameMeta`) its `source_id` (the output pad), `batch_id` (the slot) and `buf_pts` (the output PTS).
- **Writes:** on each output buffer, a new one-frame `RdsBatchMeta` holding a deep copy of the frame meta
  (`rds_copy_frame_meta_to_batch`), with `batch_id` 0. The copy has the frame's objects, classifier and label metas,
  display meta and user meta (user meta through its copy function; user meta without one is skipped, with one
  debug-log warning per process). An object whose parent is in another frame gets a NULL parent. The batch's own
  meta is not changed.

## Behaviour

**Buffers.**
- Each output buffer is a read-only view of the frame's slot (`rds_buffer_new_surface_view`), not a copy. The batch
  goes back to the mux's pool once every view of it is released.
- The view has the batch's flags and duration. Its PTS is the frame's `buf_pts` (the source's own timestamp); its DTS
  is unset.
- The first buffer of every stream on a pad, and the first after a flush, is DISCONT.
- To write pixels (an OSD, say), make the buffer writable (`gst_buffer_make_writable`) and map it for writing: the
  writer gets its own copy of the frame, and the batch the other sources share stays untouched. A write map of the
  view's memory itself fails.
- Frames stay at the mux's `width`×`height`; nothing is scaled back to the source's size, as DeepStream documents.
- Frames of a source with no pad, or whose pad has ended, are dropped (the batch's flow return stays OK). A frame
  whose `batch_id` names no slot of the batch is dropped with a debug-log warning.
- A buffer without a batch meta or a `memory:RDS` surface is an element error (STREAM, DEMUX).

**Sticky events**, per src pad, also for pads requested while data flows:
- **stream-start:** the upstream stream id plus `/N`, then `/N/<k>` for the k-th later stream on the pad, with the
  upstream group id. A new one comes for every `rds-stream-start (N)`, when an ended pad starts again, and on every
  pad for an upstream stream-start. Going from PAUSED to READY starts the count again, so the next id is `/N`.
- **caps:** the sink caps, again whenever they change.
- **segment**, before the first buffer or EOS: source N's own segment (from `rds-stream-segment (N)`) once one came,
  else the mux's output segment. `buf_pts` is in the source's segment, so a synchronized sink computes the source's
  running times. A source's segment is forgotten on a flush and on its `rds-stream-start`, `rds-stream-reset` and
  `rds-pad-deleted`; the mux sends a new one before the source's next frame.
- Events without a source (a GAP, other timed events) are in the mux's output time base, not the sources'.

**Other downstream events.**
- The stream events for source N (`rds-pad-added`, `rds-pad-deleted`, `rds-stream-start`, `rds-stream-segment`,
  `rds-stream-eos`, `rds-stream-reset`) go to `src_N` only, and are dropped when it does not exist, so a per-source
  branch sees only its own source's lifecycle. A malformed one is dropped with a debug-log warning.
- `rds-pad-deleted (N)` always ends `src_N`; `rds-stream-eos (N)` ends it when `per-stream-eos` is `true`.
- The pipeline EOS ends every pad not ended yet. A pad gets EOS at most once per stream.
- After a pad has ended, `rds-stream-start`, `rds-pad-added` or `rds-stream-reset` for its source start a new stream
  on it.
- Flushes and every other event go to every pad. A flush (its FLUSH_STOP) also clears every pad's ended state, so a
  pad that got EOS carries frames again. Going from PAUSED to READY resets every pad to a new stream.

**Upstream events** from the src pads: QoS is dropped, since one late branch would throttle every source. A seek
sent by several branches (the same seqnum, as every sink of one pipeline sends it) goes upstream once. Other events
go upstream.

**Flow and branches.**
- The return for a batch combines the pads' returns (`GstFlowCombiner`): NOT_LINKED only when every pad is unlinked.
  A pad released during a push returns FLUSHING, which is not counted.
- As with `tee`, FLUSHING or an error from one branch stops the streaming thread for all of them. **Block or unlink a
  branch before stopping it.**
- **Give every branch a `queue`.** The branches share the demux's streaming thread: without queues the first sink to
  preroll blocks the others, and the pipeline never finishes prerolling.
- <a id="views-pin-batches"></a>**Views pin batches.** A view keeps its whole batch out of the mux's pool until it is
  released, and most sinks keep their last buffer (`appsink` always; base sinks with `enable-last-sample`, the default).
  So N branches can pin N batches: set `rdsstreammux`'s `buffer-pool-size` (default 4) larger than the number of such
  branches, or the mux stalls.
- **Removing a source:** send EOS into its mux pad and wait for `rds-stream-eos (N)` downstream before releasing the
  mux pad. Releasing right after the EOS drops the stream-eos.

## Differences from DeepStream 8.0

The pad and property names, and the documented behaviour, follow DeepStream's public `nvstreamdemux` documentation.
Where it is silent, ours:
- **Segments:** `src_N` gets source N's own segment, so a synchronized sink computes the source's running time from
  `buf_pts`, also when the source's segment does not start at 0 (MPEG-TS timestamps, seeks, restarts).
- **Stream events:** the per-source `rds-*` events go to the source's pad only, so a branch is not reset by other
  sources' events.
- **QoS** from a branch is not forwarded upstream; a seek every branch sends goes upstream once.
- **Writes** to a view get their own copy of the frame, never the shared batch.

## Known issues and limits

- Frames are not scaled back to their source's size (DeepStream doesn't either).
- Each branch pins batches until its sink drops them: see [Views pin batches](#views-pin-batches) for
  `buffer-pool-size`.
- Video only; audio batches (as DeepStream's new mux makes) are planned, on request.
