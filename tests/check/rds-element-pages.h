/* Each factory's reference page under docs/elements/, read by the element_docs and element_compat_matrix lints.
 * A factory is listed once its page exists.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>

#include "rds-md.h"

typedef struct
{
  const char *factory;
  const char *page;             /* file name in docs/elements/ */
  gboolean optional;            /* not built in every configuration (rdsosd: -Dosd) */
} RdsElementPage;

static const RdsElementPage rds_element_pages[] = {
  {"rdslogger", "rdslogger.md", FALSE},
  {"rdsvideosink", "rdsvideosink.md", FALSE},
  {"rdsstreamdemux", "rdsstreamdemux.md", FALSE},
  {"rdsvideodec", "rdsvideodec.md", FALSE},
  {"rdsvideoconvert", "rdsvideoconvert.md", FALSE},
  {"rdsurisrcbin", "rdsurisrcbin.md", FALSE},
  {"rdsstreammux", "rdsstreammux.md", FALSE},
  {"rdsmultistreamtiler", "rdsmultistreamtiler.md", FALSE},
  {"rdsosd", "rdsosd.md", TRUE},
  {"rdsinfer", "rdsinfer.md", FALSE},
  {"rdstracker", "rdstracker.md", FALSE},
  {"rdsanalytics", "rdsanalytics.md", FALSE},
};
