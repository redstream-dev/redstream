# rdsvideodec

Decodes H.264 and H.265 with rocDecode (the GPU's VCN decoder) into NV12 frames in `memory:RDS`, or in system memory
when downstream can't take `memory:RDS`. DeepStream counterpart: `nvv4l2decoder`
([ref](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideo4linux2.html)).
Built when: always.

## Example

```sh
# decode to memory:RDS (the clips come from tests/media/make-test-media.sh, in .cache/media)
gst-launch-1.0 filesrc location=.cache/media/ball-720p.h264 ! h264parse ! rdsvideodec \
  ! "video/x-raw(memory:RDS)" ! fakesink sync=false

# autoplugging: uridecodebin picks rdsvideodec by its rank
gst-launch-1.0 -v uridecodebin uri=file://$PWD/.cache/media/ball-720p.h264 ! fakesink sync=false \
  | grep -o "GstRdsVideoDec:[a-z0-9]*"
```

A parser (`h264parse`, `h265parse`) goes in front: it gives the decoder whole access units.

**Rank.** `rdsvideodec` registers with rank primary + 10 (266), above the VA decoders `vah264dec` and `vah265dec`
(primary + 1). So `decodebin`, `decodebin3`, `uridecodebin` and `uridecodebin3`, and the bins built on them such as
`rdsurisrcbin`, pick it for H.264 and H.265, as DeepStream's autoplugging picks `nvv4l2decoder`. To keep it out of
autoplugging, lower its rank before the pipeline is built, with the environment variable

```sh
GST_PLUGIN_FEATURE_RANK=rdsvideodec:NONE gst-launch-1.0 uridecodebin uri=... ! ...
```

or from code, before the pipeline is created:

```c
GstPluginFeature *f = gst_registry_lookup_feature (gst_registry_get (), "rdsvideodec");

if (f != NULL) {
  gst_plugin_feature_set_rank (f, GST_RANK_NONE);
  gst_object_unref (f);
}
```

The element can still be used by name with either.

## Pads

| Pad | Direction | Presence | Caps |
|---|---|---|---|
| `sink` | sink | always | `video/x-h264` or `video/x-h265`, `stream-format=byte-stream`, `alignment=au`, `parsed=true` |
| `src` | src | always | `video/x-raw(memory:RDS)` or `video/x-raw`, format NV12, 1–16384 × 1–16384 |

- **Input:** 8-bit 4:2:0 H.264 and H.265 in Annex B byte-stream, one access unit per buffer. The caps want
  `alignment=au` and `parsed=true`, so a parser in front negotiates them; rocDecode decodes split NAL units wrongly.
- **Output memory:** `memory:RDS` (single NV12 surfaces from the decoder's own pool, in the memory `cudadec-memtype`
  selects) when downstream accepts it; otherwise system memory, copied from the GPU. The choice is made again at every
  negotiation, so a new downstream peer can move the output between the two. The output size is the stream's display
  size (the coded size cropped to the display window); the frame rate and pixel aspect ratio come from the input caps.

## Properties

`gpu-id`, `cudadec-memtype` and `num-extra-surfaces` are read when the decoder starts (READY to PAUSED). A new value
set later is stored, without a warning, and used at the next start; it doesn't change a running decoder. The other
properties can change while playing and apply from the next frame. A value outside a property's range is refused by
GObject with a GLib warning.

| Property | Type | Default | Values | Changeable | Description |
|---|---|---|---|---|---|
| `gpu-id` | uint | `0` | 0–4294967295: a visible ROCm device | any state (applied at the next start) | the GPU that decodes. One that doesn't exist fails the start: "GPU N is not available" (RESOURCE, NOT_FOUND) |
| `cudadec-memtype` | enum `GstRdsCudaDecMemType` | `memtype_device` | `0` `memtype_device`, `1` `memtype_pinned`, `2` `memtype_unified` | any state (applied at the next start) | the memory of the `memory:RDS` output surfaces; no effect on system-memory output. DeepStream's default is `2` (unified), which is slow on discrete AMD GPUs |
| `num-extra-surfaces` | uint | `0` | 0–24 | any state (applied at the next start) | decode surfaces on top of the stream's minimum; the decoder always adds 4 of its own |
| `drop-frame-interval` | uint | `0` | 0–30 | any state | output displayed frames 0, N, 2N, …; 0 and 1 output every frame. Counted from the start or the last flush |
| `skip-frames` | enum `GstRdsSkipFrames` | `decode_all` | `0` `decode_all`, `1` `decode_non_ref`, `2` `decode_key` | any state | `decode_key` drops delta frames before decoding. `decode_non_ref` (Jetson-only on DeepStream) decodes every frame and logs a debug-log warning |
| `low-latency-mode` | boolean | `false` | | any state | accepted no-op: pictures always come out as soon as the stream order allows |
| `qos` | boolean | `true` | | any state | GstVideoDecoder's: drop late frames on QoS events |
| `max-errors` | int | `-1` | −1 – 2147483647 | any state | GstVideoDecoder's: decode errors tolerated before an element error (see Errors) |
| `min-force-key-unit-interval` | uint64 | `0` | the uint64 range | any state | GstVideoDecoder's |
| `discard-corrupted-frames` | boolean | `false` | | any state | GstVideoDecoder's |
| `automatic-request-sync-points` | boolean | `false` | | any state | GstVideoDecoder's |
| `automatic-request-sync-point-flags` | flags `GstVideoDecoderRequestSyncPointFlags` | `discard-input+corrupt-output` | `GstVideoDecoderRequestSyncPointFlags` nicks | any state | GstVideoDecoder's |

## Metadata

None read or written: the output buffers carry no `RdsBatchMeta`. `rdsstreammux` adds it when it batches the frames.

## Behaviour

- **Decoding.** Each access unit goes to rocDecode's parser as one packet. Pictures come out in display order, each
  copied from the decoder's surface into an output buffer on the element's own HIP stream, which is synchronized
  before the frame is pushed: the frame is complete when downstream gets it.
- **Stream changes.** A new sequence with another size reconfigures the decoder in place when the new coded size
  fits the one it was created for, else creates it again; either way the output caps follow. A new codec in the
  input caps shows the old stream's pending pictures first.
- **Flushes and EOS.** A flush (a seek) drops the parser and the decoder, so stale reference pictures are gone; both
  come back with the next frame. A drain or EOS outputs every pending picture first.
- **`skip-frames`.** With `decode_key`, delta frames never reach the decoder. After a skipped delta frame, the next
  delta frames are dropped too until a key frame, also after going back to `decode_all`: they would decode against
  missing references, which rocDecode doesn't report.
- **The last picture.** rocDecode's parser holds the last picture it was given until the next one arrives, since the
  end of a picture is only known then. A stream that stops mid-flow (an RTSP source that reconnects) gets its last
  picture out only with new data or an EOS.

**Errors.**
- 10-bit or 12-bit video: "Unsupported bit depth N (only 8-bit video is supported)"; a chroma format other than
  4:2:0: "Unsupported chroma format N (only 4:2:0 is supported)"; an empty display window: "Invalid display area".
  All are element errors (STREAM, DECODE).
- A bitstream error ("Decoding failed") is tolerated as GstVideoDecoder tolerates errors: the frame is dropped, and
  the element error comes only after more than `max-errors` of them in a row (`-1`: never). A picture whose
  references are missing (a start mid-GOP) is dropped the same way.
- Errors outside the bitstream are element errors: the decoder, parser, HIP stream or output buffer can't be created
  (RESOURCE, FAILED), the picture can't be copied out (RESOURCE, FAILED), an input buffer can't be mapped ("Cannot
  map the input buffer", STREAM, DECODE), or no NV12 output of the size negotiates (CORE, NEGOTIATION).

## Differences from DeepStream 8.0

- `cudadec-memtype` defaults to `0` (device memory); DeepStream's default is `2` (unified).
- `num-extra-surfaces` and `drop-frame-interval` accept 0; NVIDIA's page gives 1–24 and 1–30, both with default 0.
- `skip-frames=decode_non_ref` decodes every frame (it's Jetson-only on DeepStream).
- `low-latency-mode` is accepted and does nothing: output is always as early as the stream order allows.
- Jetson-only properties, `capture-io-mode`, `output-io-mode` and the SEI properties are not there.
- The rank puts it above the stock VA decoders, so autoplugging picks it.

## Known issues and limits

- 8-bit 4:2:0 H.264 and H.265 only: no 10-bit, 4:2:2, 4:4:4, VP9 or AV1 yet.
- rocDecode surfaces of 1 MiB or less (320×240 NV12, say) lose their first decoded picture unless they are mapped to
  HIP before anything is decoded (seen with rocDecode 1.10 on Mesa 25.2 radeonsi). The decoder maps every surface
  when it creates or reconfigures the decoder, which avoids it.
- The stock `vah264dec`/`vah265dec` output is tiled on AMD, so they are not used for analytics; `rdsvideodec` gives
  pitch-linear surfaces.
