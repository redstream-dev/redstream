/* Tracking metrics without ground truth, for the real-clip tracker tests: ID switches by fragmentation, coverage and
 * mean track length per class, from per-frame detections and the IDs a tracker gave them. Also reads and writes
 * those detections as text, so a clip's detections can be replayed through the tracking core. Header-only; GLib only.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_TRACKS_H
#define RDS_TEST_TRACKS_H

#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TRACK_NO_ID G_MAXUINT64
/* fragmentation: a new same-class track starting within these of where another one ended counts as an ID switch */
#define TRACK_SWITCH_PX 30.0f
#define TRACK_SWITCH_FRAMES 15
/* coverage only counts detections at least this confident */
#define TRACK_COVERAGE_CONF 0.5f
/* a detection belongs to a track's probation if its IOU with the track's next box is at least this */
#define TRACK_PROBATION_IOU 0.3f

/* One detection of the detector on one frame, with the ID the tracker gave it (TRACK_NO_ID: none). */
typedef struct
{
  gint frame, class_id;
  gfloat conf, x, y, w, h;
  guint64 id;
} TrackDet;

typedef struct
{
  guint tracks, switches, eligible, covered;
  gdouble mean_length;          /* frames reported per track */
} TrackStats;

static gfloat
track_iou (const TrackDet * a, const TrackDet * b)
{
  gfloat x1 = MAX (a->x, b->x), y1 = MAX (a->y, b->y);
  gfloat x2 = MIN (a->x + a->w, b->x + b->w), y2 = MIN (a->y + a->h, b->y + b->h);
  gfloat inter = MAX (x2 - x1, 0.0f) * MAX (y2 - y1, 0.0f);
  gfloat uni = a->w * a->h + b->w * b->h - inter;

  return uni > 0.0f ? inter / uni : 0.0f;
}

typedef struct
{
  guint64 id;
  gint class_id, first_frame, last_frame;
  guint n;
  const TrackDet *first, *last;
} Track;

static gint
track_by_first_frame (gconstpointer a, gconstpointer b)
{
  const Track *ta = a, *tb = b;

  return ta->first_frame != tb->first_frame ? ta->first_frame - tb->first_frame : (ta->id > tb->id) - (ta->id < tb->id);
}

/* Metrics of @dets (sorted by frame) for @class_id (-1: every class), with the tracker's @probation (probationAge):
 * - tracks and their mean length (frames with the ID);
 * - switches: a track that ends, then a new track of the same class starting within TRACK_SWITCH_FRAMES frames and
 *   TRACK_SWITCH_PX (box centres) of its last box; each track ends and starts at most one switch, the nearest first;
 * - coverage (@covered of @eligible): detections with conf >= TRACK_COVERAGE_CONF that carry an ID, leaving out each
 *   track's probation: going back from its first box, on each of the @probation frames before it, the untracked
 *   same-class detection overlapping the track's next box most (IOU >= TRACK_PROBATION_IOU). */
static void
track_metrics (const GArray * dets, gint class_id, guint probation, TrackStats * st)
{
  GHashTable *by_id = g_hash_table_new_full (g_int64_hash, g_int64_equal, NULL, g_free);
  GArray *tracks = g_array_new (FALSE, FALSE, sizeof (Track));
  gboolean *probation_det = g_new0 (gboolean, dets->len + 1);
  gboolean *ended = NULL, *started = NULL;
  gint n_frames = dets->len ? g_array_index (dets, TrackDet, dets->len - 1).frame + 1 : 0;
  guint *frame_start = g_new0 (guint, n_frames + 1);
  GHashTableIter it;
  gpointer value;
  guint64 total_len = 0;

  memset (st, 0, sizeof (*st));
  for (guint i = 0, f = 0; f <= (guint) n_frames; f++) {
    while (i < dets->len && g_array_index (dets, TrackDet, i).frame < (gint) f)
      i++;
    frame_start[f] = i;
  }
  for (guint i = 0; i < dets->len; i++) {
    const TrackDet *d = &g_array_index (dets, TrackDet, i);
    Track *t;

    if (d->id == TRACK_NO_ID || (class_id >= 0 && d->class_id != class_id))
      continue;
    t = g_hash_table_lookup (by_id, &d->id);
    if (!t) {
      t = g_new0 (Track, 1);
      t->id = d->id;
      t->class_id = d->class_id;
      t->first_frame = d->frame;
      t->first = d;
      g_hash_table_insert (by_id, &t->id, t);
    }
    t->last_frame = d->frame;
    t->last = d;
    t->n++;
  }
  g_hash_table_iter_init (&it, by_id);
  while (g_hash_table_iter_next (&it, NULL, &value))
    g_array_append_vals (tracks, value, 1);
  g_array_sort (tracks, track_by_first_frame);
  st->tracks = tracks->len;
  for (guint i = 0; i < tracks->len; i++)
    total_len += g_array_index (tracks, Track, i).n;
  st->mean_length = tracks->len ? (gdouble) total_len / tracks->len : 0.0;

  /* switches: tracks in order of their end, each pairing with the nearest unclaimed start */
  ended = g_new0 (gboolean, tracks->len + 1);
  started = g_new0 (gboolean, tracks->len + 1);
  for (gint e = 0; e < n_frames; e++) {
    for (guint a = 0; a < tracks->len; a++) {
      const Track *ta = &g_array_index (tracks, Track, a);
      gfloat ax, ay, best_d = TRACK_SWITCH_PX;
      gint best = -1;

      if (ta->last_frame != e || ended[a])
        continue;
      ax = ta->last->x + ta->last->w / 2;
      ay = ta->last->y + ta->last->h / 2;
      for (guint b = 0; b < tracks->len; b++) {
        const Track *tb = &g_array_index (tracks, Track, b);
        gfloat dist;

        if (started[b] || tb->class_id != ta->class_id || tb->first_frame <= e
            || tb->first_frame > e + TRACK_SWITCH_FRAMES)
          continue;
        dist = hypotf (tb->first->x + tb->first->w / 2 - ax, tb->first->y + tb->first->h / 2 - ay);
        if (dist <= best_d) {
          best_d = dist;
          best = (gint) b;
        }
      }
      if (best >= 0) {
        ended[a] = TRUE;
        started[best] = TRUE;
        st->switches++;
      }
    }
  }

  /* each track's probation detections */
  for (guint t = 0; t < tracks->len; t++) {
    const Track *tr = &g_array_index (tracks, Track, t);
    const TrackDet *next = tr->first;

    for (guint k = 1; k <= probation && tr->first_frame - (gint) k >= 0; k++) {
      gint f = tr->first_frame - (gint) k;
      gint best = -1;
      gfloat best_iou = TRACK_PROBATION_IOU;

      for (guint i = frame_start[f]; i < frame_start[f + 1]; i++) {
        const TrackDet *d = &g_array_index (dets, TrackDet, i);
        gfloat v;

        if (d->id != TRACK_NO_ID || d->class_id != tr->class_id || probation_det[i])
          continue;
        v = track_iou (d, next);
        if (v >= best_iou) {
          best_iou = v;
          best = (gint) i;
        }
      }
      if (best < 0)
        break;
      probation_det[best] = TRUE;
      next = &g_array_index (dets, TrackDet, best);
    }
  }
  for (guint i = 0; i < dets->len; i++) {
    const TrackDet *d = &g_array_index (dets, TrackDet, i);

    if ((class_id >= 0 && d->class_id != class_id) || d->conf < TRACK_COVERAGE_CONF || probation_det[i])
      continue;
    st->eligible++;
    if (d->id != TRACK_NO_ID)
      st->covered++;
  }

  g_free (started);
  g_free (ended);
  g_free (frame_start);
  g_free (probation_det);
  g_array_unref (tracks);
  g_hash_table_unref (by_id);
}

/* NaN without eligible detections: any comparison with a budget then fails */
static gdouble
track_coverage (const TrackStats * st)
{
  return st->eligible ? (gdouble) st->covered / st->eligible : NAN;
}

#define TRACK_CLASS_PERSON 0
#define TRACK_CLASS_CAR 2

/* Prints a table (g_test_message) of the metrics for person, car and every class; @all gets the last row. */
static void
track_print_table (const gchar * title, const GArray * dets, guint probation, TrackStats * all)
{
  static const struct
  {
    const gchar *name;
    gint class_id;
  } rows[] = { {"person", TRACK_CLASS_PERSON}, {"car", TRACK_CLASS_CAR}, {"all", -1} };

  g_test_message ("%s", title);
  g_test_message ("  %-8s %7s %8s %9s %10s %9s", "class", "tracks", "mean len", "switches", "detections", "coverage");
  for (guint r = 0; r < G_N_ELEMENTS (rows); r++) {
    TrackStats st;

    track_metrics (dets, rows[r].class_id, probation, &st);
    g_test_message ("  %-8s %7u %8.1f %9u %10u %9.3f", rows[r].name, st.tracks, st.mean_length, st.switches,
        st.eligible, track_coverage (&st));
    if (rows[r].class_id < 0 && all)
      *all = st;
  }
}

/* The sha256 of a file's contents, as hex (NULL if it can't be read). */
static gchar *
track_file_sha256 (const gchar * path)
{
  gchar *text = NULL, *sum;
  gsize len = 0;

  if (!g_file_get_contents (path, &text, &len, NULL))
    return NULL;
  sum = g_compute_checksum_for_data (G_CHECKSUM_SHA256, (const guchar *) text, len);
  g_free (text);
  return sum;
}

/* The sha256 of the contents of the ';'-separated files @paths, in order (NULL if one can't be read). */
static gchar *
track_files_sha256 (const gchar * paths)
{
  gchar **list = g_strsplit (paths, ";", -1), *sum = NULL;
  GChecksum *ck = g_checksum_new (G_CHECKSUM_SHA256);

  for (guint i = 0; list[i]; i++) {
    gchar *text = NULL;
    gsize len = 0;

    if (!g_file_get_contents (list[i], &text, &len, NULL))
      goto out;
    g_checksum_update (ck, (const guchar *) text, (gssize) len);
    g_free (text);
  }
  sum = g_strdup (g_checksum_get_string (ck));
out:
  g_checksum_free (ck);
  g_strfreev (list);
  return sum;
}

/* A dump's first line: which tracker config (@config's sha256) and which tracking core (the sha256 of @core, the
 * core's source files, ';'-separated) made its IDs. */
static gchar *
track_dump_header (const gchar * config, const gchar * core)
{
  gchar *config_sum = track_file_sha256 (config), *core_sum = track_files_sha256 (core);
  gchar *head;

  g_assert_nonnull (config_sum);
  g_assert_nonnull (core_sum);
  head = g_strdup_printf ("# config %s core %s\n", config_sum, core_sum);
  g_free (config_sum);
  g_free (core_sum);
  return head;
}

/* The detections as text: a first line from track_dump_header, then one detection per line:
 * "frame class conf x y w h id" (id -1: none). */
static G_GNUC_UNUSED gboolean
track_dets_save (const GArray * dets, const gchar * config, const gchar * core, const gchar * path, GError ** error)
{
  gchar *head = track_dump_header (config, core);
  GString *s = g_string_new (head);
  gboolean ok;

  g_free (head);
  for (guint i = 0; i < dets->len; i++) {
    const TrackDet *d = &g_array_index (dets, TrackDet, i);

    g_string_append_printf (s, "%d %d %.9g %.9g %.9g %.9g %.9g %" G_GINT64_FORMAT "\n", d->frame, d->class_id, d->conf,
        d->x, d->y, d->w, d->h, d->id == TRACK_NO_ID ? (gint64) - 1 : (gint64) d->id);
  }
  ok = g_file_set_contents (path, s->str, (gssize) s->len, error);
  g_string_free (s, TRUE);
  return ok;
}

/* NULL if @path can't be read, a line doesn't parse, or the dump wasn't made with @config and @core as they are now
 * (both NULL: by any). The IDs are the ones saved. */
static G_GNUC_UNUSED GArray *
track_dets_load (const gchar * path, const gchar * config, const gchar * core)
{
  gchar *head = config ? track_dump_header (config, core) : g_strdup ("");
  gchar *text = NULL, **lines;
  GArray *dets;

  if (!g_file_get_contents (path, &text, NULL, NULL) || !g_str_has_prefix (text, head)) {
    g_free (head);
    g_free (text);
    return NULL;
  }
  g_free (head);
  dets = g_array_new (FALSE, FALSE, sizeof (TrackDet));
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    TrackDet d;
    gint64 id;

    if (!*lines[i] || *lines[i] == '#')
      continue;
    if (sscanf (lines[i], "%d %d %g %g %g %g %g %" G_GINT64_MODIFIER "d", &d.frame, &d.class_id, &d.conf, &d.x, &d.y,
            &d.w, &d.h, &id) != 8) {
      g_array_unref (dets);
      dets = NULL;
      break;
    }
    d.id = id < 0 ? TRACK_NO_ID : (guint64) id;
    g_array_append_val (dets, d);
  }
  g_strfreev (lines);
  g_free (text);
  return dets;
}

#endif /* RDS_TEST_TRACKS_H */
