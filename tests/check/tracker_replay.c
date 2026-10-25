/* The tracking core on recorded and scripted detections, with the sample tracker configs (samples/tracker):
 * - the street clip's YOLO11n detections that pipeline_tracker saves, replayed through the core: a consistency check,
 *   not a quality one. The core must give exactly the IDs the element gave (the element adds nothing to the core's
 *   decisions), and the metrics table is printed. Skipped unless pipeline_tracker has run in this build directory
 *   with the configs and the core library as they are now (the dump's header records both);
 * - a scripted scene with known identities (noisy boxes, misses, occlusions, crossings): ID switches and coverage
 *   against the truth.
 * With RDS_TRACKER_GRID=1, also the parameter grid the sample configs were chosen from, on both.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tracker-core.h"
#include "rds-test-tracks.h"

#include <glib/gstdio.h>
#include <unistd.h>

#define NVSORT_CONFIG RDS_TEST_SAMPLES "/tracker/config_tracker_NvSORT.yml"
#define IOU_CONFIG RDS_TEST_SAMPLES "/tracker/config_tracker_IOU.yml"
#define DUMP_DIR RDS_TEST_BUILD_DIR "/pipeline-tracker"

static RdsTrackerConfig *
load_config (const gchar * path)
{
  GError *err = NULL;
  RdsTrackerConfig *cfg = rds_tracker_config_load (path, &err);

  g_assert_no_error (err);
  g_assert_cmpuint (cfg->warnings->len, ==, 0);
  return cfg;
}

/* Runs @dets (sorted by frame; one stream, inference on every frame) through a core made from @cfg and writes the
 * IDs it gives into @dets. */
static void
replay (const RdsTrackerConfig * cfg, GArray * dets)
{
  RdsTrackerCore *core = rds_tracker_core_new (cfg, NULL);
  GArray *in = g_array_new (FALSE, FALSE, sizeof (RdsTrackerDet));
  gint n_frames = dets->len ? g_array_index (dets, TrackDet, dets->len - 1).frame + 1 : 0;
  guint i = 0;

  for (gint f = 0; f < n_frames; f++) {
    RdsTrackerFrameIn fi = {.stream_id = 0,.frame_num = f,.detection_done = TRUE };
    RdsTrackerBatchOut out;
    guint first = i;

    g_array_set_size (in, 0);
    for (; i < dets->len && g_array_index (dets, TrackDet, i).frame == f; i++) {
      TrackDet *d = &g_array_index (dets, TrackDet, i);
      RdsTrackerDet td = {.x = d->x,.y = d->y,.w = d->w,.h = d->h,.class_id = d->class_id,.confidence = d->conf,
        .user = GUINT_TO_POINTER (i + 1)
      };

      d->id = TRACK_NO_ID;
      g_array_append_val (in, td);
    }
    fi.dets = (const RdsTrackerDet *) in->data;
    fi.n_dets = in->len;
    rds_tracker_core_process (core, &fi, 1, &out);
    g_assert_cmpuint (out.n_frames, ==, 1);
    for (guint t = 0; t < out.frames[0].n_targets; t++) {
      const RdsTrackerTarget *tg = &out.frames[0].targets[t];
      guint k = GPOINTER_TO_UINT (tg->associated_user) - 1;

      g_assert_nonnull (tg->associated_user);
      g_assert_cmpuint (k, >=, first);
      g_assert_cmpuint (k, <, i);
      g_array_index (dets, TrackDet, k).id = tg->id;
    }
  }
  g_array_unref (in);
  rds_tracker_core_free (core);
}

/* ---- the street clip ---- */

/* The dump of the element run with @config: the core gives the same IDs as the element did. */
static void
check_clip (const gchar * dump_name, const gchar * config)
{
  gchar *path = g_build_filename (DUMP_DIR, dump_name, NULL);
  GArray *dets = track_dets_load (path, config, RDS_TEST_TRACKER_CORE), *ours;
  RdsTrackerConfig *cfg;
  guint n_tracked = 0;

  if (!dets) {
    gchar *msg = g_strdup_printf ("no %s made with %s and the core as they are (run pipeline_tracker in this "
        "build first)", path, config);

    g_test_skip (msg);
    g_free (msg);
    g_free (path);
    return;
  }
  ours = g_array_copy (dets);
  cfg = load_config (config);
  replay (cfg, ours);
  for (guint i = 0; i < dets->len; i++) {
    const TrackDet *a = &g_array_index (dets, TrackDet, i), *b = &g_array_index (ours, TrackDet, i);

    if (a->id != b->id)
      g_error ("detection %u (frame %d): the element gave ID %" G_GINT64_FORMAT ", the core %" G_GINT64_FORMAT, i,
          a->frame, a->id == TRACK_NO_ID ? -1 : (gint64) a->id, b->id == TRACK_NO_ID ? -1 : (gint64) b->id);
    n_tracked += a->id != TRACK_NO_ID;
  }
  g_assert_cmpuint (n_tracked, >, 0);
  track_print_table (config, ours, cfg->probation_age, NULL);
  rds_tracker_config_free (cfg);
  g_array_unref (ours);
  g_array_unref (dets);
  g_free (path);
}

static void
test_clip_nvsort (void)
{
  check_clip ("crosswalk-nvsort.txt", NVSORT_CONFIG);
}

static void
test_clip_iou (void)
{
  check_clip ("crosswalk-iou.txt", IOU_CONFIG);
}

/* ---- a scripted scene with known identities ---- */

#define SCENE_W 1280
#define SCENE_H 720
#define SCENE_FRAMES 600
#define SCENE_OBJECTS 40

typedef struct
{
  gint class_id, start, end;
  gfloat x, y, w, h, vx, vy;
  gint occluded_from, occluded_to;      /* a stretch of frames without detections */
} SceneObject;

/* Detections of SCENE_OBJECTS objects over SCENE_FRAMES frames, seeded. People (tall, slow) and cars (wide, fast)
 * enter and leave, move at constant velocity (so paths cross), and are detected with 2 px jitter, 3 % size noise
 * and a random confidence in [0.3, 0.95]; each misses 8 % of its frames at random and has one occlusion of 3 to
 * 12 frames. The ID field holds the true identity; the order within a frame is shuffled. */
static GArray *
make_scene (GArray ** truth)
{
  GRand *rand = g_rand_new_with_seed (20261025);
  SceneObject obj[SCENE_OBJECTS];
  GArray *dets = g_array_new (FALSE, FALSE, sizeof (TrackDet));
  GArray *frame = g_array_new (FALSE, FALSE, sizeof (TrackDet));

  for (guint k = 0; k < SCENE_OBJECTS; k++) {
    SceneObject *o = &obj[k];
    gboolean car = g_rand_double (rand) < 0.4;
    gint len;

    o->class_id = car ? TRACK_CLASS_CAR : TRACK_CLASS_PERSON;
    o->w = car ? (gfloat) g_rand_double_range (rand, 120, 220) : (gfloat) g_rand_double_range (rand, 30, 60);
    o->h = car ? o->w * 0.5f : o->w * 2.5f;
    o->vx = car ? (gfloat) g_rand_double_range (rand, 3, 8) : (gfloat) g_rand_double_range (rand, 0.5, 2.5);
    if (g_rand_boolean (rand))
      o->vx = -o->vx;
    o->vy = (gfloat) g_rand_double_range (rand, -0.8, 0.8);
    o->x = o->vx > 0 ? (gfloat) g_rand_double_range (rand, 0, 300) : (gfloat) g_rand_double_range (rand, 700, 1000);
    o->y = (gfloat) g_rand_double_range (rand, 100, SCENE_H - o->h - 100);
    len = g_rand_int_range (rand, 120, 300);
    o->start = g_rand_int_range (rand, 0, SCENE_FRAMES - 60);
    o->end = MIN (o->start + len, SCENE_FRAMES);
    o->occluded_from = g_rand_int_range (rand, o->start + 20, MAX (o->start + 21, o->end - 15));
    o->occluded_to = o->occluded_from + g_rand_int_range (rand, 3, 13);
  }
  for (gint f = 0; f < SCENE_FRAMES; f++) {
    g_array_set_size (frame, 0);
    for (guint k = 0; k < SCENE_OBJECTS; k++) {
      const SceneObject *o = &obj[k];
      gint t = f - o->start;
      gfloat s = 1.0f + (gfloat) g_rand_double_range (rand, -0.03, 0.03);
      TrackDet d;

      if (f < o->start || f >= o->end)
        continue;
      if ((f >= o->occluded_from && f < o->occluded_to) || g_rand_double (rand) < 0.08)
        continue;
      d.frame = f;
      d.class_id = o->class_id;
      d.conf = (gfloat) g_rand_double_range (rand, 0.3, 0.95);
      d.w = o->w * s;
      d.h = o->h * s;
      d.x = o->x + o->vx * t + (gfloat) g_rand_double_range (rand, -2, 2);
      d.y = o->y + o->vy * t + (gfloat) g_rand_double_range (rand, -2, 2);
      d.id = k;
      g_array_append_val (frame, d);
    }
    for (guint i = frame->len; i > 1; i--) {
      guint j = (guint) g_rand_int_range (rand, 0, (gint32) i);
      TrackDet tmp = g_array_index (frame, TrackDet, i - 1);

      g_array_index (frame, TrackDet, i - 1) = g_array_index (frame, TrackDet, j);
      g_array_index (frame, TrackDet, j) = tmp;
    }
    g_array_append_vals (dets, frame->data, frame->len);
  }
  g_array_unref (frame);
  g_rand_free (rand);
  *truth = g_array_copy (dets);
  return dets;
}

typedef struct
{
  guint switches, eligible, covered, merges;
} SceneStats;

/* Against the truth: a switch is an object's tracker ID changing from one tracked detection to its next; a merge is
 * a tracker ID given to more than one object (counted once per ID). Coverage: detections with conf >=
 * TRACK_COVERAGE_CONF that carry an ID, leaving out each object's first @probation detections. */
static void
scene_stats (const GArray * truth, const GArray * tracked, guint probation, SceneStats * st)
{
  guint64 last[SCENE_OBJECTS];
  guint seen[SCENE_OBJECTS] = { 0 };
  GHashTable *owner = g_hash_table_new (g_int64_hash, g_int64_equal);
  GHashTable *merged = g_hash_table_new (g_int64_hash, g_int64_equal);

  memset (st, 0, sizeof (*st));
  for (guint k = 0; k < SCENE_OBJECTS; k++)
    last[k] = TRACK_NO_ID;
  for (guint i = 0; i < truth->len; i++) {
    const TrackDet *t = &g_array_index (truth, TrackDet, i), *d = &g_array_index (tracked, TrackDet, i);
    guint k = (guint) t->id;
    gpointer prev;

    if (seen[k]++ >= probation && d->conf >= TRACK_COVERAGE_CONF) {
      st->eligible++;
      st->covered += d->id != TRACK_NO_ID;
    }
    if (d->id == TRACK_NO_ID)
      continue;
    if (last[k] != TRACK_NO_ID && last[k] != d->id)
      st->switches++;
    last[k] = d->id;
    prev = g_hash_table_lookup (owner, &d->id);
    if (!prev)
      g_hash_table_insert (owner, (gpointer) & d->id, GUINT_TO_POINTER (k + 1));
    else if (GPOINTER_TO_UINT (prev) != k + 1 && g_hash_table_add (merged, (gpointer) & d->id))
      st->merges++;
  }
  g_hash_table_unref (merged);
  g_hash_table_unref (owner);
}

static void
run_scene (const RdsTrackerConfig * cfg, SceneStats * st)
{
  GArray *truth, *dets = make_scene (&truth);

  replay (cfg, dets);
  scene_stats (truth, dets, cfg->probation_age, st);
  g_array_unref (dets);
  g_array_unref (truth);
}

/* The sample configs on the scripted scene: at most 6 ID switches and 6 merged identities, coverage >= 0.95 (measured:
 * NvSORT 4, 4, 0.977; IOU 1, 0, 0.995). */
static void
check_scene (const gchar * config)
{
  RdsTrackerConfig *cfg = load_config (config);
  SceneStats st;

  run_scene (cfg, &st);
  g_test_message ("%s: %u switches, %u merges, coverage %u/%u = %.3f", config, st.switches, st.merges, st.covered,
      st.eligible, (gdouble) st.covered / st.eligible);
  g_assert_cmpuint (st.switches, <=, 6);
  g_assert_cmpuint (st.merges, <=, 6);
  g_assert_cmpfloat ((gdouble) st.covered / st.eligible, >=, 0.95);
  rds_tracker_config_free (cfg);
}

static void
test_scene_nvsort (void)
{
  check_scene (NVSORT_CONFIG);
}

static void
test_scene_iou (void)
{
  check_scene (IOU_CONFIG);
}

/* ---- the grid ---- */

typedef struct
{
  guint probation, shadow;
  gdouble iou_diff, min_iou, min_overall, min_size;
} GridPoint;

/* One run of the grid: @base_path with the grid's keys replaced by @g, on the clip's detections (if saved) and the
 * scene. */
static void
grid_run (const gchar * base_path, GArray * clip, const GridPoint * g)
{
  gchar *text = NULL, *tmp = NULL, **lines;
  GString *s = g_string_new (NULL);
  gint fd = g_file_open_tmp ("tracker-grid-XXXXXX.yml", &tmp, NULL);
  RdsTrackerConfig *cfg;
  SceneStats sc;
  TrackStats cs = { 0 };

  g_assert_cmpint (fd, >=, 0);
  close (fd);
  g_assert_true (g_file_get_contents (base_path, &text, NULL, NULL));
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    const gchar *l = lines[i];

    if (g_str_has_prefix (l, "  probationAge:"))
      g_string_append_printf (s, "  probationAge: %u\n", g->probation);
    else if (g_str_has_prefix (l, "  maxShadowTrackingAge:"))
      g_string_append_printf (s, "  maxShadowTrackingAge: %u\n", g->shadow);
    else if (g_str_has_prefix (l, "  minIouDiff4NewTarget:"))
      g_string_append_printf (s, "  minIouDiff4NewTarget: %g\n", g->iou_diff);
    else if (g_str_has_prefix (l, "  minMatchingScore4Iou:"))
      g_string_append_printf (s, "  minMatchingScore4Iou: %g\n", g->min_iou);
    else if (g_str_has_prefix (l, "  minMatchingScore4Overall:"))
      g_string_append_printf (s, "  minMatchingScore4Overall: %g\n", g->min_overall);
    else if (g_str_has_prefix (l, "  minMatchingScore4SizeSimilarity:"))
      g_string_append_printf (s, "  minMatchingScore4SizeSimilarity: %g\n", g->min_size);
    else
      g_string_append_printf (s, "%s\n", l);
  }
  g_assert_true (g_file_set_contents (tmp, s->str, -1, NULL));
  cfg = load_config (tmp);
  g_assert_cmpuint (cfg->probation_age, ==, g->probation);
  g_assert_cmpfloat (cfg->min_matching_score4overall, ==, (gfloat) g->min_overall);
  g_assert_cmpfloat (cfg->min_matching_score4size_similarity, ==, (gfloat) g->min_size);
  run_scene (cfg, &sc);
  if (clip) {
    replay (cfg, clip);
    track_metrics (clip, -1, g->probation, &cs);
  }
  g_test_message ("  %5u %6u %7.2f %6.2f %7.2f %6.2f | %6u %7.3f %7.1f | %7u %6u %8.3f", g->probation, g->shadow,
      g->iou_diff, g->min_iou, g->min_overall, g->min_size, cs.switches, clip ? track_coverage (&cs) : 0.0,
      cs.mean_length, sc.switches, sc.merges, (gdouble) sc.covered / sc.eligible);
  rds_tracker_config_free (cfg);
  g_unlink (tmp);
  g_free (tmp);
  g_strfreev (lines);
  g_free (text);
  g_string_free (s, TRUE);
}

static void
grid_header (const gchar * title)
{
  g_test_message ("%s", title);
  g_test_message ("  %5s %6s %7s %6s %7s %6s | %6s %7s %7s | %7s %6s %8s", "prob", "shadow", "iouDiff", "minIou",
      "overall", "size", "clipSw", "clipCov", "clipLen", "sceneSw", "merges", "sceneCov");
}

/* Two stages per tracker (each run takes milliseconds): the life-cycle keys and the IOU minimum with the other match
 * minimums at the sample configs' values, then the match minimums (overall score, size similarity, IOU) around the
 * life-cycle values chosen in the first stage. */
static void
test_grid (void)
{
  static const guint probations[] = { 2, 3, 5 };
  static const guint shadows_nvsort[] = { 15, 30, 60 }, shadows_iou[] = { 5, 15, 30 };
  static const gdouble iou_diffs[] = { 0.3, 0.5 }, min_ious[] = { 0.1, 0.3 };
  static const gdouble min_overalls[] = { 0.1, 0.2, 0.3 }, min_sizes[] = { 0.3, 0.5 };
  const gchar *configs[] = { NVSORT_CONFIG, IOU_CONFIG };
  gchar *path;
  GArray *clip;

  if (!g_getenv ("RDS_TRACKER_GRID")) {
    g_test_skip ("set RDS_TRACKER_GRID=1 to run the grid");
    return;
  }
  path = g_build_filename (DUMP_DIR, "crosswalk-nvsort.txt", NULL);
  /* the detections, whatever config and core tracked them */
  clip = track_dets_load (path, NULL, NULL);
  if (!clip)
    g_test_message ("no %s: the scripted scene only", path);
  for (guint c = 0; c < G_N_ELEMENTS (configs); c++) {
    const guint *shadows = c == 0 ? shadows_nvsort : shadows_iou;

    grid_header (configs[c]);
    for (guint p = 0; p < G_N_ELEMENTS (probations); p++)
      for (guint s = 0; s < 3; s++)
        for (guint d = 0; d < G_N_ELEMENTS (iou_diffs); d++)
          for (guint m = 0; m < G_N_ELEMENTS (min_ious); m++) {
            GridPoint g = { probations[p], shadows[s], iou_diffs[d], min_ious[m], 0.2, 0.5 };

            grid_run (configs[c], clip, &g);
          }
    grid_header ("  match minimums:");
    for (guint o = 0; o < G_N_ELEMENTS (min_overalls); o++)
      for (guint z = 0; z < G_N_ELEMENTS (min_sizes); z++)
        for (guint m = 0; m < G_N_ELEMENTS (min_ious); m++) {
          GridPoint g = { 3, 30, 0.5, min_ious[m], min_overalls[o], min_sizes[z] };

          grid_run (configs[c], clip, &g);
        }
  }
  if (clip)
    g_array_unref (clip);
  g_free (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/tracker-replay/clip-nvsort", test_clip_nvsort);
  g_test_add_func ("/cpu/tracker-replay/clip-iou", test_clip_iou);
  g_test_add_func ("/cpu/tracker-replay/scene-nvsort", test_scene_nvsort);
  g_test_add_func ("/cpu/tracker-replay/scene-iou", test_scene_iou);
  g_test_add_func ("/cpu/tracker-replay/grid", test_grid);
  return g_test_run ();
}
