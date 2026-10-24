/* Redstream: helpers shared by the rdsvideo elements.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-video-util.h"

#include <stddef.h>
#include <string.h>

gboolean
rds_video_frame_wrap_surface (GstVideoFrame * frame, RdsBufSurface * surf, RdsBufSurfaceParams * params)
{
  const GstVideoFormatInfo *finfo = frame->info.finfo;
  RdsBufSurfaceColorFormat fmt;
  RdsBufSurfacePlaneParams pp;
  const guint8 *base;
  guint64 end = 0;
  guint n_planes;

  g_return_val_if_fail (frame != NULL && surf != NULL && params != NULL, FALSE);

  fmt = rds_buf_color_format_from_video_format (GST_VIDEO_FRAME_FORMAT (frame));
  n_planes = GST_VIDEO_FRAME_N_PLANES (frame);
  if (fmt == RDS_BUF_COLOR_FORMAT_INVALID || n_planes == 0 || n_planes > 3)
    return FALSE;

  memset (&pp, 0, sizeof (pp));
  base = GST_VIDEO_FRAME_PLANE_DATA (frame, 0);
  pp.num_planes = n_planes;
  for (guint p = 0; p < n_planes; p++) {
    ptrdiff_t off = (const guint8 *) GST_VIDEO_FRAME_PLANE_DATA (frame, p) - base;
    gint stride = GST_VIDEO_FRAME_PLANE_STRIDE (frame, p);
    gint comp[GST_VIDEO_MAX_COMPONENTS];
    guint64 psize;

    gst_video_format_info_component (finfo, p, comp);
    if (off < 0 || (guint64) off > G_MAXUINT32 || stride <= 0 || comp[0] < 0)
      return FALSE;
    pp.offset[p] = (guint32) off;
    pp.pitch[p] = (guint32) stride;
    pp.width[p] = (guint32) GST_VIDEO_FRAME_COMP_WIDTH (frame, comp[0]);
    pp.height[p] = (guint32) GST_VIDEO_FRAME_COMP_HEIGHT (frame, comp[0]);
    pp.bytesPerPix[p] = (guint32) GST_VIDEO_FRAME_COMP_PSTRIDE (frame, comp[0]);
    psize = (guint64) pp.pitch[p] * pp.height[p];
    if (psize > G_MAXUINT32)
      return FALSE;
    pp.psize[p] = (guint32) psize;
    end = MAX (end, (guint64) off + psize);
  }
  if (end > G_MAXUINT32)
    return FALSE;

  memset (params, 0, sizeof (*params));
  params->width = GST_VIDEO_FRAME_WIDTH (frame);
  params->height = GST_VIDEO_FRAME_HEIGHT (frame);
  params->pitch = pp.pitch[0];
  params->colorFormat = fmt;
  params->layout = RDS_BUF_LAYOUT_PITCH;
  params->dataSize = (guint32) end;
  params->dataPtr = (gpointer) base;
  params->planeParams = pp;

  memset (surf, 0, sizeof (*surf));
  surf->batchSize = 1;
  surf->numFilled = 1;
  surf->isContiguous = TRUE;
  surf->memType = RDS_BUF_MEM_SYSTEM;
  surf->surfaceList = params;
  return TRUE;
}

gboolean
rds_video_parse_rect (const gchar * str, RdsBufSurfTransformRect * rect)
{
  guint64 v[4];
  gchar **fields;
  gboolean ok = TRUE;

  g_return_val_if_fail (rect != NULL, FALSE);

  if (str == NULL || *str == '\0') {
    memset (rect, 0, sizeof (*rect));
    return TRUE;
  }
  fields = g_strsplit (str, ":", -1);
  if (g_strv_length (fields) != 4)
    ok = FALSE;
  for (guint i = 0; ok && i < 4; i++) {
    const gchar *f = fields[i];

    /* digits only: no sign, no whitespace, not empty */
    for (const gchar * c = f; *c != '\0'; c++)
      if (!g_ascii_isdigit (*c))
        ok = FALSE;
    if (ok && !g_ascii_string_to_unsigned (f, 10, 0, G_MAXUINT32, &v[i], NULL))
      ok = FALSE;
  }
  g_strfreev (fields);
  if (!ok)
    return FALSE;

  rect->left = (guint32) v[0];
  rect->top = (guint32) v[1];
  rect->width = (guint32) v[2];
  rect->height = (guint32) v[3];
  return TRUE;
}
