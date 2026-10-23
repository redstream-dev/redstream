/* Redstream: DeepStream-shaped analytics metadata (batch -> frame -> object -> classifier -> label, plus display
 * and user meta), allocated from per-batch pools. Field names and semantics follow NVIDIA's public NvDs metadata
 * reference so traversal code ports by renaming.
 *
 * Ownership: text_params.display_text, mask_params.data and pResult_label are g_malloc'd and freed on release;
 * font_params.font_name and classifier_type are not owned. A user meta's release_func frees user_meta_data.
 * Every add/remove takes the batch's recursive meta_mutex; hold rds_acquire_meta_lock() around traversals that
 * race with other threads.
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
  /* 8 .. 4095: reserved for Redstream components (inference tensors in Phase 5, analytics in M2). */
  RDS_RESERVED_META = 4095,
  RDS_GST_CUSTOM_META = 4096,
  RDS_BATCH_GST_META,                               /* RdsMeta wrapping an RdsBatchMeta (rds-gst-meta.h) */
  RDS_START_USER_META = RDS_GST_CUSTOM_META + 4096 + 1,
  RDS_FORCE32_META = 0x7FFFFFFF,
} RdsMetaType;

typedef struct _RdsBatchMeta RdsBatchMeta;
typedef struct _RdsMetaPool RdsMetaPool;            /* opaque */

/* For user meta: data is the RdsUserMeta, user_data is its base_meta.uContext. copy returns new user_meta_data. */
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
/* Releases everything in the batch and frees the pools, including metas acquired but never added (their owned
 * members are not freed). NULL is ignored. */
RDS_API void rds_destroy_batch_meta (RdsBatchMeta * batch_meta);
RDS_API void rds_acquire_meta_lock (RdsBatchMeta * batch_meta);
RDS_API void rds_release_meta_lock (RdsBatchMeta * batch_meta);

RDS_API RdsFrameMeta *rds_acquire_frame_meta_from_pool (RdsBatchMeta * batch_meta);
RDS_API void rds_add_frame_meta_to_batch (RdsBatchMeta * batch_meta, RdsFrameMeta * frame_meta);
/* Releases the frame with its objects, display and user meta. Objects in other frames whose parent was one of
 * them get parent = NULL. */
RDS_API void rds_remove_frame_meta_from_batch (RdsBatchMeta * batch_meta, RdsFrameMeta * frame_meta);

RDS_API RdsObjectMeta *rds_acquire_obj_meta_from_pool (RdsBatchMeta * batch_meta);
/* parent may be NULL. */
RDS_API void rds_add_obj_meta_to_frame (RdsFrameMeta * frame_meta, RdsObjectMeta * obj_meta,
    RdsObjectMeta * obj_parent);
/* Releases the object and everything hanging off it (classifiers, user meta, owned strings and mask). Objects
 * whose parent it was get parent = NULL: in this frame and in every frame of the batch. */
RDS_API void rds_remove_obj_meta_from_frame (RdsFrameMeta * frame_meta, RdsObjectMeta * obj_meta);

G_END_DECLS

#endif /* RDS_META_H */
