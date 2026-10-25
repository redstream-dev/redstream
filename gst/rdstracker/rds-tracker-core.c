/* Redstream: the rdstracker tracking core. Per-stream target lists in a hash table keyed by stream ID; one ID counter
 * shared by all streams; greedy association. The IOU tracker associates in one pass against each target's last matched
 * box; NvSORT predicts every target with a Kalman filter and associates in three stages (the cascade).
 * Comments say "DeepStream 8.0 behaviour" for what matches observed DeepStream 8.0 behaviour and "ours" for our own
 * choices.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tracker-core.h"
#include "rds-tracker-kf.h"

#include <math.h>

/* DeepStream 8.0 behaviour: the IOU tracker (and NvSORT) report a constant tracker confidence. */
#define TRACKER_CONFIDENCE 0.5f

/* Ours: when minMatchingScore4Iou, minMatchingScore4SizeSimilarity and minMatchingScore4Overall are all 0, a
 * target-detection pair also needs an IOU above this. Without it every pair would be a candidate (the size
 * similarity of two boxes is never 0), and with a size weight a target could jump to a disjoint detection of a
 * similar size. With any minimum set, the configured filters alone decide. */
#define UNCONSTRAINED_MIN_IOU 0.0f

typedef enum
{
  STATE_TENTATIVE,
  STATE_ACTIVE,
  STATE_INACTIVE,
} TargetState;

typedef struct
{
  guint64 serial;               /* creation order, core-wide: the greedy tie-break */
  guint64 activation;           /* activation order, core-wide: newest ID first on frames without inference */
  guint64 id;                   /* the reported ID, set at activation */
  TargetState state;
  guint age;                    /* 1 on the creation frame, +1 on every later frame of the stream */
  guint shadow_age;             /* consecutive frames with inference without a match */
  gfloat x, y, w, h;            /* the last matched box */
  /* the box association and the duplicate check use on this frame: NvSORT's prediction with usePrediction4Assoc,
   * else the last matched box; the detection's box once matched */
  gfloat bx, by, bw, bh;
  RdsTrackerKf kf;              /* NvSORT with a state estimator only */
  gint class_id;
  gpointer last_user;
  gint det;                     /* the detection matched on this frame, or -1 */
} Target;

typedef struct _Stream Stream;

struct _Stream
{
  guint64 stream_id;            /* the hash table key points here */
  RdsTrackerCore *core;
  GPtrArray *targets;           /* Target *, in creation order */
  gboolean has_base;
  guint32 base;                 /* per-stream ID modes: the counter at the stream's first activation */
};

typedef struct
{
  gfloat score;
  guint64 serial;
  guint det;
  Target *target;
} Candidate;

typedef struct
{
  guint first_target, n_targets, first_rejected, n_rejected;
} FrameSpan;

struct _RdsTrackerCore
{
  RdsTrackerType tracker_type;
  gfloat min_detector_confidence, min_iou_diff4new_target;
  gboolean check_class_match;
  guint max_targets_per_stream, probation_age, max_shadow_tracking_age, early_termination_age;
  gfloat min_score_overall, min_score_size, min_score_iou, weight_size, weight_iou;
  gboolean need_overlap;        /* every minimum is 0: UNCONSTRAINED_MIN_IOU applies */
  gboolean cascade;             /* NvSORT: three association stages, tentative detections create no target */
  gfloat tentative_detector_confidence, min_score_tentative_iou;
  gboolean has_kf, use_prediction;
  RdsTrackerKfParams kf_params;

  RdsTrackerCoreOptions opts;
  guint32 upper;                /* useUniqueID's upper half, 0 when off */
  guint32 counter;              /* the next ID's lower half, shared by all streams */
  guint64 n_created, n_activated;
  GHashTable *streams;          /* &Stream.stream_id -> Stream */

  /* scratch, reused from frame to frame */
  GArray *candidates;           /* Candidate */
  GArray *valid;                /* gboolean per detection: a finite box with an area, above minDetectorConfidence */
  GArray *stage_dets;           /* gboolean per detection, twice: NvSORT's confirmed and tentative detections */
  GPtrArray *det_target;        /* per detection: the target it matched or created, or NULL */
  GPtrArray *sorted;            /* Target *, frames without inference */

  /* the last batch's output */
  GArray *spans;                /* FrameSpan, while a batch is built */
  GArray *frames_out;           /* RdsTrackerFrameOut */
  GArray *targets_out;          /* RdsTrackerTarget */
  GPtrArray *rejected_out;
};

static gboolean
reset_terminates (const RdsTrackerCore * core)
{
  return core->opts.reset_mode == RDS_TRACKER_ID_RESET_ON_RESET || core->opts.reset_mode == RDS_TRACKER_ID_RESET_BOTH;
}

/* Modes 2 and 3: per-stream IDs (counter − base), restarted at 0 by a stream EOS. IDs are then not unique across
 * streams; that is DeepStream 8.0 behaviour and documented as such. */
static gboolean
per_stream_ids (const RdsTrackerCore * core)
{
  return core->opts.reset_mode == RDS_TRACKER_ID_RESET_ON_EOS || core->opts.reset_mode == RDS_TRACKER_ID_RESET_BOTH;
}

static void
target_free (RdsTrackerCore * core, Target * t)
{
  if (t->last_user && core->opts.user_copy && core->opts.user_free)
    core->opts.user_free (t->last_user);
  g_free (t);
}

static void
target_set_last_user (RdsTrackerCore * core, Target * t, gpointer user)
{
  if (!core->opts.user_copy) {
    t->last_user = user;
    return;
  }
  if (t->last_user && core->opts.user_free)
    core->opts.user_free (t->last_user);
  t->last_user = core->opts.user_copy (user, core->opts.user_copy_data);
}

static void
stream_terminate_all (Stream * st)
{
  for (guint i = 0; i < st->targets->len; i++)
    target_free (st->core, g_ptr_array_index (st->targets, i));
  g_ptr_array_set_size (st->targets, 0);
}

static void
stream_free (gpointer data)
{
  Stream *st = data;

  stream_terminate_all (st);
  g_ptr_array_unref (st->targets);
  g_free (st);
}

static Stream *
get_stream (RdsTrackerCore * core, guint64 stream_id, gboolean create)
{
  Stream *st = g_hash_table_lookup (core->streams, &stream_id);

  if (!st && create) {
    st = g_new0 (Stream, 1);
    st->stream_id = stream_id;
    st->core = core;
    st->targets = g_ptr_array_new ();
    g_hash_table_insert (core->streams, &st->stream_id, st);
  }
  return st;
}

RdsTrackerCore *
rds_tracker_core_new (const RdsTrackerConfig * cfg, const RdsTrackerCoreOptions * opts)
{
  RdsTrackerCore *core;

  g_return_val_if_fail (cfg != NULL, NULL);
  g_return_val_if_fail (!opts || !opts->user_copy || opts->user_free, NULL);

  core = g_new0 (RdsTrackerCore, 1);
  core->tracker_type = cfg->tracker_type;
  core->min_detector_confidence = cfg->min_detector_confidence;
  core->min_iou_diff4new_target = cfg->min_iou_diff4new_target;
  core->check_class_match = cfg->check_class_match;
  core->max_targets_per_stream = cfg->max_targets_per_stream;
  core->probation_age = cfg->probation_age;
  core->max_shadow_tracking_age = cfg->max_shadow_tracking_age;
  core->early_termination_age = cfg->early_termination_age;
  core->min_score_overall = cfg->min_matching_score4overall;
  core->min_score_size = cfg->min_matching_score4size_similarity;
  core->min_score_iou = cfg->min_matching_score4iou;
  core->weight_size = cfg->matching_score_weight4size_similarity;
  core->weight_iou = cfg->matching_score_weight4iou;
  core->need_overlap = core->min_score_overall <= 0 && core->min_score_size <= 0 && core->min_score_iou <= 0;
  core->cascade = cfg->tracker_type == RDS_TRACKER_TYPE_NVSORT;
  core->tentative_detector_confidence = cfg->tentative_detector_confidence;
  core->min_score_tentative_iou = cfg->min_matching_score4tentative_iou;
  core->has_kf = core->cascade && rds_tracker_kf_params_from_config (&core->kf_params, cfg);
  core->use_prediction = core->has_kf && cfg->use_prediction4assoc;
  if (opts)
    core->opts = *opts;
  else
    core->opts.reset_mode = RDS_TRACKER_ID_RESET_ON_RESET;
  /* DeepStream 8.0 behaviour: one random upper half per tracker instance, shared by every stream. */
  core->upper = cfg->use_unique_id ? g_random_int () : 0;
  core->streams = g_hash_table_new_full (g_int64_hash, g_int64_equal, NULL, stream_free);
  core->candidates = g_array_new (FALSE, FALSE, sizeof (Candidate));
  core->valid = g_array_new (FALSE, FALSE, sizeof (gboolean));
  core->stage_dets = g_array_new (FALSE, FALSE, sizeof (gboolean));
  core->det_target = g_ptr_array_new ();
  core->sorted = g_ptr_array_new ();
  core->spans = g_array_new (FALSE, FALSE, sizeof (FrameSpan));
  core->frames_out = g_array_new (FALSE, TRUE, sizeof (RdsTrackerFrameOut));
  core->targets_out = g_array_new (FALSE, FALSE, sizeof (RdsTrackerTarget));
  core->rejected_out = g_ptr_array_new ();
  return core;
}

void
rds_tracker_core_free (RdsTrackerCore * core)
{
  if (!core)
    return;
  g_hash_table_unref (core->streams);
  g_array_unref (core->candidates);
  g_array_unref (core->valid);
  g_array_unref (core->stage_dets);
  g_ptr_array_unref (core->det_target);
  g_ptr_array_unref (core->sorted);
  g_array_unref (core->spans);
  g_array_unref (core->frames_out);
  g_array_unref (core->targets_out);
  g_ptr_array_unref (core->rejected_out);
  g_free (core);
}

static gfloat
box_iou (gfloat ax, gfloat ay, gfloat aw, gfloat ah, gfloat bx, gfloat by, gfloat bw, gfloat bh)
{
  gfloat iw = MIN (ax + aw, bx + bw) - MAX (ax, bx);
  gfloat ih = MIN (ay + ah, by + bh) - MAX (ay, by);
  gfloat inter, uni;

  if (iw <= 0 || ih <= 0)
    return 0;
  inter = iw * ih;
  uni = aw * ah + bw * bh - inter;
  return uni > 0 ? inter / uni : 0;
}

/* DeepStream 8.0 behaviour: the ratio of the smaller area to the larger, not a per-axis comparison. Both boxes are
 * valid: detections are checked (det_box_valid) and target boxes too (target_predicted_box). */
static gfloat
size_similarity (gfloat aw, gfloat ah, gfloat bw, gfloat bh)
{
  gfloat a = aw * ah, b = bw * bh;
  gfloat hi = MAX (a, b);

  g_assert (aw > 0 && ah > 0 && bw > 0 && bh > 0);

  return hi > 0 ? MIN (a, b) / hi : 0;
}

static gint
candidate_cmp (gconstpointer pa, gconstpointer pb)
{
  const Candidate *a = pa, *b = pb;

  if (a->score != b->score)
    return a->score > b->score ? -1 : 1;
  if (a->serial != b->serial)
    return a->serial < b->serial ? -1 : 1;
  return a->det < b->det ? -1 : a->det > b->det;
}

#define STATE_BIT(s) (1u << (s))
#define ALL_STATES (STATE_BIT (STATE_TENTATIVE) | STATE_BIT (STATE_ACTIVE) | STATE_BIT (STATE_INACTIVE))

/* One greedy pass (ours: the order of equal scores is not documented): candidates by score, best first, then the
 * older target, then the lower detection index; a pair is taken when both sides are still free. It pairs the
 * detections flagged in @use with the targets in @states, both still unmatched, on their association boxes. @joint:
 * the weighted score with every minimum; otherwise IOU only, at least minMatchingScore4TentativeIou (NvSORT's
 * tentative stages). Sets Target.det and core->det_target. */
static void
associate_stage (RdsTrackerCore * core, Stream * st, const RdsTrackerDet * dets, guint n_dets, const gboolean * use,
    guint states, gboolean joint)
{
  GArray *cands = core->candidates;
  gboolean need_overlap = joint ? core->need_overlap : core->min_score_tentative_iou <= 0;

  g_array_set_size (cands, 0);
  for (guint ti = 0; ti < st->targets->len; ti++) {
    Target *t = g_ptr_array_index (st->targets, ti);

    if (t->det >= 0 || !(states & STATE_BIT (t->state)))
      continue;
    for (guint i = 0; i < n_dets; i++) {
      const RdsTrackerDet *d = &dets[i];
      gfloat iou, sim, score;
      Candidate c;

      if (!use[i] || g_ptr_array_index (core->det_target, i)
          || (core->check_class_match && d->class_id != t->class_id))
        continue;
      iou = box_iou (t->bx, t->by, t->bw, t->bh, d->x, d->y, d->w, d->h);
      if (need_overlap && iou <= UNCONSTRAINED_MIN_IOU)
        continue;
      if (joint) {
        if (iou < core->min_score_iou)
          continue;
        sim = size_similarity (t->bw, t->bh, d->w, d->h);
        if (sim < core->min_score_size)
          continue;
        score = core->weight_iou * iou + core->weight_size * sim;
        /* valid boxes and weights in [0, 1] give a finite score; candidate_cmp needs one */
        g_assert (!isnan (score));
        if (score < core->min_score_overall)
          continue;
      } else {
        if (iou < core->min_score_tentative_iou)
          continue;
        score = iou;
      }
      c.score = score;
      c.serial = t->serial;
      c.det = i;
      c.target = t;
      g_array_append_val (cands, c);
    }
  }
  g_array_sort (cands, candidate_cmp);
  for (guint k = 0; k < cands->len; k++) {
    Candidate *c = &g_array_index (cands, Candidate, k);

    if (c->target->det >= 0 || g_ptr_array_index (core->det_target, c->det))
      continue;
    c->target->det = c->det;
    g_ptr_array_index (core->det_target, c->det) = c->target;
  }
}

/* The IOU tracker: one joint pass over every target, whatever its state. NvSORT's cascade (DeepStream's documented
 * stages): confirmed detections (confidence >= tentativeDetectorConfidence) with the Active and Inactive targets on
 * the joint score, then tentative detections (below it) with the remaining Active targets, then the remaining
 * confirmed detections with the Tentative targets, both on IOU only. */
static void
associate (RdsTrackerCore * core, Stream * st, const RdsTrackerDet * dets, guint n_dets, const gboolean * valid)
{
  gboolean *confirmed, *tentative;

  if (!core->cascade) {
    associate_stage (core, st, dets, n_dets, valid, ALL_STATES, TRUE);
    return;
  }
  g_array_set_size (core->stage_dets, 2 * n_dets);
  confirmed = (gboolean *) core->stage_dets->data;
  tentative = confirmed + n_dets;
  for (guint i = 0; i < n_dets; i++) {
    confirmed[i] = valid[i] && dets[i].confidence >= core->tentative_detector_confidence;
    tentative[i] = valid[i] && !confirmed[i];
  }
  associate_stage (core, st, dets, n_dets, confirmed, STATE_BIT (STATE_ACTIVE) | STATE_BIT (STATE_INACTIVE), TRUE);
  associate_stage (core, st, dets, n_dets, tentative, STATE_BIT (STATE_ACTIVE), FALSE);
  associate_stage (core, st, dets, n_dets, confirmed, STATE_BIT (STATE_TENTATIVE), FALSE);
}

/* Whether an unmatched detection may start a target, before the duplicate and cap checks. The IOU tracker creates
 * targets from any detection that passed minDetectorConfidence. DeepStream 8.0 behaviour: NvSORT never creates one
 * from a tentative detection (below tentativeDetectorConfidence); unmatched, it is rejected. */
static gboolean
may_create_target (RdsTrackerCore * core, const RdsTrackerDet * d)
{
  return !core->cascade || d->confidence >= core->tentative_detector_confidence;
}

/* Ours: a detection the matcher may see: finite coordinates, a positive size, and a finite far corner and area.
 * Anything else could make a score NaN, so it is rejected before association. */
static gboolean
det_box_valid (const RdsTrackerDet * d)
{
  return isfinite (d->x) && isfinite (d->y) && isfinite (d->w) && isfinite (d->h) && d->w > 0 && d->h > 0
      && isfinite (d->x + d->w) && isfinite (d->y + d->h) && isfinite (d->w * d->h);
}

/* DeepStream 8.0 behaviour: the duplicate check compares with every target of the stream, ignoring class and
 * state. Ours: that includes targets created earlier on the same frame, and it uses each target's box of this frame
 * (Target.bx...: for NvSORT, the matched detection or the association box). */
static gfloat
max_iou_with_targets (Stream * st, const RdsTrackerDet * d)
{
  gfloat best = 0;

  for (guint i = 0; i < st->targets->len; i++) {
    Target *t = g_ptr_array_index (st->targets, i);

    best = MAX (best, box_iou (t->bx, t->by, t->bw, t->bh, d->x, d->y, d->w, d->h));
  }
  return best;
}

static void
activate (RdsTrackerCore * core, Stream * st, Target * t)
{
  guint32 lower = core->counter;

  if (per_stream_ids (core)) {
    if (!st->has_base) {
      st->base = core->counter;
      st->has_base = TRUE;
    }
    lower -= st->base;
  }
  t->state = STATE_ACTIVE;
  t->id = ((guint64) core->upper << 32) | lower;
  t->activation = core->n_activated++;
  core->counter++;
}

/* The Kalman prediction of @t, or (ours) its last matched box when the prediction is not a finite box with an area. */
static void
target_predicted_box (const Target * t, gfloat * x, gfloat * y, gfloat * w, gfloat * h)
{
  rds_tracker_kf_get_box (&t->kf, x, y, w, h);
  if (isfinite (*x) && isfinite (*y) && isfinite (*w) && isfinite (*h) && *w > 0 && *h > 0
      && isfinite (*x + *w) && isfinite (*y + *h) && isfinite (*w * *h))
    return;
  *x = t->x;
  *y = t->y;
  *w = t->w;
  *h = t->h;
}

static void
target_set_box (Target * t, const RdsTrackerDet * d)
{
  t->x = t->bx = d->x;
  t->y = t->by = d->y;
  t->w = t->bw = d->w;
  t->h = t->bh = d->h;
}

/* DeepStream 8.0 behaviour: a matched target is reported with its detection's box, also by NvSORT (whose filter still
 * takes the detection); on a frame without inference the IOU tracker reports the last matched box and NvSORT the
 * prediction. */
static void
emit_target (RdsTrackerCore * core, const Target * t, gboolean matched, gpointer associated_user)
{
  RdsTrackerTarget o = {
    .id = t->id,.x = t->x,.y = t->y,.w = t->w,.h = t->h,.class_id = t->class_id,
    .tracker_confidence = TRACKER_CONFIDENCE,.associated_user = associated_user,.last_user = t->last_user,
    .age = t->age,
  };

  if (!matched && core->has_kf)
    target_predicted_box (t, &o.x, &o.y, &o.w, &o.h);
  g_array_append_val (core->targets_out, o);
}

static gint
newest_first (gconstpointer pa, gconstpointer pb)
{
  const Target *a = *(Target * const *) pa, *b = *(Target * const *) pb;

  return a->activation > b->activation ? -1 : a->activation < b->activation;
}

/* A frame with inference: association, the life cycle, new targets, IDs, then the report in detection order. */
static void
process_inferred (RdsTrackerCore * core, Stream * st, const RdsTrackerFrameIn * in)
{
  gboolean *valid;
  guint i = 0;

  g_array_set_size (core->valid, in->n_dets);
  valid = (gboolean *) core->valid->data;
  for (guint k = 0; k < in->n_dets; k++)
    valid[k] = det_box_valid (&in->dets[k]) && in->dets[k].confidence >= core->min_detector_confidence;
  associate (core, st, in->dets, in->n_dets, valid);

  /* matched targets take the detection; unmatched ones age in shadow and may terminate */
  while (i < st->targets->len) {
    Target *t = g_ptr_array_index (st->targets, i);

    if (t->det >= 0) {
      const RdsTrackerDet *d = &in->dets[t->det];

      t->shadow_age = 0;
      target_set_box (t, d);
      if (core->has_kf)
        rds_tracker_kf_update (&t->kf, d->x, d->y, d->w, d->h);
      if (!core->check_class_match)
        t->class_id = d->class_id;
      target_set_last_user (core, t, d->user);
      if (t->state == STATE_INACTIVE)
        t->state = STATE_ACTIVE;
    } else {
      gboolean dead;

      t->shadow_age++;
      if (t->state == STATE_ACTIVE)
        t->state = STATE_INACTIVE;
      dead = t->shadow_age > core->max_shadow_tracking_age;
      if (t->state == STATE_TENTATIVE && t->shadow_age >= core->early_termination_age)
        dead = TRUE;
      if (dead) {
        target_free (core, t);
        g_ptr_array_remove_index (st->targets, i);
        continue;
      }
    }
    i++;
  }

  /* unmatched detections start Tentative targets */
  for (guint k = 0; k < in->n_dets; k++) {
    const RdsTrackerDet *d = &in->dets[k];
    Target *t;
    gfloat best;

    if (!valid[k] || g_ptr_array_index (core->det_target, k) || !may_create_target (core, d))
      continue;
    if (st->targets->len >= core->max_targets_per_stream)
      continue;
    /* Ours: only an overlapping detection can be a duplicate, so minIouDiff4NewTarget 0 doesn't block every new
     * target (the max IOU of a lone detection is 0). */
    best = max_iou_with_targets (st, d);
    if (best > 0 && best >= core->min_iou_diff4new_target)
      continue;
    t = g_new0 (Target, 1);
    t->serial = core->n_created++;
    t->state = STATE_TENTATIVE;
    t->age = 1;
    target_set_box (t, d);
    if (core->has_kf)
      rds_tracker_kf_init (&t->kf, &core->kf_params, d->x, d->y, d->w, d->h);
    t->class_id = d->class_id;
    t->det = k;
    target_set_last_user (core, t, d->user);
    g_ptr_array_add (st->targets, t);
    g_ptr_array_index (core->det_target, k) = t;
  }

  /* DeepStream 8.0 behaviour: a Tentative target activates, and takes its ID, on the first matched frame where its
   * age exceeds probationAge; activations take IDs in detection order. Then only the Active targets matched on this
   * frame are reported, with the detection's box; every other detection is rejected. */
  for (guint k = 0; k < in->n_dets; k++) {
    Target *t = g_ptr_array_index (core->det_target, k);

    if (t && t->state == STATE_TENTATIVE && t->age > core->probation_age)
      activate (core, st, t);
    if (t && t->state == STATE_ACTIVE)
      emit_target (core, t, TRUE, in->dets[k].user);
    else
      g_ptr_array_add (core->rejected_out, in->dets[k].user);
  }
}

/* A frame without inference: no association and no shadow aging; every Active target is reported (with its last
 * matched box, or NvSORT's prediction), newest ID first. Detections on such a frame are never tracked and are all
 * rejected, even one on an Active target's box (DeepStream 8.0 behaviour, and 7.1's: the objects are removed from
 * the frame). */
static void
process_uninferred (RdsTrackerCore * core, Stream * st, const RdsTrackerFrameIn * in)
{
  g_ptr_array_set_size (core->sorted, 0);
  for (guint i = 0; i < st->targets->len; i++) {
    Target *t = g_ptr_array_index (st->targets, i);

    if (t->state == STATE_ACTIVE)
      g_ptr_array_add (core->sorted, t);
  }
  g_ptr_array_sort (core->sorted, newest_first);
  for (guint i = 0; i < core->sorted->len; i++)
    emit_target (core, g_ptr_array_index (core->sorted, i), FALSE, NULL);
  for (guint k = 0; k < in->n_dets; k++)
    g_ptr_array_add (core->rejected_out, in->dets[k].user);
}

static gboolean
has_duplicate_streams (const RdsTrackerFrameIn * frames, guint n_frames)
{
  for (guint f = 1; f < n_frames; f++)
    for (guint g = 0; g < f; g++)
      if (frames[f].stream_id == frames[g].stream_id)
        return TRUE;
  return FALSE;
}

void
rds_tracker_core_process (RdsTrackerCore * core, const RdsTrackerFrameIn * frames, guint n_frames,
    RdsTrackerBatchOut * out)
{
  out->frames = NULL;
  out->n_frames = 0;
  g_return_if_fail (!has_duplicate_streams (frames, n_frames));
  g_array_set_size (core->spans, 0);
  g_array_set_size (core->targets_out, 0);
  g_ptr_array_set_size (core->rejected_out, 0);

  for (guint f = 0; f < n_frames; f++) {
    const RdsTrackerFrameIn *in = &frames[f];
    Stream *st = get_stream (core, in->stream_id, TRUE);
    FrameSpan span = {.first_target = core->targets_out->len,.first_rejected = core->rejected_out->len };

    if (in->reset && reset_terminates (core))
      stream_terminate_all (st);
    /* NvSORT predicts every target once per frame, with or without inference, before association */
    for (guint i = 0; i < st->targets->len; i++) {
      Target *t = g_ptr_array_index (st->targets, i);

      t->age++;
      t->det = -1;
      if (core->has_kf)
        rds_tracker_kf_predict (&t->kf);
      if (core->use_prediction) {
        target_predicted_box (t, &t->bx, &t->by, &t->bw, &t->bh);
      } else {
        t->bx = t->x;
        t->by = t->y;
        t->bw = t->w;
        t->bh = t->h;
      }
    }
    g_ptr_array_set_size (core->det_target, in->n_dets);
    for (guint k = 0; k < in->n_dets; k++)
      g_ptr_array_index (core->det_target, k) = NULL;

    if (in->detection_done)
      process_inferred (core, st, in);
    else
      process_uninferred (core, st, in);

    span.n_targets = core->targets_out->len - span.first_target;
    span.n_rejected = core->rejected_out->len - span.first_rejected;
    g_array_append_val (core->spans, span);
  }

  /* the output arrays are complete: point into them */
  g_array_set_size (core->frames_out, n_frames);
  for (guint f = 0; f < n_frames; f++) {
    const FrameSpan *span = &g_array_index (core->spans, FrameSpan, f);
    RdsTrackerFrameOut *fo = &g_array_index (core->frames_out, RdsTrackerFrameOut, f);

    fo->targets = span->n_targets ? &g_array_index (core->targets_out, RdsTrackerTarget, span->first_target) : NULL;
    fo->n_targets = span->n_targets;
    fo->rejected = span->n_rejected ? &g_ptr_array_index (core->rejected_out, span->first_rejected) : NULL;
    fo->n_rejected = span->n_rejected;
  }
  out->frames = (const RdsTrackerFrameOut *) core->frames_out->data;
  out->n_frames = n_frames;
}

void
rds_tracker_core_remove_stream (RdsTrackerCore * core, guint64 stream_id)
{
  g_hash_table_remove (core->streams, &stream_id);
}

/* Ours: the shared counter is never restarted by a reset, an EOS or a clear, so live IDs never repeat (DeepStream
 * 8.0 restarts it on reset and EOS, giving duplicate live IDs). */
void
rds_tracker_core_stream_reset (RdsTrackerCore * core, guint64 stream_id)
{
  Stream *st = get_stream (core, stream_id, FALSE);

  if (st && reset_terminates (core))
    stream_terminate_all (st);
}

void
rds_tracker_core_stream_eos (RdsTrackerCore * core, guint64 stream_id)
{
  Stream *st = get_stream (core, stream_id, FALSE);

  if (st && per_stream_ids (core)) {
    stream_terminate_all (st);
    st->has_base = FALSE;
  }
}

void
rds_tracker_core_clear (RdsTrackerCore * core)
{
  /* the counter continues: IDs after a flush don't repeat earlier ones */
  g_hash_table_remove_all (core->streams);
}

guint
rds_tracker_core_get_n_targets (RdsTrackerCore * core, guint64 stream_id)
{
  Stream *st = get_stream (core, stream_id, FALSE);

  return st ? st->targets->len : 0;
}

void
rds_tracker_core_set_unique_id_upper (RdsTrackerCore * core, guint32 upper)
{
  core->upper = upper;
}
