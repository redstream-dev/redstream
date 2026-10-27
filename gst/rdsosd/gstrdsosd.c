/* Redstream: rdsosd, draws each frame's object meta, display meta and the clock onto its RGBA surface, on the GPU
 * (process-mode 1, the default, and 2: rds-osd-gpu) or on the CPU (process-mode 0: rds-osd-draw), the same bytes in
 * both: hard-edged shapes, Pango text. Property names, ranges and defaults follow DeepStream's nvdsosd as gst-inspect
 * shows it for 8.0; drawing behaviour the docs leave open matches DeepStream 8.0's default GPU mode ("DeepStream 8.0
 * behaviour") except where marked "ours". Every such choice has a row in docs/compat-matrix.md.
 *
 * In place: the base class hands transform_ip a writable buffer, and the WRITE map gives this element memory it owns
 * (a shared surface or a read-only surface view, as rdsstreamdemux pushes, is replaced by a private copy first, a
 * synchronous device copy; the batch it came from is never written). Each frame meta is drawn on its slot (batch_id).
 * GPU modes: the batch is planned under its meta lock (one plan: commands, text masks), then drawn pass by pass on
 * the element's own non-blocking stream, which each pass waits for; the frames never leave the GPU. CPU mode: each
 * slot is synced to the CPU, drawn and synced back, under the meta lock (synchronous syncs). Either way the frame is
 * ready when pushed. Frames with nothing to draw are not touched, and a buffer with nothing to draw is not mapped at
 * all (no copy of view memory). A GPU failure is an element error: the element never falls back to the CPU.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "gstrdsosd.h"
#include "rds-osd-draw.h"
#include "rds-osd-gpu.h"
#include "rds-osd-plan.h"

#include <rds/rds.h>

/* GLib first: HIP's headers define __noinline__, which breaks GLib if HIP comes first. */
#include <hip/hip_runtime_api.h>

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
  MODE_VIC = 2,                 /* ours: not in DeepStream 8.0's dGPU enum (Jetson's VIC); drawn on the GPU */
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
  /* accepted no-ops warn once each; process-mode=2 logs once */
  gboolean display_mask_warned;
  gboolean blur_bbox_warned;
  gboolean blur_ids_warned;
  gboolean vic_logged;

  /* streaming state (streaming thread, or stopped) */
  gint mode;                    /* process-mode at start */
  RdsOsdDraw *draw;             /* MODE_CPU */
  RdsOsdPlan *plan;             /* the GPU modes: its mask cache mirrors gpu's device heap, so it is gpu's alone */
  RdsOsdGpu *gpu;
  guint stream_gpu;             /* the GPU gpu is on */
  GArray *surfaces;             /* RdsOsdGpuSurface per slot of the batch being drawn */
  gboolean border_warned;
  gboolean slot_warned;
  /* test hooks: counts since start (streaming thread; read while idle), and a failure to inject */
  guint64 cpu_frames, gpu_passes;
  gint fail_next_pass;          /* atomic */
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
    {MODE_GPU, "GPU", "MODE_GPU"},
    {MODE_VIC, "VIC on Jetson; drawn on the GPU here", "MODE_VIC"},
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
  GError *error = NULL;
  gboolean log_vic;
  int prev = 0;

  GST_OBJECT_LOCK (self);
  self->mode = self->process_mode;
  self->stream_gpu = self->gpu_id;
  log_vic = self->mode == MODE_VIC && !self->vic_logged;
  self->vic_logged |= log_vic;
  GST_OBJECT_UNLOCK (self);

  self->border_warned = FALSE;
  self->slot_warned = FALSE;
  self->cpu_frames = 0;
  self->gpu_passes = 0;
  if (self->mode == MODE_CPU) {
    GST_INFO_OBJECT (self, "process-mode 0: drawing on the CPU");
    self->draw = rds_osd_draw_new ();
    rds_osd_draw_warm_up (self->draw);
    return TRUE;
  }

  /* ours: there is no VIC, so process-mode 2 draws on the GPU as 1 does (logged once per element) */
  if (log_vic)
    GST_WARNING_OBJECT (self, "process-mode=2 (VIC) is a Jetson unit: drawing on the GPU instead");
  if (hipGetDevice (&prev) != hipSuccess || hipSetDevice ((int) self->stream_gpu) != hipSuccess) {
    GST_ELEMENT_ERROR (self, RESOURCE, NOT_FOUND, ("GPU %u is not available", self->stream_gpu),
        ("process-mode %d draws on the GPU (process-mode=0 draws on the CPU)", self->mode));
    return FALSE;
  }
  self->gpu = rds_osd_gpu_new (RDS_OSD_MASK_HEAP_SIZE, &error);
  (void) hipSetDevice (prev);
  if (self->gpu == NULL) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot set up drawing on GPU %u", self->stream_gpu),
        ("%s", error->message));
    g_error_free (error);
    return FALSE;
  }
  /* a new plan: its mask cache starts empty, as the new device heap does */
  self->plan = rds_osd_plan_new (RDS_OSD_MASK_HEAP_SIZE);
  rds_osd_plan_warm_up (self->plan);
  self->surfaces = g_array_new (FALSE, TRUE, sizeof (RdsOsdGpuSurface));
  GST_INFO_OBJECT (self, "process-mode %d: drawing on GPU %u", self->mode, self->stream_gpu);
  return TRUE;
}

static void
free_streaming (GstRdsOsd * self)
{
  rds_osd_draw_free (self->draw);
  self->draw = NULL;
  /* waits for the stream */
  rds_osd_gpu_free (self->gpu);
  self->gpu = NULL;
  if (self->plan != NULL)
    rds_osd_plan_free (self->plan);
  self->plan = NULL;
  if (self->surfaces != NULL)
    g_array_unref (self->surfaces);
  self->surfaces = NULL;
}

static gboolean
gst_rds_osd_stop (GstBaseTransform * trans)
{
  free_streaming (GST_RDS_OSD (trans));
  return TRUE;
}

static gboolean
check_rgba (GstRdsOsd * self, const RdsBufSurface * surf, guint slot)
{
  if (surf->surfaceList[slot].colorFormat == RDS_BUF_COLOR_FORMAT_RGBA)
    return TRUE;
  GST_ELEMENT_ERROR (self, STREAM, FORMAT, ("rdsosd draws on RGBA surfaces only"),
      ("slot %u has colour format %d", slot, surf->surfaceList[slot].colorFormat));
  return FALSE;
}

static void
warn_border (GstRdsOsd * self, RdsOsdDrawFlags flags)
{
  if ((flags & RDS_OSD_DRAW_BORDER_CLAMPED) && !self->border_warned) {
    self->border_warned = TRUE;
    GST_ELEMENT_WARNING (self, STREAM, FAILED, ("A border_width above %d was drawn %d pixels wide",
            RDS_OSD_MAX_BORDER_WIDTH, RDS_OSD_MAX_BORDER_WIDTH), ("logged once"));
  }
}

/* Draws @fm (NULL: the clock only) onto slot @slot of @surf: map, sync to the CPU, draw, sync back. */
static gboolean
draw_slot (GstRdsOsd * self, RdsBufSurface * surf, guint slot, const RdsFrameMeta * fm,
    const RdsOsdDrawOptions * opt)
{
  RdsBufSurfaceParams *p = &surf->surfaceList[slot];
  gboolean ok;

  if (!check_rgba (self, surf, slot))
    return FALSE;
  self->cpu_frames++;
  if (rds_buf_surface_map (surf, (int) slot, 0, RDS_BUF_MAP_READ_WRITE) != 0) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map frame %u for drawing", slot),
        ("rds_buf_surface_map failed"));
    (void) rds_buf_surface_unmap (surf, (int) slot, 0);
    return FALSE;
  }
  ok = rds_buf_surface_sync_for_cpu (surf, (int) slot, 0) == 0;
  if (ok) {
    warn_border (self, rds_osd_draw_frame (self->draw, opt, fm, p->mappedAddr.addr[0], (gint) p->width,
            (gint) p->height, (gint) p->planeParams.pitch[0]));
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

/* Plans the frames of @work on @surf's slots for the GPU (under the meta lock: the plan reads the metas, and keeps
 * nothing of them). FALSE (an element error) when a slot cannot be drawn on the GPU. */
static gboolean
plan_gpu (GstRdsOsd * self, RdsBufSurface * surf, guint n_slots, GArray * work, const RdsOsdDrawOptions * opt)
{
  RdsOsdDrawFlags flags = 0;

  if (surf->memType == RDS_BUF_MEM_SYSTEM) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("process-mode %d draws on frames the GPU can reach", self->mode),
        ("the frames are in system memory"));
    return FALSE;
  }
  if ((surf->memType == RDS_BUF_MEM_DEFAULT || surf->memType == RDS_BUF_MEM_HIP_DEVICE)
      && surf->gpuId != self->stream_gpu) {
    GST_ELEMENT_ERROR (self, STREAM, FAILED, ("The frames are on GPU %u, gpu-id is %u", surf->gpuId,
            self->stream_gpu), (NULL));
    return FALSE;
  }
  g_array_set_size (self->surfaces, 0);
  g_array_set_size (self->surfaces, n_slots);
  rds_osd_plan_begin (self->plan);
  for (guint i = 0; i < work->len; i++) {
    Work *w = &g_array_index (work, Work, i);
    const RdsBufSurfaceParams *p = &surf->surfaceList[w->slot];

    if (!check_rgba (self, surf, w->slot))
      return FALSE;
    g_array_index (self->surfaces, RdsOsdGpuSurface, w->slot) = (RdsOsdGpuSurface) {
    .data = (guint8 *) p->dataPtr + p->planeParams.offset[0],.pitch = p->planeParams.pitch[0],.width =
          (gint32) p->width,.height = (gint32) p->height};
    flags |= rds_osd_plan_add_frame (self->plan, opt, w->slot, w->fm, (gint) p->width, (gint) p->height);
  }
  warn_border (self, flags);
  return TRUE;
}

/* Draws the planned batch on the GPU, pass by pass; each pass is complete when rds_osd_gpu_draw_pass returns. FALSE
 * (an element error) on a HIP failure. */
static gboolean
draw_gpu (GstRdsOsd * self)
{
  RdsOsdPass pass;
  GError *error = NULL;
  gboolean ok = TRUE;
  int prev = 0;

  if (hipGetDevice (&prev) != hipSuccess || hipSetDevice ((int) self->stream_gpu) != hipSuccess) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot draw on the GPU"), ("GPU %u cannot be selected",
            self->stream_gpu));
    return FALSE;
  }
  while (ok && rds_osd_plan_next_pass (self->plan, &pass)) {
    guint n_surfaces = self->surfaces->len;

    /* (tests) no plane for any slot: the pass fails as on a HIP error, its masks dropped */
    if (g_atomic_int_compare_and_exchange (&self->fail_next_pass, TRUE, FALSE))
      n_surfaces = 0;
    ok = rds_osd_gpu_draw_pass (self->gpu, self->plan, &pass, (const RdsOsdGpuSurface *) self->surfaces->data,
        n_surfaces, &error);
    /* a pass with neither commands nor uploads sends nothing to the GPU */
    if (ok && (pass.n_cmds > 0 || pass.n_uploads > 0))
      self->gpu_passes++;
  }
  (void) hipSetDevice (prev);
  if (!ok) {
    GST_ELEMENT_ERROR (self, RESOURCE, FAILED, ("Cannot draw on the GPU"), ("%s", error->message));
    g_error_free (error);
  }
  return ok;
}

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
    /* writable buffer + WRITE map: memory this element may write (a copy of shared or view memory, made by a
     * synchronous copy: complete before any drawing reads it) */
    if (!gst_buffer_map (buf, &map, GST_MAP_WRITE)) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED, ("Cannot map the buffer for drawing"),
          ("the input must be one writable memory:RDS memory"));
      ret = GST_FLOW_ERROR;
    } else {
      RdsBufSurface *surf = (RdsBufSurface *) map.data;

      if (self->mode == MODE_CPU) {
        for (guint i = 0; i < work->len && ret == GST_FLOW_OK; i++) {
          Work *w = &g_array_index (work, Work, i);

          if (!draw_slot (self, surf, w->slot, w->fm, &opt))
            ret = GST_FLOW_ERROR;
        }
      } else {
        if (!plan_gpu (self, surf, n_slots, work, &opt))
          ret = GST_FLOW_ERROR;
        /* the passes read the plan only, not the metas */
        if (bm != NULL)
          rds_release_meta_lock (bm);
        bm = NULL;
        if (ret == GST_FLOW_OK && !draw_gpu (self))
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
  free_streaming (self);
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
          "Where to draw: on the CPU, or on the GPU (the same pixels; 2, Jetson's VIC, draws on the GPU here)",
          GST_TYPE_RDS_OSD_MODE, DEFAULT_PROCESS_MODE, flags | GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_GPU_ID,
      g_param_spec_uint ("gpu-id", "GPU id",
          "GPU to draw on in the GPU modes (frames in device memory must be on it; the CPU mode ignores it)", 0,
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

/* ---- test hooks ---- */

void
rds_osd_test_get_counts (GstElement * element, guint64 * cpu_frames, guint64 * gpu_passes)
{
  GstRdsOsd *self = GST_RDS_OSD (element);

  *cpu_frames = self->cpu_frames;
  *gpu_passes = self->gpu_passes;
}

void
rds_osd_test_fail_next_pass (GstElement * element)
{
  g_atomic_int_set (&GST_RDS_OSD (element)->fail_next_pass, TRUE);
}
