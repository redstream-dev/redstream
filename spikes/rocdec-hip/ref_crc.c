/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Decode an H.264 byte-stream with vah264dec into system-memory NV12 and print per-frame CRC32s.
 * Output format (shared with rocdec_spike): "frame <n> y=<crc> uv=<crc>". */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <zlib.h>
#include <stdio.h>

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  if (argc != 2) {
    fprintf (stderr, "usage: %s file.h264\n", argv[0]);
    return 1;
  }
  gchar *desc = g_strdup_printf ("filesrc location=%s ! h264parse ! vah264dec "
      "! video/x-raw,format=NV12 ! appsink name=sink sync=false", argv[1]);
  GError *err = NULL;
  GstElement *pipe = gst_parse_launch (desc, &err);
  g_free (desc);
  if (!pipe) {
    fprintf (stderr, "pipeline: %s\n", err->message);
    g_error_free (err);
    return 1;
  }
  GstAppSink *sink = GST_APP_SINK (gst_bin_get_by_name (GST_BIN (pipe), "sink"));
  GstBus *bus = gst_element_get_bus (pipe);
  gst_element_set_state (pipe, GST_STATE_PLAYING);

  /* pull_sample blocks forever if an error stops streaming before EOS (or before the
   * sink starts), so poll with a timeout and check the bus for errors in between. */
  long n = 0;
  int ok = 1;
  for (;;) {
    GstSample *s = gst_app_sink_try_pull_sample (sink, 100 * GST_MSECOND);
    if (!s) {
      GstMessage *msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR);
      if (msg) {
        GError *e = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error (msg, &e, &dbg);
        fprintf (stderr, "error from %s after %ld frames: %s\n%s\n",
            GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), n, e->message, dbg ? dbg : "");
        g_error_free (e);
        g_free (dbg);
        gst_message_unref (msg);
        ok = 0;
        break;
      }
      if (gst_app_sink_is_eos (sink))
        break;
      continue;
    }
    GstVideoInfo info;
    GstVideoFrame f;
    if (!gst_video_info_from_caps (&info, gst_sample_get_caps (s))) {
      fprintf (stderr, "frame %ld: cannot parse caps\n", n);
      gst_sample_unref (s);
      return 1;
    }
    if (!gst_video_frame_map (&f, &info, gst_sample_get_buffer (s), GST_MAP_READ)) {
      fprintf (stderr, "frame %ld: cannot map buffer\n", n);
      gst_sample_unref (s);
      return 1;
    }
    int w = GST_VIDEO_INFO_WIDTH (&info), h = GST_VIDEO_INFO_HEIGHT (&info);
    /* NV12 UV plane: interleaved, so row bytes = 2 * chroma width; rounds up for odd sizes. */
    int uvw = 2 * GST_VIDEO_FRAME_COMP_WIDTH (&f, 1), uvh = GST_VIDEO_FRAME_COMP_HEIGHT (&f, 1);
    uLong y = crc32 (0, Z_NULL, 0), uv = crc32 (0, Z_NULL, 0);
    for (int r = 0; r < h; r++)
      y = crc32 (y, (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&f, 0) + r * GST_VIDEO_FRAME_PLANE_STRIDE (&f, 0), w);
    for (int r = 0; r < uvh; r++)
      uv = crc32 (uv, (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&f, 1) + r * GST_VIDEO_FRAME_PLANE_STRIDE (&f, 1), uvw);
    printf ("frame %ld y=%08lx uv=%08lx\n", n++, y, uv);
    gst_video_frame_unmap (&f);
    gst_sample_unref (s);
  }
  gst_object_unref (bus);

  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (pipe);
  return ok && n > 0 ? 0 : 1;
}
