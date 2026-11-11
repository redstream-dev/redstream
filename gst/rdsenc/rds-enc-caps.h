/* Redstream: the caps decisions of rdsh264enc and rdsh265enc (plugin-internal, not installed): the preferred profile
 * and the Baseline + B frames rule on the VA encoder's downstream caps answer, and the downstream level check. Plain
 * caps in, caps or a verdict out, no element code, so the cpu tests check them on plain caps.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ENC_CAPS_H
#define RDS_ENC_CAPS_H

#include <gst/gst.h>

G_BEGIN_DECLS

/* The answer to give the VA encoder for its downstream caps query, whose answer from downstream is @answer (structures
 * named @caps_name). VA takes the highest profile the answer allows, whatever the order of a list, so a preference
 * is expressed by narrowing:
 * - to the profile @preferred when that leaves something (downstream allows it); otherwise @answer stays, so
 *   downstream caps that fix another profile win (DeepStream 8.0 behaviour);
 * - then, when @b_frames > 0 and the result allows a Baseline profile and another one, the Baseline ones are left
 *   out (ours: B frames need a non-Baseline profile, and VA could otherwise fall back to Baseline).
 * *@only_baseline (may be NULL) is set when the result allows Baseline profiles only, so with @b_frames > 0 the
 * caller turns B frames off. The profile @preferred wins over the Baseline removal: a preferred Baseline that
 * downstream allows stays Baseline. Returns a new reference (an unchanged answer is @answer with a reference
 * added, so @answer is const only in that it is not changed). */
G_GNUC_INTERNAL GstCaps *rds_enc_caps_narrow (const GstCaps * answer, const gchar * caps_name, const gchar * preferred,
    guint b_frames, gboolean * only_baseline);

/* Whether the encoder's output @caps are refused by the downstream caps @allowed only because of `level`: @caps
 * carry a level, don't intersect @allowed, and do once the level is removed from @caps. Then *@wanted (g_free) is
 * downstream's level, serialized (a string, or a list); otherwise FALSE and *@wanted untouched. */
G_GNUC_INTERNAL gboolean rds_enc_caps_level_refused (const GstCaps * caps, const GstCaps * allowed,
    gchar ** wanted);

G_END_DECLS

#endif /* RDS_ENC_CAPS_H */
