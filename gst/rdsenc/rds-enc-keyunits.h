/* Redstream: the key-frame requests of rdsh264enc and rdsh265enc (plugin-internal, not installed). The bins run the VA
 * encoder behind a queue; a request for "the next frame" (force-idr, force-intra, an upstream force-key-unit event
 * without a running time) must land on the next frame into that queue, not on the oldest frame already in it, which
 * is what VA alone would force. This keeps the requests and counts the frames on both sides of the queue: no element
 * code and no locking (the caller serialises the calls), so the cpu tests check it on its own.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ENC_KEYUNITS_H
#define RDS_ENC_KEYUNITS_H

#include <gst/gst.h>

G_BEGIN_DECLS

typedef struct
{
  GQueue waiting;               /* GstEvent *: requests not yet given a frame, oldest first */
  GQueue due;                   /* (internal) the same, given the frame they apply to, oldest first */
  guint64 frames_in;            /* frames into the queue since the init, the last flush or clear */
  guint64 frames_out;           /* frames out of it, counted the same way */
} RdsEncKeyUnits;

G_GNUC_INTERNAL void rds_enc_key_units_init (RdsEncKeyUnits * k);
/* A request (@event, taken) for the next frame into the queue. */
G_GNUC_INTERNAL void rds_enc_key_units_request (RdsEncKeyUnits * k, GstEvent * event);
/* @n frames went into the queue (a buffer, 1, or a buffer list, its length): the waiting requests go to the first. */
G_GNUC_INTERNAL void rds_enc_key_units_frames_in (RdsEncKeyUnits * k, guint n);
/* The next frame comes out of the queue: the requests given to it (GstEvent *, owned by the caller), oldest first,
 * or NULL. */
G_GNUC_INTERNAL GList *rds_enc_key_units_take_due (RdsEncKeyUnits * k);
/* The queue was flushed (its frames dropped): the requests given to those frames wait again, in front of the ones
 * still waiting and in their order, for the first frame after the flush; both counts restart. */
G_GNUC_INTERNAL void rds_enc_key_units_flush (RdsEncKeyUnits * k);
/* Drops every request and restarts the counts; returns the dropped events (owned by the caller, so they can be
 * freed after a lock is dropped), oldest first, or NULL. */
G_GNUC_INTERNAL GList *rds_enc_key_units_clear (RdsEncKeyUnits * k);

G_END_DECLS

#endif /* RDS_ENC_KEYUNITS_H */
