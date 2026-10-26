/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include <string.h>
#include <gst/gst.h>
#include <gst/check/gstharness.h>
#include <rds/rds-meta.h>
#include <rds/rds-gst-meta.h>

#define LONG_LABEL_LEN 200
#define MASK_FLOATS 16

static gint created, copied, released;

static void
reset_counters (void)
{
  created = copied = released = 0;
}

static gpointer
copy_user (gpointer data, gpointer user_data)
{
  RdsUserMeta *um = data;

  g_assert_true (user_data == um->base_meta.uContext);
  copied++;
  return g_memdup2 (um->user_meta_data, sizeof (gint));
}

static void
release_user (gpointer data, gpointer user_data)
{
  RdsUserMeta *um = data;

  g_assert_true (user_data == um->base_meta.uContext);
  g_free (um->user_meta_data);
  um->user_meta_data = NULL;
  released++;
}

static RdsUserMeta *
new_user (RdsBatchMeta * b, gint value, gboolean copyable)
{
  RdsUserMeta *um = rds_acquire_user_meta_from_pool (b);

  um->user_meta_data = g_memdup2 (&value, sizeof value);
  um->base_meta.meta_type = rds_get_user_meta_type ("TEST.COPY.USER");
  um->base_meta.uContext = GINT_TO_POINTER (0x5678);
  um->base_meta.release_func = release_user;
  um->base_meta.copy_func = copyable ? copy_user : NULL;
  created++;
  return um;
}

static gint
user_value (RdsMetaList * list)
{
  return *(gint *) ((RdsUserMeta *) list->data)->user_meta_data;
}

static RdsFrameMeta *
frame_at (RdsBatchMeta * b, guint i)
{
  return g_list_nth_data (b->frame_meta_list, i);
}

static RdsObjectMeta *
obj_at (RdsFrameMeta * f, guint i)
{
  return g_list_nth_data (f->obj_meta_list, i);
}

/* frame 0: car (classifier with two labels, obj user meta, mask) and plate (parent car), a display meta and a
 * frame user meta. frame 1: person (parent car: cross-frame) with an uncopyable obj user meta. One batch user
 * meta. Four user metas, three copyable. */
static RdsBatchMeta *
build_batch (void)
{
  RdsBatchMeta *b = rds_create_batch_meta (2);
  RdsFrameMeta *f0 = rds_acquire_frame_meta_from_pool (b);
  RdsFrameMeta *f1 = rds_acquire_frame_meta_from_pool (b);
  RdsObjectMeta *car = rds_acquire_obj_meta_from_pool (b);
  RdsObjectMeta *plate = rds_acquire_obj_meta_from_pool (b);
  RdsObjectMeta *person = rds_acquire_obj_meta_from_pool (b);
  RdsClassifierMeta *c = rds_acquire_classifier_meta_from_pool (b);
  RdsLabelInfo *red = rds_acquire_label_info_meta_from_pool (b);
  RdsLabelInfo *lng = rds_acquire_label_info_meta_from_pool (b);
  RdsDisplayMeta *d = rds_acquire_display_meta_from_pool (b);

  f0->source_id = 0;
  f0->batch_id = 0;
  f0->buf_pts = 1000;
  f0->misc_frame_info[0] = 11;
  rds_add_frame_meta_to_batch (b, f0);
  f1->source_id = 3;
  f1->batch_id = 1;
  rds_add_frame_meta_to_batch (b, f1);

  car->class_id = 2;
  car->rect_params.left = 1;
  car->rect_params.top = 2;
  car->rect_params.width = 30;
  car->rect_params.height = 40;
  car->confidence = 0.75f;
  car->text_params.display_text = g_strdup ("car");
  car->mask_params.data = g_new (gfloat, MASK_FLOATS);
  for (gint i = 0; i < MASK_FLOATS; i++)
    car->mask_params.data[i] = i * 0.5f;
  car->mask_params.size = MASK_FLOATS * sizeof (gfloat);
  car->mask_params.width = car->mask_params.height = 4;
  rds_add_obj_meta_to_frame (f0, car, NULL);

  c->classifier_type = "color";
  c->unique_component_id = 3;
  rds_add_classifier_meta_to_object (car, c);
  g_strlcpy (red->result_label, "red", sizeof red->result_label);
  red->result_prob = 0.8f;
  rds_add_label_info_meta_to_classifier (c, red);
  lng->pResult_label = g_strnfill (LONG_LABEL_LEN, 'z');
  rds_add_label_info_meta_to_classifier (c, lng);
  rds_add_user_meta_to_obj (car, new_user (b, 1, TRUE));

  plate->class_id = 5;
  plate->confidence = 0.5f;
  rds_add_obj_meta_to_frame (f0, plate, car);

  d->num_labels = 1;
  d->text_params[0].display_text = g_strdup ("HUD");
  d->text_params[0].font_params.font_name = (gchar *) "Serif";
  rds_add_display_meta_to_frame (f0, d);
  rds_add_user_meta_to_frame (f0, new_user (b, 2, TRUE));

  person->class_id = 0;
  rds_add_obj_meta_to_frame (f1, person, car);
  rds_add_user_meta_to_obj (person, new_user (b, 4, FALSE));

  rds_add_user_meta_to_batch (b, new_user (b, 3, TRUE));
  return b;
}

static void
assert_frame0_copied (RdsFrameMeta * sf, RdsFrameMeta * cf, RdsBatchMeta * copy)
{
  RdsObjectMeta *scar = obj_at (sf, 0), *ccar = obj_at (cf, 0);
  RdsClassifierMeta *sc, *cc;
  RdsLabelInfo *sl, *cl;
  RdsDisplayMeta *sd, *cd;

  g_assert_true (cf != sf);
  g_assert_true (cf->base_meta.batch_meta == copy);
  g_assert_cmpuint (cf->source_id, ==, sf->source_id);
  g_assert_cmpuint (cf->buf_pts, ==, 1000);
  g_assert_cmpint (cf->misc_frame_info[0], ==, 11);
  g_assert_cmpuint (cf->num_obj_meta, ==, 2);
  g_assert_cmpuint (g_list_length (cf->obj_meta_list), ==, 2);

  for (guint i = 0; i < 2; i++) {
    RdsObjectMeta *so = obj_at (sf, i), *co = obj_at (cf, i);

    g_assert_true (co != so);
    g_assert_true (co->base_meta.batch_meta == copy);
    g_assert_cmpint (co->base_meta.meta_type, ==, RDS_OBJ_META);
    g_assert_cmpint (co->class_id, ==, so->class_id);
    g_assert_cmpfloat (co->rect_params.left, ==, so->rect_params.left);
    g_assert_cmpfloat (co->rect_params.width, ==, so->rect_params.width);
    g_assert_cmpfloat (co->confidence, ==, so->confidence);
    g_assert_cmpuint (co->object_id, ==, so->object_id);
  }

  /* Owned memory is duplicated. */
  g_assert_true (ccar->text_params.display_text != scar->text_params.display_text);
  g_assert_cmpstr (ccar->text_params.display_text, ==, "car");
  g_assert_true (ccar->mask_params.data != scar->mask_params.data);
  g_assert_cmpuint (ccar->mask_params.size, ==, MASK_FLOATS * sizeof (gfloat));
  g_assert_cmpmem (ccar->mask_params.data, ccar->mask_params.size, scar->mask_params.data,
      scar->mask_params.size);

  /* Classifier and labels. */
  g_assert_cmpuint (g_list_length (ccar->classifier_meta_list), ==, 1);
  sc = scar->classifier_meta_list->data;
  cc = ccar->classifier_meta_list->data;
  g_assert_true (cc != sc);
  g_assert_true (cc->base_meta.batch_meta == copy);
  g_assert_cmpstr (cc->classifier_type, ==, "color");
  g_assert_cmpint (cc->unique_component_id, ==, 3);
  g_assert_cmpuint (cc->num_labels, ==, 2);
  g_assert_cmpuint (g_list_length (cc->label_info_list), ==, 2);
  cl = cc->label_info_list->data;
  g_assert_true (cl != sc->label_info_list->data);
  g_assert_true (cl->base_meta.batch_meta == copy);
  g_assert_cmpstr (cl->result_label, ==, "red");
  g_assert_cmpfloat (cl->result_prob, ==, 0.8f);
  sl = sc->label_info_list->next->data;
  cl = cc->label_info_list->next->data;
  g_assert_true (cl->pResult_label != sl->pResult_label);
  g_assert_cmpstr (cl->pResult_label, ==, sl->pResult_label);

  /* Object user meta, copied with copy_func. */
  g_assert_cmpuint (g_list_length (ccar->obj_user_meta_list), ==, 1);
  g_assert_true (ccar->obj_user_meta_list->data != scar->obj_user_meta_list->data);
  g_assert_true (((RdsUserMeta *) ccar->obj_user_meta_list->data)->user_meta_data !=
      ((RdsUserMeta *) scar->obj_user_meta_list->data)->user_meta_data);
  g_assert_cmpint (user_value (ccar->obj_user_meta_list), ==, 1);
  g_assert_true (((RdsUserMeta *) ccar->obj_user_meta_list->data)->base_meta.batch_meta == copy);
  g_assert_cmpint (((RdsUserMeta *) ccar->obj_user_meta_list->data)->base_meta.meta_type, ==,
      rds_get_user_meta_type ("TEST.COPY.USER"));

  /* The parent is remapped to the copy's car. */
  g_assert_true (obj_at (cf, 1)->parent == ccar);

  /* Display meta. */
  g_assert_cmpuint (g_list_length (cf->display_meta_list), ==, 1);
  sd = sf->display_meta_list->data;
  cd = cf->display_meta_list->data;
  g_assert_true (cd != sd);
  g_assert_true (cd->base_meta.batch_meta == copy);
  g_assert_cmpuint (cd->num_labels, ==, 1);
  g_assert_true (cd->text_params[0].display_text != sd->text_params[0].display_text);
  g_assert_cmpstr (cd->text_params[0].display_text, ==, "HUD");
  g_assert_cmpstr (cd->text_params[0].font_params.font_name, ==, "Serif");

  /* Frame user meta. */
  g_assert_cmpuint (g_list_length (cf->frame_user_meta_list), ==, 1);
  g_assert_cmpint (user_value (cf->frame_user_meta_list), ==, 2);
  g_assert_true (((RdsUserMeta *) cf->frame_user_meta_list->data)->base_meta.batch_meta == copy);
}

static void
test_copy_batch (void)
{
  RdsBatchMeta *src, *copy;
  RdsFrameMeta *sf0, *cf0, *cf1;
  RdsObjectMeta *ccar, *cplate;

  reset_counters ();
  src = build_batch ();
  copy = rds_batch_meta_copy (src);

  g_assert_nonnull (copy);
  g_assert_true (copy != src);
  g_assert_true (copy->base_meta.batch_meta == copy);
  g_assert_cmpuint (copy->max_frames_in_batch, ==, 2);
  g_assert_cmpuint (copy->num_frames_in_batch, ==, 2);
  g_assert_cmpuint (g_list_length (copy->frame_meta_list), ==, 2);
  sf0 = frame_at (src, 0);
  cf0 = frame_at (copy, 0);
  cf1 = frame_at (copy, 1);
  assert_frame0_copied (sf0, cf0, copy);

  /* Frame 1: the cross-frame parent is remapped to the copy's car; the uncopyable user meta is skipped. */
  g_assert_cmpuint (cf1->source_id, ==, 3);
  g_assert_cmpuint (cf1->num_obj_meta, ==, 1);
  g_assert_true (obj_at (cf1, 0)->parent == obj_at (cf0, 0));
  g_assert_null (obj_at (cf1, 0)->obj_user_meta_list);
  g_assert_nonnull (obj_at (frame_at (src, 1), 0)->obj_user_meta_list);

  /* Batch user meta. */
  g_assert_cmpuint (g_list_length (copy->batch_user_meta_list), ==, 1);
  g_assert_cmpint (user_value (copy->batch_user_meta_list), ==, 3);
  g_assert_true (((RdsUserMeta *) copy->batch_user_meta_list->data)->base_meta.batch_meta == copy);
  g_assert_cmpint (copied, ==, 3);

  /* Edits to the copy leave the source alone. */
  ccar = obj_at (cf0, 0);
  cplate = obj_at (cf0, 1);
  ccar->rect_params.left = 999;
  g_free (ccar->text_params.display_text);
  ccar->text_params.display_text = g_strdup ("edited");
  /* The remapped parent link is a real one: removing the copy's car clears the copy's plate. */
  rds_remove_obj_meta_from_frame (cf0, ccar);
  g_assert_null (cplate->parent);
  g_assert_null (obj_at (cf1, 0)->parent);
  rds_remove_obj_meta_from_frame (cf0, cplate);
  g_assert_cmpuint (cf0->num_obj_meta, ==, 0);

  g_assert_cmpfloat (obj_at (sf0, 0)->rect_params.left, ==, 1);
  g_assert_cmpstr (obj_at (sf0, 0)->text_params.display_text, ==, "car");
  g_assert_cmpuint (sf0->num_obj_meta, ==, 2);
  g_assert_true (obj_at (sf0, 1)->parent == obj_at (sf0, 0));
  g_assert_true (obj_at (frame_at (src, 1), 0)->parent == obj_at (sf0, 0));

  rds_destroy_batch_meta (copy);
  rds_destroy_batch_meta (src);
  g_assert_cmpint (created, ==, 4);
  g_assert_cmpint (released, ==, created + copied);
}

static void
test_copy_frame_subset (void)
{
  RdsBatchMeta *src, *dst;
  RdsFrameMeta *f;

  reset_counters ();
  src = build_batch ();
  dst = rds_create_batch_meta (1);
  f = rds_copy_frame_meta_to_batch (frame_at (src, 0), dst);

  g_assert_nonnull (f);
  g_assert_cmpuint (dst->num_frames_in_batch, ==, 1);
  g_assert_true (dst->frame_meta_list->data == f);
  assert_frame0_copied (frame_at (src, 0), f, dst);
  g_assert_cmpint (copied, ==, 2);

  /* Frame 1 alone: its parent is in a frame that was not copied, so it becomes NULL. */
  f = rds_copy_frame_meta_to_batch (frame_at (src, 1), dst);
  g_assert_nonnull (f);
  g_assert_cmpuint (dst->num_frames_in_batch, ==, 2);
  g_assert_cmpuint (f->source_id, ==, 3);
  g_assert_cmpuint (f->num_obj_meta, ==, 1);
  g_assert_null (obj_at (f, 0)->parent);

  rds_destroy_batch_meta (dst);
  rds_destroy_batch_meta (src);
  g_assert_cmpint (released, ==, created + copied);
}

static void
test_copy_gst_buffer (void)
{
  GstBuffer *buf, *dup, *extra_ref, *writable, *empty;
  RdsBatchMeta *src, *b;

  reset_counters ();
  buf = gst_buffer_new ();
  src = build_batch ();
  g_assert_nonnull (rds_buffer_add_batch_meta (buf, src));
  g_assert_true (gst_buffer_get_rds_batch_meta (buf) == src);

  dup = gst_buffer_copy (buf);
  b = gst_buffer_get_rds_batch_meta (dup);
  g_assert_nonnull (b);
  g_assert_true (b != src);
  g_assert_cmpuint (b->num_frames_in_batch, ==, 2);
  assert_frame0_copied (frame_at (src, 0), frame_at (b, 0), b);

  extra_ref = gst_buffer_ref (buf);
  writable = gst_buffer_make_writable (buf);
  g_assert_true (writable != extra_ref);
  g_assert_true (gst_buffer_get_rds_batch_meta (extra_ref) == src);
  b = gst_buffer_get_rds_batch_meta (writable);
  g_assert_nonnull (b);
  g_assert_true (b != src);
  g_assert_cmpuint (b->num_frames_in_batch, ==, 2);

  empty = gst_buffer_new ();
  g_assert_null (gst_buffer_get_rds_batch_meta (empty));

  gst_buffer_unref (empty);
  gst_buffer_unref (writable);
  gst_buffer_unref (extra_ref);
  gst_buffer_unref (dup);
  g_assert_cmpint (copied, ==, 6);
  g_assert_cmpint (released, ==, created + copied);
}

static void
check_survives (const gchar * element)
{
  GstHarness *h = gst_harness_new (element);
  GstBuffer *in = gst_buffer_new (), *out;
  RdsBatchMeta *b;

  RdsBatchMeta *orig = build_batch ();
  gint copied_before = copied;

  gst_harness_set_src_caps_str (h, "application/x-rds-test");
  rds_buffer_add_batch_meta (in, orig);
  out = gst_harness_push_and_pull (h, in);
  g_assert_nonnull (out);
  b = gst_buffer_get_rds_batch_meta (out);
  g_assert_true (b == orig);                        /* passed through, not copied */
  g_assert_cmpint (copied, ==, copied_before);
  g_assert_cmpuint (b->num_frames_in_batch, ==, 2);
  gst_buffer_unref (out);
  gst_harness_teardown (h);
}

static void
test_survives_elements (void)
{
  reset_counters ();
  check_survives ("queue");
  check_survives ("identity");
  g_assert_cmpint (copied, ==, 0);
  g_assert_cmpint (released, ==, created + copied);
}

/* A child whose parent is in a later frame, and one whose parent is in no list. */
static void
test_copy_parent_in_later_frame (void)
{
  RdsBatchMeta *src = rds_create_batch_meta (2), *copy;
  RdsFrameMeta *f0 = rds_acquire_frame_meta_from_pool (src);
  RdsFrameMeta *f1 = rds_acquire_frame_meta_from_pool (src);
  RdsObjectMeta *child = rds_acquire_obj_meta_from_pool (src);
  RdsObjectMeta *parent = rds_acquire_obj_meta_from_pool (src);
  RdsObjectMeta *loose = rds_acquire_obj_meta_from_pool (src);          /* never added */
  RdsObjectMeta *orphan = rds_acquire_obj_meta_from_pool (src);
  RdsObjectMeta *cchild, *cparent;

  rds_add_frame_meta_to_batch (src, f0);
  rds_add_frame_meta_to_batch (src, f1);
  rds_add_obj_meta_to_frame (f1, parent, NULL);
  rds_add_obj_meta_to_frame (f0, child, parent);
  rds_add_obj_meta_to_frame (f0, orphan, loose);

  copy = rds_batch_meta_copy (src);
  cchild = obj_at (frame_at (copy, 0), 0);
  cparent = obj_at (frame_at (copy, 1), 0);
  g_assert_true (cchild != child);
  g_assert_true (cchild->parent == cparent);
  g_assert_null (obj_at (frame_at (copy, 0), 1)->parent);

  /* A real link: removing the copied parent clears the copied child. */
  rds_remove_obj_meta_from_frame (frame_at (copy, 1), cparent);
  g_assert_null (cchild->parent);
  g_assert_true (child->parent == parent);

  rds_destroy_batch_meta (copy);
  rds_destroy_batch_meta (src);
}

/* dest already has a batch meta: it wins, nothing is copied. */
static void
test_copy_into_existing (void)
{
  GstBuffer *src_buf = gst_buffer_new (), *dest = gst_buffer_new ();
  RdsBatchMeta *dest_batch;
  gpointer state = NULL;
  guint n = 0;
  gint copied_before;

  reset_counters ();
  rds_buffer_add_batch_meta (src_buf, build_batch ());
  dest_batch = rds_create_batch_meta (1);
  rds_buffer_add_batch_meta (dest, dest_batch);
  copied_before = copied;

  g_assert_true (gst_buffer_copy_into (dest, src_buf, GST_BUFFER_COPY_METADATA, 0, -1));
  while (gst_buffer_iterate_meta_filtered (dest, &state, RDS_META_API_TYPE) != NULL)
    n++;
  g_assert_cmpuint (n, ==, 1);
  g_assert_true (gst_buffer_get_rds_batch_meta (dest) == dest_batch);
  g_assert_cmpint (copied, ==, copied_before);

  gst_buffer_unref (dest);
  gst_buffer_unref (src_buf);
  g_assert_cmpint (released, ==, created + copied);
}

/* A buffer carries at most one batch meta: a second add is refused and ownership stays with the caller. */
static void
test_add_twice_refused (void)
{
  GstBuffer *buf = gst_buffer_new ();
  RdsBatchMeta *first = rds_create_batch_meta (1), *second = rds_create_batch_meta (1);

  g_assert_nonnull (rds_buffer_add_batch_meta (buf, first));
  g_test_expect_message ("rds", G_LOG_LEVEL_CRITICAL, "*gst_buffer_get_rds_batch_meta (buffer) == NULL*");
  g_assert_null (rds_buffer_add_batch_meta (buf, second));
  g_test_assert_expected_messages ();
  g_assert_true (gst_buffer_get_rds_batch_meta (buf) == first);

  rds_destroy_batch_meta (second);
  gst_buffer_unref (buf);
}

/* Object and display lists copied into a frame of another batch: appended after what the frame has, parents
 * remapped within the list (a parent outside it becomes NULL), owned memory duplicated. */
static void
test_copy_lists (void)
{
  RdsBatchMeta *src, *dst;
  RdsFrameMeta *f, *sf0;
  RdsObjectMeta *existing, *ccar, *cplate, *cperson;
  RdsDisplayMeta *cd;

  reset_counters ();
  src = build_batch ();
  sf0 = frame_at (src, 0);
  dst = rds_create_batch_meta (1);
  f = rds_acquire_frame_meta_from_pool (dst);
  rds_add_frame_meta_to_batch (dst, f);
  existing = rds_acquire_obj_meta_from_pool (dst);
  rds_add_obj_meta_to_frame (f, existing, NULL);

  rds_copy_obj_meta_list (sf0->obj_meta_list, f);
  rds_copy_obj_meta_list (frame_at (src, 1)->obj_meta_list, f);
  rds_copy_display_meta_list (sf0->display_meta_list, f);
  rds_copy_display_meta_list (sf0->display_meta_list, f);
  /* empty lists are fine */
  rds_copy_obj_meta_list (NULL, f);
  rds_copy_display_meta_list (NULL, f);

  g_assert_cmpuint (f->num_obj_meta, ==, 4);
  g_assert_cmpuint (g_list_length (f->obj_meta_list), ==, 4);
  g_assert_true (obj_at (f, 0) == existing);
  ccar = obj_at (f, 1);
  cplate = obj_at (f, 2);
  cperson = obj_at (f, 3);
  g_assert_true (ccar != obj_at (sf0, 0));
  g_assert_true (ccar->base_meta.batch_meta == dst);
  g_assert_cmpint (ccar->class_id, ==, 2);
  g_assert_true (ccar->text_params.display_text != obj_at (sf0, 0)->text_params.display_text);
  g_assert_cmpstr (ccar->text_params.display_text, ==, "car");
  g_assert_true (ccar->mask_params.data != obj_at (sf0, 0)->mask_params.data);
  g_assert_cmpuint (g_list_length (ccar->classifier_meta_list), ==, 1);
  g_assert_cmpuint (((RdsClassifierMeta *) ccar->classifier_meta_list->data)->num_labels, ==, 2);
  g_assert_cmpint (user_value (ccar->obj_user_meta_list), ==, 1);
  g_assert_true (cplate->parent == ccar);
  /* person's parent (car) was in another list: not remapped */
  g_assert_cmpint (cperson->class_id, ==, 0);
  g_assert_null (cperson->parent);

  g_assert_cmpuint (g_list_length (f->display_meta_list), ==, 2);
  cd = f->display_meta_list->data;
  g_assert_true (cd != sf0->display_meta_list->data);
  g_assert_true (cd->base_meta.batch_meta == dst);
  g_assert_cmpstr (cd->text_params[0].display_text, ==, "HUD");
  g_assert_true (cd->text_params[0].display_text !=
      ((RdsDisplayMeta *) sf0->display_meta_list->data)->text_params[0].display_text);

  /* a real parent link: removing the copied car clears the copied plate */
  rds_remove_obj_meta_from_frame (f, ccar);
  g_assert_null (cplate->parent);
  g_assert_true (obj_at (sf0, 1)->parent == obj_at (sf0, 0));

  rds_destroy_batch_meta (dst);
  rds_destroy_batch_meta (src);
  g_assert_cmpint (released, ==, created + copied);
}

/* Copying a list into a frame of its own batch. */
static void
test_copy_lists_same_batch (void)
{
  RdsBatchMeta *b;
  RdsFrameMeta *f;

  reset_counters ();
  b = build_batch ();
  f = rds_acquire_frame_meta_from_pool (b);
  rds_add_frame_meta_to_batch (b, f);
  rds_copy_obj_meta_list (frame_at (b, 0)->obj_meta_list, f);
  rds_copy_display_meta_list (frame_at (b, 0)->display_meta_list, f);
  g_assert_cmpuint (f->num_obj_meta, ==, 2);
  g_assert_true (obj_at (f, 1)->parent == obj_at (f, 0));
  g_assert_cmpuint (g_list_length (f->display_meta_list), ==, 1);

  /* a frame's lists into themselves: each entry once more, the copies' parents among the copies */
  rds_copy_obj_meta_list (f->obj_meta_list, f);
  rds_copy_display_meta_list (f->display_meta_list, f);
  g_assert_cmpuint (f->num_obj_meta, ==, 4);
  g_assert_cmpuint (g_list_length (f->obj_meta_list), ==, 4);
  g_assert_true (obj_at (f, 1)->parent == obj_at (f, 0));
  g_assert_true (obj_at (f, 3)->parent == obj_at (f, 2));
  g_assert_true (obj_at (f, 2) != obj_at (f, 0));
  g_assert_cmpstr (obj_at (f, 2)->text_params.display_text, ==, "car");
  g_assert_cmpuint (g_list_length (f->display_meta_list), ==, 2);
  rds_destroy_batch_meta (b);
  g_assert_cmpint (released, ==, created + copied);
}

int
main (int argc, char **argv)
{
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/meta-copy/batch", test_copy_batch);
  g_test_add_func ("/meta-copy/frame-subset", test_copy_frame_subset);
  g_test_add_func ("/meta-copy/gst-buffer", test_copy_gst_buffer);
  g_test_add_func ("/meta-copy/survives-elements", test_survives_elements);
  g_test_add_func ("/meta-copy/parent-in-later-frame", test_copy_parent_in_later_frame);
  g_test_add_func ("/meta-copy/add-twice-refused", test_add_twice_refused);
  g_test_add_func ("/meta-copy/copy-into-existing", test_copy_into_existing);
  g_test_add_func ("/meta-copy/lists", test_copy_lists);
  g_test_add_func ("/meta-copy/lists-same-batch", test_copy_lists_same_batch);
  {
    /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at
     * process exit otherwise (seen under parallel ASan runs) */
    int ret = g_test_run ();

    gst_deinit ();
    return ret;
  }
}
