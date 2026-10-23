/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include <gst/gst.h>
#include <rds/rds-meta.h>

#define MAX_VALUES 16

static gint released;
static gint released_by_value[MAX_VALUES];

static void
reset_counters (void)
{
  released = 0;
  memset (released_by_value, 0, sizeof released_by_value);
}

static void
release_user (gpointer data, gpointer user_data)
{
  RdsUserMeta *um = data;
  gint value = *(gint *) um->user_meta_data;

  g_assert_true (user_data == um->base_meta.uContext);
  g_assert_cmpint (value, >=, 0);
  g_assert_cmpint (value, <, MAX_VALUES);
  released_by_value[value]++;
  g_free (um->user_meta_data);
  um->user_meta_data = NULL;
  released++;
}

static RdsUserMeta *
new_user_meta (RdsBatchMeta * b, RdsMetaType type, gint value)
{
  RdsUserMeta *um = rds_acquire_user_meta_from_pool (b);

  um->user_meta_data = g_memdup2 (&value, sizeof value);
  um->base_meta.meta_type = type;
  um->base_meta.uContext = GINT_TO_POINTER (0x1234);
  um->base_meta.release_func = release_user;
  return um;
}

static RdsFrameMeta *
new_frame (RdsBatchMeta * b)
{
  RdsFrameMeta *f = rds_acquire_frame_meta_from_pool (b);

  rds_add_frame_meta_to_batch (b, f);
  return f;
}

static RdsObjectMeta *
new_obj (RdsBatchMeta * b, RdsFrameMeta * f)
{
  RdsObjectMeta *o = rds_acquire_obj_meta_from_pool (b);

  rds_add_obj_meta_to_frame (f, o, NULL);
  return o;
}

static void
test_classifier_labels (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *f = new_frame (b);
  RdsObjectMeta *o = new_obj (b, f);
  RdsClassifierMeta *c = rds_acquire_classifier_meta_from_pool (b);
  RdsLabelInfo *red = rds_acquire_label_info_meta_from_pool (b);
  RdsLabelInfo *lng = rds_acquire_label_info_meta_from_pool (b);
  gchar *long_text = g_strnfill (200, 'x');

  g_assert_cmpint (c->base_meta.meta_type, ==, RDS_CLASSIFIER_META);
  g_assert_cmpint (red->base_meta.meta_type, ==, RDS_LABEL_INFO_META);
  g_assert_true (c->base_meta.batch_meta == b);
  c->classifier_type = "color";
  c->unique_component_id = 3;
  rds_add_classifier_meta_to_object (o, c);

  g_strlcpy (red->result_label, "red", sizeof red->result_label);
  red->result_prob = 0.8f;
  rds_add_label_info_meta_to_classifier (c, red);
  lng->pResult_label = g_strdup (long_text);
  rds_add_label_info_meta_to_classifier (c, lng);

  g_assert_cmpuint (c->num_labels, ==, 2);
  g_assert_true (o->classifier_meta_list->data == c);
  g_assert_true (c->label_info_list->data == red);
  g_assert_true (c->label_info_list->next->data == lng);
  g_assert_cmpstr (((RdsLabelInfo *) c->label_info_list->next->data)->pResult_label, ==, long_text);

  rds_remove_label_info_meta_from_classifier (c, red);
  g_assert_cmpuint (c->num_labels, ==, 1);
  g_assert_true (c->label_info_list->data == lng);
  rds_remove_label_info_meta_from_classifier (c, red);      /* not a member: ignored */
  g_assert_cmpuint (c->num_labels, ==, 1);

  rds_remove_classifier_meta_from_obj (o, c);               /* releases lng and its pResult_label */
  g_assert_null (o->classifier_meta_list);

  /* A second classifier is released with the batch; ASan checks its long label is freed. */
  c = rds_acquire_classifier_meta_from_pool (b);
  rds_add_classifier_meta_to_object (o, c);
  lng = rds_acquire_label_info_meta_from_pool (b);
  lng->pResult_label = g_strdup (long_text);
  rds_add_label_info_meta_to_classifier (c, lng);
  g_free (long_text);
  rds_destroy_batch_meta (b);
}

static void
test_display (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *f = new_frame (b);
  RdsDisplayMeta *d = rds_acquire_display_meta_from_pool (b);

  g_assert_cmpint (d->base_meta.meta_type, ==, RDS_DISPLAY_META);
  d->num_rects = 2;
  d->rect_params[0].left = 1;
  d->rect_params[1].left = 2;
  d->num_labels = 1;
  d->text_params[0].display_text = g_strdup ("HUD");
  d->text_params[0].font_params.font_name = (gchar *) "Serif";
  d->num_lines = 1;
  d->line_params[0].x2 = 10;
  d->num_circles = 1;
  d->circle_params[0].radius = 5;
  d->num_arrows = 1;
  d->arrow_params[0].arrow_head = RDS_OSD_ARROW_BOTH_HEAD;
  /* Text beyond num_labels is still owned and freed. */
  d->text_params[5].display_text = g_strdup ("extra");
  rds_add_display_meta_to_frame (f, d);
  g_assert_true (f->display_meta_list->data == d);

  rds_remove_display_meta_from_frame (f, d);
  g_assert_null (f->display_meta_list);
  d = rds_acquire_display_meta_from_pool (b);
  g_assert_null (d->text_params[0].display_text);           /* recycled and zeroed */
  d->text_params[0].display_text = g_strdup ("HUD");
  d->text_params[0].font_params.font_name = (gchar *) "Serif";
  rds_add_display_meta_to_frame (f, d);
  rds_add_display_meta_to_frame (f, d);                     /* refused: already attached */
  g_assert_cmpuint (g_list_length (f->display_meta_list), ==, 1);
  rds_destroy_batch_meta (b);
}

static void
test_user_levels (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *f = new_frame (b);
  RdsObjectMeta *o = new_obj (b, f);
  RdsMetaType tb = rds_get_user_meta_type ("TEST.LEVELS.BATCH");
  RdsMetaType tf = rds_get_user_meta_type ("TEST.LEVELS.FRAME");
  RdsMetaType to = rds_get_user_meta_type ("TEST.LEVELS.OBJECT");
  RdsUserMeta *ub = new_user_meta (b, tb, 0);
  RdsUserMeta *uf = new_user_meta (b, tf, 1);
  RdsUserMeta *uo = new_user_meta (b, to, 2);

  reset_counters ();
  g_assert_cmpint (tb, !=, tf);
  g_assert_cmpint (tf, !=, to);
  rds_add_user_meta_to_batch (b, ub);
  rds_add_user_meta_to_frame (f, uf);
  rds_add_user_meta_to_obj (o, uo);
  rds_add_user_meta_to_obj (o, uf);                         /* refused: attached to the frame */
  g_assert_true (b->batch_user_meta_list->data == ub);
  g_assert_true (f->frame_user_meta_list->data == uf);
  g_assert_true (o->obj_user_meta_list->data == uo);
  g_assert_cmpuint (g_list_length (o->obj_user_meta_list), ==, 1);
  g_assert_cmpint (((RdsUserMeta *) f->frame_user_meta_list->data)->base_meta.meta_type, ==, tf);

  rds_remove_user_meta_from_object (o, uf);                 /* not a member: ignored */
  rds_remove_user_meta_from_batch (b, uf);                  /* not a member: ignored */
  g_assert_cmpint (released, ==, 0);
  rds_remove_user_meta_from_frame (f, uf);
  g_assert_cmpint (released, ==, 1);
  g_assert_null (f->frame_user_meta_list);

  rds_destroy_batch_meta (b);
  g_assert_cmpint (released, ==, 3);
  for (gint i = 0; i < 3; i++)
    g_assert_cmpint (released_by_value[i], ==, 1);
}

static void
test_cross_batch_refused (void)
{
  RdsBatchMeta *a = rds_create_batch_meta (1);
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *fb = new_frame (b);
  RdsObjectMeta *ob = new_obj (b, fb);
  RdsClassifierMeta *ca = rds_acquire_classifier_meta_from_pool (a);
  RdsClassifierMeta *cb = rds_acquire_classifier_meta_from_pool (b);
  RdsLabelInfo *la = rds_acquire_label_info_meta_from_pool (a);
  RdsDisplayMeta *da = rds_acquire_display_meta_from_pool (a);
  RdsUserMeta *ua = new_user_meta (a, RDS_USER_META, 4);

  reset_counters ();
  rds_add_classifier_meta_to_object (ob, ca);
  rds_add_classifier_meta_to_object (ob, cb);
  rds_add_label_info_meta_to_classifier (cb, la);
  rds_add_display_meta_to_frame (fb, da);
  rds_add_user_meta_to_batch (b, ua);
  rds_add_user_meta_to_frame (fb, ua);
  rds_add_user_meta_to_obj (ob, ua);
  g_assert_cmpuint (g_list_length (ob->classifier_meta_list), ==, 1);
  g_assert_true (ob->classifier_meta_list->data == cb);
  g_assert_null (cb->label_info_list);
  g_assert_cmpuint (cb->num_labels, ==, 0);
  g_assert_null (fb->display_meta_list);
  g_assert_null (b->batch_user_meta_list);
  g_assert_null (fb->frame_user_meta_list);
  g_assert_null (ob->obj_user_meta_list);

  rds_add_user_meta_to_batch (a, ua);
  rds_destroy_batch_meta (b);
  g_assert_cmpint (released, ==, 0);
  rds_destroy_batch_meta (a);
  g_assert_cmpint (released, ==, 1);
}

#define EXTRA_VALUE 8

/* A release_func that calls back into the API: removes a sibling (if any) from the frame's user list, then
 * acquires a new counted user meta (value EXTRA_VALUE) and adds it to the batch. */
typedef struct
{
  RdsBatchMeta *batch;
  RdsFrameMeta *frame;
  RdsUserMeta *sibling;
  gint calls;
} Reentry;

static void
release_reentrant (gpointer data, gpointer user_data)
{
  RdsUserMeta *um = data;
  Reentry *r = user_data;
  RdsUserMeta *extra;

  g_assert_true (um->base_meta.uContext == r);
  if (r->sibling != NULL)
    rds_remove_user_meta_from_frame (r->frame, r->sibling);
  extra = new_user_meta (r->batch, RDS_USER_META, EXTRA_VALUE);
  rds_add_user_meta_to_batch (r->batch, extra);
  r->calls++;
}

static RdsUserMeta *
new_reentrant (RdsBatchMeta * b, Reentry * r)
{
  RdsUserMeta *um = rds_acquire_user_meta_from_pool (b);

  um->base_meta.uContext = r;
  um->base_meta.release_func = release_reentrant;
  return um;
}

static void
test_release_reentrant (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (2);
  RdsFrameMeta *f0 = new_frame (b);
  RdsFrameMeta *f1 = new_frame (b);
  Reentry r0 = { b, f0, NULL, 0 }, r1 = { b, f1, NULL, 0 };
  RdsUserMeta *u0, *u1, *u2, *uy;

  reset_counters ();

  /* Explicit removal: the callback removes a sibling that is still in the list. */
  u0 = new_reentrant (b, &r0);
  u1 = new_user_meta (b, RDS_USER_META, 1);
  u2 = new_user_meta (b, RDS_USER_META, 2);
  r0.sibling = u1;
  rds_add_user_meta_to_frame (f0, u0);
  rds_add_user_meta_to_frame (f0, u1);
  rds_add_user_meta_to_frame (f0, u2);
  rds_remove_user_meta_from_frame (f0, u0);
  g_assert_cmpint (r0.calls, ==, 1);
  g_assert_cmpint (released_by_value[1], ==, 1);
  g_assert_cmpuint (g_list_length (f0->frame_user_meta_list), ==, 1);
  g_assert_true (f0->frame_user_meta_list->data == u2);
  g_assert_cmpuint (g_list_length (b->batch_user_meta_list), ==, 1);

  /* Teardown: the callback runs while f1's list is being released and tries to remove a sibling that was
   * already released earlier in that walk. The list is detached, so this is a no-op, not a double release. */
  uy = new_user_meta (b, RDS_USER_META, 3);
  r1.sibling = uy;
  rds_add_user_meta_to_frame (f1, uy);
  rds_add_user_meta_to_frame (f1, new_reentrant (b, &r1));
  rds_remove_frame_meta_from_batch (b, f1);
  g_assert_cmpint (r1.calls, ==, 1);
  g_assert_cmpint (released_by_value[3], ==, 1);
  g_assert_cmpuint (g_list_length (b->batch_user_meta_list), ==, 2);
  /* No meta went back to the pool twice: fresh acquires alias nothing that is still attached. */
  for (gint i = 0; i < 4; i++) {
    RdsUserMeta *fresh = rds_acquire_user_meta_from_pool (b);

    g_assert_null (g_list_find (b->batch_user_meta_list, fresh));
    g_assert_null (g_list_find (f0->frame_user_meta_list, fresh));
    rds_add_user_meta_to_frame (f0, fresh);
  }
  g_assert_cmpuint (g_list_length (f0->frame_user_meta_list), ==, 5);

  /* Batch teardown, from a batch-level user meta: the meta it adds to the batch is released too. */
  r0.sibling = NULL;
  rds_add_user_meta_to_batch (b, new_reentrant (b, &r0));
  g_assert_cmpint (released, ==, 2);
  rds_destroy_batch_meta (b);
  g_assert_cmpint (r0.calls, ==, 2);
  g_assert_cmpint (released_by_value[2], ==, 1);
  g_assert_cmpint (released_by_value[EXTRA_VALUE], ==, 3);
  g_assert_cmpint (released, ==, 6);
}

#define N_THREADS 8
#define N_DESCRIPTORS 100

static gpointer
register_types (gpointer data)
{
  RdsMetaType *out = data;

  for (gint i = 0; i < N_DESCRIPTORS; i++) {
    gchar *name = g_strdup_printf ("TEST.THREADS.TYPE%d", i);

    out[i] = rds_get_user_meta_type (name);
    g_free (name);
  }
  return NULL;
}

static void
test_user_meta_type (void)
{
  RdsMetaType abc = rds_get_user_meta_type ("A.B.C");
  RdsMetaType results[N_THREADS][N_DESCRIPTORS];
  GThread *threads[N_THREADS];
  gchar *dup = g_strdup ("A.B.C");

  g_assert_cmpint (abc, >=, RDS_START_USER_META);
  g_assert_cmpint (rds_get_user_meta_type (dup), ==, abc);  /* by content, not by pointer */
  g_free (dup);
  g_assert_cmpint (rds_get_user_meta_type ("A.B.D"), !=, abc);
  g_assert_cmpint (rds_get_user_meta_type ("A.B.D"), >=, RDS_START_USER_META);

  for (gint t = 0; t < N_THREADS; t++)
    threads[t] = g_thread_new ("register", register_types, results[t]);
  for (gint t = 0; t < N_THREADS; t++)
    g_thread_join (threads[t]);
  for (gint i = 0; i < N_DESCRIPTORS; i++) {
    g_assert_cmpint (results[0][i], >=, RDS_START_USER_META);
    g_assert_cmpint (results[0][i], !=, abc);
    for (gint t = 1; t < N_THREADS; t++)
      g_assert_cmpint (results[t][i], ==, results[0][i]);
    for (gint j = 0; j < i; j++)
      g_assert_cmpint (results[0][j], !=, results[0][i]);
  }
}

typedef struct
{
  RdsBatchMeta *batch;
  GMutex lock;
  GCond cond;
  gboolean started;
  gint acquired;                /* atomic */
} LockCtx;

static gpointer
lock_thread (gpointer data)
{
  LockCtx *ctx = data;

  g_mutex_lock (&ctx->lock);
  ctx->started = TRUE;
  g_cond_signal (&ctx->cond);
  g_mutex_unlock (&ctx->lock);

  rds_acquire_meta_lock (ctx->batch);
  g_atomic_int_set (&ctx->acquired, 1);
  rds_release_meta_lock (ctx->batch);
  return NULL;
}

static void
test_lock_recursive (void)
{
  LockCtx ctx = { 0 };
  GThread *thread;
  RdsFrameMeta *f;

  ctx.batch = rds_create_batch_meta (1);
  g_mutex_init (&ctx.lock);
  g_cond_init (&ctx.cond);

  rds_acquire_meta_lock (ctx.batch);
  f = rds_acquire_frame_meta_from_pool (ctx.batch);       /* these lock again: must not deadlock */
  rds_add_frame_meta_to_batch (ctx.batch, f);
  new_obj (ctx.batch, f);

  thread = g_thread_new ("locker", lock_thread, &ctx);
  g_mutex_lock (&ctx.lock);
  while (!ctx.started)
    g_cond_wait (&ctx.cond, &ctx.lock);
  g_mutex_unlock (&ctx.lock);
  g_usleep (10 * 1000);
  g_assert_cmpint (g_atomic_int_get (&ctx.acquired), ==, 0);
  rds_release_meta_lock (ctx.batch);
  g_thread_join (thread);
  g_assert_cmpint (g_atomic_int_get (&ctx.acquired), ==, 1);

  g_cond_clear (&ctx.cond);
  g_mutex_clear (&ctx.lock);
  rds_destroy_batch_meta (ctx.batch);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/meta/classifier-labels", test_classifier_labels);
  g_test_add_func ("/meta/display", test_display);
  g_test_add_func ("/meta/user-levels", test_user_levels);
  g_test_add_func ("/meta/children-cross-batch-refused", test_cross_batch_refused);
  g_test_add_func ("/meta/release-reentrant", test_release_reentrant);
  g_test_add_func ("/meta/user-meta-type", test_user_meta_type);
  g_test_add_func ("/meta/lock-recursive", test_lock_recursive);
  return g_test_run ();
}
