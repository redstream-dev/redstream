/* Redstream: the GstMeta that carries an RdsBatchMeta on a buffer.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-gst-meta.h"
#include "rds-meta-private.h"

#define GST_CAT_DEFAULT rds_meta_debug

GType
rds_meta_api_get_type (void)
{
  static gsize type_id = 0;

  if (g_once_init_enter (&type_id)) {
    /* No tags (D10): elements that keep only metas without content tags across transforms keep this one. */
    static const gchar *tags[] = { NULL };
    GType type = gst_meta_api_type_register ("RdsMetaAPI", tags);

    g_once_init_leave (&type_id, type);
  }
  return (GType) type_id;
}

static gboolean
rds_meta_init (GstMeta * meta, G_GNUC_UNUSED gpointer params, G_GNUC_UNUSED GstBuffer * buffer)
{
  RdsMeta *m = (RdsMeta *) meta;

  m->meta_data = NULL;
  m->user_data = NULL;
  m->meta_type = RDS_INVALID_META;
  return TRUE;
}

static void
rds_meta_free (GstMeta * meta, G_GNUC_UNUSED GstBuffer * buffer)
{
  RdsMeta *m = (RdsMeta *) meta;

  if (m->meta_type == RDS_BATCH_GST_META)
    rds_destroy_batch_meta (m->meta_data);
  m->meta_data = NULL;
}

/* A14: copies (gst_buffer_copy, make_writable) get their own deep copy, so writers never touch metadata another
 * buffer still shares. Every other transform drops the meta. */
static gboolean
rds_meta_transform (GstBuffer * dest, GstMeta * meta, G_GNUC_UNUSED GstBuffer * buffer, GQuark type,
    G_GNUC_UNUSED gpointer data)
{
  RdsMeta *m = (RdsMeta *) meta;
  RdsBatchMeta *copy;

  if (!GST_META_TRANSFORM_IS_COPY (type) || m->meta_type != RDS_BATCH_GST_META || m->meta_data == NULL)
    return FALSE;

  copy = rds_batch_meta_copy (m->meta_data);
  if (copy == NULL)
    return FALSE;
  if (rds_buffer_add_batch_meta (dest, copy) == NULL) {
    rds_destroy_batch_meta (copy);
    return FALSE;
  }
  return TRUE;
}

const GstMetaInfo *
rds_meta_get_info (void)
{
  static const GstMetaInfo *info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & info)) {
    const GstMetaInfo *mi = gst_meta_register (RDS_META_API_TYPE, "RdsMeta", sizeof (RdsMeta),
        rds_meta_init, rds_meta_free, rds_meta_transform);

    g_once_init_leave ((GstMetaInfo **) & info, (GstMetaInfo *) mi);
  }
  return info;
}

RdsMeta *
rds_buffer_add_batch_meta (GstBuffer * buffer, RdsBatchMeta * batch_meta)
{
  RdsMeta *m;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);
  g_return_val_if_fail (gst_buffer_is_writable (buffer), NULL);
  g_return_val_if_fail (batch_meta != NULL, NULL);

  m = (RdsMeta *) gst_buffer_add_meta (buffer, rds_meta_get_info (), NULL);
  if (m == NULL)
    return NULL;
  m->meta_data = batch_meta;
  m->meta_type = RDS_BATCH_GST_META;
  return m;
}

RdsBatchMeta *
gst_buffer_get_rds_batch_meta (GstBuffer * buffer)
{
  gpointer state = NULL;
  GstMeta *meta;

  g_return_val_if_fail (GST_IS_BUFFER (buffer), NULL);

  while ((meta = gst_buffer_iterate_meta_filtered (buffer, &state, RDS_META_API_TYPE)) != NULL) {
    RdsMeta *m = (RdsMeta *) meta;

    if (m->meta_type == RDS_BATCH_GST_META)
      return m->meta_data;
  }
  return NULL;
}
