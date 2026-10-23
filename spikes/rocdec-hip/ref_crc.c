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
    return 1;
  }
  GstAppSink *sink = GST_APP_SINK (gst_bin_get_by_name (GST_BIN (pipe), "sink"));
  gst_element_set_state (pipe, GST_STATE_PLAYING);

  long n = 0;
  GstSample *s;
  while ((s = gst_app_sink_pull_sample (sink))) {
    GstVideoInfo info;
    GstVideoFrame f;
    gst_video_info_from_caps (&info, gst_sample_get_caps (s));
    gst_video_frame_map (&f, &info, gst_sample_get_buffer (s), GST_MAP_READ);
    int w = GST_VIDEO_INFO_WIDTH (&info), h = GST_VIDEO_INFO_HEIGHT (&info);
    uLong y = crc32 (0, Z_NULL, 0), uv = crc32 (0, Z_NULL, 0);
    for (int r = 0; r < h; r++)
      y = crc32 (y, (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&f, 0) + r * GST_VIDEO_FRAME_PLANE_STRIDE (&f, 0), w);
    for (int r = 0; r < h / 2; r++)
      uv = crc32 (uv, (guint8 *) GST_VIDEO_FRAME_PLANE_DATA (&f, 1) + r * GST_VIDEO_FRAME_PLANE_STRIDE (&f, 1), w);
    printf ("frame %ld y=%08lx uv=%08lx\n", n++, y, uv);
    gst_video_frame_unmap (&f);
    gst_sample_unref (s);
  }
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (pipe);
  return n > 0 ? 0 : 1;
}
