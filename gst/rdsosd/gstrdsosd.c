/* Redstream: rdsosd, draws each frame's object meta, display meta and the clock onto its RGBA surface, on the CPU
 * (rds-osd-draw: hard-edged shapes, Pango text). Property names, ranges and defaults follow DeepStream's nvdsosd as
 * gst-inspect shows it for 8.0; drawing behaviour the docs leave open matches DeepStream 8.0's default GPU mode
 * ("DeepStream 8.0 behaviour") except where marked "ours". Every such choice has a row in docs/compat-matrix.md.
 *
 * In place: the base class hands transform_ip a writable buffer, and the WRITE map gives this element memory it owns
 * (a shared surface or a read-only surface view, as rdsstreamdemux pushes, is replaced by a private copy first; the
 * batch it came from is never written). Each frame meta's slot (batch_id) is synced to the CPU, drawn and synced
 * back, under the batch's meta lock. The syncs are synchronous: the frame is ready when pushed. Frames with nothing
 * to draw are not synced, and a buffer with nothing to draw is not mapped at all (no copy of view memory).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsosd.h"
#include "rds-osd-draw.h"

#include <rds/rds.h>

GST_DEBUG_CATEGORY_STATIC (gst_rds_osd_debug);
#define GST_CAT_DEFAULT gst_rds_osd_debug

/* defaults as gst-inspect shows them for DeepStream 8.0 */
#define DEFAULT_DISPLAY_TEXT TRUE
#define DEFAULT_DISPLAY_BBOX TRUE
#define DEFAULT_DISPLAY_CLOCK FALSE
#define DEFAULT_DISPLAY_MASK FALSE
#define DEFAULT_CLOCK_FONT NULL
#define DEFAULT_CLOCK_FONT_SIZE 0
#define MAX_CLOCK_FONT_SIZE 60
#define DEFAULT_CLOCK_OFFSET 0
#define DEFAULT_CLOCK_COLOR 0
#define DEFAULT_PROCESS_MODE MODE_GPU
#define DEFAULT_GPU_ID 0
#define DEFAULT_BLUR_BBOX FALSE
#define DEFAULT_BLUR_ON_GIE_CLASS_IDS NULL

/* clock-color never set: opaque red (DeepStream 8.0 behaviour; set to 0 explicitly, the clock is invisible) */
#define UNSET_CLOCK_COLOR 0xff0000ffu

enum
{
  MODE_CPU = 0,
  MODE_GPU = 1,
};

enum
{
  PROP_0,
  PROP_DISPLAY_TEXT,
  PROP_DISPLAY_BBOX,
  PROP_DISPLAY_CLOCK,
  PROP_DISPLAY_MASK,
  PROP_CLOCK_FONT,
  PROP_CLOCK_FONT_SIZE,
  PROP_X_CLOCK_OFFSET,
  PROP_Y_CLOCK_OFFSET,
  PROP_CLOCK_COLOR,
  PROP_PROCESS_MODE,
  PROP_GPU_ID,
  PROP_BLUR_BBOX,
  PROP_BLUR_ON_GIE_CLASS_IDS,
};

struct _GstRdsOsd
{
  GstBaseTransform parent;

  /* properties (object lock) */
  gboolean display_text;
  gboolean display_bbox;
  gboolean display_clock;
  gboolean display_mask;
  gchar *clock_font;
  guint clock_font_size;
  guint x_clock_offset;
  guint y_clock_offset;
  guint clock_color;
  gboolean clock_color_set;
  gint process_mode;
  guint gpu_id;
  gboolean blur_bbox;
  gchar *blur_on_gie_class_ids;
  /* accepted no-ops warn once each */
  gboolean display_mask_warned;
  gboolean blur_bbox_warned;
  gboolean blur_ids_warned;

  /* streaming state (streaming thread, or stopped) */
  RdsOsdDraw *draw;
  gboolean border_warned;
  gboolean slot_warned;
};

#define RDS_OSD_CAPS \
  "video/x-raw(" RDS_CAPS_FEATURE_MEMORY_RDS "), format = (string) RGBA, " \
  "width = (int) [ 1, 16384 ], height = (int) [ 1, 16384 ], framerate = (fraction) [ 0/1, 2147483647/1 ]"

/* RGBA only (ours: DeepStream 8.0's GPU mode also takes NV12) */
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_OSD_CAPS));
static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS (RDS_OSD_CAPS));

/* Enum integers and nicks as in DeepStream (configs use the integers); descriptions are our own. */
#define GST_TYPE_RDS_OSD_MODE (gst_rds_osd_mode_get_type ())
static GType
gst_rds_osd_mode_get_type (void)
{
  static GType type = 0;
  static const GEnumValue values[] = {
    {MODE_CPU, "CPU", "MODE_CPU"},
    {MODE_GPU, "GPU in DeepStream; drawn on the CPU here, with the GPU mode's geometry", "MODE_GPU"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&type)) {
    GType t = g_enum_register_static ("GstRdsOsdMode", values);
    g_once_init_leave (&type, t);
  }
  return type;
}

G_DEFINE_TYPE (GstRdsOsd, gst_rds_osd, GST_TYPE_BASE_TRANSFORM);
GST_ELEMENT_REGISTER_DEFINE (rdsosd, "rdsosd", GST_RANK_NONE, GST_TYPE_RDS_OSD);

/* ---- streaming ---- */

static gboolean
gst_rds_osd_start (GstBaseTransform * trans)
{
  GstRdsOsd *self = GST_RDS_OSD (trans);
  gint mode;

  GST_OBJECT_LOCK (self);
  mode = self->process_mode;
  GST_OBJECT_UNLOCK (self);

  GST_INFO_OBJECT (self, "process-mode %d: drawing on the CPU, with the GPU mode's geometry", mode);
  self->draw = rds_osd_draw_new ();
  rds_osd_draw_warm_up (self->draw);
  self->border_warned = FALSE;
  self->slot_warned = FALSE;
  return TRUE;
}

static gboolean
gst_rds_osd_stop (GstBaseTransform * trans)
{
  GstRdsOsd *self = GST_RDS_OSD (trans);

  rds_osd_draw_free (self->draw);
  self->draw = NULL;
  return TRUE;
}

/* Draws @fm (NULL: the clock only) onto slot @slot of @surf: map, sync to the CPU, draw, sync back. */
static gboolean
draw_slot (GstRdsOsd * self, RdsBufSurface * surf, guint slot, const RdsFrameMeta * fm,
    const RdsOsdDrawOptions * opt)
{
  RdsBufSurfaceParams *p = &surf->surfaceList[slot];
  RdsOsdDrawFlags flags;
  gboolean ok;

  if (p->colorFormat != RDS_BUF_COLOR_FORMAT_RGBA) {
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("rdsosd draws on RGBA surfaces only"),
        ("slot %u has colour format %d", slot, p->colorFormat));
    return FALSE;
  }
  if (rds_buf_surface_map (surf, (int) slot, 0, RDS_BUF_MAP_READ_WRITE) != 0) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map frame %u for drawing", slot),
        ("rds_buf_surface_map failed"));
    (void) rds_buf_surface_unmap (surf, (int) slot, 0);
    return FALSE;
  }
  ok = rds_buf_surface_sync_for_cpu (surf, (int) slot, 0) == 0;
  if (ok) {
    flags = rds_osd_draw_frame (self->draw, opt, fm, p->mappedAddr.addr[0], (gint) p->width, (gint) p->height,
        (gint) p->planeParams.pitch[0]);
    if ((flags & RDS_OSD_DRAW_BORDER_CLAMPED) && !self->border_warned) {
      self->border_warned = TRUE;
      GST_ELEMENT_WARNING (self, STREAM, FAILED, ("A border_width above %d was drawn %d pixels wide",
              RDS_OSD_MAX_BORDER_WIDTH, RDS_OSD_MAX_BORDER_WIDTH), ("logged once"));
    }
    ok = rds_buf_surface_sync_for_device (surf, (int) slot, 0) == 0;
  }
  if (!ok)
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot copy frame %u between the GPU and the CPU", slot),
        ("rds_buf_surface_sync_for_cpu or sync_for_device failed"));
  if (rds_buf_surface_unmap (surf, (int) slot, 0) != 0 && ok) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot unmap frame %u", slot), (NULL));
    ok = FALSE;
  }
  return ok;
}

/* A frame to draw: its slot and frame meta (NULL: the clock only). */
typedef struct
{
  guint slot;
  RdsFrameMeta *fm;
} Work;

static GstFlowReturn
gst_rds_osd_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstRdsOsd *self = GST_RDS_OSD (trans);
  RdsBatchMeta *bm = gst_buffer_get_rds_batch_meta (buf);
  RdsOsdDrawOptions opt = { 0 };
  gchar *clock_font, *clock_text = NULL;
  GstMapInfo map;
  RdsBufSurface *in;
  GArray *work = g_array_new (FALSE, FALSE, sizeof (Work));
  GstFlowReturn ret = GST_FLOW_OK;
  guint n_slots;

  GST_OBJECT_LOCK (self);
  opt.display_bbox = self->display_bbox;
  opt.display_text = self->display_text;
  opt.display_clock = self->display_clock;
  clock_font = g_strdup (self->clock_font);
  opt.clock_font_size = self->clock_font_size;
  opt.x_clock_offset = self->x_clock_offset;
  opt.y_clock_offset = self->y_clock_offset;
  opt.clock_color = rds_osd_color_from_rgba (self->clock_color_set ? self->clock_color : UNSET_CLOCK_COLOR);
  GST_OBJECT_UNLOCK (self);
  opt.clock_font = clock_font;
  if (opt.display_clock && opt.display_text) {
    /* local time, one reading per buffer (DeepStream 8.0 behaviour) */
    GDateTime *now = g_date_time_new_now_local ();

    clock_text = rds_osd_clock_text (now);
    g_date_time_unref (now);
    opt.clock_text = clock_text;
  }

  in = rds_buffer_get_surface (buf);
  if (in == NULL) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot draw on the buffer"), ("the input must be memory:RDS"));
    ret = GST_FLOW_ERROR;
    goto done;
  }
  n_slots = MIN (in->numFilled, in->batchSize);

  if (bm != NULL)
    rds_acquire_meta_lock (bm);
  /* The frames with something to draw. Frames with nothing are not mapped, and a buffer with nothing at all is not
   * mapped for writing either: shared or view memory then passes through without a copy. */
  if (bm == NULL || bm->frame_meta_list == NULL) {
    /* no frame meta: only the clock, on every frame (ours) */
    for (guint slot = 0; slot < n_slots && rds_osd_draw_frame_has_work (&opt, NULL); slot++) {
      Work w = { slot, NULL };

      g_array_append_val (work, w);
    }
  }
  for (GList * l = bm != NULL ? bm->frame_meta_list : NULL; l != NULL; l = l->next) {
    Work w = { ((RdsFrameMeta *) l->data)->batch_id, l->data };

    /* each frame on its own surface (ours: DeepStream 8.0 draws every frame's meta on surface 0) */
    if (w.slot >= n_slots) {
      if (!self->slot_warned) {
        self->slot_warned = TRUE;
        GST_ELEMENT_WARNING (self, STREAM, FAILED, ("A frame meta's batch_id is outside the batch: not drawn"),
            ("batch_id %u, %u frames in the buffer (logged once)", w.slot, n_slots));
      }
      continue;
    }
    if (rds_osd_draw_frame_has_work (&opt, w.fm))
      g_array_append_val (work, w);
  }

  if (work->len > 0) {
    /* writable buffer + WRITE map: memory this element may write (a copy of shared or view memory) */
    if (!gst_buffer_map (buf, &map, GST_MAP_WRITE)) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map the buffer for drawing"),
          ("the input must be one writable memory:RDS memory"));
      ret = GST_FLOW_ERROR;
    } else {
      RdsBufSurface *surf = (RdsBufSurface *) map.data;

      for (guint i = 0; i < work->len && ret == GST_FLOW_OK; i++) {
        Work *w = &g_array_index (work, Work, i);

        if (!draw_slot (self, surf, w->slot, w->fm, &opt))
          ret = GST_FLOW_ERROR;
      }
      gst_buffer_unmap (buf, &map);
    }
  }
  if (bm != NULL)
    rds_release_meta_lock (bm);

done:
  g_array_unref (work);
  g_free (clock_text);
  g_free (clock_font);
  return ret;
}

/* ---- properties ---- */

/* Properties DeepStream marks "changeable only in NULL or READY": refuse changes once streaming (object lock held). */
static gboolean
check_mutable (GstRdsOsd * self, GParamSpec * pspec)
{
  GstState highest = MAX (GST_STATE (self), MAX (GST_STATE_NEXT (self), GST_STATE_PENDING (self)));

  if (highest <= GST_STATE_READY)
    return TRUE;
  g_warning ("%s: property '%s' can only be changed in the NULL or READY state; keeping the current value",
      GST_OBJECT_NAME (self), pspec->name);
  return FALSE;
}

static void
gst_rds_osd_set_property (GObject * object, guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstRdsOsd *self = GST_RDS_OSD (object);
  const gchar *noop = NULL;

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_DISPLAY_TEXT:
      self->display_text = g_value_get_boolean (value);
      break;
    case PROP_DISPLAY_BBOX:
      self->display_bbox = g_value_get_boolean (value);
      break;
    case PROP_DISPLAY_CLOCK:
      self->display_clock = g_value_get_boolean (value);
      break;
    case PROP_DISPLAY_MASK:
      self->display_mask = g_value_get_boolean (value);
      if (self->display_mask && !self->display_mask_warned) {
        self->display_mask_warned = TRUE;
        noop = "display-mask: masks are not drawn yet";
      }
      break;
    case PROP_CLOCK_FONT:
      g_free (self->clock_font);
      self->clock_font = g_value_dup_string (value);
      break;
    case PROP_CLOCK_FONT_SIZE:
      if (check_mutable (self, pspec))
        self->clock_font_size = g_value_get_uint (value);
      break;
    case PROP_X_CLOCK_OFFSET:
      if (check_mutable (self, pspec))
        self->x_clock_offset = g_value_get_uint (value);
      break;
    case PROP_Y_CLOCK_OFFSET:
      if (check_mutable (self, pspec))
        self->y_clock_offset = g_value_get_uint (value);
      break;
    case PROP_CLOCK_COLOR:
      if (check_mutable (self, pspec)) {
        self->clock_color = g_value_get_uint (value);
        self->clock_color_set = TRUE;
      }
      break;
    case PROP_PROCESS_MODE:
      if (check_mutable (self, pspec))
        self->process_mode = g_value_get_enum (value);
      break;
    case PROP_GPU_ID:
      if (check_mutable (self, pspec))
        self->gpu_id = g_value_get_uint (value);
      break;
    case PROP_BLUR_BBOX:
      if (check_mutable (self, pspec)) {
        self->blur_bbox = g_value_get_boolean (value);
        if (self->blur_bbox && !self->blur_bbox_warned) {
          self->blur_bbox_warned = TRUE;
          noop = "blur-bbox: objects are not blurred yet";
        }
      }
      break;
    case PROP_BLUR_ON_GIE_CLASS_IDS:
      if (check_mutable (self, pspec)) {
        g_free (self->blur_on_gie_class_ids);
        self->blur_on_gie_class_ids = g_value_dup_string (value);
        if (self->blur_on_gie_class_ids != NULL && !self->blur_ids_warned) {
          self->blur_ids_warned = TRUE;
          noop = "blur-on-gie-class-ids: objects are not blurred yet";
        }
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);

  if (noop != NULL)
    g_warning ("%s: %s; the property is accepted and has no effect", GST_OBJECT_NAME (self), noop);
}

static void
gst_rds_osd_get_property (GObject * object, guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstRdsOsd *self = GST_RDS_OSD (object);

  GST_OBJECT_LOCK (self);
  switch (prop_id) {
    case PROP_DISPLAY_TEXT:
      g_value_set_boolean (value, self->display_text);
      break;
    case PROP_DISPLAY_BBOX:
      g_value_set_boolean (value, self->display_bbox);
      break;
    case PROP_DISPLAY_CLOCK:
      g_value_set_boolean (value, self->display_clock);
      break;
    case PROP_DISPLAY_MASK:
      g_value_set_boolean (value, self->display_mask);
      break;
    case PROP_CLOCK_FONT:
      g_value_set_string (value, self->clock_font);
      break;
    case PROP_CLOCK_FONT_SIZE:
      g_value_set_uint (value, self->clock_font_size);
      break;
    case PROP_X_CLOCK_OFFSET:
      g_value_set_uint (value, self->x_clock_offset);
      break;
    case PROP_Y_CLOCK_OFFSET:
      g_value_set_uint (value, self->y_clock_offset);
      break;
    case PROP_CLOCK_COLOR:
      g_value_set_uint (value, self->clock_color);
      break;
    case PROP_PROCESS_MODE:
      g_value_set_enum (value, self->process_mode);
      break;
    case PROP_GPU_ID:
      g_value_set_uint (value, self->gpu_id);
      break;
    case PROP_BLUR_BBOX:
      g_value_set_boolean (value, self->blur_bbox);
      break;
    case PROP_BLUR_ON_GIE_CLASS_IDS:
      g_value_set_string (value, self->blur_on_gie_class_ids);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
  GST_OBJECT_UNLOCK (self);
}

static void
gst_rds_osd_finalize (GObject * object)
{
  GstRdsOsd *self = GST_RDS_OSD (object);

  g_free (self->clock_font);
  g_free (self->blur_on_gie_class_ids);
  rds_osd_draw_free (self->draw);
  G_OBJECT_CLASS (gst_rds_osd_parent_class)->finalize (object);
}

static void
gst_rds_osd_init (GstRdsOsd * self)
{
  self->display_text = DEFAULT_DISPLAY_TEXT;
  self->display_bbox = DEFAULT_DISPLAY_BBOX;
  self->display_clock = DEFAULT_DISPLAY_CLOCK;
  self->display_mask = DEFAULT_DISPLAY_MASK;
  self->clock_font = g_strdup (DEFAULT_CLOCK_FONT);
  self->clock_font_size = DEFAULT_CLOCK_FONT_SIZE;
  self->x_clock_offset = DEFAULT_CLOCK_OFFSET;
  self->y_clock_offset = DEFAULT_CLOCK_OFFSET;
  self->clock_color = DEFAULT_CLOCK_COLOR;
  self->process_mode = DEFAULT_PROCESS_MODE;
  self->gpu_id = DEFAULT_GPU_ID;
  self->blur_bbox = DEFAULT_BLUR_BBOX;
  self->blur_on_gie_class_ids = g_strdup (DEFAULT_BLUR_ON_GIE_CLASS_IDS);
  gst_base_transform_set_in_place (GST_BASE_TRANSFORM (self), TRUE);
}

static void
gst_rds_osd_class_init (GstRdsOsdClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseTransformClass *trans_class = GST_BASE_TRANSFORM_CLASS (klass);
  const GParamFlags flags = G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS;

  GST_DEBUG_CATEGORY_INIT (gst_rds_osd_debug, "rdsosd", 0, "Redstream on-screen display");

  gobject_class->set_property = gst_rds_osd_set_property;
  gobject_class->get_property = gst_rds_osd_get_property;
  gobject_class->finalize = gst_rds_osd_finalize;

  g_object_class_install_property (gobject_class, PROP_DISPLAY_TEXT,
      g_param_spec_boolean ("display-text", "Display text",
          "Draw the object text, the display-meta labels and the clock", DEFAULT_DISPLAY_TEXT, flags));
  g_object_class_install_property (gobject_class, PROP_DISPLAY_BBOX,
      g_param_spec_boolean ("display-bbox", "Display boxes", "Draw the object and display-meta rectangles",
          DEFAULT_DISPLAY_BBOX, flags));
  g_object_class_install_property (gobject_class, PROP_DISPLAY_CLOCK,
      g_param_spec_boolean ("display-clock", "Display clock", "Draw the local time on every frame",
          DEFAULT_DISPLAY_CLOCK, flags));
  g_object_class_install_property (gobject_class, PROP_DISPLAY_MASK,
      g_param_spec_boolean ("display-mask", "Display masks",
          "Draw the object masks (accepted; masks are not drawn yet)", DEFAULT_DISPLAY_MASK, flags));
  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT,
      g_param_spec_string ("clock-font", "Clock font", "Font of the clock, a fontconfig name (NULL: Serif)",
          DEFAULT_CLOCK_FONT, flags));
  g_object_class_install_property (gobject_class, PROP_CLOCK_FONT_SIZE,
      g_param_spec_uint ("clock-font-size", "Clock font size", "Font size of the clock in points (0: 12)", 0,
          MAX_CLOCK_FONT_SIZE, DEFAULT_CLOCK_FONT_SIZE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_X_CLOCK_OFFSET,
      g_param_spec_uint ("x-clock-offset", "Clock x offset", "Horizontal position of the clock in pixels", 0,
          G_MAXUINT, DEFAULT_CLOCK_OFFSET, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_Y_CLOCK_OFFSET,
      g_param_spec_uint ("y-clock-offset", "Clock y offset", "Vertical position of the clock in pixels", 0,
          G_MAXUINT, DEFAULT_CLOCK_OFFSET, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_CLOCK_COLOR,
      g_param_spec_uint ("clock-color", "Clock colour",
          "Colour of the clock as 0xRRGGBBAA (never set: opaque red; 0: invisible)", 0, G_MAXUINT,
          DEFAULT_CLOCK_COLOR, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_PROCESS_MODE,
      g_param_spec_enum ("process-mode", "Process mode",
          "Accepted for compatibility: both modes draw on the CPU, with the GPU mode's geometry",
          GST_TYPE_RDS_OSD_MODE, DEFAULT_PROCESS_MODE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU id", "Accepted for compatibility (the frames are drawn where they are)", 0,
          G_MAXUINT, DEFAULT_GPU_ID, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_BLUR_BBOX,
      g_param_spec_boolean ("blur-bbox", "Blur boxes", "Blur inside the object boxes (accepted; not blurred yet)",
          DEFAULT_BLUR_BBOX, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_BLUR_ON_GIE_CLASS_IDS,
      g_param_spec_string ("blur-on-gie-class-ids", "Blur on GIE class ids",
          "Blur only the objects of these \"<unique id>,<class id>\" pairs (accepted; not blurred yet)",
          DEFAULT_BLUR_ON_GIE_CLASS_IDS, flags | GST_PARAM_MUTABLE_READY));

  gst_element_class_add_static_pad_template (element_class, &sink_template);
  gst_element_class_add_static_pad_template (element_class, &src_template);
  gst_element_class_set_static_metadata (element_class, "Redstream on-screen display", "Filter/Effect/Video",
      "Draws the boxes, text, lines, arrows and circles of the object and display meta onto memory:RDS RGBA frames",
      "Redstream contributors");

  trans_class->start = GST_DEBUG_FUNCPTR (gst_rds_osd_start);
  trans_class->stop = GST_DEBUG_FUNCPTR (gst_rds_osd_stop);
  trans_class->transform_ip = GST_DEBUG_FUNCPTR (gst_rds_osd_transform_ip);
  trans_class->passthrough_on_same_caps = FALSE;
  trans_class->transform_ip_on_passthrough = FALSE;

  gst_type_mark_as_plugin_api (GST_TYPE_RDS_OSD_MODE, 0);
}
