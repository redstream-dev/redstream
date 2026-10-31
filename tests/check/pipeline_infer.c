/* rdsinfer end to end with YOLO11n (gpu): detections on a public street photo against Ultralytics' own prediction
 * (through RGBA and through NV12 batches), the same frame on four sources at batch 4 against batch 1, YOLO11n-cls in
 * secondary mode (the rds-test2 sample's classifier config) on crops of the photo's vehicles (the top-1 against ONNX
 * Runtime's on the same stretched crop; at least 80 % in Ultralytics' top 5, its top-1 rate printed only), and a long
 * four-source run through the decoder and the mux (the soak, its own test entry) that neither leaks host nor GPU
 * memory.
 * The YOLO11n and YOLO11n-cls models and the reference data come from tests/models/fetch-yolo11n.sh and
 * fetch-yolo11n-cls.sh (.cache/models, never committed); the tests that need them skip without them. Without them the
 * soak runs the committed scale2 model, so the leak check always runs.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include "rds-test-media.h"
#include "rds-test-soak.h"

#include <gst/app/gstappsrc.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <rds/rds.h>
#include <string.h>
#include <unistd.h>

#define YOLO RDS_TEST_CACHE_MODELS "/yolo11n.onnx"
#define LABELS RDS_TEST_CACHE_MODELS "/coco-labels.txt"
#define STREET RDS_TEST_CACHE_MODELS "/street-1280x720.rgba"
#define STREET_REF RDS_TEST_CACHE_MODELS "/street-ref.json"
#define FETCH_HINT "run tests/models/fetch-yolo11n.sh"
#define YOLO_CLS RDS_TEST_CACHE_MODELS "/yolo11n-cls.onnx"
#define IMAGENET_LABELS RDS_TEST_CACHE_MODELS "/imagenet-labels.txt"
#define STREET_CLS_REF RDS_TEST_CACHE_MODELS "/street-cls-ref.json"
#define CLS_FETCH_HINT "run tests/models/fetch-yolo11n-cls.sh"
#define SGIE_CONFIG RDS_TEST_SAMPLES "/rds-test2/rds_test2_sgie_config.yml"
/* The same directory as infer_engine's YOLO11n cases, so both use one cached batch-1 program. */
#define YOLO_DIR RDS_TEST_BUILD_DIR "/infer-engine-yolo"
#define WORK_DIR RDS_TEST_BUILD_DIR "/pipeline-infer"

#define STREET_W 1280
#define STREET_H 720
#define N_CLASSES 80

static gboolean
have_yolo (const gchar * const *also)
{
  if (!g_file_test (YOLO, G_FILE_TEST_IS_REGULAR) || !g_file_test (LABELS, G_FILE_TEST_IS_REGULAR)) {
    g_test_skip ("no " YOLO " (" FETCH_HINT ")");
    return FALSE;
  }
  if (!rds_test_have_element ("rawvideoparse")) {
    g_test_skip ("rawvideoparse not available");
    return FALSE;
  }
  for (; also && *also; also++) {
    if (!g_file_test (*also, G_FILE_TEST_IS_REGULAR)) {
      gchar *msg = g_strdup_printf ("no %s (" FETCH_HINT ")", *also);

      g_test_skip (msg);
      g_free (msg);
      return FALSE;
    }
  }
  return TRUE;
}

/* The fetched model behind a symlink in YOLO_DIR, so its programs are cached there and not next to the model. */
static gchar *
yolo_link (void)
{
  gchar *link = g_build_filename (YOLO_DIR, "yolo11n.onnx", NULL);

  g_assert_cmpint (g_mkdir_with_parents (YOLO_DIR, 0755), ==, 0);
  if (!g_file_test (link, G_FILE_TEST_IS_SYMLINK))
    g_assert_cmpint (symlink (YOLO, link), ==, 0);
  return link;
}

/* Writes an nvinfer key-file config into WORK_DIR; returns its path. YOLO11n as Ultralytics runs it: RGB scaled to
 * [0, 1], letterboxed into 640x640 (centred), confidence 0.25, per-class NMS at IoU 0.45. */
static gchar *
yolo_config (guint batch)
{
  gchar *link = yolo_link ();
  gchar *name = g_strdup_printf ("yolo-b%u.txt", batch);
  gchar *path = g_build_filename (WORK_DIR, name, NULL);
  gchar *text = g_strdup_printf ("[property]\nonnx-file=%s\nlabelfile-path=" LABELS "\ninfer-dims=3;640;640\n"
      "batch-size=%u\nnetwork-type=0\nnum-detected-classes=%u\nnet-scale-factor=0.00392156862745098\n"
      "model-color-format=0\nmaintain-aspect-ratio=1\nsymmetric-padding=1\ncluster-mode=2\n"
      "parse-bbox-func-name=RdsInferParseYolo\ngie-unique-id=1\n"
      "[class-attrs-all]\npre-cluster-threshold=0.25\nnms-iou-threshold=0.45\n", link, batch, N_CLASSES);
  GError *err = NULL;

  g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  g_free (text);
  g_free (name);
  g_free (link);
  return path;
}

/* scale2 (output = 2 x input) at 640x640 with tensor output only: the soak's stand-in without YOLO11n. It is
 * copied into WORK_DIR, so its programs are cached there. */
static gchar *
scale2_config (guint batch)
{
  gchar *model = g_build_filename (WORK_DIR, "scale2.onnx", NULL);
  gchar *path = g_build_filename (WORK_DIR, "scale2-soak.txt", NULL);
  gchar *contents = NULL, *text;
  gsize len = 0;
  GError *err = NULL;

  g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
  if (!g_file_test (model, G_FILE_TEST_EXISTS)) {
    g_assert_true (g_file_get_contents (RDS_TEST_MODELS "/scale2.onnx", &contents, &len, &err));
    g_assert_true (g_file_set_contents (model, contents, (gssize) len, &err));
    g_free (contents);
  }
  text = g_strdup_printf ("[property]\nonnx-file=%s\ninfer-dims=3;640;640\nbatch-size=%u\nnetwork-type=100\n"
      "output-tensor-meta=1\nnet-scale-factor=0.00392156862745098\nmaintain-aspect-ratio=1\n"
      "symmetric-padding=1\n", model, batch);
  g_assert_true (g_file_set_contents (path, text, -1, &err));
  g_assert_no_error (err);
  g_free (text);
  g_free (model);
  return path;
}

/* Waits for EOS on the bus; fails on an error or after @timeout. */
static void
wait_eos (GstElement * pipe, GstClockTime timeout)
{
  GstBus *bus = gst_element_get_bus (pipe);
  GstMessage *msg = gst_bus_timed_pop_filtered (bus, timeout, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);

  if (msg == NULL)
    g_error ("no EOS within %" GST_TIME_FORMAT, GST_TIME_ARGS (timeout));
  if (GST_MESSAGE_TYPE (msg) == GST_MESSAGE_ERROR) {
    GError *err = NULL;
    gchar *dbg = NULL;

    gst_message_parse_error (msg, &err, &dbg);
    g_error ("pipeline error from %s: %s (%s)", GST_OBJECT_NAME (GST_MESSAGE_SRC (msg)), err->message,
        dbg ? dbg : "");
  }
  gst_message_unref (msg);
  gst_object_unref (bus);
}

/* ---- detections ---- */

typedef struct
{
  gint class_id;
  gfloat confidence;
  gfloat left, top, width, height;
} Det;

/* Per source (pad_index): a GArray of Det, filled by a probe on the sink. */
typedef struct
{
  guint n_sources, frames[8];
  GArray *dets[8];
} Collected;

static GstPadProbeReturn
collect_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  Collected *c = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  for (GList * l = bm->frame_meta_list; l; l = l->next) {
    RdsFrameMeta *fm = l->data;

    g_assert_cmpuint (fm->pad_index, <, c->n_sources);
    g_assert_true (fm->bInferDone);
    c->frames[fm->pad_index]++;
    for (GList * o = fm->obj_meta_list; o; o = o->next) {
      RdsObjectMeta *om = o->data;
      Det d = { om->class_id, om->confidence, om->rect_params.left, om->rect_params.top, om->rect_params.width,
        om->rect_params.height
      };

      g_array_append_val (c->dets[fm->pad_index], d);
    }
  }
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

/* The street image on @n_sources sources (one frame each) -> rdsstreammux (1280x720, batch @n_sources; RGBA
 * batches when @rgba, else the mux's default NV12) -> rdsinfer (YOLO11n, batch @n_sources) -> fakesink. Returns the
 * detections per source; each got exactly one frame. */
static void
run_street (guint n_sources, gboolean rgba, Collected * c)
{
  gchar *cfg = yolo_config (n_sources);
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *sink;
  GstPad *pad;
  GError *err = NULL;

  memset (c, 0, sizeof (*c));
  c->n_sources = n_sources;
  for (guint i = 0; i < n_sources; i++)
    c->dets[i] = g_array_new (FALSE, FALSE, sizeof (Det));
  for (guint i = 0; i < n_sources; i++)
    g_string_append_printf (desc, "filesrc location=" STREET " blocksize=%u ! rawvideoparse format=rgba width=%u "
        "height=%u framerate=30/1 ! m.sink_%u ", STREET_W * STREET_H * 4, STREET_W, STREET_H, i);
  /* RGBA batches keep the photo as it is; NV12 (the mux's default) halves the chroma resolution */
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u ! %s"
      "rdsinfer config-file-path=%s ! fakesink name=k sync=false", STREET_W, STREET_H, n_sources,
      rgba ? "video/x-raw(memory:RDS),format=RGBA ! " : "", cfg);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  sink = gst_bin_get_by_name (GST_BIN (pipe), "k");
  pad = gst_element_get_static_pad (sink, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, collect_probe, c, NULL);
  gst_object_unref (pad);
  gst_object_unref (sink);

  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* a first YOLO11n compile takes about a minute at batch 1 and more at batch 4 on a small GPU */
  wait_eos (pipe, 600 * GST_SECOND);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
  g_free (cfg);
  for (guint i = 0; i < n_sources; i++)
    g_assert_cmpuint (c->frames[i], ==, 1);
}

static void
collected_clear (Collected * c)
{
  for (guint i = 0; i < c->n_sources; i++)
    g_array_unref (c->dets[i]);
}

/* The reference objects of street-ref.json (Ultralytics predict). */
static GArray *
load_reference (void)
{
  JsonParser *parser = json_parser_new ();
  GError *err = NULL;
  JsonObject *root;
  JsonArray *objects;
  GArray *ref = g_array_new (FALSE, FALSE, sizeof (Det));

  g_assert_true (json_parser_load_from_file (parser, STREET_REF, &err));
  g_assert_no_error (err);
  root = json_node_get_object (json_parser_get_root (parser));
  g_assert_cmpint (json_object_get_int_member (root, "width"), ==, STREET_W);
  g_assert_cmpint (json_object_get_int_member (root, "height"), ==, STREET_H);
  objects = json_object_get_array_member (root, "objects");
  for (guint i = 0; i < json_array_get_length (objects); i++) {
    JsonObject *o = json_array_get_object_element (objects, i);
    Det d = {
      .class_id = (gint) json_object_get_int_member (o, "class_id"),
      .confidence = (gfloat) json_object_get_double_member (o, "confidence"),
      .left = (gfloat) json_object_get_double_member (o, "left"),
      .top = (gfloat) json_object_get_double_member (o, "top"),
      .width = (gfloat) json_object_get_double_member (o, "width"),
      .height = (gfloat) json_object_get_double_member (o, "height"),
    };

    g_assert_cmpint (d.class_id, >=, 0);
    g_assert_cmpint (d.class_id, <, N_CLASSES);
    g_array_append_val (ref, d);
  }
  g_object_unref (parser);
  return ref;
}

static gfloat
iou (const Det * a, const Det * b)
{
  gfloat x1 = MAX (a->left, b->left), y1 = MAX (a->top, b->top);
  gfloat x2 = MIN (a->left + a->width, b->left + b->width), y2 = MIN (a->top + a->height, b->top + b->height);
  gfloat inter = MAX (x2 - x1, 0.0f) * MAX (y2 - y1, 0.0f);
  gfloat uni = a->width * a->height + b->width * b->height - inter;

  return uni > 0.0f ? inter / uni : 0.0f;
}

static gint
by_confidence_desc (gconstpointer a, gconstpointer b)
{
  gfloat ca = ((const Det *) a)->confidence, cb = ((const Det *) b)->confidence;

  return ca < cb ? 1 : ca > cb ? -1 : 0;
}

static gchar **
load_labels (void)
{
  gchar *text = NULL, **labels;

  g_assert_true (g_file_get_contents (LABELS, &text, NULL, NULL));
  labels = g_strsplit (text, "\n", -1);
  g_free (text);
  g_assert_cmpuint (g_strv_length (labels), >=, N_CLASSES);
  return labels;
}

#define CLASS_PERSON 0

/* The street photo at batch 1 against Ultralytics' prediction: per class, detections are matched greedily (highest
 * confidence first) to the unmatched reference object of that class with the highest IoU, if it is >= 0.5. Every
 * class with at least 3 reference objects is checked: precision and recall >= @min_pr. At least one class must be
 * checked, person among them. Prints the per-class table. */
static void
check_street (gboolean rgba, gdouble min_pr)
{
  const gchar *need[] = { STREET, STREET_REF, NULL };
  guint n_ref[N_CLASSES] = { 0 }, n_ours[N_CLASSES] = { 0 }, n_match[N_CLASSES] = { 0 }, n_checked = 0;
  gboolean *used;
  gchar **labels;
  Collected c;
  GArray *ref, *ours;
  gint64 t0;
  gboolean ok = TRUE;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_yolo (need))
    return;
  ref = load_reference ();
  g_assert_cmpuint (ref->len, >, 0);
  labels = load_labels ();
  t0 = g_get_monotonic_time ();
  run_street (1, rgba, &c);
  g_test_message ("%s pipeline run (with the program load or build): %.1f s", rgba ? "RGBA" : "NV12",
      (g_get_monotonic_time () - t0) / 1e6);
  ours = c.dets[0];
  g_array_sort (ours, by_confidence_desc);

  used = g_new0 (gboolean, ref->len);
  for (guint i = 0; i < ref->len; i++)
    n_ref[g_array_index (ref, Det, i).class_id]++;
  for (guint i = 0; i < ours->len; i++) {
    const Det *d = &g_array_index (ours, Det, i);
    gint best = -1;
    gfloat best_iou = 0.5f;

    g_assert_cmpint (d->class_id, >=, 0);
    g_assert_cmpint (d->class_id, <, N_CLASSES);
    n_ours[d->class_id]++;
    for (guint j = 0; j < ref->len; j++) {
      const Det *r = &g_array_index (ref, Det, j);
      gfloat v;

      if (used[j] || r->class_id != d->class_id)
        continue;
      v = iou (d, r);
      if (v >= best_iou) {
        best_iou = v;
        best = (gint) j;
      }
    }
    g_test_message ("ours: %-10s %.4f (%7.1f, %6.1f, %6.1f, %6.1f) -> %s", labels[d->class_id], d->confidence,
        d->left, d->top, d->width, d->height, best >= 0 ? "matched" : "unmatched");
    if (best >= 0) {
      used[best] = TRUE;
      n_match[d->class_id]++;
    }
  }
  for (guint j = 0; j < ref->len; j++) {
    const Det *r = &g_array_index (ref, Det, j);

    if (!used[j])
      g_test_message ("reference not found: %s %.4f (%.1f, %.1f, %.1f, %.1f)", labels[r->class_id],
          r->confidence, r->left, r->top, r->width, r->height);
  }

  g_test_message ("%-12s %4s %5s %8s %9s %6s", "class", "ref", "ours", "matched", "precision", "recall");
  for (guint k = 0; k < N_CLASSES; k++) {
    gdouble p, r;

    if (n_ref[k] == 0 && n_ours[k] == 0)
      continue;
    p = n_ours[k] ? (gdouble) n_match[k] / n_ours[k] : 1.0;
    r = n_ref[k] ? (gdouble) n_match[k] / n_ref[k] : 1.0;
    g_test_message ("%-12s %4u %5u %8u %9.2f %6.2f%s", labels[k], n_ref[k], n_ours[k], n_match[k], p, r,
        n_ref[k] >= 3 ? "  (checked)" : "");
    if (n_ref[k] >= 3) {
      n_checked++;
      if (p < min_pr || r < min_pr)
        ok = FALSE;
    }
  }
  g_assert_cmpuint (n_checked, >, 0);
  g_assert_cmpuint (n_ref[CLASS_PERSON], >=, 3);
  g_assert_true (ok);

  g_free (used);
  g_strfreev (labels);
  g_array_unref (ref);
  collected_clear (&c);
}

/* RGBA batches: P and R >= 0.9. Our letterbox pads with 0 where Ultralytics pads with 114 (and fits the image into
 * a 640x384 rectangle), and our resampling differs from OpenCV's, so confidences near the threshold may differ. */
static void
test_detect (void)
{
  check_street (TRUE, 0.9);
}

/* The mux's default NV12 batches (as after a decoder): the RGBA photo loses half its chroma resolution and goes
 * through BT.601 and back, so P and R >= 0.8. That allows one missed person; car, with 3 reference objects, must
 * still be 3/3. */
static void
test_detect_nv12 (void)
{
  check_street (FALSE, 0.8);
}

/* The street image on four sources at batch 4: every slot gets the batch-1 detections. Each batch-4 detection is
 * paired with the unpaired batch-1 detection of its class with the highest IoU; the counts must be equal and each
 * pair within 1e-3 confidence and 0.5 px per box value. */
static void
test_batch4 (void)
{
  const gchar *need[] = { STREET, NULL };
  Collected one, four;
  GArray *want;
  gfloat max_conf = 0.0f, max_px = 0.0f;
  gboolean *used;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_yolo (need))
    return;
  run_street (1, TRUE, &one);
  run_street (4, TRUE, &four);
  want = one.dets[0];
  g_assert_cmpuint (want->len, >, 0);
  used = g_new (gboolean, want->len);
  for (guint s = 0; s < 4; s++) {
    GArray *got = four.dets[s];

    g_assert_cmpuint (got->len, ==, want->len);
    memset (used, 0, want->len * sizeof (gboolean));
    for (guint i = 0; i < got->len; i++) {
      const Det *b = &g_array_index (got, Det, i), *a;
      gint best = -1;
      gfloat best_iou = 0.0f;

      for (guint j = 0; j < want->len; j++) {
        const Det *w = &g_array_index (want, Det, j);
        gfloat v;

        if (used[j] || w->class_id != b->class_id)
          continue;
        v = iou (w, b);
        if (best < 0 || v > best_iou) {
          best_iou = v;
          best = (gint) j;
        }
      }
      if (best < 0)
        g_error ("slot %u: no batch-1 detection of class %d left for (%.1f, %.1f, %.1f, %.1f)", s, b->class_id,
            b->left, b->top, b->width, b->height);
      used[best] = TRUE;
      a = &g_array_index (want, Det, best);
      max_conf = MAX (max_conf, fabsf (a->confidence - b->confidence));
      max_px = MAX (max_px, fabsf (a->left - b->left));
      max_px = MAX (max_px, fabsf (a->top - b->top));
      max_px = MAX (max_px, fabsf (a->width - b->width));
      max_px = MAX (max_px, fabsf (a->height - b->height));
    }
  }
  g_test_message ("%u objects per slot; batch 4 vs batch 1: max |confidence diff| %g, max box diff %g px",
      want->len, max_conf, max_px);
  g_assert_cmpfloat (max_conf, <=, 1e-3f);
  g_assert_cmpfloat (max_px, <=, 0.5f);
  g_free (used);
  collected_clear (&one);
  collected_clear (&four);
}

/* ---- the secondary classifier ---- */

#define SGIE_ID 2
#define CLS_PROB_TOL 0.05f
/* 13 of the 15 crops are in Ultralytics' top 5, against a floor of 12. Deterministic with the pinned reference data
 * and engine: should an engine change lose one more crop, that is a real change in the outputs, not a flake. */
#define CLS_MIN_TOP5 0.8

/* The rds-test2 sample's SGIE config with its model and labels pointed at YOLO11n-cls behind a symlink in YOLO_DIR,
 * so the programs are cached there and not next to the model, and classifier-threshold 0. Written into WORK_DIR;
 * returns its path. */
static gchar *
sgie_config (void)
{
  gchar *link = g_build_filename (YOLO_DIR, "yolo11n-cls.onnx", NULL);
  gchar *path = g_build_filename (WORK_DIR, "sgie.yml", NULL);
  gchar *text = NULL, **lines;
  GString *out = g_string_new (NULL);
  guint replaced = 0, stretch = 0;
  GError *err = NULL;

  g_assert_cmpint (g_mkdir_with_parents (YOLO_DIR, 0755), ==, 0);
  g_assert_cmpint (g_mkdir_with_parents (WORK_DIR, 0755), ==, 0);
  if (!g_file_test (link, G_FILE_TEST_IS_SYMLINK))
    g_assert_cmpint (symlink (YOLO_CLS, link), ==, 0);
  g_assert_true (g_file_get_contents (SGIE_CONFIG, &text, NULL, &err));
  g_assert_no_error (err);
  lines = g_strsplit (text, "\n", -1);
  for (guint i = 0; lines[i]; i++) {
    if (g_str_has_prefix (lines[i], "  onnx-file:")) {
      g_string_append_printf (out, "  onnx-file: %s\n", link);
      replaced++;
    } else if (g_str_has_prefix (lines[i], "  labelfile-path:")) {
      g_string_append (out, "  labelfile-path: " IMAGENET_LABELS "\n");
      replaced++;
    } else if (g_str_has_prefix (lines[i], "  classifier-threshold:")) {
      /* every object gets its top-1, however unsure */
      g_string_append (out, "  classifier-threshold: 0\n");
      replaced++;
    } else if (lines[i + 1] || *lines[i]) {
      g_string_append_printf (out, "%s\n", lines[i]);
      if (strcmp (lines[i], "  maintain-aspect-ratio: 0") == 0)
        stretch++;
    }
  }
  g_assert_cmpuint (replaced, ==, 3);
  /* street-cls-ref.json's stretch_top1 is for this preprocessing */
  g_assert_cmpuint (stretch, ==, 1);
  g_assert_true (g_file_set_contents (path, out->str, (gssize) out->len, &err));
  g_assert_no_error (err);
  g_string_free (out, TRUE);
  g_strfreev (lines);
  g_free (text);
  g_free (link);
  return path;
}

/* A street-ref.json object as the classifier sees it, and what came back. */
typedef struct
{
  Det det;
  JsonObject *ref;              /* its street-cls-ref.json entry; NULL for a non-vehicle */
  gint result_class_id;         /* -1: no classifier meta */
  gfloat result_prob;
  gchar *result_label;
} ClsObject;

/* On rdsinfer's sink pad: the reference objects (GArray of ClsObject) as objects of GIE 1 on the one frame, tracked
 * with their index as object_id. */
static GstPadProbeReturn
add_objects_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GArray *objects = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  RdsFrameMeta *fm;

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  g_assert_cmpuint (g_list_length (bm->frame_meta_list), ==, 1);
  fm = bm->frame_meta_list->data;
  for (guint i = 0; i < objects->len; i++) {
    const Det *d = &g_array_index (objects, ClsObject, i).det;
    RdsObjectMeta *om = rds_acquire_obj_meta_from_pool (bm);

    om->unique_component_id = 1;
    om->class_id = d->class_id;
    om->object_id = i;
    om->confidence = d->confidence;
    om->rect_params.left = d->left;
    om->rect_params.top = d->top;
    om->rect_params.width = d->width;
    om->rect_params.height = d->height;
    rds_add_obj_meta_to_frame (fm, om, NULL);
  }
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

/* On the sink: each object's classifier meta of SGIE_ID (at most one, with one label). */
static GstPadProbeReturn
collect_classes_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  GArray *objects = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));
  RdsFrameMeta *fm;

  g_assert_nonnull (bm);
  rds_acquire_meta_lock (bm);
  fm = bm->frame_meta_list->data;
  g_assert_cmpuint (g_list_length (fm->obj_meta_list), ==, objects->len);
  for (GList * o = fm->obj_meta_list; o; o = o->next) {
    RdsObjectMeta *om = o->data;
    ClsObject *c;

    g_assert_cmpuint (om->object_id, <, objects->len);
    c = &g_array_index (objects, ClsObject, om->object_id);
    for (GList * l = om->classifier_meta_list; l; l = l->next) {
      RdsClassifierMeta *cm = l->data;
      RdsLabelInfo *li;

      if (cm->unique_component_id != SGIE_ID)
        continue;
      g_assert_cmpint (c->result_class_id, ==, -1);
      g_assert_cmpuint (cm->num_labels, ==, 1);
      li = cm->label_info_list->data;
      c->result_class_id = (gint) li->result_class_id;
      c->result_prob = li->result_prob;
      c->result_label = g_strdup (li->result_label);
    }
  }
  rds_release_meta_lock (bm);
  return GST_PAD_PROBE_OK;
}

static gboolean
is_vehicle (gint class_id)
{
  return class_id == 2 || class_id == 3 || class_id == 5 || class_id == 7;
}

static JsonObject *
member_object (JsonObject * o, const gchar * name)
{
  return json_object_get_object_member (o, name);
}

/* The street photo -> rdsstreammux (RGBA, 1280x720) -> rdsinfer with the rds-test2 sample's classifier config
 * (YOLO11n-cls, process-mode 2 on GIE 1's car, motorcycle, bus and truck; classifier-threshold 0 here, so every
 * object gets its top-1) -> fakesink. The objects, all of GIE 1: street-ref.json's non-vehicles, which must get no
 * classifier meta, and street-cls-ref.json's vehicle boxes (each of the photo's 5 vehicles as detected and grown by
 * 1.15 and 1.3 about its centre, 15 crops), compared with the reference made on the same aligned crops:
 * - ONNX Runtime's output on the crop stretched to 224x224 on the CPU (OpenCV bilinear, RGB / 255), the sample
 *   config's preprocessing (maintain-aspect-ratio=0): the correctness check. Every crop has the same top-1 with a
 *   probability within CLS_PROB_TOL, except where ONNX Runtime's top two are within CLS_PROB_TOL of each other
 *   (a near tie that resampling differences may flip) and ours is its second class.
 * - Ultralytics' prediction (its own export through ONNX Runtime): a sanity check. Ultralytics resizes the crop's
 *   shorter side to 224 and takes the centre square, which nvinfer-style preprocessing can't do (it stretches or
 *   letterboxes the whole crop). YOLO11n-cls is unsure on these crops (Ultralytics' top-1 mostly at 0.2 to 0.5), so
 *   the two transforms often pick different, related classes (minivan, beach_wagon, police_van): the top-1 agreement
 *   is only printed, and at least CLS_MIN_TOP5 of our top-1 must be in Ultralytics' top 5.
 * Prints every crop's labels and the rates. */
static void
test_cls_crops (void)
{
  const gchar *need[] = { STREET, STREET_REF, NULL };
  GArray *ref, *objects = g_array_new (FALSE, TRUE, sizeof (ClsObject));
  JsonParser *parser;
  JsonArray *cls_ref;
  GError *err = NULL;
  gchar *cfg, *desc;
  GstElement *pipe, *el;
  GstPad *pad;
  guint n_vehicles = 0, n_agree = 0, n_top5 = 0, n_stretch = 0, n_ties = 0;
  gfloat max_prob_diff = 0.0f;
  gboolean ok = TRUE;

  RDS_TEST_REQUIRE_GPU ();
  if (!have_yolo (need))
    return;
  if (!g_file_test (YOLO_CLS, G_FILE_TEST_IS_REGULAR) || !g_file_test (IMAGENET_LABELS, G_FILE_TEST_IS_REGULAR)
      || !g_file_test (STREET_CLS_REF, G_FILE_TEST_IS_REGULAR)) {
    g_test_skip ("no " YOLO_CLS ", " IMAGENET_LABELS " or " STREET_CLS_REF " (" CLS_FETCH_HINT ")");
    return;
  }
  ref = load_reference ();
  for (guint i = 0; i < ref->len; i++) {
    ClsObject c = { g_array_index (ref, Det, i), NULL, -1, 0.0f, NULL };

    if (!is_vehicle (c.det.class_id))
      g_array_append_val (objects, c);
  }
  parser = json_parser_new ();
  g_assert_true (json_parser_load_from_file (parser, STREET_CLS_REF, &err));
  g_assert_no_error (err);
  cls_ref = json_object_get_array_member (json_node_get_object (json_parser_get_root (parser)), "objects");
  g_assert_cmpuint (json_array_get_length (cls_ref), >, 0);
  for (guint i = 0; i < json_array_get_length (cls_ref); i++) {
    JsonObject *o = json_array_get_object_element (cls_ref, i);
    ClsObject c = { {
            .class_id = (gint) json_object_get_int_member (o, "class_id"),
            .confidence = (gfloat) json_object_get_double_member (o, "confidence"),
            .left = (gfloat) json_object_get_double_member (o, "left"),
            .top = (gfloat) json_object_get_double_member (o, "top"),
            .width = (gfloat) json_object_get_double_member (o, "width"),
            .height = (gfloat) json_object_get_double_member (o, "height"),
        }, o, -1, 0.0f, NULL
    };

    g_assert_true (is_vehicle (c.det.class_id));
    g_array_append_val (objects, c);
  }

  cfg = sgie_config ();
  desc = g_strdup_printf ("filesrc location=" STREET " blocksize=%u ! rawvideoparse format=rgba width=%u height=%u "
      "framerate=30/1 ! m.sink_0 rdsstreammux name=m width=%u height=%u batch-size=1 "
      "! video/x-raw(memory:RDS),format=RGBA ! rdsinfer name=sgie config-file-path=%s ! fakesink name=k sync=false",
      STREET_W * STREET_H * 4, STREET_W, STREET_H, STREET_W, STREET_H, cfg);
  pipe = gst_parse_launch (desc, &err);
  g_assert_no_error (err);
  g_free (desc);
  el = gst_bin_get_by_name (GST_BIN (pipe), "sgie");
  pad = gst_element_get_static_pad (el, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, add_objects_probe, objects, NULL);
  gst_object_unref (pad);
  gst_object_unref (el);
  el = gst_bin_get_by_name (GST_BIN (pipe), "k");
  pad = gst_element_get_static_pad (el, "sink");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, collect_classes_probe, objects, NULL);
  gst_object_unref (pad);
  gst_object_unref (el);
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* a first compile of the batch-16 program */
  wait_eos (pipe, 600 * GST_SECOND);
  gst_element_set_state (pipe, GST_STATE_NULL);
  gst_object_unref (pipe);
  g_free (cfg);

  for (guint i = 0; i < objects->len; i++) {
    const ClsObject *c = &g_array_index (objects, ClsObject, i);
    JsonObject *r = c->ref, *ultra, *stretch;
    gboolean in_top5 = FALSE, tie, match;
    gfloat stretch_prob, second_prob;
    JsonArray *top5;
    const gchar *note;

    if (!r) {
      /* not a vehicle: the sample config's operate-on-class-ids leaves it out */
      g_assert_cmpint (c->result_class_id, ==, -1);
      continue;
    }
    n_vehicles++;
    ultra = member_object (r, "top1");
    stretch = member_object (r, "stretch_top1");
    stretch_prob = (gfloat) json_object_get_double_member (stretch, "prob");
    second_prob = (gfloat) json_object_get_double_member (stretch, "second_prob");
    /* a near tie excuses ONNX Runtime's second class, nothing else */
    tie = stretch_prob - second_prob < CLS_PROB_TOL
        && c->result_class_id == json_object_get_int_member (stretch, "second_class_id");
    top5 = json_object_get_array_member (r, "top5");
    for (guint k = 0; k < json_array_get_length (top5); k++)
      if (c->result_class_id == json_object_get_int_member (json_array_get_object_element (top5, k), "class_id"))
        in_top5 = TRUE;
    if (c->result_class_id == json_object_get_int_member (ultra, "class_id"))
      n_agree++;
    if (in_top5)
      n_top5++;
    match = c->result_class_id == json_object_get_int_member (stretch, "class_id");
    note = "";
    if (match) {
      n_stretch++;
      max_prob_diff = MAX (max_prob_diff, fabsf (c->result_prob - stretch_prob));
      if (fabsf (c->result_prob - stretch_prob) > CLS_PROB_TOL) {
        note = "  FAIL: probability";
        ok = FALSE;
      }
    } else if (tie) {
      n_ties++;
      note = "  (a near tie in ONNX Runtime's top two)";
    } else {
      note = "  FAIL: top-1";
      ok = FALSE;
    }
    g_test_message ("%-10s x%.2f (%6.1f, %5.1f, %5.1f, %5.1f): ours %-14s %.4f | ONNX Runtime stretched %-14s %.4f "
        "(2nd %.4f) | Ultralytics %-14s %.4f%s%s", json_object_get_string_member (r, "label"),
        json_object_get_double_member (r, "scale"), c->det.left, c->det.top, c->det.width, c->det.height,
        c->result_label ? c->result_label : "(none)", c->result_prob,
        json_object_get_string_member (stretch, "label"), stretch_prob, second_prob,
        json_object_get_string_member (ultra, "label"), json_object_get_double_member (ultra, "prob"),
        in_top5 ? "" : " (ours not in its top 5)", note);
  }
  g_test_message ("%u vehicle crops: top-1 as ONNX Runtime's on the stretched crop on %u (%u near ties excused; max "
      "|probability diff| %.4f); as Ultralytics' top-1 on %u (%.0f %%), in Ultralytics' top 5 on %u (%.0f %%)",
      n_vehicles, n_stretch, n_ties, max_prob_diff, n_agree, n_vehicles ? 100.0 * n_agree / n_vehicles : 0.0, n_top5,
      n_vehicles ? 100.0 * n_top5 / n_vehicles : 0.0);
  g_assert_cmpuint (n_vehicles, ==, json_array_get_length (cls_ref));
  g_assert_true (ok);
  g_assert_cmpfloat ((gdouble) n_top5, >=, CLS_MIN_TOP5 * n_vehicles);

  for (guint i = 0; i < objects->len; i++)
    g_free (g_array_index (objects, ClsObject, i).result_label);
  g_array_unref (objects);
  g_array_unref (ref);
  g_object_unref (parser);
}

/* ---- soak ---- */

#define SOAK_SOURCES 4
#define SOAK_W 1280
#define SOAK_H 720
#define SOAK_FRAMES 300
#define SOAK_LOOPS 10

typedef struct
{
  guint batches, frames, objects;
  gsize rss[SOAK_LOOPS], gpu_used[SOAK_LOOPS];  /* after each loop's last batch */
} InferSoakStats;

/* Runs in rdsinfer's source thread: counts frames and objects, and samples memory use when a loop's last batch
 * leaves rdsinfer. */
static GstPadProbeReturn
infer_soak_probe (G_GNUC_UNUSED GstPad * pad, GstPadProbeInfo * info, gpointer user_data)
{
  InferSoakStats *st = user_data;
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (GST_PAD_PROBE_INFO_BUFFER (info));

  g_assert_nonnull (bm);
  st->batches++;
  st->frames += bm->num_frames_in_batch;
  for (GList * l = bm->frame_meta_list; l; l = l->next)
    st->objects += ((RdsFrameMeta *) l->data)->num_obj_meta;
  if (st->batches % SOAK_FRAMES == 0 && st->batches / SOAK_FRAMES <= SOAK_LOOPS) {
    guint loop = st->batches / SOAK_FRAMES - 1;

    st->rss[loop] = host_memory_bytes ();
    st->gpu_used[loop] = gpu_used_bytes ();
  }
  return GST_PAD_PROBE_OK;
}

/* Four 1280x720 H.264 sources of 300 frames, each looped 10 times (3000 batches of 4), through rdsvideodec,
 * rdsstreammux and rdsinfer (YOLO11n, or scale2 at 640x640 when YOLO11n has not been fetched) into a fakesink:
 * VmRSS (under ASan, the live heap) grows by less than 8 MiB and the process's GPU memory moves by less than 16 MiB
 * between the end of loop 2 and the end of loop 10. Reports the fps. */
static void
test_infer_soak (void)
{
  InferSoakStats st = { 0 };
  GString *desc = g_string_new (NULL);
  GstElement *pipe, *infer, *src[SOAK_SOURCES];
  gboolean yolo;
  gchar *cfg;
  GError *err = NULL;
  GstPad *pad;
  GBytes *stream;
  gint64 t0, t1;
  gssize rss_growth, gpu_delta;
  gdouble secs;

  RDS_TEST_REQUIRE_GPU ();
  if (!rds_test_have_element ("vah264enc") || !rds_test_have_element ("h264parse")) {
    g_test_skip ("vah264enc or h264parse not available");
    return;
  }
  yolo = g_file_test (YOLO, G_FILE_TEST_IS_REGULAR) && g_file_test (LABELS, G_FILE_TEST_IS_REGULAR);
  if (!yolo)
    g_test_message ("no " YOLO ": running with scale2 at 640x640");
  cfg = yolo ? yolo_config (SOAK_SOURCES) : scale2_config (SOAK_SOURCES);
  stream = rds_test_encode ("h264", SOAK_W, SOAK_H, SOAK_FRAMES, 2, "ball");
  g_assert_nonnull (stream);

  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_string_append_printf (desc, "appsrc name=s%u caps=video/x-h264,stream-format=byte-stream format=bytes "
        "! h264parse ! rdsvideodec ! m.sink_%u ", i, i);
  g_string_append_printf (desc, "rdsstreammux name=m width=%u height=%u batch-size=%u "
      "! rdsinfer name=infer config-file-path=%s ! fakesink sync=false enable-last-sample=false",
      SOAK_W, SOAK_H, SOAK_SOURCES, cfg);
  pipe = gst_parse_launch (desc->str, &err);
  g_assert_no_error (err);
  g_string_free (desc, TRUE);
  infer = gst_bin_get_by_name (GST_BIN (pipe), "infer");
  pad = gst_element_get_static_pad (infer, "src");
  gst_pad_add_probe (pad, GST_PAD_PROBE_TYPE_BUFFER, infer_soak_probe, &st, NULL);
  gst_object_unref (pad);
  for (guint i = 0; i < SOAK_SOURCES; i++) {
    gchar name[4];

    g_snprintf (name, sizeof (name), "s%u", i);
    src[i] = gst_bin_get_by_name (GST_BIN (pipe), name);
  }

  /* the program is loaded (or built) in the state change, before the clock starts */
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PAUSED), !=, GST_STATE_CHANGE_FAILURE);
  t0 = g_get_monotonic_time ();
  g_assert_cmpint (gst_element_set_state (pipe, GST_STATE_PLAYING), !=, GST_STATE_CHANGE_FAILURE);
  /* each loop is a complete stream (SPS/PPS + IDR first); the buffers share the encoded bytes */
  for (guint l = 0; l < SOAK_LOOPS; l++)
    for (guint i = 0; i < SOAK_SOURCES; i++)
      g_assert_cmpint (gst_app_src_push_buffer (GST_APP_SRC (src[i]), gst_buffer_new_wrapped_bytes (stream)), ==,
          GST_FLOW_OK);
  for (guint i = 0; i < SOAK_SOURCES; i++)
    g_assert_cmpint (gst_app_src_end_of_stream (GST_APP_SRC (src[i])), ==, GST_FLOW_OK);
  wait_eos (pipe, 1500 * GST_SECOND);
  t1 = g_get_monotonic_time ();
  gst_element_set_state (pipe, GST_STATE_NULL);
  for (guint i = 0; i < SOAK_SOURCES; i++)
    gst_object_unref (src[i]);
  gst_object_unref (infer);
  gst_object_unref (pipe);
  g_bytes_unref (stream);
  g_free (cfg);

  secs = (t1 - t0) / 1e6;
  for (guint i = 0; i < SOAK_LOOPS; i++)
    g_test_message ("loop %2u: " RDS_TEST_HOST_MEMORY_NAME " %.1f MiB, GPU used %.1f MiB", i + 1, MIB (st.rss[i]),
        MIB (st.gpu_used[i]));
  rss_growth = (gssize) st.rss[SOAK_LOOPS - 1] - (gssize) st.rss[1];
  gpu_delta = (gssize) st.gpu_used[SOAK_LOOPS - 1] - (gssize) st.gpu_used[1];
  g_test_message ("%s: %u frames in %u batches in %.1f s (%.0f fps, %.1f batches/s), %u objects; "
      RDS_TEST_HOST_MEMORY_NAME " growth loop 2 -> %u: %+.2f MiB; GPU used delta: %+.2f MiB",
      yolo ? "YOLO11n" : "scale2", st.frames, st.batches, secs, st.frames / secs, st.batches / secs, st.objects,
      SOAK_LOOPS, MIB (rss_growth), MIB (gpu_delta));
  g_assert_cmpuint (st.batches, ==, SOAK_FRAMES * SOAK_LOOPS);
  g_assert_cmpuint (st.frames, ==, SOAK_SOURCES * SOAK_FRAMES * SOAK_LOOPS);
  if (yolo)
    g_assert_cmpuint (st.objects, >, 0);
  g_assert_cmpint (rss_growth, <, 8 * 1024 * 1024);
  g_assert_cmpint (ABS (gpu_delta), <, 16 * 1024 * 1024);
}

int
main (int argc, char **argv)
{
  int ret;

  /* MIOpen and comgr caches in the build directory (shared with infer_engine), so re-runs compile nothing */
  g_setenv ("MIOPEN_USER_DB_PATH", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_setenv ("MIOPEN_CUSTOM_CACHE_DIR", RDS_TEST_BUILD_DIR "/miopen", FALSE);
  g_setenv ("AMD_COMGR_CACHE_DIR", RDS_TEST_BUILD_DIR "/comgr", FALSE);
  gst_init (&argc, &argv);
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/infer-yolo/detect", test_detect);
  g_test_add_func ("/gpu/infer-yolo/detect-nv12", test_detect_nv12);
  g_test_add_func ("/gpu/infer-yolo/batch4", test_batch4);
  g_test_add_func ("/gpu/infer-yolo/cls-crops", test_cls_crops);
  g_test_add_func ("/gpu/infer-soak/yolo", test_infer_soak);
  ret = g_test_run ();
  /* gst_deinit stops GStreamer's task-pool threads before exit: their HIP thread-local destructors crash at process
   * exit otherwise */
  gst_deinit ();
  return ret;
}
