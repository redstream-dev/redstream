/* rdsinfer's built-in YOLO parser (through the parser handle the element uses) and the detection post-processing:
 * each step on its own, the letterbox geometry against a hand-computed table, a combined case, random sets against
 * the naive reference in infer-post-ref.h, and timings on a YOLO11n-sized output.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-parse.h"
#include "rds-infer-post.h"
#include "infer-post-ref.h"

#include <math.h>
#include <string.h>

/* ---- Helpers ---- */

/* A detector config with @n_classes classes and the [class-attrs-all] defaults. */
static RdsInferConfig *
make_cfg (guint n_classes)
{
  RdsInferConfig *cfg = g_new0 (RdsInferConfig, 1);

  cfg->num_detected_classes = n_classes;
  cfg->cluster_mode = 2;
  cfg->filter_out_class_ids = g_array_new (FALSE, FALSE, sizeof (guint));
  cfg->warnings = g_ptr_array_new_with_free_func (g_free);
  cfg->class_attrs = g_new0 (RdsInferClassAttrs, n_classes);
  for (guint i = 0; i < n_classes; i++) {
    cfg->class_attrs[i].pre_cluster_threshold = 0.25f;
    cfg->class_attrs[i].post_cluster_threshold = 0.0f;
    cfg->class_attrs[i].nms_iou_threshold = 0.45f;
    cfg->class_attrs[i].topk = -1;
  }
  return cfg;
}

static RdsInferObjectDetectionInfo
obj (guint class_id, gfloat left, gfloat top, gfloat width, gfloat height, gfloat confidence)
{
  RdsInferObjectDetectionInfo o = { class_id, left, top, width, height, confidence, 0.0f };

  return o;
}

/* Network 640x640, frame 640x640: frame pixels are network pixels. */
static void
identity_lb (RdsInferLetterbox * lb)
{
  rds_infer_letterbox (640, 640, 640, 640, TRUE, FALSE, lb);
}

static GArray *
detections_new (void)
{
  return g_array_new (FALSE, FALSE, sizeof (RdsInferDetection));
}

static GArray *
post (const RdsInferConfig * cfg, const RdsInferLetterbox * lb, const RdsInferObjectDetectionInfo * in, guint n,
    RdsInferPostFlags * flags)
{
  GArray *out = detections_new ();
  RdsInferPostFlags f = rds_infer_postprocess (cfg, lb, in, n, out);

  if (flags)
    *flags = f;
  return out;
}

static GArray *
post_identity (const RdsInferConfig * cfg, const RdsInferObjectDetectionInfo * in, guint n)
{
  RdsInferLetterbox lb;

  identity_lb (&lb);
  return post (cfg, &lb, in, n, NULL);
}

static const RdsInferDetection *
det (GArray * out, guint i)
{
  g_assert_cmpuint (i, <, out->len);
  return &g_array_index (out, RdsInferDetection, i);
}

/* The detections are the input objects @idx (identity letterbox, so the boxes are unchanged), in that order. */
static void
assert_kept (GArray * out, const RdsInferObjectDetectionInfo * in, const guint * idx, guint n_idx)
{
  g_assert_cmpuint (out->len, ==, n_idx);
  for (guint i = 0; i < n_idx; i++) {
    const RdsInferDetection *d = det (out, i);
    const RdsInferObjectDetectionInfo *o = &in[idx[i]];

    g_assert_cmpuint (d->class_id, ==, o->classId);
    g_assert_cmpfloat (d->confidence, ==, o->detectionConfidence);
    g_assert_cmpfloat (d->box.left, ==, o->left);
    g_assert_cmpfloat (d->box.top, ==, o->top);
    g_assert_cmpfloat (d->box.width, ==, o->width);
    g_assert_cmpfloat (d->box.height, ==, o->height);
  }
}

static void
assert_box (const RdsInferBox * b, gfloat left, gfloat top, gfloat width, gfloat height)
{
  g_assert_cmpfloat_with_epsilon (b->left, left, 1e-3);
  g_assert_cmpfloat_with_epsilon (b->top, top, 1e-3);
  g_assert_cmpfloat_with_epsilon (b->width, width, 1e-3);
  g_assert_cmpfloat_with_epsilon (b->height, height, 1e-3);
}

/* ---- YOLO parser ---- */

#define YOLO_CLASSES 80
#define YOLO_ROWS (4 + YOLO_CLASSES)
#define YOLO_ANCHORS 8400

typedef struct
{
  gfloat *data;
  guint n_anchors, n_classes;
  gboolean transposed;          /* [A, 4 + C] instead of [4 + C, A] */
  RdsInferLayerInfo layer;
} YoloTensor;

static void
yolo_init (YoloTensor * t, guint n_anchors, guint n_classes, gboolean transposed)
{
  guint rows = 4 + n_classes;

  t->data = g_new0 (gfloat, (gsize) rows * n_anchors);
  t->n_anchors = n_anchors;
  t->n_classes = n_classes;
  t->transposed = transposed;
  memset (&t->layer, 0, sizeof (t->layer));
  t->layer.dataType = RDS_INFER_DATA_FLOAT;
  t->layer.inferDims.numDims = 2;
  t->layer.inferDims.d[0] = transposed ? n_anchors : rows;
  t->layer.inferDims.d[1] = transposed ? rows : n_anchors;
  t->layer.inferDims.numElements = rows * n_anchors;
  t->layer.layerName = "output0";
  t->layer.buffer = t->data;
}

static gfloat *
yolo_at (YoloTensor * t, guint row, guint anchor)
{
  if (t->transposed)
    return &t->data[(gsize) anchor * (4 + t->n_classes) + row];
  return &t->data[(gsize) row * t->n_anchors + anchor];
}

static void
yolo_set_box (YoloTensor * t, guint anchor, gfloat cx, gfloat cy, gfloat w, gfloat h)
{
  *yolo_at (t, 0, anchor) = cx;
  *yolo_at (t, 1, anchor) = cy;
  *yolo_at (t, 2, anchor) = w;
  *yolo_at (t, 3, anchor) = h;
}

static void
yolo_set_score (YoloTensor * t, guint anchor, guint class_id, gfloat score)
{
  *yolo_at (t, 4 + class_id, anchor) = score;
}

static void
yolo_clear (YoloTensor * t)
{
  g_free (t->data);
}

static RdsInferParser *
yolo_parser (guint n_classes, const gfloat * pre)
{
  GError *err = NULL;
  RdsInferParser *parser = rds_infer_parser_open_builtin (RDS_INFER_PARSER_YOLO, &err);
  gfloat *post_thr = g_new0 (gfloat, n_classes);

  g_assert_no_error (err);
  g_assert_nonnull (parser);
  g_assert_cmpstr (rds_infer_parser_get_func_name (parser), ==, "RdsInferParseYolo");
  rds_infer_parser_set_detection_params (parser, n_classes, pre, post_thr);
  g_free (post_thr);
  return parser;
}

static const RdsInferNetworkInfo yolo_net = { 640, 640, 3 };

static void
yolo_parse_ok (RdsInferParser * parser, YoloTensor * t, GArray * objects)
{
  GError *err = NULL;

  g_assert_true (rds_infer_parser_check_layers (parser, &t->layer, 1, &err));
  g_assert_no_error (err);
  g_assert_true (rds_infer_parser_parse (parser, &t->layer, 1, &yolo_net, objects, &err));
  g_assert_no_error (err);
}

static void
assert_object (const RdsInferObjectDetectionInfo * o, guint class_id, gfloat cx, gfloat cy, gfloat w, gfloat h,
    gfloat conf)
{
  g_assert_cmpuint (o->classId, ==, class_id);
  g_assert_cmpfloat (o->left, ==, cx - w / 2);
  g_assert_cmpfloat (o->top, ==, cy - h / 2);
  g_assert_cmpfloat (o->width, ==, w);
  g_assert_cmpfloat (o->height, ==, h);
  g_assert_cmpfloat (o->detectionConfidence, ==, conf);
  g_assert_cmpfloat (o->rotation_angle, ==, 0.0f);
}

static void
yolo_parse_layout (gboolean transposed)
{
  gfloat pre[YOLO_CLASSES];
  RdsInferParser *parser;
  GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  YoloTensor t;

  for (guint i = 0; i < YOLO_CLASSES; i++)
    pre[i] = 0.25f;
  pre[7] = 0.6f;
  parser = yolo_parser (YOLO_CLASSES, pre);
  yolo_init (&t, YOLO_ANCHORS, YOLO_CLASSES, transposed);

  /* The best class wins; one object per anchor. */
  yolo_set_box (&t, 0, 100, 200, 40, 60);
  yolo_set_score (&t, 0, 3, 0.9f);
  yolo_set_score (&t, 0, 5, 0.5f);
  /* The last class. */
  yolo_set_box (&t, 17, 320.5f, 10, 8, 12);
  yolo_set_score (&t, 17, 79, 0.3f);
  /* Below the threshold: dropped. */
  yolo_set_box (&t, 100, 50, 50, 10, 10);
  yolo_set_score (&t, 100, 2, 0.2f);
  /* Below its own class's threshold (0.6), above the others': dropped. */
  yolo_set_box (&t, 200, 60, 60, 10, 10);
  yolo_set_score (&t, 200, 7, 0.5f);
  /* At the threshold: kept. */
  yolo_set_box (&t, 300, 600, 620, 30, 40);
  yolo_set_score (&t, 300, 7, 0.6f);
  /* Equal scores: the lower class id. The last anchor. */
  yolo_set_box (&t, YOLO_ANCHORS - 1, 630, 630, 20, 20);
  yolo_set_score (&t, YOLO_ANCHORS - 1, 1, 0.6f);
  yolo_set_score (&t, YOLO_ANCHORS - 1, 0, 0.6f);

  yolo_parse_ok (parser, &t, objects);
  g_assert_cmpuint (objects->len, ==, 4);
  assert_object (&g_array_index (objects, RdsInferObjectDetectionInfo, 0), 3, 100, 200, 40, 60, 0.9f);
  assert_object (&g_array_index (objects, RdsInferObjectDetectionInfo, 1), 79, 320.5f, 10, 8, 12, 0.3f);
  assert_object (&g_array_index (objects, RdsInferObjectDetectionInfo, 2), 7, 600, 620, 30, 40, 0.6f);
  assert_object (&g_array_index (objects, RdsInferObjectDetectionInfo, 3), 0, 630, 630, 20, 20, 0.6f);

  yolo_clear (&t);
  g_array_unref (objects);
  rds_infer_parser_free (parser);
}

static void
test_yolo_parse (void)
{
  yolo_parse_layout (FALSE);
}

static void
test_yolo_parse_transposed (void)
{
  yolo_parse_layout (TRUE);
}

/* [84, 84] matches both layouts: it is read as [4 + C, A]. */
static void
test_yolo_square (void)
{
  gfloat pre[YOLO_CLASSES] = { 0 };
  RdsInferParser *parser;
  GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  YoloTensor t;

  for (guint i = 0; i < YOLO_CLASSES; i++)
    pre[i] = 0.25f;
  parser = yolo_parser (YOLO_CLASSES, pre);
  yolo_init (&t, YOLO_ROWS, YOLO_CLASSES, FALSE);
  yolo_set_box (&t, 5, 10, 20, 4, 6);
  yolo_set_score (&t, 5, 9, 0.75f);
  yolo_parse_ok (parser, &t, objects);
  g_assert_cmpuint (objects->len, ==, 1);
  assert_object (&g_array_index (objects, RdsInferObjectDetectionInfo, 0), 9, 10, 20, 4, 6, 0.75f);
  yolo_clear (&t);
  g_array_unref (objects);
  rds_infer_parser_free (parser);
}

/* Fails the start-up check and the per-call one with RDS_INFER_PARSER_ERROR_LAYERS; the message contains each of
 * the NULL-terminated @want. */
static void
assert_layers_fail (RdsInferParser * parser, const RdsInferLayerInfo * layers, guint n_layers, ...)
{
  GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  GError *err = NULL;
  const gchar *want;
  va_list ap;

  g_assert_false (rds_infer_parser_check_layers (parser, layers, n_layers, &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS);
  g_test_message ("%s", err->message);
  va_start (ap, n_layers);
  while ((want = va_arg (ap, const gchar *)))
    if (!strstr (err->message, want))
      g_error ("\"%s\" not in \"%s\"", want, err->message);
  va_end (ap);
  g_clear_error (&err);

  g_assert_false (rds_infer_parser_parse (parser, layers, n_layers, &yolo_net, objects, &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS);
  g_assert_cmpuint (objects->len, ==, 0);
  g_clear_error (&err);
  g_array_unref (objects);
}

static void
test_yolo_bad_layers (void)
{
  gfloat pre[YOLO_CLASSES] = { 0 };
  RdsInferParser *parser = yolo_parser (YOLO_CLASSES, pre);
  RdsInferLayerInfo two[2];
  YoloTensor t, wrong;

  yolo_init (&t, 64, YOLO_CLASSES, FALSE);
  /* 81 classes' worth of rows. */
  yolo_init (&wrong, 64, YOLO_CLASSES + 1, FALSE);
  assert_layers_fail (parser, &wrong.layer, 1, "built-in parser", "RdsInferParseYolo", "\"output0\"",
      "[85, 64]", "[84, anchors]", "[anchors, 84]", "num-detected-classes 80", NULL);

  /* A batch dimension. */
  t.layer.inferDims.numDims = 3;
  t.layer.inferDims.d[0] = 1;
  t.layer.inferDims.d[1] = YOLO_ROWS;
  t.layer.inferDims.d[2] = 64;
  assert_layers_fail (parser, &t.layer, 1, "[1, 84, 64]", NULL);
  t.layer.inferDims.numDims = 2;
  t.layer.inferDims.d[0] = YOLO_ROWS;
  t.layer.inferDims.d[1] = 64;
  t.layer.inferDims.d[2] = 0;

  /* No anchors. */
  t.layer.inferDims.d[1] = 0;
  t.layer.inferDims.numElements = 0;
  assert_layers_fail (parser, &t.layer, 1, "[84, 0]", NULL);
  t.layer.inferDims.d[1] = 64;
  t.layer.inferDims.numElements = YOLO_ROWS * 64;

  /* numElements disagrees with the dimensions. */
  t.layer.inferDims.numElements = YOLO_ROWS * 64 - 1;
  assert_layers_fail (parser, &t.layer, 1, "[84, 64]", "5375 elements", NULL);
  t.layer.inferDims.numElements = YOLO_ROWS * 64;

  t.layer.dataType = RDS_INFER_DATA_HALF;
  assert_layers_fail (parser, &t.layer, 1, "FP16", "FP32", NULL);
  t.layer.dataType = RDS_INFER_DATA_FLOAT;

  assert_layers_fail (parser, NULL, 0, "0 output layers", "one", NULL);
  two[0] = t.layer;
  two[1] = t.layer;
  two[1].layerName = "output1";
  assert_layers_fail (parser, two, 2, "2 output layers", "one", NULL);

  /* An input layer is not an output. */
  t.layer.isInput = 1;
  assert_layers_fail (parser, &t.layer, 1, "input layer", NULL);
  t.layer.isInput = 0;

  /* The per-call check also catches a missing buffer. */
  {
    GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
    GError *err = NULL;

    t.layer.buffer = NULL;
    g_assert_false (rds_infer_parser_parse (parser, &t.layer, 1, &yolo_net, objects, &err));
    g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_LAYERS);
    g_assert_nonnull (strstr (err->message, "no host buffer"));
    g_clear_error (&err);
    t.layer.buffer = t.data;
    g_assert_true (rds_infer_parser_parse (parser, &t.layer, 1, &yolo_net, objects, &err));
    g_assert_no_error (err);
    g_array_unref (objects);
  }

  /* The class count comes from the detection params. */
  {
    gfloat pre81[YOLO_CLASSES + 1] = { 0 };
    GError *err = NULL;

    rds_infer_parser_set_detection_params (parser, YOLO_CLASSES + 1, pre81, pre81);
    assert_layers_fail (parser, &t.layer, 1, "[84, 64]", "[85, anchors]", NULL);
    g_assert_true (rds_infer_parser_check_layers (parser, &wrong.layer, 1, &err));
    g_assert_no_error (err);
  }

  yolo_clear (&t);
  yolo_clear (&wrong);
  rds_infer_parser_free (parser);
}

static void
test_builtin_unknown (void)
{
  GError *err = NULL;

  g_assert_null (rds_infer_parser_open_builtin ("NvDsInferParseYolo", &err));
  g_assert_error (err, RDS_INFER_PARSER_ERROR, RDS_INFER_PARSER_ERROR_SYMBOL);
  g_test_message ("%s", err->message);
  g_assert_nonnull (strstr (err->message, "NvDsInferParseYolo"));
  g_assert_nonnull (strstr (err->message, "RdsInferParseYolo"));
  g_assert_nonnull (strstr (err->message, "custom-lib-path"));
  g_error_free (err);
}

/* ---- Post-processing, step by step ---- */

static void
test_validate (void)
{
  RdsInferConfig *cfg = make_cfg (2);
  RdsInferObjectDetectionInfo in[] = {
    obj (2, 10, 10, 10, 10, 0.9f),      /* class id out of range */
    obj (0, NAN, 10, 10, 10, 0.9f),
    obj (0, 10, INFINITY, 10, 10, 0.9f),
    obj (0, 10, 10, -INFINITY, 10, 0.9f),
    obj (0, 10, 10, 10, NAN, 0.9f),
    obj (0, 10, 10, 10, 10, NAN),
    obj (0, 10, 10, 10, 10, INFINITY),
    obj (0, 10, 10, 0, 10, 0.9f),
    obj (0, 10, 10, 10, -1, 0.9f),
    obj (1, 100, 100, 10, 10, 0.9f),    /* 9: valid */
    obj (0, 200, 200, 10, 10, 0.2f),    /* below the pre-cluster threshold: dropped, but not invalid */
    obj (0, 300, 300, 10, 10, 0.25f),   /* 11: at the threshold */
  };
  const guint keep[] = { 9, 11 };
  RdsInferLetterbox lb;
  RdsInferPostFlags flags;
  GArray *out;

  identity_lb (&lb);
  out = post (cfg, &lb, in, G_N_ELEMENTS (in), &flags);
  g_assert_cmpint (flags, ==, RDS_INFER_POST_INVALID);
  assert_kept (out, in, keep, G_N_ELEMENTS (keep));
  g_array_unref (out);

  /* Only valid objects: no flags. */
  out = post (cfg, &lb, in + 9, 3, &flags);
  g_assert_cmpint (flags, ==, 0);
  g_assert_cmpuint (out->len, ==, 2);
  g_array_unref (out);

  /* A rotated object is kept, its angle ignored. */
  in[9].rotation_angle = 0.5f;
  out = post (cfg, &lb, in + 9, 1, &flags);
  g_assert_cmpint (flags, ==, RDS_INFER_POST_ROTATED);
  g_assert_cmpuint (out->len, ==, 1);
  assert_box (&det (out, 0)->box, 100, 100, 10, 10);
  g_array_unref (out);

  out = post (cfg, &lb, in, G_N_ELEMENTS (in), &flags);
  g_assert_cmpint (flags, ==, RDS_INFER_POST_INVALID | RDS_INFER_POST_ROTATED);
  g_array_unref (out);

  /* Nothing in, nothing out. */
  out = post (cfg, &lb, NULL, 0, &flags);
  g_assert_cmpint (flags, ==, 0);
  g_assert_cmpuint (out->len, ==, 0);
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

static void
test_nms (void)
{
  RdsInferConfig *cfg = make_cfg (2);
  GArray *out;

  cfg->class_attrs[0].nms_iou_threshold = 0.5f;
  cfg->class_attrs[1].nms_iou_threshold = 0.5f;
  {
    /* Different classes: both survive. The same class: the lower confidence goes, wherever it is in the input. */
    RdsInferObjectDetectionInfo in[] = {
      obj (0, 0, 0, 10, 10, 0.9f),
      obj (1, 1, 1, 10, 10, 0.8f),
      obj (0, 101, 0, 10, 10, 0.8f),    /* IoU with 3: 90 / 110 */
      obj (0, 100, 0, 10, 10, 0.9f),
    };
    const guint keep[] = { 0, 1, 3 };

    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, keep, G_N_ELEMENTS (keep));
    g_array_unref (out);
  }
  {
    /* IoU exactly 0.5 (50 / 100): kept at a 0.5 threshold, suppressed below it. */
    RdsInferObjectDetectionInfo in[] = {
      obj (0, 0, 0, 10, 10, 0.9f),
      obj (0, 0, 0, 10, 5, 0.8f),
    };
    const guint both[] = { 0, 1 }, first[] = { 0 };

    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, both, G_N_ELEMENTS (both));
    g_array_unref (out);
    cfg->class_attrs[0].nms_iou_threshold = 0.49f;
    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, first, G_N_ELEMENTS (first));
    g_array_unref (out);
    cfg->class_attrs[0].nms_iou_threshold = 0.5f;
  }
  {
    /* Equal confidence: the lower index wins, whichever box it is. */
    RdsInferObjectDetectionInfo in[] = {
      obj (0, 50, 50, 10, 10, 0.5f),
      obj (0, 1, 0, 10, 10, 0.7f),
      obj (0, 0, 0, 10, 10, 0.7f),
    };
    const guint keep[] = { 0, 1 };

    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, keep, G_N_ELEMENTS (keep));
    g_array_unref (out);
    in[1].left = 0;
    in[2].left = 1;
    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, keep, G_N_ELEMENTS (keep));
    g_array_unref (out);
  }
  {
    /* Greedy: 1 is suppressed by 0, so it doesn't suppress 2 (which overlaps 1 only). */
    RdsInferObjectDetectionInfo in[] = {
      obj (0, 0, 0, 10, 10, 0.9f),
      obj (0, 2, 0, 10, 10, 0.8f),
      obj (0, 10, 0, 10, 10, 0.7f),     /* IoU with 1: 20 / 180, with 0: 0 */
    };
    const guint keep[] = { 0, 2 };

    cfg->class_attrs[0].nms_iou_threshold = 0.1f;
    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, keep, G_N_ELEMENTS (keep));
    g_array_unref (out);
    /* A threshold of 0 suppresses any overlap; boxes that only touch don't overlap. */
    cfg->class_attrs[0].nms_iou_threshold = 0.0f;
    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, keep, G_N_ELEMENTS (keep));
    g_array_unref (out);
  }
  rds_infer_config_free (cfg);
}

static void
test_topk (void)
{
  RdsInferConfig *cfg = make_cfg (2);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, 0, 0, 10, 10, 0.5f),
    obj (0, 20, 0, 10, 10, 0.9f),
    obj (0, 40, 0, 10, 10, 0.7f),
    obj (0, 60, 0, 10, 10, 0.6f),
    obj (1, 0, 0, 10, 10, 0.3f),
    obj (1, 20, 0, 10, 10, 0.4f),
    obj (0, 21, 0, 10, 10, 0.8f),       /* suppressed by 1: doesn't count towards topk */
  };
  const guint all[] = { 0, 1, 2, 3, 4, 5 }, top2[] = { 1, 2, 4, 5 }, none[] = { 4, 5 };
  GArray *out;

  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, all, G_N_ELEMENTS (all));
  g_array_unref (out);

  /* Per class: class 1 keeps everything. */
  cfg->class_attrs[0].topk = 2;
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, top2, G_N_ELEMENTS (top2));
  g_array_unref (out);

  cfg->class_attrs[0].topk = 0;
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, none, G_N_ELEMENTS (none));
  g_array_unref (out);

  /* More than there are. */
  cfg->class_attrs[0].topk = 100;
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, all, G_N_ELEMENTS (all));
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

static void
test_post_cluster_threshold (void)
{
  RdsInferConfig *cfg = make_cfg (2);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, 0, 0, 10, 10, 0.6f),
    obj (0, 20, 0, 10, 10, 0.5f),
    obj (0, 40, 0, 10, 10, 0.4f),
    obj (1, 60, 0, 10, 10, 0.4f),
    /* Suppresses 5 in NMS, then fails the post-cluster threshold: neither is output. */
    obj (0, 100, 0, 10, 10, 0.45f),
    obj (0, 101, 0, 10, 10, 0.44f),
  };
  const guint keep[] = { 0, 1, 3 };
  GArray *out;

  cfg->class_attrs[0].post_cluster_threshold = 0.5f;
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, keep, G_N_ELEMENTS (keep));
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

/* ---- Letterbox ---- */

typedef struct
{
  guint net_w, net_h, frame_w, frame_h;
  gboolean keep_aspect, symmetric;
  guint pad_left, pad_top, scaled_w, scaled_h;
} LetterboxCase;

/* Computed by hand: scale = min (net_w / frame_w, net_h / frame_h), sizes rounded to nearest (halves up). */
static const LetterboxCase letterbox_cases[] = {
  /* Wider than the network. */
  { 640, 640, 1920, 1080, TRUE, FALSE, 0, 0, 640, 360 },
  { 640, 640, 1920, 1080, TRUE, TRUE, 0, 140, 640, 360 },
  /* Taller. */
  { 640, 640, 1080, 1920, TRUE, FALSE, 0, 0, 360, 640 },
  { 640, 640, 1080, 1920, TRUE, TRUE, 140, 0, 360, 640 },
  /* 333 * 0.64 = 213.12 -> 213; floor (427 / 2) = 213. */
  { 640, 640, 1000, 333, TRUE, TRUE, 0, 213, 640, 213 },
  /* 721 * 0.5 = 360.5 -> 361; floor (279 / 2) = 139. */
  { 640, 640, 1280, 721, TRUE, TRUE, 0, 139, 640, 361 },
  { 640, 640, 1280, 721, TRUE, FALSE, 0, 0, 640, 361 },
  /* 1080 * 416 / 1920 = 234; floor (182 / 2) = 91. */
  { 416, 416, 1920, 1080, TRUE, TRUE, 0, 91, 416, 234 },
  /* Non-square network, width-limited: 1080 / 3 = 360; floor (24 / 2) = 12. */
  { 640, 384, 1920, 1080, TRUE, TRUE, 0, 12, 640, 360 },
  /* Height-limited: 1280 * 384 / 1080 = 455.11 -> 455; floor (185 / 2) = 92. */
  { 640, 384, 1280, 1080, TRUE, TRUE, 92, 0, 455, 384 },
  /* Upscaling. */
  { 640, 640, 320, 180, TRUE, TRUE, 0, 140, 640, 360 },
  /* Same aspect: no padding either way. */
  { 640, 640, 1080, 1080, TRUE, TRUE, 0, 0, 640, 640 },
  /* 1 * 0.64 -> 1; 1 * 0.064 -> 0, at least 1. */
  { 640, 640, 1, 1000, TRUE, TRUE, 319, 0, 1, 640 },
  { 640, 640, 1, 10000, TRUE, FALSE, 0, 0, 1, 640 },
  /* No aspect ratio: each axis on its own, symmetric-padding has nothing to do. */
  { 640, 640, 1920, 1080, FALSE, FALSE, 0, 0, 640, 640 },
  { 640, 640, 1920, 1080, FALSE, TRUE, 0, 0, 640, 640 },
  { 640, 384, 1080, 1920, FALSE, TRUE, 0, 0, 640, 384 },
};

static void
test_letterbox (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (letterbox_cases); i++) {
    const LetterboxCase *c = &letterbox_cases[i];
    RdsInferLetterbox lb;
    gfloat m[4];

    g_test_message ("case %u: net %ux%u frame %ux%u keep %d symmetric %d", i, c->net_w, c->net_h, c->frame_w,
        c->frame_h, c->keep_aspect, c->symmetric);
    rds_infer_letterbox (c->net_w, c->net_h, c->frame_w, c->frame_h, c->keep_aspect, c->symmetric, &lb);
    g_assert_cmpuint (lb.net_w, ==, c->net_w);
    g_assert_cmpuint (lb.net_h, ==, c->net_h);
    g_assert_cmpuint (lb.frame_w, ==, c->frame_w);
    g_assert_cmpuint (lb.frame_h, ==, c->frame_h);
    g_assert_cmpuint (lb.pad_left, ==, c->pad_left);
    g_assert_cmpuint (lb.pad_top, ==, c->pad_top);
    g_assert_cmpuint (lb.scaled_w, ==, c->scaled_w);
    g_assert_cmpuint (lb.scaled_h, ==, c->scaled_h);
    g_assert_cmpfloat (lb.scale_x, ==, (gfloat) c->scaled_w / c->frame_w);
    g_assert_cmpfloat (lb.scale_y, ==, (gfloat) c->scaled_h / c->frame_h);

    /* The scaled frame's edges map exactly onto the frame's. */
    rds_infer_letterbox_map (&lb, lb.pad_left, lb.pad_top, lb.scaled_w, lb.scaled_h, m);
    g_assert_cmpfloat (m[0], ==, 0);
    g_assert_cmpfloat (m[1], ==, 0);
    g_assert_cmpfloat (m[2], ==, c->frame_w);
    g_assert_cmpfloat (m[3], ==, c->frame_h);
    g_assert_cmpfloat (m[0] + m[2], ==, c->frame_w);
    g_assert_cmpfloat (m[1] + m[3], ==, c->frame_h);
    /* So does the centre. */
    rds_infer_letterbox_map (&lb, lb.pad_left + lb.scaled_w / 2.0f, lb.pad_top + lb.scaled_h / 2.0f, 0, 0, m);
    g_assert_cmpfloat_with_epsilon (m[0], c->frame_w / 2.0, 1e-3);
    g_assert_cmpfloat_with_epsilon (m[1], c->frame_h / 2.0, 1e-3);
  }
}

static void
test_map (void)
{
  RdsInferConfig *cfg = make_cfg (1);
  RdsInferObjectDetectionInfo in[] = { obj (0, 10, 150, 30, 60, 0.9f) };
  RdsInferLetterbox lb;
  GArray *out;

  /* 1920x1080 -> 640x360 at (0, 140): x3. */
  rds_infer_letterbox (640, 640, 1920, 1080, TRUE, TRUE, &lb);
  out = post (cfg, &lb, in, 1, NULL);
  g_assert_cmpuint (out->len, ==, 1);
  assert_box (&det (out, 0)->box, 30, 30, 90, 180);
  assert_box (&det (out, 0)->rect, 30, 30, 90, 180);
  g_array_unref (out);

  /* Top-left: no offset. */
  rds_infer_letterbox (640, 640, 1920, 1080, TRUE, FALSE, &lb);
  out = post (cfg, &lb, in, 1, NULL);
  assert_box (&det (out, 0)->box, 30, 450, 90, 180);
  g_array_unref (out);

  /* Taller: 1080x1920 -> 360x640 at (140, 0). */
  in[0] = obj (0, 150, 10, 60, 30, 0.9f);
  rds_infer_letterbox (640, 640, 1080, 1920, TRUE, TRUE, &lb);
  out = post (cfg, &lb, in, 1, NULL);
  assert_box (&det (out, 0)->box, 30, 30, 180, 90);
  g_array_unref (out);

  /* Per axis: x3 and x1.6875. */
  in[0] = obj (0, 64, 64, 64, 64, 0.9f);
  rds_infer_letterbox (640, 640, 1920, 1080, FALSE, FALSE, &lb);
  out = post (cfg, &lb, in, 1, NULL);
  assert_box (&det (out, 0)->box, 192, 108, 192, 108);
  g_array_unref (out);

  /* Rounded sizes: 1000x333 -> 640x213 at (0, 213), y scale 213 / 333. */
  in[0] = obj (0, 64, 213 + 71, 64, 71, 0.9f);
  rds_infer_letterbox (640, 640, 1000, 333, TRUE, TRUE, &lb);
  out = post (cfg, &lb, in, 1, NULL);
  assert_box (&det (out, 0)->box, 100, 111, 100, 111);
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

/* ---- Frame-space steps ---- */

static void
test_filter_out (void)
{
  RdsInferConfig *cfg = make_cfg (3);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, 0, 0, 10, 10, 0.9f),
    obj (1, 0, 0, 10, 10, 0.9f),
    obj (2, 0, 0, 10, 10, 0.9f),
  };
  const guint keep[] = { 0 };
  const guint filter[] = { 2, 1 };
  GArray *out;

  g_array_append_vals (cfg->filter_out_class_ids, filter, G_N_ELEMENTS (filter));
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, keep, G_N_ELEMENTS (keep));
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

static void
test_roi (void)
{
  RdsInferConfig *cfg = make_cfg (2);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, 0, 90, 10, 18, 0.9f),       /* centre y 99: above the band */
    obj (0, 20, 90, 10, 20, 0.9f),      /* 100: in */
    obj (0, 40, 530, 10, 19, 0.9f),     /* 539.5: in */
    obj (0, 60, 530, 10, 20, 0.9f),     /* 540: below the band (640 - 100) */
    obj (1, 80, 0, 10, 20, 0.9f),       /* class 1 has no band */
    obj (0, 100, -100, 10, 160, 0.9f),  /* centre outside the frame, partly inside */
  };
  const guint all[] = { 0, 1, 2, 3, 4, 5 }, band[] = { 1, 2, 4 };
  GArray *out;

  /* Both offsets 0: no band, even for a centre outside the frame. */
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, all, G_N_ELEMENTS (all));
  g_array_unref (out);

  cfg->class_attrs[0].roi_top_offset = 100;
  cfg->class_attrs[0].roi_bottom_offset = 100;
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, band, G_N_ELEMENTS (band));
  g_array_unref (out);

  /* In frame pixels: 1920x1080 at x3, band [300, 1080 - 30). */
  {
    RdsInferObjectDetectionInfo net[] = {
      obj (0, 0, 140 + 99, 10, 2, 0.9f),        /* centre 100 * 3 = 300: in */
      obj (0, 0, 140 + 98, 10, 2, 0.9f),        /* 297: out */
      obj (0, 0, 140 + 349, 10, 2, 0.9f),       /* 1050: out */
      obj (0, 0, 140 + 348, 10, 2, 0.9f),       /* 1047: in */
    };
    RdsInferLetterbox lb;

    cfg->class_attrs[0].roi_top_offset = 300;
    cfg->class_attrs[0].roi_bottom_offset = 30;
    rds_infer_letterbox (640, 640, 1920, 1080, TRUE, TRUE, &lb);
    out = post (cfg, &lb, net, G_N_ELEMENTS (net), NULL);
    g_assert_cmpuint (out->len, ==, 2);
    assert_box (&det (out, 0)->box, 0, 297, 30, 6);
    assert_box (&det (out, 1)->box, 0, 1044, 30, 6);
    g_array_unref (out);
  }

  /* A band of no height drops everything of that class. */
  cfg->class_attrs[0].roi_top_offset = 320;
  cfg->class_attrs[0].roi_bottom_offset = 320;
  {
    const guint only1[] = { 4 };

    out = post_identity (cfg, in, G_N_ELEMENTS (in));
    assert_kept (out, in, only1, G_N_ELEMENTS (only1));
    g_array_unref (out);
  }
  rds_infer_config_free (cfg);
}

static void
test_min_max (void)
{
  RdsInferConfig *cfg = make_cfg (2);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, 0, 0, 19, 50, 0.9f),        /* too narrow */
    obj (0, 30, 0, 20, 50, 0.9f),       /* 1 */
    obj (0, 60, 0, 100, 50, 0.9f),      /* 2 */
    obj (0, 200, 0, 101, 50, 0.9f),     /* too wide */
    obj (0, 400, 0, 50, 9, 0.9f),       /* too short */
    obj (0, 500, 0, 50, 600, 0.9f),     /* 5: no maximum height */
    obj (1, 0, 300, 5, 5, 0.9f),        /* 6: class 1 has no limits */
  };
  const guint keep[] = { 1, 2, 5, 6 };
  GArray *out;

  cfg->class_attrs[0].detected_min_w = 20;
  cfg->class_attrs[0].detected_min_h = 10;
  cfg->class_attrs[0].detected_max_w = 100;
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, keep, G_N_ELEMENTS (keep));
  g_array_unref (out);

  /* On the mapped size: 7 network pixels are 21 frame pixels at x3. */
  {
    RdsInferObjectDetectionInfo small[] = { obj (0, 0, 150, 7, 7, 0.9f), obj (0, 20, 150, 6, 6, 0.9f) };
    RdsInferLetterbox lb;

    rds_infer_letterbox (640, 640, 1920, 1080, TRUE, TRUE, &lb);
    cfg->class_attrs[0].detected_min_h = 0;
    out = post (cfg, &lb, small, G_N_ELEMENTS (small), NULL);
    g_assert_cmpuint (out->len, ==, 1);
    assert_box (&det (out, 0)->box, 0, 30, 21, 21);
    g_array_unref (out);
  }
  /* On the unclipped size: this box is 60 wide, 10 of it inside the frame. */
  {
    RdsInferObjectDetectionInfo edge[] = { obj (0, -50, 0, 60, 50, 0.9f) };

    out = post_identity (cfg, edge, 1);
    g_assert_cmpuint (out->len, ==, 1);
    assert_box (&det (out, 0)->rect, 0, 0, 10, 50);
    g_array_unref (out);
  }
  rds_infer_config_free (cfg);
}

static void
test_clip (void)
{
  RdsInferConfig *cfg = make_cfg (1);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, -10, 600, 50, 100, 0.9f),   /* partly outside: clipped */
    obj (0, 650, 0, 10, 10, 0.9f),      /* fully outside */
    obj (0, 640, 0, 10, 10, 0.9f),      /* touches the right edge: empty */
    obj (0, 0, -10, 10, 10, 0.9f),      /* touches the top edge: empty */
    obj (0, -5, -5, 700, 700, 0.9f),    /* covers the frame */
    obj (0, 100, 100, 10, 10, 0.9f),    /* inside */
  };
  GArray *out;

  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  g_assert_cmpuint (out->len, ==, 3);
  assert_box (&det (out, 0)->box, -10, 600, 50, 100);
  assert_box (&det (out, 0)->rect, 0, 600, 40, 40);
  assert_box (&det (out, 1)->box, -5, -5, 700, 700);
  assert_box (&det (out, 1)->rect, 0, 0, 640, 640);
  assert_box (&det (out, 2)->box, 100, 100, 10, 10);
  assert_box (&det (out, 2)->rect, 100, 100, 10, 10);
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

/* cluster-mode=4: no NMS and no topk; the thresholds still apply. */
static void
test_cluster_none (void)
{
  RdsInferConfig *cfg = make_cfg (1);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, 0, 0, 10, 10, 0.9f),
    obj (0, 0, 0, 10, 10, 0.8f),
    obj (0, 1, 1, 10, 10, 0.7f),
    obj (0, 1, 1, 10, 10, 0.1f),
    obj (0, 1, 1, 10, 10, 0.3f),
  };
  const guint keep[] = { 0, 1, 2 };
  GArray *out;

  cfg->cluster_mode = 4;
  cfg->class_attrs[0].topk = 0;
  cfg->class_attrs[0].post_cluster_threshold = 0.5f;
  out = post_identity (cfg, in, G_N_ELEMENTS (in));
  assert_kept (out, in, keep, G_N_ELEMENTS (keep));
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

/* All steps at once, on a 1920x1080 frame letterboxed (centred) into 640x640: x3, 140 rows of padding on top. */
static void
test_combined (void)
{
  RdsInferConfig *cfg = make_cfg (3);
  RdsInferObjectDetectionInfo in[] = {
    obj (0, 10, 150, 30, 60, 0.9f),     /* 0: kept -> (30, 30) 90x180 */
    obj (0, 11, 151, 30, 60, 0.85f),    /* NMS: suppressed by 0 */
    obj (0, 300, 300, 20, 20, 0.2f),    /* pre-cluster threshold */
    obj (0, 400, 300, 20, 20, 0.6f),    /* 3: topk 3rd of class 0 */
    obj (0, 500, 300, 20, 20, 0.7f),    /* 4: kept -> (1500, 480) 60x60 */
    obj (1, 10, 150, 30, 60, 0.95f),    /* other class, same box as 0: filtered out */
    obj (2, 600, 480, 60, 20, 0.5f),    /* 6: clipped -> (1800, 1020) 120x60 */
    obj (2, 100, 141, 20, 10, 0.5f),    /* centre y 18: outside class 2's band */
    obj (2, 200, 400, 2, 20, 0.5f),     /* 6 frame pixels wide: below class 2's minimum */
    obj (2, 300, 400, 20, 20, 0.35f),   /* post-cluster threshold */
    obj (5, 300, 400, 20, 20, 0.9f),    /* invalid class */
  };
  RdsInferLetterbox lb;
  RdsInferPostFlags flags;
  GArray *out;
  const guint filter = 1;

  cfg->class_attrs[0].topk = 2;
  cfg->class_attrs[2].roi_top_offset = 30;
  cfg->class_attrs[2].detected_min_w = 10;
  cfg->class_attrs[2].post_cluster_threshold = 0.4f;
  g_array_append_val (cfg->filter_out_class_ids, filter);
  rds_infer_letterbox (640, 640, 1920, 1080, TRUE, TRUE, &lb);
  out = post (cfg, &lb, in, G_N_ELEMENTS (in), &flags);
  g_assert_cmpint (flags, ==, RDS_INFER_POST_INVALID);
  g_assert_cmpuint (out->len, ==, 3);
  g_assert_cmpuint (det (out, 0)->class_id, ==, 0);
  g_assert_cmpfloat (det (out, 0)->confidence, ==, 0.9f);
  assert_box (&det (out, 0)->box, 30, 30, 90, 180);
  assert_box (&det (out, 0)->rect, 30, 30, 90, 180);
  g_assert_cmpuint (det (out, 1)->class_id, ==, 0);
  g_assert_cmpfloat (det (out, 1)->confidence, ==, 0.7f);
  assert_box (&det (out, 1)->box, 1500, 480, 60, 60);
  g_assert_cmpuint (det (out, 2)->class_id, ==, 2);
  assert_box (&det (out, 2)->box, 1800, 1020, 180, 60);
  assert_box (&det (out, 2)->rect, 1800, 1020, 120, 60);
  g_array_unref (out);
  rds_infer_config_free (cfg);
}

/* ---- Random sets against the reference ---- */

static gfloat
pick_f (GRand * r, const gfloat * values, guint n)
{
  return values[g_rand_int_range (r, 0, n)];
}

static void
random_config (GRand * r, RdsInferConfig * cfg)
{
  static const gfloat iou[] = { 0.0f, 0.3f, 0.45f, 0.5f, 0.7f, 1.0f };
  static const gfloat pre[] = { 0.0f, 0.25f, 0.5f };
  static const gfloat postc[] = { 0.0f, 0.0f, 0.3f, 0.6f };
  static const gint topk[] = { -1, -1, -1, 0, 1, 2, 5 };

  cfg->cluster_mode = g_rand_int_range (r, 0, 5) == 0 ? 4 : 2;
  for (guint c = 0; c < cfg->num_detected_classes; c++) {
    RdsInferClassAttrs *a = &cfg->class_attrs[c];

    a->nms_iou_threshold = pick_f (r, iou, G_N_ELEMENTS (iou));
    a->pre_cluster_threshold = pick_f (r, pre, G_N_ELEMENTS (pre));
    a->post_cluster_threshold = pick_f (r, postc, G_N_ELEMENTS (postc));
    a->topk = topk[g_rand_int_range (r, 0, G_N_ELEMENTS (topk))];
    a->roi_top_offset = g_rand_int_range (r, 0, 4) == 0 ? (guint) g_rand_int_range (r, 0, 300) : 0;
    a->roi_bottom_offset = g_rand_int_range (r, 0, 4) == 0 ? (guint) g_rand_int_range (r, 0, 300) : 0;
    a->detected_min_w = g_rand_int_range (r, 0, 4) == 0 ? (guint) g_rand_int_range (r, 0, 60) : 0;
    a->detected_min_h = g_rand_int_range (r, 0, 4) == 0 ? (guint) g_rand_int_range (r, 0, 60) : 0;
    a->detected_max_w = g_rand_int_range (r, 0, 4) == 0 ? (guint) g_rand_int_range (r, 0, 400) : 0;
    a->detected_max_h = g_rand_int_range (r, 0, 4) == 0 ? (guint) g_rand_int_range (r, 0, 400) : 0;
  }
  g_array_set_size (cfg->filter_out_class_ids, 0);
  if (g_rand_int_range (r, 0, 4) == 0) {
    guint id = g_rand_int_range (r, 0, cfg->num_detected_classes + 1);

    g_array_append_val (cfg->filter_out_class_ids, id);
  }
}

/* Coordinates on a half-pixel grid and confidences in tenths, so overlaps, IoUs at the threshold and equal
 * confidences all happen. Objects cluster around a few centres. */
static void
random_objects (GRand * r, guint n_classes, RdsInferObjectDetectionInfo * in, guint n)
{
  gfloat cx[4], cy[4];

  for (guint i = 0; i < G_N_ELEMENTS (cx); i++) {
    cx[i] = g_rand_int_range (r, -40, 700);
    cy[i] = g_rand_int_range (r, -40, 700);
  }
  for (guint i = 0; i < n; i++) {
    guint k = g_rand_int_range (r, 0, G_N_ELEMENTS (cx));
    RdsInferObjectDetectionInfo *o = &in[i];

    o->classId = g_rand_int_range (r, 0, 20) == 0 ? n_classes : (guint) g_rand_int_range (r, 0, n_classes);
    o->width = g_rand_int_range (r, 0, 240) / 2.0f;
    o->height = g_rand_int_range (r, 0, 240) / 2.0f;
    o->left = cx[k] + g_rand_int_range (r, -40, 40) / 2.0f - o->width / 2;
    o->top = cy[k] + g_rand_int_range (r, -40, 40) / 2.0f - o->height / 2;
    o->detectionConfidence = g_rand_int_range (r, 0, 11) / 10.0f;
    o->rotation_angle = g_rand_int_range (r, 0, 20) == 0 ? 0.25f : 0.0f;
    if (g_rand_int_range (r, 0, 50) == 0)
      o->left = NAN;
    if (g_rand_int_range (r, 0, 50) == 0)
      o->detectionConfidence = INFINITY;
  }
}

static void
test_random (void)
{
  static const guint frames[][2] = { {640, 640}, {1920, 1080}, {1080, 1920}, {1280, 721}, {320, 180} };
  const guint32 seed = 20261025;
  GRand *r = g_rand_new_with_seed (seed);
  guint total = 0;

  g_test_message ("seed %u", seed);
  for (guint set = 0; set < 200; set++) {
    RdsInferConfig *cfg = make_cfg (g_rand_int_range (r, 1, 5));
    guint n = g_rand_int_range (r, 0, 80);
    RdsInferObjectDetectionInfo *in = g_new0 (RdsInferObjectDetectionInfo, MAX (n, 1));
    guint f = g_rand_int_range (r, 0, G_N_ELEMENTS (frames));
    RdsInferLetterbox lb;
    GArray *got = detections_new (), *want = detections_new ();

    random_config (r, cfg);
    random_objects (r, cfg->num_detected_classes, in, n);
    rds_infer_letterbox (640, 640, frames[f][0], frames[f][1], g_rand_boolean (r), g_rand_boolean (r), &lb);
    rds_infer_postprocess (cfg, &lb, in, n, got);
    ref_postprocess (cfg, &lb, in, n, want);
    if (got->len != want->len)
      g_error ("set %u: %u detections, the reference has %u", set, got->len, want->len);
    for (guint i = 0; i < got->len; i++) {
      const RdsInferDetection *a = &g_array_index (got, RdsInferDetection, i);
      const RdsInferDetection *b = &g_array_index (want, RdsInferDetection, i);

      if (memcmp (a, b, sizeof (*a)) != 0)
        g_error ("set %u, detection %u: class %u conf %g (%g, %g) %gx%g, the reference has class %u conf %g "
            "(%g, %g) %gx%g", set, i, a->class_id, a->confidence, a->box.left, a->box.top, a->box.width,
            a->box.height, b->class_id, b->confidence, b->box.left, b->box.top, b->box.width, b->box.height);
    }
    total += got->len;
    g_array_unref (got);
    g_array_unref (want);
    g_free (in);
    rds_infer_config_free (cfg);
  }
  g_test_message ("%u detections in 200 sets", total);
  /* The sets exercise something. */
  g_assert_cmpuint (total, >, 200);
  g_rand_free (r);
}

/* ---- Timings ---- */

static gint
cmp_int64 (gconstpointer a, gconstpointer b)
{
  gint64 x = *(const gint64 *) a, y = *(const gint64 *) b;

  return x < y ? -1 : x > y;
}

/* Parses @t and post-processes the result @runs times; reports the median of each. */
static void
time_yolo (const gchar * what, RdsInferParser * parser, const RdsInferConfig * cfg, YoloTensor * t, guint runs)
{
  gint64 *parse_us = g_new (gint64, runs), *post_us = g_new (gint64, runs);
  GArray *objects = g_array_new (FALSE, FALSE, sizeof (RdsInferObjectDetectionInfo));
  GArray *out = detections_new ();
  RdsInferLetterbox lb;
  guint n_objects = 0, n_out = 0;

  rds_infer_letterbox (640, 640, 1920, 1080, TRUE, TRUE, &lb);
  for (guint i = 0; i < runs; i++) {
    GError *err = NULL;
    gint64 t0, t1, t2;

    g_array_set_size (objects, 0);
    g_array_set_size (out, 0);
    t0 = g_get_monotonic_time ();
    g_assert_true (rds_infer_parser_parse (parser, &t->layer, 1, &yolo_net, objects, &err));
    t1 = g_get_monotonic_time ();
    rds_infer_postprocess (cfg, &lb, (const RdsInferObjectDetectionInfo *) (gpointer) objects->data,
        objects->len, out);
    t2 = g_get_monotonic_time ();
    g_assert_no_error (err);
    parse_us[i] = t1 - t0;
    post_us[i] = t2 - t1;
    n_objects = objects->len;
    n_out = out->len;
  }
  qsort (parse_us, runs, sizeof (gint64), cmp_int64);
  qsort (post_us, runs, sizeof (gint64), cmp_int64);
  g_test_message ("%s: [84, 8400] parse %.3f ms (%u objects), post-processing %.3f ms (%u detections), median of "
      "%u", what, parse_us[runs / 2] / 1000.0, n_objects, post_us[runs / 2] / 1000.0, n_out, runs);
  g_free (parse_us);
  g_free (post_us);
  g_array_unref (objects);
  g_array_unref (out);
}

static void
test_timing (void)
{
  gfloat pre[YOLO_CLASSES];
  RdsInferConfig *cfg = make_cfg (YOLO_CLASSES);
  RdsInferParser *parser;
  GRand *r = g_rand_new_with_seed (7);
  YoloTensor t;

  for (guint i = 0; i < YOLO_CLASSES; i++)
    pre[i] = 0.25f;
  parser = yolo_parser (YOLO_CLASSES, pre);
  yolo_init (&t, YOLO_ANCHORS, YOLO_CLASSES, FALSE);

  /* Typical: low scores everywhere, 1% of the anchors above the threshold, in clusters of overlapping boxes. */
  for (guint a = 0; a < YOLO_ANCHORS; a++) {
    yolo_set_box (&t, a, g_rand_double_range (r, 0, 640), g_rand_double_range (r, 0, 640), 20, 20);
    for (guint c = 0; c < YOLO_CLASSES; c++)
      yolo_set_score (&t, a, c, g_rand_double_range (r, 0, 0.05));
  }
  for (guint a = 0; a < YOLO_ANCHORS; a += 100) {
    yolo_set_box (&t, a, 50 + (a / 100) % 10 * 60 + a % 7, 50 + (a / 1000) * 60, 40, 80);
    yolo_set_score (&t, a, (a / 100) % 4, g_rand_double_range (r, 0.3, 0.95));
  }
  time_yolo ("1% of the anchors above the threshold", parser, cfg, &t, 51);

  /* Worst case for NMS: every anchor above the threshold, one class, no overlaps (nothing suppressed, so every box
   * is compared with every kept one). */
  memset (t.data, 0, sizeof (gfloat) * YOLO_ROWS * YOLO_ANCHORS);
  for (guint a = 0; a < YOLO_ANCHORS; a++) {
    yolo_set_box (&t, a, 3 + (a % 92) * 6.5f, 3 + (a / 92) * 6.5f, 4, 4);
    yolo_set_score (&t, a, 0, 0.5f + (a % 97) / 200.0f);
  }
  time_yolo ("every anchor above the threshold, one class, no overlaps", parser, cfg, &t, 3);

  yolo_clear (&t);
  g_rand_free (r);
  rds_infer_parser_free (parser);
  rds_infer_config_free (cfg);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/infer-yolo/parse", test_yolo_parse);
  g_test_add_func ("/cpu/infer-yolo/parse-transposed", test_yolo_parse_transposed);
  g_test_add_func ("/cpu/infer-yolo/square", test_yolo_square);
  g_test_add_func ("/cpu/infer-yolo/bad-layers", test_yolo_bad_layers);
  g_test_add_func ("/cpu/infer-yolo/builtin-unknown", test_builtin_unknown);
  g_test_add_func ("/cpu/infer-post/validate", test_validate);
  g_test_add_func ("/cpu/infer-post/nms", test_nms);
  g_test_add_func ("/cpu/infer-post/topk", test_topk);
  g_test_add_func ("/cpu/infer-post/post-cluster-threshold", test_post_cluster_threshold);
  g_test_add_func ("/cpu/infer-post/letterbox", test_letterbox);
  g_test_add_func ("/cpu/infer-post/map", test_map);
  g_test_add_func ("/cpu/infer-post/filter-out", test_filter_out);
  g_test_add_func ("/cpu/infer-post/roi", test_roi);
  g_test_add_func ("/cpu/infer-post/min-max", test_min_max);
  g_test_add_func ("/cpu/infer-post/clip", test_clip);
  g_test_add_func ("/cpu/infer-post/cluster-none", test_cluster_none);
  g_test_add_func ("/cpu/infer-post/combined", test_combined);
  g_test_add_func ("/cpu/infer-post/random", test_random);
  g_test_add_func ("/cpu/infer-post/timing", test_timing);
  return g_test_run ();
}
