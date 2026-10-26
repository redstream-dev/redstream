/* rdsosd's drawing core on plain host RGBA buffers: rectangle border bands and fills (exact pixels, clipping, alpha),
 * text boxes and fonts, lines, arrows, circles, the display switches, draw order, display-meta limits, the clock, and
 * the draw time of a 1280x720 frame with 60 objects. No GStreamer, no GPU.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-osd-draw.h"

#include <math.h>
#include <string.h>

#define GREY 128
/* bytes past each row's pixels, to check that nothing is drawn into the row padding */
#define ROW_PAD 16
#define PAD_BYTE 0xEE

typedef struct
{
  guint8 *data;
  gint width, height, stride;
} Img;

static const RdsOSD_ColorParams RED = { 1, 0, 0, 1 }, GREEN = { 0, 1, 0, 1 }, BLUE = { 0, 0, 1, 1 },
    WHITE = { 1, 1, 1, 1 }, YELLOW = { 1, 1, 0, 1 }, MAGENTA = { 1, 0, 1, 1 }, CYAN = { 0, 1, 1, 1 };

static RdsOsdDraw *draw_ctx;

static void
img_init (Img * im, gint w, gint h)
{
  im->width = w;
  im->height = h;
  im->stride = w * 4 + ROW_PAD;
  im->data = g_malloc ((gsize) im->stride * h);
  memset (im->data, PAD_BYTE, (gsize) im->stride * h);
  for (gint y = 0; y < h; y++)
    for (gint x = 0; x < w; x++) {
      guint8 *p = im->data + (gsize) y * im->stride + 4 * x;
      p[0] = p[1] = p[2] = GREY;
      p[3] = 255;
    }
}

static void
img_clear (Img * im)
{
  for (gint y = 0; y < im->height; y++)
    for (gint i = 0; i < ROW_PAD; i++)
      g_assert_cmpuint (im->data[(gsize) y * im->stride + 4 * im->width + i], ==, PAD_BYTE);
  g_free (im->data);
}

static const guint8 *
px (const Img * im, gint x, gint y)
{
  g_assert_true (x >= 0 && y >= 0 && x < im->width && y < im->height);
  return im->data + (gsize) y * im->stride + 4 * x;
}

static gboolean
is_rgb (const Img * im, gint x, gint y, guint8 r, guint8 g, guint8 b)
{
  const guint8 *p = px (im, x, y);

  return p[0] == r && p[1] == g && p[2] == b && p[3] == 255;
}

static gboolean
is_grey (const Img * im, gint x, gint y)
{
  return is_rgb (im, x, y, GREY, GREY, GREY);
}

static gboolean
is_color (const Img * im, gint x, gint y, const RdsOSD_ColorParams * c)
{
  return is_rgb (im, x, y, (guint8) (c->red * 255), (guint8) (c->green * 255), (guint8) (c->blue * 255));
}

/* Any pixel within @r of (x, y) not grey. */
static gboolean
any_drawn_near (const Img * im, gint x, gint y, gint r)
{
  for (gint yy = MAX (y - r, 0); yy <= MIN (y + r, im->height - 1); yy++)
    for (gint xx = MAX (x - r, 0); xx <= MIN (x + r, im->width - 1); xx++)
      if (!is_grey (im, xx, yy))
        return TRUE;
  return FALSE;
}

/* The bounding box of the non-grey pixels; FALSE when there are none. */
static gboolean
drawn_bbox (const Img * im, gint * x0, gint * y0, gint * x1, gint * y1)
{
  *x0 = *y0 = G_MAXINT;
  *x1 = *y1 = -1;
  for (gint y = 0; y < im->height; y++)
    for (gint x = 0; x < im->width; x++)
      if (!is_grey (im, x, y)) {
        *x0 = MIN (*x0, x);
        *y0 = MIN (*y0, y);
        *x1 = MAX (*x1, x);
        *y1 = MAX (*y1, y);
      }
  return *x1 >= 0;
}

static gboolean
untouched (const Img * im)
{
  gint x0, y0, x1, y1;

  return !drawn_bbox (im, &x0, &y0, &x1, &y1);
}

/* ---- meta ---- */

/* A hand-built frame: objects and display metas live in the test's arrays, the lists point at them. */
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
  om->object_id = RDS_UNTRACKED_OBJECT_ID;
  om->confidence = 0.9f;
  f->fm.obj_meta_list = g_list_append (f->fm.obj_meta_list, om);
  f->fm.num_obj_meta++;
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
text (const gchar * s, guint x, guint y, const gchar * font, guint size, gboolean bg)
{
  return (RdsOSD_TextParams) {.display_text = (gchar *) s,.x_offset = x,.y_offset = y,.font_params = {
  .font_name = (gchar *) font,.font_size = size,.font_color = WHITE},.set_bg_clr = bg,.text_bg_clr = BLUE};
}

static RdsOsdDrawOptions
options (void)
{
  return (RdsOsdDrawOptions) {.display_bbox = TRUE,.display_text = TRUE,.clock_color = RED };
}

static RdsOsdDrawFlags
draw (Img * im, Frame * f, const RdsOsdDrawOptions * opt)
{
  RdsOsdDrawOptions def = options ();

  return rds_osd_draw_frame (draw_ctx, opt != NULL ? opt : &def, f != NULL ? &f->fm : NULL, im->data, im->width,
      im->height, im->stride);
}

/* ---- rectangles ---- */

/* Border band of a rect with edges L, R (R = L + width) and width bw (DeepStream 8.0 GPU mode): left/top band
 * [L - (bw-1)/2, L + bw/2], right/bottom band [R - bw/2, R + (bw-1)/2]. */
static gboolean
in_border (gint x, gint y, gint L, gint T, gint R, gint B, gint bw)
{
  gint lo = (bw - 1) / 2, hi = bw / 2;
  gboolean in_x = x >= L - lo && x <= R + lo, in_y = y >= T - lo && y <= B + lo;

  if (bw == 0 || !in_x || !in_y)
    return FALSE;
  return x <= L + hi || x >= R - hi || y <= T + hi || y >= B - hi;
}

static void
check_border (guint bw, guint drawn_bw)
{
  Img im;
  Frame f;
  RdsOsdDrawFlags flags;
  const gint L = 100, T = 80, R = 220, B = 170;

  img_init (&im, 320, 240);
  frame_init (&f);
  frame_add_object (&f)->rect_params = rect (L, T, R - L, B - T, bw, RED);
  flags = draw (&im, &f, NULL);
  g_assert_cmpint (flags & RDS_OSD_DRAW_BORDER_CLAMPED, ==, bw > RDS_OSD_MAX_BORDER_WIDTH ?
      RDS_OSD_DRAW_BORDER_CLAMPED : 0);
  for (gint y = 0; y < im.height; y++)
    for (gint x = 0; x < im.width; x++) {
      gboolean want = in_border (x, y, L, T, R, B, (gint) drawn_bw);

      if (want != is_rgb (&im, x, y, 255, 0, 0) || (!want && !is_grey (&im, x, y)))
        g_error ("bw %u: pixel %d,%d: expected %s", bw, x, y, want ? "border" : "background");
    }
  frame_clear (&f);
  img_clear (&im);
}

static void
test_border_bands (void)
{
  Img im;
  Frame f;

  check_border (0, 0);
  check_border (1, 1);
  check_border (2, 2);
  check_border (3, 3);
  check_border (4, 4);
  check_border (10, 10);
  check_border (32, 32);
  /* ours: clamped to MAX_BORDER_WIDTH (DeepStream 8.0 stops the pipeline) */
  check_border (40, 32);

  /* spot checks of the band formula: bw 3 at L 100 is 99..101, bw 4 is 99..102, bw 32 is 85..116 */
  img_init (&im, 320, 240);
  frame_init (&f);
  frame_add_object (&f)->rect_params = rect (100, 80, 120, 90, 4, RED);
  draw (&im, &f, NULL);
  g_assert_true (is_grey (&im, 98, 120));
  for (gint x = 99; x <= 102; x++)
    g_assert_true (is_rgb (&im, x, 120, 255, 0, 0));
  g_assert_true (is_grey (&im, 103, 120));
  /* right edge R = 220: 218..221 */
  g_assert_true (is_grey (&im, 217, 120));
  for (gint x = 218; x <= 221; x++)
    g_assert_true (is_rgb (&im, x, 120, 255, 0, 0));
  g_assert_true (is_grey (&im, 222, 120));
  frame_clear (&f);
  img_clear (&im);
}

static void
test_border_truncation (void)
{
  Img im;
  Frame f;

  /* DeepStream 8.0 behaviour: each float field truncated ([700.6,300.4,50.5,40.5] -> 700..750 x 300..340) */
  img_init (&im, 800, 400);
  frame_init (&f);
  frame_add_object (&f)->rect_params = rect (700.6f, 300.4f, 50.5f, 40.5f, 1, RED);
  draw (&im, &f, NULL);
  g_assert_true (is_rgb (&im, 700, 320, 255, 0, 0));
  g_assert_true (is_grey (&im, 699, 320));
  g_assert_true (is_grey (&im, 701, 320));
  g_assert_true (is_rgb (&im, 750, 320, 255, 0, 0));
  g_assert_true (is_grey (&im, 751, 320));
  g_assert_true (is_rgb (&im, 720, 300, 255, 0, 0));
  g_assert_true (is_rgb (&im, 720, 340, 255, 0, 0));
  g_assert_true (is_grey (&im, 720, 341));
  frame_clear (&f);
  img_clear (&im);
}

static void
test_border_clipping (void)
{
  Img im;
  Frame f;
  gint x0, y0, x1, y1;

  /* partly outside: clipped, not moved (DeepStream 8.0 GPU mode) */
  img_init (&im, 320, 240);
  frame_init (&f);
  frame_add_object (&f)->rect_params = rect (-50, -20, 100, 60, 4, RED);   /* R 50, B 40 */
  frame_add_object (&f)->rect_params = rect (280, 200, 100, 100, 1, RED);  /* L 280, T 200 */
  draw (&im, &f, NULL);
  for (gint x = 0; x <= 51; x++)
    g_assert_true (is_rgb (&im, x, 39, 255, 0, 0));
  for (gint y = 0; y <= 41; y++)
    g_assert_true (is_rgb (&im, 49, y, 255, 0, 0));
  /* nothing at the frame's left edge inside the box: the box was not clamped into the frame */
  g_assert_true (is_grey (&im, 0, 20));
  g_assert_true (is_grey (&im, 20, 0));
  g_assert_true (is_rgb (&im, 280, 239, 255, 0, 0));
  g_assert_true (is_rgb (&im, 319, 200, 255, 0, 0));
  g_assert_true (is_grey (&im, 319, 239));
  frame_clear (&f);
  img_clear (&im);

  /* wholly outside, empty, negative and absurd sizes: nothing drawn, no crash */
  img_init (&im, 320, 240);
  frame_init (&f);
  frame_add_object (&f)->rect_params = rect (400, 10, 50, 50, 4, RED);
  frame_add_object (&f)->rect_params = rect (10, -300, 50, 50, 4, RED);
  frame_add_object (&f)->rect_params = rect (1e30f, 1e30f, 1e30f, 1e30f, 4, RED);
  frame_add_object (&f)->rect_params = rect (-1e30f, -1e30f, 10, 10, 4, RED);
  frame_add_object (&f)->rect_params = rect (NAN, NAN, NAN, NAN, 4, RED);
  frame_add_object (&f)->rect_params = rect (100, 100, 0, 0, 0, RED);
  frame_add_object (&f)->rect_params = rect (100, 100, 50, 50, 0, RED);
  draw (&im, &f, NULL);
  g_assert_true (untouched (&im));
  frame_clear (&f);
  img_clear (&im);

  /* a huge box: only its visible bands */
  img_init (&im, 320, 240);
  frame_init (&f);
  frame_add_object (&f)->rect_params = rect (10, 10, 1e9f, 1e9f, 2, RED);
  draw (&im, &f, NULL);
  g_assert_true (drawn_bbox (&im, &x0, &y0, &x1, &y1));
  g_assert_cmpint (x0, ==, 10);
  g_assert_cmpint (y0, ==, 10);
  g_assert_cmpint (x1, ==, 319);
  g_assert_cmpint (y1, ==, 239);
  g_assert_true (is_grey (&im, 100, 100));
  frame_clear (&f);
  img_clear (&im);
}

static void
test_fill_alpha (void)
{
  Img im;
  Frame f;
  RdsOSD_RectParams r = rect (100, 50, 100, 60, 4, (RdsOSD_ColorParams) {
      0, 0, 1, 0.5});

  img_init (&im, 320, 240);
  frame_init (&f);
  /* bw 4 border, semi-transparent blue; red fill at alpha 0.5 */
  r.has_bg_color = 1;
  r.bg_color = (RdsOSD_ColorParams) {
  1, 0, 0, 0.5};
  frame_add_object (&f)->rect_params = r;
  /* bw 0, opaque green fill */
  frame_add_object (&f)->rect_params = fill (220, 50, 60, 60, GREEN);
  draw (&im, &f, NULL);
  /* DeepStream 8.0 behaviour: a*src + (1-a)*dst, truncated: red 0.5 on 128 -> (191,64,64); border alpha applied */
  g_assert_true (is_rgb (&im, 150, 80, 191, 64, 64));
  g_assert_true (is_rgb (&im, 100, 80, 64, 64, 191));
  /* the fill lies strictly inside the band (bw 4: L-1..L+2), from L+3 to R-3 */
  g_assert_true (is_rgb (&im, 102, 80, 64, 64, 191));
  g_assert_true (is_rgb (&im, 103, 80, 191, 64, 64));
  g_assert_true (is_rgb (&im, 197, 80, 191, 64, 64));
  g_assert_true (is_rgb (&im, 198, 80, 64, 64, 191));
  g_assert_true (is_rgb (&im, 150, 53, 191, 64, 64));
  g_assert_true (is_rgb (&im, 150, 52, 64, 64, 191));
  /* corners blended once */
  g_assert_true (is_rgb (&im, 99, 49, 64, 64, 191));
  /* bw 0: the fill covers L+1 .. R-1 */
  g_assert_true (is_grey (&im, 220, 80));
  g_assert_true (is_rgb (&im, 221, 80, 0, 255, 0));
  g_assert_true (is_rgb (&im, 279, 80, 0, 255, 0));
  g_assert_true (is_grey (&im, 280, 80));
  g_assert_true (is_grey (&im, 250, 50));
  g_assert_true (is_rgb (&im, 250, 51, 0, 255, 0));
  g_assert_true (is_rgb (&im, 250, 109, 0, 255, 0));
  g_assert_true (is_grey (&im, 250, 110));
  frame_clear (&f);
  img_clear (&im);
}

/* ---- text ---- */

static gint
count_rgb (const Img * im, gint x0, gint y0, gint x1, gint y1, guint8 r, guint8 g, guint8 b)
{
  gint n = 0;

  for (gint y = MAX (y0, 0); y <= MIN (y1, im->height - 1); y++)
    for (gint x = MAX (x0, 0); x <= MIN (x1, im->width - 1); x++)
      n += is_rgb (im, x, y, r, g, b);
  return n;
}

static void
test_text_box (void)
{
  Img im;
  Frame f;
  gint x0, y0, x1, y1;

  /* DeepStream 8.0 behaviour: (x_offset, y_offset) + (1, 1) is the box's top left; size 20: a ~40 px tall box with
   * ~10 px padding left and right */
  img_init (&im, 640, 360);
  frame_init (&f);
  frame_add_object (&f)->text_params = text ("Hello", 300, 100, "Sans", 20, TRUE);
  draw (&im, &f, NULL);
  g_assert_true (drawn_bbox (&im, &x0, &y0, &x1, &y1));
  g_test_message ("text box %d,%d .. %d,%d (%dx%d)", x0, y0, x1, y1, x1 - x0 + 1, y1 - y0 + 1);
  g_assert_cmpint (x0, ==, 301);
  g_assert_cmpint (y0, ==, 101);
  g_assert_cmpint (y1 - y0 + 1, >=, 37);
  g_assert_cmpint (y1 - y0 + 1, <=, 44);
  g_assert_cmpint (x1 - x0 + 1, >=, 75);
  g_assert_cmpint (x1 - x0 + 1, <=, 105);
  /* the box is opaque blue at its corners and the text white inside it, inset by the padding */
  g_assert_true (is_rgb (&im, x0, y0, 0, 0, 255));
  g_assert_true (is_rgb (&im, x1, y1, 0, 0, 255));
  g_assert_cmpint (count_rgb (&im, x0, y0, x1, y1, 255, 255, 255), >, 60);
  g_assert_cmpint (count_rgb (&im, x0, y0, x0 + 6, y1, 255, 255, 255), ==, 0);
  g_assert_cmpint (count_rgb (&im, x1 - 6, y0, x1, y1, 255, 255, 255), ==, 0);
  frame_clear (&f);
  img_clear (&im);

  /* without the background: only the glyphs, at the same place */
  img_init (&im, 640, 360);
  frame_init (&f);
  frame_add_object (&f)->text_params = text ("Hello", 300, 100, "Sans", 20, FALSE);
  draw (&im, &f, NULL);
  g_assert_true (drawn_bbox (&im, &x0, &y0, &x1, &y1));
  g_assert_cmpint (x0, >=, 305);
  g_assert_cmpint (x0, <=, 318);
  g_assert_cmpint (y0, >=, 104);
  g_assert_cmpint (y0, <=, 115);
  g_assert_cmpint (x1, <=, 300 + 105);
  g_assert_cmpint (y1, <=, 100 + 44);
  frame_clear (&f);
  img_clear (&im);

  /* three lines at size 20: ~104 px tall; size 10: about half a size-20 box, size 40 about twice */
  {
    const struct
    {
      const gchar *s;
      guint size;
      gint min_h, max_h;
    } cases[] = { {"Line1\nLine2\nLine3", 20, 98, 110}, {"Size10", 10, 17, 24}, {"Size40", 40, 76, 88} };

    for (guint i = 0; i < G_N_ELEMENTS (cases); i++) {
      img_init (&im, 640, 360);
      frame_init (&f);
      frame_add_object (&f)->text_params = text (cases[i].s, 100, 100, "Serif", cases[i].size, TRUE);
      draw (&im, &f, NULL);
      g_assert_true (drawn_bbox (&im, &x0, &y0, &x1, &y1));
      g_test_message ("'%s' size %u: box %dx%d", cases[i].s, cases[i].size, x1 - x0 + 1, y1 - y0 + 1);
      g_assert_cmpint (y1 - y0 + 1, >=, cases[i].min_h);
      g_assert_cmpint (y1 - y0 + 1, <=, cases[i].max_h);
      frame_clear (&f);
      img_clear (&im);
    }
  }
}

static void
test_text_clipped (void)
{
  Img im;
  Frame f;

  /* DeepStream 8.0 behaviour: not clamped into the frame, clipped at its edges */
  img_init (&im, 640, 360);
  frame_init (&f);
  frame_add_object (&f)->text_params = text ("RightEdgeText", 600, 100, "Serif", 20, TRUE);
  frame_add_object (&f)->text_params = text ("Bottom", 100, 350, "Serif", 20, TRUE);
  frame_add_object (&f)->text_params = text ("Far", 5000, 5000, "Serif", 20, TRUE);
  frame_add_object (&f)->text_params = text ("Far", G_MAXUINT, G_MAXUINT, "Serif", 20, TRUE);
  draw (&im, &f, NULL);
  g_assert_true (is_rgb (&im, 639, 101, 0, 0, 255));
  g_assert_true (is_rgb (&im, 601, 101, 0, 0, 255));
  g_assert_true (is_grey (&im, 600, 101));
  g_assert_true (is_rgb (&im, 101, 351, 0, 0, 255));
  g_assert_true (is_rgb (&im, 101, 359, 0, 0, 255));
  g_assert_true (is_grey (&im, 101, 350));
  frame_clear (&f);
  img_clear (&im);
}

/* Draws "Hello" in @font and returns the image (to compare renderings). */
static Img
render_font (const gchar * font, guint size)
{
  Img im;
  Frame f;

  img_init (&im, 320, 120);
  frame_init (&f);
  frame_add_object (&f)->text_params = text ("Hello Fonts", 10, 10, font, size, TRUE);
  draw (&im, &f, NULL);
  frame_clear (&f);
  return im;
}

static gboolean
same_pixels (const Img * a, const Img * b)
{
  return memcmp (a->data, b->data, (gsize) a->stride * a->height) == 0;
}

static void
test_text_fonts (void)
{
  Img sans = render_font ("Sans", 20), null_font = render_font (NULL, 20), empty = render_font ("", 20),
      unknown = render_font ("NoSuchFontXYZ", 20), serif = render_font ("Serif", 20),
      mono = render_font ("DejaVu Sans Mono", 20), zero = render_font ("Sans", 0);

  /* unknown -> Sans (DeepStream 8.0 behaviour, through fontconfig); NULL and "" -> Sans (ours: 8.0's GPU mode
   * crashes on NULL) */
  g_assert_false (untouched (&sans));
  g_assert_true (same_pixels (&sans, &null_font));
  g_assert_true (same_pixels (&sans, &empty));
  g_assert_true (same_pixels (&sans, &unknown));
  /* font_name is honoured */
  g_assert_false (same_pixels (&sans, &serif));
  g_assert_false (same_pixels (&sans, &mono));
  /* font_size 0 draws nothing, not even the box (DeepStream 8.0 behaviour) */
  g_assert_true (untouched (&zero));

  img_clear (&sans);
  img_clear (&null_font);
  img_clear (&empty);
  img_clear (&unknown);
  img_clear (&serif);
  img_clear (&mono);
  img_clear (&zero);
}

static void
test_text_empty (void)
{
  Img im;
  Frame f;

  /* NULL or empty text draws nothing, not even the box (DeepStream 8.0 GPU mode) */
  img_init (&im, 320, 120);
  frame_init (&f);
  frame_add_object (&f)->text_params = text (NULL, 10, 10, "Sans", 20, TRUE);
  frame_add_object (&f)->text_params = text ("", 100, 10, "Sans", 20, TRUE);
  draw (&im, &f, NULL);
  g_assert_true (untouched (&im));
  frame_clear (&f);
  img_clear (&im);
}

static void
test_text_utf8 (void)
{
  Img im;
  Frame f;
  gint x0, y0, x1, y1;

  /* ours: text is UTF-8, shaped by Pango; invalid bytes are drawn as U+FFFD instead of being refused */
  img_init (&im, 640, 120);
  frame_init (&f);
  frame_add_object (&f)->text_params = text ("Gr\xc3\xb6\xc3\x9f" "e \xe2\x82\xac", 10, 10, "Sans", 20, FALSE);
  frame_add_object (&f)->text_params = text ("bad \xff\xfe bytes", 300, 10, "Sans", 20, FALSE);
  draw (&im, &f, NULL);
  g_assert_true (drawn_bbox (&im, &x0, &y0, &x1, &y1));
  g_assert_cmpint (count_rgb (&im, 0, 0, 290, 119, 255, 255, 255), >, 30);
  g_assert_cmpint (count_rgb (&im, 300, 0, 639, 119, 255, 255, 255), >, 30);
  frame_clear (&f);
  img_clear (&im);
}

static void
test_text_alpha (void)
{
  Img im;
  Frame f;
  RdsOSD_TextParams t = text ("Alpha", 10, 10, "Sans", 20, TRUE);

  img_init (&im, 320, 120);
  frame_init (&f);
  t.text_bg_clr = (RdsOSD_ColorParams) {
  1, 0, 0, 0.5};
  frame_add_object (&f)->text_params = t;
  draw (&im, &f, NULL);
  /* the box is blended like a fill */
  g_assert_true (is_rgb (&im, 11, 11, 191, 64, 64));
  frame_clear (&f);
  img_clear (&im);
}

/* ---- lines, arrows, circles ---- */

static RdsOSD_LineParams
line (guint x1, guint y1, guint x2, guint y2, guint w, RdsOSD_ColorParams c)
{
  return (RdsOSD_LineParams) {.x1 = x1,.y1 = y1,.x2 = x2,.y2 = y2,.line_width = w,.line_color = c };
}

static void
test_lines (void)
{
  Img im;
  Frame f;
  RdsDisplayMeta *dm;

  img_init (&im, 640, 360);
  frame_init (&f);
  dm = frame_add_display (&f);
  dm->num_lines = 4;
  dm->line_params[0] = line (50, 100, 250, 100, 5, RED);
  dm->line_params[1] = line (50, 150, 250, 150, 1, GREEN);
  dm->line_params[2] = line (400, 50, 400, 150, 6, BLUE);
  dm->line_params[3] = line (500, 200, 700, 200, 3, YELLOW);        /* past the right edge: clipped */
  draw (&im, &f, NULL);
  /* DeepStream 8.0 GPU mode: hard edges, (w-1)/2 on each side of the segment, ends extended as far */
  g_assert_true (is_grey (&im, 150, 97));
  for (gint y = 98; y <= 102; y++)
    g_assert_true (is_color (&im, 150, y, &RED));
  g_assert_true (is_grey (&im, 150, 103));
  g_assert_true (is_grey (&im, 47, 100));
  g_assert_true (is_color (&im, 48, 100, &RED));
  g_assert_true (is_color (&im, 252, 100, &RED));
  g_assert_true (is_grey (&im, 253, 100));
  /* width 1: one row, from x1 to x2 */
  g_assert_true (is_grey (&im, 150, 149));
  g_assert_true (is_color (&im, 150, 150, &GREEN));
  g_assert_true (is_grey (&im, 150, 151));
  g_assert_true (is_grey (&im, 49, 150));
  g_assert_true (is_color (&im, 50, 150, &GREEN));
  g_assert_true (is_color (&im, 250, 150, &GREEN));
  g_assert_true (is_grey (&im, 251, 150));
  /* width 6 is drawn 5 wide */
  g_assert_true (is_grey (&im, 397, 100));
  for (gint x = 398; x <= 402; x++)
    g_assert_true (is_color (&im, x, 100, &BLUE));
  g_assert_true (is_grey (&im, 403, 100));
  g_assert_true (is_color (&im, 400, 48, &BLUE));
  g_assert_true (is_grey (&im, 400, 47));
  g_assert_true (is_color (&im, 400, 152, &BLUE));
  g_assert_true (is_grey (&im, 400, 153));
  g_assert_true (is_color (&im, 639, 200, &YELLOW));
  frame_clear (&f);
  img_clear (&im);
}

static void
check_arrow (RdsOSD_Arrow_Head_Direction head, gboolean at_start, gboolean at_end)
{
  Img im;
  Frame f;
  RdsDisplayMeta *dm;

  img_init (&im, 400, 300);
  frame_init (&f);
  dm = frame_add_display (&f);
  dm->num_arrows = 2;
  dm->arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 100,.y1 = 100,.x2 = 300,.y2 = 100,.arrow_width = 3,
    .arrow_head = head,.arrow_color = RED };
  dm->arrow_params[1] = (RdsOSD_ArrowParams) {.x1 = 50,.y1 = 50,.x2 = 50,.y2 = 250,.arrow_width = 2,
    .arrow_head = head,.arrow_color = GREEN };
  draw (&im, &f, NULL);
  /* the shaft */
  g_assert_true (is_color (&im, 200, 100, &RED));
  g_assert_true (is_grey (&im, 200, 104));
  g_assert_true (is_color (&im, 50, 150, &GREEN));
  /* the heads' wings: ~7 px off the shaft, ~15 px back from the tip */
  g_assert_cmpint (any_drawn_near (&im, 285, 95, 1) && any_drawn_near (&im, 285, 105, 1), ==, at_end);
  g_assert_cmpint (any_drawn_near (&im, 115, 95, 1) && any_drawn_near (&im, 115, 105, 1), ==, at_start);
  g_assert_cmpint (any_drawn_near (&im, 45, 235, 1) && any_drawn_near (&im, 55, 235, 1), ==, at_end);
  g_assert_cmpint (any_drawn_near (&im, 45, 65, 1) && any_drawn_near (&im, 55, 65, 1), ==, at_start);
  /* no head reaches far from the tip */
  g_assert_false (any_drawn_near (&im, 260, 90, 2));
  frame_clear (&f);
  img_clear (&im);
}

static void
test_arrows (void)
{
  /* DeepStream 8.0 behaviour for START and END; ours for BOTH (8.0 draws one head or none) */
  check_arrow (RDS_OSD_ARROW_START_HEAD, TRUE, FALSE);
  check_arrow (RDS_OSD_ARROW_END_HEAD, FALSE, TRUE);
  check_arrow (RDS_OSD_ARROW_BOTH_HEAD, TRUE, TRUE);
}

static void
test_arrow_degenerate (void)
{
  Img im;
  Frame f;
  RdsDisplayMeta *dm;

  /* a zero-length arrow and one far outside the frame: no crash */
  img_init (&im, 100, 100);
  frame_init (&f);
  dm = frame_add_display (&f);
  dm->num_arrows = 2;
  dm->arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 50,.y1 = 50,.x2 = 50,.y2 = 50,.arrow_width = 3,
    .arrow_head = RDS_OSD_ARROW_BOTH_HEAD,.arrow_color = RED };
  dm->arrow_params[1] = (RdsOSD_ArrowParams) {.x1 = G_MAXUINT,.y1 = G_MAXUINT,.x2 = G_MAXUINT - 5,.y2 = 7,
    .arrow_width = 3,.arrow_head = RDS_OSD_ARROW_END_HEAD,.arrow_color = RED };
  draw (&im, &f, NULL);
  frame_clear (&f);
  img_clear (&im);
}

static gboolean
drawn_at_distance (const Img * im, gint xc, gint yc, gint d)
{
  return !is_grey (im, xc - d, yc) && !is_grey (im, xc + d, yc) && !is_grey (im, xc, yc - d)
      && !is_grey (im, xc, yc + d);
}

static gboolean
grey_at_distance (const Img * im, gint xc, gint yc, gint d)
{
  return is_grey (im, xc - d, yc) && is_grey (im, xc + d, yc) && is_grey (im, xc, yc - d) && is_grey (im, xc, yc + d);
}

static void
test_circles (void)
{
  Img im;
  Frame f;
  RdsDisplayMeta *dm;

  img_init (&im, 640, 360);
  frame_init (&f);
  dm = frame_add_display (&f);
  dm->num_circles = 4;
  dm->circle_params[0] = (RdsOSD_CircleParams) {.xc = 100,.yc = 100,.radius = 60,.circle_color = RED,
    .circle_width = 3 };
  dm->circle_params[1] = (RdsOSD_CircleParams) {.xc = 300,.yc = 100,.radius = 60,.circle_color = GREEN,
    .has_bg_color = 1,.bg_color = BLUE,.circle_width = 3 };
  /* circle_width 0 still outlines (DeepStream 8.0 behaviour) */
  dm->circle_params[2] = (RdsOSD_CircleParams) {.xc = 500,.yc = 100,.radius = 60,.circle_color = YELLOW };
  /* past the frame's corner: clipped */
  dm->circle_params[3] = (RdsOSD_CircleParams) {.xc = 630,.yc = 350,.radius = 40,.circle_color = CYAN,
    .circle_width = 3 };
  draw (&im, &f, NULL);
  /* DeepStream 8.0 GPU mode: w 3, r 60 covers distances 59..62 on every side */
  g_assert_true (grey_at_distance (&im, 100, 100, 58));
  for (gint dist = 59; dist <= 62; dist++)
    g_assert_true (drawn_at_distance (&im, 100, 100, dist));
  g_assert_true (grey_at_distance (&im, 100, 100, 63));
  g_assert_true (is_color (&im, 40, 100, &RED));
  g_assert_true (is_color (&im, 162, 100, &RED));
  g_assert_true (is_grey (&im, 100, 100));
  /* filled inside the ring (to distance 58), the ring on top */
  g_assert_true (is_color (&im, 300, 100, &BLUE));
  g_assert_true (is_color (&im, 300 - 58, 100, &BLUE));
  g_assert_true (is_color (&im, 300 + 58, 100, &BLUE));
  for (gint dist = 59; dist <= 62; dist++) {
    g_assert_true (is_color (&im, 300 - dist, 100, &GREEN));
    g_assert_true (is_color (&im, 300 + dist, 100, &GREEN));
    g_assert_true (is_color (&im, 300, 100 - dist, &GREEN));
    g_assert_true (is_color (&im, 300, 100 + dist, &GREEN));
  }
  g_assert_true (grey_at_distance (&im, 300, 100, 63));
  /* width 0: a 3 px ring, 59..61 (DeepStream 8.0 behaviour) */
  g_assert_true (grey_at_distance (&im, 500, 100, 58));
  for (gint dist = 59; dist <= 61; dist++)
    g_assert_true (drawn_at_distance (&im, 500, 100, dist));
  g_assert_true (grey_at_distance (&im, 500, 100, 62));
  g_assert_true (is_color (&im, 630 - 40, 350, &CYAN));
  frame_clear (&f);
  img_clear (&im);
}

/* ---- switches, order, limits ---- */

/* One of everything: an object (rect + text), a display meta (rect, label, line, arrow, circle) and the clock. */
static void
frame_everything (Frame * f)
{
  RdsObjectMeta *om;
  RdsDisplayMeta *dm;

  frame_init (f);
  om = frame_add_object (f);
  om->rect_params = rect (20, 20, 60, 40, 3, RED);
  om->text_params = text ("obj", 20, 100, "Sans", 12, TRUE);
  dm = frame_add_display (f);
  dm->num_rects = 1;
  dm->rect_params[0] = rect (120, 20, 60, 40, 3, GREEN);
  dm->num_labels = 1;
  dm->text_params[0] = text ("label", 120, 100, "Sans", 12, TRUE);
  dm->num_lines = 1;
  dm->line_params[0] = line (20, 180, 180, 180, 3, YELLOW);
  dm->num_arrows = 1;
  dm->arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 20,.y1 = 220,.x2 = 180,.y2 = 220,.arrow_width = 3,
    .arrow_head = RDS_OSD_ARROW_END_HEAD,.arrow_color = MAGENTA };
  dm->num_circles = 1;
  dm->circle_params[0] = (RdsOSD_CircleParams) {.xc = 260,.yc = 200,.radius = 30,.circle_color = CYAN,
    .circle_width = 2 };
}

typedef struct
{
  gboolean obj_rect, dm_rect, obj_text, dm_text, line, arrow, circle, clock;
} Seen;

static Seen
seen_everything (const Img * im)
{
  return (Seen) {
  .obj_rect = is_color (im, 20, 40, &RED),.dm_rect = is_color (im, 120, 40, &GREEN),.obj_text =
        is_color (im, 21, 101, &BLUE),.dm_text = is_color (im, 121, 101, &BLUE),.line =
        is_color (im, 100, 180, &YELLOW),.arrow = is_color (im, 100, 220, &MAGENTA),.circle =
        is_color (im, 230, 200, &CYAN),.clock = count_rgb (im, 300, 250, 639, 359, 255, 0, 0) > 0};
}

static void
check_switches (gboolean bbox, gboolean txt, gboolean clock, Seen want)
{
  Img im;
  Frame f;
  RdsOsdDrawOptions opt = options ();
  Seen got;

  opt.display_bbox = bbox;
  opt.display_text = txt;
  opt.display_clock = clock;
  opt.clock_text = "2026-10-26 12:34:56";
  opt.x_clock_offset = 300;
  opt.y_clock_offset = 250;
  img_init (&im, 640, 360);
  frame_everything (&f);
  draw (&im, &f, &opt);
  got = seen_everything (&im);
  g_assert_cmpint (got.obj_rect, ==, want.obj_rect);
  g_assert_cmpint (got.dm_rect, ==, want.dm_rect);
  g_assert_cmpint (got.obj_text, ==, want.obj_text);
  g_assert_cmpint (got.dm_text, ==, want.dm_text);
  g_assert_cmpint (got.line, ==, want.line);
  g_assert_cmpint (got.arrow, ==, want.arrow);
  g_assert_cmpint (got.circle, ==, want.circle);
  g_assert_cmpint (got.clock, ==, want.clock);
  frame_clear (&f);
  img_clear (&im);
}

static void
test_switches (void)
{
  /* DeepStream 8.0 behaviour: display-bbox hides object and display-meta rects only; display-text hides object
   * text, display-meta labels and the clock */
  check_switches (TRUE, TRUE, FALSE, (Seen) {
      TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, FALSE});
  check_switches (TRUE, TRUE, TRUE, (Seen) {
      TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE});
  check_switches (FALSE, TRUE, TRUE, (Seen) {
      FALSE, FALSE, TRUE, TRUE, TRUE, TRUE, TRUE, TRUE});
  check_switches (TRUE, FALSE, TRUE, (Seen) {
      TRUE, TRUE, FALSE, FALSE, TRUE, TRUE, TRUE, FALSE});
  check_switches (FALSE, FALSE, TRUE, (Seen) {
      FALSE, FALSE, FALSE, FALSE, TRUE, TRUE, TRUE, FALSE});
}

static void
test_draw_order (void)
{
  Img im;
  Frame f;
  RdsDisplayMeta *dm;

  /* DeepStream 8.0 behaviour: rects (objects, then display metas) < text (objects, then labels) < lines < arrows <
   * circles; within a kind, list order, later on top */
  img_init (&im, 400, 300);
  frame_init (&f);
  frame_add_object (&f)->rect_params = fill (100, 100, 100, 100, RED);
  frame_add_object (&f)->rect_params = fill (120, 100, 100, 100, YELLOW);
  frame_add_object (&f)->text_params = text ("XXXXXXXX", 160, 160, "Sans", 20, TRUE);
  dm = frame_add_display (&f);
  dm->num_rects = 1;
  dm->rect_params[0] = fill (150, 150, 100, 100, GREEN);
  dm->num_labels = 1;
  dm->text_params[0] = text ("XXXXXXXX", 160, 220, "Sans", 20, TRUE);
  dm->text_params[0].text_bg_clr = WHITE;
  dm->num_lines = 1;
  dm->line_params[0] = line (100, 170, 300, 170, 5, CYAN);
  dm->num_arrows = 1;
  dm->arrow_params[0] = (RdsOSD_ArrowParams) {.x1 = 170,.y1 = 100,.x2 = 170,.y2 = 290,.arrow_width = 3,
    .arrow_head = RDS_OSD_ARROW_END_HEAD,.arrow_color = MAGENTA };
  dm->num_circles = 1;
  dm->circle_params[0] = (RdsOSD_CircleParams) {.xc = 170,.yc = 180,.radius = 6,.circle_color = BLUE,
    .has_bg_color = 1,.bg_color = BLUE,.circle_width = 1 };
  /* a later display meta's rect still goes under every text */
  dm = frame_add_display (&f);
  dm->num_rects = 1;
  dm->rect_params[0] = fill (0, 215, 400, 40, RED);
  draw (&im, &f, NULL);
  g_assert_true (is_color (&im, 110, 150, &RED));
  g_assert_true (is_color (&im, 130, 150, &YELLOW));       /* the later object rect on top */
  g_assert_true (is_color (&im, 155, 190, &GREEN));        /* the display rect over object rects */
  g_assert_true (is_color (&im, 162, 162, &BLUE));         /* object text over rects */
  g_assert_true (is_color (&im, 300, 222, &WHITE));        /* labels over display rects of later metas too */
  g_assert_true (is_color (&im, 290, 170, &CYAN));         /* lines over text */
  g_assert_true (is_color (&im, 170, 150, &MAGENTA));      /* arrows over lines and rects */
  g_assert_true (is_color (&im, 170, 169, &MAGENTA));      /* arrows over lines (the line covers rows 168..172) */
  g_assert_true (is_color (&im, 175, 170, &CYAN));
  g_assert_true (is_color (&im, 170, 180, &BLUE));         /* circles over arrows */
  g_assert_true (is_color (&im, 170, 174, &BLUE));         /* the ring over the line's last row and the arrow */
  frame_clear (&f);
  img_clear (&im);
}

static void
test_display_limits (void)
{
  Img im;
  Frame f;
  RdsDisplayMeta *dm;

  /* DeepStream 8.0 behaviour: at most 16 of each kind are read; several display metas are all drawn */
  img_init (&im, 640, 360);
  frame_init (&f);
  for (guint k = 0; k < 3; k++) {
    dm = frame_add_display (&f);
    dm->num_rects = 20;
    dm->num_labels = 20;
    dm->num_lines = 20;
    dm->num_arrows = 20;
    dm->num_circles = 20;
    for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
      dm->rect_params[i] = rect (10 + 30 * i, 10 + 60 * k, 20, 20, 1, RED);
  }
  draw (&im, &f, NULL);
  for (guint k = 0; k < 3; k++)
    for (guint i = 0; i < RDS_MAX_ELEMENTS_IN_DISPLAY_META; i++)
      g_assert_true (is_color (&im, 10 + 30 * i, 20 + 60 * k, &RED));
  frame_clear (&f);
  img_clear (&im);
}

static void
test_tracker_object (void)
{
  Img a, b;
  Frame f;
  RdsObjectMeta *om;

  /* a tracker-created object (confidence -0.1, an object id) is drawn like any other (DeepStream 8.0 behaviour) */
  img_init (&a, 320, 240);
  img_init (&b, 320, 240);
  frame_init (&f);
  om = frame_add_object (&f);
  om->rect_params = rect (50, 50, 100, 80, 3, RED);
  om->text_params = text ("car 7", 50, 30, "Serif", 10, TRUE);
  draw (&a, &f, NULL);
  om->object_id = 7;
  om->confidence = -0.1f;
  om->tracker_confidence = 0.4f;
  draw (&b, &f, NULL);
  g_assert_false (untouched (&a));
  g_assert_true (same_pixels (&a, &b));
  frame_clear (&f);
  img_clear (&a);
  img_clear (&b);
}

static void
test_masks_not_drawn (void)
{
  Img im;
  Frame f;
  RdsObjectMeta *om;
  gfloat mask[16];

  /* masks are not drawn yet (display-mask is accepted by the element, with a warning) */
  for (guint i = 0; i < G_N_ELEMENTS (mask); i++)
    mask[i] = 1.0f;
  img_init (&im, 320, 240);
  frame_init (&f);
  om = frame_add_object (&f);
  om->rect_params = rect (50, 50, 100, 80, 0, RED);
  om->mask_params = (RdsOSD_MaskParams) {.data = mask,.size = sizeof mask,.threshold = 0.5f,.width = 4,.height = 4 };
  draw (&im, &f, NULL);
  g_assert_true (untouched (&im));
  frame_clear (&f);
  img_clear (&im);
}

/* ---- clock ---- */

static void
test_clock_text (void)
{
  GDateTime *t = g_date_time_new_local (2026, 1, 2, 3, 4, 5.9);
  gchar *s = rds_osd_clock_text (t);

  g_assert_cmpstr (s, ==, "2026-01-02 03:04:05");
  g_free (s);
  g_date_time_unref (t);

  g_assert_cmpfloat (rds_osd_color_from_rgba (0x00ff00ffu).green, ==, 1.0);
  g_assert_cmpfloat (rds_osd_color_from_rgba (0x00ff00ffu).red, ==, 0.0);
  g_assert_cmpfloat (rds_osd_color_from_rgba (0x00ff00ffu).alpha, ==, 1.0);
  g_assert_cmpfloat (rds_osd_color_from_rgba (0xff000080u).red, ==, 1.0);
  g_assert_cmpfloat (fabs (rds_osd_color_from_rgba (0xff000080u).alpha - 128.0 / 255.0), <, 1e-9);
}

static void
check_clock (guint size, gint min_h, gint max_h)
{
  Img im;
  RdsOsdDrawOptions opt = options ();
  gint x0, y0, x1, y1;

  opt.display_clock = TRUE;
  opt.clock_text = "2026-10-26 12:34:56";
  opt.clock_font_size = size;
  opt.clock_color = GREEN;
  opt.x_clock_offset = 20;
  opt.y_clock_offset = 30;
  img_init (&im, 640, 360);
  draw (&im, NULL, &opt);
  g_assert_true (drawn_bbox (&im, &x0, &y0, &x1, &y1));
  g_test_message ("clock size %u: ink %d,%d .. %d,%d", size, x0, y0, x1, y1);
  /* glyphs only (no box), green, inset from the offsets like text */
  g_assert_cmpint (x0, >, 20);
  g_assert_cmpint (x0, <, 20 + 16);
  g_assert_cmpint (y0, >, 30);
  g_assert_cmpint (y0, <, 30 + 16);
  g_assert_cmpint (y1 - y0 + 1, >=, min_h);
  g_assert_cmpint (y1 - y0 + 1, <=, max_h);
  g_assert_cmpint (count_rgb (&im, x0, y0, x1, y1, 0, 255, 0), >, 20);
  g_assert_cmpint (count_rgb (&im, x0, y0, x1, y1, 255, 0, 0), ==, 0);
  img_clear (&im);
}

static void
test_clock (void)
{
  Img im;
  RdsOsdDrawOptions opt = options ();

  /* size 0: ~12 px digits; size 20: ~20 px digits (DeepStream 8.0 behaviour) */
  check_clock (0, 10, 14);
  check_clock (20, 18, 22);

  /* alpha 0 (clock-color 0): invisible */
  opt.display_clock = TRUE;
  opt.clock_text = "2026-10-26 12:34:56";
  opt.clock_color = (RdsOSD_ColorParams) {
  0, 0, 0, 0};
  img_init (&im, 640, 360);
  draw (&im, NULL, &opt);
  g_assert_true (untouched (&im));
  img_clear (&im);
}

/* ---- timing ---- */

static void
test_draw_time (void)
{
  Img im;
  Frame f;
  gchar labels[60][32];
  gint64 t0;
  const guint iterations = 50;
  gdouble ms;

  /* 60 objects styled as rdsinfer and rdstracker style them (rds-object-style.h): a 3 px red box and a white size-10
   * label on black above it */
  img_init (&im, 1280, 720);
  frame_init (&f);
  for (guint i = 0; i < 60; i++) {
    RdsObjectMeta *om = frame_add_object (&f);
    gfloat l = 20 + (i % 10) * 120, t = 40 + (i / 10) * 110;

    om->rect_params = rect (l, t, 90, 80, 3, RED);
    g_snprintf (labels[i], sizeof labels[i], "person %u", i);
    om->text_params = text (labels[i], (guint) l, (guint) t - 20, "Serif", 10, TRUE);
    om->text_params.text_bg_clr = (RdsOSD_ColorParams) {
    0, 0, 0, 1};
  }
  draw (&im, &f, NULL);         /* warm the font cache */
  t0 = g_get_monotonic_time ();
  for (guint i = 0; i < iterations; i++)
    draw (&im, &f, NULL);
  ms = (g_get_monotonic_time () - t0) / 1000.0 / iterations;
  g_test_message ("osd draw time: %.3f ms per 1280x720 frame with 60 objects (budget 5 ms in a release build)", ms);
  frame_clear (&f);
  img_clear (&im);
}

int
main (int argc, char **argv)
{
  gint ret;

  g_test_init (&argc, &argv, NULL);
  draw_ctx = rds_osd_draw_new ();

  g_test_add_func ("/cpu/osd/border-bands", test_border_bands);
  g_test_add_func ("/cpu/osd/border-truncation", test_border_truncation);
  g_test_add_func ("/cpu/osd/border-clipping", test_border_clipping);
  g_test_add_func ("/cpu/osd/fill-alpha", test_fill_alpha);
  g_test_add_func ("/cpu/osd/text-box", test_text_box);
  g_test_add_func ("/cpu/osd/text-clipped", test_text_clipped);
  g_test_add_func ("/cpu/osd/text-fonts", test_text_fonts);
  g_test_add_func ("/cpu/osd/text-empty", test_text_empty);
  g_test_add_func ("/cpu/osd/text-alpha", test_text_alpha);
  g_test_add_func ("/cpu/osd/text-utf8", test_text_utf8);
  g_test_add_func ("/cpu/osd/lines", test_lines);
  g_test_add_func ("/cpu/osd/arrows", test_arrows);
  g_test_add_func ("/cpu/osd/arrow-degenerate", test_arrow_degenerate);
  g_test_add_func ("/cpu/osd/circles", test_circles);
  g_test_add_func ("/cpu/osd/switches", test_switches);
  g_test_add_func ("/cpu/osd/draw-order", test_draw_order);
  g_test_add_func ("/cpu/osd/display-limits", test_display_limits);
  g_test_add_func ("/cpu/osd/tracker-object", test_tracker_object);
  g_test_add_func ("/cpu/osd/masks-not-drawn", test_masks_not_drawn);
  g_test_add_func ("/cpu/osd/clock-text", test_clock_text);
  g_test_add_func ("/cpu/osd/clock", test_clock);
  g_test_add_func ("/cpu/osd/draw-time", test_draw_time);
  ret = g_test_run ();

  rds_osd_draw_free (draw_ctx);
  return ret;
}
