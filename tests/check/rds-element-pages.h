/* Each factory's reference page under docs/elements/, read by the element_docs and element_compat_matrix lints.
 * A factory is listed once its page exists.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <glib.h>
#include <string.h>

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
};

/* CommonMark code fences, for both lints. A line of at most 3 spaces, then 3 or more '`' or '~' (a '`' fence's info
 * string has no '`') opens a fence; a line of at most 3 spaces, then at least as many of the same character and only
 * blanks after, closes it. Returns whether `line` is a fence line or inside a fence. `fence` is the open fence's
 * character (0: none) and `fence_len` its length; start both at 0. */
static inline gboolean
rds_md_fence_line (const char *line, char *fence, guint * fence_len)
{
  const char *p = line;
  guint n = 0;

  for (guint indent = 0; *p == ' ' && indent < 3; indent++)
    p++;
  if (*p == '`' || *p == '~')
    while (p[n] == *p)
      n++;
  if (*fence == 0) {
    if (n < 3 || (*p == '`' && strchr (p + n, '`') != NULL))
      return FALSE;
    *fence = *p;
    *fence_len = n;
    return TRUE;
  }
  if (n > 0 && *p == *fence && n >= *fence_len) {
    const char *q = p + n;

    while (*q == ' ' || *q == '\t' || *q == '\r')
      q++;
    if (*q == '\0')
      *fence = 0;
  }
  return TRUE;
}
