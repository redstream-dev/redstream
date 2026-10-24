/* Test media made in-process: encode a videotestsrc clip with the VA encoder, decode a byte stream with any decoder
 * and checksum every frame. Header-only (static functions); GPU tests skip when an element is missing.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_MEDIA_H
#define RDS_TEST_MEDIA_H

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

/* How long a pipeline may sit without producing a sample before the helpers give up. */
#define RDS_TEST_MEDIA_TIMEOUT (30 * GST_SECOND)

static inline gboolean
rds_test_have_element (const gchar * name)
{
  GstElementFactory *f = gst_element_factory_find (name);

  if (f == NULL)
    return FALSE;
  gst_object_unref (f);
  return TRUE;
}

/* Pops one bus message: fails the test on an error, TRUE on EOS. */
static inline gboolean
rds_test_bus_check (GstElement * pipe)
{
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg;
  gboolean eos = FALSE;

  while ((msg = gst_bus_pop_filtered (bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS)) != NULL) {
    if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
      GError *err = NULL;
      gchar *dbg = NULL;

      gst_message_parse_error (msg, &err, &dbg);
      g_error ("pipeline error from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message,
          dbg ? dbg : "");
    }
    eos = TRUE;
    gst_message_unref (msg);
  }
  gst_object_unref (bus);
  return eos;
}

/* Pulls the next sample from @sink without blocking forever (a blocking pull hangs on pipeline errors): polls with
 * a short timeout and checks the bus in between. NULL at EOS; fails the test on errors or a stall. */
static inline GstSample *
rds_test_pull (GstElement * pipe, GstElement * sink)
{
  gint64 deadline = g_get_monotonic_time () + RDS_TEST_MEDIA_TIMEOUT / GST_USECOND;

  while (g_get_monotonic_time () < deadline) {
    GstSample *s = gst_app_sink_try_pull_sample (GST_APP_SINK (sink), 100 * GST_MSECOND);

    if (s != NULL)
      return s;
    if (gst_app_sink_is_eos (GST_APP_SINK (sink)))
      return NULL;
    (void) rds_test_bus_check (pipe);
  }
  g_error ("no sample within %" GST_TIME_FORMAT, GST_TIME_ARGS (RDS_TEST_MEDIA_TIMEOUT));
  return NULL;
}

/* Encodes @frames frames of videotestsrc @pattern at @width x @height (NV12, 30 fps) with va{codec}enc and returns
 * the byte stream (whole access units, concatenated). @codec is "h264" or "h265". NULL if an element is missing. */
static inline GBytes *
rds_test_encode_full (const gchar * codec, guint width, guint height, guint frames, guint b_frames,
    const gchar * pattern, const gchar * enc_extra)
{
  gchar *enc = g_strdup_printf ("va%senc", codec), *parse = g_strdup_printf ("%sparse", codec);
  GByteArray *out;
  GstElement *pipe, *sink;
  GstSample *s;
  GError *err = NULL;
  gchar *desc;

  if (!rds_test_have_element (enc) || !rds_test_have_element (parse)) {
    g_free (enc);
    g_free (parse);
    return NULL;
  }
  desc = g_strdup_printf ("videotestsrc pattern=%s num-buffers=%u "
      "! video/x-raw,format=NV12,width=%u,height=%u,framerate=30/1 ! %s b-frames=%u %s ! %s "
      "! video/x-%s,stream-format=byte-stream,alignment=au ! appsink name=sink sync=false",
      pattern, frames, width, height, enc, b_frames, enc_extra ? enc_extra : "", parse, codec);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  out = g_byte_array_new ();
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstMapInfo map;

    g_assert_true (gst_buffer_map (gst_sample_get_buffer (s), &map, GST_MAP_READ));
    g_byte_array_append (out, map.data, map.size);
    gst_buffer_unmap (gst_sample_get_buffer (s), &map);
    gst_sample_unref (s);
  }
  (void) rds_test_bus_check (pipe);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (pipe);
  g_free (desc);
  g_free (enc);
  g_free (parse);
  g_assert_cmpuint (out->len, >, 0);
  return g_byte_array_free_to_bytes (out);
}

static inline GBytes *
rds_test_encode (const gchar * codec, guint width, guint height, guint frames, guint b_frames,
    const gchar * pattern)
{
  return rds_test_encode_full (codec, width, height, frames, b_frames, pattern, NULL);
}

/* Checksum of a mapped NV12 frame: Y (width x height bytes) and UV (2*ceil(w/2) x ceil(h/2) bytes), row by row
 * with the frame's strides, so padding never counts. "y=<md5> uv=<md5>"; free with g_free. */
static inline gchar *
rds_test_nv12_checksum (const GstVideoFrame * frame)
{
  guint w = GST_VIDEO_FRAME_WIDTH (frame), h = GST_VIDEO_FRAME_HEIGHT (frame);
  GChecksum *y = g_checksum_new (G_CHECKSUM_MD5), *uv = g_checksum_new (G_CHECKSUM_MD5);
  gchar *ret;

  g_assert_cmpint (GST_VIDEO_FRAME_FORMAT (frame), ==, GST_VIDEO_FORMAT_NV12);
  for (guint r = 0; r < h; r++)
    g_checksum_update (y, (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (frame, 0)
        + (gsize) r * GST_VIDEO_FRAME_PLANE_STRIDE (frame, 0), w);
  for (guint r = 0; r < (h + 1) / 2; r++)
    g_checksum_update (uv, (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (frame, 1)
        + (gsize) r * GST_VIDEO_FRAME_PLANE_STRIDE (frame, 1), 2 * ((w + 1) / 2));
  ret = g_strdup_printf ("y=%s uv=%s", g_checksum_get_string (y), g_checksum_get_string (uv));
  g_checksum_free (uv);
  g_checksum_free (y);
  return ret;
}

/* Decodes @stream with `appsrc ! {codec}parse ! @decoder [! rdsvideoconvert] ! video/x-raw,format=NV12 ! appsink`
 * and returns one checksum string per output frame (rds_test_nv12_checksum), in output order. With @rds_memory
 * FALSE the system-memory capsfilter follows the decoder directly (a memory:RDS decoder must use its system
 * fallback); with TRUE, a memory:RDS capsfilter and rdsvideoconvert (which downloads) follow it. */
static inline GPtrArray *
rds_test_decode_crc (GBytes * stream, const gchar * codec, const gchar * decoder, gboolean rds_memory)
{
  GPtrArray *crcs = g_ptr_array_new_with_free_func (g_free);
  GstElement *pipe, *src, *sink;
  GstSample *s;
  GError *err = NULL;
  gchar *desc;

  desc = g_strdup_printf ("appsrc name=src caps=video/x-%s,stream-format=byte-stream format=bytes "
      "! %sparse ! %s %s ! video/x-raw,format=NV12 ! appsink name=sink sync=false",
      codec, codec, decoder, rds_memory ? "! video/x-raw(memory:RDS),format=NV12 ! rdsvideoconvert" : "");
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  src = gst_bin_get_by_name (GST_BIN (pipe), "src");
  sink = gst_bin_get_by_name (GST_BIN (pipe), "sink");
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src), gst_buffer_new_wrapped_bytes (stream)), ==,
      GST_FLOW_OK);
  g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src)), ==, GST_FLOW_OK);
  while ((s = rds_test_pull (pipe, sink)) != NULL) {
    GstVideoInfo info;
    GstVideoFrame frame;

    g_assert_true (gst_video_info_from_caps (&info, gst_sample_get_caps (s)));
    g_assert_true (gst_video_frame_map (&frame, &info, gst_sample_get_buffer (s), GST_MAP_READ));
    g_ptr_array_add (crcs, rds_test_nv12_checksum (&frame));
    gst_video_frame_unmap (&frame);
    gst_sample_unref (s);
  }
  (void) rds_test_bus_check (pipe);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (sink);
  gst_object_unref (src);
  gst_object_unref (pipe);
  g_free (desc);
  return crcs;
}

#endif /* RDS_TEST_MEDIA_H */
