/* rdstracker tracking core: target life cycle, IDs, frames without inference, rejections, IOU association, NvSORT
 * (cascade, Kalman prediction, reported boxes), reset modes, streams, determinism, and a timing printout. Scripted
 * sequences: 100x100 boxes on one row unless a test says otherwise; each detection's user pointer is a small tag.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-tracker-core.h"

#include <math.h>
#include <string.h>

#define TAG(n) GUINT_TO_POINTER (n)

static RdsTrackerDet
det (gfloat x, gfloat y, gfloat w, gfloat h, gint class_id, gfloat confidence, guint tag)
{
  RdsTrackerDet d = {.x = x,.y = y,.w = w,.h = h,.class_id = class_id,.confidence = confidence,.user = TAG (tag) };

  return d;
}

/* a 100x100 box at (x, 100), class 0, confidence 0.9 */
static RdsTrackerDet
sq (gfloat x, guint tag)
{
  return det (x, 100, 100, 100, 0, 0.9f, tag);
}

static RdsTrackerConfig *
config_new (void)
{
  GError *err = NULL;
  RdsTrackerConfig *cfg = rds_tracker_config_load (NULL, &err);

  g_assert_no_error (err);
  return cfg;
}

/* Takes @cfg and frees it: the core keeps its own copy. */
static RdsTrackerCore *
core_new (RdsTrackerConfig * cfg, RdsTrackerIdResetMode mode)
{
  RdsTrackerCoreOptions opts = {.reset_mode = mode };
  RdsTrackerCore *core = rds_tracker_core_new (cfg, &opts);

  rds_tracker_config_free (cfg);
  return core;
}

static RdsTrackerBatchOut out;

/* One frame of one stream, as a batch of one. */
static const RdsTrackerFrameOut *
step (RdsTrackerCore * core, guint64 stream, gboolean done, const RdsTrackerDet * dets, guint n)
{
  RdsTrackerFrameIn in = {.stream_id = stream,.detection_done = done,.dets = dets,.n_dets = n };

  rds_tracker_core_process (core, &in, 1, &out);
  g_assert_cmpuint (out.n_frames, ==, 1);
  return &out.frames[0];
}

#define DETS(...) ((const RdsTrackerDet[]) { __VA_ARGS__ })
#define STEP(core, stream, done, ...) \
  step (core, stream, done, DETS (__VA_ARGS__), G_N_ELEMENTS (DETS (__VA_ARGS__)))
#define EMPTY(core, stream, done) step (core, stream, done, NULL, 0)

/* Streams 0 and 1 in one batch, stream 0 first. */
static const RdsTrackerBatchOut *
batch2 (RdsTrackerCore * core, const RdsTrackerDet * a, guint na, gboolean reset_a, const RdsTrackerDet * b,
    guint nb)
{
  RdsTrackerFrameIn in[] = {
    {.stream_id = 0,.detection_done = TRUE,.reset = reset_a,.dets = a,.n_dets = na},
    {.stream_id = 1,.detection_done = TRUE,.dets = b,.n_dets = nb},
  };

  rds_tracker_core_process (core, in, 2, &out);
  g_assert_cmpuint (out.n_frames, ==, 2);
  return &out;
}

/* The reported target matched to the detection tagged @tag, or NULL. */
static const RdsTrackerTarget *
reported (const RdsTrackerFrameOut * fo, guint tag)
{
  for (guint i = 0; i < fo->n_targets; i++)
    if (fo->targets[i].associated_user == TAG (tag))
      return &fo->targets[i];
  return NULL;
}

static guint64
id_of (const RdsTrackerFrameOut * fo, guint tag)
{
  const RdsTrackerTarget *t = reported (fo, tag);

  if (!t)
    g_error ("the detection tagged %u has no reported target", tag);
  return t->id;
}

/* The frame's rejected list is exactly the tags given, in that order (0-terminated). */
static void
assert_rejected (const RdsTrackerFrameOut * fo, ...)
{
  va_list ap;
  guint n = 0, tag;

  va_start (ap, fo);
  while ((tag = va_arg (ap, guint))) {
    g_assert_cmpuint (n, <, fo->n_rejected);
    g_assert_true (fo->rejected[n] == TAG (tag));
    n++;
  }
  va_end (ap);
  g_assert_cmpuint (fo->n_rejected, ==, n);
}

static void
assert_box (const RdsTrackerTarget * t, gfloat x, gfloat y, gfloat w, gfloat h)
{
  g_assert_cmpfloat (t->x, ==, x);
  g_assert_cmpfloat (t->y, ==, y);
  g_assert_cmpfloat (t->w, ==, w);
  g_assert_cmpfloat (t->h, ==, h);
}

/* --- activation --- */

static void
test_activation (void)
{
  const guint probation[] = { 0, 2, 5 };

  for (guint p = 0; p < G_N_ELEMENTS (probation); p++) {
    RdsTrackerConfig *cfg = config_new ();
    RdsTrackerCore *core;

    cfg->probation_age = probation[p];
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    for (guint f = 0; f < 10; f++) {
      const RdsTrackerFrameOut *fo = STEP (core, 0, TRUE, sq (f, 1));

      g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
      if (f < probation[p]) {
        g_assert_cmpuint (fo->n_targets, ==, 0);
        assert_rejected (fo, 1, 0);
      } else {
        const RdsTrackerTarget *t = &fo->targets[0];

        g_assert_cmpuint (fo->n_targets, ==, 1);
        assert_rejected (fo, 0);
        g_assert_cmpuint (t->id, ==, 0);
        g_assert_cmpuint (t->age, ==, f + 1);
        g_assert_true (t->associated_user == TAG (1));
        g_assert_true (t->last_user == TAG (1));
        g_assert_cmpint (t->class_id, ==, 0);
        g_assert_cmpfloat (t->tracker_confidence, ==, 0.5f);
        assert_box (t, f, 100, 100, 100);
      }
    }
    rds_tracker_core_free (core);
  }
}

/* One counter for all streams; same-batch activations in batch order, then by the matched detection's index. */
static void
test_ids_shared (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerDet a[] = { sq (0, 1), sq (300, 2) }, b[] = { sq (0, 3) };
  RdsTrackerFrameIn in[] = {
    {.stream_id = 7,.detection_done = TRUE,.dets = b,.n_dets = 1},
    {.stream_id = 3,.detection_done = TRUE,.dets = a,.n_dets = 2},
  };
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 0;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  rds_tracker_core_process (core, in, 2, &out);
  g_assert_cmpuint (id_of (&out.frames[0], 3), ==, 0);
  g_assert_cmpuint (id_of (&out.frames[1], 1), ==, 1);
  g_assert_cmpuint (id_of (&out.frames[1], 2), ==, 2);
  rds_tracker_core_free (core);

  /* Created in one order, activated together with their detections in the other order. */
  cfg = config_new ();
  cfg->probation_age = 1;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1), sq (300, 2));
  fo = STEP (core, 0, TRUE, sq (300, 3), sq (0, 4));
  g_assert_cmpuint (fo->n_targets, ==, 2);
  g_assert_cmpuint (fo->targets[0].id, ==, 0);
  g_assert_true (fo->targets[0].associated_user == TAG (3));
  g_assert_cmpuint (fo->targets[1].id, ==, 1);
  g_assert_true (fo->targets[1].associated_user == TAG (4));
  rds_tracker_core_free (core);
}

static void
test_dead_tentative_no_id (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 2;
  cfg->early_termination_age = 1;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  STEP (core, 0, TRUE, sq (500, 2));
  STEP (core, 0, TRUE, sq (500, 2));
  fo = STEP (core, 0, TRUE, sq (500, 2));
  g_assert_cmpuint (id_of (fo, 2), ==, 0);
  rds_tracker_core_free (core);
}

/* --- termination --- */

static void
test_tentative_early_termination (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;

  cfg->probation_age = 5;
  cfg->early_termination_age = 2;
  cfg->max_shadow_tracking_age = 30;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  rds_tracker_core_free (core);
}

static void
test_tentative_shadow_termination (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;

  cfg->probation_age = 10;
  cfg->early_termination_age = 100;
  cfg->max_shadow_tracking_age = 2;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  EMPTY (core, 0, TRUE);
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  rds_tracker_core_free (core);
}

/* An Active target survives exactly maxShadowTrackingAge missed frames; one more and the object comes back as a new
 * target. */
static void
test_active_termination (void)
{
  for (guint missed = 3; missed <= 4; missed++) {
    RdsTrackerConfig *cfg = config_new ();
    RdsTrackerCore *core;
    const RdsTrackerFrameOut *fo;

    cfg->probation_age = 0;
    cfg->max_shadow_tracking_age = 3;
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    g_assert_cmpuint (id_of (STEP (core, 0, TRUE, sq (0, 1)), 1), ==, 0);
    for (guint f = 0; f < missed; f++) {
      fo = EMPTY (core, 0, TRUE);
      g_assert_cmpuint (fo->n_targets, ==, 0);
    }
    g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, missed == 3 ? 1 : 0);
    fo = STEP (core, 0, TRUE, sq (0, 2));
    g_assert_cmpuint (id_of (fo, 2), ==, missed == 3 ? 0 : 1);
    if (missed == 3)
      g_assert_cmpuint (fo->targets[0].age, ==, missed + 2);
    rds_tracker_core_free (core);
  }
}

/* Age passing probationAge on a missed frame doesn't activate: activation needs a match. */
static void
test_activation_needs_match (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 2;
  cfg->early_termination_age = 2;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  STEP (core, 0, TRUE, sq (0, 1));
  fo = EMPTY (core, 0, TRUE);
  g_assert_cmpuint (fo->n_targets, ==, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  fo = STEP (core, 0, TRUE, sq (0, 2));
  g_assert_cmpuint (id_of (fo, 2), ==, 0);
  g_assert_cmpuint (fo->targets[0].age, ==, 4);
  rds_tracker_core_free (core);
}

/* A match resets the shadow age: miss, match, miss survives maxShadowTrackingAge 1. */
static void
test_shadow_reset_on_match (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;

  cfg->probation_age = 0;
  cfg->max_shadow_tracking_age = 1;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (id_of (STEP (core, 0, TRUE, sq (0, 2)), 2), ==, 0);
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (id_of (STEP (core, 0, TRUE, sq (0, 3)), 3), ==, 0);
  EMPTY (core, 0, TRUE);
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  rds_tracker_core_free (core);
}

/* --- frames without inference --- */

/* Inference every 2nd or 3rd frame; the object is missing on the second inference frame. Frames without inference
 * don't add to the shadow age, so maxShadowTrackingAge 1 keeps the target; while Inactive it isn't reported. */
static void
test_interval (void)
{
  for (guint interval = 2; interval <= 3; interval++) {
    RdsTrackerConfig *cfg = config_new ();
    RdsTrackerCore *core;
    guint n_inferred = 0;
    gboolean active = FALSE;

    cfg->probation_age = 0;
    cfg->max_shadow_tracking_age = 1;
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    for (guint f = 0; f < 13; f++) {
      gboolean done = f % interval == 0;
      gboolean present = done && n_inferred != 1;
      const RdsTrackerFrameOut *fo = present ? STEP (core, 0, TRUE, sq (f, 1)) : EMPTY (core, 0, done);

      if (done) {
        n_inferred++;
        active = present;
      }
      g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
      g_assert_cmpuint (fo->n_targets, ==, active ? 1 : 0);
      if (active) {
        g_assert_cmpuint (fo->targets[0].id, ==, 0);
        g_assert_cmpuint (fo->targets[0].age, ==, f + 1);
        g_assert_true (fo->targets[0].associated_user == (done ? TAG (1) : NULL));
      }
    }
    rds_tracker_core_free (core);
  }
}

static void
test_no_inference_report (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 0;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1), sq (300, 2));
  STEP (core, 0, TRUE, sq (10, 3), det (310, 105, 100, 90, 0, 0.9f, 4));
  fo = STEP (core, 0, FALSE, sq (500, 5));
  /* newest ID first, last matched box, no associated detection */
  g_assert_cmpuint (fo->n_targets, ==, 2);
  g_assert_cmpuint (fo->targets[0].id, ==, 1);
  g_assert_cmpuint (fo->targets[1].id, ==, 0);
  assert_box (&fo->targets[0], 310, 105, 100, 90);
  assert_box (&fo->targets[1], 10, 100, 100, 100);
  for (guint i = 0; i < 2; i++) {
    g_assert_null (fo->targets[i].associated_user);
    g_assert_cmpfloat (fo->targets[i].tracker_confidence, ==, 0.5f);
    g_assert_cmpuint (fo->targets[i].age, ==, 3);
  }
  g_assert_true (fo->targets[0].last_user == TAG (4));
  g_assert_true (fo->targets[1].last_user == TAG (3));
  /* detections on a frame without inference are not tracked: rejected */
  assert_rejected (fo, 5, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 2);
  /* even one on an Active target's exact box: the target is still reported, without it */
  fo = STEP (core, 0, FALSE, sq (10, 6));
  assert_rejected (fo, 6, 0);
  g_assert_cmpuint (fo->n_targets, ==, 2);
  g_assert_cmpuint (fo->targets[1].id, ==, 0);
  assert_box (&fo->targets[1], 10, 100, 100, 100);
  g_assert_null (fo->targets[1].associated_user);
  g_assert_true (fo->targets[1].last_user == TAG (3));
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 2);
  rds_tracker_core_free (core);
}

/* Frames without inference advance the probation clock: activation after two matches with probationAge 3. */
static void
test_no_inference_probation (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 3;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  g_assert_cmpuint (EMPTY (core, 0, FALSE)->n_targets, ==, 0);
  g_assert_cmpuint (EMPTY (core, 0, FALSE)->n_targets, ==, 0);
  fo = STEP (core, 0, TRUE, sq (0, 2));
  g_assert_cmpuint (id_of (fo, 2), ==, 0);
  g_assert_cmpuint (fo->targets[0].age, ==, 4);
  rds_tracker_core_free (core);
}

static RdsTrackerCore *core_probation0 (void);

static guint n_copies, n_frees;

static gpointer
copy_label (gconstpointer src, gpointer data)
{
  g_assert_true (data == TAG (42));
  n_copies++;
  return g_strdup (src);
}

static void
free_label (gpointer p)
{
  n_frees++;
  g_free (p);
}

/* With a copy function, last_user is the target's own copy of its last matched detection's user data, freed with
 * the target. */
static void
test_last_user_copy (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCoreOptions opts = {.reset_mode = RDS_TRACKER_ID_RESET_ON_RESET,.user_copy = copy_label,
    .user_copy_data = TAG (42),.user_free = free_label
  };
  RdsTrackerCore *core;
  gchar label_a[] = "car", label_b[] = "truck";
  RdsTrackerDet d = sq (0, 0);
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 1;
  cfg->max_shadow_tracking_age = 0;
  core = rds_tracker_core_new (cfg, &opts);
  rds_tracker_config_free (cfg);
  n_copies = n_frees = 0;
  d.user = label_a;
  step (core, 0, TRUE, &d, 1);
  d.user = label_b;
  fo = step (core, 0, TRUE, &d, 1);
  g_assert_true (fo->targets[0].associated_user == label_b);
  g_assert_cmpstr (fo->targets[0].last_user, ==, "truck");
  g_assert_true (fo->targets[0].last_user != (gpointer) label_b);
  /* the caller's detection may be gone: the copy stays */
  label_b[0] = 'X';
  fo = EMPTY (core, 0, FALSE);
  g_assert_cmpstr (fo->targets[0].last_user, ==, "truck");
  g_assert_cmpuint (n_copies, ==, 2);
  g_assert_cmpuint (n_frees, ==, 1);
  /* terminated (maxShadowTrackingAge 0): its copy is freed */
  EMPTY (core, 0, TRUE);
  g_assert_cmpuint (n_frees, ==, 2);
  d.user = label_a;
  step (core, 0, TRUE, &d, 1);
  rds_tracker_core_free (core);
  g_assert_cmpuint (n_copies, ==, 3);
  g_assert_cmpuint (n_frees, ==, 3);
}

/* Every target's copy is freed when a reset, an EOS, remove_stream or clear ends it. */
static void
test_last_user_free_events (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCoreOptions opts = {.reset_mode = RDS_TRACKER_ID_RESET_BOTH,.user_copy = copy_label,
    .user_copy_data = TAG (42),.user_free = free_label
  };
  RdsTrackerCore *core;
  gchar label[] = "person";
  RdsTrackerDet d = sq (0, 0);
  RdsTrackerFrameIn in[4];

  cfg->probation_age = 0;
  core = rds_tracker_core_new (cfg, &opts);
  rds_tracker_config_free (cfg);
  n_copies = n_frees = 0;
  d.user = label;
  for (guint s = 0; s < 4; s++)
    in[s] = (RdsTrackerFrameIn) {
    .stream_id = s,.detection_done = TRUE,.dets = &d,.n_dets = 1};
  rds_tracker_core_process (core, in, 4, &out);
  g_assert_cmpuint (n_copies, ==, 4);
  rds_tracker_core_stream_reset (core, 0);
  g_assert_cmpuint (n_frees, ==, 1);
  rds_tracker_core_stream_eos (core, 1);
  g_assert_cmpuint (n_frees, ==, 2);
  rds_tracker_core_remove_stream (core, 2);
  g_assert_cmpuint (n_frees, ==, 3);
  rds_tracker_core_clear (core);
  g_assert_cmpuint (n_frees, ==, 4);
  rds_tracker_core_free (core);
  g_assert_cmpuint (n_copies, ==, 4);
  g_assert_cmpuint (n_frees, ==, 4);
}

/* A copy function needs a free function. */
static void
test_user_copy_needs_free (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCoreOptions opts = {.user_copy = copy_label };

  g_test_expect_message ("rdstracker", G_LOG_LEVEL_CRITICAL, "*user_free*");
  g_assert_null (rds_tracker_core_new (cfg, &opts));
  g_test_assert_expected_messages ();
  rds_tracker_config_free (cfg);
}

/* Two frames of one stream in one batch are a caller error: no output. */
static void
test_duplicate_stream_in_batch (void)
{
  RdsTrackerCore *core = core_probation0 ();
  const RdsTrackerDet d = sq (0, 1);
  RdsTrackerFrameIn in[] = {
    {.stream_id = 5,.detection_done = TRUE,.dets = &d,.n_dets = 1},
    {.stream_id = 5,.detection_done = TRUE,.dets = &d,.n_dets = 1},
  };

  g_test_expect_message ("rdstracker", G_LOG_LEVEL_CRITICAL, "*has_duplicate_streams*");
  rds_tracker_core_process (core, in, 2, &out);
  g_test_assert_expected_messages ();
  g_assert_cmpuint (out.n_frames, ==, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 5), ==, 0);
  rds_tracker_core_free (core);
}

/* --- rejections --- */

static void
test_low_confidence (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 0;
  cfg->min_detector_confidence = 0.5f;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  fo = STEP (core, 0, TRUE, det (0, 0, 100, 100, 0, 0.49f, 1), det (300, 0, 100, 100, 0, 0.5f, 2));
  assert_rejected (fo, 1, 0);
  g_assert_cmpuint (id_of (fo, 2), ==, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  /* below the threshold, it doesn't even match an existing target */
  fo = STEP (core, 0, TRUE, det (300, 0, 100, 100, 0, 0.3f, 3));
  g_assert_cmpuint (fo->n_targets, ==, 0);
  assert_rejected (fo, 3, 0);
  rds_tracker_core_free (core);
}

/* minIouDiff4NewTarget compares a new detection with every target, whatever its class and state. */
static void
test_duplicate_ignores_class (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 0;
  cfg->check_class_match = TRUE;
  cfg->min_iou_diff4new_target = 0.5f;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  /* same class, matched: the overlapping class-1 detection is a duplicate of the Active target */
  fo = STEP (core, 0, TRUE, sq (0, 2), det (5, 100, 100, 100, 1, 0.9f, 3));
  g_assert_cmpuint (id_of (fo, 2), ==, 0);
  assert_rejected (fo, 3, 0);
  /* a duplicate of the now Inactive target */
  fo = STEP (core, 0, TRUE, det (5, 100, 100, 100, 1, 0.9f, 4));
  g_assert_cmpuint (fo->n_targets, ==, 0);
  assert_rejected (fo, 4, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  /* below the threshold it is a new target */
  fo = STEP (core, 0, TRUE, det (60, 100, 100, 100, 1, 0.9f, 5));
  g_assert_cmpuint (id_of (fo, 5), ==, 1);
  rds_tracker_core_free (core);
}

/* minIouDiff4NewTarget 0: only overlapping detections are duplicates, lone ones still start targets. */
static void
test_min_iou_diff_zero (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 0;
  cfg->min_iou_diff4new_target = 0;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  fo = STEP (core, 0, TRUE, sq (0, 1), sq (300, 2));
  g_assert_cmpuint (id_of (fo, 1), ==, 0);
  g_assert_cmpuint (id_of (fo, 2), ==, 1);
  /* class 1 can't match the class-0 target, and overlaps it */
  fo = STEP (core, 0, TRUE, sq (0, 3), det (90, 100, 100, 100, 1, 0.9f, 4), sq (600, 5));
  g_assert_cmpuint (id_of (fo, 3), ==, 0);
  g_assert_cmpuint (id_of (fo, 5), ==, 2);
  assert_rejected (fo, 4, 0);
  rds_tracker_core_free (core);
}

/* The duplicate check sees a target created earlier on the same frame. */
static void
test_duplicate_same_frame (void)
{
  RdsTrackerCore *core;
  RdsTrackerConfig *cfg = config_new ();
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 0;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  fo = STEP (core, 0, TRUE, sq (0, 1), sq (5, 2));
  g_assert_cmpuint (id_of (fo, 1), ==, 0);
  assert_rejected (fo, 2, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  rds_tracker_core_free (core);
}

/* Boxes that are not finite or have no area are rejected before association and never start a target. */
static void
test_invalid_boxes (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;
  const gfloat inf = INFINITY, nan = NAN;

  cfg->probation_age = 0;
  cfg->min_iou_diff4new_target = 1.0f;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  fo = STEP (core, 0, TRUE, det (nan, 100, 100, 100, 0, 0.9f, 1), det (0, nan, 100, 100, 0, 0.9f, 2),
      det (0, 100, inf, 100, 0, 0.9f, 3), det (0, 100, 100, nan, 0, 0.9f, 4), det (-inf, 100, 100, 100, 0, 0.9f, 5),
      det (0, 100, 0, 100, 0, 0.9f, 6), det (0, 100, 100, -1, 0, 0.9f, 7),
      det (G_MAXFLOAT, 100, G_MAXFLOAT, 100, 0, 0.9f, 8), det (0, 0, 1e30f, 1e30f, 0, 0.9f, 9));
  assert_rejected (fo, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  /* next to a target, invalid detections match nothing; a huge valid box gives finite scores */
  STEP (core, 0, TRUE, sq (0, 10));
  fo = STEP (core, 0, TRUE, det (nan, nan, nan, nan, 0, 0.9f, 11), det (0, 100, 0, 0, 0, 0.9f, 12),
      det (-1e18f, -1e18f, 2e18f, 2e18f, 0, 0.9f, 13), sq (1, 14));
  g_assert_cmpuint (id_of (fo, 14), ==, 0);
  g_assert_cmpuint (fo->n_targets, ==, 2);
  assert_rejected (fo, 11, 12, 0);
  rds_tracker_core_free (core);
}

static void
test_cap (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  /* Tentative targets count */
  cfg->probation_age = 5;
  cfg->max_targets_per_stream = 2;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  fo = STEP (core, 0, TRUE, sq (0, 1), sq (300, 2), sq (600, 3));
  assert_rejected (fo, 1, 2, 3, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 2);
  rds_tracker_core_free (core);

  /* Inactive (shadow) targets count */
  cfg = config_new ();
  cfg->probation_age = 0;
  cfg->max_targets_per_stream = 2;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1), sq (300, 2));
  fo = STEP (core, 0, TRUE, sq (600, 3));
  g_assert_cmpuint (fo->n_targets, ==, 0);
  assert_rejected (fo, 3, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 2);
  rds_tracker_core_free (core);
}

/* The rejected list holds exactly the detections without a reported target. */
static void
test_rejected_exact (void)
{
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 2;
  cfg->min_detector_confidence = 0.5f;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  STEP (core, 0, TRUE, sq (0, 1));
  fo = STEP (core, 0, TRUE, sq (0, 1), sq (300, 2));
  g_assert_cmpuint (id_of (fo, 1), ==, 0);
  assert_rejected (fo, 2, 0);
  /* low confidence, matches the Tentative target, matches the Active one, duplicate, creates a target */
  fo = STEP (core, 0, TRUE, det (600, 100, 100, 100, 0, 0.2f, 3), sq (300, 4), sq (0, 5), sq (5, 6), sq (900, 7));
  g_assert_cmpuint (fo->n_targets, ==, 1);
  g_assert_cmpuint (id_of (fo, 5), ==, 0);
  assert_rejected (fo, 3, 4, 6, 7, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 3);
  rds_tracker_core_free (core);
}

/* --- association --- */

static RdsTrackerCore *
core_probation0 (void)
{
  RdsTrackerConfig *cfg = config_new ();

  cfg->probation_age = 0;
  return core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
}

/* Two objects crossing: equal scores go to the older target first, then to the lower detection index. */
static void
test_crossing (void)
{
  RdsTrackerCore *core = core_probation0 ();
  const RdsTrackerFrameOut *fo;

  STEP (core, 0, TRUE, sq (0, 1), sq (100, 2));
  /* target 0 (x 0) scores 60/140 with tag 4 (x 40), target 1 (x 100) the same with tag 3 (x 60) */
  fo = STEP (core, 0, TRUE, sq (60, 3), sq (40, 4));
  g_assert_cmpuint (id_of (fo, 3), ==, 1);
  g_assert_cmpuint (id_of (fo, 4), ==, 0);
  g_assert_true (fo->targets[0].associated_user == TAG (3));
  fo = STEP (core, 0, TRUE, sq (100, 5), sq (0, 6));
  g_assert_cmpuint (id_of (fo, 5), ==, 1);
  g_assert_cmpuint (id_of (fo, 6), ==, 0);
  /* both on the same box: all four pairs tie */
  fo = STEP (core, 0, TRUE, sq (50, 7), sq (50, 8));
  g_assert_cmpuint (id_of (fo, 7), ==, 0);
  g_assert_cmpuint (id_of (fo, 8), ==, 1);
  rds_tracker_core_free (core);
}

static void
test_class_match (void)
{
  for (gint check = 0; check <= 1; check++) {
    RdsTrackerConfig *cfg = config_new ();
    RdsTrackerCore *core;
    const RdsTrackerFrameOut *fo;

    cfg->probation_age = 0;
    cfg->check_class_match = check;
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    STEP (core, 0, TRUE, sq (0, 1));
    fo = STEP (core, 0, TRUE, det (0, 100, 100, 100, 1, 0.9f, 2));
    if (check) {
      g_assert_cmpuint (fo->n_targets, ==, 0);
      assert_rejected (fo, 2, 0);
    } else {
      g_assert_cmpuint (id_of (fo, 2), ==, 0);
      g_assert_cmpint (fo->targets[0].class_id, ==, 1);
      /* the target keeps the new class on frames without inference */
      fo = EMPTY (core, 0, FALSE);
      g_assert_cmpint (fo->targets[0].class_id, ==, 1);
    }
    rds_tracker_core_free (core);
  }
}

static RdsTrackerCore *
core_size_only (gfloat min_size_sim)
{
  RdsTrackerConfig *cfg = config_new ();

  cfg->probation_age = 0;
  cfg->matching_score_weight4iou = 0;
  cfg->matching_score_weight4size_similarity = 1;
  cfg->min_matching_score4size_similarity = min_size_sim;
  cfg->min_iou_diff4new_target = 1.0f;
  return core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
}

/* Size similarity is the area ratio: 200x50 against 100x100 is 1 (per axis it would be 0.25). */
static void
test_size_similarity (void)
{
  RdsTrackerCore *core = core_size_only (0.6f);
  const RdsTrackerFrameOut *fo;

  STEP (core, 0, TRUE, sq (0, 1));
  fo = STEP (core, 0, TRUE, det (0, 100, 200, 50, 0, 0.9f, 2));
  g_assert_cmpuint (id_of (fo, 2), ==, 0);
  rds_tracker_core_free (core);

  /* half the area: 0.5 < 0.6, so a new target */
  core = core_size_only (0.6f);
  STEP (core, 0, TRUE, sq (0, 1));
  fo = STEP (core, 0, TRUE, det (0, 100, 100, 50, 0, 0.9f, 2));
  g_assert_cmpuint (id_of (fo, 2), ==, 1);
  rds_tracker_core_free (core);
}

/* With every minimum at 0, a pair needs some overlap: disjoint boxes don't match on size alone. */
static void
test_iou_positive (void)
{
  RdsTrackerCore *core = core_size_only (0);
  const RdsTrackerFrameOut *fo;

  STEP (core, 0, TRUE, sq (0, 1));
  fo = STEP (core, 0, TRUE, sq (500, 2));
  g_assert_cmpuint (id_of (fo, 2), ==, 1);
  fo = STEP (core, 0, TRUE, sq (590, 3));
  g_assert_cmpuint (id_of (fo, 3), ==, 1);
  rds_tracker_core_free (core);
}

/* --- NvSORT --- */

/* NvSORT with height-proportional noise and the aspect ratio, as in DeepStream's NvSORT sample; probationAge 0. */
static RdsTrackerConfig *
nvsort_config (void)
{
  RdsTrackerConfig *cfg = config_new ();

  cfg->tracker_type = RDS_TRACKER_TYPE_NVSORT;
  cfg->association_matcher_type = 1;
  cfg->state_estimator_type = 2;
  cfg->noise_weight_var4loc = 0.05f;
  cfg->noise_weight_var4vel = 0.00625f;
  cfg->use_aspect_ratio = TRUE;
  cfg->use_prediction4assoc = TRUE;
  cfg->probation_age = 0;
  return cfg;
}

/* Stage 1 takes confirmed detections first, even over a better tentative one; stage 2 then lets a tentative
 * detection (confidence below 0.5) keep an Active target, at minMatchingScore4TentativeIou, and never an Inactive
 * one. */
static void
test_nvsort_stage2 (void)
{
  RdsTrackerConfig *cfg = nvsort_config ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->min_matching_score4tentative_iou = 0.5f;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  g_assert_cmpuint (id_of (STEP (core, 0, TRUE, sq (0, 1)), 1), ==, 0);
  /* the confirmed detection (IOU 0.54) wins over the tentative one on the target's box (IOU 1) */
  fo = STEP (core, 0, TRUE, det (0, 100, 100, 100, 0, 0.4f, 2), sq (30, 3));
  g_assert_cmpuint (id_of (fo, 3), ==, 0);
  assert_rejected (fo, 2, 0);
  /* a tentative detection alone keeps the Active target */
  fo = STEP (core, 0, TRUE, det (30, 100, 100, 100, 0, 0.4f, 4));
  g_assert_cmpuint (id_of (fo, 4), ==, 0);
  /* below minMatchingScore4TentativeIou (about 0.3 with the prediction): not matched, and no new target */
  fo = STEP (core, 0, TRUE, det (100, 100, 100, 100, 0, 0.4f, 5));
  g_assert_cmpuint (fo->n_targets, ==, 0);
  assert_rejected (fo, 5, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  /* the target is Inactive now: a tentative detection on its box doesn't bring it back */
  fo = STEP (core, 0, TRUE, det (30, 100, 100, 100, 0, 0.4f, 6));
  g_assert_cmpuint (fo->n_targets, ==, 0);
  assert_rejected (fo, 6, 0);
  /* a confirmed one does */
  fo = STEP (core, 0, TRUE, sq (30, 7));
  g_assert_cmpuint (id_of (fo, 7), ==, 0);
  rds_tracker_core_free (core);
}

/* Stage 3 matches confirmed detections with Tentative targets on IOU alone, at minMatchingScore4TentativeIou: the
 * joint minimum (minMatchingScore4Iou 0.9) doesn't apply there. Tentative detections never match Tentative targets. */
static void
test_nvsort_stage3 (void)
{
  RdsTrackerConfig *cfg = nvsort_config ();
  RdsTrackerCore *core;
  const RdsTrackerFrameOut *fo;

  cfg->probation_age = 2;
  cfg->min_matching_score4iou = 0.9f;
  cfg->min_matching_score4tentative_iou = 0.5f;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  /* IOU 0.82: matched in stage 3, not a duplicate creating nothing */
  fo = STEP (core, 0, TRUE, sq (10, 2));
  assert_rejected (fo, 2, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 1);
  fo = STEP (core, 0, TRUE, sq (20, 3));
  g_assert_cmpuint (id_of (fo, 3), ==, 0);
  g_assert_cmpuint (fo->targets[0].age, ==, 3);
  rds_tracker_core_free (core);

  /* a tentative detection on a Tentative target's box: no match, so the target misses frames and dies early */
  cfg = nvsort_config ();
  cfg->probation_age = 2;
  cfg->early_termination_age = 2;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  STEP (core, 0, TRUE, sq (0, 1));
  assert_rejected (STEP (core, 0, TRUE, det (0, 100, 100, 100, 0, 0.4f, 2)), 2, 0);
  assert_rejected (STEP (core, 0, TRUE, det (0, 100, 100, 100, 0, 0.4f, 3)), 3, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  rds_tracker_core_free (core);
}

/* A tentative detection alone never creates a target (DeepStream 8.0 behaviour); the IOU tracker creates one. */
static void
test_nvsort_tentative_no_target (void)
{
  for (guint nvsort = 0; nvsort <= 1; nvsort++) {
    RdsTrackerConfig *cfg = nvsort ? nvsort_config () : config_new ();
    RdsTrackerCore *core;
    const RdsTrackerFrameOut *fo = NULL;

    cfg->probation_age = 0;
    cfg->min_detector_confidence = 0.3f;
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    for (guint f = 0; f < 5; f++) {
      fo = STEP (core, 0, TRUE, det (0, 100, 100, 100, 0, 0.4f, 1));
      if (nvsort) {
        g_assert_cmpuint (fo->n_targets, ==, 0);
        assert_rejected (fo, 1, 0);
      } else {
        g_assert_cmpuint (id_of (fo, 1), ==, 0);
      }
    }
    g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, nvsort ? 0 : 1);
    /* at tentativeDetectorConfidence a detection is confirmed */
    fo = STEP (core, 0, TRUE, det (500, 100, 100, 100, 0, 0.5f, 2));
    g_assert_cmpuint (id_of (fo, 2), ==, nvsort ? 0 : 1);
    rds_tracker_core_free (core);
  }
}

/* A box moving 30 px a frame, detected on every frame for 10 frames, then on every 4th: 120 px between detections,
 * more than its width. NvSORT's prediction keeps the ID; the IOU tracker, on the last matched box, starts a new
 * target on every detection. */
static void
test_nvsort_interval_gap (void)
{
  for (guint nvsort = 0; nvsort <= 1; nvsort++) {
    RdsTrackerConfig *cfg = nvsort ? nvsort_config () : config_new ();
    RdsTrackerCore *core;
    guint n_ids = 0;
    guint64 last_id = G_MAXUINT64;

    cfg->probation_age = 0;
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    for (guint f = 0; f < 40; f++) {
      gboolean done = f < 10 || f % 4 == 0;
      const RdsTrackerFrameOut *fo = done ? STEP (core, 0, TRUE, sq (30 * f, 1)) : EMPTY (core, 0, FALSE);

      if (!done)
        continue;
      if (id_of (fo, 1) != last_id) {
        last_id = id_of (fo, 1);
        n_ids++;
      }
    }
    g_test_message ("%s: %u IDs", nvsort ? "NvSORT" : "IOU", n_ids);
    if (nvsort)
      g_assert_cmpuint (n_ids, ==, 1);
    else
      g_assert_cmpuint (n_ids, >=, 7);
    rds_tracker_core_free (core);
  }
}

/* Matched frames report the detector box exactly (DeepStream 8.0 behaviour), while the filter takes it: the next
 * frame without inference reports the prediction, which keeps moving with the object's velocity. */
static void
test_nvsort_reported_boxes (void)
{
  RdsTrackerCore *core = core_new (nvsort_config (), RDS_TRACKER_ID_RESET_ON_RESET);
  const RdsTrackerFrameOut *fo;
  gfloat x = 0;

  for (guint f = 0; f < 12; f++) {
    RdsTrackerDet d = det (40.25f + 10.5f * f, 200.75f - 2.25f * f, 80.5f + 0.5f * f, 120.25f + 0.25f * f, 0, 0.9f, 1);

    fo = step (core, 0, TRUE, &d, 1);
    g_assert_cmpuint (fo->n_targets, ==, 1);
    assert_box (&fo->targets[0], d.x, d.y, d.w, d.h);
  }
  x = fo->targets[0].x;
  for (guint f = 0; f < 3; f++) {
    fo = EMPTY (core, 0, FALSE);
    g_assert_cmpuint (fo->n_targets, ==, 1);
    g_assert_null (fo->targets[0].associated_user);
    g_test_message ("prediction %u: %.3f %.3f %.3f %.3f", f, fo->targets[0].x, fo->targets[0].y, fo->targets[0].w,
        fo->targets[0].h);
    /* about 10.5 px a frame to the right, 2.25 up */
    g_assert_cmpfloat (fo->targets[0].x - x, >, 9);
    g_assert_cmpfloat (fo->targets[0].x - x, <, 12);
    g_assert_cmpfloat (fo->targets[0].y, <, 200.75f - 2.25f * (11 + f));
    g_assert_cmpfloat (fo->targets[0].tracker_confidence, ==, 0.5f);
    x = fo->targets[0].x;
  }
  /* back to the detector box on the next match */
  fo = STEP (core, 0, TRUE, det (40.25f + 10.5f * 15, 200.75f - 2.25f * 15, 88, 124, 0, 0.9f, 2));
  assert_box (reported (fo, 2), 40.25f + 10.5f * 15, 200.75f - 2.25f * 15, 88, 124);
  rds_tracker_core_free (core);
}

/* A box accelerating to 60 px a frame: against the last matched box its IOU falls to 0.25, below
 * minMatchingScore4Iou 0.3, so with usePrediction4Assoc 0 it gets a new ID; the prediction keeps it. */
static void
test_nvsort_use_prediction (void)
{
  for (guint use = 0; use <= 1; use++) {
    RdsTrackerConfig *cfg = nvsort_config ();
    RdsTrackerCore *core;
    gfloat x = 0, speed = 0;
    guint n_ids = 0;
    guint64 last_id = G_MAXUINT64;

    cfg->use_prediction4assoc = use;
    cfg->min_matching_score4iou = 0.3f;
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    for (guint f = 0; f < 30; f++) {
      const RdsTrackerFrameOut *fo = STEP (core, 0, TRUE, sq (x, 1));

      if (id_of (fo, 1) != last_id) {
        last_id = id_of (fo, 1);
        n_ids++;
      }
      speed = MIN (speed + 5, 60);
      x += speed;
    }
    g_test_message ("usePrediction4Assoc %u: %u IDs", use, n_ids);
    if (use)
      g_assert_cmpuint (n_ids, ==, 1);
    else
      g_assert_cmpuint (n_ids, >, 1);
    rds_tracker_core_free (core);
  }
}

/* Observed DeepStream 8.0 output for this scenario (a 100x100 box moving 10 px a frame, detected on even frames,
 * inference every second frame, with the NvSORT config below): the predicted x on the odd frames 3 to 29. */
static const gfloat ds80_predicted_x[] = {
  121.2072f, 145.2672f, 168.7500f, 190.1781f, 210.5359f, 230.5069f, 250.3831f, 270.2644f, 290.1749f, 310.1136f,
  330.0735f, 350.0477f, 370.0312f, 390.0207f,
};

/* A sanity check against DeepStream 8.0, not an exactness test: our predictions on the frames without inference
 * are within 1 px of the observed ones on the first three and within 1.5 px after (the Kalman filter's matrices are
 * not documented: ours follow DeepSORT). */
static void
test_nvsort_ds80_predictions (void)
{
  RdsTrackerConfig *cfg = nvsort_config ();
  RdsTrackerCore *core;
  gdouble worst = 0;

  cfg->min_detector_confidence = 0.3f;
  cfg->max_targets_per_stream = 30;
  cfg->min_iou_diff4new_target = 0.5f;
  cfg->probation_age = 1;
  cfg->max_shadow_tracking_age = 1;
  cfg->early_termination_age = 3;
  cfg->check_class_match = TRUE;
  cfg->min_matching_score4overall = 0;
  cfg->min_matching_score4size_similarity = 0;
  cfg->min_matching_score4iou = 0.1f;
  cfg->matching_score_weight4size_similarity = 0;
  cfg->matching_score_weight4iou = 1;
  cfg->tentative_detector_confidence = 0.5f;
  cfg->min_matching_score4tentative_iou = 0.3f;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  for (guint f = 0; f < 30; f++) {
    const RdsTrackerFrameOut *fo;

    if (f % 2) {
      fo = EMPTY (core, 0, FALSE);
    } else {
      RdsTrackerDet d = det (100 + 10 * f, 300, 100, 100, 0, 0.9f, 1);

      fo = step (core, 0, TRUE, &d, 1);
    }
    /* Tentative on frames 0 and 1, ID 0 from frame 2 */
    g_assert_cmpuint (fo->n_targets, ==, f < 2 ? 0 : 1);
    if (f < 2)
      continue;
    g_assert_cmpuint (fo->targets[0].id, ==, 0);
    if (f % 2 == 0) {
      assert_box (&fo->targets[0], 100 + 10 * f, 300, 100, 100);
    } else {
      gdouble err = fo->targets[0].x - ds80_predicted_x[(f - 3) / 2];

      g_test_message ("frame %u: x %.4f, DeepStream 8.0 %.4f, difference %+.4f", f, fo->targets[0].x,
          ds80_predicted_x[(f - 3) / 2], err);
      g_assert_cmpfloat (fabs (err), <, (f <= 7 ? 1.0 : 1.5));
      worst = MAX (worst, fabs (err));
      g_assert_cmpfloat (fabs (fo->targets[0].y - 300), <, 1e-3);
      g_assert_cmpfloat (fabs (fo->targets[0].w - 100), <, 1e-3);
      g_assert_cmpfloat (fabs (fo->targets[0].h - 100), <, 1e-3);
    }
  }
  g_test_message ("largest difference: %.4f px", worst);
  rds_tracker_core_free (core);
}

/* --- reset modes --- */

static RdsTrackerCore *
core_mode (RdsTrackerIdResetMode mode)
{
  RdsTrackerConfig *cfg = config_new ();

  cfg->probation_age = 0;
  return core_new (cfg, mode);
}

#define BATCH2(core, reset_a, a, b) batch2 (core, a, G_N_ELEMENTS (a), reset_a, b, G_N_ELEMENTS (b))

static void
test_reset_mode0 (void)
{
  RdsTrackerCore *core = core_mode (RDS_TRACKER_ID_RESET_NONE);
  const RdsTrackerDet a[] = { sq (0, 1) }, b[] = { sq (0, 2) };
  const RdsTrackerBatchOut *bo = BATCH2 (core, FALSE, a, b);

  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 0);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 1);
  rds_tracker_core_stream_reset (core, 0);
  rds_tracker_core_stream_eos (core, 0);
  bo = BATCH2 (core, TRUE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 0);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 1);
  rds_tracker_core_free (core);
}

static void
test_reset_mode1 (void)
{
  RdsTrackerCore *core = core_mode (RDS_TRACKER_ID_RESET_ON_RESET);
  const RdsTrackerDet a[] = { sq (0, 1) }, b[] = { sq (0, 2) };
  const RdsTrackerBatchOut *bo = BATCH2 (core, FALSE, a, b);

  rds_tracker_core_stream_eos (core, 0);
  bo = BATCH2 (core, FALSE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 0);
  rds_tracker_core_stream_reset (core, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 1), ==, 1);
  bo = BATCH2 (core, FALSE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 2);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 1);
  /* a frame flagged reset does the same */
  bo = BATCH2 (core, TRUE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 3);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 1);
  rds_tracker_core_free (core);
}

/* Per-stream IDs: counter minus the stream's base, the counter value at the stream's first activation. */
static void
test_reset_mode2 (void)
{
  RdsTrackerCore *core = core_mode (RDS_TRACKER_ID_RESET_ON_EOS);
  const RdsTrackerDet a[] = { sq (0, 1) }, b[] = { sq (0, 2) };
  const RdsTrackerDet a2[] = { sq (0, 1), sq (300, 3) }, b2[] = { sq (0, 2), sq (300, 4) };
  const RdsTrackerBatchOut *bo = BATCH2 (core, FALSE, a, b);

  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 0);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 0);
  bo = BATCH2 (core, FALSE, a2, b2);
  g_assert_cmpuint (id_of (&bo->frames[0], 3), ==, 2);
  g_assert_cmpuint (id_of (&bo->frames[1], 4), ==, 2);
  /* a reset changes nothing in mode 2 */
  rds_tracker_core_stream_reset (core, 0);
  bo = BATCH2 (core, TRUE, a2, b2);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 0);
  g_assert_cmpuint (id_of (&bo->frames[0], 3), ==, 2);
  /* EOS: stream 0 starts again at 0, stream 1 is untouched */
  rds_tracker_core_stream_eos (core, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  bo = BATCH2 (core, FALSE, a, b2);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 0);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 0);
  g_assert_cmpuint (id_of (&bo->frames[1], 4), ==, 2);
  bo = BATCH2 (core, FALSE, a2, b2);
  g_assert_cmpuint (id_of (&bo->frames[0], 3), ==, 1);
  rds_tracker_core_free (core);
}

static void
test_reset_mode3 (void)
{
  RdsTrackerCore *core = core_mode (RDS_TRACKER_ID_RESET_BOTH);
  const RdsTrackerDet a[] = { sq (0, 1) }, b[] = { sq (0, 2) };
  const RdsTrackerBatchOut *bo = BATCH2 (core, FALSE, a, b);

  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 0);
  /* reset: new targets, the counter continues (2 - base 0) */
  rds_tracker_core_stream_reset (core, 0);
  bo = BATCH2 (core, FALSE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 2);
  bo = BATCH2 (core, TRUE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 3);
  /* EOS: back to 0 */
  rds_tracker_core_stream_eos (core, 0);
  bo = BATCH2 (core, FALSE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 0);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 0);
  rds_tracker_core_free (core);
}

/* --- a random scenario, shared by the ID, determinism and timing tests --- */

typedef void (*FrameCheck) (const RdsTrackerFrameIn * in, guint n, const RdsTrackerBatchOut * bo, gpointer data);

/* @n_streams streams of @n_objs objects on a grid, drifting right; each detection is missed 10% of the time, a
 * frame has no inference 20% of the time (its detections come from elsewhere: the core drops them) and, with @events, a stream gets a reset or an EOS now and then. */
static void
run_scenario (RdsTrackerCore * core, guint n_streams, guint n_objs, guint n_frames, guint32 seed, gboolean events,
    FrameCheck check, gpointer data)
{
  GRand *rand = g_rand_new_with_seed (seed);
  RdsTrackerDet *dets = g_new0 (RdsTrackerDet, n_streams * n_objs);
  RdsTrackerFrameIn *in = g_new0 (RdsTrackerFrameIn, n_streams);

  for (guint f = 0; f < n_frames; f++) {
    for (guint s = 0; s < n_streams; s++) {
      RdsTrackerDet *sd = dets + s * n_objs;
      guint n = 0;

      if (events && g_rand_int_range (rand, 0, 100) == 0) {
        if (g_rand_boolean (rand))
          rds_tracker_core_stream_reset (core, s);
        else
          rds_tracker_core_stream_eos (core, s);
      }
      in[s] = (RdsTrackerFrameIn) {
      .stream_id = s,.frame_num = f,.detection_done = g_rand_int_range (rand, 0, 5) != 0,.dets = sd};
      for (guint i = 0; i < n_objs; i++) {
        if (g_rand_int_range (rand, 0, 10) == 0)
          continue;
        sd[n] = det (20 + (i % 10) * 150 + f * (0.5f + 0.1f * (i % 3)) + (gfloat) g_rand_double_range (rand, -2, 2),
            20 + (i / 10) * 150 + (gfloat) g_rand_double_range (rand, -2, 2), 80, 80, i % 3,
            (gfloat) g_rand_double_range (rand, 0.3, 1.0), s * n_objs + i + 1);
        n++;
      }
      in[s].n_dets = n;
    }
    rds_tracker_core_process (core, in, n_streams, &out);
    g_assert_cmpuint (out.n_frames, ==, n_streams);
    if (check)
      check (in, n_streams, &out, data);
  }
  g_free (in);
  g_free (dets);
  g_rand_free (rand);
}

typedef struct
{
  gboolean global;              /* IDs unique across streams (mode 0 and 1) */
  guint64 max_id;               /* mode 1: every new ID is above all earlier ones */
  GHashTable *seen;
  guint n_reported;
} IdCheck;

static void
check_ids (const RdsTrackerFrameIn * in, guint n, const RdsTrackerBatchOut * bo, gpointer data)
{
  IdCheck *c = data;
  GHashTable *batch = g_hash_table_new (g_int64_hash, g_int64_equal);

  for (guint s = 0; s < n; s++) {
    const RdsTrackerFrameOut *fo = &bo->frames[s];
    GHashTable *frame = g_hash_table_new (g_int64_hash, g_int64_equal);

    /* every detection is reported or rejected; on frames without inference, all are rejected */
    if (in[s].detection_done)
      g_assert_cmpuint (fo->n_targets + fo->n_rejected, ==, in[s].n_dets);
    else
      g_assert_cmpuint (fo->n_rejected, ==, in[s].n_dets);
    for (guint i = 0; i < fo->n_targets; i++) {
      const guint64 *id = &fo->targets[i].id;

      g_assert_false (g_hash_table_contains (frame, id));
      g_hash_table_add (frame, (gpointer) id);
      if (c->global) {
        g_assert_false (g_hash_table_contains (batch, id));
        g_hash_table_add (batch, (gpointer) id);
        if (!g_hash_table_contains (c->seen, id)) {
          g_assert_true (c->max_id == G_MAXUINT64 || *id > c->max_id);
          c->max_id = *id;
          g_hash_table_add (c->seen, g_memdup2 (id, sizeof *id));
        }
      }
      c->n_reported++;
    }
    g_hash_table_unref (frame);
  }
  g_hash_table_unref (batch);
}

/* Random resets and EOS on three streams: never two live targets with one ID (per stream in modes 2 and 3), and in
 * modes 0 and 1 the shared counter never goes back. */
static void
test_no_duplicate_ids (void)
{
  for (guint mode = 0; mode <= 3; mode++) {
    RdsTrackerConfig *cfg = config_new ();
    IdCheck c = {.global = mode <= 1,.max_id = G_MAXUINT64,
      .seen = g_hash_table_new_full (g_int64_hash, g_int64_equal, g_free, NULL)
    };
    RdsTrackerCore *core;

    cfg->probation_age = 2;
    cfg->max_shadow_tracking_age = 5;
    core = core_new (cfg, mode);
    run_scenario (core, 3, 20, 400, 1234 + mode, TRUE, check_ids, &c);
    g_test_message ("mode %u: %u reported targets", mode, c.n_reported);
    if (c.global)
      g_assert_cmpuint (g_hash_table_size (c.seen), >, 20);
    g_assert_cmpuint (c.n_reported, >, 1000);
    g_hash_table_unref (c.seen);
    rds_tracker_core_free (core);
  }
}

/* --- IDs and streams --- */

static void
test_unique_id (void)
{
  const RdsTrackerDet a[] = { sq (0, 1) }, b[] = { sq (0, 2) };
  RdsTrackerConfig *cfg = config_new ();
  RdsTrackerCore *core;
  const RdsTrackerBatchOut *bo;
  guint64 id_a, id_b;

  /* fixed upper half */
  cfg->probation_age = 0;
  cfg->use_unique_id = TRUE;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  rds_tracker_core_set_unique_id_upper (core, 0xdeadbeef);
  bo = BATCH2 (core, FALSE, a, b);
  g_assert_cmphex (id_of (&bo->frames[0], 1), ==, G_GUINT64_CONSTANT (0xdeadbeef00000000));
  g_assert_cmphex (id_of (&bo->frames[1], 2), ==, G_GUINT64_CONSTANT (0xdeadbeef00000001));
  rds_tracker_core_free (core);

  /* random: one upper half for every stream, the counter below */
  cfg = config_new ();
  cfg->probation_age = 0;
  cfg->use_unique_id = TRUE;
  core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
  bo = BATCH2 (core, FALSE, a, b);
  id_a = id_of (&bo->frames[0], 1);
  id_b = id_of (&bo->frames[1], 2);
  g_test_message ("useUniqueID upper half: 0x%08x", (guint32) (id_a >> 32));
  g_assert_cmphex (id_a >> 32, ==, id_b >> 32);
  g_assert_cmphex (id_a & G_MAXUINT32, ==, 0);
  g_assert_cmphex (id_b & G_MAXUINT32, ==, 1);
  rds_tracker_core_free (core);

  /* off: no upper half */
  core = core_mode (RDS_TRACKER_ID_RESET_ON_RESET);
  bo = BATCH2 (core, FALSE, a, b);
  g_assert_cmphex (id_of (&bo->frames[1], 2), ==, 1);
  rds_tracker_core_free (core);
}

static void
test_remove_stream (void)
{
  RdsTrackerCore *core = core_mode (RDS_TRACKER_ID_RESET_ON_RESET);
  const RdsTrackerDet a[] = { sq (0, 1) }, b[] = { sq (0, 2) };
  const RdsTrackerBatchOut *bo = BATCH2 (core, FALSE, a, b);

  rds_tracker_core_remove_stream (core, 0);
  rds_tracker_core_remove_stream (core, 99);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 1), ==, 1);
  bo = BATCH2 (core, FALSE, a, b);
  g_assert_cmpuint (id_of (&bo->frames[0], 1), ==, 2);
  g_assert_cmpuint (id_of (&bo->frames[1], 2), ==, 1);
  g_assert_cmpuint (bo->frames[1].targets[0].age, ==, 2);
  rds_tracker_core_free (core);
}

static void
test_clear (void)
{
  RdsTrackerCore *core = core_mode (RDS_TRACKER_ID_RESET_ON_RESET);
  const RdsTrackerDet a[] = { sq (0, 1) }, b[] = { sq (0, 2) };
  const RdsTrackerBatchOut *bo = BATCH2 (core, FALSE, a, b);

  rds_tracker_core_clear (core);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), ==, 0);
  g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 1), ==, 0);
  /* new targets; the counter continues, so no ID repeats across the flush */
  bo = BATCH2 (core, FALSE, b, a);
  g_assert_cmpuint (id_of (&bo->frames[0], 2), ==, 2);
  g_assert_cmpuint (id_of (&bo->frames[1], 1), ==, 3);
  g_assert_cmpuint (bo->frames[0].targets[0].age, ==, 1);
  rds_tracker_core_free (core);
}

/* --- determinism and timing --- */

static void
log_batch (const RdsTrackerFrameIn * in, guint n, const RdsTrackerBatchOut * bo, gpointer data)
{
  GString *log = data;

  for (guint s = 0; s < n; s++) {
    const RdsTrackerFrameOut *fo = &bo->frames[s];

    g_string_append_printf (log, "%" G_GUINT64_FORMAT ":", in[s].stream_id);
    for (guint i = 0; i < fo->n_targets; i++) {
      const RdsTrackerTarget *t = &fo->targets[i];

      g_string_append_printf (log, " %" G_GUINT64_FORMAT "/%a,%a,%a,%a/%d/%p/%p/%u", t->id, t->x, t->y, t->w, t->h,
          t->class_id, t->associated_user, t->last_user, t->age);
    }
    g_string_append (log, " |");
    for (guint i = 0; i < fo->n_rejected; i++)
      g_string_append_printf (log, " %p", fo->rejected[i]);
    g_string_append_c (log, '\n');
  }
}

static void
test_determinism (void)
{
  GString *log[2];

  for (guint nvsort = 0; nvsort <= 1; nvsort++) {
    for (guint r = 0; r < 2; r++) {
      RdsTrackerConfig *cfg = nvsort ? nvsort_config () : config_new ();
      RdsTrackerCore *core;

      cfg->probation_age = 3;
      cfg->check_class_match = FALSE;
      core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
      log[r] = g_string_new (NULL);
      run_scenario (core, 2, 30, 300, 99, TRUE, log_batch, log[r]);
      rds_tracker_core_free (core);
    }
    g_assert_cmpuint (log[0]->len, >, 10000);
    g_assert_cmpstr (log[0]->str, ==, log[1]->str);
    g_string_free (log[0], TRUE);
    g_string_free (log[1], TRUE);
  }
}

/* Printed, not asserted: 4 streams of 50 objects for 1000 frames with the default config (IOU) and with NvSORT, room
 * for every object. */
static void
test_timing (void)
{
  for (guint nvsort = 0; nvsort <= 1; nvsort++) {
    RdsTrackerConfig *cfg = nvsort ? nvsort_config () : config_new ();
    RdsTrackerCore *core;
    gint64 start;
    gdouble ms;

    if (nvsort)
      cfg->probation_age = 5;
    cfg->max_targets_per_stream = 100;
    core = core_new (cfg, RDS_TRACKER_ID_RESET_ON_RESET);
    start = g_get_monotonic_time ();
    run_scenario (core, 4, 50, 1000, 7, FALSE, NULL, NULL);
    ms = (g_get_monotonic_time () - start) / 1000.0;
    g_test_message ("%s, 4 streams x 50 objects x 1000 frames: %.1f ms, %.4f ms per batch (scenario included)",
        nvsort ? "NvSORT" : "IOU", ms, ms / 1000);
    g_assert_cmpuint (rds_tracker_core_get_n_targets (core, 0), >=, 40);
    rds_tracker_core_free (core);
  }
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/tracker-core/activation", test_activation);
  g_test_add_func ("/cpu/tracker-core/ids-shared", test_ids_shared);
  g_test_add_func ("/cpu/tracker-core/dead-tentative-no-id", test_dead_tentative_no_id);
  g_test_add_func ("/cpu/tracker-core/activation-needs-match", test_activation_needs_match);
  g_test_add_func ("/cpu/tracker-core/shadow-reset-on-match", test_shadow_reset_on_match);
  g_test_add_func ("/cpu/tracker-core/tentative-early-termination", test_tentative_early_termination);
  g_test_add_func ("/cpu/tracker-core/tentative-shadow-termination", test_tentative_shadow_termination);
  g_test_add_func ("/cpu/tracker-core/active-termination", test_active_termination);
  g_test_add_func ("/cpu/tracker-core/interval", test_interval);
  g_test_add_func ("/cpu/tracker-core/no-inference-report", test_no_inference_report);
  g_test_add_func ("/cpu/tracker-core/no-inference-probation", test_no_inference_probation);
  g_test_add_func ("/cpu/tracker-core/last-user-copy", test_last_user_copy);
  g_test_add_func ("/cpu/tracker-core/last-user-free-events", test_last_user_free_events);
  g_test_add_func ("/cpu/tracker-core/user-copy-needs-free", test_user_copy_needs_free);
  g_test_add_func ("/cpu/tracker-core/duplicate-stream-in-batch", test_duplicate_stream_in_batch);
  g_test_add_func ("/cpu/tracker-core/low-confidence", test_low_confidence);
  g_test_add_func ("/cpu/tracker-core/duplicate-ignores-class", test_duplicate_ignores_class);
  g_test_add_func ("/cpu/tracker-core/min-iou-diff-zero", test_min_iou_diff_zero);
  g_test_add_func ("/cpu/tracker-core/duplicate-same-frame", test_duplicate_same_frame);
  g_test_add_func ("/cpu/tracker-core/invalid-boxes", test_invalid_boxes);
  g_test_add_func ("/cpu/tracker-core/cap", test_cap);
  g_test_add_func ("/cpu/tracker-core/rejected-exact", test_rejected_exact);
  g_test_add_func ("/cpu/tracker-core/crossing", test_crossing);
  g_test_add_func ("/cpu/tracker-core/class-match", test_class_match);
  g_test_add_func ("/cpu/tracker-core/size-similarity", test_size_similarity);
  g_test_add_func ("/cpu/tracker-core/iou-positive", test_iou_positive);
  g_test_add_func ("/cpu/tracker-core/nvsort-stage2", test_nvsort_stage2);
  g_test_add_func ("/cpu/tracker-core/nvsort-stage3", test_nvsort_stage3);
  g_test_add_func ("/cpu/tracker-core/nvsort-tentative-no-target", test_nvsort_tentative_no_target);
  g_test_add_func ("/cpu/tracker-core/nvsort-interval-gap", test_nvsort_interval_gap);
  g_test_add_func ("/cpu/tracker-core/nvsort-reported-boxes", test_nvsort_reported_boxes);
  g_test_add_func ("/cpu/tracker-core/nvsort-use-prediction", test_nvsort_use_prediction);
  g_test_add_func ("/cpu/tracker-core/nvsort-ds80-predictions", test_nvsort_ds80_predictions);
  g_test_add_func ("/cpu/tracker-core/reset-mode0", test_reset_mode0);
  g_test_add_func ("/cpu/tracker-core/reset-mode1", test_reset_mode1);
  g_test_add_func ("/cpu/tracker-core/reset-mode2", test_reset_mode2);
  g_test_add_func ("/cpu/tracker-core/reset-mode3", test_reset_mode3);
  g_test_add_func ("/cpu/tracker-core/no-duplicate-ids", test_no_duplicate_ids);
  g_test_add_func ("/cpu/tracker-core/unique-id", test_unique_id);
  g_test_add_func ("/cpu/tracker-core/remove-stream", test_remove_stream);
  g_test_add_func ("/cpu/tracker-core/clear", test_clear);
  g_test_add_func ("/cpu/tracker-core/determinism", test_determinism);
  g_test_add_func ("/cpu/tracker-core/timing", test_timing);
  return g_test_run ();
}
