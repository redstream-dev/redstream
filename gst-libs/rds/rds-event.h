/* Redstream: per-source stream events sent downstream by rdsstreammux and rdsurisrcbin
 * (counterpart of DeepStream's gst-nvevent; names from NVIDIA's public API reference).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_EVENT_H
#define RDS_EVENT_H

#include <gst/gst.h>
#include <rds/rds-prelude.h>

G_BEGIN_DECLS

#define RDS_EVENT_FLAGS (GST_EVENT_TYPE_DOWNSTREAM | GST_EVENT_TYPE_SERIALIZED)

/* Real GstEventType numbers, so apps can dispatch on GST_EVENT_TYPE (ev). Warning-free idioms:
 *   switch ((RdsEventType) GST_EVENT_TYPE (ev)) { case RDS_EVENT_PAD_ADDED: ... default: ... }
 *   if ((RdsEventType) GST_EVENT_TYPE (ev) == RDS_EVENT_STREAM_EOS) ...
 * An uncast switch on GstEventType with RDS_EVENT_* cases triggers -Wswitch ("case value not in
 * enumerated type"), and an uncast == triggers -Wenum-compare, as with DeepStream's event types. */
typedef enum
{
  RDS_EVENT_PAD_ADDED = GST_EVENT_MAKE_TYPE (500, RDS_EVENT_FLAGS),
  RDS_EVENT_PAD_DELETED = GST_EVENT_MAKE_TYPE (501, RDS_EVENT_FLAGS),
  RDS_EVENT_STREAM_EOS = GST_EVENT_MAKE_TYPE (502, RDS_EVENT_FLAGS),
  RDS_EVENT_STREAM_SEGMENT = GST_EVENT_MAKE_TYPE (503, RDS_EVENT_FLAGS),
  RDS_EVENT_STREAM_RESET = GST_EVENT_MAKE_TYPE (504, RDS_EVENT_FLAGS),
  RDS_EVENT_STREAM_START = GST_EVENT_MAKE_TYPE (505, RDS_EVENT_FLAGS),
} RdsEventType;

RDS_API GstEvent *rds_event_new_pad_added (guint source_id);
RDS_API GstEvent *rds_event_new_pad_deleted (guint source_id);
RDS_API GstEvent *rds_event_new_stream_eos (guint source_id);
RDS_API GstEvent *rds_event_new_stream_segment (guint source_id, const GstSegment * segment);
RDS_API GstEvent *rds_event_new_stream_reset (guint source_id);
/* stream_id may be NULL: rds_event_parse_stream_start then returns TRUE with *stream_id = NULL. */
RDS_API GstEvent *rds_event_new_stream_start (guint source_id, const gchar * stream_id);

/* Each returns FALSE (outputs untouched) if the event is not of that type, or is of that type but
 * lacks a field or has one of the wrong type (source-id, segment, stream-id). Outputs may be NULL.
 * segment and stream_id are transfer none: valid while the event is alive. */
RDS_API gboolean rds_event_parse_pad_added (GstEvent * event, guint * source_id);
RDS_API gboolean rds_event_parse_pad_deleted (GstEvent * event, guint * source_id);
RDS_API gboolean rds_event_parse_stream_eos (GstEvent * event, guint * source_id);
RDS_API gboolean rds_event_parse_stream_segment (GstEvent * event, guint * source_id, GstSegment ** segment);
RDS_API gboolean rds_event_parse_stream_reset (GstEvent * event, guint * source_id);
RDS_API gboolean rds_event_parse_stream_start (GstEvent * event, guint * source_id, gchar ** stream_id);

G_END_DECLS

#endif /* RDS_EVENT_H */
