/* Redstream: DeepStream-shaped analytics metadata (batch -> frame -> object -> classifier -> label, plus display
 * and user meta), allocated from per-batch pools. Field names and semantics follow NVIDIA's public NvDs metadata
 * reference so traversal code ports by renaming.
 *
 * Ownership: text_params.display_text, mask_params.data and pResult_label are g_malloc'd and freed on release;
 * font_params.font_name and classifier_type are not owned. A user meta's release_func frees user_meta_data.
 * A user meta's release_func may run while the batch is being torn down. Adding or removing metas from it is
 * unsupported. It is tolerated for the batch-level lists (frames and batch user meta: destroy keeps releasing
 * them, for at most 16 passes); adds to the frame or object being released are not supported and may leak.
 * Every add/remove takes the batch's recursive meta_mutex; hold rds_acquire_meta_lock() around traversals that
 * race with other threads.
 *
 * Metas must come from the rds_acquire_*_from_pool functions; a meta allocated any other way is refused. A meta
 * can be in one list at a time: adding it again, or to another frame or batch, is refused with a GST_WARNING.
 * Removing releases the meta, so when removing during a traversal save l->next first:
 *
 *   for (l = frame->obj_meta_list; l != NULL; l = next) {
 *     next = l->next;
 *     if (drop (l->data))
 *       rds_remove_obj_meta_from_frame (frame, l->data);
 *   }
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_META_H
#define RDS_META_H

#include <glib.h>
#include <rds/rds-prelude.h>
#include <rds/rds-osd-types.h>

G_BEGIN_DECLS

#define RDS_MAX_LABEL_SIZE 128
#define RDS_MAX_ELEMENTS_IN_DISPLAY_META 16
#define RDS_MAX_USER_FIELDS 4
#define RDS_MAX_RESERVED_FIELDS 4
#define RDS_UNTRACKED_OBJECT_ID G_MAXUINT64

typedef GList RdsMetaList;
typedef RdsMetaList RdsFrameMetaList;
typedef RdsMetaList RdsObjectMetaList;
typedef RdsMetaList RdsClassifierMetaList;
typedef RdsMetaList RdsLabelInfoList;
typedef RdsMetaList RdsDisplayMetaList;
typedef RdsMetaList RdsUserMetaList;

typedef enum
{
  RDS_INVALID_META = -1,
  RDS_BATCH_META = 1,
  RDS_FRAME_META,
  RDS_OBJ_META,
  RDS_DISPLAY_META,
  RDS_CLASSIFIER_META,
  RDS_LABEL_INFO_META,
  RDS_USER_META,
  /* 8 .. 4095: reserved for Redstream components. The ones DeepStream defines keep their documented values. */
  RDS_INFER_TENSOR_OUTPUT_META = 12,                /* RdsInferTensorMeta (rds-infer-meta.h) in a user meta */
  /* the tracker's misc meta types (not emitted by rdstracker yet) */
  RDS_TRACKER_PAST_FRAME_META = 15,
  RDS_TRACKER_BATCH_REID_META,
  RDS_TRACKER_OBJ_REID_META,
  RDS_TRACKER_TERMINATED_LIST_META,
  RDS_TRACKER_SHADOW_LIST_META,
  RDS_OBJ_VISIBILITY,
  RDS_OBJ_IMAGE_FOOT_LOCATION,
  RDS_OBJ_WORLD_FOOT_LOCATION,
  RDS_OBJ_IMAGE_CONVEX_HULL,
  RDS_RESERVED_META = 4095,
  RDS_GST_CUSTOM_META = 4096,
  RDS_BATCH_GST_META,                               /* RdsMeta wrapping an RdsBatchMeta (rds-gst-meta.h) */
  RDS_START_USER_META = RDS_GST_CUSTOM_META + 4096 + 1,
  RDS_FORCE32_META = 0x7FFFFFFF,
} RdsMetaType;

/* The state of a tracked target, as tracker meta reports it. */
typedef enum
{
  RDS_TRACKER_STATE_EMPTY = 0,
  RDS_TRACKER_STATE_ACTIVE = 1,
  RDS_TRACKER_STATE_INACTIVE = 2,
  RDS_TRACKER_STATE_TENTATIVE = 3,
  RDS_TRACKER_STATE_PROJECTED = 4,
  RDS_TRACKER_STATE_QUASIACTIVE = 5,
} RdsTrackerState;

typedef struct _RdsBatchMeta RdsBatchMeta;
typedef struct _RdsMetaPool RdsMetaPool;            /* opaque */

/* For user meta: data is the RdsUserMeta, user_data is its base_meta.uContext. copy returns the new
 * user_meta_data (NULL gives a copied user meta with NULL data). copy runs with both batches' locks held: it must
 * not block on locks the pipeline may hold while it waits for metadata. */
typedef gpointer (*RdsMetaCopyFunc) (gpointer data, gpointer user_data);
typedef void (*RdsMetaReleaseFunc) (gpointer data, gpointer user_data);

typedef struct
{
  RdsBatchMeta *batch_meta;
  RdsMetaType meta_type;
  gpointer uContext;
  RdsMetaCopyFunc copy_func;
  RdsMetaReleaseFunc release_func;
} RdsBaseMeta;

typedef struct
{
  gfloat left, top, width, height;
} RdsBbox_Coords;

typedef struct
{
  RdsBbox_Coords org_bbox_coords;
} RdsComp_BboxInfo;

typedef struct
{
  RdsBaseMeta base_meta;
  gpointer user_meta_data;
} RdsUserMeta;

typedef struct
{
  RdsBaseMeta base_meta;
  guint num_classes;
  gchar result_label[RDS_MAX_LABEL_SIZE];
  gchar *pResult_label;                             /* owned; for labels longer than result_label */
  guint result_class_id;
  guint label_id;
  gfloat result_prob;
} RdsLabelInfo;

typedef struct
{
  RdsBaseMeta base_meta;
  guint num_labels;
  gint unique_component_id;
  RdsLabelInfoList *label_info_list;
  const gchar *classifier_type;                     /* not owned */
} RdsClassifierMeta;

typedef struct _RdsObjectMeta
{
  RdsBaseMeta base_meta;
  struct _RdsObjectMeta *parent;
  gint unique_component_id;
  gint class_id;
  guint64 object_id;
  RdsComp_BboxInfo detector_bbox_info;
  RdsComp_BboxInfo tracker_bbox_info;
  gfloat confidence;
  gfloat tracker_confidence;
  RdsOSD_RectParams rect_params;
  RdsOSD_MaskParams mask_params;
  RdsOSD_TextParams text_params;
  gchar obj_label[RDS_MAX_LABEL_SIZE];
  RdsClassifierMetaList *classifier_meta_list;
  RdsUserMetaList *obj_user_meta_list;
  gint64 misc_obj_info[RDS_MAX_USER_FIELDS];
  gint64 reserved[RDS_MAX_RESERVED_FIELDS];
} RdsObjectMeta;

typedef struct
{
  RdsBaseMeta base_meta;
  guint num_rects;
  guint num_labels;
  guint num_lines;
  guint num_arrows;
  guint num_circles;
  RdsOSD_RectParams rect_params[RDS_MAX_ELEMENTS_IN_DISPLAY_META];
  RdsOSD_TextParams text_params[RDS_MAX_ELEMENTS_IN_DISPLAY_META];
  RdsOSD_LineParams line_params[RDS_MAX_ELEMENTS_IN_DISPLAY_META];
  RdsOSD_ArrowParams arrow_params[RDS_MAX_ELEMENTS_IN_DISPLAY_META];
  RdsOSD_CircleParams circle_params[RDS_MAX_ELEMENTS_IN_DISPLAY_META];
  gint64 misc_osd_data[RDS_MAX_USER_FIELDS];
  gint64 reserved[RDS_MAX_RESERVED_FIELDS];
} RdsDisplayMeta;

typedef struct
{
  RdsBaseMeta base_meta;
  guint pad_index;
  guint batch_id;
  gint frame_num;
  guint64 buf_pts;
  guint64 ntp_timestamp;
  guint source_id;
  gint num_surfaces_per_frame;
  guint source_frame_width;
  guint source_frame_height;
  guint surface_type;
  guint surface_index;
  guint num_obj_meta;
  gboolean bInferDone;
  RdsObjectMetaList *obj_meta_list;
  RdsDisplayMetaList *display_meta_list;
  RdsUserMetaList *frame_user_meta_list;
  gint64 misc_frame_info[RDS_MAX_USER_FIELDS];
  guint pipeline_width;
  guint pipeline_height;
  gint64 reserved[RDS_MAX_RESERVED_FIELDS];
} RdsFrameMeta;

/* Always created by rds_create_batch_meta and freed by rds_destroy_batch_meta: never embed it by value or
 * allocate it yourself (private state follows it in memory). */
struct _RdsBatchMeta
{
  RdsBaseMeta base_meta;
  guint max_frames_in_batch;
  guint num_frames_in_batch;
  RdsMetaPool *frame_meta_pool;
  RdsMetaPool *obj_meta_pool;
  RdsMetaPool *classifier_meta_pool;
  RdsMetaPool *display_meta_pool;
  RdsMetaPool *user_meta_pool;
  RdsMetaPool *label_info_meta_pool;
  RdsFrameMetaList *frame_meta_list;
  RdsUserMetaList *batch_user_meta_list;
  GRecMutex meta_mutex;
  gint64 misc_batch_info[RDS_MAX_USER_FIELDS];
  gint64 reserved[RDS_MAX_RESERVED_FIELDS];
};

RDS_API RdsBatchMeta *rds_create_batch_meta (guint max_batch_size);
/* Releases everything in the batch, then every meta acquired from it but never added (with its owned members
 * and children; user metas get their release_func), and frees the pools. NULL is ignored. */
RDS_API void rds_destroy_batch_meta (RdsBatchMeta * batch_meta);
RDS_API void rds_acquire_meta_lock (RdsBatchMeta * batch_meta);
RDS_API void rds_release_meta_lock (RdsBatchMeta * batch_meta);

RDS_API RdsFrameMeta *rds_acquire_frame_meta_from_pool (RdsBatchMeta * batch_meta);
/* Adding more than max_frames_in_batch frames is allowed (logged at debug level). */
RDS_API void rds_add_frame_meta_to_batch (RdsBatchMeta * batch_meta, RdsFrameMeta * frame_meta);
/* Releases the frame with its objects, display and user meta. Objects in other frames whose parent was one of
 * them get parent = NULL. */
RDS_API void rds_remove_frame_meta_from_batch (RdsBatchMeta * batch_meta, RdsFrameMeta * frame_meta);

RDS_API RdsObjectMeta *rds_acquire_obj_meta_from_pool (RdsBatchMeta * batch_meta);
/* parent may be NULL. A parent that is obj_meta itself or from another batch is dropped (parent = NULL) with a
 * GST_WARNING. Set the parent here: assigning obj_meta->parent directly after the add is unsupported, because
 * removals would not know to clear it and it could dangle. Frames that were never added to the batch are not
 * scanned either when a parent is removed (except the frame passed to the remove call). */
RDS_API void rds_add_obj_meta_to_frame (RdsFrameMeta * frame_meta, RdsObjectMeta * obj_meta,
    RdsObjectMeta * obj_parent);
/* Releases the object and everything hanging off it (classifiers, user meta, owned strings and mask). Objects
 * whose parent it was get parent = NULL: in this frame and in every frame of the batch. */
RDS_API void rds_remove_obj_meta_from_frame (RdsFrameMeta * frame_meta, RdsObjectMeta * obj_meta);

/* Classifier, label, display and user meta follow the same rules as objects: the meta must come from the owner's
 * batch and be in no other list, and removing releases it with everything hanging off it. */
RDS_API RdsClassifierMeta *rds_acquire_classifier_meta_from_pool (RdsBatchMeta * batch_meta);
RDS_API void rds_add_classifier_meta_to_object (RdsObjectMeta * obj_meta, RdsClassifierMeta * classifier_meta);
RDS_API void rds_remove_classifier_meta_from_obj (RdsObjectMeta * obj_meta,
    RdsClassifierMeta * classifier_meta);

/* Adding and removing labels updates classifier_meta->num_labels. */
RDS_API RdsLabelInfo *rds_acquire_label_info_meta_from_pool (RdsBatchMeta * batch_meta);
RDS_API void rds_add_label_info_meta_to_classifier (RdsClassifierMeta * classifier_meta,
    RdsLabelInfo * label_info_meta);
RDS_API void rds_remove_label_info_meta_from_classifier (RdsClassifierMeta * classifier_meta,
    RdsLabelInfo * label_info_meta);

/* Releasing a display meta frees every non-NULL text_params[i].display_text, whatever num_labels says. */
RDS_API RdsDisplayMeta *rds_acquire_display_meta_from_pool (RdsBatchMeta * batch_meta);
RDS_API void rds_add_display_meta_to_frame (RdsFrameMeta * frame_meta, RdsDisplayMeta * display_meta);
RDS_API void rds_remove_display_meta_from_frame (RdsFrameMeta * frame_meta, RdsDisplayMeta * display_meta);

/* The caller sets base_meta.meta_type (usually from rds_get_user_meta_type), user_meta_data, and
 * base_meta.release_func (and copy_func, see rds_batch_meta_copy) before adding. */
RDS_API RdsUserMeta *rds_acquire_user_meta_from_pool (RdsBatchMeta * batch_meta);
RDS_API void rds_add_user_meta_to_batch (RdsBatchMeta * batch_meta, RdsUserMeta * user_meta);
RDS_API void rds_add_user_meta_to_frame (RdsFrameMeta * frame_meta, RdsUserMeta * user_meta);
RDS_API void rds_add_user_meta_to_obj (RdsObjectMeta * obj_meta, RdsUserMeta * user_meta);
RDS_API void rds_remove_user_meta_from_batch (RdsBatchMeta * batch_meta, RdsUserMeta * user_meta);
RDS_API void rds_remove_user_meta_from_frame (RdsFrameMeta * frame_meta, RdsUserMeta * user_meta);
RDS_API void rds_remove_user_meta_from_object (RdsObjectMeta * obj_meta, RdsUserMeta * user_meta);

/* Process-wide: the same descriptor (e.g. "EXAMPLE.MYAPP.USER_META") always yields the same type,
 * >= RDS_START_USER_META. Thread-safe. */
RDS_API RdsMetaType rds_get_user_meta_type (const gchar * meta_descriptor);

/* Deep copy through the destination's pools: strings, masks, classifiers, labels and display meta are duplicated,
 * object parent pointers are remapped to the copies, also across frames (NULL if the parent is in no list), and
 * user meta is copied with its copy_func (user meta without one is skipped, with a one-time warning). For
 * metas other than user meta, base_meta.uContext, copy_func and release_func are not carried over. Takes src's
 * lock, then the destination's. Returns NULL if src is NULL. */
RDS_API RdsBatchMeta *rds_batch_meta_copy (RdsBatchMeta * src);
/* Deep-copies one frame into dst and appends it there (rdsstreamdemux's per-source subset). Parents are remapped
 * within the frame only: a parent in another frame, or in no list, becomes NULL. Scalar fields, including
 * batch_id and pad_index, are kept from the source; the caller sets batch_id for its position in dst. Same
 * copy rules as rds_batch_meta_copy otherwise. Returns the copy, or NULL if src does not belong to a batch.
 * Locks src's batch, then dst: never call it with dst's lock held unless you also hold src's. */
RDS_API RdsFrameMeta *rds_copy_frame_meta_to_batch (RdsFrameMeta * src, RdsBatchMeta * dst);

G_END_DECLS

#endif /* RDS_META_H */
