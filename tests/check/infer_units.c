/* rdsinfer's secondary-mode geometry against hand-computed tables: which objects are selected, the aligned and
 * clipped crop rects, the crop letterbox, and objects found in a crop mapped back to the frame.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-infer-units.h"

#include <math.h>
#include <string.h>

/* ---- Selection ---- */

/* A secondary config: operate-on-gie-id @gie_id, the class list @ids (@n_ids of them, normalised as the config
 * does), no size limits. */
static RdsInferConfig *
make_cfg (gint gie_id, const gint * ids, guint n_ids)
{
  RdsInferConfig *cfg = g_new0 (RdsInferConfig, 1);

  cfg->gie_unique_id = 3;
  cfg->process_mode = 2;
  cfg->operate_on_gie_id = gie_id;
  cfg->filter_out_class_ids = g_array_new (FALSE, FALSE, sizeof (guint));
  cfg->operate_on_class_ids = g_array_new (FALSE, FALSE, sizeof (gint));
  g_array_append_vals (cfg->operate_on_class_ids, ids, n_ids);
  rds_infer_config_normalize_class_ids (cfg->operate_on_class_ids);
  return cfg;
}

typedef struct
{
  gint uid, class_id;
  gboolean selected;
} IdCase;

static void
check_ids (const RdsInferConfig * cfg, const IdCase * cases, guint n)
{
  for (guint i = 0; i < n; i++) {
    g_test_message ("gie %d, %u class ids: uid %d class %d", cfg->operate_on_gie_id, cfg->operate_on_class_ids->len,
        cases[i].uid, cases[i].class_id);
    g_assert_cmpint (rds_infer_object_selected (cfg, cases[i].uid, cases[i].class_id, 50, 50), ==,
        cases[i].selected);
  }
}

/* -1 is every producer, our own id (3) and 0 included; otherwise the id must match, and 0 is a real id. */
static void
test_select_gie_id (void)
{
  static const IdCase any[] = { {0, 0, TRUE}, {1, 0, TRUE}, {3, 0, TRUE}, {G_MAXINT, 0, TRUE}, {-1, 0, TRUE} };
  static const IdCase zero[] = { {0, 0, TRUE}, {1, 0, FALSE}, {3, 0, FALSE} };
  static const IdCase own[] = { {3, 0, TRUE}, {0, 0, FALSE}, {1, 0, FALSE} };
  static const IdCase one[] = { {1, 0, TRUE}, {0, 0, FALSE}, {3, 0, FALSE}, {-1, 0, FALSE} };
  RdsInferConfig *cfg;

  cfg = make_cfg (-1, NULL, 0);
  check_ids (cfg, any, G_N_ELEMENTS (any));
  rds_infer_config_free (cfg);
  cfg = make_cfg (0, NULL, 0);
  check_ids (cfg, zero, G_N_ELEMENTS (zero));
  rds_infer_config_free (cfg);
  cfg = make_cfg (3, NULL, 0);
  check_ids (cfg, own, G_N_ELEMENTS (own));
  rds_infer_config_free (cfg);
  cfg = make_cfg (1, NULL, 0);
  check_ids (cfg, one, G_N_ELEMENTS (one));
  rds_infer_config_free (cfg);
}

/* No list: every class, a negative one too. A list: its members only, never a negative class id. A -1 in the list
 * empties it: every class. */
static void
test_select_class_ids (void)
{
  static const gint list[] = { 2, 5 };
  static const gint with_any[] = { 2, -1 };
  static const IdCase all[] = { {1, 0, TRUE}, {1, 7, TRUE}, {1, -1, TRUE}, {1, -5, TRUE} };
  static const IdCase listed[] = { {1, 2, TRUE}, {1, 5, TRUE}, {1, 0, FALSE}, {1, 3, FALSE}, {1, -1, FALSE},
    {1, -2, FALSE}, {1, G_MININT, FALSE}
  };
  static const IdCase gie_and_class[] = { {1, 2, TRUE}, {0, 2, FALSE}, {1, 3, FALSE} };
  RdsInferConfig *cfg;

  cfg = make_cfg (-1, NULL, 0);
  check_ids (cfg, all, G_N_ELEMENTS (all));
  rds_infer_config_free (cfg);
  cfg = make_cfg (-1, list, G_N_ELEMENTS (list));
  check_ids (cfg, listed, G_N_ELEMENTS (listed));
  rds_infer_config_free (cfg);
  cfg = make_cfg (-1, with_any, G_N_ELEMENTS (with_any));
  g_assert_cmpuint (cfg->operate_on_class_ids->len, ==, 0);
  check_ids (cfg, all, G_N_ELEMENTS (all));
  rds_infer_config_free (cfg);
  /* Both rules at once. */
  cfg = make_cfg (1, list, G_N_ELEMENTS (list));
  check_ids (cfg, gie_and_class, G_N_ELEMENTS (gie_and_class));
  rds_infer_config_free (cfg);
}

typedef struct
{
  guint min_w, min_h, max_w, max_h;
  gfloat w, h;
  gboolean selected;
} SizeCase;

/* Inclusive limits on the float size; 0 is no limit. */
static const SizeCase size_cases[] = {
  {0, 0, 0, 0, 0.001f, 0.001f, TRUE},
  {0, 0, 0, 0, 1e9f, 1e9f, TRUE},
  /* Ours: an empty or negative size is never selected. */
  {0, 0, 0, 0, 0, 50, FALSE},
  {0, 0, 0, 0, 50, 0, FALSE},
  {0, 0, 0, 0, -1, 50, FALSE},
  {0, 0, 0, 0, 50, -0.5f, FALSE},
  {16, 16, 0, 0, -20, -20, FALSE},
  {16, 0, 0, 0, 15.5f, 50, FALSE},
  {16, 0, 0, 0, 16.0f, 50, TRUE},
  {16, 0, 0, 0, 16.01f, 50, TRUE},
  {0, 16, 0, 0, 50, 15.5f, FALSE},
  {0, 16, 0, 0, 50, 16.0f, TRUE},
  {0, 0, 16, 0, 16.4f, 50, FALSE},
  {0, 0, 16, 0, 16.0f, 50, TRUE},
  {0, 0, 16, 0, 15.9f, 1e9f, TRUE},
  {0, 0, 0, 16, 50, 16.4f, FALSE},
  {0, 0, 0, 16, 50, 16.0f, TRUE},
  {16, 16, 16, 16, 16.0f, 16.0f, TRUE},
  {16, 16, 16, 16, 16.0f, 15.99f, FALSE},
  {16, 16, 16, 16, 16.01f, 16.0f, FALSE},
  {10, 20, 30, 40, 10, 40, TRUE},
  {10, 20, 30, 40, 30, 20, TRUE},
  {10, 20, 30, 40, 9.99f, 30, FALSE},
  {10, 20, 30, 40, 20, 40.01f, FALSE},
};

static void
test_select_size (void)
{
  RdsInferConfig *cfg = make_cfg (-1, NULL, 0);

  for (guint i = 0; i < G_N_ELEMENTS (size_cases); i++) {
    const SizeCase *c = &size_cases[i];

    g_test_message ("case %u: min %ux%u max %ux%u, size %gx%g", i, c->min_w, c->min_h, c->max_w, c->max_h, c->w,
        c->h);
    cfg->input_object_min_width = c->min_w;
    cfg->input_object_min_height = c->min_h;
    cfg->input_object_max_width = c->max_w;
    cfg->input_object_max_height = c->max_h;
    g_assert_cmpint (rds_infer_object_selected (cfg, 1, 0, c->w, c->h), ==, c->selected);
  }
  /* A size that is not finite is never selected. */
  cfg->input_object_min_width = cfg->input_object_min_height = 0;
  cfg->input_object_max_width = cfg->input_object_max_height = 0;
  g_assert_false (rds_infer_object_selected (cfg, 1, 0, NAN, 50));
  g_assert_false (rds_infer_object_selected (cfg, 1, 0, 50, NAN));
  g_assert_false (rds_infer_object_selected (cfg, 1, 0, INFINITY, 50));
  rds_infer_config_free (cfg);
}

/* ---- Crop rects ---- */

typedef struct
{
  gfloat left, top, w, h;
  guint frame_w, frame_h;
  gboolean ok;
  guint32 out_left, out_top, out_w, out_h;
} CropCase;

static const CropCase crop_cases[] = {
  /* Alignment: origin floor, then up to even; size floor, then down to even, at least 2. */
  {11, 43.7f, 15.6f, 17, 1920, 1080, TRUE, 12, 44, 14, 16},
  {12, 44, 14, 16, 1920, 1080, TRUE, 12, 44, 14, 16},
  {12.9f, 44.99f, 14.99f, 16.99f, 1920, 1080, TRUE, 12, 44, 14, 16},
  {100, 100, 1, 1, 1920, 1080, TRUE, 100, 100, 2, 2},
  {100, 100, 1.99f, 3.99f, 1920, 1080, TRUE, 100, 100, 2, 2},
  {101, 101, 2, 3, 1920, 1080, TRUE, 102, 102, 2, 2},
  {0, 0, 1920, 1080, 1920, 1080, TRUE, 0, 0, 1920, 1080},
  /* Measured on DeepStream 8.0: overflow on the right, the aligned span clipped. */
  {312, 10, 16, 16, 320, 240, TRUE, 312, 10, 8, 16},
  /* A side under 1 px: skipped. */
  {100, 100, 0.99f, 10, 1920, 1080, FALSE, 0, 0, 0, 0},
  {100, 100, 10, 0.5f, 1920, 1080, FALSE, 0, 0, 0, 0},
  {100, 100, 0, 0, 1920, 1080, FALSE, 0, 0, 0, 0},
  {100, 100, -5, 10, 1920, 1080, FALSE, 0, 0, 0, 0},
  /* Right and bottom edges: the aligned span is clipped, the size kept even. */
  {90, 90, 10, 10, 100, 100, TRUE, 90, 90, 10, 10},
  {95, 90, 10, 10, 100, 100, TRUE, 96, 90, 4, 10},
  {11, 21, 100, 100, 40, 30, TRUE, 12, 22, 28, 8},
  {95, 0, 10, 99, 99, 99, TRUE, 96, 0, 2, 98},
  {98.5f, 98.5f, 1.5f, 1.5f, 100, 100, TRUE, 98, 98, 2, 2},
  {99, 50, 3, 10, 100, 100, FALSE, 0, 0, 0, 0},     /* aligned to [100, 102): nothing left */
  {97, 50, 3, 10, 99, 100, FALSE, 0, 0, 0, 0},      /* [98, 100) clipped to [98, 99): 1 px, below 2 */
  /* Negative origins: clipped at 0 after alignment. */
  {-3, -5.5f, 10, 20, 100, 100, TRUE, 0, 0, 8, 14},
  {-0.5f, -1, 10, 11, 100, 100, TRUE, 0, 0, 10, 10},       /* -1 rounds up to 0 */
  {-9, 10, 10, 10, 100, 100, TRUE, 0, 10, 2, 10},  /* [-8, 2) is [0, 2) */
  {-10, 10, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},   /* [-10, 0): nothing left */
  /* Fully outside. */
  {-50, 10, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {150, 10, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {10, 200, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {100, 10, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {1e30f, 10, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {-1e30f, 10, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  /* Larger than the frame. */
  {-10.5f, -20, 300, 500, 100, 80, TRUE, 0, 0, 100, 80},
  {-10.5f, -20, 300, 500, 99, 79, TRUE, 0, 0, 98, 78},
  {0, 0, 1e30f, 1e30f, 1920, 1080, TRUE, 0, 0, 1920, 1080},
  {-1e30f, -1e30f, 1e30f * 3, 1e30f * 3, 1920, 1080, TRUE, 0, 0, 1920, 1080},
  /* A frame under 2 px. */
  {0, 0, 5, 5, 1, 100, FALSE, 0, 0, 0, 0},
  {0, 0, 5, 5, 100, 1, FALSE, 0, 0, 0, 0},
  {0, 0, 5, 5, 0, 100, FALSE, 0, 0, 0, 0},
  /* Not finite. */
  {NAN, 0, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {0, NAN, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {0, 0, NAN, 10, 100, 100, FALSE, 0, 0, 0, 0},
  {0, 0, 10, INFINITY, 100, 100, FALSE, 0, 0, 0, 0},
  {-INFINITY, 0, 10, 10, 100, 100, FALSE, 0, 0, 0, 0},
};

static void
test_crop_rect (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (crop_cases); i++) {
    const CropCase *c = &crop_cases[i];
    RdsBufSurfTransformRect r = { 7, 7, 7, 7 };
    gboolean ok;

    g_test_message ("case %u: (%g, %g) %gx%g in %ux%u", i, c->left, c->top, c->w, c->h, c->frame_w, c->frame_h);
    ok = rds_infer_crop_rect (c->left, c->top, c->w, c->h, c->frame_w, c->frame_h, &r);
    g_assert_cmpint (ok, ==, c->ok);
    if (!ok) {
      /* Untouched. */
      g_assert_cmpuint (r.left, ==, 7);
      g_assert_cmpuint (r.top, ==, 7);
      g_assert_cmpuint (r.width, ==, 7);
      g_assert_cmpuint (r.height, ==, 7);
      continue;
    }
    g_assert_cmpuint (r.left, ==, c->out_left);
    g_assert_cmpuint (r.top, ==, c->out_top);
    g_assert_cmpuint (r.width, ==, c->out_w);
    g_assert_cmpuint (r.height, ==, c->out_h);
    /* Even, at least 2x2, inside the frame. */
    g_assert_cmpuint (r.left % 2, ==, 0);
    g_assert_cmpuint (r.top % 2, ==, 0);
    g_assert_cmpuint (r.width % 2, ==, 0);
    g_assert_cmpuint (r.height % 2, ==, 0);
    g_assert_cmpuint (r.width, >=, 2);
    g_assert_cmpuint (r.height, >=, 2);
    g_assert_cmpuint ((guint64) r.left + r.width, <=, c->frame_w);
    g_assert_cmpuint ((guint64) r.top + r.height, <=, c->frame_h);
  }
}

/* Random boxes around a frame: the invariants hold, and a box inside the frame gets 8.0's alignment unchanged. */
static void
test_crop_rect_random (void)
{
  GRand *rand = g_rand_new_with_seed (1234);
  guint n_ok = 0, n_exact = 0;

  for (guint i = 0; i < 100000; i++) {
    guint fw = g_rand_int_range (rand, 1, 200), fh = g_rand_int_range (rand, 1, 200);
    gfloat left = (gfloat) g_rand_double_range (rand, -100, 300), top = (gfloat) g_rand_double_range (rand, -100, 300);
    gfloat w = (gfloat) g_rand_double_range (rand, 0, 250), h = (gfloat) g_rand_double_range (rand, 0, 250);
    RdsBufSurfTransformRect r;
    gboolean inside;

    if (!rds_infer_crop_rect (left, top, w, h, fw, fh, &r))
      continue;
    n_ok++;
    g_assert_cmpuint (r.left % 2, ==, 0);
    g_assert_cmpuint (r.top % 2, ==, 0);
    g_assert_cmpuint (r.width % 2, ==, 0);
    g_assert_cmpuint (r.height % 2, ==, 0);
    g_assert_cmpuint (r.width, >=, 2);
    g_assert_cmpuint (r.height, >=, 2);
    g_assert_cmpuint (r.left + r.width, <=, fw);
    g_assert_cmpuint (r.top + r.height, <=, fh);
    /* Never wider than the aligned size. */
    g_assert_cmpuint (r.width, <=, MAX (2, (guint) floorf (w) & ~1u));
    g_assert_cmpuint (r.height, <=, MAX (2, (guint) floorf (h) & ~1u));
    inside = left >= 0 && top >= 0;
    if (inside) {
      guint al = (guint) floorf (left), at = (guint) floorf (top);
      guint aw = MAX (2, (guint) floorf (w) & ~1u), ah = MAX (2, (guint) floorf (h) & ~1u);

      al += al & 1;
      at += at & 1;
      if (al + aw <= fw && at + ah <= fh) {
        g_assert_cmpuint (r.left, ==, al);
        g_assert_cmpuint (r.top, ==, at);
        g_assert_cmpuint (r.width, ==, aw);
        g_assert_cmpuint (r.height, ==, ah);
        n_exact++;
      }
    }
  }
  g_test_message ("%u crops, %u of them with an aligned span inside the frame", n_ok, n_exact);
  /* The ranges give both kinds plenty of cases. */
  g_assert_cmpuint (n_ok, >, 10000);
  g_assert_cmpuint (n_exact, >, 200);
  g_rand_free (rand);
}

/* ---- Crop letterbox ---- */

typedef struct
{
  guint net_w, net_h, crop_w, crop_h;
  gboolean keep_aspect, symmetric;
  guint pad_left, pad_top, scaled_w, scaled_h;
} LetterboxCase;

static const LetterboxCase letterbox_cases[] = {
  /* Stretched to the network: no padding, no rounding. */
  {224, 224, 37, 91, FALSE, FALSE, 0, 0, 224, 224},
  {224, 224, 37, 91, FALSE, TRUE, 0, 0, 224, 224},
  {63, 65, 17, 9, FALSE, TRUE, 0, 0, 63, 65},
  /* Width fills: 20 * 224 / 30 = 149.33 -> 148, at the top-left or at floor ((224 - 149.33) / 2) = 37 (the even size
   * would give 38). */
  {224, 224, 30, 20, TRUE, FALSE, 0, 0, 224, 148},
  {224, 224, 30, 20, TRUE, TRUE, 0, 37, 224, 148},
  /* Odd sizes: the filling axis is floored to even too (63 -> 62); 9 * 63 / 17 = 33.35 -> 32; pad
   * floor ((65 - 33.35) / 2) = 15. */
  {63, 65, 17, 9, TRUE, FALSE, 0, 0, 62, 32},
  {63, 65, 17, 9, TRUE, TRUE, 0, 15, 62, 32},
  /* Height fills: 15 * 48 / 21 = 34.29 -> 34; pad floor ((64 - 34.29) / 2) = 14. */
  {64, 48, 15, 21, TRUE, FALSE, 0, 0, 34, 48},
  {64, 48, 15, 21, TRUE, TRUE, 14, 0, 34, 48},
  /* Height fills, odd: 35 * 47 / 22 = 74.77 -> 74; 47 -> 46; pad floor ((101 - 74.77) / 2) = 13. */
  {101, 47, 35, 22, TRUE, TRUE, 13, 0, 74, 46},
  /* The crop's aspect: it fills both axes. */
  {64, 64, 32, 32, TRUE, FALSE, 0, 0, 64, 64},
  {64, 64, 32, 32, TRUE, TRUE, 0, 0, 64, 64},
  {64, 32, 128, 64, TRUE, TRUE, 0, 0, 64, 32},
  /* Upscaled odd crop: 2 * 64 / 34 = 3.76 -> 2; pad floor ((64 - 3.76) / 2) = 30. */
  {64, 64, 34, 2, TRUE, TRUE, 0, 30, 64, 2},
  /* Too thin for 2 pixels: at least 2. 2 * 64 / 1000 = 0.128; pad floor ((64 - 0.128) / 2) = 31. */
  {64, 64, 2, 1000, TRUE, TRUE, 31, 0, 2, 64},
  {64, 64, 1000, 2, TRUE, FALSE, 0, 0, 64, 2},
  {3, 3, 2, 1000, TRUE, TRUE, 1, 0, 2, 2},
  {2, 2, 2, 1000, TRUE, TRUE, 0, 0, 2, 2},
  /* A 1-pixel network axis gets 1. */
  {1, 64, 2, 2, TRUE, TRUE, 0, 31, 1, 2},
  {1, 1, 2, 2, TRUE, TRUE, 0, 0, 1, 1},
};

static void
test_crop_letterbox (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (letterbox_cases); i++) {
    const LetterboxCase *c = &letterbox_cases[i];
    RdsInferLetterbox lb;

    g_test_message ("case %u: net %ux%u crop %ux%u keep %d symmetric %d", i, c->net_w, c->net_h, c->crop_w,
        c->crop_h, c->keep_aspect, c->symmetric);
    memset (&lb, 0xff, sizeof (lb));
    rds_infer_crop_letterbox (c->net_w, c->net_h, c->crop_w, c->crop_h, c->keep_aspect, c->symmetric, &lb);
    g_assert_cmpuint (lb.net_w, ==, c->net_w);
    g_assert_cmpuint (lb.net_h, ==, c->net_h);
    g_assert_cmpuint (lb.frame_w, ==, c->crop_w);
    g_assert_cmpuint (lb.frame_h, ==, c->crop_h);
    g_assert_cmpuint (lb.pad_left, ==, c->pad_left);
    g_assert_cmpuint (lb.pad_top, ==, c->pad_top);
    g_assert_cmpuint (lb.scaled_w, ==, c->scaled_w);
    g_assert_cmpuint (lb.scaled_h, ==, c->scaled_h);
    g_assert_cmpfloat (lb.scale_x, ==, (gfloat) c->scaled_w / c->crop_w);
    g_assert_cmpfloat (lb.scale_y, ==, (gfloat) c->scaled_h / c->crop_h);
    g_assert_cmpuint (lb.pad_left + lb.scaled_w, <=, lb.net_w);
    g_assert_cmpuint (lb.pad_top + lb.scaled_h, <=, lb.net_h);
  }
}

typedef struct
{
  guint net_w, net_h, crop_w, crop_h;
  gboolean keep_aspect, symmetric;
  gfloat net_x, net_y, net_w_box, net_h_box;    /* in the network */
  gfloat x, y, w, h;                            /* in the crop, by hand */
} LetterboxMapCase;

/* Network points and sizes back to the crop with rds_infer_letterbox_map, against hand-computed answers. */
static const LetterboxMapCase letterbox_map_cases[] = {
  /* Stretched: x * 37 / 224, y * 91 / 224. */
  {224, 224, 37, 91, FALSE, FALSE, 112, 112, 56, 32, 18.5f, 45.5f, 9.25f, 13},
  /* pad_top 37, content 224x148: (112 * 30 / 224, (111 - 37) * 20 / 148). */
  {224, 224, 30, 20, TRUE, TRUE, 112, 111, 112, 74, 15, 10, 15, 10},
  /* Top-left, content 224x148: y * 20 / 148. */
  {224, 224, 30, 20, TRUE, FALSE, 112, 74, 112, 74, 15, 10, 15, 10},
  /* pad_top 15, content 62x32: (31 * 17 / 62, (31 - 15) * 9 / 32). */
  {63, 65, 17, 9, TRUE, TRUE, 31, 31, 62, 32, 8.5f, 4.5f, 17, 9},
  /* pad_left 14, content 34x48: ((31 - 14) * 15 / 34, 24 * 21 / 48). */
  {64, 48, 15, 21, TRUE, TRUE, 31, 24, 17, 16, 7.5f, 10.5f, 7.5f, 7},
};

static void
test_crop_letterbox_map (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (letterbox_map_cases); i++) {
    const LetterboxMapCase *c = &letterbox_map_cases[i];
    RdsInferLetterbox lb;
    gfloat m[4];

    g_test_message ("case %u", i);
    rds_infer_crop_letterbox (c->net_w, c->net_h, c->crop_w, c->crop_h, c->keep_aspect, c->symmetric, &lb);
    rds_infer_letterbox_map (&lb, c->net_x, c->net_y, c->net_w_box, c->net_h_box, m);
    g_assert_cmpfloat_with_epsilon (m[0], c->x, 1e-5);
    g_assert_cmpfloat_with_epsilon (m[1], c->y, 1e-5);
    g_assert_cmpfloat_with_epsilon (m[2], c->w, 1e-5);
    g_assert_cmpfloat_with_epsilon (m[3], c->h, 1e-5);
  }
}

/* Large sizes stay exact; random ones keep the content inside the network and even. */
static void
test_crop_letterbox_random (void)
{
  GRand *rand = g_rand_new_with_seed (99);
  RdsInferLetterbox lb;

  rds_infer_crop_letterbox (G_MAXINT, 640, 640, G_MAXINT, TRUE, TRUE, &lb);
  g_assert_cmpuint (lb.scaled_h, ==, 640);
  g_assert_cmpuint (lb.scaled_w, ==, 2);
  g_assert_cmpuint (lb.pad_top, ==, 0);
  g_assert_cmpuint (lb.pad_left, ==, (G_MAXINT - 1) / 2);
  for (guint i = 0; i < 100000; i++) {
    guint nw = g_rand_int_range (rand, 2, 1000), nh = g_rand_int_range (rand, 2, 1000);
    guint cw = g_rand_int_range (rand, 1, 4000), ch = g_rand_int_range (rand, 1, 4000);
    gboolean sym = g_rand_boolean (rand);
    gdouble scale, ew, eh;

    rds_infer_crop_letterbox (nw, nh, cw, ch, FALSE, sym, &lb);
    g_assert_true (lb.scaled_w == nw && lb.scaled_h == nh && lb.pad_left == 0 && lb.pad_top == 0);
    rds_infer_crop_letterbox (nw, nh, cw, ch, TRUE, sym, &lb);
    g_assert_cmpuint (lb.scaled_w % 2, ==, 0);
    g_assert_cmpuint (lb.scaled_h % 2, ==, 0);
    g_assert_cmpuint (lb.scaled_w, >=, 2);
    g_assert_cmpuint (lb.scaled_h, >=, 2);
    g_assert_cmpuint (lb.pad_left + lb.scaled_w, <=, nw);
    g_assert_cmpuint (lb.pad_top + lb.scaled_h, <=, nh);
    /* Against the exact sizes, in double (far from the rounding edges for these sizes). */
    scale = MIN ((gdouble) nw / cw, (gdouble) nh / ch);
    ew = cw * scale;
    eh = ch * scale;
    if (ew >= 2 && fabs (ew - round (ew)) > 1e-6)
      g_assert_cmpuint (lb.scaled_w, ==, (guint) floor (ew) & ~1u);
    if (eh >= 2 && fabs (eh - round (eh)) > 1e-6)
      g_assert_cmpuint (lb.scaled_h, ==, (guint) floor (eh) & ~1u);
    if (sym && fabs ((nw - ew) / 2 - round ((nw - ew) / 2)) > 1e-6)
      g_assert_cmpuint (lb.pad_left, ==, (guint) floor ((nw - ew) / 2));
    if (sym && fabs ((nh - eh) / 2 - round ((nh - eh) / 2)) > 1e-6)
      g_assert_cmpuint (lb.pad_top, ==, (guint) floor ((nh - eh) / 2));
    if (!sym) {
      g_assert_cmpuint (lb.pad_left, ==, 0);
      g_assert_cmpuint (lb.pad_top, ==, 0);
    }
  }
  g_rand_free (rand);
}

/* Bad sizes: a critical, and a zeroed result. */
static void
test_crop_letterbox_invalid (void)
{
  static const guint bad[][4] = {
    {0, 640, 640, 640}, {640, 0, 640, 640}, {640, 640, 0, 640}, {640, 640, 640, 0},
    {(guint) G_MAXINT + 1, 640, 640, 640}, {640, 640, 640, G_MAXUINT},
  };

  for (guint i = 0; i < G_N_ELEMENTS (bad); i++) {
    RdsInferLetterbox lb;

    memset (&lb, 0xff, sizeof (lb));
    g_test_expect_message ("rdsinfer", G_LOG_LEVEL_CRITICAL, "*assertion*failed*");
    rds_infer_crop_letterbox (bad[i][0], bad[i][1], bad[i][2], bad[i][3], TRUE, TRUE, &lb);
    g_test_assert_expected_messages ();
    g_assert_cmpuint (lb.scaled_w, ==, 0);
    g_assert_cmpuint (lb.frame_h, ==, 0);
    g_assert_cmpfloat (lb.scale_x, ==, 0);
  }
}

/* ---- Child mapping ---- */

typedef struct
{
  RdsBufSurfTransformRect crop;
  guint frame_w, frame_h;
  RdsInferBox in;
  gboolean ok;
  RdsInferBox box, rect;
} ChildCase;

/* RdsBufSurfTransformRect's fields are top, left, width, height. */
#define CROP(l, t, w, h) {.left = (l),.top = (t),.width = (w),.height = (h) }

static const ChildCase child_cases[] = {
  /* Inside the crop: moved by the crop's origin. */
  {CROP (100, 50, 64, 32), 1920, 1080, {10, 5, 20, 10}, TRUE, {110, 55, 20, 10}, {110, 55, 20, 10}},
  {CROP (100, 50, 64, 32), 1920, 1080, {0, 0, 64, 32}, TRUE, {100, 50, 64, 32}, {100, 50, 64, 32}},
  {CROP (100, 50, 64, 32), 1920, 1080, {0.25f, 0.5f, 1.5f, 2.75f}, TRUE, {100.25f, 50.5f, 1.5f, 2.75f},
      {100.25f, 50.5f, 1.5f, 2.75f}},
  /* Partly outside the crop, inside the frame: kept whole. */
  {CROP (100, 50, 64, 32), 1920, 1080, {-10, -5, 20, 10}, TRUE, {90, 45, 20, 10}, {90, 45, 20, 10}},
  {CROP (100, 50, 64, 32), 1920, 1080, {60, 30, 20, 10}, TRUE, {160, 80, 20, 10}, {160, 80, 20, 10}},
  /* A crop at the right/bottom edge: the rect is clipped to the frame, the box is not. */
  {CROP (1900, 1060, 20, 20), 1920, 1080, {10, 10, 30, 30}, TRUE, {1910, 1070, 30, 30}, {1910, 1070, 10, 10}},
  /* A crop at the origin: clipped at 0. */
  {CROP (0, 0, 20, 20), 1920, 1080, {-5, -5, 10, 10}, TRUE, {-5, -5, 10, 10}, {0, 0, 5, 5}},
  /* Nothing left in the frame. */
  {CROP (0, 0, 20, 20), 1920, 1080, {-20, -20, 10, 10}, FALSE, {0, 0, 0, 0}, {0, 0, 0, 0}},
  {CROP (0, 0, 20, 20), 1920, 1080, {-10, 5, 10, 10}, FALSE, {0, 0, 0, 0}, {0, 0, 0, 0}},
  {CROP (1900, 1060, 20, 20), 1920, 1080, {20, 0, 10, 10}, FALSE, {0, 0, 0, 0}, {0, 0, 0, 0}},
};

static void
test_map_child (void)
{
  for (guint i = 0; i < G_N_ELEMENTS (child_cases); i++) {
    const ChildCase *c = &child_cases[i];
    RdsInferDetection in = {.class_id = 4,.confidence = 0.75f,.box = c->in,.rect = {1, 2, 3, 4} };
    RdsInferDetection out;
    gboolean ok;

    g_test_message ("case %u", i);
    memset (&out, 0x5a, sizeof (out));
    ok = rds_infer_map_child (&c->crop, c->frame_w, c->frame_h, &in, &out);
    g_assert_cmpint (ok, ==, c->ok);
    if (!ok) {
      RdsInferDetection untouched;

      memset (&untouched, 0x5a, sizeof (untouched));
      g_assert_cmpmem (&out, sizeof (out), &untouched, sizeof (untouched));
      continue;
    }
    g_assert_cmpuint (out.class_id, ==, 4);
    g_assert_cmpfloat (out.confidence, ==, 0.75f);
    g_assert_cmpfloat (out.box.left, ==, c->box.left);
    g_assert_cmpfloat (out.box.top, ==, c->box.top);
    g_assert_cmpfloat (out.box.width, ==, c->box.width);
    g_assert_cmpfloat (out.box.height, ==, c->box.height);
    g_assert_cmpfloat (out.rect.left, ==, c->rect.left);
    g_assert_cmpfloat (out.rect.top, ==, c->rect.top);
    g_assert_cmpfloat (out.rect.width, ==, c->rect.width);
    g_assert_cmpfloat (out.rect.height, ==, c->rect.height);
    /* In place: the same result. */
    g_assert_true (rds_infer_map_child (&c->crop, c->frame_w, c->frame_h, &in, &in));
    g_assert_cmpmem (&in, sizeof (in), &out, sizeof (out));
  }
}

/* The whole chain on one object: its rect_params -> crop -> network box -> rds_infer_postprocess with the crop's
 * letterbox -> frame. */
static void
test_round_trip (void)
{
  RdsInferConfig *cfg = make_cfg (-1, NULL, 0);
  RdsBufSurfTransformRect crop;
  RdsInferLetterbox lb;
  GArray *dets = g_array_new (FALSE, FALSE, sizeof (RdsInferDetection));
  RdsInferObjectDetectionInfo o[2];
  RdsInferDetection child;

  cfg->num_detected_classes = 1;
  cfg->cluster_mode = 4;
  cfg->class_attrs = g_new0 (RdsInferClassAttrs, 1);
  cfg->class_attrs[0].pre_cluster_threshold = 0.25f;
  cfg->class_attrs[0].topk = -1;

  /* (101.3, 49.7) 63.6x33.2 aligns to (102, 50) 62x32. */
  g_assert_true (rds_infer_object_selected (cfg, 1, 0, 63.6f, 33.2f));
  g_assert_true (rds_infer_crop_rect (101.3f, 49.7f, 63.6f, 33.2f, 1920, 1080, &crop));
  g_assert_cmpuint (crop.left, ==, 102);
  g_assert_cmpuint (crop.top, ==, 50);
  g_assert_cmpuint (crop.width, ==, 62);
  g_assert_cmpuint (crop.height, ==, 32);
  /* 64x64 network: the width fills (62 -> 64); 32 * 64 / 62 = 33.03 -> 32, at floor ((64 - 33.03) / 2) = 15. */
  rds_infer_crop_letterbox (64, 64, crop.width, crop.height, TRUE, TRUE, &lb);
  g_assert_cmpuint (lb.scaled_w, ==, 64);
  g_assert_cmpuint (lb.scaled_h, ==, 32);
  g_assert_cmpuint (lb.pad_left, ==, 0);
  g_assert_cmpuint (lb.pad_top, ==, 15);

  /* (16, 23) 32x16 in the network is (15.5, 8) 31x16 in the crop, (117.5, 58) 31x16 in the frame. The second
   * object reaches past the crop's right edge (crop x 46.5 + 31 = 77.5): the child keeps it. */
  o[0] = (RdsInferObjectDetectionInfo) { 0, 16, 23, 32, 16, 0.9f, 0 };
  o[1] = (RdsInferObjectDetectionInfo) { 0, 48, 15, 32, 16, 0.8f, 0 };
  rds_infer_postprocess (cfg, &lb, o, 2, NULL, dets);
  g_assert_cmpuint (dets->len, ==, 2);

  g_assert_true (rds_infer_map_child (&crop, 1920, 1080, &g_array_index (dets, RdsInferDetection, 0), &child));
  g_assert_cmpfloat (child.confidence, ==, 0.9f);
  g_assert_cmpfloat (child.box.left, ==, 117.5f);
  g_assert_cmpfloat (child.box.top, ==, 58);
  g_assert_cmpfloat (child.box.width, ==, 31);
  g_assert_cmpfloat (child.box.height, ==, 16);
  g_assert_cmpfloat (child.rect.left, ==, 117.5f);
  g_assert_cmpfloat (child.rect.top, ==, 58);
  g_assert_cmpfloat (child.rect.width, ==, 31);
  g_assert_cmpfloat (child.rect.height, ==, 16);

  g_assert_true (rds_infer_map_child (&crop, 1920, 1080, &g_array_index (dets, RdsInferDetection, 1), &child));
  g_assert_cmpfloat (child.box.left, ==, 148.5f);
  g_assert_cmpfloat (child.box.top, ==, 50);
  g_assert_cmpfloat (child.box.width, ==, 31);
  g_assert_cmpfloat (child.rect.left, ==, 148.5f);
  g_assert_cmpfloat (child.rect.width, ==, 31);
  g_assert_cmpfloat (child.rect.height, ==, 16);

  /* The same object in a crop at the frame's right edge: clipped to the frame. */
  g_assert_true (rds_infer_crop_rect (1858.4f, 49.7f, 70, 33.2f, 1920, 1080, &crop));
  g_assert_cmpuint (crop.left, ==, 1858);
  g_assert_cmpuint (crop.width, ==, 62);
  g_assert_true (rds_infer_map_child (&crop, 1920, 1080, &g_array_index (dets, RdsInferDetection, 1), &child));
  g_assert_cmpfloat (child.box.left, ==, 1904.5f);
  g_assert_cmpfloat (child.box.width, ==, 31);
  g_assert_cmpfloat (child.rect.left, ==, 1904.5f);
  g_assert_cmpfloat (child.rect.width, ==, 15.5f);

  g_array_unref (dets);
  rds_infer_config_free (cfg);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/cpu/infer-units/select-gie-id", test_select_gie_id);
  g_test_add_func ("/cpu/infer-units/select-class-ids", test_select_class_ids);
  g_test_add_func ("/cpu/infer-units/select-size", test_select_size);
  g_test_add_func ("/cpu/infer-units/crop-rect", test_crop_rect);
  g_test_add_func ("/cpu/infer-units/crop-rect-random", test_crop_rect_random);
  g_test_add_func ("/cpu/infer-units/crop-letterbox", test_crop_letterbox);
  g_test_add_func ("/cpu/infer-units/crop-letterbox-map", test_crop_letterbox_map);
  g_test_add_func ("/cpu/infer-units/crop-letterbox-random", test_crop_letterbox_random);
  g_test_add_func ("/cpu/infer-units/crop-letterbox-invalid", test_crop_letterbox_invalid);
  g_test_add_func ("/cpu/infer-units/map-child", test_map_child);
  g_test_add_func ("/cpu/infer-units/round-trip", test_round_trip);
  return g_test_run ();
}
