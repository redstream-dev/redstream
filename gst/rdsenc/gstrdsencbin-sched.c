/* Redstream: GstRdsEncBin's scheduling around the queue in front of the VA encoder: the key-frame requests that wait
 * for the next frame into the queue, and the convert's output pool (plain memory, not VA's). Locking:
 * gstrdsencbin-private.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsencbin-private.h"

#include <gst/video/video.h>

#define GST_CAT_DEFAULT gst_rds_enc_bin_debug

/* ---- key-frame requests ---- */

/* Requests for the next frame (force-idr, force-intra, and upstream force-key-unit events without a running time) apply
 * to the next frame into the queue in front of the VA encoder: queue_probe counts the frames in (rds-enc-keyunits gives
 * a waiting request the next one), va_sink_buffer_probe sends it into VA's src pad just before VA takes that frame, and
 * VA makes it an IDR frame. Sent straight to VA, they would land on the oldest frame in the queue instead, up to
 * VA_QUEUE_FRAMES earlier, depending on timing. Upstream events with a running time go straight to VA, which waits for
 * that time itself, and downstream force-key-unit events from upstream pass through the queue in order with the
 * frames. */

void
gst_rds_enc_bin_request_key_unit (GstRdsEncBin * self, GstEvent * event)
{
  GstRdsEncBinPrivate *priv = PRIV (self);

  g_mutex_lock (&priv->probe_lock);
  rds_enc_key_units_request (&priv->key_units, event);
  g_mutex_unlock (&priv->probe_lock);
}

/* The queue's input (the convert's output, on the upstream streaming thread): frames are counted, and the requests
 * waiting get the number of the first frame in. At a flush the queue drops its frames: the requests given to them wait
 * again, for the first frame after the flush, and both counts restart (neither side streams at FLUSH_STOP). */
static GstPadProbeReturn
queue_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBinPrivate *priv = PRIV (GST_RDS_ENC_BIN (user_data));
  guint n;

  if (GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_EVENT_BOTH) {
    if (GST_EVENT_TYPE (GST_PAD_PROBE_INFO_EVENT (info)) != GST_EVENT_FLUSH_STOP)
      return GST_PAD_PROBE_OK;
    g_mutex_lock (&priv->probe_lock);
    rds_enc_key_units_flush (&priv->key_units);
    g_mutex_unlock (&priv->probe_lock);
    return GST_PAD_PROBE_OK;
  }
  n = GST_PAD_PROBE_INFO_TYPE (info) & GST_PAD_PROBE_TYPE_BUFFER_LIST ?
      gst_buffer_list_length (GST_PAD_PROBE_INFO_BUFFER_LIST (info)) : 1;
  g_mutex_lock (&priv->probe_lock);
  rds_enc_key_units_frames_in (&priv->key_units, n);
  g_mutex_unlock (&priv->probe_lock);
  return GST_PAD_PROBE_OK;
}

/* Upstream events into the bin's src pad: a force-key-unit event without a running time waits for the next frame into
 * the queue, like force-idr (ours: VA alone forces the next frame it takes). Others go on. */
static GstPadProbeReturn
src_event_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstEvent *ev = GST_PAD_PROBE_INFO_EVENT (info);
  GstClockTime running_time;

  if (!gst_video_event_is_force_key_unit (ev) || GST_EVENT_TYPE (ev) != GST_EVENT_CUSTOM_UPSTREAM
      || !gst_video_event_parse_upstream_force_key_unit (ev, &running_time, NULL, NULL)
      || GST_CLOCK_TIME_IS_VALID (running_time))
    return GST_PAD_PROBE_OK;
  GST_DEBUG_OBJECT (user_data, "force-key-unit from downstream: for the next frame into the queue");
  gst_rds_enc_bin_request_key_unit (GST_RDS_ENC_BIN (user_data), ev);
  GST_PAD_PROBE_INFO_DATA (info) = NULL;
  return GST_PAD_PROBE_HANDLED;
}

void
gst_rds_enc_bin_send_key_units (GstRdsEncBin * self, GstPad * va_sink, GstBuffer * buf)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  GList *now;

  g_mutex_lock (&priv->probe_lock);
  now = rds_enc_key_units_take_due (&priv->key_units);
  g_mutex_unlock (&priv->probe_lock);
  if (now != NULL) {
    /* VA takes an upstream force-key-unit event at once (it notes it for its next frame) and holds no lock of its
     * pads' streams for it, so sending it from this probe, before VA's chain runs, is safe */
    GstElement *encoder = gst_pad_get_parent_element (va_sink);
    GstPad *src = encoder != NULL ? gst_element_get_static_pad (encoder, "src") : NULL;

    for (GList * l = now; l != NULL; l = l->next) {
      GST_DEBUG_OBJECT (self, "frame with PTS %" GST_TIME_FORMAT ": forcing a key frame",
          GST_TIME_ARGS (GST_BUFFER_PTS (buf)));
      if (src != NULL && !gst_pad_send_event (src, gst_event_ref (l->data)))
        GST_DEBUG_OBJECT (self, "the VA encoder refused the force-key-unit event");
    }
    g_list_free_full (now, (GDestroyNotify) gst_event_unref);
    gst_clear_object (&src);
    gst_clear_object (&encoder);
  }
}

/* ---- the convert's output pool ---- */

/* The VA encoder's answer to the ALLOCATION query (from the inner rdsvideoconvert, through the queue) loses its pools
 * and allocators and keeps its metas (GstVideoMeta): the convert then downloads into a plain system-memory pool of its
 * own (a pool entry without a pool), and VA copies each frame into one of its surfaces itself. Writing the download
 * straight into VA's mapped surfaces, as the convert does when offered VA's pool, costs several times more per frame
 * (ours).
 *
 * Only at the run's first size, though. VA makes the surface pool it copies plain frames into once per start, at the
 * first size it copies, and keeps it through caps changes (gst-plugins-bad 1.28's va plugin): after a resolution
 * change its copy would use the old size (a critical and a lost frame when it shrinks, a partial frame when it grows).
 * At any other size VA's answer stays as it is, so the convert writes into VA's own surfaces of the new size, which VA
 * takes without a copy; back at the first size, plain memory again. /gpu/enc/resize-* fails if this goes wrong. */
static GstPadProbeReturn
allocation_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GstRdsEncBinPrivate *priv = PRIV (GST_RDS_ENC_BIN (user_data));
  GstQuery *q = GST_PAD_PROBE_INFO_QUERY (info);
  GstCaps *caps = NULL;
  const GstStructure *st;
  VideoSize v = { 0 };
  gboolean plain;
  guint size = 0;

  if (GST_QUERY_TYPE (q) != GST_QUERY_ALLOCATION)
    return GST_PAD_PROBE_OK;
  gst_query_parse_allocation (q, &caps, NULL);
  if (caps == NULL || gst_caps_is_empty (caps))
    return GST_PAD_PROBE_OK;
  st = gst_caps_get_structure (caps, 0);
  (void) gst_structure_get_int (st, "width", &v.width);
  (void) gst_structure_get_int (st, "height", &v.height);
  g_mutex_lock (&priv->probe_lock);
  if (priv->copy_size.width == 0)
    priv->copy_size = v;
  plain = v.width == priv->copy_size.width && v.height == priv->copy_size.height;
  g_mutex_unlock (&priv->probe_lock);
  if (!plain) {
    GST_DEBUG_OBJECT (user_data, "%dx%d is not the run's first size: the convert keeps the VA encoder's pool",
        v.width, v.height);
    return GST_PAD_PROBE_OK;
  }
  GST_DEBUG_OBJECT (user_data, "the VA encoder's allocation answer %" GST_PTR_FORMAT " without pools or allocators",
      q);
  if (gst_query_get_n_allocation_pools (q) > 0)
    gst_query_parse_nth_allocation_pool (q, 0, NULL, &size, NULL, NULL);
  while (gst_query_get_n_allocation_pools (q) > 0)
    gst_query_remove_nth_allocation_pool (q, 0);
  while (gst_query_get_n_allocation_params (q) > 0)
    gst_query_remove_nth_allocation_param (q, 0);
  /* no pool, only its size: enough buffers made up front for the queue, the frame VA copies and the one the convert
   * writes, so the pool doesn't grow mid-stream; no maximum, so the convert never waits for one */
  gst_query_add_allocation_pool (q, NULL, size, VA_QUEUE_FRAMES + 2, 0);
  return GST_PAD_PROBE_OK;
}

/* ---- adding them ---- */

void
gst_rds_enc_bin_add_sched_probes (GstRdsEncBin * self, GstElement * queue, GstElement * encoder)
{
  GstRdsEncBinPrivate *priv = PRIV (self);
  GstPad *pad;

  pad = gst_element_get_static_pad (encoder, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_QUERY_DOWNSTREAM | GST_PAD_PROBE_TYPE_PULL, allocation_probe, self,
      NULL);
  gst_object_unref (pad);
  pad = gst_element_get_static_pad (queue, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST | GST_PAD_PROBE_TYPE_EVENT_FLUSH,
      queue_probe, self, NULL);
  gst_object_unref (pad);
  gst_pad_add_probe (priv->src_ghost, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM, src_event_probe, self, NULL);
}
