/* Redstream: per-source stream events (D9: real GstEventType numbers on custom events).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-event.h"

#define FIELD_SOURCE_ID "source-id"
#define FIELD_SEGMENT "segment"
#define FIELD_STREAM_ID "stream-id"

static GstEvent *
new_event (RdsEventType type, const gchar * name, guint source_id)
{
  return gst_event_new_custom ((GstEventType) type,
      gst_structure_new (name, FIELD_SOURCE_ID, G_TYPE_UINT, source_id, NULL));
}

/* FALSE, quietly, for any event that is not ours of this type: probes see every event. */
static gboolean
parse_source_id (GstEvent * event, RdsEventType type, guint * source_id)
{
  const GstStructure *s;
  guint id;

  g_return_val_if_fail (GST_IS_EVENT (event), FALSE);

  if ((guint) GST_EVENT_TYPE (event) != (guint) type)
    return FALSE;
  s = gst_event_get_structure (event);
  if (s == NULL || !gst_structure_get_uint (s, FIELD_SOURCE_ID, &id))
    return FALSE;
  if (source_id)
    *source_id = id;
  return TRUE;
}

GstEvent *
rds_event_new_pad_added (guint source_id)
{
  return new_event (RDS_EVENT_PAD_ADDED, "rds-pad-added", source_id);
}

GstEvent *
rds_event_new_pad_deleted (guint source_id)
{
  return new_event (RDS_EVENT_PAD_DELETED, "rds-pad-deleted", source_id);
}

GstEvent *
rds_event_new_stream_eos (guint source_id)
{
  return new_event (RDS_EVENT_STREAM_EOS, "rds-stream-eos", source_id);
}

GstEvent *
rds_event_new_stream_segment (guint source_id, const GstSegment * segment)
{
  GstEvent *event;

  g_return_val_if_fail (segment != NULL, NULL);

  event = new_event (RDS_EVENT_STREAM_SEGMENT, "rds-stream-segment", source_id);
  gst_structure_set (gst_event_writable_structure (event), FIELD_SEGMENT, GST_TYPE_SEGMENT, segment, NULL);
  return event;
}

GstEvent *
rds_event_new_stream_reset (guint source_id)
{
  return new_event (RDS_EVENT_STREAM_RESET, "rds-stream-reset", source_id);
}

GstEvent *
rds_event_new_stream_start (guint source_id, const gchar * stream_id)
{
  GstEvent *event = new_event (RDS_EVENT_STREAM_START, "rds-stream-start", source_id);

  gst_structure_set (gst_event_writable_structure (event), FIELD_STREAM_ID, G_TYPE_STRING, stream_id, NULL);
  return event;
}

gboolean
rds_event_parse_pad_added (GstEvent * event, guint * source_id)
{
  return parse_source_id (event, RDS_EVENT_PAD_ADDED, source_id);
}

gboolean
rds_event_parse_pad_deleted (GstEvent * event, guint * source_id)
{
  return parse_source_id (event, RDS_EVENT_PAD_DELETED, source_id);
}

gboolean
rds_event_parse_stream_eos (GstEvent * event, guint * source_id)
{
  return parse_source_id (event, RDS_EVENT_STREAM_EOS, source_id);
}

gboolean
rds_event_parse_stream_segment (GstEvent * event, guint * source_id, GstSegment ** segment)
{
  const GValue *value;
  guint id;

  if (!parse_source_id (event, RDS_EVENT_STREAM_SEGMENT, &id))
    return FALSE;
  value = gst_structure_get_value (gst_event_get_structure (event), FIELD_SEGMENT);
  if (value == NULL || !G_VALUE_HOLDS (value, GST_TYPE_SEGMENT))
    return FALSE;
  if (source_id)
    *source_id = id;
  if (segment)
    *segment = (GstSegment *) g_value_get_boxed (value);
  return TRUE;
}

gboolean
rds_event_parse_stream_reset (GstEvent * event, guint * source_id)
{
  return parse_source_id (event, RDS_EVENT_STREAM_RESET, source_id);
}

gboolean
rds_event_parse_stream_start (GstEvent * event, guint * source_id, gchar ** stream_id)
{
  const GstStructure *s;
  guint id;

  if (!parse_source_id (event, RDS_EVENT_STREAM_START, &id))
    return FALSE;
  s = gst_event_get_structure (event);
  if (!gst_structure_has_field_typed (s, FIELD_STREAM_ID, G_TYPE_STRING))
    return FALSE;
  if (source_id)
    *source_id = id;
  if (stream_id)
    *stream_id = (gchar *) gst_structure_get_string (s, FIELD_STREAM_ID);
  return TRUE;
}
