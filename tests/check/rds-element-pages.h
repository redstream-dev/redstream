/* Each factory's reference page under docs/elements/, read by the element_docs and element_compat_matrix lints.
 * Every factory is listed. The includer is built with RDS_HAVE_OSD, RDS_HAVE_INFER and RDS_HAVE_TRACKER (0 or 1) from
 * meson: a factory the build leaves out is skipped with a message; one the build has must load.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

#include "rds-md.h"

#if !defined (RDS_HAVE_OSD) || !defined (RDS_HAVE_INFER) || !defined (RDS_HAVE_TRACKER)
#error "define RDS_HAVE_OSD, RDS_HAVE_INFER and RDS_HAVE_TRACKER (meson's rds_*_enabled)"
#endif

typedef struct
{
  const char *factory;
  const char *page;             /* file name in docs/elements/ */
  gboolean configured;          /* FALSE when the build leaves it out (-Dosd, -Dinfer, -Dtracker) */
} RdsElementPage;

static const RdsElementPage rds_element_pages[] = {
  {"rdslogger", "rdslogger.md", TRUE},
  {"rdsvideosink", "rdsvideosink.md", TRUE},
  {"rdsstreamdemux", "rdsstreamdemux.md", TRUE},
  {"rdsvideodec", "rdsvideodec.md", TRUE},
  {"rdsvideoconvert", "rdsvideoconvert.md", TRUE},
  {"rdsurisrcbin", "rdsurisrcbin.md", TRUE},
  {"rdsstreammux", "rdsstreammux.md", TRUE},
  {"rdsmultistreamtiler", "rdsmultistreamtiler.md", TRUE},
  {"rdsosd", "rdsosd.md", RDS_HAVE_OSD},
  {"rdsinfer", "rdsinfer.md", RDS_HAVE_INFER},
  {"rdstracker", "rdstracker.md", RDS_HAVE_TRACKER},
  {"rdsanalytics", "rdsanalytics.md", TRUE},
  {"rdsh264enc", "rdsh26xenc.md", TRUE},
  {"rdsh265enc", "rdsh26xenc.md", TRUE},
};
