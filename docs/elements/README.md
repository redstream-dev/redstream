# Element reference

One page per Redstream element: what it does, its pads and caps, properties, signals, the metadata it reads and
writes, its behaviour, how it differs from DeepStream 8.0, and its known issues. The DeepStream names and their status
are in the [compatibility matrix](../compat-matrix.md).

The examples run from the repository root after a build, with `GST_PLUGIN_PATH_1_0=build/gst` so GStreamer finds the
build tree's plugins (they find `libgstrds` through their run path). A program built against `libgstrds` from the
build tree also needs `LD_LIBRARY_PATH=build/gst-libs/rds`.

## How to read these pages

The Changeable column of the property tables says when a property can be set:

- `any state`: at any time; it applies from the next buffer (or as the row says).
- `NULL, READY`: only in NULL or READY. A value set in PAUSED or PLAYING, or on the way there, is refused with a
  GLib warning, and the property keeps its value.
- `any state (applied at the next start)`: stored at any time, without a warning, and applied when the element
  starts, READY to PAUSED; a running element keeps the value it started with. When the start is another state
  change, or the value is read more than once, the cell says `any state (applied at the next start: see <section>)`
  and that section of the page says when.
- `any state (stored with a GLib warning, applied at the next start)`: as the previous form, but a value set while
  the element is started also logs a GLib warning saying so.
- `read-only`: it can't be set.

A value outside a property's range is refused by GObject with a GLib warning. A Values cell that starts with a range
(`0–4294967295`) gives the property's whole range, as GObject checks it; narrower practical limits are in words after
it (`0–4294967295: a visible ROCm device`). "Debug-log warning" means a
`GST_WARNING` in the element's debug category, usually its name (seen with `GST_DEBUG=<category>:2`); "GLib warning"
means a `g_warning` on stderr. A "debug-log INFO message" is a `GST_INFO` in the element's debug category (seen with
`GST_DEBUG=<category>:4`).

The Description column uses these words from the compatibility matrix:
- **accepted no-op**: the property is stored and read back, and does nothing. Any log message it produces is
  described on the element's page.
- **accepted**: stored and read back, and does what the row says rather than what DeepStream does with it.

Behaviour tables have the columns `| Behaviour | Status | Notes |`. A status is "matches nvX (DeepStream 8.0)", observed
DeepStream 8.0 behaviour where NVIDIA's docs are silent, or "ours", where Redstream decides.

## Elements

| Element | Purpose | DeepStream |
|---|---|---|
| [`rdsanalytics`](rdsanalytics.md) | ROI filtering, overcrowding, direction and line crossing, with DeepStream-shaped metas; config keys on [rdsanalytics-config](rdsanalytics-config.md) | `nvdsanalytics` |
| [`rdsh264enc`, `rdsh265enc`](rdsh26xenc.md) | encode single frames to H.264 or H.265 with the stock VA encoders, under DeepStream 8.0's properties | `nvv4l2h264enc`, `nvv4l2h265enc` |
| [`rdsinfer`](rdsinfer.md) | runs an ONNX model with MIGraphX: detectors, classifiers, tensor output; config keys on [rdsinfer-config](rdsinfer-config.md) | `nvinfer` |
| [`rdslogger`](rdslogger.md) | prints each stream's frame rate to stdout | `nvdslogger` |
| [`rdsmultistreamtiler`](rdsmultistreamtiler.md) | composes a batch into one tiled frame, with the metadata scaled into the tiles | `nvmultistreamtiler` |
| [`rdsosd`](rdsosd.md) | draws boxes, labels, display meta and the clock on RGBA frames, on the GPU or the CPU | `nvdsosd` |
| [`rdsstreamdemux`](rdsstreamdemux.md) | splits batches back into one stream per source, without copying | `nvstreamdemux` |
| [`rdsstreammux`](rdsstreammux.md) | batches frames from several sources into fixed-size `memory:RDS` slots | `nvstreammux` |
| [`rdstracker`](rdstracker.md) | gives each stream's objects tracking IDs: IOU and NvSORT on the CPU; config keys on [rdstracker-config](rdstracker-config.md) | `nvtracker` |
| [`rdsurisrcbin`](rdsurisrcbin.md) | decodes a file or RTSP URI to `memory:RDS` frames, with RTSP reconnection | `nvurisrcbin` |
| [`rdsvideoconvert`](rdsvideoconvert.md) | converts, scales and crops single frames on the GPU | `nvvideoconvert` |
| [`rdsvideodec`](rdsvideodec.md) | decodes H.264 and H.265 with rocDecode into `memory:RDS` | `nvv4l2decoder` |
| [`rdsvideosink`](rdsvideosink.md) | shows single frames through a stock GStreamer video sink | `nveglglessink`, `nv3dsink` |
