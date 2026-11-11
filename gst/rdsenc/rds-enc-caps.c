/* Redstream: the caps decisions of rdsh264enc and rdsh265enc: the preferred profile, the Baseline + B frames rule,
 * the downstream level check and the VA encoder's size range, on plain caps.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-enc-caps.h"

#include "rds-enc-props.h"

/* Adds the profile names in @v (a string or a list of strings) to @names. */
static void
collect_profiles (const GValue * v, GPtrArray * names)
{
  if (v == NULL)
    return;
  if (G_VALUE_HOLDS_STRING (v)) {
    g_ptr_array_add (names, (gpointer) g_value_get_string (v));
  } else if (GST_VALUE_HOLDS_LIST (v)) {
    for (guint i = 0; i < gst_value_list_get_size (v); i++)
      collect_profiles (gst_value_list_get_value (v, i), names);
  }
}

/* @caps narrowed to the profiles @keep (@n of them), or NULL when that leaves nothing. */
static GstCaps *
narrow_profiles (const GstCaps * caps, const gchar * caps_name, const gchar * const *keep, guint n)
{
  GstCaps *want = gst_caps_new_empty_simple (caps_name), *res;
  GValue v = G_VALUE_INIT;

  if (n == 1) {
    g_value_init (&v, G_TYPE_STRING);
    g_value_set_string (&v, keep[0]);
  } else {
    gst_value_list_init (&v, n);
    for (guint i = 0; i < n; i++) {
      GValue s = G_VALUE_INIT;

      g_value_init (&s, G_TYPE_STRING);
      g_value_set_string (&s, keep[i]);
      gst_value_list_append_and_take_value (&v, &s);
    }
  }
  gst_structure_take_value (gst_caps_get_structure (want, 0), "profile", &v);
  res = gst_caps_intersect ((GstCaps *) caps, want);
  gst_caps_unref (want);
  if (gst_caps_is_empty (res)) {
    gst_caps_unref (res);
    return NULL;
  }
  return res;
}

GstCaps *
rds_enc_caps_narrow (const GstCaps * answer, const gchar * caps_name, const gchar * preferred, guint b_frames,
    gboolean * only_baseline)
{
  GstCaps *res = NULL, *n;
  GPtrArray *names, *others;
  gboolean any_baseline = FALSE;

  g_return_val_if_fail (GST_IS_CAPS (answer), NULL);
  if (preferred != NULL)
    res = narrow_profiles (answer, caps_name, &preferred, 1);
  if (res == NULL)
    res = gst_caps_ref ((GstCaps *) answer);

  /* the strings stay valid while res is */
  names = g_ptr_array_new ();
  others = g_ptr_array_new ();
  for (guint i = 0; i < gst_caps_get_size (res); i++)
    collect_profiles (gst_structure_get_value (gst_caps_get_structure (res, i), "profile"), names);
  for (guint i = 0; i < names->len; i++) {
    const gchar *name = g_ptr_array_index (names, i);

    if (rds_enc_profile_is_baseline (name))
      any_baseline = TRUE;
    else
      g_ptr_array_add (others, (gpointer) name);
  }
  if (only_baseline != NULL)
    *only_baseline = any_baseline && others->len == 0;
  if (b_frames > 0 && any_baseline && others->len > 0) {
    n = narrow_profiles (res, caps_name, (const gchar * const *) others->pdata, others->len);
    if (n != NULL)
      gst_caps_take (&res, n);
  }
  g_ptr_array_unref (others);
  g_ptr_array_unref (names);
  return res;
}

gboolean
rds_enc_caps_level_refused (const GstCaps * caps, const GstCaps * allowed, gchar ** wanted)
{
  GstCaps *bare;
  gboolean ok_without_level;
  gchar *want = NULL;

  g_return_val_if_fail (GST_IS_CAPS (caps) && GST_IS_CAPS (allowed), FALSE);
  g_return_val_if_fail (wanted != NULL, FALSE);
  if (gst_caps_is_empty (caps) || !gst_structure_has_field (gst_caps_get_structure (caps, 0), "level")
      || gst_caps_can_intersect (caps, allowed))
    return FALSE;
  bare = gst_caps_copy (caps);
  for (guint i = 0; i < gst_caps_get_size (bare); i++)
    gst_structure_remove_field (gst_caps_get_structure (bare, i), "level");
  ok_without_level = gst_caps_can_intersect (bare, allowed);
  gst_caps_unref (bare);
  if (!ok_without_level)
    return FALSE;               /* refused for another reason too */
  for (guint i = 0; i < gst_caps_get_size (allowed) && want == NULL; i++) {
    const GValue *v = gst_structure_get_value (gst_caps_get_structure (allowed, i), "level");

    if (v != NULL)
      want = gst_value_serialize (v);
  }
  if (want == NULL)
    return FALSE;
  *wanted = want;
  return TRUE;
}

/* @v (an int or an int range) as [*min, *max]; FALSE for anything else. */
static gboolean
int_range (const GValue * v, gint * min, gint * max)
{
  if (v != NULL && G_VALUE_HOLDS_INT (v)) {
    *min = *max = g_value_get_int (v);
    return TRUE;
  }
  if (v != NULL && GST_VALUE_HOLDS_INT_RANGE (v)) {
    *min = gst_value_get_int_range_min (v);
    *max = gst_value_get_int_range_max (v);
    return TRUE;
  }
  return FALSE;
}

gboolean
rds_enc_caps_size_range (const GstCaps * templ, gint * min_width, gint * max_width, gint * min_height,
    gint * max_height)
{
  if (gst_caps_is_any (templ))
    return FALSE;
  for (guint i = 0; i < gst_caps_get_size (templ); i++) {
    const GstStructure *st = gst_caps_get_structure (templ, i);
    GstCapsFeatures *f = gst_caps_get_features (templ, i);
    gint w0, w1, h0, h1;

    if (f != NULL && !gst_caps_features_is_equal (f, GST_CAPS_FEATURES_MEMORY_SYSTEM_MEMORY))
      continue;
    if (!int_range (gst_structure_get_value (st, "width"), &w0, &w1)
        || !int_range (gst_structure_get_value (st, "height"), &h0, &h1))
      continue;
    *min_width = w0;
    *max_width = w1;
    *min_height = h0;
    *max_height = h1;
    return TRUE;
  }
  return FALSE;
}

/* Sets @field of @st to [@min, @max] (an int when they are equal). */
static void
set_int_range (GstStructure * st, const gchar * field, gint min, gint max)
{
  if (min == max)
    gst_structure_set (st, field, G_TYPE_INT, min, NULL);
  else
    gst_structure_set (st, field, GST_TYPE_INT_RANGE, min, max, NULL);
}

GstCaps *
rds_enc_caps_limit_size (const GstCaps * caps, gint min_width, gint max_width, gint min_height, gint max_height)
{
  GstCaps *res;

  if (gst_caps_is_any (caps))
    return gst_caps_new_any ();
  res = gst_caps_new_empty ();
  for (guint i = 0; i < gst_caps_get_size (caps); i++) {
    const GstStructure *st = gst_caps_get_structure (caps, i);
    GstCapsFeatures *f = gst_caps_get_features (caps, i);
    GstStructure *lim = gst_structure_new_empty (gst_structure_get_name (st)), *got;

    set_int_range (lim, "width", min_width, max_width);
    set_int_range (lim, "height", min_height, max_height);
    got = gst_structure_intersect (st, lim);

    gst_structure_free (lim);
    if (got != NULL)
      res = gst_caps_merge_structure_full (res, got, f != NULL ? gst_caps_features_copy (f) : NULL);
  }
  return res;
}
