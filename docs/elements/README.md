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
- `read-only`: it can't be set.

A value outside a property's range is refused by GObject with a GLib warning. "Debug-log warning" means a
`GST_WARNING` in the element's debug category, usually its name (seen with `GST_DEBUG=<category>:2`); "GLib warning"
means a `g_warning` on stderr. A "debug-log INFO message" is a `GST_INFO` in the element's debug category (seen with
`GST_DEBUG=<category>:4`).

The Description column uses these words from the compatibility matrix:
- **accepted no-op**: the property is stored and read back, and does nothing. Any log message it produces is
  described on the element's page.
- **accepted**: stored and read back, and does what the row says rather than what DeepStream does with it.

## Elements

| Element | Purpose | DeepStream |
|---|---|---|
| [`rdslogger`](rdslogger.md) | prints each stream's frame rate to stdout | `nvdslogger` |
| [`rdsstreamdemux`](rdsstreamdemux.md) | splits batches back into one stream per source, without copying | `nvstreamdemux` |
| [`rdsstreammux`](rdsstreammux.md) | batches frames from several sources into fixed-size `memory:RDS` slots | `nvstreammux` |
| [`rdsurisrcbin`](rdsurisrcbin.md) | decodes a file or RTSP URI to `memory:RDS` frames, with RTSP reconnection | `nvurisrcbin` |
| [`rdsvideoconvert`](rdsvideoconvert.md) | converts, scales and crops single frames on the GPU | `nvvideoconvert` |
| [`rdsvideodec`](rdsvideodec.md) | decodes H.264 and H.265 with rocDecode into `memory:RDS` | `nvv4l2decoder` |
| [`rdsvideosink`](rdsvideosink.md) | shows single frames through a stock GStreamer video sink | `nveglglessink`, `nv3dsink` |
