/* Redstream: the rdsanalytics core. Geometry: rule scaling, the reference point of a box and the inside test. The
 * per-stream state and the features: ROI filtering, overcrowding, the object counts, the per-object histories,
 * direction with the STOPPED state, and line crossing.
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

/* History and direction (DeepStream 8.0 behaviour; see rds-analytics-core.h) */
#define HISTORY_WINDOW 50       /* the window's points */
#define DIR_MIN_N 10            /* no direction before n = 10 */
#define DIR_MIN_DIST2 (55.0 * 55.0)     /* |d| >= 55 px, compared squared */
#define STOPPED_MIN_N 50        /* no STOPPED before n = 50 */
#define STOPPED_MAX_SX 20.0     /* sx <= 20 px */
/* sy <= STOPPED_SY_COEF * sqrt (h). Ours: 8.0's limits, measured for box heights 10 to 200, are each within 0.5 %
 * of 0.1 * sqrt (h), but no single coefficient fits all six. Replayed on 8.0's direction runs, 0.1 misses 4 states
 * of its box-height run, and the same 4 at another config size (any coefficient from 0.0999 to 0.1002 does as
 * well); 0.1004 misses 8 of each. */
#define STOPPED_SY_COEF 0.1

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

/* One object's history: its last HISTORY_WINDOW points and how many it has had, and the lines it has been counted
 * on. */
typedef struct
{
  guint64 id;                   /* the hash table's key */
  RdsAnalyticsPoint win[HISTORY_WINDOW];        /* a ring, the oldest at head */
  guint head, len;
  guint64 n_obs;                /* observations so far; n = n_obs - 1 */
  gboolean lc_counted[];        /* per line of the stream's line-crossing group: already counted */
} History;

static void
history_free (History * h)
{
  g_free (h);
}

typedef struct
{
  const CoreRules *rules;       /* NULL: the stream has no enabled group */
  GHashTable *histories;        /* guint64 * id -> History *, when the stream has direction or line crossing */
  guint n_lines;                /* the lines of its line-crossing group; 0 without one */
  guint64 *lc_cum;              /* objLCCumCnt per line, apart from the histories so that a reset can keep them */
  gboolean untracked_warned;    /* untracked_warning was given */
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
  guint lc_first, n_lc;
  gsize status, dir_status;
  gboolean stopped;
} ObjectSpans;

struct _RdsAnalyticsCore
{
  guint64 window_ns;
  GHashTable *rules;            /* pad_index -> CoreRules * */
  GHashTable *streams;          /* pad_index -> StreamState * */

  /* the frame result, reused */
  GArray *roi_counts;           /* RdsAnalyticsRoiCount */
  GArray *oc;                   /* RdsAnalyticsOcStatus */
  GArray *lc;                   /* RdsAnalyticsLcCount */
  GArray *obj_cnt;              /* RdsAnalyticsClassCount */
  GArray *objects;              /* RdsAnalyticsObjectResult */
  GArray *spans;                /* ObjectSpans */
  GPtrArray *labels;            /* the objects' labels (the rules' strings) */
  GString *strings;             /* the objects' strings, each NUL-terminated */
  GString *dir;                 /* one object's dirStatus while it is built */
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
  g_clear_pointer (&st->histories, g_hash_table_unref);
  g_free (st->lc_cum);
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
  core->lc = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsLcCount));
  core->obj_cnt = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsClassCount));
  core->objects = g_array_new (FALSE, FALSE, sizeof (RdsAnalyticsObjectResult));
  core->spans = g_array_new (FALSE, FALSE, sizeof (ObjectSpans));
  core->labels = g_ptr_array_new ();
  core->strings = g_string_new (NULL);
  core->dir = g_string_new (NULL);
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
  g_array_unref (core->lc);
  g_array_unref (core->obj_cnt);
  g_array_unref (core->objects);
  g_array_unref (core->spans);
  g_ptr_array_unref (core->labels);
  g_string_free (core->strings, TRUE);
  g_string_free (core->dir, TRUE);
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
    if (st->rules != NULL && (st->rules->groups[RDS_ANALYTICS_DIRECTION] != NULL
            || st->rules->groups[RDS_ANALYTICS_LINE_CROSSING] != NULL))
      st->histories = g_hash_table_new_full (g_int64_hash, g_int64_equal, NULL, (GDestroyNotify) history_free);
    if (st->rules != NULL && st->rules->groups[RDS_ANALYTICS_LINE_CROSSING] != NULL) {
      st->n_lines = st->rules->groups[RDS_ANALYTICS_LINE_CROSSING]->n_rules;
      st->lc_cum = g_new0 (guint64, st->n_lines);
    }
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
/* Histories and direction */

static History *
history_get (StreamState * st, guint64 id)
{
  History *h = g_hash_table_lookup (st->histories, &id);

  if (h == NULL) {
    h = g_malloc0 (sizeof (History) + st->n_lines * sizeof (gboolean));
    h->id = id;
    g_hash_table_insert (st->histories, &h->id, h);
  }
  return h;
}

/* The @i-th point of the window, from the oldest */
static RdsAnalyticsPoint
history_point (const History * h, guint i)
{
  return h->win[(h->head + i) % HISTORY_WINDOW];
}

/* DeepStream 8.0 behaviour: one point per observation, x clamped at 0. Ours: two objects with the same id in one
 * frame add a point each. */
static void
history_push (History * h, RdsAnalyticsPoint p)
{
  p.x = MAX (p.x, 0);
  if (h->len < HISTORY_WINDOW) {
    h->win[(h->head + h->len++) % HISTORY_WINDOW] = p;
  } else {
    h->win[h->head] = p;
    h->head = (h->head + 1) % HISTORY_WINDOW;
  }
  h->n_obs++;
}

/* The population standard deviation of the window's x (@axis 0) or y (1) coordinates. DeepStream 8.0 behaviour:
 * population, over the integer points. Ours: computed in double, the mean first (8.0's precision is unknown; the
 * observed limits, sx = 20.0 exactly for a 0/40 px step, are exact in double). */
static gdouble
history_pstd (const History * h, guint axis)
{
  gdouble sum = 0, var = 0, mean;

  for (guint i = 0; i < h->len; i++) {
    RdsAnalyticsPoint p = history_point (h, i);

    sum += axis == 0 ? p.x : p.y;
  }
  mean = sum / h->len;
  for (guint i = 0; i < h->len; i++) {
    RdsAnalyticsPoint p = history_point (h, i);
    gdouble dv = (axis == 0 ? p.x : p.y) - mean;

    var += dv * dv;
  }
  return sqrt (var / h->len);
}

/* DeepStream 8.0 behaviour: from n = 50 on, sx <= 20 and sy <= 0.1 * sqrt (h), both inclusive; h is the current
 * box height (ours: 8.0's is undetermined; the measured limits don't depend on the box width) */
static gboolean
history_stopped (const History * h, gfloat height)
{
  if (h->n_obs - 1 < STOPPED_MIN_N)
    return FALSE;
  return history_pstd (h, 0) <= STOPPED_MAX_SX
      && history_pstd (h, 1) <= STOPPED_SY_COEF * sqrt (MAX ((gdouble) height, 0.0));
}

/* DeepStream 8.0 behaviour: the integer-truncated mean of the window's oldest 2 points while it fills, of its
 * oldest 4 once it is full. C's integer division truncates toward zero, as 8.0's does. */
static RdsAnalyticsPoint
history_reference (const History * h)
{
  guint k = h->len < HISTORY_WINDOW ? 2 : 4;
  gint64 sx = 0, sy = 0;
  RdsAnalyticsPoint ref;

  for (guint i = 0; i < k; i++) {
    RdsAnalyticsPoint p = history_point (h, i);

    sx += p.x;
    sy += p.y;
  }
  ref.x = (gint) (sx / (gint64) k);
  ref.y = (gint) (sy / (gint64) k);
  return ref;
}

/* Whether the direction rule's vector is within 60 degrees of (@dx, @dy): cos >= 0.5, inclusive (DeepStream 8.0
 * behaviour). A zero vector never matches (DeepStream 8.0 behaviour). Ours: the test is dot >= 0 and
 * 4 dot^2 >= |d|^2 |v|^2, in double from integers, so it is exact while the products stay below 2^53 (coordinates
 * up to about 2^13); on integer vectors the angle is never exactly 60 degrees, so a cosine in float or double
 * gives the same answers. */
static gboolean
direction_matches (const CoreRule * rule, gdouble dx, gdouble dy)
{
  gdouble vx = (gdouble) rule->points[1].x - rule->points[0].x;
  gdouble vy = (gdouble) rule->points[1].y - rule->points[0].y;
  gdouble dot = dx * vx + dy * vy;

  if ((vx == 0 && vy == 0) || dot < 0)
    return FALSE;
  return 4 * dot * dot >= (dx * dx + dy * dy) * (vx * vx + vy * vy);
}

/* The direction of an object with history @h and current box height @height: sets @stopped, or appends
 * " DIR:<l> " to core->dir for each matching rule of @dir, in config order (DeepStream 8.0 behaviour) */
static void
direction_eval (RdsAnalyticsCore * core, const CoreGroup * dir, const History * h, gfloat height,
    gboolean * stopped)
{
  RdsAnalyticsPoint ref, cur;
  gdouble dx, dy;

  *stopped = history_stopped (h, height);
  if (*stopped || h->n_obs - 1 < DIR_MIN_N)
    return;
  ref = history_reference (h);
  cur = history_point (h, h->len - 1);
  dx = (gdouble) cur.x - ref.x;
  dy = (gdouble) cur.y - ref.y;
  /* |d| >= 55, Euclidean, compared squared: exact on integers */
  if (dx * dx + dy * dy < DIR_MIN_DIST2)
    return;
  for (guint r = 0; r < dir->n_rules; r++) {
    if (direction_matches (&dir->rules[r], dx, dy)) {
      g_string_append (core->dir, " DIR:");
      g_string_append (core->dir, dir->rules[r].label);
      g_string_append_c (core->dir, ' ');
    }
  }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* Line crossing */

/* DeepStream 8.0 behaviour: per mode, the earliest n, the least |d| and the angle limit between d and the rule's
 * vector: cos > 0 (loose: under 90 degrees), cos > 0.5 (balanced: under 60) or cos > 0.8 (strict: under 36.87),
 * tested as dot > 0 and dot^2 * cos2_den > cos2_num * |d|^2 |v|^2. Replayed on 8.0's runs:
 * - loose: 89 degrees counted, 91 not;
 * - balanced: integer vectors are never exactly 60 degrees apart, so > and >= agree; 8.0's 60 degree path, whose
 *   integer points stay a little over 60 until frame 54, counts there, as this rule does;
 * - strict: 8.0 counted a d 36.24 degrees off the vector, and not a 3-4-5 one at exactly cos = 0.8 (36.87). So the
 *   limit is in [36.24, 36.87) degrees, strict at 0.8 if it is a cosine; a limit of 36 degrees counts two of 8.0's
 *   crossings late. Ours: cos > 0.8, the one round limit in that range, strict as the other two. */
typedef struct
{
  guint min_n;
  gdouble min_dist;
  gdouble cos2_num, cos2_den;
} LcMode;

static const LcMode lc_modes[] = {
  [RDS_ANALYTICS_MODE_LOOSE] = {3, 32.0, 0, 1},
  [RDS_ANALYTICS_MODE_BALANCED] = {10, 55.0, 1, 4},
  [RDS_ANALYTICS_MODE_STRICT] = {20, 78.0, 16, 25},
};

/* Cross products for the line tests, exact for any int coordinates: each factor is a difference of two ints (33
 * bits), each product 66 bits, so they are computed in 128 bits (GCC and Clang have __int128 on 64-bit targets) */
__extension__ typedef __int128 LcCross;

/* Which side of the line through @a and @b the point @p is on: the sign of the cross product (b - a) x (p - a) */
static LcCross
line_side (RdsAnalyticsPoint a, RdsAnalyticsPoint b, RdsAnalyticsPoint p)
{
  return (LcCross) ((gint64) b.x - a.x) * ((gint64) p.y - a.y) - (LcCross) ((gint64) b.y - a.y) * ((gint64) p.x -
      a.x);
}

/* Whether an object with reference @ref, newest point @cur and current box width @width crosses @rule (the
 * direction vector d1 -> d2, then the line l1 - l2) under @g's mode and extended (DeepStream 8.0 behaviour; see
 * rds-analytics-core.h). The caller checks n and the "already counted" flag. */
static gboolean
line_crossed (const CoreGroup * g, const CoreRule * rule, RdsAnalyticsPoint ref, RdsAnalyticsPoint cur,
    gfloat width)
{
  const LcMode *mode = &lc_modes[g->mode];
  RdsAnalyticsPoint d1 = rule->points[0], d2 = rule->points[1], l1 = rule->points[2], l2 = rule->points[3];
  gdouble vx = (gdouble) d2.x - d1.x, vy = (gdouble) d2.y - d1.y;
  gdouble dx = (gdouble) cur.x - ref.x, dy = (gdouble) cur.y - ref.y;
  gdouble dot = dx * vx + dy * vy;
  /* DeepStream 8.0 behaviour: "past" is the side of the line the vector points to, the sign of (l2 - l1) x v (in
   * 8.0's runs every vector started before its line and ended past it, so whether its start point matters is not
   * known). A vector along the line, or a line of one point, has no past side: never counted (ours). The sides are
   * exact cross products on the integer points (ours): a point exactly on a slanted line is on it. 8.0 counted
   * such a point at the line's first end but not in its middle or at its second end, one frame later each time,
   * as a float slope, y1 + (x - x1) * ((float) (y2 - y1) / (float) (x2 - x1)), evaluated in double would. */
  LcCross past = (LcCross) ((gint64) l2.x - l1.x) * ((gint64) d2.y - d1.y) - (LcCross) ((gint64) l2.y - l1.y) *
      ((gint64) d2.x - d1.x);
  LcCross s_ref = line_side (l1, l2, ref), s_cur = line_side (l1, l2, cur);

  if (past == 0)
    return FALSE;
  if (past < 0) {
    s_ref = -s_ref;
    s_cur = -s_cur;
  }
  /* the reference strictly before the line; the newest point on or past it (loose), strictly past it (balanced,
   * strict) */
  if (s_ref >= 0 || s_cur < 0 || (s_cur == 0 && g->mode != RDS_ANALYTICS_MODE_LOOSE))
    return FALSE;
  /* |d| >= the mode's least displacement, compared squared: exact on integers */
  if (dx * dx + dy * dy < mode->min_dist * mode->min_dist)
    return FALSE;
  /* the angle, as for direction: squared, in double from integers, so exact while the products stay below 2^53
   * (coordinates up to about 2^12) */
  if (dot <= 0 || mode->cos2_den * dot * dot <= mode->cos2_num * (dx * dx + dy * dy) * (vx * vx + vy * vy))
    return FALSE;
  if (!g->extended) {
    /* DeepStream 8.0 behaviour: the crossing must hit the segment, lengthened by half the box width at each end,
     * strictly (a 20 px box counts from 9 px before an end, not from 10). 8.0's runs crossed horizontal segments
     * straight down, where every reading agrees; ours: the point where ref -> cur meets the line, projected on the
     * segment, and the current box's width, in double. A crossing exactly w/2 past an end is a tie decided exactly
     * only when ref -> cur is perpendicular to the line (as in 8.0's runs); otherwise rounding decides it. The
     * fraction u is in double, from the exact sides (their difference may not fit 64 bits). */
    gdouble lx = (gdouble) l2.x - l1.x, ly = (gdouble) l2.y - l1.y, len = sqrt (lx * lx + ly * ly);
    gdouble u = (gdouble) s_ref / ((gdouble) s_ref - (gdouble) s_cur);  /* s_ref < 0 <= s_cur */
    gdouble cx = ref.x + u * dx, cy = ref.y + u * dy;
    gdouble t = ((cx - l1.x) * lx + (cy - l1.y) * ly) / len, tol = MAX ((gdouble) width, 0.0) / 2;

    if (!(t > -tol && t < len + tol))
      return FALSE;
  }
  return TRUE;
}

/* Counts the object of history @h and box width @width on every line of @lc it newly crosses (DeepStream 8.0
 * behaviour): adds their labels to core->labels, their counts to core->lc and st->lc_cum, and marks them counted */
static void
line_crossing_eval (RdsAnalyticsCore * core, StreamState * st, const CoreGroup * lc, History * h, gfloat width)
{
  RdsAnalyticsPoint ref, cur;

  if (h->n_obs - 1 < lc_modes[lc->mode].min_n)
    return;
  ref = history_reference (h);
  cur = history_point (h, h->len - 1);
  /* the history's flags are sized by st->n_lines, the group's rule count */
  g_assert (st->n_lines == lc->n_rules);
  for (guint r = 0; r < st->n_lines; r++) {
    if (h->lc_counted[r] || !line_crossed (lc, &lc->rules[r], ref, cur, width))
      continue;
    h->lc_counted[r] = TRUE;
    g_array_index (core->lc, RdsAnalyticsLcCount, r).current++;
    st->lc_cum[r]++;
    g_ptr_array_add (core->labels, lc->rules[r].label);
  }
}

/* ------------------------------------------------------------------------------------------------------------ */
/* One frame */

void
rds_analytics_core_process_frame (RdsAnalyticsCore * core, guint pad_index, guint64 pts,
    const RdsAnalyticsObject * objs, guint n, RdsAnalyticsFrameResult * out)
{
  StreamState *st;
  const CoreGroup *roi = NULL, *oc = NULL, *dir = NULL, *lc = NULL;
  guint n_roi_rules = 0;

  g_return_if_fail (core != NULL && out != NULL);
  g_return_if_fail (n == 0 || objs != NULL);

  st = get_stream (core, pad_index);
  if (st->rules != NULL) {
    roi = st->rules->groups[RDS_ANALYTICS_ROI_FILTERING];
    oc = st->rules->groups[RDS_ANALYTICS_OVERCROWDING];
    dir = st->rules->groups[RDS_ANALYTICS_DIRECTION];
    lc = st->rules->groups[RDS_ANALYTICS_LINE_CROSSING];
  }
  out->untracked_warning = FALSE;

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

  /* objLCCurrCnt: every line, from 0 */
  g_array_set_size (core->lc, 0);
  for (guint i = 0; lc != NULL && i < lc->n_rules; i++) {
    RdsAnalyticsLcCount c = { lc->rules[i].label, 0, 0 };

    g_array_append_val (core->lc, c);
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

    /* the history, the direction and the line crossings; untracked objects are skipped (ours) */
    g_string_truncate (core->dir, 0);
    sp->stopped = FALSE;
    sp->lc_first = core->labels->len;
    if (st->histories != NULL) {
      if (o->id == RDS_ANALYTICS_UNTRACKED) {
        if (!st->untracked_warned)
          st->untracked_warned = out->untracked_warning = TRUE;
      } else {
        History *h = history_get (st, o->id);

        /* every tracked object builds its history, whatever the class filters: line crossing shares it (ours) */
        history_push (h, p);
        /* direction and STOPPED only for the classes of the group's class-id (DeepStream 8.0 behaviour) */
        if (dir != NULL && class_passes (dir, o->class_id))
          direction_eval (core, dir, h, o->height, &sp->stopped);
        /* line crossing only for the classes of the group's class-id; no STOPPED gate (DeepStream 8.0 behaviour) */
        if (lc != NULL && class_passes (lc, o->class_id))
          line_crossing_eval (core, st, lc, h, o->width);
      }
    }
    sp->n_lc = core->labels->len - sp->lc_first;

    /* objStatus: the ROI pieces, then " DIR:<l> " per direction, then " LC:<l> " per line counted on this frame,
     * then " STOPPED " (DeepStream 8.0 behaviour) */
    g_string_append_len (core->strings, core->dir->str, core->dir->len);
    for (guint k = 0; k < sp->n_lc; k++) {
      g_string_append (core->strings, " LC:");
      g_string_append (core->strings, g_ptr_array_index (core->labels, sp->lc_first + k));
      g_string_append_c (core->strings, ' ');
    }
    if (sp->stopped)
      g_string_append (core->strings, " STOPPED ");
    status_len = core->strings->len - sp->status;
    g_string_append_c (core->strings, '\0');
    /* dirStatus: the same " DIR:<l> " pieces (DeepStream 8.0 behaviour) */
    sp->dir_status = core->strings->len;
    g_string_append_len (core->strings, core->dir->str, core->dir->len);
    g_string_append_c (core->strings, '\0');
    /* a label, a direction, a line crossing or STOPPED (the last three are in objStatus) */
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
    r->lc_labels = labels != NULL ? labels + sp->lc_first : NULL;
    r->n_lc_labels = sp->n_lc;
    r->obj_status = core->strings->str + sp->status;
    r->dir_status = core->strings->str + sp->dir_status;
    r->stopped = sp->stopped;
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

  /* objLCCumCnt */
  for (guint i = 0; i < core->lc->len; i++)
    g_array_index (core->lc, RdsAnalyticsLcCount, i).cumulative = st->lc_cum[i];

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
  out->lc = (const RdsAnalyticsLcCount *) core->lc->data;
  out->n_lc = core->lc->len;
  out->obj_cnt = (const RdsAnalyticsClassCount *) core->obj_cnt->data;
  out->n_obj_cnt = core->obj_cnt->len;
  out->objects = (const RdsAnalyticsObjectResult *) core->objects->data;
  out->n_objects = n;
}
