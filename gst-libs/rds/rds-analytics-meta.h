/* Redstream: rdsanalytics' metadata (counterpart of DeepStream's analytics frame and object metas). Library
 * libgstrds-analytics-1.0 (pkg-config redstream-analytics-1.0).
 *
 * rdsanalytics attaches one RdsUserMeta per frame, base_meta.meta_type RDS_FRAME_META_ANALYTICS, whose
 * user_meta_data is an RdsAnalyticsFrameMeta, to frame_user_meta_list; and one per object that has something to
 * report, RDS_OBJ_META_ANALYTICS with an RdsAnalyticsObjInfo, to obj_user_meta_list. The two types are DeepStream
 * 8.0's numbers (32 and 33), which 8.0 also uses as its user meta types.
 *
 * The structs are C++ (std::string, std::vector, std::unordered_map) with DeepStream's documented field names and
 * types, so C++ reads them directly:
 *
 *   for (RdsMetaList *l = frame_meta->frame_user_meta_list; l != NULL; l = l->next) {
 *     RdsUserMeta *um = (RdsUserMeta *) l->data;
 *     if (um->base_meta.meta_type == RDS_FRAME_META_ANALYTICS) {
 *       RdsAnalyticsFrameMeta *m = (RdsAnalyticsFrameMeta *) um->user_meta_data;
 *       uint32_t in_roi = m->objInROIcnt["RF"];
 *     }
 *   }
 *
 * C sees them as opaque types and uses the functions below, which C++ can use too. No function lets an exception
 * out: a failure (a NULL argument, an allocation that fails) logs a critical and returns NULL, FALSE or 0.
 *
 * The metas are owned by their user meta: rds_batch_meta_copy (and rdsstreamdemux) copies them deeply, and releasing
 * the user meta frees them. A user meta whose copy failed has NULL user_meta_data. Strings returned by the getters
 * belong to the meta: valid until it is changed or released. Writes follow the batch meta's locking rules
 * (rds_acquire_meta_lock).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_ANALYTICS_META_H
#define RDS_ANALYTICS_META_H

#include <glib.h>
#include <rds/rds-prelude.h>
#include <rds/rds-meta.h>

#ifdef __cplusplus
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#endif

typedef struct RdsAnalyticsFrameMeta RdsAnalyticsFrameMeta;
typedef struct RdsAnalyticsObjInfo RdsAnalyticsObjInfo;

#ifdef __cplusplus
/* Per frame. The fields in DeepStream's documented order. */
struct RdsAnalyticsFrameMeta
{
  /* per overcrowding ROI label: overcrowded on this frame */
  std::unordered_map<std::string, bool> ocStatus;
  /* per ROI label (roi-filtering and overcrowding ROIs): the objects counted in it on this frame */
  std::unordered_map<std::string, uint32_t> objInROIcnt;
  /* per line-crossing label: the crossings counted on this frame */
  std::unordered_map<std::string, uint64_t> objLCCurrCnt;
  /* per line-crossing label: the crossings counted so far */
  std::unordered_map<std::string, uint64_t> objLCCumCnt;
  /* the rdsanalytics element's unique-id */
  guint unique_id = 0;
  /* per class_id: the objects on the frame (every class, whatever the class-id filters) */
  std::unordered_map<int, uint32_t> objCnt;
};

/* Per object. The fields in DeepStream's documented order. */
struct RdsAnalyticsObjInfo
{
  /* the labels of the ROIs the object is in (outside, with inverse-roi) */
  std::vector<std::string> roiStatus;
  /* the labels of the overcrowding ROIs the object is in */
  std::vector<std::string> ocStatus;
  /* the labels of the lines the object crossed on this frame */
  std::vector<std::string> lcStatus;
  /* the matched direction labels, concatenated */
  std::string dirStatus;
  /* the rdsanalytics element's unique-id */
  guint unique_id = 0;
  /* the object's statuses as one string (ROI, DIR, LC, STOPPED pieces) */
  std::string objStatus;
};
#endif

G_BEGIN_DECLS

/* ---- Frame meta ---- */

/* A new, empty frame meta that the caller owns (free it with rds_analytics_frame_meta_free). */
RDS_API RdsAnalyticsFrameMeta *rds_analytics_frame_meta_new (guint unique_id);
/* A deep copy that the caller owns. */
RDS_API RdsAnalyticsFrameMeta *rds_analytics_frame_meta_copy (const RdsAnalyticsFrameMeta * meta);
/* Frees a meta from _new or _copy (never one attached to a frame). NULL is ignored. */
RDS_API void rds_analytics_frame_meta_free (RdsAnalyticsFrameMeta * meta);
/* Creates an empty frame meta and adds it to frame_meta's frame_user_meta_list, as a user meta of type
 * RDS_FRAME_META_ANALYTICS from frame_meta's batch, with its copy and release functions. frame_meta must come from a
 * batch's pool. Returns the meta, owned by the user meta, for the setters. */
RDS_API RdsAnalyticsFrameMeta *rds_analytics_frame_meta_attach (RdsFrameMeta * frame_meta, guint unique_id);

/* Setters: each adds the entry or replaces its value. TRUE on success. */
RDS_API gboolean rds_analytics_frame_meta_set_roi_count (RdsAnalyticsFrameMeta * meta, const gchar * label,
    guint32 count);                             /* objInROIcnt */
RDS_API gboolean rds_analytics_frame_meta_set_oc_status (RdsAnalyticsFrameMeta * meta, const gchar * label,
    gboolean overcrowded);                      /* ocStatus */
RDS_API gboolean rds_analytics_frame_meta_set_lc_counts (RdsAnalyticsFrameMeta * meta, const gchar * label,
    guint64 current, guint64 cumulative);       /* objLCCurrCnt and objLCCumCnt */
RDS_API gboolean rds_analytics_frame_meta_set_obj_count (RdsAnalyticsFrameMeta * meta, gint class_id,
    guint32 count);                             /* objCnt */

/* Getters by key: TRUE and the value in *out (which may be NULL) if the key is there; FALSE, *out untouched, if
 * not. */
RDS_API gboolean rds_analytics_frame_meta_get_roi_count (const RdsAnalyticsFrameMeta * meta, const gchar * label,
    guint32 * count);
RDS_API gboolean rds_analytics_frame_meta_get_oc_status (const RdsAnalyticsFrameMeta * meta, const gchar * label,
    gboolean * overcrowded);
RDS_API gboolean rds_analytics_frame_meta_get_lc_count (const RdsAnalyticsFrameMeta * meta, const gchar * label,
    guint64 * count);                           /* objLCCurrCnt */
RDS_API gboolean rds_analytics_frame_meta_get_lc_cum_count (const RdsAnalyticsFrameMeta * meta,
    const gchar * label, guint64 * count);      /* objLCCumCnt */
RDS_API gboolean rds_analytics_frame_meta_get_obj_count (const RdsAnalyticsFrameMeta * meta, gint class_id,
    guint32 * count);
/* 0 (with a critical) for NULL. */
RDS_API guint rds_analytics_frame_meta_get_unique_id (const RdsAnalyticsFrameMeta * meta);

/* Callbacks of the foreach functions. They must not change the meta they iterate. */
typedef void (*RdsAnalyticsLabelCountFunc) (const gchar * label, guint64 count, gpointer user_data);
typedef void (*RdsAnalyticsLabelStatusFunc) (const gchar * label, gboolean status, gpointer user_data);
typedef void (*RdsAnalyticsClassCountFunc) (gint class_id, guint32 count, gpointer user_data);

/* Call func once per entry of a map, in no particular order (the map's). TRUE unless an argument is NULL. */
RDS_API gboolean rds_analytics_frame_meta_foreach_roi_count (const RdsAnalyticsFrameMeta * meta,
    RdsAnalyticsLabelCountFunc func, gpointer user_data);
RDS_API gboolean rds_analytics_frame_meta_foreach_oc_status (const RdsAnalyticsFrameMeta * meta,
    RdsAnalyticsLabelStatusFunc func, gpointer user_data);
RDS_API gboolean rds_analytics_frame_meta_foreach_lc_count (const RdsAnalyticsFrameMeta * meta,
    RdsAnalyticsLabelCountFunc func, gpointer user_data);
RDS_API gboolean rds_analytics_frame_meta_foreach_lc_cum_count (const RdsAnalyticsFrameMeta * meta,
    RdsAnalyticsLabelCountFunc func, gpointer user_data);
RDS_API gboolean rds_analytics_frame_meta_foreach_obj_count (const RdsAnalyticsFrameMeta * meta,
    RdsAnalyticsClassCountFunc func, gpointer user_data);

/* ---- Object info ---- */

/* As for the frame meta: caller-owned new, copy and free, and attach to obj_meta's obj_user_meta_list as a user
 * meta of type RDS_OBJ_META_ANALYTICS (obj_meta must come from a batch's pool). */
RDS_API RdsAnalyticsObjInfo *rds_analytics_obj_info_new (guint unique_id);
RDS_API RdsAnalyticsObjInfo *rds_analytics_obj_info_copy (const RdsAnalyticsObjInfo * info);
RDS_API void rds_analytics_obj_info_free (RdsAnalyticsObjInfo * info);
RDS_API RdsAnalyticsObjInfo *rds_analytics_obj_info_attach (RdsObjectMeta * obj_meta, guint unique_id);

/* Setters: _add_* appends a label to its list (a repeat is kept); _set_* replaces the string. TRUE on success. */
RDS_API gboolean rds_analytics_obj_info_add_roi_status (RdsAnalyticsObjInfo * info, const gchar * label);
RDS_API gboolean rds_analytics_obj_info_add_oc_status (RdsAnalyticsObjInfo * info, const gchar * label);
RDS_API gboolean rds_analytics_obj_info_add_lc_status (RdsAnalyticsObjInfo * info, const gchar * label);
RDS_API gboolean rds_analytics_obj_info_set_dir_status (RdsAnalyticsObjInfo * info, const gchar * status);
RDS_API gboolean rds_analytics_obj_info_set_obj_status (RdsAnalyticsObjInfo * info, const gchar * status);

/* Getters: the list lengths, and the i-th label (NULL past the end); the strings ("" when unset). NULL info: a
 * critical and 0 or NULL. */
RDS_API guint rds_analytics_obj_info_n_roi_status (const RdsAnalyticsObjInfo * info);
RDS_API const gchar *rds_analytics_obj_info_get_roi_status (const RdsAnalyticsObjInfo * info, guint i);
RDS_API guint rds_analytics_obj_info_n_oc_status (const RdsAnalyticsObjInfo * info);
RDS_API const gchar *rds_analytics_obj_info_get_oc_status (const RdsAnalyticsObjInfo * info, guint i);
RDS_API guint rds_analytics_obj_info_n_lc_status (const RdsAnalyticsObjInfo * info);
RDS_API const gchar *rds_analytics_obj_info_get_lc_status (const RdsAnalyticsObjInfo * info, guint i);
RDS_API const gchar *rds_analytics_obj_info_get_dir_status (const RdsAnalyticsObjInfo * info);
RDS_API const gchar *rds_analytics_obj_info_get_obj_status (const RdsAnalyticsObjInfo * info);
RDS_API guint rds_analytics_obj_info_get_unique_id (const RdsAnalyticsObjInfo * info);

G_END_DECLS

#endif /* RDS_ANALYTICS_META_H */
