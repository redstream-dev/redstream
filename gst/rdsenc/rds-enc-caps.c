/* Redstream: the caps decisions of rdsh264enc and rdsh265enc: the preferred profile, the Baseline + B frames rule
 * and the downstream level check, on plain caps.
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
