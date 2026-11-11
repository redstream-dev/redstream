/* Redstream: the key-frame requests of rdsh264enc and rdsh265enc. See rds-enc-keyunits.h.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-keyunits.h"

typedef struct
{
  guint64 frame;
  GstEvent *event;
} Due;

void
rds_enc_key_units_init (RdsEncKeyUnits * k)
{
  g_queue_init (&k->waiting);
  g_queue_init (&k->due);
  k->frames_in = k->frames_out = 0;
}

void
rds_enc_key_units_request (RdsEncKeyUnits * k, GstEvent * event)
{
  g_queue_push_tail (&k->waiting, event);
}

void
rds_enc_key_units_frames_in (RdsEncKeyUnits * k, guint n)
{
  if (n == 0)
    return;
  while (!g_queue_is_empty (&k->waiting)) {
    Due *d = g_new (Due, 1);

    d->frame = k->frames_in;
    d->event = g_queue_pop_head (&k->waiting);
    g_queue_push_tail (&k->due, d);
  }
  k->frames_in += n;
}

GList *
rds_enc_key_units_take_due (RdsEncKeyUnits * k)
{
  guint64 frame = k->frames_out++;
  GList *now = NULL;
  Due *d;

  while ((d = g_queue_peek_head (&k->due)) != NULL && d->frame <= frame) {
    g_queue_pop_head (&k->due);
    now = g_list_prepend (now, d->event);
    g_free (d);
  }
  return g_list_reverse (now);
}

void
rds_enc_key_units_flush (RdsEncKeyUnits * k)
{
  Due *d;

  while ((d = g_queue_pop_tail (&k->due)) != NULL) {
    g_queue_push_head (&k->waiting, d->event);
    g_free (d);
  }
  k->frames_in = k->frames_out = 0;
}

GList *
rds_enc_key_units_clear (RdsEncKeyUnits * k)
{
  GList *dropped;

  /* the due ones are older than the waiting ones */
  rds_enc_key_units_flush (k);
  dropped = k->waiting.head;
  g_queue_init (&k->waiting);
  return dropped;
}
