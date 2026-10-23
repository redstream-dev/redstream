/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <gst/gst.h>
#include <rds/rds-meta.h>

static RdsObjectMeta *
add_obj (RdsBatchMeta * b, RdsFrameMeta * f, gint class_id, RdsObjectMeta * parent)
{
  RdsObjectMeta *o = rds_acquire_obj_meta_from_pool (b);

  o->class_id = class_id;
  o->rect_params.left = 10.0f * class_id;
  o->text_params.display_text = g_strdup_printf ("class %d", class_id);
  rds_add_obj_meta_to_frame (f, o, parent);
  return o;
}

static void
test_create_destroy (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (4);

  g_assert_nonnull (b);
  g_assert_cmpint (b->base_meta.meta_type, ==, RDS_BATCH_META);
  g_assert_true (b->base_meta.batch_meta == b);
  g_assert_cmpuint (b->max_frames_in_batch, ==, 4);
  g_assert_cmpuint (b->num_frames_in_batch, ==, 0);
  g_assert_null (b->frame_meta_list);
  rds_destroy_batch_meta (b);
}

static void
test_frames_and_objects (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (2);
  RdsFrameMeta *f0 = rds_acquire_frame_meta_from_pool (b);
  RdsFrameMeta *f1 = rds_acquire_frame_meta_from_pool (b);
  RdsObjectMeta *car, *plate;
  guint n = 0;

  g_assert_cmpint (f0->base_meta.meta_type, ==, RDS_FRAME_META);
  g_assert_true (f0->base_meta.batch_meta == b);
  f0->source_id = 0;
  f0->batch_id = 0;
  f1->source_id = 7;
  f1->batch_id = 1;
  rds_add_frame_meta_to_batch (b, f0);
  rds_add_frame_meta_to_batch (b, f1);
  g_assert_cmpuint (b->num_frames_in_batch, ==, 2);

  car = add_obj (b, f1, 2, NULL);
  plate = add_obj (b, f1, 5, car);
  g_assert_cmpint (car->base_meta.meta_type, ==, RDS_OBJ_META);
  g_assert_cmpuint (car->object_id, ==, RDS_UNTRACKED_OBJECT_ID);
  g_assert_true (plate->parent == car);
  g_assert_cmpuint (f1->num_obj_meta, ==, 2);

  /* Traversal in insertion order, DeepStream style. */
  for (RdsMetaList * l = b->frame_meta_list; l != NULL; l = l->next) {
    RdsFrameMeta *f = (RdsFrameMeta *) l->data;
    g_assert_cmpuint (f->batch_id, ==, n++);
  }
  g_assert_true (((RdsObjectMeta *) f1->obj_meta_list->data) == car);
  g_assert_true (((RdsObjectMeta *) f1->obj_meta_list->next->data) == plate);

  /* Removing the parent clears the child's parent pointer. */
  rds_remove_obj_meta_from_frame (f1, car);
  g_assert_cmpuint (f1->num_obj_meta, ==, 1);
  g_assert_null (plate->parent);

  rds_remove_frame_meta_from_batch (b, f0);
  g_assert_cmpuint (b->num_frames_in_batch, ==, 1);
  g_assert_true (b->frame_meta_list->data == f1);
  rds_destroy_batch_meta (b);                       /* releases f1 and plate; ASan checks the strings */
}

static void
test_pool_reuse (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *f = rds_acquire_frame_meta_from_pool (b);
  RdsObjectMeta *o, *again;

  rds_add_frame_meta_to_batch (b, f);
  o = add_obj (b, f, 1, NULL);
  o->confidence = 0.9f;
  rds_remove_obj_meta_from_frame (f, o);
  again = rds_acquire_obj_meta_from_pool (b);
  g_assert_true (again == o);                       /* recycled ... */
  g_assert_cmpfloat (again->confidence, ==, 0.0f);  /* ... and zeroed */
  g_assert_null (again->text_params.display_text);
  g_assert_cmpuint (again->object_id, ==, RDS_UNTRACKED_OBJECT_ID);
  g_assert_true (again->base_meta.batch_meta == b);
  rds_add_obj_meta_to_frame (f, again, NULL);
  rds_destroy_batch_meta (b);
}

static void
test_pool_grows (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);
  RdsFrameMeta *f = rds_acquire_frame_meta_from_pool (b);

  rds_add_frame_meta_to_batch (b, f);
  for (gint i = 0; i < 5000; i++)
    add_obj (b, f, i % 80, NULL);
  g_assert_cmpuint (f->num_obj_meta, ==, 5000);
  g_assert_cmpuint (g_list_length (f->obj_meta_list), ==, 5000);
  rds_destroy_batch_meta (b);
}

static void
test_cross_frame_parent (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (2);
  RdsFrameMeta *f0 = rds_acquire_frame_meta_from_pool (b);
  RdsFrameMeta *f1 = rds_acquire_frame_meta_from_pool (b);
  RdsObjectMeta *car, *person, *bike;

  rds_add_frame_meta_to_batch (b, f0);
  rds_add_frame_meta_to_batch (b, f1);
  car = add_obj (b, f0, 2, NULL);
  bike = add_obj (b, f0, 3, NULL);
  person = add_obj (b, f1, 0, car);
  add_obj (b, f1, 1, bike);

  /* Removing an object clears parent pointers in other frames too. */
  rds_remove_obj_meta_from_frame (f0, car);
  g_assert_null (person->parent);

  /* Removing a frame clears parents that pointed at its objects. */
  rds_remove_frame_meta_from_batch (b, f0);
  g_assert_null (((RdsObjectMeta *) f1->obj_meta_list->next->data)->parent);
  rds_destroy_batch_meta (b);
}

static void
test_remove_not_in_list (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (2);
  RdsFrameMeta *f0 = rds_acquire_frame_meta_from_pool (b);
  RdsFrameMeta *f1 = rds_acquire_frame_meta_from_pool (b);
  RdsObjectMeta *o;

  rds_add_frame_meta_to_batch (b, f0);
  o = add_obj (b, f0, 1, NULL);

  /* Misuse is warned about and ignored: nothing is released or unlinked. */
  rds_remove_obj_meta_from_frame (f1, o);
  rds_remove_frame_meta_from_batch (b, f1);
  g_assert_cmpuint (f0->num_obj_meta, ==, 1);
  g_assert_cmpuint (b->num_frames_in_batch, ==, 1);
  g_assert_cmpstr (o->text_params.display_text, ==, "class 1");

  rds_add_frame_meta_to_batch (b, f1);
  rds_destroy_batch_meta (b);
}

static void
test_destroy_frees_unadded (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (1);

  /* Acquired but never added: the pools still own them, so ASan sees no leak. */
  for (gint i = 0; i < 100; i++) {
    rds_acquire_frame_meta_from_pool (b);
    rds_acquire_obj_meta_from_pool (b);
  }
  rds_destroy_batch_meta (b);
  rds_destroy_batch_meta (NULL);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/meta/create-destroy", test_create_destroy);
  g_test_add_func ("/meta/frames-and-objects", test_frames_and_objects);
  g_test_add_func ("/meta/pool-reuse", test_pool_reuse);
  g_test_add_func ("/meta/pool-grows", test_pool_grows);
  g_test_add_func ("/meta/cross-frame-parent", test_cross_frame_parent);
  g_test_add_func ("/meta/remove-not-in-list", test_remove_not_in_list);
  g_test_add_func ("/meta/destroy-frees-unadded", test_destroy_frees_unadded);
  return g_test_run ();
}
