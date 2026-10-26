/* rdsosd's draw planner (rds-osd-plan.h) on host buffers: the commands of a frame in draw order, the clamp and skip
 * rules, exact tile binning against brute force (union chains never split), work items only for non-empty tiles, the
 * text-mask cache (hits, misses, least-recently-used eviction) and batches split into passes when their masks do not
 * fit the heap, drawn the same as in one pass. No GStreamer, no GPU.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-plan.h"

#include <math.h>
#include <string.h>

#define GREY 128

static const RdsOSD_ColorParams WHITE = { 1, 1, 1, 1 }, BLUE = { 0, 0, 1, 1 };

/* A colour whose red channel tells the element apart: id 0..255. */
static RdsOSD_ColorParams
col (guint id)
{
  return (RdsOSD_ColorParams) {
  id / 255.0, 0, 0, 1};
}

static guint
cmd_id (const RdsOsdCmd * c)
{
  return (guint) lround (c->color.value[0]);
}

/* ---- meta ---- */

typedef struct
{
  RdsFrameMeta fm;
  RdsObjectMeta obj[64];
  RdsDisplayMeta dm[4];
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

static RdsOSD_RectParams
rect (gfloat l, gfloat t, gfloat w, gfloat h, guint bw, RdsOSD_ColorParams border)
{
  return (RdsOSD_RectParams) {.left = l,.top = t,.width = w,.height = h,.border_width = bw,.border_color = border };
}

static RdsOSD_RectParams
fill (gfloat l, gfloat t, gfloat w, gfloat h, RdsOSD_ColorParams bg)
{
  return (RdsOSD_RectParams) {.left = l,.top = t,.width = w,.height = h,.has_bg_color = 1,.bg_color = bg };
}

static RdsOSD_TextParams
text (const gchar * s, guint x, guint y, guint size, RdsOSD_ColorParams fg, gboolean bg, RdsOSD_ColorParams bg_color)
{
  return (RdsOSD_TextParams) {.display_text = (gchar *) s,.x_offset = x,.y_offset = y,.font_params = {
  .font_name = (gchar *) "Sans",.font_size = size,.font_color = fg},.set_bg_clr = bg,.text_bg_clr = bg_color};
}

static RdsOSD_LineParams
line (guint x1, guint y1, guint x2, guint y2, guint w, RdsOSD_ColorParams c)
{
  return (RdsOSD_LineParams) {.x1 = x1,.y1 = y1,.x2 = x2,.y2 = y2,.line_width = w,.line_color = c };
}

static RdsOsdDrawOptions
options (void)
{
  return (RdsOsdDrawOptions) {.display_bbox = TRUE,.display_text = TRUE,.clock_color = WHITE };
}

/* ---- images ---- */

typedef struct
{
  guint8 *data;
  gint width, height;
  gsize stride;
} Img;

static void
img_init (Img * im, gint w, gint h)
{
  im->width = w;
  im->height = h;
  im->stride = (gsize) w * 4;
  im->data = g_malloc (im->stride * h);
  for (gsize i = 0; i < im->stride * h; i += 4) {
    im->data[i] = im->data[i + 1] = im->data[i + 2] = GREY;
    im->data[i + 3] = 255;
  }
}

static void
img_clear (Img * im)
{
  g_free (im->data);
}

static gboolean
same_pixels (const Img * a, const Img * b)
{
  return a->stride == b->stride && a->height == b->height && memcmp (a->data, b->data, a->stride * a->height) == 0;
}

/* The CPU path: each frame of the pass shaded over its commands' bounding boxes, reading @masks. */
static void
draw_pass (const guint8 * masks, const RdsOsdPass * pass, Img * imgs)
{
  for (guint32 i = 0; i < pass->n_frames; i++) {
    const RdsOsdPlanFrame *f = &pass->frames[i];
    Img *im = &imgs[f->slot];

    g_assert_cmpint (f->width, ==, im->width);
    g_assert_cmpint (f->height, ==, im->height);
    rds_osd_shade_commands (pass->cmds + f->first, f->n_cmds, masks, im->data, im->width, im->height, im->stride);
  }
}

/* The GPU path, per pixel: every pixel of each work item's tile shaded with its list, reading @masks. */
static void
draw_pass_tiles (const guint8 * masks, const RdsOsdPass * pass, Img * imgs)
{
  for (guint32 i = 0; i < pass->n_items; i++) {
    const RdsOsdWorkItem *w = &pass->items[i];
    Img *im = &imgs[w->slot];

    for (gint y = (gint) w->tile_y * RDS_OSD_TILE_SIZE;
        y < MIN ((gint) (w->tile_y + 1) * RDS_OSD_TILE_SIZE, im->height); y++)
      for (gint x = (gint) w->tile_x * RDS_OSD_TILE_SIZE;
          x < MIN ((gint) (w->tile_x + 1) * RDS_OSD_TILE_SIZE, im->width); x++)
        rds_osd_shade_pixel (pass->cmds, pass->lists + w->list_offset, w->count, x, y,
            im->data + (gsize) y * im->stride + 4 * (gsize) x, masks);
  }
}

/* Plans one frame on slot 0 and returns its only pass (asserting there is exactly one). */
static gboolean
plan_one (RdsOsdPlan * plan, const RdsOsdDrawOptions * opt, const Frame * f, gint w, gint h, RdsOsdPass * pass,
    RdsOsdDrawFlags * flags)
{
  RdsOsdPass extra;
  RdsOsdDrawFlags fl;

  rds_osd_plan_begin (plan);
  fl = rds_osd_plan_add_frame (plan, opt, 0, f != NULL ? &f->fm : NULL, w, h);
  if (flags != NULL)
    *flags = fl;
  if (!rds_osd_plan_next_pass (plan, pass))
    return FALSE;
  /* a copy: the arrays stay valid while no further pass is asked for, and there is none */
  g_assert_false (rds_osd_plan_next_pass (plan, &extra));
  return TRUE;
}

/* ---- order ---- */

static void
test_order (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  Frame f;
  RdsObjectMeta *om;
  RdsDisplayMeta *dm;
  /* expected (id, type, union flag) in draw order */
  struct
  {
    guint id;
    RdsOsdCmdType type;
    guint8 flags;
  } want[64];
  guint n = 0;

#define WANT(i, t, fl) do { want[n].id = (i); want[n].type = (t); want[n].flags = (fl); n++; } while (0)

  frame_init (&f);
  om = frame_add_object (&f);
  om->rect_params = fill (10, 10, 40, 30, col (1));
  om->text_params = text ("a", 100, 10, 12, col (3), TRUE, col (2));
  om = frame_add_object (&f);
  om->rect_params = fill (60, 10, 40, 30, col (4));
  om->text_params = text ("b", 150, 10, 12, col (5), FALSE, BLUE);
  dm = frame_add_display (&f);
  dm->num_rects = 1;
  dm->rect_params[0] = fill (10, 50, 40, 30, col (6));
  dm->num_labels = 1;
  dm->text_params[0] = text ("c", 200, 10, 12, col (8), TRUE, col (7));
  /* 20 lines: the first 16 are read (DeepStream 8.0 behaviour) */
  dm->num_lines = 20;
  for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
    dm->line_params[i] = line (10, 100 + 5 * i, 300, 100 + 5 * i, 1, col (10 + i));
  dm->num_arrows = 1;
  dm->arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 320,.y1 = 50,.x2 = 320,.y2 = 250,.arrow_width = 3,
    .arrow_head = RDS_OSD_ARROW_END_HEAD,.arrow_color = col (40) };
  dm->num_circles = 1;
  dm->circle_params[0] = (RdsOSD_CircleParams) {.xc = 360,.yc = 60,.radius = 20,.circle_color = col (41),
    .circle_width = 2,.has_bg_color = 1,.bg_color = col (42) };
  dm = frame_add_display (&f);
  dm->num_rects = 1;
  dm->rect_params[0] = fill (60, 50, 40, 30, col (50));
  dm->num_lines = 1;
  dm->line_params[0] = line (10, 200, 300, 200, 1, col (51));
  opt.display_clock = TRUE;
  opt.clock_text = "2026-10-26 12:00:00";
  opt.clock_color = col (60);
  opt.y_clock_offset = 260;

  /* DeepStream 8.0 behaviour: rects (objects, then display metas), text (objects, then labels), lines, arrows,
   * circles, the clock; list order within a kind */
  WANT (1, RDS_OSD_CMD_BOX, 0);
  WANT (4, RDS_OSD_CMD_BOX, 0);
  WANT (6, RDS_OSD_CMD_BOX, 0);
  WANT (50, RDS_OSD_CMD_BOX, 0);
  WANT (2, RDS_OSD_CMD_BOX, 0);
  WANT (3, RDS_OSD_CMD_MASK, 0);
  WANT (5, RDS_OSD_CMD_MASK, 0);
  WANT (7, RDS_OSD_CMD_BOX, 0);
  WANT (8, RDS_OSD_CMD_MASK, 0);
  for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
    WANT (10 + i, RDS_OSD_CMD_SEGMENT, 0);
  WANT (51, RDS_OSD_CMD_SEGMENT, 0);
  WANT (40, RDS_OSD_CMD_SEGMENT, 0);
  WANT (40, RDS_OSD_CMD_SEGMENT, RDS_OSD_CMD_UNION_NEXT);
  WANT (40, RDS_OSD_CMD_SEGMENT, RDS_OSD_CMD_UNION_NEXT);
  WANT (40, RDS_OSD_CMD_TRIANGLE, 0);
  WANT (42, RDS_OSD_CMD_RING, 0);
  WANT (41, RDS_OSD_CMD_RING, 0);
  WANT (60, RDS_OSD_CMD_MASK, 0);
#undef WANT

  g_assert_true (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  g_assert_cmpuint (pass.n_cmds, ==, n);
  for (guint i = 0; i < n; i++) {
    if (cmd_id (&pass.cmds[i]) != want[i].id || pass.cmds[i].type != want[i].type
        || (pass.cmds[i].flags & RDS_OSD_CMD_UNION_NEXT) != want[i].flags)
      g_error ("command %u: id %u type %u flags %u, expected id %u type %u flags %u", i, cmd_id (&pass.cmds[i]),
          pass.cmds[i].type, pass.cmds[i].flags & RDS_OSD_CMD_UNION_NEXT, want[i].id, want[i].type, want[i].flags);
  }
  g_assert_cmpuint (pass.n_frames, ==, 1);
  g_assert_cmpuint (pass.frames[0].slot, ==, 0);
  g_assert_cmpuint (pass.frames[0].first, ==, 0);
  g_assert_cmpuint (pass.frames[0].n_cmds, ==, n);
  /* four new masks: "a", "b", "c" and the clock, back to back in a new heap: one upload */
  g_assert_cmpuint (pass.n_new_masks, ==, 4);
  g_assert_cmpuint (pass.n_uploads, ==, 1);
  g_assert_cmpuint (pass.uploads[0].offset, ==, 0);
  g_assert_cmpuint (pass.uploads[0].size, ==, pass.cmds[n - 1].mask.offset + pass.cmds[n - 1].mask.stride *
      (guint32) (pass.cmds[n - 1].y1 - pass.cmds[n - 1].y0 + 1));

  /* a frame with nothing to draw has no pass at all */
  frame_clear (&f);
  frame_init (&f);
  frame_add_object (&f);
  opt = options ();
  g_assert_false (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  frame_clear (&f);
  rds_osd_plan_free (plan);
}

/* ---- clamps and skips ---- */

static gboolean
same_cmds (const RdsOsdPass * a, const RdsOsdCmd * b, guint32 n)
{
  return a->n_cmds == n && memcmp (a->cmds, b, n * sizeof *b) == 0;
}

/* The commands of one frame, copied. */
static RdsOsdCmd *
cmds_of (RdsOsdPlan * plan, const Frame * f, gint w, gint h, guint32 * n, RdsOsdDrawFlags * flags)
{
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;

  if (!plan_one (plan, &opt, f, w, h, &pass, flags)) {
    *n = 0;
    return NULL;
  }
  *n = pass.n_cmds;
  return g_memdup2 (pass.cmds, pass.n_cmds * sizeof *pass.cmds);
}

static void
test_clamps (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  RdsOsdDrawFlags flags;
  RdsDisplayMeta *dm;
  RdsOsdCmd *ref;
  guint32 n;
  Frame f;

  /* border_width above 32: drawn 32 wide, flagged (ours) */
  frame_init (&f);
  frame_add_object (&f)->rect_params = rect (20, 20, 100, 80, RDS_OSD_MAX_BORDER_WIDTH, BLUE);
  ref = cmds_of (plan, &f, 400, 300, &n, &flags);
  g_assert_cmpuint (n, ==, 4);
  g_assert_cmpint (flags, ==, 0);
  f.obj[0].rect_params.border_width = 40;
  g_assert_true (plan_one (plan, &opt, &f, 400, 300, &pass, &flags));
  g_assert_cmpint (flags, ==, RDS_OSD_DRAW_BORDER_CLAMPED);
  g_assert_true (same_cmds (&pass, ref, n));
  g_free (ref);
  frame_clear (&f);

  /* a rect with a NaN field: nothing (ours) */
  frame_init (&f);
  frame_add_object (&f)->rect_params = fill (NAN, 20, 100, 80, BLUE);
  frame_add_object (&f)->rect_params = fill (20, 20, 100, NAN, BLUE);
  g_assert_false (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  frame_clear (&f);

  /* line_width above 2 · max (W, H) + 1 is that width (ours) */
  frame_init (&f);
  dm = frame_add_display (&f);
  dm->num_lines = 1;
  dm->line_params[0] = line (10, 40, 90, 40, 2 * 100 + 1, BLUE);
  ref = cmds_of (plan, &f, 100, 80, &n, NULL);
  g_assert_cmpuint (n, ==, 1);
  dm->line_params[0].line_width = G_MAXUINT;
  g_assert_true (plan_one (plan, &opt, &f, 100, 80, &pass, NULL));
  g_assert_true (same_cmds (&pass, ref, n));
  g_free (ref);
  frame_clear (&f);

  /* an arrow with an end beyond 2^21: the shaft only, no head (ours) */
  frame_init (&f);
  dm = frame_add_display (&f);
  dm->num_arrows = 1;
  dm->arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 10,.y1 = 10,.x2 = RDS_OSD_MAX_COORD + 5,.y2 = 10,
    .arrow_width = 3,.arrow_head = RDS_OSD_ARROW_BOTH_HEAD,.arrow_color = BLUE };
  g_assert_true (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  g_assert_cmpuint (pass.n_cmds, ==, 1);
  g_assert_cmpuint (pass.cmds[0].type, ==, RDS_OSD_CMD_SEGMENT);
  frame_clear (&f);

  /* lines, arrows and circles are not drawn on frames above 2^20 a side (ours); rects are */
  frame_init (&f);
  frame_add_object (&f)->rect_params = fill (0, -1, 50, 2, BLUE);
  dm = frame_add_display (&f);
  dm->num_lines = 1;
  dm->line_params[0] = line (0, 0, 50, 0, 1, BLUE);
  dm->num_arrows = 1;
  dm->arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 0,.y1 = 0,.x2 = 50,.y2 = 0,.arrow_width = 1,
    .arrow_head = RDS_OSD_ARROW_END_HEAD,.arrow_color = BLUE };
  dm->num_circles = 1;
  dm->circle_params[0] = (RdsOSD_CircleParams) {.xc = 20,.yc = 0,.radius = 5,.circle_color = BLUE,
    .circle_width = 1 };
  g_assert_true (plan_one (plan, &opt, &f, RDS_OSD_MAX_SHAPE_FRAME + 1, 1, &pass, NULL));
  g_assert_cmpuint (pass.n_cmds, ==, 1);
  g_assert_cmpuint (pass.cmds[0].type, ==, RDS_OSD_CMD_BOX);
  g_assert_true (plan_one (plan, &opt, &f, RDS_OSD_MAX_SHAPE_FRAME, 1, &pass, NULL));
  g_assert_cmpuint (pass.n_cmds, >, 3);
  frame_clear (&f);

  /* font sizes above 1024 are drawn at 1024 (ours): the same commands and mask bytes */
  {
    guint8 *mask;
    gsize mask_len;

    frame_init (&f);
    frame_add_object (&f)->text_params = text ("Big", 0, 0, 1024, WHITE, TRUE, BLUE);
    g_assert_true (plan_one (plan, &opt, &f, 900, 700, &pass, NULL));
    g_assert_cmpuint (pass.n_cmds, ==, 2);
    g_assert_cmpuint (pass.cmds[1].type, ==, RDS_OSD_CMD_MASK);
    g_assert_cmpuint (pass.n_uploads, ==, 1);
    mask_len = pass.uploads[0].size;
    mask = g_memdup2 (rds_osd_plan_heap (plan) + pass.uploads[0].offset, mask_len);
    ref = g_memdup2 (pass.cmds, 2 * sizeof *pass.cmds);
    f.obj[0].text_params.font_params.font_size = 5000;
    g_assert_true (plan_one (plan, &opt, &f, 900, 700, &pass, NULL));
    g_assert_cmpuint (pass.n_cmds, ==, 2);
    g_assert_cmpuint (memcmp (&pass.cmds[0], &ref[0], sizeof ref[0]), ==, 0);
    g_assert_cmpuint (pass.cmds[1].mask.stride, ==, ref[1].mask.stride);
    g_assert_cmpuint (pass.cmds[1].x1, ==, ref[1].x1);
    g_assert_cmpuint (pass.cmds[1].y1, ==, ref[1].y1);
    g_assert_cmpuint (memcmp (rds_osd_plan_heap (plan) + pass.cmds[1].mask.offset, mask, mask_len), ==, 0);
    g_free (mask);
    g_free (ref);
    frame_clear (&f);
  }

  /* NULL and empty text, font_size 0 and a box past the frame: nothing, not even the box (DeepStream 8.0
   * behaviour) */
  frame_init (&f);
  frame_add_object (&f)->text_params = text (NULL, 10, 10, 20, WHITE, TRUE, BLUE);
  frame_add_object (&f)->text_params = text ("", 10, 10, 20, WHITE, TRUE, BLUE);
  frame_add_object (&f)->text_params = text ("x", 10, 10, 0, WHITE, TRUE, BLUE);
  frame_add_object (&f)->text_params = text ("x", 399, 10, 20, WHITE, TRUE, BLUE);
  g_assert_false (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  frame_clear (&f);
  rds_osd_plan_free (plan);

  /* a text whose mask is larger than the heap: its box is drawn, its glyphs are not (ours) */
  plan = rds_osd_plan_new (1024);
  frame_init (&f);
  frame_add_object (&f)->text_params = text ("Hello", 10, 10, 20, WHITE, TRUE, BLUE);
  frame_add_object (&f)->text_params = text ("i", 200, 10, 8, WHITE, FALSE, BLUE);
  g_assert_true (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  g_assert_cmpuint (pass.n_cmds, ==, 2);
  g_assert_cmpuint (pass.cmds[0].type, ==, RDS_OSD_CMD_BOX);
  g_assert_cmpuint (pass.cmds[1].type, ==, RDS_OSD_CMD_MASK);
  g_assert_cmpint (pass.cmds[1].x0, >=, 200);
  frame_clear (&f);
  rds_osd_plan_free (plan);
}

/* ---- binning ---- */

static RdsOSD_ColorParams
random_color (GRand * r)
{
  return (RdsOSD_ColorParams) {
  g_rand_double (r), g_rand_double (r), g_rand_double (r), g_rand_boolean (r) ? 1.0 : g_rand_double_range (r, 0.2,
        0.9)};
}

static gint
rnd (GRand * r, gint lo, gint hi)
{
  return g_rand_int_range (r, lo, hi + 1);
}

/* Random object rects (some text), display rects, lines, arrows with heads (union chains) and circles. */
static void
random_frame (GRand * r, Frame * f, gint w, gint h)
{
  RdsDisplayMeta *dm;
  static const gchar *words[] = { "car", "person 12", "bike", "Wq", "0.93" };

  frame_init (f);
  for (gint i = rnd (r, 0, 12); i > 0; i--) {
    RdsObjectMeta *om = frame_add_object (f);

    om->rect_params = rect (rnd (r, -40, w + 10), rnd (r, -40, h + 10), rnd (r, 0, w), rnd (r, 0, h),
        rnd (r, 0, 6), random_color (r));
    if (g_rand_int_range (r, 0, 3) == 0) {
      om->rect_params.has_bg_color = 1;
      om->rect_params.bg_color = random_color (r);
    }
    if (g_rand_int_range (r, 0, 4) == 0)
      om->text_params = text (words[rnd (r, 0, G_N_ELEMENTS (words) - 1)], rnd (r, 0, w), rnd (r, 0, h),
          rnd (r, 6, 16), random_color (r), g_rand_boolean (r), random_color (r));
  }
  dm = frame_add_display (f);
  dm->num_lines = rnd (r, 0, 4);
  for (guint i = 0; i < dm->num_lines; i++)
    dm->line_params[i] = line (rnd (r, 0, w + 30), rnd (r, 0, h + 30), rnd (r, 0, w + 30), rnd (r, 0, h + 30),
        rnd (r, 1, 9), random_color (r));
  dm->num_arrows = rnd (r, 0, 4);
  for (guint i = 0; i < dm->num_arrows; i++)
    dm->arrow_params[i] = (RdsOSD_ArrowParams) {.x1 = rnd (r, 0, w + 30),.y1 = rnd (r, 0, h + 30),.x2 =
          rnd (r, 0, w + 30),.y2 = rnd (r, 0, h + 30),.arrow_width = rnd (r, 1, 7),.arrow_head =
          RDS_OSD_ARROW_BOTH_HEAD,.arrow_color = random_color (r)
    };
  dm->num_circles = rnd (r, 0, 3);
  for (guint i = 0; i < dm->num_circles; i++)
    dm->circle_params[i] = (RdsOSD_CircleParams) {.xc = rnd (r, 0, w + 20),.yc = rnd (r, 0, h + 20),.radius =
          rnd (r, 0, 80),.circle_width = rnd (r, 0, 6),.circle_color = random_color (r),.has_bg_color =
          g_rand_boolean (r),.bg_color = random_color (r)
    };
}

/* The bounding box of the shape cmds[i] belongs to, and its first and last command. */
static void
shape_box (const RdsOsdCmd * cmds, guint32 n, guint32 i, gint * x0, gint * y0, gint * x1, gint * y1)
{
  guint32 s = i, e = i;

  while (s > 0 && (cmds[s - 1].flags & RDS_OSD_CMD_UNION_NEXT))
    s--;
  while (e + 1 < n && (cmds[e].flags & RDS_OSD_CMD_UNION_NEXT))
    e++;
  *x0 = cmds[s].x0;
  *y0 = cmds[s].y0;
  *x1 = cmds[s].x1;
  *y1 = cmds[s].y1;
  for (guint32 k = s + 1; k <= e; k++) {
    *x0 = MIN (*x0, cmds[k].x0);
    *y0 = MIN (*y0, cmds[k].y0);
    *x1 = MAX (*x1, cmds[k].x1);
    *y1 = MAX (*y1, cmds[k].y1);
  }
}

/* The first and last command of the shape cmds[i] belongs to. */
static void
shape_range (const RdsOsdCmd * cmds, guint32 n, guint32 i, guint32 * s, guint32 * e)
{
  for (*s = i; *s > 0 && (cmds[*s - 1].flags & RDS_OSD_CMD_UNION_NEXT); (*s)--);
  for (*e = i; *e + 1 < n && (cmds[*e].flags & RDS_OSD_CMD_UNION_NEXT); (*e)++);
}

/* Whether the shape cmds[s .. e] covers a pixel of the @w × @h frame in [tx0, tx1] × [ty0, ty1] (a mask: whether its
 * box touches them; the heap is not at hand). */
static gboolean
shape_covers (const RdsOsdCmd * cmds, guint32 s, guint32 e, gint w, gint h, gint tx0, gint ty0, gint tx1, gint ty1)
{
  for (guint32 c = s; c <= e; c++) {
    gint x0 = MAX (tx0, cmds[c].x0), x1 = MIN (MIN (tx1, cmds[c].x1), w - 1);
    gint y0 = MAX (ty0, cmds[c].y0), y1 = MIN (MIN (ty1, cmds[c].y1), h - 1);

    if (x0 > x1 || y0 > y1)
      continue;
    if (cmds[c].type == RDS_OSD_CMD_MASK)
      return TRUE;
    for (gint y = y0; y <= y1; y++)
      for (gint x = x0; x <= x1; x++)
        if (rds_osd_cmd_coverage (&cmds[c], x, y, NULL) != 0)
          return TRUE;
  }
  return FALSE;
}

/* Checks @pass's work items against brute force: for each slot and tile, in order, the shapes of the slot's frames
 * that cover a pixel of the tile, and only shapes whose box touches it, each whole; an item only for a tile with a
 * shape; items in slot, then row order. Returns the number of shapes binned to tiles they don't cover. */
static guint
check_binning (const RdsOsdPass * pass, const Img * imgs, guint n_slots)
{
  guint32 item = 0, list_end = 0;
  gboolean seen[8] = { FALSE };
  guint order[8], n_order = 0, extra = 0;
  GArray *boxed = g_array_new (FALSE, FALSE, sizeof (guint32)), *covered = g_array_new (FALSE, FALSE,
      sizeof (gboolean));

  g_assert_cmpuint (n_slots, <=, G_N_ELEMENTS (seen));
  for (guint32 i = 0; i < pass->n_frames; i++)
    if (!seen[pass->frames[i].slot]) {
      seen[pass->frames[i].slot] = TRUE;
      order[n_order++] = pass->frames[i].slot;
    }

  for (guint k = 0; k < n_order; k++) {
    guint slot = order[k];
    gint w = imgs[slot].width, h = imgs[slot].height;
    gint tiles_x = (w + RDS_OSD_TILE_SIZE - 1) / RDS_OSD_TILE_SIZE;
    gint tiles_y = (h + RDS_OSD_TILE_SIZE - 1) / RDS_OSD_TILE_SIZE;

    for (gint ty = 0; ty < tiles_y; ty++)
      for (gint tx = 0; tx < tiles_x; tx++) {
        gint tx0 = tx * RDS_OSD_TILE_SIZE, ty0 = ty * RDS_OSD_TILE_SIZE;
        gint tx1 = tx0 + RDS_OSD_TILE_SIZE - 1, ty1 = ty0 + RDS_OSD_TILE_SIZE - 1;
        gboolean any = FALSE;
        guint32 at = 0, b = 0;
        const RdsOsdWorkItem *wi;

        /* the commands whose shape's box touches the tile, in order, and whether that shape covers a pixel of it */
        g_array_set_size (boxed, 0);
        g_array_set_size (covered, 0);
        for (guint32 fi = 0; fi < pass->n_frames; fi++) {
          const RdsOsdPlanFrame *fr = &pass->frames[fi];

          if (fr->slot != slot)
            continue;
          for (guint32 c = fr->first; c < fr->first + fr->n_cmds; c++) {
            gint x0, y0, x1, y1;
            guint32 s, e;
            gboolean cov;

            shape_box (pass->cmds, pass->n_cmds, c, &x0, &y0, &x1, &y1);
            if (!(x0 <= tx1 && x1 >= tx0 && y0 <= ty1 && y1 >= ty0))
              continue;
            shape_range (pass->cmds, pass->n_cmds, c, &s, &e);
            cov = shape_covers (pass->cmds, s, e, w, h, tx0, ty0, tx1, ty1);
            any |= cov;
            g_array_append_val (boxed, c);
            g_array_append_val (covered, cov);
          }
        }
        wi = item < pass->n_items ? &pass->items[item] : NULL;
        if (wi == NULL || wi->slot != slot || wi->tile_x != tx || wi->tile_y != ty) {
          if (any)
            g_error ("slot %u tile %d,%d: no work item", slot, tx, ty);
          continue;
        }
        g_assert_cmpuint (wi->count, >, 0);
        g_assert_cmpuint (wi->list_offset + wi->count, <=, pass->n_lists);
        /* the list: a subsequence of the boxed commands, whole shapes, every covering one */
        while (at < wi->count) {
          guint32 c = pass->lists[wi->list_offset + at], s, e;

          while (b < boxed->len && g_array_index (boxed, guint32, b) != c) {
            if (g_array_index (covered, gboolean, b))
              g_error ("slot %u tile %d,%d: command %u covers the tile but is not listed", slot, tx, ty,
                  g_array_index (boxed, guint32, b));
            b++;
          }
          if (b == boxed->len)
            g_error ("slot %u tile %d,%d: command %u listed out of order or off its box", slot, tx, ty, c);
          shape_range (pass->cmds, pass->n_cmds, c, &s, &e);
          g_assert_cmpuint (c, ==, s);
          g_assert_cmpuint (at + (e - s), <, wi->count);
          for (guint32 m = s; m <= e; m++, at++, b++) {
            g_assert_cmpuint (pass->lists[wi->list_offset + at], ==, m);
            g_assert_cmpuint (g_array_index (boxed, guint32, b), ==, m);
          }
          extra += !g_array_index (covered, gboolean, b - 1);
        }
        for (; b < boxed->len; b++)
          if (g_array_index (covered, gboolean, b))
            g_error ("slot %u tile %d,%d: command %u covers the tile but is not listed", slot, tx, ty,
                g_array_index (boxed, guint32, b));
        list_end = MAX (list_end, wi->list_offset + wi->count);
        item++;
      }
  }
  g_assert_cmpuint (item, ==, pass->n_items);
  g_assert_cmpuint (list_end, ==, pass->n_lists);
  g_array_unref (boxed);
  g_array_unref (covered);
  return extra;
}

static void
test_binning (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  GRand *r = g_rand_new_with_seed (20261026);
  guint chains = 0;

  for (guint iter = 0; iter < 40; iter++) {
    /* slots 0..2 with random sizes (rarely multiples of 32); frames 0 and 3 share slot 0 */
    const guint slots[4] = { 0, 1, 2, 0 };
    Img a[3], b[3];
    Frame f[4];
    RdsOsdPass pass;

    for (guint s = 0; s < 3; s++) {
      gint w = rnd (r, 1, 200), h = rnd (r, 1, 160);

      img_init (&a[s], w, h);
      img_init (&b[s], w, h);
    }
    rds_osd_plan_begin (plan);
    for (guint i = 0; i < 4; i++) {
      random_frame (r, &f[i], a[slots[i]].width, a[slots[i]].height);
      rds_osd_plan_add_frame (plan, &opt, slots[i], &f[i].fm, a[slots[i]].width, a[slots[i]].height);
    }
    while (rds_osd_plan_next_pass (plan, &pass)) {
      g_assert_cmpuint (pass.n_items, ==, 0);
      rds_osd_plan_bin (plan, &pass);
      check_binning (&pass, a, 3);
      for (guint32 i = 0; i < pass.n_cmds; i++)
        chains += (pass.cmds[i].flags & RDS_OSD_CMD_UNION_NEXT) != 0;
      draw_pass (rds_osd_plan_heap (plan), &pass, a);
      draw_pass_tiles (rds_osd_plan_heap (plan), &pass, b);
    }
    for (guint s = 0; s < 3; s++) {
      if (!same_pixels (&a[s], &b[s]))
        g_error ("iteration %u slot %u: per-tile shading differs from the CPU path", iter, s);
      img_clear (&a[s]);
      img_clear (&b[s]);
    }
    for (guint i = 0; i < 4; i++)
      frame_clear (&f[i]);
  }
  /* the random shapes did include arrow heads */
  g_assert_cmpuint (chains, >, 20);
  g_rand_free (r);
  rds_osd_plan_free (plan);
}

static void
test_empty_tiles (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  Frame f, g;

  /* one 4 × 4 box inside tile (2, 1) of a 200 × 100 frame: one work item; one across a tile corner: four */
  frame_init (&f);
  frame_add_object (&f)->rect_params = fill (70, 40, 5, 5, BLUE);
  g_assert_true (plan_one (plan, &opt, &f, 200, 100, &pass, NULL));
  rds_osd_plan_bin (plan, &pass);
  g_assert_cmpuint (pass.n_items, ==, 1);
  g_assert_cmpuint (pass.items[0].slot, ==, 0);
  g_assert_cmpuint (pass.items[0].tile_x, ==, 2);
  g_assert_cmpuint (pass.items[0].tile_y, ==, 1);
  g_assert_cmpuint (pass.items[0].count, ==, 1);
  g_assert_cmpuint (pass.n_lists, ==, 1);
  g_assert_cmpuint (pass.lists[0], ==, 0);
  f.obj[0].rect_params = fill (60, 25, 10, 10, BLUE);
  g_assert_true (plan_one (plan, &opt, &f, 200, 100, &pass, NULL));
  rds_osd_plan_bin (plan, &pass);
  g_assert_cmpuint (pass.n_items, ==, 4);
  frame_clear (&f);

  /* two frames on slots 3 and 1, the first with nothing to draw: items of slot 1 only, list indices into the pass */
  frame_init (&f);
  frame_add_object (&f);
  frame_init (&g);
  frame_add_object (&g)->rect_params = fill (0, 0, 40, 5, BLUE);
  rds_osd_plan_begin (plan);
  rds_osd_plan_add_frame (plan, &opt, 3, &f.fm, 64, 64);
  rds_osd_plan_add_frame (plan, &opt, 1, &g.fm, 64, 64);
  g_assert_true (rds_osd_plan_next_pass (plan, &pass));
  rds_osd_plan_bin (plan, &pass);
  g_assert_cmpuint (pass.n_frames, ==, 1);
  g_assert_cmpuint (pass.frames[0].slot, ==, 1);
  g_assert_cmpuint (pass.n_items, ==, 2);
  g_assert_cmpuint (pass.items[0].slot, ==, 1);
  g_assert_cmpuint (pass.items[1].tile_x, ==, 1);
  g_assert_false (rds_osd_plan_next_pass (plan, &pass));
  frame_clear (&f);
  frame_clear (&g);
  rds_osd_plan_free (plan);
}

/* A long diagonal line and a large ring are binned to the tiles they cover, not every tile of their boxes. */
static void
test_culled_tiles (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  Frame f;
  RdsDisplayMeta *dm;
  Img im;
  guint extra;

  img_init (&im, 1920, 1080);
  frame_init (&f);
  dm = frame_add_display (&f);
  dm->num_lines = 1;
  dm->line_params[0] = line (0, 0, 1919, 1079, 3, BLUE);
  g_assert_true (plan_one (plan, &opt, &f, 1920, 1080, &pass, NULL));
  rds_osd_plan_bin (plan, &pass);
  extra = check_binning (&pass, &im, 1);
  g_test_message ("diagonal line: %u tiles (box: 60 x 34), %u covering none", pass.n_items, extra);
  /* about two tiles a column of the 60 × 34 grid */
  g_assert_cmpuint (pass.n_items, <, 3 * 60);

  dm->num_lines = 0;
  dm->num_circles = 1;
  dm->circle_params[0] = (RdsOSD_CircleParams) {.xc = 960,.yc = 540,.radius = 500,.circle_width = 3,.circle_color =
        BLUE
  };
  g_assert_true (plan_one (plan, &opt, &f, 1920, 1080, &pass, NULL));
  rds_osd_plan_bin (plan, &pass);
  extra = check_binning (&pass, &im, 1);
  g_test_message ("ring of radius 500: %u tiles (box: 33 x 34), %u covering none", pass.n_items, extra);
  /* the circumference is ~100 tiles long */
  g_assert_cmpuint (pass.n_items, <, 250);
  frame_clear (&f);
  img_clear (&im);
  rds_osd_plan_free (plan);
}

/* ---- text masks ---- */

static guint32
aligned (guint32 v)
{
  return (v + RDS_OSD_MASK_ALIGN - 1) / RDS_OSD_MASK_ALIGN * RDS_OSD_MASK_ALIGN;
}

/* Plans @s in @font at @size alone at (x, y) of a 400 × 300 frame; returns the pass's new masks (and the new mask's
 * offset and size). */
static guint
plan_text_full (RdsOsdPlan * plan, const gchar * font, guint size, const gchar * s, guint x, guint y,
    guint32 * offset, guint32 * mask_size)
{
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  Frame f;

  frame_init (&f);
  frame_add_object (&f)->text_params = text (s, x, y, size, WHITE, FALSE, BLUE);
  f.obj[0].text_params.font_params.font_name = (gchar *) font;
  g_assert_true (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  frame_clear (&f);
  g_assert_cmpuint (pass.n_cmds, ==, 1);
  g_assert_cmpuint (pass.n_uploads, ==, pass.n_new_masks);
  if (pass.n_uploads > 0) {
    g_assert_cmpuint (pass.uploads[0].offset, ==, pass.cmds[0].mask.offset);
    if (offset != NULL)
      *offset = pass.uploads[0].offset;
    if (mask_size != NULL)
      *mask_size = pass.uploads[0].size;
  }
  return pass.n_new_masks;
}

static guint
plan_text_at (RdsOsdPlan * plan, const gchar * s, guint x, guint y)
{
  return plan_text_full (plan, "Sans", 20, s, x, y, NULL, NULL);
}

static guint
plan_text (RdsOsdPlan * plan, const gchar * s, guint32 * offset, guint32 * mask_size)
{
  return plan_text_full (plan, "Sans", 20, s, 10, 10, offset, mask_size);
}

static void
test_text_cache (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  guint32 sa, sb, sc, off;

  /* a miss rasterizes and uploads; the same text, font and size again is a hit, wherever it is drawn whole */
  g_assert_cmpuint (plan_text (plan, "Alpha", NULL, &sa), ==, 1);
  g_assert_cmpuint (plan_text (plan, "Alpha", NULL, NULL), ==, 0);
  g_assert_cmpuint (plan_text_at (plan, "Alpha", 200, 150), ==, 0);
  /* clipped by the frame's edge: another mask, a hit again at the same clip */
  g_assert_cmpuint (plan_text_at (plan, "Alpha", 370, 150), ==, 1);
  g_assert_cmpuint (plan_text_at (plan, "Alpha", 370, 100), ==, 0);
  g_assert_cmpuint (plan_text_at (plan, "Alpha", 380, 150), ==, 1);
  g_assert_cmpuint (plan_text (plan, "Bravo", NULL, &sb), ==, 1);
  g_assert_cmpuint (plan_text (plan, "i", NULL, &sc), ==, 1);
  rds_osd_plan_free (plan);
  g_test_message ("mask sizes: Alpha %u, Bravo %u, i %u", sa, sb, sc);
  g_assert_cmpuint (aligned (sc), <=, aligned (sb));

  /* a heap that holds Alpha and Bravo exactly */
  plan = rds_osd_plan_new (aligned (sa) + aligned (sb));
  g_assert_cmpuint (plan_text (plan, "Alpha", &off, NULL), ==, 1);
  g_assert_cmpuint (off, ==, 0);
  g_assert_cmpuint (plan_text (plan, "Bravo", &off, NULL), ==, 1);
  g_assert_cmpuint (off, ==, aligned (sa));
  /* Alpha used last: Bravo is the least recently used and makes room for i (first in, Alpha, stays) */
  g_assert_cmpuint (plan_text (plan, "Alpha", NULL, NULL), ==, 0);
  g_assert_cmpuint (plan_text (plan, "i", &off, NULL), ==, 1);
  g_assert_cmpuint (off, ==, aligned (sa));
  g_assert_cmpuint (plan_text (plan, "Alpha", NULL, NULL), ==, 0);
  /* Bravo again: i (now the least recently used) goes */
  g_assert_cmpuint (plan_text (plan, "Bravo", &off, NULL), ==, 1);
  g_assert_cmpuint (off, ==, aligned (sa));
  g_assert_cmpuint (plan_text (plan, "Alpha", NULL, NULL), ==, 0);
  g_assert_cmpuint (plan_text (plan, "i", NULL, NULL), ==, 1);
  rds_osd_plan_free (plan);

  /* a mask the rest of the batch needs is evicted after those it does not: Alpha, the least recently used, stays
   * for the batch [i, Alpha], Bravo makes room for i; one upload, one pass */
  plan = rds_osd_plan_new (aligned (sa) + aligned (sb));
  g_assert_cmpuint (plan_text (plan, "Alpha", NULL, NULL), ==, 1);
  g_assert_cmpuint (plan_text (plan, "Bravo", NULL, NULL), ==, 1);
  {
    RdsOsdDrawOptions opt = options ();
    RdsOsdPass pass;
    Frame f;

    frame_init (&f);
    frame_add_object (&f)->text_params = text ("i", 10, 10, 20, WHITE, FALSE, BLUE);
    frame_add_object (&f)->text_params = text ("Alpha", 10, 10, 20, WHITE, FALSE, BLUE);
    g_assert_true (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
    g_assert_cmpuint (pass.n_cmds, ==, 2);
    g_assert_cmpuint (pass.n_uploads, ==, 1);
    g_assert_cmpuint (pass.uploads[0].offset, ==, aligned (sa));
    g_assert_cmpuint (pass.cmds[1].mask.offset, ==, 0);
    frame_clear (&f);
  }
  rds_osd_plan_free (plan);

  /* a mask exactly the heap's size fits; a heap one alignment step smaller draws nothing */
  plan = rds_osd_plan_new (aligned (sc));
  g_assert_cmpuint (plan_text (plan, "i", &off, NULL), ==, 1);
  rds_osd_plan_free (plan);
  if (aligned (sc) > RDS_OSD_MASK_ALIGN) {
    RdsOsdDrawOptions opt = options ();
    RdsOsdPass pass;
    Frame f;

    plan = rds_osd_plan_new (aligned (sc) - RDS_OSD_MASK_ALIGN);
    frame_init (&f);
    frame_add_object (&f)->text_params = text ("i", 10, 10, 20, WHITE, FALSE, BLUE);
    g_assert_false (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
    frame_clear (&f);
    rds_osd_plan_free (plan);
  }
  /* the heap is a whole number of alignment steps */
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_CRITICAL, "*heap_size % RDS_OSD_MASK_ALIGN*");
  g_assert_null (rds_osd_plan_new (aligned (sc) + 1));
  g_test_assert_expected_messages ();
}

static void
test_text_keys (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE), *alone = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  RdsOsdCmd want[2];
  guint8 *want_mask;
  Frame f;

  /* font "Sans\x1f20" at 10 with "x" and font "Sans" at 20 with "10\x1fx" join to the same string with a 0x1f
   * separator: they must not share extents or masks */
  frame_init (&f);
  frame_add_object (&f)->text_params = text ("10\x1fx", 10, 10, 20, WHITE, TRUE, BLUE);
  g_assert_true (plan_one (alone, &opt, &f, 400, 300, &pass, NULL));
  g_assert_cmpuint (pass.n_cmds, ==, 2);
  memcpy (want, pass.cmds, sizeof want);
  want_mask = g_memdup2 (rds_osd_plan_heap (alone) + pass.cmds[1].mask.offset, pass.uploads[0].size);

  g_assert_cmpuint (plan_text_full (plan, "Sans\x1f" "20", 10, "x", 10, 10, NULL, NULL), ==, 1);
  g_assert_true (plan_one (plan, &opt, &f, 400, 300, &pass, NULL));
  g_assert_cmpuint (pass.n_cmds, ==, 2);
  g_assert_cmpuint (pass.n_uploads, ==, 1);
  g_assert_cmpuint (memcmp (&pass.cmds[0], &want[0], sizeof want[0]), ==, 0);
  g_assert_cmpint (pass.cmds[1].x1, ==, want[1].x1);
  g_assert_cmpint (pass.cmds[1].y1, ==, want[1].y1);
  g_assert_cmpuint (pass.cmds[1].mask.stride, ==, want[1].mask.stride);
  g_assert_cmpuint (memcmp (rds_osd_plan_heap (plan) + pass.cmds[1].mask.offset, want_mask, pass.uploads[0].size),
      ==, 0);
  g_free (want_mask);
  frame_clear (&f);
  rds_osd_plan_free (plan);
  rds_osd_plan_free (alone);
}

static void
test_slot_size (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  Frame f;

  /* a second frame on a slot with another size is not drawn (its tiles would not be the slot's) */
  frame_init (&f);
  frame_add_object (&f)->rect_params = fill (0, 0, 90, 90, BLUE);
  rds_osd_plan_begin (plan);
  rds_osd_plan_add_frame (plan, &opt, 0, &f.fm, 100, 100);
  g_test_expect_message ("rdsosd", G_LOG_LEVEL_CRITICAL, "*slot 0*");
  rds_osd_plan_add_frame (plan, &opt, 0, &f.fm, 200, 200);
  g_test_assert_expected_messages ();
  rds_osd_plan_add_frame (plan, &opt, 0, &f.fm, 100, 100);
  g_assert_true (rds_osd_plan_next_pass (plan, &pass));
  g_assert_cmpuint (pass.n_frames, ==, 2);
  g_assert_cmpint (pass.frames[0].width, ==, 100);
  g_assert_cmpint (pass.frames[1].width, ==, 100);
  rds_osd_plan_bin (plan, &pass);
  g_assert_cmpuint (pass.n_items, ==, 9);
  frame_clear (&f);
  rds_osd_plan_free (plan);
}

#define N_BATCHES 4

/* A batch of random labelled objects and arrows (union chains) on two slots; @r continues across batches, and labels
 * repeat, so masks are hits within and across batches. */
static void
labels_batch (GRand * r, Frame f[2], gchar labels[2][24][16])
{
  for (guint k = 0; k < 2; k++) {
    RdsDisplayMeta *dm;

    frame_init (&f[k]);
    for (guint i = 0; i < 24; i++) {
      RdsObjectMeta *om = frame_add_object (&f[k]);

      g_snprintf (labels[k][i], sizeof labels[k][i], "obj %u", rnd (r, 0, 30));
      om->rect_params = rect (rnd (r, 0, 300), rnd (r, 0, 200), rnd (r, 10, 80), rnd (r, 10, 60), 2,
          random_color (r));
      om->text_params = text (labels[k][i], rnd (r, 0, 330), rnd (r, 0, 230), rnd (r, 8, 18), random_color (r),
          g_rand_boolean (r), random_color (r));
    }
    dm = frame_add_display (&f[k]);
    dm->num_arrows = 4;
    for (guint i = 0; i < dm->num_arrows; i++)
      dm->arrow_params[i] = (RdsOSD_ArrowParams) {.x1 = rnd (r, 0, 360),.y1 = rnd (r, 0, 260),.x2 =
            rnd (r, 0, 360),.y2 = rnd (r, 0, 260),.arrow_width = rnd (r, 1, 5),.arrow_head =
            RDS_OSD_ARROW_BOTH_HEAD,.arrow_color = random_color (r)
      };
  }
}

/* Draws N_BATCHES batches of labels_batch (from @seed) with one plan of @heap_size into imgs[batch][slot], twice: on
 * the CPU path and per tile, both reading a mirror of the heap that only the passes' uploads fill (as the GPU's).
 * Returns the number of passes; @max_mask: the largest mask. */
static guint
draw_labels (gsize heap_size, guint seed, Img imgs[N_BATCHES][2], guint32 * max_mask)
{
  RdsOsdPlan *plan = rds_osd_plan_new (heap_size);
  RdsOsdDrawOptions opt = options ();
  GRand *r = g_rand_new_with_seed (seed);
  guint8 *mirror = g_malloc (heap_size);
  gchar labels[2][24][16];
  RdsOsdPass pass;
  guint passes = 0;

  /* the clock after each frame's arrows: a pass can end right after a union chain */
  opt.display_clock = TRUE;
  opt.clock_text = "2026-10-26 12:00:00";
  opt.y_clock_offset = 200;
  memset (mirror, 0xA5, heap_size);
  if (max_mask != NULL)
    *max_mask = 0;
  for (guint b = 0; b < N_BATCHES; b++) {
    Frame f[2];
    Img tiles[2];

    labels_batch (r, f, labels);
    rds_osd_plan_begin (plan);
    for (guint k = 0; k < 2; k++) {
      img_init (&tiles[k], imgs[b][k].width, imgs[b][k].height);
      rds_osd_plan_add_frame (plan, &opt, k, &f[k].fm, imgs[b][k].width, imgs[b][k].height);
    }
    while (rds_osd_plan_next_pass (plan, &pass)) {
      passes++;
      for (guint32 i = 0; i < pass.n_uploads; i++) {
        g_assert_cmpuint ((gsize) pass.uploads[i].offset + pass.uploads[i].size, <=, heap_size);
        memcpy (mirror + pass.uploads[i].offset, rds_osd_plan_heap (plan) + pass.uploads[i].offset,
            pass.uploads[i].size);
      }
      for (guint32 i = 0; i < pass.n_cmds && max_mask != NULL; i++)
        if (pass.cmds[i].type == RDS_OSD_CMD_MASK)
          *max_mask = MAX (*max_mask, pass.cmds[i].mask.stride * (guint32) (pass.cmds[i].y1 - pass.cmds[i].y0 + 1));
      rds_osd_plan_bin (plan, &pass);
      check_binning (&pass, imgs[b], 2);
      draw_pass (mirror, &pass, imgs[b]);
      draw_pass_tiles (mirror, &pass, tiles);
    }
    for (guint k = 0; k < 2; k++) {
      g_assert_true (same_pixels (&imgs[b][k], &tiles[k]));
      img_clear (&tiles[k]);
      frame_clear (&f[k]);
    }
  }
  g_free (mirror);
  g_rand_free (r);
  rds_osd_plan_free (plan);
  return passes;
}

static void
test_passes (void)
{
  for (guint seed = 1; seed <= 5; seed++) {
    Img one[N_BATCHES][2], many[N_BATCHES][2];
    guint32 max_mask;
    guint passes;

    for (guint b = 0; b < N_BATCHES; b++)
      for (guint k = 0; k < 2; k++) {
        img_init (&one[b][k], 360, 260);
        img_init (&many[b][k], 360, 260);
      }
    /* one pass a batch with the full heap */
    g_assert_cmpuint (draw_labels (RDS_OSD_MASK_HEAP_SIZE, seed, one, &max_mask), ==, N_BATCHES);
    /* room for a few masks only: the batches are split, the pixels are the same */
    passes = draw_labels (aligned (max_mask) * 3, seed, many, NULL);
    g_test_message ("seed %u: %u passes for %u batches with a %u-byte heap", seed, passes, N_BATCHES,
        aligned (max_mask) * 3);
    g_assert_cmpuint (passes, >, 2 * N_BATCHES);
    for (guint b = 0; b < N_BATCHES; b++)
      for (guint k = 0; k < 2; k++) {
        if (!same_pixels (&one[b][k], &many[b][k]))
          g_error ("seed %u batch %u slot %u: split passes draw other pixels", seed, b, k);
        img_clear (&one[b][k]);
        img_clear (&many[b][k]);
      }
  }
}

/* Plans the batch @f (two slots of 360 × 260) with @plan and draws it from @mirror, a copy of the heap that only the
 * passes' uploads fill; returns the masks rasterized. */
static guint
draw_batch_mirror (RdsOsdPlan * plan, guint8 * mirror, Frame f[2], Img imgs[2])
{
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  guint new_masks = 0;

  rds_osd_plan_begin (plan);
  for (guint k = 0; k < 2; k++)
    rds_osd_plan_add_frame (plan, &opt, k, &f[k].fm, imgs[k].width, imgs[k].height);
  while (rds_osd_plan_next_pass (plan, &pass)) {
    for (guint32 i = 0; i < pass.n_uploads; i++)
      memcpy (mirror + pass.uploads[i].offset, rds_osd_plan_heap (plan) + pass.uploads[i].offset,
          pass.uploads[i].size);
    new_masks += pass.n_new_masks;
    draw_pass (mirror, &pass, imgs);
  }
  return new_masks;
}

static void
test_drop_masks (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  guint8 *mirror = g_malloc (RDS_OSD_MASK_HEAP_SIZE);
  GRand *r = g_rand_new_with_seed (7);
  gchar labels[2][24][16];
  Img a[2], b[2], c[2];
  Frame f[2];
  guint first;

  labels_batch (r, f, labels);
  for (guint k = 0; k < 2; k++) {
    img_init (&a[k], 360, 260);
    img_init (&b[k], 360, 260);
    img_init (&c[k], 360, 260);
  }
  memset (mirror, 0xA5, RDS_OSD_MASK_HEAP_SIZE);
  first = draw_batch_mirror (plan, mirror, f, a);
  g_assert_cmpuint (first, >, 10);
  g_assert_cmpuint (draw_batch_mirror (plan, mirror, f, b), ==, 0);
  /* the GPU's heap is lost: after the drop every mask is rasterized and uploaded again */
  rds_osd_plan_drop_masks (plan);
  memset (mirror, 0x5A, RDS_OSD_MASK_HEAP_SIZE);
  g_assert_cmpuint (draw_batch_mirror (plan, mirror, f, c), ==, first);
  for (guint k = 0; k < 2; k++) {
    g_assert_true (same_pixels (&a[k], &b[k]));
    g_assert_true (same_pixels (&a[k], &c[k]));
    img_clear (&a[k]);
    img_clear (&b[k]);
    img_clear (&c[k]);
    frame_clear (&f[k]);
  }
  g_rand_free (r);
  g_free (mirror);
  rds_osd_plan_free (plan);
}

static void
test_pass_budget (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE), *small = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  GRand *r = g_rand_new_with_seed (99);
  const guint64 budget = 100;
  guint split = 0;

  rds_osd_plan_set_pass_budget (small, budget);
  for (guint iter = 0; iter < 20; iter++) {
    Img one[2], many[2], tiles[2];
    Frame f[2];
    RdsOsdPass pass;
    guint passes = 0;

    for (guint k = 0; k < 2; k++) {
      gint w = rnd (r, 40, 240), h = rnd (r, 40, 200);

      img_init (&one[k], w, h);
      img_init (&many[k], w, h);
      img_init (&tiles[k], w, h);
      random_frame (r, &f[k], w, h);
    }
    rds_osd_plan_begin (plan);
    rds_osd_plan_begin (small);
    for (guint k = 0; k < 2; k++) {
      rds_osd_plan_add_frame (plan, &opt, k, &f[k].fm, one[k].width, one[k].height);
      rds_osd_plan_add_frame (small, &opt, k, &f[k].fm, one[k].width, one[k].height);
    }
    while (rds_osd_plan_next_pass (plan, &pass))
      draw_pass (rds_osd_plan_heap (plan), &pass, one);
    /* each pass within the budget (counted as the binning uses it: lists, and the tiles of each slot's grid), or a
     * single shape */
    while (rds_osd_plan_next_pass (small, &pass)) {
      guint64 words, shapes = 0;
      gboolean seen[2] = { FALSE, FALSE };

      passes++;
      rds_osd_plan_bin (small, &pass);
      check_binning (&pass, one, 2);
      words = pass.n_lists;
      for (guint32 i = 0; i < pass.n_frames; i++)
        if (!seen[pass.frames[i].slot]) {
          seen[pass.frames[i].slot] = TRUE;
          words += (guint64) ((pass.frames[i].width + 31) / 32) * ((pass.frames[i].height + 31) / 32);
        }
      for (guint32 i = 0; i < pass.n_cmds; i++)
        shapes += !(pass.cmds[i].flags & RDS_OSD_CMD_UNION_NEXT);
      if (words > budget && shapes != 1)
        g_error ("iteration %u: a pass of %u shapes needs %" G_GUINT64_FORMAT " words", iter, (guint) shapes, words);
      draw_pass (rds_osd_plan_heap (small), &pass, many);
      draw_pass_tiles (rds_osd_plan_heap (small), &pass, tiles);
    }
    split += passes > 1;
    for (guint k = 0; k < 2; k++) {
      if (!same_pixels (&one[k], &many[k]) || !same_pixels (&one[k], &tiles[k]))
        g_error ("iteration %u slot %u: the budget's passes draw other pixels", iter, k);
      img_clear (&one[k]);
      img_clear (&many[k]);
      img_clear (&tiles[k]);
      frame_clear (&f[k]);
    }
  }
  g_assert_cmpuint (split, >, 10);
  g_rand_free (r);
  rds_osd_plan_free (plan);
  rds_osd_plan_free (small);
}

static void
test_frame_limits (void)
{
  RdsOsdPlan *plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  RdsOsdDrawOptions opt = options ();
  RdsOsdPass pass;
  Frame f;

  /* ours: frames above 2^21 a side or 2^20 tiles are not drawn */
  frame_init (&f);
  frame_add_object (&f)->rect_params = fill (0, -1, 50, 2, BLUE);
  g_assert_true (plan_one (plan, &opt, &f, RDS_OSD_MAX_PLAN_FRAME, 1, &pass, NULL));
  g_assert_false (plan_one (plan, &opt, &f, RDS_OSD_MAX_PLAN_FRAME + 1, 1, &pass, NULL));
  g_assert_false (plan_one (plan, &opt, &f, 1, RDS_OSD_MAX_PLAN_FRAME + 1, &pass, NULL));
  g_assert_true (plan_one (plan, &opt, &f, 1024 * 32, 1024 * 32, &pass, NULL));
  g_assert_false (plan_one (plan, &opt, &f, 1024 * 32 + 1, 1024 * 32, &pass, NULL));
  /* the last tile of the widest frame */
  f.obj[0].rect_params = fill (RDS_OSD_MAX_PLAN_FRAME - 10, -1, 50, 2, BLUE);
  g_assert_true (plan_one (plan, &opt, &f, RDS_OSD_MAX_PLAN_FRAME, 1, &pass, NULL));
  rds_osd_plan_bin (plan, &pass);
  g_assert_cmpuint (pass.n_items, ==, 1);
  g_assert_cmpuint (pass.items[0].tile_x, ==, 65535);
  frame_clear (&f);
  rds_osd_plan_free (plan);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/cpu/osd-plan/order", test_order);
  g_test_add_func ("/cpu/osd-plan/clamps", test_clamps);
  g_test_add_func ("/cpu/osd-plan/binning", test_binning);
  g_test_add_func ("/cpu/osd-plan/empty-tiles", test_empty_tiles);
  g_test_add_func ("/cpu/osd-plan/culled-tiles", test_culled_tiles);
  g_test_add_func ("/cpu/osd-plan/text-cache", test_text_cache);
  g_test_add_func ("/cpu/osd-plan/text-keys", test_text_keys);
  g_test_add_func ("/cpu/osd-plan/slot-size", test_slot_size);
  g_test_add_func ("/cpu/osd-plan/drop-masks", test_drop_masks);
  g_test_add_func ("/cpu/osd-plan/pass-budget", test_pass_budget);
  g_test_add_func ("/cpu/osd-plan/frame-limits", test_frame_limits);
  g_test_add_func ("/cpu/osd-plan/passes", test_passes);
  return g_test_run ();
}
