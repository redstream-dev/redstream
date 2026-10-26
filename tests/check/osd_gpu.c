/* rdsosd's GPU drawing (rds-osd-gpu.h) against the CPU path: seeded random batches of 1 to 4 slots of different
 * sizes (translucent rects, text, lines at any angle and width, arrows with every head, circles up to huge and off the
 * frame, the clock) drawn over random bytes, byte for byte as rds_osd_shade_commands draws them pass by pass from the
 * host heap; the device mask heap reused across batches, many passes with a tiny heap and pass budget, the mask cache
 * dropped after a failed pass, full coverage with and without the colour tables. Prints the draw time of a 720p frame
 * of labelled boxes and of a 1080p batch of 4.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-gpu.h"

#include <string.h>

#include "rds-test-util.h"

static const RdsOSD_ColorParams WHITE = { 1, 1, 1, 1 };

/* ---- meta ---- */

typedef struct
{
  RdsFrameMeta fm;
  RdsObjectMeta obj[64];
  RdsDisplayMeta dm[2];
  gchar labels[64][24];
  guint n_obj, n_dm;
} Frame;

static void
frame_init (Frame * f)
{
  memset (f, 0, sizeof *f);
}

static RdsObjectMeta *
frame_add_object (Frame * f)
{
  RdsObjectMeta *om = &f->obj[f->n_obj++];

  g_assert_cmpuint (f->n_obj, <=, G_N_ELEMENTS (f->obj));
  f->fm.obj_meta_list = g_list_append (f->fm.obj_meta_list, om);
  return om;
}

static RdsDisplayMeta *
frame_add_display (Frame * f)
{
  RdsDisplayMeta *dm = &f->dm[f->n_dm++];

  g_assert_cmpuint (f->n_dm, <=, G_N_ELEMENTS (f->dm));
  f->fm.display_meta_list = g_list_append (f->fm.display_meta_list, dm);
  return dm;
}

static void
frame_clear (Frame * f)
{
  g_list_free (f->fm.obj_meta_list);
  g_list_free (f->fm.display_meta_list);
}

static gint
rnd (GRand * r, gint lo, gint hi)
{
  return g_rand_int_range (r, lo, hi + 1);
}

/* opaque a third of the time, else translucent */
static RdsOSD_ColorParams
random_color (GRand * r)
{
  return (RdsOSD_ColorParams) {
  g_rand_double (r), g_rand_double (r), g_rand_double (r), rnd (r, 0, 2) == 0 ? 1.0 : g_rand_double_range (r, 0.05,
        0.95)};
}

/* A few colours reused, as real frames do (most full-coverage pixels then blend through a table). */
static RdsOSD_ColorParams
palette_color (GRand * r)
{
  static const RdsOSD_ColorParams palette[] = { {1, 0, 0, 1}, {0, 1, 0, 0.5}, {0, 0, 1, 0.3}, {1, 1, 0, 1} };

  return rnd (r, 0, 1) == 0 ? palette[rnd (r, 0, G_N_ELEMENTS (palette) - 1)] : random_color (r);
}

static RdsOsdDrawOptions
options (void)
{
  return (RdsOsdDrawOptions) {
  .display_bbox = TRUE,.display_text = TRUE,.display_clock = TRUE,.clock_text = "2026-10-26 12:00:00",.clock_color =
        WHITE};
}

static void
set_text (RdsOSD_TextParams * t, const gchar * s, guint x, guint y, guint size, RdsOSD_ColorParams fg, gboolean bg,
    RdsOSD_ColorParams bg_color)
{
  *t = (RdsOSD_TextParams) {.display_text = (gchar *) s,.x_offset = x,.y_offset = y,.font_params = {
  .font_name = (gchar *) "Sans",.font_size = size,.font_color = fg},.set_bg_clr = bg,.text_bg_clr = bg_color};
}

/* Random rects (borders, fills, labels), lines at any angle and width, arrows with every head, circles from tiny to
 * far larger than the frame and off it, on a @w × @h frame. */
static void
random_frame (GRand * r, Frame * f, gint w, gint h)
{
  static const gchar *words[] = { "car", "person 12", "bike", "Wq", "0.93", "truck 7", "ÄÖ" };
  RdsDisplayMeta *dm;

  frame_init (f);
  for (gint i = rnd (r, 0, 24); i > 0; i--) {
    RdsObjectMeta *om = frame_add_object (f);
    RdsOSD_RectParams *rp = &om->rect_params;

    rp->left = rnd (r, -40, w + 10);
    rp->top = rnd (r, -40, h + 10);
    rp->width = rnd (r, 0, w);
    rp->height = rnd (r, 0, h);
    rp->border_width = rnd (r, 0, 8);
    rp->border_color = palette_color (r);
    if (rnd (r, 0, 3) == 0) {
      rp->has_bg_color = 1;
      rp->bg_color = random_color (r);
    }
    if (rnd (r, 0, 2) == 0) {
      g_strlcpy (f->labels[f->n_obj - 1], words[rnd (r, 0, G_N_ELEMENTS (words) - 1)], sizeof f->labels[0]);
      set_text (&om->text_params, f->labels[f->n_obj - 1], rnd (r, 0, w), rnd (r, 0, h), rnd (r, 6, 24),
          palette_color (r), g_rand_boolean (r), random_color (r));
    }
  }
  dm = frame_add_display (f);
  dm->num_lines = rnd (r, 0, 6);
  for (guint i = 0; i < dm->num_lines; i++)
    dm->line_params[i] = (RdsOSD_LineParams) {.x1 = rnd (r, 0, w + 60),.y1 = rnd (r, 0, h + 60),.x2 =
          rnd (r, 0, w + 60),.y2 = rnd (r, 0, h + 60),.line_width = rnd (r, 0, 3) == 0 ? rnd (r, 10, 40) : rnd (r, 1,
          9),.line_color = random_color (r)
    };
  dm->num_arrows = rnd (r, 0, 6);
  for (guint i = 0; i < dm->num_arrows; i++)
    dm->arrow_params[i] = (RdsOSD_ArrowParams) {.x1 = rnd (r, 0, w + 30),.y1 = rnd (r, 0, h + 30),.x2 =
          rnd (r, 0, w + 30),.y2 = rnd (r, 0, h + 30),.arrow_width = rnd (r, 1, 12),.arrow_head =
          (RdsOSD_Arrow_Head_Direction) (i % 3),.arrow_color = random_color (r)
    };
  dm->num_circles = rnd (r, 0, 4);
  for (guint i = 0; i < dm->num_circles; i++) {
    gboolean huge = rnd (r, 0, 4) == 0;

    dm->circle_params[i] = (RdsOSD_CircleParams) {.xc = huge ? rnd (r, 0, 4 * w) : rnd (r, 0, w + 20),.yc =
          huge ? rnd (r, 0, 4 * h) : rnd (r, 0, h + 20),.radius = huge ? rnd (r, w, 6 * w + 6 * h) : rnd (r, 0, 90),
      .circle_width = rnd (r, 0, 9),.circle_color = random_color (r),.has_bg_color = g_rand_boolean (r),.bg_color =
          random_color (r)
    };
  }
  /* an opaque fill of the whole frame now and then: every tile, full coverage */
  if (rnd (r, 0, 5) == 0) {
    dm = frame_add_display (f);
    dm->num_rects = 1;
    dm->rect_params[0] = (RdsOSD_RectParams) {.left = 0,.top = 0,.width = w,.height = h,.has_bg_color = 1,.bg_color =
          {0.2, 0.4, 0.6, 1}
    };
  }
}

/* Labelled boxes as a detector's output: @n objects with a 2 or 3 px border and a label on a background. */
static void
labelled_frame (GRand * r, Frame * f, gint w, gint h, guint n)
{
  static const RdsOSD_ColorParams classes[] = { {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1} };

  frame_init (f);
  for (guint i = 0; i < n; i++) {
    RdsObjectMeta *om = frame_add_object (f);
    guint cls = rnd (r, 0, G_N_ELEMENTS (classes) - 1);
    gint bw = rnd (r, 20, MIN (200, w / 2)), bh = rnd (r, 30, MIN (240, h / 2)), x = rnd (r, 0, w - bw);
    gint y = rnd (r, 20, h - bh);

    om->rect_params = (RdsOSD_RectParams) {.left = x,.top = y,.width = bw,.height = bh,.border_width =
          rnd (r, 2, 3),.border_color = classes[cls]
    };
    g_snprintf (f->labels[i], sizeof f->labels[0], "%s %u", cls == 0 ? "person" : cls == 1 ? "car" : "bicycle",
        rnd (r, 0, 40));
    set_text (&om->text_params, f->labels[i], x, y - 20, 14, WHITE, TRUE, (RdsOSD_ColorParams) {
        0, 0, 0, 0.6});
  }
}

/* ---- images: one on the host for the CPU path, one in device memory, both starting from the same bytes ---- */

typedef struct
{
  gint width, height;
  gsize pitch;                  /* more than 4 × width: the GPU must not touch the padding either */
  guint8 *cpu;
  guint8 *dev;
} Slot;

static void
slot_init (GRand * r, Slot * s, gint w, gint h)
{
  s->width = w;
  s->height = h;
  s->pitch = ((gsize) w * 4 + 16 + 255) & ~(gsize) 255;
  s->cpu = g_malloc (s->pitch * h);
  for (gsize i = 0; i < s->pitch * h; i += 4) {
    guint32 v = g_rand_int (r);

    memcpy (s->cpu + i, &v, 4);
  }
  g_assert_cmpint (hipMalloc ((void **) &s->dev, s->pitch * h), ==, hipSuccess);
  g_assert_cmpint (hipMemcpy (s->dev, s->cpu, s->pitch * h, hipMemcpyHostToDevice), ==, hipSuccess);
}

static void
slot_clear (Slot * s)
{
  g_free (s->cpu);
  g_assert_cmpint (hipFree (s->dev), ==, hipSuccess);
}

static RdsOsdGpuSurface
slot_surface (const Slot * s)
{
  return (RdsOsdGpuSurface) {
  s->dev, s->pitch, s->width, s->height};
}

/* Fails on the first byte where the GPU's plane differs from the CPU's (padding included). */
static void
check_slot (const Slot * s, guint slot, const gchar * what)
{
  guint8 *gpu = g_malloc (s->pitch * s->height);

  g_assert_cmpint (hipMemcpy (gpu, s->dev, s->pitch * s->height, hipMemcpyDeviceToHost), ==, hipSuccess);
  if (memcmp (gpu, s->cpu, s->pitch * s->height) != 0)
    for (gsize i = 0; i < s->pitch * s->height; i++)
      if (gpu[i] != s->cpu[i]) {
        gsize y = i / s->pitch, x = (i % s->pitch) / 4, at = y * s->pitch + 4 * x;

        g_error ("%s: slot %u (%dx%d) pixel %" G_GSIZE_FORMAT ",%" G_GSIZE_FORMAT "%s: cpu %02x %02x %02x %02x, "
            "gpu %02x %02x %02x %02x", what, slot, s->width, s->height, x, y, x >= (gsize) s->width ?
            " (padding)" : "", s->cpu[at], s->cpu[at + 1], s->cpu[at + 2], s->cpu[at + 3], gpu[at], gpu[at + 1],
            gpu[at + 2], gpu[at + 3]);
      }
  g_free (gpu);
}

/* ---- a batch ---- */

typedef struct
{
  guint passes, uploads, new_masks;
} Counts;

/* Plans @frames (frames[k] on slots[k]) and draws every pass on the CPU (from the host heap) and the GPU, in step. */
static void
draw_batch (RdsOsdPlan * plan, RdsOsdGpu * gpu, const RdsOsdDrawOptions * opt, Frame * frames, const guint * slots,
    guint n_frames, Slot * imgs, guint n_slots, Counts * counts)
{
  RdsOsdGpuSurface surfaces[8];
  RdsOsdPass pass;
  GError *err = NULL;

  g_assert_cmpuint (n_slots, <=, G_N_ELEMENTS (surfaces));
  for (guint k = 0; k < n_slots; k++)
    surfaces[k] = slot_surface (&imgs[k]);
  rds_osd_plan_begin (plan);
  for (guint i = 0; i < n_frames; i++)
    rds_osd_plan_add_frame (plan, opt, slots[i], &frames[i].fm, imgs[slots[i]].width, imgs[slots[i]].height);
  while (rds_osd_plan_next_pass (plan, &pass)) {
    for (guint32 i = 0; i < pass.n_frames; i++) {
      const RdsOsdPlanFrame *f = &pass.frames[i];
      Slot *s = &imgs[f->slot];

      rds_osd_shade_commands (pass.cmds + f->first, f->n_cmds, rds_osd_plan_heap (plan), s->cpu, s->width,
          s->height, s->pitch);
    }
    if (counts != NULL) {
      counts->passes++;
      counts->uploads += pass.n_uploads;
      counts->new_masks += pass.n_new_masks;
    }
    if (!rds_osd_gpu_draw_pass (gpu, plan, &pass, surfaces, n_slots, &err))
      g_error ("draw_pass: %s", err->message);
  }
}

static RdsOsdGpu *
new_gpu (void)
{
  GError *err = NULL;
  RdsOsdGpu *gpu = rds_osd_gpu_new (RDS_OSD_MASK_HEAP_SIZE, &err);

  if (gpu == NULL)
    g_error ("rds_osd_gpu_new: %s", err->message);
  return gpu;
}

/* Random batches of 1 to 4 slots of random sizes from 16 × 16 to 1920 × 1080, some frames sharing a slot. */
static void
random_batches (gboolean use_luts, guint n_batches, guint seed)
{
  static const gint sizes[][2] = { {16, 16}, {33, 17}, {100, 75}, {320, 240}, {640, 360}, {1280, 720},
  {1920, 1080}
  };
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdGpu *gpu = new_gpu ();
  RdsOsdDrawOptions opt = options ();
  GRand *r = g_rand_new_with_seed (seed);
  Counts counts = { 0 };

  rds_osd_gpu_set_luts (gpu, use_luts);
  for (guint b = 0; b < n_batches; b++) {
    guint n_slots = rnd (r, 1, 4), n_frames = n_slots + rnd (r, 0, 1), slots[5];
    Slot imgs[4];
    Frame frames[5];
    gchar *what;

    for (guint k = 0; k < n_slots; k++) {
      guint i = rnd (r, 0, G_N_ELEMENTS (sizes));

      if (i == G_N_ELEMENTS (sizes))
        slot_init (r, &imgs[k], rnd (r, 1, 700), rnd (r, 1, 500));
      else
        slot_init (r, &imgs[k], sizes[i][0], sizes[i][1]);
    }
    opt.x_clock_offset = rnd (r, 0, 200);
    opt.y_clock_offset = rnd (r, 0, 100);
    for (guint i = 0; i < n_frames; i++) {
      slots[i] = i < n_slots ? i : (guint) rnd (r, 0, n_slots - 1);
      random_frame (r, &frames[i], imgs[slots[i]].width, imgs[slots[i]].height);
    }
    draw_batch (plan, gpu, &opt, frames, slots, n_frames, imgs, n_slots, &counts);
    what = g_strdup_printf ("seed %u batch %u%s", seed, b, use_luts ? "" : " (no tables)");
    for (guint k = 0; k < n_slots; k++) {
      check_slot (&imgs[k], k, what);
      slot_clear (&imgs[k]);
    }
    g_free (what);
    for (guint i = 0; i < n_frames; i++)
      frame_clear (&frames[i]);
  }
  g_test_message ("%u batches: %u passes, %u masks in %u uploads", n_batches, counts.passes, counts.new_masks,
      counts.uploads);
  g_assert_cmpuint (counts.new_masks, >, 0);
  g_rand_free (r);
  rds_osd_gpu_free (gpu);
  rds_osd_plan_free (plan);
}

static void
test_random (void)
{
  RDS_TEST_REQUIRE_GPU ();
  for (guint seed = 1; seed <= 4; seed++)
    random_batches (TRUE, 12, seed);
}

/* The double-precision blend everywhere (the tables off) draws the same bytes too. */
static void
test_no_luts (void)
{
  RDS_TEST_REQUIRE_GPU ();
  random_batches (FALSE, 12, 5);
}

/* A second identical batch finds every mask in the device heap: nothing is uploaded, the pixels still match. */
static void
test_mask_reuse (void)
{
  RdsOsdPlan *plan;
  RdsOsdGpu *gpu;
  RdsOsdDrawOptions opt = options ();
  GRand *r;
  Frame frames[2];
  const guint slots[2] = { 0, 1 };

  RDS_TEST_REQUIRE_GPU ();
  plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  gpu = new_gpu ();
  r = g_rand_new_with_seed (11);
  labelled_frame (r, &frames[0], 640, 360, 30);
  labelled_frame (r, &frames[1], 320, 240, 12);
  for (guint b = 0; b < 2; b++) {
    Slot imgs[2];
    Counts counts = { 0 };

    slot_init (r, &imgs[0], 640, 360);
    slot_init (r, &imgs[1], 320, 240);
    draw_batch (plan, gpu, &opt, frames, slots, 2, imgs, 2, &counts);
    if (b == 0)
      g_assert_cmpuint (counts.uploads, >, 0);
    else
      g_assert_cmpuint (counts.uploads, ==, 0);
    for (guint k = 0; k < 2; k++) {
      check_slot (&imgs[k], k, b == 0 ? "first batch" : "second batch");
      slot_clear (&imgs[k]);
    }
  }
  frame_clear (&frames[0]);
  frame_clear (&frames[1]);
  g_rand_free (r);
  rds_osd_gpu_free (gpu);
  rds_osd_plan_free (plan);
}

/* The largest mask of a batch: its rows × stride, 16-byte aligned. */
static guint32
largest_mask (RdsOsdPlan * plan, const RdsOsdDrawOptions * opt, Frame * frames, const Slot * imgs)
{
  RdsOsdPass pass;
  guint32 max = 0;

  rds_osd_plan_begin (plan);
  for (guint k = 0; k < 2; k++)
    rds_osd_plan_add_frame (plan, opt, k, &frames[k].fm, imgs[k].width, imgs[k].height);
  while (rds_osd_plan_next_pass (plan, &pass))
    for (guint32 i = 0; i < pass.n_cmds; i++)
      if (pass.cmds[i].type == RDS_OSD_CMD_MASK)
        max = MAX (max, pass.cmds[i].mask.stride * (guint32) (pass.cmds[i].y1 - pass.cmds[i].y0 + 1));
  return (max + RDS_OSD_MASK_ALIGN - 1) & ~(guint32) (RDS_OSD_MASK_ALIGN - 1);
}

/* A heap for three masks and a small pass budget: many passes a batch, evicted masks uploaded again, the same
 * pixels. */
static void
test_passes (void)
{
  RdsOsdDrawOptions opt = options ();
  GRand *r;

  RDS_TEST_REQUIRE_GPU ();
  r = g_rand_new_with_seed (23);
  for (guint b = 0; b < 4; b++) {
    RdsOsdPlan *sizer = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE), *plan;
    RdsOsdGpu *gpu;
    Slot imgs[2];
    Frame frames[2];
    const guint slots[2] = { 0, 1 };
    Counts counts = { 0 };
    gsize heap;
    GError *err = NULL;

    slot_init (r, &imgs[0], rnd (r, 100, 500), rnd (r, 100, 400));
    slot_init (r, &imgs[1], rnd (r, 100, 500), rnd (r, 100, 400));
    random_frame (r, &frames[0], imgs[0].width, imgs[0].height);
    labelled_frame (r, &frames[1], imgs[1].width, imgs[1].height, 16);
    heap = 3 * (gsize) largest_mask (sizer, &opt, frames, imgs);
    rds_osd_plan_free (sizer);
    plan = rds_osd_plan_new (heap);
    rds_osd_plan_set_pass_budget (plan, 200);
    gpu = rds_osd_gpu_new (heap, &err);
    g_assert_no_error (err);
    draw_batch (plan, gpu, &opt, frames, slots, 2, imgs, 2, &counts);
    g_test_message ("batch %u: %u passes, %u masks in %u uploads with a %" G_GSIZE_FORMAT "-byte heap", b,
        counts.passes, counts.new_masks, counts.uploads, heap);
    g_assert_cmpuint (counts.passes, >, 4);
    for (guint k = 0; k < 2; k++) {
      check_slot (&imgs[k], k, "small heap");
      slot_clear (&imgs[k]);
      frame_clear (&frames[k]);
    }
    rds_osd_gpu_free (gpu);
    rds_osd_plan_free (plan);
  }
  g_rand_free (r);
}

/* A pass that fails (here: a slot without a plane) after its masks were placed drops the mask cache: the next batch
 * rasterizes and uploads every mask again and draws right. Without the drop it would find them cached but never
 * uploaded. */
static void
test_failure (void)
{
  RdsOsdPlan *plan, *fresh;
  RdsOsdGpu *gpu, *fresh_gpu;
  RdsOsdDrawOptions opt = options ();
  GRand *r;
  Frame frames[2];
  const guint slots[2] = { 0, 1 };
  Slot imgs[2], fresh_imgs[2];
  RdsOsdPass pass;
  RdsOsdGpuSurface surfaces[2];
  Counts counts = { 0 }, want = { 0 };
  GError *err = NULL;

  RDS_TEST_REQUIRE_GPU ();
  plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  fresh = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  gpu = new_gpu ();
  fresh_gpu = new_gpu ();
  r = g_rand_new_with_seed (31);
  labelled_frame (r, &frames[0], 400, 300, 20);
  labelled_frame (r, &frames[1], 300, 200, 10);
  slot_init (r, &imgs[0], 400, 300);
  slot_init (r, &imgs[1], 300, 200);
  surfaces[0] = slot_surface (&imgs[0]);
  surfaces[1] = slot_surface (&imgs[1]);
  surfaces[1].data = NULL;

  rds_osd_plan_begin (plan);
  for (guint k = 0; k < 2; k++)
    rds_osd_plan_add_frame (plan, &opt, k, &frames[k].fm, imgs[k].width, imgs[k].height);
  g_assert_true (rds_osd_plan_next_pass (plan, &pass));
  g_assert_cmpuint (pass.n_new_masks, >, 0);
  g_assert_false (rds_osd_gpu_draw_pass (gpu, plan, &pass, surfaces, 2, &err));
  g_assert_error (err, RDS_OSD_GPU_ERROR, 0);
  g_test_message ("%s", err->message);
  g_clear_error (&err);
  /* a plane of the wrong size fails the same way */
  surfaces[1] = slot_surface (&imgs[1]);
  surfaces[1].width--;
  g_assert_false (rds_osd_gpu_draw_pass (gpu, plan, &pass, surfaces, 2, &err));
  g_assert_error (err, RDS_OSD_GPU_ERROR, 0);
  g_clear_error (&err);
  /* and so does a missing slot */
  g_assert_false (rds_osd_gpu_draw_pass (gpu, plan, &pass, surfaces, 1, &err));
  g_assert_error (err, RDS_OSD_GPU_ERROR, 0);
  g_clear_error (&err);

  /* the batch is given up; the next one uploads as much as a fresh plan does, and draws the same bytes */
  slot_clear (&imgs[0]);
  slot_clear (&imgs[1]);
  slot_init (r, &imgs[0], 400, 300);
  slot_init (r, &imgs[1], 300, 200);
  memcpy (fresh_imgs, imgs, sizeof imgs);
  for (guint k = 0; k < 2; k++) {
    fresh_imgs[k].cpu = g_memdup2 (imgs[k].cpu, imgs[k].pitch * imgs[k].height);
    g_assert_cmpint (hipMalloc ((void **) &fresh_imgs[k].dev, imgs[k].pitch * imgs[k].height), ==, hipSuccess);
    g_assert_cmpint (hipMemcpy (fresh_imgs[k].dev, imgs[k].cpu, imgs[k].pitch * imgs[k].height,
            hipMemcpyHostToDevice), ==, hipSuccess);
  }
  draw_batch (plan, gpu, &opt, frames, slots, 2, imgs, 2, &counts);
  draw_batch (fresh, fresh_gpu, &opt, frames, slots, 2, fresh_imgs, 2, &want);
  g_assert_cmpuint (counts.new_masks, ==, want.new_masks);
  g_assert_cmpuint (counts.uploads, ==, want.uploads);
  for (guint k = 0; k < 2; k++) {
    check_slot (&imgs[k], k, "after a failure");
    slot_clear (&imgs[k]);
    slot_clear (&fresh_imgs[k]);
    frame_clear (&frames[k]);
  }
  g_rand_free (r);
  rds_osd_gpu_free (gpu);
  rds_osd_gpu_free (fresh_gpu);
  rds_osd_plan_free (plan);
  rds_osd_plan_free (fresh);
}

/* ---- timing ---- */

static int
cmp_double (gconstpointer a, gconstpointer b)
{
  double x = *(const double *) a, y = *(const double *) b;

  return x < y ? -1 : x > y;
}

/* Lines, arrows and circles only: 16 of each across a @w × @h frame. */
static void
shapes_frame (GRand * r, Frame * f, gint w, gint h)
{
  RdsDisplayMeta *dm;

  frame_init (f);
  dm = frame_add_display (f);
  dm->num_lines = dm->num_arrows = dm->num_circles = 16;
  for (guint i = 0; i < 16; i++) {
    dm->line_params[i] = (RdsOSD_LineParams) {.x1 = rnd (r, 0, w),.y1 = rnd (r, 0, h),.x2 = rnd (r, 0, w),.y2 =
          rnd (r, 0, h),.line_width = rnd (r, 2, 8),.line_color = palette_color (r)
    };
    dm->arrow_params[i] = (RdsOSD_ArrowParams) {.x1 = rnd (r, 0, w),.y1 = rnd (r, 0, h),.x2 = rnd (r, 0, w),.y2 =
          rnd (r, 0, h),.arrow_width = rnd (r, 2, 6),.arrow_head = RDS_OSD_ARROW_BOTH_HEAD,.arrow_color =
          palette_color (r)
    };
    dm->circle_params[i] = (RdsOSD_CircleParams) {.xc = rnd (r, 0, w),.yc = rnd (r, 0, h),.radius = rnd (r, 10, 200),
      .circle_width = rnd (r, 2, 6),.circle_color = palette_color (r)
    };
  }
}

/* The median wall time of drawing @n_slots frames on the GPU (and on the CPU path, for comparison) of @n_boxes
 * labelled boxes each, or of lines, arrows and circles when 0, with a warm mask cache (the same batch again, as from
 * frame to frame of a video); planning excluded. */
static void
time_batch (const gchar * what, gint w, gint h, guint n_slots, guint n_boxes, gboolean use_luts)
{
  enum { RUNS = 41 };
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdGpu *gpu = new_gpu ();
  RdsOsdDrawOptions opt = options ();
  GRand *r = g_rand_new_with_seed (5);
  RdsOsdGpuSurface surfaces[4];
  Slot imgs[4];
  Frame frames[4];
  double ms[RUNS], cpu[RUNS], first = 0;
  guint passes = 0;
  GError *err = NULL;

  rds_osd_gpu_set_luts (gpu, use_luts);
  for (guint k = 0; k < n_slots; k++) {
    slot_init (r, &imgs[k], w, h);
    surfaces[k] = slot_surface (&imgs[k]);
    if (n_boxes > 0)
      labelled_frame (r, &frames[k], w, h, n_boxes);
    else
      shapes_frame (r, &frames[k], w, h);
  }
  for (guint run = 0; run <= RUNS; run++) {
    RdsOsdPass pass;
    double t = 0, tc = 0;

    rds_osd_plan_begin (plan);
    for (guint k = 0; k < n_slots; k++)
      rds_osd_plan_add_frame (plan, &opt, k, &frames[k].fm, w, h);
    passes = 0;
    while (rds_osd_plan_next_pass (plan, &pass)) {
      gint64 t0 = g_get_monotonic_time ();

      if (!rds_osd_gpu_draw_pass (gpu, plan, &pass, surfaces, n_slots, &err))
        g_error ("draw_pass: %s", err->message);
      t += (g_get_monotonic_time () - t0) / 1000.0;
      t0 = g_get_monotonic_time ();
      for (guint32 i = 0; i < pass.n_frames; i++)
        rds_osd_shade_commands (pass.cmds + pass.frames[i].first, pass.frames[i].n_cmds, rds_osd_plan_heap (plan),
            imgs[pass.frames[i].slot].cpu, w, h, imgs[pass.frames[i].slot].pitch);
      tc += (g_get_monotonic_time () - t0) / 1000.0;
      passes++;
    }
    if (run == 0) {
      first = t;
    } else {
      ms[run - 1] = t;
      cpu[run - 1] = tc;
    }
  }
  qsort (ms, RUNS, sizeof ms[0], cmp_double);
  qsort (cpu, RUNS, sizeof cpu[0], cmp_double);
  g_test_message ("%s, %s: median %.3f ms (min %.3f, max %.3f) over %u runs, %u pass(es); first run (masks "
      "uploaded) %.3f ms; CPU path median %.3f ms", what, use_luts ? "tables" : "double blend", ms[RUNS / 2], ms[0],
      ms[RUNS - 1], RUNS, passes, first, cpu[RUNS / 2]);
  for (guint k = 0; k < n_slots; k++) {
    slot_clear (&imgs[k]);
    frame_clear (&frames[k]);
  }
  g_rand_free (r);
  rds_osd_gpu_free (gpu);
  rds_osd_plan_free (plan);
}

static void
test_timing (void)
{
  RDS_TEST_REQUIRE_GPU ();
  for (guint luts = 0; luts <= 1; luts++) {
    time_batch ("1 x 1280x720, 60 labelled boxes", 1280, 720, 1, 60, luts);
    time_batch ("4 x 1920x1080, 60 labelled boxes each", 1920, 1080, 4, 60, luts);
  }
  time_batch ("1 x 1920x1080, 16 lines, arrows and circles", 1920, 1080, 1, 0, TRUE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/osd-gpu/random", test_random);
  g_test_add_func ("/gpu/osd-gpu/no-luts", test_no_luts);
  g_test_add_func ("/gpu/osd-gpu/mask-reuse", test_mask_reuse);
  g_test_add_func ("/gpu/osd-gpu/passes", test_passes);
  g_test_add_func ("/gpu/osd-gpu/failure", test_failure);
  g_test_add_func ("/gpu/osd-gpu/timing", test_timing);
  return g_test_run ();
}
