/* Redstream: what GstRdsEncBin's files and its two codecs share (plugin-internal): the enum helper, the property ids,
 * the private instance struct and its locking rules, and the functions gstrdsencbin.c, gstrdsencbin-probes.c (the pad
 * probes), gstrdsencbin-sched.c (the probes around the queue) and gstrdsencbin-va.c (the VA encoder's configuration)
 * call in each other.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_RDS_ENC_BIN_PRIVATE_H
#define GST_RDS_ENC_BIN_PRIVATE_H

#include <gst/gst.h>

#include "gstrdsencbin.h"
#include "rds-enc-keyunits.h"

/* Defines `GType func (void)`, registering the enum @type_name with the GEnumValues given (without the terminating
 * entry) the first time it is called. Precede it with a static prototype for a file-local type. */
#define RDS_ENC_DEFINE_ENUM(func, type_name, ...)                             \
GType                                                                         \
func (void)                                                                   \
{                                                                             \
  static gsize id = 0;                                                        \
  static const GEnumValue values[] = { __VA_ARGS__, {0, NULL, NULL} };        \
                                                                              \
  if (g_once_init_enter (&id))                                                \
    g_once_init_leave (&id, g_enum_register_static (type_name, values));      \
  return (GType) id;                                                          \
}

GST_DEBUG_CATEGORY_EXTERN (gst_rds_enc_bin_debug);

/* Property ids, in gst_rds_enc_bin_class_setup's order. */
enum
{
  PROP_0,
  PROP_BITRATE,
  PROP_CONTROL_RATE,
  PROP_MAXBITRATE,
  PROP_CONSTQP,
  PROP_QP_RANGE,
  PROP_VBVBUFSIZE,
  PROP_IDRINTERVAL,
  PROP_IFRAMEINTERVAL,
  PROP_FORCE_IDR,
  PROP_FORCE_INTRA,
  PROP_PROFILE,
  PROP_NUM_B_FRAMES,
  PROP_NUM_REF_FRAMES,
  PROP_DISABLE_CABAC,
  PROP_INSERT_AUD,
  PROP_MIN_FORCE_KEY_UNIT_INTERVAL,
  PROP_QOS,
  PROP_COPY_META,
  PROP_GPU_ID,
  /* accepted no-ops */
  PROP_NOOP_FIRST,
  PROP_PRESET_ID = PROP_NOOP_FIRST,
  PROP_INSERT_SPS_PPS,
  PROP_INITQP,
  PROP_CQ,
  PROP_VBVINIT,
  PROP_AQ,
  PROP_TEMPORALAQ,
  PROP_INTRA_REFRESH,
  PROP_TUNING_INFO_ID,
  PROP_INSERT_VUI,
  PROP_EXTENDED_COLORFORMAT,
  PROP_SEI_UUID,
  PROP_CUDAENC_MEMTYPE,
  PROP_SLICE_HEADER_SPACING,
  PROP_BIT_PACKETIZATION,
  PROP_SLICE_INTRA_REFRESH_INTERVAL,
  PROP_ENABLE_LOSSLESS,
  PROP_CAPTURE_IO_MODE,
  PROP_OUTPUT_IO_MODE,
  PROP_EXTRA_CONTROLS,
  /* read-only */
  PROP_DEVICE,
  PROP_DEVICE_FD,
  PROP_DEVICE_NAME,
  N_PROPS,
};

#define IS_NOOP(id) ((id) >= PROP_NOOP_FIRST && (id) < PROP_DEVICE)

G_STATIC_ASSERT (PROP_DEVICE - PROP_NOOP_FIRST <= 32);

/* What the probes keep of a video caps (0: unknown). */
typedef struct
{
  gint width, height, fps_n, fps_d;
} VideoSize;

/* copy-meta: one input buffer's metas, kept on an empty buffer. */
typedef struct
{
  GstClockTime pts;
  GstBuffer *metas;
} MetaRecord;

/* copy-meta keeps an input frame's metas until its output, or until 64 newer frames came in. */
#define MAX_RECORD_AGE 64
/* frames the queue in front of the VA encoder holds at most */
#define VA_QUEUE_FRAMES 3u
/* input segments waiting for their output segment; more than this means VA dropped some */
#define MAX_PENDING_SEGMENTS 16

/* Locking: the object lock guards the property values and the inner-element pointers. Forward a property to an inner
 * element or send it an event after unlocking, never under the object lock: take a reference with get_encoder () (or
 * the like) and drop the lock first. va_lock serialises the live bitrate's forward to the VA encoder with the start's
 * configure (), so a value set during a start is not overwritten by the start's older snapshot: the forward takes
 * va_lock, reads the latest stored value under the object lock, and sets it with only va_lock held. force-idr and
 * force-intra skip va_lock: they queue a request under probe_lock, which the probes send to VA. Lock order: va_lock,
 * then the object lock. The probes never take va_lock (they run inside the VA encoder's own calls). configure () holds
 * va_lock while it sets the children's properties and posts nothing then, so don't set this bin's properties from its
 * children's notify / deep-notify or from a sync bus handler while it starts. The chain, and the probes on it, are made
 * once, in build_chain; per-run state (what the probes measure or warn about once) is reset at READY to PAUSED and
 * PAUSED to READY, not there.
 *
 * The stream probes (filler, timestamps, caps, batches, copy-meta, key-frame requests) run on the streaming threads
 * (upstream's at the ghost sink pad and the queue's sink pad, the queue's at the VA encoder's sink pad, VA's at its
 * src pad) and keep their state under probe_lock, a leaf lock: no element, pad or meta lock is taken while it is held
 * (metas are copied, and records and events freed, after it is dropped), and nothing is pushed, sent or posted under
 * it. They read b_frames_set under the object lock first, never both at once. */
struct _GstRdsEncBinPrivate
{
  GMutex va_lock;

  /* the inner chain (object lock), made at the first NULL to READY and kept until finalize (the bin owns them) */
  GstElement *convert;          /* rdsvideoconvert */
  GstElement *rawcaps;          /* video/x-raw,format=NV12 */
  GstElement *queue;            /* queue: the VA encoder's own streaming thread */
  GstElement *encoder;          /* vah264enc / vah265enc */
  GstElement *srccaps;          /* the codec's byte-stream caps */
  GstPad *sink_ghost, *src_ghost;
  /* the sizes the VA encoder takes on system memory, from its sink template (have_size_range FALSE: none known, any
   * size is left to it) */
  gboolean have_size_range;
  gint min_width, max_width, min_height, max_height;

  /* properties (object lock), indexed by property id */
  GValue values[N_PROPS];
  guint noops_warned;           /* bit per no-op property, from PROP_NOOP_FIRST */

  /* per run (object lock): set at READY to PAUSED, cleared at PAUSED to READY */
  const gchar *va_profile;      /* the profile property's VA caps profile */
  guint b_frames;               /* num-B-Frames read at the start */
  guint b_frames_set;           /* what the VA encoder's b-frames was last set to */
  gboolean b_frames_warned;     /* the Baseline + B frames warning was given */
  gboolean level_warned;        /* the downstream level warning was given */
  gboolean size_warned;         /* the input size warning was given */

  /* the stream probes' state (probe_lock), cleared at READY to PAUSED and PAUSED to READY */
  GMutex probe_lock;
  gboolean copy_meta;           /* copy-meta read at the start */
  GQueue in_segments;           /* GstSegment *: TIME segments the VA encoder received and hasn't pushed yet */
  GstSegment last_in_segment;   /* the latest one, for an output segment without its own (format UNDEFINED: none) */
  guint32 out_seqnum;           /* the seqnum of VA's last output segment (GST_SEQNUM_INVALID: none) */
  guint64 out_start;            /* and its start, as VA sent it */
  GstClockTime va_shift;        /* VA's timestamp shift on the current output segment */
  GstClockTime shift;           /* what the src probe removes: va_shift without B frames, else 0 */
  VideoSize in_size;            /* the VA encoder's input caps */
  GstClockTime in_since;        /* input PTS of the first frame with in_size (NONE: none yet, or out_size is it) */
  VideoSize out_size;           /* the input caps of the frames now leaving it */
  GstCaps *out_caps;            /* the last caps that left the VA encoder, as they left it */
  GstCaps *va_caps;             /* the last caps the VA encoder sent, before the framerate rewrite */
  guint hevc_extra_bits;        /* num_extra_slice_header_bits of the last PPS */
  const gchar *level;           /* the downstream level written into the parameter sets (a static string; NULL: VA's
                                 * own), set by each CAPS event out of the VA encoder */
  VideoSize copy_size;          /* the run's first allocation size: the convert writes plain memory at it only */
  GQueue records;               /* MetaRecord *, copy-meta: the input metas by PTS, oldest first */
  RdsEncKeyUnits key_units;     /* requests for a key frame on the next frame (gst_rds_enc_bin_request_key_unit) */
};

typedef struct _GstRdsEncBinPrivate GstRdsEncBinPrivate;

G_GNUC_INTERNAL GstRdsEncBinPrivate *gst_rds_enc_bin_get_priv (GstRdsEncBin * self);
#define PRIV(self) gst_rds_enc_bin_get_priv (self)

/* gstrdsencbin-va.c */
/* Reads the properties for a start (READY to PAUSED) and sets them on the inner elements. FALSE (with an element
 * error): a setting the VA encoder can't do. */
G_GNUC_INTERNAL gboolean gst_rds_enc_bin_configure (GstRdsEncBin * self);
/* Sets @o's uint @name to @v, clamped to its range; nothing when @o has no such property. */
G_GNUC_INTERNAL void gst_rds_enc_bin_set_va_uint (GObject * o, const gchar * name, guint v);

/* gstrdsencbin-sched.c */
/* Adds the probes around the queue (the key-frame requests, the convert's pool); from gst_rds_enc_bin_add_probes. */
G_GNUC_INTERNAL void gst_rds_enc_bin_add_sched_probes (GstRdsEncBin * self, GstElement * queue,
    GstElement * encoder);
/* From the VA encoder's sink buffer probe (@va_sink, its pad), for the frame @buf it is about to take: sends the
 * key-frame requests given to that frame into VA's src pad. */
G_GNUC_INTERNAL void gst_rds_enc_bin_send_key_units (GstRdsEncBin * self, GstPad * va_sink, GstBuffer * buf);

/* gstrdsencbin-probes.c */
/* Adds every probe to the inner chain around @queue and @encoder and to the ghost pads; once, from build_chain. */
G_GNUC_INTERNAL void gst_rds_enc_bin_add_probes (GstRdsEncBin * self, GstElement * queue, GstElement * encoder);
/* Asks for a key frame on the next frame into the queue in front of the VA encoder: @event (taken), an upstream
 * force-key-unit event without a running time, is sent into the VA encoder's src pad just before that frame reaches
 * VA. Called from any thread; never blocks. */
G_GNUC_INTERNAL void gst_rds_enc_bin_request_key_unit (GstRdsEncBin * self, GstEvent * event);
/* Clears the stream probes' state for a new run (also at finalize), with @copy_meta for it. */
G_GNUC_INTERNAL void gst_rds_enc_bin_reset_probes (GstRdsEncBin * self, gboolean copy_meta);

#endif /* GST_RDS_ENC_BIN_PRIVATE_H */
