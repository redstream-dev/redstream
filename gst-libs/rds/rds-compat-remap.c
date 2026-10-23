/* Redstream compat: DeepStream -> Redstream element factory name mapping.
 * Factory names are taken from NVIDIA's public DeepStream plugin documentation.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-compat-remap.h"

#include <string.h>

GST_DEBUG_CATEGORY_STATIC (rds_compat_debug);
#define GST_CAT_DEFAULT rds_compat_debug

typedef struct
{
  const gchar *nv;
  const gchar *rds;
} RdsCompatAlias;

static const RdsCompatAlias aliases[] = {
  {"nvurisrcbin", "rdsurisrcbin"},
  {"nvv4l2decoder", "rdsvideodec"},
  {"nvvideoconvert", "rdsvideoconvert"},
  {"nvstreammux", "rdsstreammux"},
  {"nvstreamdemux", "rdsstreamdemux"},
  {"nvinfer", "rdsinfer"},
  {"nvtracker", "rdstracker"},
  {"nvmultistreamtiler", "rdsmultistreamtiler"},
  {"nvdsosd", "rdsosd"},
  {"nvdsanalytics", "rdsanalytics"},
  {"nvdslogger", "rdslogger"},
  {"nveglglessink", "rdsvideosink"},
  {"nv3dsink", "rdsvideosink"},
  {"nvv4l2h264enc", "rdsh264enc"},
  {"nvv4l2h265enc", "rdsh265enc"},
};

static void
ensure_debug_category (void)
{
  static gsize initialized = 0;

  if (g_once_init_enter (&initialized)) {
    GST_DEBUG_CATEGORY_INIT (rds_compat_debug, "rdscompat", 0, "Redstream compat layer");
    g_once_init_leave (&initialized, 1);
  }
}

static gboolean
remap_disabled (void)
{
  return g_getenv ("RDS_COMPAT_NO_REMAP") != NULL;
}

const gchar *
rds_compat_map_factory_name (const gchar * factory_name)
{
  if (factory_name == NULL || remap_disabled ())
    return factory_name;

  for (gsize i = 0; i < G_N_ELEMENTS (aliases); i++) {
    if (strcmp (factory_name, aliases[i].nv) == 0)
      return aliases[i].rds;
  }
  return factory_name;
}

static gboolean
rewrite_token (const GMatchInfo * info, GString * result, G_GNUC_UNUSED gpointer user_data)
{
  gchar *token = g_match_info_fetch (info, 1);

  g_string_append (result, rds_compat_map_factory_name (token));
  g_free (token);
  return FALSE;
}

gchar *
rds_compat_rewrite_launch (const gchar * description)
{
  static GRegex *token_re;
  static gsize initialized = 0;

  if (description == NULL)
    return NULL;
  if (remap_disabled ())
    return g_strdup (description);

  /* Only whole tokens are rewritten: "nvinfer" becomes "rdsinfer", "config-file-path=nvinfer.txt" is untouched. */
  if (g_once_init_enter (&initialized)) {
    token_re = g_regex_new ("(?<=^|[\\s!])(nv[A-Za-z0-9_]+)(?=$|[\\s!])", G_REGEX_OPTIMIZE, 0, NULL);
    g_assert (token_re != NULL);
    g_once_init_leave (&initialized, 1);
  }
  return g_regex_replace_eval (token_re, description, -1, 0, 0, rewrite_token, NULL, NULL);
}

GstElement *
rds_compat_element_factory_make (const gchar * factoryname, const gchar * name)
{
  const gchar *mapped = rds_compat_map_factory_name (factoryname);

  ensure_debug_category ();
  if (g_strcmp0 (mapped, factoryname) != 0)
    GST_DEBUG ("remapped element factory %s -> %s", factoryname, mapped);
  return gst_element_factory_make (mapped, name);
}

GstElement *
rds_compat_parse_launch (const gchar * pipeline_description, GError ** error)
{
  gchar *rewritten = rds_compat_rewrite_launch (pipeline_description);
  GstElement *pipeline;

  ensure_debug_category ();
  if (g_strcmp0 (rewritten, pipeline_description) != 0)
    GST_DEBUG ("remapped launch description: %s", rewritten);
  pipeline = gst_parse_launch (rewritten, error);
  g_free (rewritten);
  return pipeline;
}
