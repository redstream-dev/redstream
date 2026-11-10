/* Redstream: rdsanalytics' metadata: create, copy and release, and the C API over the C++ structs.
 * Every C-callable function catches at its boundary: no exception reaches C (ours).
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-analytics-meta.h"

#include <exception>
#include <utility>

namespace
{

/* Runs f; on an exception logs a critical naming the function and returns fail. */
template <typename R, typename F>
R
guarded (const char *what, R fail, F && f) noexcept
{
  try {
    return f ();
  }
  catch (const std::exception & e) {
    g_critical ("%s: %s", what, e.what ());
  }
  catch (...) {
    g_critical ("%s: unknown exception", what);
  }
  return fail;
}

template <typename T>
T *
new_meta (const char *what, guint unique_id) noexcept
{
  return guarded<T *> (what, nullptr, [&] {
        T *meta = new T ();
        meta->unique_id = unique_id;
        return meta;
      });
}

template <typename T>
T *
copy_meta (const char *what, const T * src) noexcept
{
  return guarded<T *> (what, nullptr, [&] { return new T (*src); });
}

/* The user meta's copy and release functions: data is the RdsUserMeta. A failed copy gives NULL data, which the
 * release function accepts. */
template <typename T>
gpointer
user_copy (gpointer data, gpointer) noexcept
{
  const T *src = static_cast<const T *> (static_cast<RdsUserMeta *> (data)->user_meta_data);

  if (src == nullptr)
    return nullptr;
  return copy_meta ("analytics meta copy", src);
}

template <typename T>
void
user_release (gpointer data, gpointer) noexcept
{
  RdsUserMeta *um = static_cast<RdsUserMeta *> (data);

  delete static_cast<T *> (um->user_meta_data);
  um->user_meta_data = nullptr;
}

/* Wraps meta in a user meta of the given type from batch_meta; NULL (meta freed) on failure. */
template <typename T>
RdsUserMeta *
wrap (RdsBatchMeta * batch_meta, RdsMetaType type, T * meta) noexcept
{
  RdsUserMeta *um = rds_acquire_user_meta_from_pool (batch_meta);

  if (um == nullptr) {
    delete meta;
    return nullptr;
  }
  um->base_meta.meta_type = type;
  um->base_meta.uContext = nullptr;
  um->base_meta.copy_func = user_copy<T>;
  um->base_meta.release_func = user_release<T>;
  um->user_meta_data = meta;
  return um;
}

/* Runs add and checks that um landed in *list, both under the batch's (recursive) meta lock. A refused add (logged
 * by libgstrds) leaves the user meta acquired, released with the batch: not ours to free. */
template <typename F>
bool
add_checked (RdsBatchMeta * batch_meta, RdsUserMetaList ** list, RdsUserMeta * um, F && add) noexcept
{
  rds_acquire_meta_lock (batch_meta);
  add ();
  bool added = g_list_find (*list, um) != NULL;
  rds_release_meta_lock (batch_meta);
  return added;
}

template <typename Map>
bool
lookup (const Map & map, const typename Map::key_type & key, typename Map::mapped_type * out)
{
  auto it = map.find (key);

  if (it == map.end ())
    return false;
  if (out != nullptr)
    *out = it->second;
  return true;
}

const gchar *
at (const std::vector<std::string> &list, guint i)
{
  return i < list.size () ? list[i].c_str () : nullptr;
}

}

/* ---- Frame meta ---- */

RdsAnalyticsFrameMeta *
rds_analytics_frame_meta_new (guint unique_id)
{
  return new_meta<RdsAnalyticsFrameMeta> (G_STRFUNC, unique_id);
}

RdsAnalyticsFrameMeta *
rds_analytics_frame_meta_copy (const RdsAnalyticsFrameMeta * meta)
{
  g_return_val_if_fail (meta != NULL, NULL);
  return copy_meta (G_STRFUNC, meta);
}

void
rds_analytics_frame_meta_free (RdsAnalyticsFrameMeta * meta)
{
  delete meta;
}

RdsAnalyticsFrameMeta *
rds_analytics_frame_meta_attach (RdsFrameMeta * frame_meta, guint unique_id)
{
  g_return_val_if_fail (frame_meta != NULL, NULL);
  g_return_val_if_fail (frame_meta->base_meta.batch_meta != NULL, NULL);

  RdsAnalyticsFrameMeta *meta = rds_analytics_frame_meta_new (unique_id);
  if (meta == nullptr)
    return nullptr;
  RdsUserMeta *um = wrap (frame_meta->base_meta.batch_meta, RDS_FRAME_META_ANALYTICS, meta);
  if (um == nullptr)
    return nullptr;
  if (!add_checked (frame_meta->base_meta.batch_meta, &frame_meta->frame_user_meta_list, um,
          [&] { rds_add_user_meta_to_frame (frame_meta, um); })) {
    g_critical ("%s: the user meta could not be added to frame meta %p", G_STRFUNC, frame_meta);
    return nullptr;
  }
  return meta;
}

gboolean
rds_analytics_frame_meta_set_roi_count (RdsAnalyticsFrameMeta * meta, const gchar * label, guint32 count)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        meta->objInROIcnt[label] = count;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_set_oc_status (RdsAnalyticsFrameMeta * meta, const gchar * label, gboolean overcrowded)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        meta->ocStatus[label] = overcrowded != FALSE;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_set_lc_counts (RdsAnalyticsFrameMeta * meta, const gchar * label, guint64 current,
    guint64 cumulative)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        std::string key (label);

        /* both or neither: a failed second insert leaves the maps as they were */
        auto curr = meta->objLCCurrCnt.find (key);
        auto cum = meta->objLCCumCnt.find (key);
        bool new_curr = curr == meta->objLCCurrCnt.end ();
        if (new_curr)
          curr = meta->objLCCurrCnt.emplace (key, 0).first;
        try {
          if (cum == meta->objLCCumCnt.end ())
            cum = meta->objLCCumCnt.emplace (key, 0).first;
        }
        catch (...) {
          if (new_curr)
            meta->objLCCurrCnt.erase (curr);
          throw;
        }
        curr->second = current;
        cum->second = cumulative;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_set_obj_count (RdsAnalyticsFrameMeta * meta, gint class_id, guint32 count)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        meta->objCnt[class_id] = count;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_get_roi_count (const RdsAnalyticsFrameMeta * meta, const gchar * label, guint32 * count)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        uint32_t v;
        if (!lookup (meta->objInROIcnt, label, &v))
          return FALSE;
        if (count != NULL)
          *count = v;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_get_oc_status (const RdsAnalyticsFrameMeta * meta, const gchar * label,
    gboolean * overcrowded)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        bool v;
        if (!lookup (meta->ocStatus, label, &v))
          return FALSE;
        if (overcrowded != NULL)
          *overcrowded = v ? TRUE : FALSE;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_get_lc_count (const RdsAnalyticsFrameMeta * meta, const gchar * label, guint64 * count)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        uint64_t v;
        if (!lookup (meta->objLCCurrCnt, label, &v))
          return FALSE;
        if (count != NULL)
          *count = v;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_get_lc_cum_count (const RdsAnalyticsFrameMeta * meta, const gchar * label,
    guint64 * count)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        uint64_t v;
        if (!lookup (meta->objLCCumCnt, label, &v))
          return FALSE;
        if (count != NULL)
          *count = v;
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_get_obj_count (const RdsAnalyticsFrameMeta * meta, gint class_id, guint32 * count)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        uint32_t v;
        if (!lookup (meta->objCnt, class_id, &v))
          return FALSE;
        if (count != NULL)
          *count = v;
        return TRUE;
      });
}

guint
rds_analytics_frame_meta_get_unique_id (const RdsAnalyticsFrameMeta * meta)
{
  g_return_val_if_fail (meta != NULL, 0);
  return meta->unique_id;
}

gboolean
rds_analytics_frame_meta_foreach_roi_count (const RdsAnalyticsFrameMeta * meta, RdsAnalyticsLabelCountFunc func,
    gpointer user_data)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (func != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        for (const auto &kv : meta->objInROIcnt)
          func (kv.first.c_str (), kv.second, user_data);
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_foreach_oc_status (const RdsAnalyticsFrameMeta * meta, RdsAnalyticsLabelStatusFunc func,
    gpointer user_data)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (func != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        for (const auto &kv : meta->ocStatus)
          func (kv.first.c_str (), kv.second ? TRUE : FALSE, user_data);
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_foreach_lc_count (const RdsAnalyticsFrameMeta * meta, RdsAnalyticsLabelCountFunc func,
    gpointer user_data)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (func != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        for (const auto &kv : meta->objLCCurrCnt)
          func (kv.first.c_str (), kv.second, user_data);
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_foreach_lc_cum_count (const RdsAnalyticsFrameMeta * meta,
    RdsAnalyticsLabelCountFunc func, gpointer user_data)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (func != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        for (const auto &kv : meta->objLCCumCnt)
          func (kv.first.c_str (), kv.second, user_data);
        return TRUE;
      });
}

gboolean
rds_analytics_frame_meta_foreach_obj_count (const RdsAnalyticsFrameMeta * meta, RdsAnalyticsClassCountFunc func,
    gpointer user_data)
{
  g_return_val_if_fail (meta != NULL, FALSE);
  g_return_val_if_fail (func != NULL, FALSE);
  return guarded<gboolean> (G_STRFUNC, FALSE, [&] () -> gboolean {
        for (const auto &kv : meta->objCnt)
          func (kv.first, kv.second, user_data);
        return TRUE;
      });
}

/* ---- Object info ---- */

RdsAnalyticsObjInfo *
rds_analytics_obj_info_new (guint unique_id)
{
  return new_meta<RdsAnalyticsObjInfo> (G_STRFUNC, unique_id);
}

RdsAnalyticsObjInfo *
rds_analytics_obj_info_copy (const RdsAnalyticsObjInfo * info)
{
  g_return_val_if_fail (info != NULL, NULL);
  return copy_meta (G_STRFUNC, info);
}

void
rds_analytics_obj_info_free (RdsAnalyticsObjInfo * info)
{
  delete info;
}

RdsAnalyticsObjInfo *
rds_analytics_obj_info_attach (RdsObjectMeta * obj_meta, guint unique_id)
{
  g_return_val_if_fail (obj_meta != NULL, NULL);
  g_return_val_if_fail (obj_meta->base_meta.batch_meta != NULL, NULL);

  RdsAnalyticsObjInfo *info = rds_analytics_obj_info_new (unique_id);
  if (info == nullptr)
    return nullptr;
  RdsUserMeta *um = wrap (obj_meta->base_meta.batch_meta, RDS_OBJ_META_ANALYTICS, info);
  if (um == nullptr)
    return nullptr;
  if (!add_checked (obj_meta->base_meta.batch_meta, &obj_meta->obj_user_meta_list, um,
          [&] { rds_add_user_meta_to_obj (obj_meta, um); })) {
    g_critical ("%s: the user meta could not be added to object meta %p", G_STRFUNC, obj_meta);
    return nullptr;
  }
  return info;
}

static gboolean
add_label (const char *what, std::vector<std::string> &list, const gchar * label) noexcept
{
  return guarded<gboolean> (what, FALSE, [&] () -> gboolean {
        list.emplace_back (label);
        return TRUE;
      });
}

static gboolean
set_string (const char *what, std::string &field, const gchar * value) noexcept
{
  return guarded<gboolean> (what, FALSE, [&] () -> gboolean {
        field.assign (value);
        return TRUE;
      });
}

gboolean
rds_analytics_obj_info_add_roi_status (RdsAnalyticsObjInfo * info, const gchar * label)
{
  g_return_val_if_fail (info != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return add_label (G_STRFUNC, info->roiStatus, label);
}

gboolean
rds_analytics_obj_info_add_oc_status (RdsAnalyticsObjInfo * info, const gchar * label)
{
  g_return_val_if_fail (info != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return add_label (G_STRFUNC, info->ocStatus, label);
}

gboolean
rds_analytics_obj_info_add_lc_status (RdsAnalyticsObjInfo * info, const gchar * label)
{
  g_return_val_if_fail (info != NULL, FALSE);
  g_return_val_if_fail (label != NULL, FALSE);
  return add_label (G_STRFUNC, info->lcStatus, label);
}

gboolean
rds_analytics_obj_info_set_dir_status (RdsAnalyticsObjInfo * info, const gchar * status)
{
  g_return_val_if_fail (info != NULL, FALSE);
  g_return_val_if_fail (status != NULL, FALSE);
  return set_string (G_STRFUNC, info->dirStatus, status);
}

gboolean
rds_analytics_obj_info_set_obj_status (RdsAnalyticsObjInfo * info, const gchar * status)
{
  g_return_val_if_fail (info != NULL, FALSE);
  g_return_val_if_fail (status != NULL, FALSE);
  return set_string (G_STRFUNC, info->objStatus, status);
}

guint
rds_analytics_obj_info_n_roi_status (const RdsAnalyticsObjInfo * info)
{
  g_return_val_if_fail (info != NULL, 0);
  return info->roiStatus.size ();
}

const gchar *
rds_analytics_obj_info_get_roi_status (const RdsAnalyticsObjInfo * info, guint i)
{
  g_return_val_if_fail (info != NULL, NULL);
  return at (info->roiStatus, i);
}

guint
rds_analytics_obj_info_n_oc_status (const RdsAnalyticsObjInfo * info)
{
  g_return_val_if_fail (info != NULL, 0);
  return info->ocStatus.size ();
}

const gchar *
rds_analytics_obj_info_get_oc_status (const RdsAnalyticsObjInfo * info, guint i)
{
  g_return_val_if_fail (info != NULL, NULL);
  return at (info->ocStatus, i);
}

guint
rds_analytics_obj_info_n_lc_status (const RdsAnalyticsObjInfo * info)
{
  g_return_val_if_fail (info != NULL, 0);
  return info->lcStatus.size ();
}

const gchar *
rds_analytics_obj_info_get_lc_status (const RdsAnalyticsObjInfo * info, guint i)
{
  g_return_val_if_fail (info != NULL, NULL);
  return at (info->lcStatus, i);
}

const gchar *
rds_analytics_obj_info_get_dir_status (const RdsAnalyticsObjInfo * info)
{
  g_return_val_if_fail (info != NULL, NULL);
  return info->dirStatus.c_str ();
}

const gchar *
rds_analytics_obj_info_get_obj_status (const RdsAnalyticsObjInfo * info)
{
  g_return_val_if_fail (info != NULL, NULL);
  return info->objStatus.c_str ();
}

guint
rds_analytics_obj_info_get_unique_id (const RdsAnalyticsObjInfo * info)
{
  g_return_val_if_fail (info != NULL, 0);
  return info->unique_id;
}
