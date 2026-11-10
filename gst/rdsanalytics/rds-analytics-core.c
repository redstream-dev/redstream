/* Redstream: the rdsanalytics core. Geometry: rule scaling, the reference point of a box and the inside test. The
 * per-stream state and the features: ROI filtering, overcrowding and the object counts.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-core.h"

#include <math.h>

/* A double to an int, clamped to the int range, NaN as 0 (ours: such inputs are undefined behaviour for a cast). */
static gint
to_int (gdouble v)
{
  if (isnan (v))
    return 0;
  return (gint) CLAMP (v, (gdouble) G_MININT, (gdouble) G_MAXINT);
}

/* DeepStream 8.0 behaviour: each vertex is scaled per axis and truncated to an int. The arithmetic is ours: 64-bit
 * integers, (int)(v * out / cfg), whose quotient truncates toward zero as the cast does, with no rounding or
 * overflow. It equals 8.0's float v * out / cfg while v * out < 2^24; whether 8.0 divides first is not known. */
static gint
scale_coord (gint v, guint cfg, guint out)
{
  gint64 s = (gint64) v * (gint64) out / (gint64) cfg;

  return (gint) CLAMP (s, (gint64) G_MININT, (gint64) G_MAXINT);
}

void
rds_analytics_scale_points (const RdsAnalyticsPoint * in, guint n, guint cfg_w, guint cfg_h, guint out_w,
    guint out_h, RdsAnalyticsPoint * out)
{
  g_return_if_fail (cfg_w > 0 && cfg_h > 0);
  g_return_if_fail (n == 0 || (in != NULL && out != NULL));

  for (guint i = 0; i < n; i++) {
    RdsAnalyticsPoint p = in[i];

    out[i].x = scale_coord (p.x, cfg_w, out_w);
    out[i].y = scale_coord (p.y, cfg_h, out_h);
  }
}

/* DeepStream 8.0 behaviour: the bottom centre, floored to integers. The sums are in float, the box meta's type
 * (ours: 8.0's precision is unknown; DeepStream 8.0's observed boxes give the same points either way). */
RdsAnalyticsPoint
rds_analytics_ref_point (gfloat left, gfloat top, gfloat width, gfloat height)
{
  gfloat x = left + width / 2.0f;
  gfloat y = top + height;
  RdsAnalyticsPoint p = { to_int (floorf (x)), to_int (floorf (y)) };

  return p;
}

/* DeepStream 8.0 behaviour: the crossing number on integer coordinates. An edge (i, j) toggles when it straddles
 * the point's row, (yi > Py) != (yj > Py), and the point is left of the crossing x truncated to an int,
 * Px < (int) xc. The y test makes the bottom edge (and vertices) half-open and the truncated crossing makes the
 * right edge exclusive, so a triangle's apex row has no inside pixel. The crossing's precision is ours: it is
 * computed in double from integers, so an exact integer crossing stays exact and the vertex order can't change the
 * result. 8.0's precision is unknown; float gives the same crossings for coordinates up to 4K. */
gboolean
rds_analytics_point_in_polygon (RdsAnalyticsPoint p, const RdsAnalyticsPoint * poly, guint n)
{
  gboolean in = FALSE;

  if (n < 3)
    return FALSE;
  g_return_val_if_fail (poly != NULL, FALSE);

  for (guint i = 0, j = n - 1; i < n; j = i++) {
    gdouble xi = poly[i].x, yi = poly[i].y, xj = poly[j].x, yj = poly[j].y;

    if ((poly[i].y > p.y) != (poly[j].y > p.y)) {
      gint xc = to_int ((xj - xi) * ((gdouble) p.y - yi) / (yj - yi) + xi);

      if (p.x < xc)
        in = !in;
    }
  }
  return in;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* The core */

/* At most this many frames in a stream's objCnt window, whatever obj-cnt-win-in-ms (ours: five minutes at 30 fps;
 * the key goes up to 10^9 ms). The oldest frames leave first. */
#define OBJ_CNT_MAX_FRAMES 9000

/* One rule, scaled to the mux output. */
typedef struct
{
  gchar *label;
  RdsAnalyticsPoint *points;
  guint n_points;
} CoreRule;

/* One enabled group. */
typedef struct
{
  RdsAnalyticsFeature feature;
  GArray *class_ids;            /* gint; NULL: every class */
  gboolean inverse_roi;
  guint object_threshold;
  RdsAnalyticsMode mode;
  gboolean extended;
  CoreRule *rules;
  guint n_rules;
} CoreGroup;

/* The enabled groups of one configured stream. */
typedef struct
{
  CoreGroup *groups[RDS_ANALYTICS_N_FEATURES];
} CoreRules;

/* objCnt: one value of a class's per-frame count in the window, with how many window frames have it and the
 * sequence number of the newest of them. */
typedef struct
{
  guint value;
  guint freq;
  guint64 last_seq;
} ModeBin;

typedef struct
{
  gint class_id;
  GArray *bins;                 /* ModeBin */
} ClassWindow;

/* One frame of the window: its nonzero class counts (a class it doesn't list counts 0). A slot of the stream's ring,
 * reused with its counts buffer. */
typedef struct
{
  guint64 pts;
  guint64 seq;
  guint n, cap;
  RdsAnalyticsClassCount *counts;
} WindowFrame;

typedef struct
{
  const CoreRules *rules;       /* NULL: the stream has no enabled group */
  GArray *classes;              /* ClassWindow, sorted by (guint) class_id */
  /* the window's frames: a ring of n_slots slots, the oldest at head, n_frames in use; it grows when full */
  WindowFrame *slots;
  guint n_slots, head, n_frames;
  guint64 seq;
  guint64 last_pts;             /* RDS_ANALYTICS_PTS_NONE: none, or the last frame had none */
} StreamState;

/* Where an object's results are in the frame's pools while they grow. */
typedef struct
{
  guint roi_first, n_roi;
  guint oc_first, n_oc;
  gsize status;
} ObjectSpans;

struct _RdsAnalyticsCore
{
  guint64 window_ns;
  GHashTable *rules;            /* pad_index -> CoreRules * */
  GHashTable *streams;          /* pad_index -> StreamState * */

  /* the frame result, reused */
  GArray *roi_counts;           /* RdsAnalyticsRoiCount */
  GArray *oc;                   /* RdsAnalyticsOcStatus */
  GArray *obj_cnt;              /* RdsAnalyticsClassCount */
  GArray *objects;              /* RdsAnalyticsObjectResult */
  GArray *spans;                /* ObjectSpans */
  GPtrArray *labels;            /* the objects' labels (the rules' strings) */
  GString *strings;             /* the objects' strings, each NUL-terminated */
  GArray *frame_counts;         /* RdsAnalyticsClassCount: this frame's raw counts, in the order first seen */
};

static void
core_group_free (CoreGroup * g)
{
  if (g == NULL)
    return;
  for (guint i = 0; i < g->n_rules; i++) {
    g_free (g->rules[i].label);
    g_free (g->rules[i].points);
  }
  g_free (g->rules);
  g_clear_pointer (&g->class_ids, g_array_unref);
  g_free (g);
}

static void
core_rules_free (CoreRules * r)
{
  for (guint f = 0; f < RDS_ANALYTICS_N_FEATURES; f++)
    core_group_free (r->groups[f]);
  g_free (r);
}

static CoreGroup *
core_group_new (const RdsAnalyticsGroup * src, const RdsAnalyticsConfig * cfg, guint out_w, guint out_h)
{
  CoreGroup *g = g_new0 (CoreGroup, 1);

  g->feature = src->feature;
  if (src->class_ids != NULL) {
    g->class_ids = g_array_sized_new (FALSE, FALSE, sizeof (gint), src->class_ids->len);
    g_array_append_vals (g->class_ids, src->class_ids->data, src->class_ids->len);
  }
  g->inverse_roi = src->inverse_roi;
  g->object_threshold = src->object_threshold;
  g->mode = src->mode;
  g->extended = src->extended;
  g->n_rules = src->rules->len;
  g->rules = g_new0 (CoreRule, g->n_rules);
  for (guint i = 0; i < g->n_rules; i++) {
    const RdsAnalyticsRule *r = g_ptr_array_index (src->rules, i);
    CoreRule *c = &g->rules[i];

    c->label = g_strdup (r->label);
    c->n_points = r->n_points;
    c->points = g_new (RdsAnalyticsPoint, r->n_points);
    rds_analytics_scale_points (r->points, r->n_points, cfg->config_width, cfg->config_height, out_w, out_h,
        c->points);
  }
  return g;
}

static void
class_window_clear (gpointer p)
{
  ClassWindow *cw = p;

  g_array_unref (cw->bins);
}

static void
window_clear (StreamState * st)
{
  st->head = st->n_frames = 0;
  for (guint i = 0; i < st->classes->len; i++)
    g_array_set_size (g_array_index (st->classes, ClassWindow, i).bins, 0);
}

static void
stream_state_free (StreamState * st)
{
  for (guint i = 0; i < st->n_slots; i++)
    g_free (st->slots[i].counts);
  g_free (st->slots);
  g_array_unref (st->classes);
  g_free (st);
}

RdsAnalyticsCore *
rds_analytics_core_new (const RdsAnalyticsConfig * cfg, guint out_w, guint out_h)
{
  RdsAnalyticsCore *core;

  g_return_val_if_fail (cfg != NULL, NULL);
  g_return_val_if_fail (cfg->config_width > 0 && cfg->config_height > 0, NULL);
  g_return_val_if_fail (out_w > 0 && out_h > 0, NULL);

  core = g_new0 (RdsAnalyticsCore, 1);
  core->window_ns = (guint64) cfg->obj_cnt_win_in_ms * G_GUINT64_CONSTANT (1000000);
  core->rules = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) core_rules_free);
  core->streams = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) stream_state_free);
  for (guint i = 0; cfg->streams != NULL && i < cfg->streams->len; i++) {
    const RdsAnalyticsStream *s = g_ptr_array_index (cfg->streams, i);
    CoreRules *r = g_new0 (CoreRules, 1);
    gboolean any = FALSE;

    for (guint f = 0; f < RDS_ANALYTICS_N_FEATURES; f++) {
      const RdsAnalyticsGroup *g = s->groups[f];

      /* DeepStream 8.0 behaviour: a disabled group is as good as absent */
      if (g != NULL && g->enable) {
        r->groups[f] = core_group_new (g, cfg, out_w, out_h);
        any = TRUE;
      }
    }
    if (any)
      g_hash_table_insert (core->rules, GUINT_TO_POINTER (s->pad_index), r);
    else
      core_rules_free (r);
  }

  core->roi_counts = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsRoiCount));
  core->oc = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsOcStatus));
  core->obj_cnt = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsClassCount));
  core->objects = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsObjectResult));
  core->spans = g_array_new (FALSE, FALSE, sizeof (ObjectSpans));
  core->labels = g_ptr_array_new ();
  core->strings = g_string_new (NULL);
  core->frame_counts = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsClassCount));
  return core;
}

void
rds_analytics_core_free (RdsAnalyticsCore * core)
{
  if (core == NULL)
    return;
  g_hash_table_unref (core->streams);
  g_hash_table_unref (core->rules);
  g_array_unref (core->roi_counts);
  g_array_unref (core->oc);
  g_array_unref (core->obj_cnt);
  g_array_unref (core->objects);
  g_array_unref (core->spans);
  g_ptr_array_unref (core->labels);
  g_string_free (core->strings, TRUE);
  g_array_unref (core->frame_counts);
  g_free (core);
}

static StreamState *
get_stream (RdsAnalyticsCore * core, guint pad_index)
{
  StreamState *st = g_hash_table_lookup (core->streams, GUINT_TO_POINTER (pad_index));

  if (st == NULL) {
    st = g_new0 (StreamState, 1);
    st->rules = g_hash_table_lookup (core->rules, GUINT_TO_POINTER (pad_index));
    st->classes = g_array_new (FALSE, FALSE, sizeof (ClassWindow));
    g_array_set_clear_func (st->classes, class_window_clear);
    st->last_pts = RDS_ANALYTICS_PTS_NONE;
    g_hash_table_insert (core->streams, GUINT_TO_POINTER (pad_index), st);
  }
  return st;
}

/* DeepStream 8.0 behaviour: unset, empty or holding -1 means every class (the parser gives NULL for those). */
static gboolean
class_passes (const CoreGroup * g, gint class_id)
{
  if (g->class_ids == NULL)
    return TRUE;
  for (guint i = 0; i < g->class_ids->len; i++)
    if (g_array_index (g->class_ids, gint, i) == class_id)
      return TRUE;
  return FALSE;
}

/* ------------------------------------------------------------------------------------------------------------ */
/* objCnt */

static guint
counts_lookup (const RdsAnalyticsClassCount * counts, guint n, gint class_id)
{
  for (guint i = 0; i < n; i++)
    if (counts[i].class_id == class_id)
      return counts[i].count;
  return 0;
}

static void
bins_add (GArray * bins, guint value, guint freq, guint64 seq)
{
  ModeBin nb = { value, freq, seq };

  for (guint i = 0; i < bins->len; i++) {
    ModeBin *b = &g_array_index (bins, ModeBin, i);

    if (b->value == value) {
      b->freq += freq;
      b->last_seq = MAX (b->last_seq, seq);
      return;
    }
  }
  g_array_append_val (bins, nb);
}

/* Removes one frame of @value: the oldest of the window, so a bin left nonempty keeps its newest frame. */
static void
bins_remove (GArray * bins, guint value)
{
  for (guint i = 0; i < bins->len; i++) {
    ModeBin *b = &g_array_index (bins, ModeBin, i);

    if (b->value == value) {
      if (--b->freq == 0)
        g_array_remove_index_fast (bins, i);
      return;
    }
  }
  g_return_if_reached ();
}

/* The most frequent value; of the tied ones, the one seen most recently (ours: DeepStream 8.0 breaks ties no
 * stable way, sometimes to the older value, sometimes to the newer). */
static guint
bins_mode (const GArray * bins)
{
  const ModeBin *best = NULL;

  for (guint i = 0; i < bins->len; i++) {
    const ModeBin *b = &g_array_index (bins, ModeBin, i);

    if (best == NULL || b->freq > best->freq || (b->freq == best->freq && b->last_seq > best->last_seq))
      best = b;
  }
  return best != NULL ? best->value : 0;
}

static WindowFrame *
window_slot (StreamState * st, guint i)
{
  return &st->slots[(st->head + i) % st->n_slots];
}

/* A new slot at the tail, with room for @n counts. */
static WindowFrame *
window_push_slot (StreamState * st, guint n)
{
  WindowFrame *wf;

  if (st->n_frames == st->n_slots) {
    /* full: unroll into a ring twice the size, oldest first (every slot is in use, buffers included) */
    guint n_slots = MAX (16, st->n_slots * 2);
    WindowFrame *slots = g_new0 (WindowFrame, n_slots);

    for (guint i = 0; i < st->n_frames; i++)
      slots[i] = *window_slot (st, i);
    g_free (st->slots);
    st->slots = slots;
    st->n_slots = n_slots;
    st->head = 0;
  }
  wf = window_slot (st, st->n_frames++);
  if (wf->cap < n) {
    wf->cap = MAX (n, 2 * wf->cap);
    wf->counts = g_renew (RdsAnalyticsClassCount, wf->counts, wf->cap);
  }
  wf->n = 0;
  return wf;
}

static void
window_pop (StreamState * st)
{
  const WindowFrame *wf = window_slot (st, 0);

  for (guint i = 0; i < st->classes->len; i++) {
    ClassWindow *cw = &g_array_index (st->classes, ClassWindow, i);

    bins_remove (cw->bins, counts_lookup (wf->counts, wf->n, cw->class_id));
  }
  st->head = (st->head + 1) % st->n_slots;
  st->n_frames--;
}

/* The window: the stream's frames whose PTS is at most W = obj-cnt-win-in-ms before the current frame's, both
 * ends included (DeepStream 8.0 behaviour). The evidence, on 8.0's object counts:
 * - the default 50 ms behaves as 2 frames at 30 fps (33.3 ms apart) and as 1 at 10 fps;
 * - 100 ms at 10 fps behaves as the default at 30 fps, 2 frames, so the frame exactly 100 ms back is in;
 * - 100 ms at 30 fps gives 4 at a frame whose last four counts were 4, 4, 2, 2: the last three alone have 2 as
 *   their mode, so the frame 100 ms back is in;
 * - 1000 ms at 10 fps gives 4 at a frame whose last eleven counts hold 2, 3 and 4 three times each: without the
 *   frame exactly 1000 ms back (a 4), 4 would not be among the most frequent.
 * Every frame of those runs that has no tie matches this rule. Ours: a PTS going backwards (a seek or a loop) or a
 * missing PTS restarts the window, and a frame without a PTS is its own window. */
static void
window_push (RdsAnalyticsCore * core, StreamState * st, guint64 pts)
{
  GArray *cur = core->frame_counts;
  WindowFrame *wf;
  guint n_nonzero = 0;
  guint64 seq;

  if (pts == RDS_ANALYTICS_PTS_NONE || st->last_pts == RDS_ANALYTICS_PTS_NONE || pts < st->last_pts)
    window_clear (st);
  st->last_pts = pts;

  /* classes seen for the first time: the frames of the window before them count 0 (ours). The classes are kept
   * sorted by their id as unsigned, so -1 comes last: the order of 8.0's display-obj-cnt text (DeepStream 8.0
   * behaviour). */
  for (guint i = 0; i < cur->len; i++) {
    const RdsAnalyticsClassCount *c = &g_array_index (cur, RdsAnalyticsClassCount, i);
    guint k;

    for (k = 0; k < st->classes->len; k++)
      if ((guint) g_array_index (st->classes, ClassWindow, k).class_id >= (guint) c->class_id)
        break;
    if (k == st->classes->len || g_array_index (st->classes, ClassWindow, k).class_id != c->class_id) {
      ClassWindow cw = { c->class_id, g_array_new (FALSE, FALSE, sizeof (ModeBin)) };

      if (st->n_frames > 0)
        bins_add (cw.bins, 0, st->n_frames, window_slot (st, st->n_frames - 1)->seq);
      g_array_insert_val (st->classes, k, cw);
    }
    if (c->count > 0)
      n_nonzero++;
  }

  seq = ++st->seq;
  wf = window_push_slot (st, n_nonzero);
  wf->pts = pts;
  wf->seq = seq;
  for (guint i = 0; i < cur->len; i++) {
    const RdsAnalyticsClassCount *c = &g_array_index (cur, RdsAnalyticsClassCount, i);

    if (c->count > 0)
      wf->counts[wf->n++] = *c;
  }
  for (guint i = 0; i < st->classes->len; i++) {
    ClassWindow *cw = &g_array_index (st->classes, ClassWindow, i);

    bins_add (cw->bins, counts_lookup (wf->counts, wf->n, cw->class_id), 1, seq);
  }

  while (st->n_frames > 1) {
    const WindowFrame *head = window_slot (st, 0);

    if (st->n_frames <= OBJ_CNT_MAX_FRAMES && pts - head->pts <= core->window_ns)
      break;
    window_pop (st);
  }
}

/* this frame's raw count per class, every object whatever the groups (DeepStream 8.0 behaviour) */
static void
count_classes (RdsAnalyticsCore * core, const RdsAnalyticsObject * objs, guint n)
{
  GArray *cur = core->frame_counts;

  g_array_set_size (cur, 0);
  for (guint i = 0; i < n; i++) {
    guint k;

    for (k = 0; k < cur->len; k++)
      if (g_array_index (cur, RdsAnalyticsClassCount, k).class_id == objs[i].class_id)
        break;
    if (k == cur->len) {
      RdsAnalyticsClassCount c = { objs[i].class_id, 0 };

      g_array_append_val (cur, c);
    }
    g_array_index (cur, RdsAnalyticsClassCount, k).count++;
  }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* One frame */

void
rds_analytics_core_process_frame (RdsAnalyticsCore * core, guint pad_index, guint64 pts,
    const RdsAnalyticsObject * objs, guint n, RdsAnalyticsFrameResult * out)
{
  StreamState *st;
  const CoreGroup *roi = NULL, *oc = NULL;
  guint n_roi_rules = 0;

  g_return_if_fail (core != NULL && out != NULL);
  g_return_if_fail (n == 0 || objs != NULL);

  st = get_stream (core, pad_index);
  if (st->rules != NULL) {
    roi = st->rules->groups[RDS_ANALYTICS_ROI_FILTERING];
    oc = st->rules->groups[RDS_ANALYTICS_OVERCROWDING];
  }

  /* objInROIcnt: every ROI, then every overcrowding ROI, from 0 (DeepStream 8.0 behaviour) */
  g_array_set_size (core->roi_counts, 0);
  if (roi != NULL) {
    for (guint i = 0; i < roi->n_rules; i++) {
      RdsAnalyticsRoiCount c = { roi->rules[i].label, RDS_ANALYTICS_ROI_FILTERING, 0 };

      g_array_append_val (core->roi_counts, c);
    }
    n_roi_rules = roi->n_rules;
  }
  if (oc != NULL) {
    for (guint i = 0; i < oc->n_rules; i++) {
      RdsAnalyticsRoiCount c = { oc->rules[i].label, RDS_ANALYTICS_OVERCROWDING, 0 };

      g_array_append_val (core->roi_counts, c);
    }
  }

  g_array_set_size (core->objects, n);
  g_array_set_size (core->spans, n);
  g_ptr_array_set_size (core->labels, 0);
  g_string_truncate (core->strings, 0);
  for (guint i = 0; i < n; i++) {
    const RdsAnalyticsObject *o = &objs[i];
    ObjectSpans *sp = &g_array_index (core->spans, ObjectSpans, i);
    RdsAnalyticsPoint p = rds_analytics_ref_point (o->left, o->top, o->width, o->height);
    gsize status_len;

    sp->status = core->strings->len;

    /* roiStatus and the " ROI:<l>" pieces; with inverse-roi the objects outside (DeepStream 8.0 behaviour) */
    sp->roi_first = core->labels->len;
    if (roi != NULL && class_passes (roi, o->class_id)) {
      for (guint r = 0; r < roi->n_rules; r++) {
        const CoreRule *rule = &roi->rules[r];
        gboolean in = rds_analytics_point_in_polygon (p, rule->points, rule->n_points);

        if (in != roi->inverse_roi) {
          g_array_index (core->roi_counts, RdsAnalyticsRoiCount, r).count++;
          g_ptr_array_add (core->labels, rule->label);
          g_string_append (core->strings, " ROI:");
          g_string_append (core->strings, rule->label);
        }
      }
    }
    sp->n_roi = core->labels->len - sp->roi_first;

    /* the overcrowding labels: never in objStatus (DeepStream 8.0 behaviour) */
    sp->oc_first = core->labels->len;
    if (oc != NULL && class_passes (oc, o->class_id)) {
      for (guint r = 0; r < oc->n_rules; r++) {
        const CoreRule *rule = &oc->rules[r];

        if (rds_analytics_point_in_polygon (p, rule->points, rule->n_points)) {
          g_array_index (core->roi_counts, RdsAnalyticsRoiCount, n_roi_rules + r).count++;
          g_ptr_array_add (core->labels, rule->label);
        }
      }
    }
    sp->n_oc = core->labels->len - sp->oc_first;

    status_len = core->strings->len - sp->status;
    g_string_append_c (core->strings, '\0');
    g_array_index (core->objects, RdsAnalyticsObjectResult, i).has_info = sp->n_roi > 0 || sp->n_oc > 0
        || status_len > 0;
  }

  /* the pools are complete: point into them */
  for (guint i = 0; i < n; i++) {
    const ObjectSpans *sp = &g_array_index (core->spans, ObjectSpans, i);
    RdsAnalyticsObjectResult *r = &g_array_index (core->objects, RdsAnalyticsObjectResult, i);
    const gchar *const *labels = (const gchar * const *) core->labels->pdata;

    r->roi_labels = labels != NULL ? labels + sp->roi_first : NULL;
    r->n_roi_labels = sp->n_roi;
    r->oc_labels = labels != NULL ? labels + sp->oc_first : NULL;
    r->n_oc_labels = sp->n_oc;
    r->obj_status = core->strings->str + sp->status;
  }

  /* ocStatus: count >= object-threshold, never at 0, on this frame (DeepStream 8.0 behaviour) */
  g_array_set_size (core->oc, 0);
  if (oc != NULL) {
    for (guint r = 0; r < oc->n_rules; r++) {
      guint count = g_array_index (core->roi_counts, RdsAnalyticsRoiCount, n_roi_rules + r).count;
      RdsAnalyticsOcStatus s = { oc->rules[r].label, count, count >= oc->object_threshold && count > 0 };

      g_array_append_val (core->oc, s);
    }
  }

  /* objCnt */
  count_classes (core, objs, n);
  window_push (core, st, pts);
  g_array_set_size (core->obj_cnt, st->classes->len);
  for (guint i = 0; i < st->classes->len; i++) {
    const ClassWindow *cw = &g_array_index (st->classes, ClassWindow, i);
    RdsAnalyticsClassCount *c = &g_array_index (core->obj_cnt, RdsAnalyticsClassCount, i);

    c->class_id = cw->class_id;
    c->count = bins_mode (cw->bins);
  }

  out->roi_counts = (const RdsAnalyticsRoiCount *) core->roi_counts->data;
  out->n_roi_counts = core->roi_counts->len;
  out->oc = (const RdsAnalyticsOcStatus *) core->oc->data;
  out->n_oc = core->oc->len;
  out->obj_cnt = (const RdsAnalyticsClassCount *) core->obj_cnt->data;
  out->n_obj_cnt = core->obj_cnt->len;
  out->objects = (const RdsAnalyticsObjectResult *) core->objects->data;
  out->n_objects = n;
}
