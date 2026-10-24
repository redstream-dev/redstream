/* Host-side helpers for GPU tests on RdsBufSurfaces: deterministic fills, CPU snapshots and comparisons. Public
 * libgstrds API only, all static, so tests linking either the shared or the internal static library can use it.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_SURFACE_H
#define RDS_TEST_SURFACE_H

#include "transform-ref.h"

#include <rds/rds-buf-surface.h>
#include <string.h>

/* One generator per test executable, not thread-safe: fills run on the test thread only. Each fill reseeds it,
 * so a pattern depends only on its seed. */
static guint32 rng_state;

static inline guint8
rng_next (void)
{
  /* xorshift32, fixed seed per fill: deterministic patterns */
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return (guint8) (rng_state >> 11);
}

/* Fills the pixel bytes of every plane of surface @idx with a pseudo-random pattern. */
static inline void
fill_random (RdsBufSurface * s, guint idx, guint32 seed)
{
  RdsBufSurfaceParams *p = &s->surfaceList[idx];

  rng_state = seed * 2654435761u + 1;
  g_assert_cmpint (rds_buf_surface_map (s, idx, -1, RDS_BUF_MAP_WRITE), ==, 0);
  for (guint pl = 0; pl < p->planeParams.num_planes; pl++) {
    guint row_bytes = p->planeParams.width[pl] * p->planeParams.bytesPerPix[pl];
    for (guint y = 0; y < p->planeParams.height[pl]; y++) {
      guint8 *row = (guint8 *) p->mappedAddr.addr[pl] + (gsize) y * p->planeParams.pitch[pl];
      for (guint x = 0; x < row_bytes; x++)
        row[x] = rng_next ();
    }
  }
  g_assert_cmpint (rds_buf_surface_sync_for_device (s, idx, -1), ==, 0);
  g_assert_cmpint (rds_buf_surface_unmap (s, idx, -1), ==, 0);
}

typedef struct
{
  RefImage im;
  guint num_planes;
  guint row_bytes[3];
  guint rows[3];
} HostImage;

static inline void
snapshot (RdsBufSurface * s, guint idx, HostImage * h)
{
  RdsBufSurfaceParams *p = &s->surfaceList[idx];

  memset (h, 0, sizeof (*h));
  h->im.format = p->colorFormat;
  h->im.width = p->width;
  h->im.height = p->height;
  h->num_planes = p->planeParams.num_planes;
  g_assert_cmpint (rds_buf_surface_map (s, idx, -1, RDS_BUF_MAP_READ), ==, 0);
  g_assert_cmpint (rds_buf_surface_sync_for_cpu (s, idx, -1), ==, 0);
  for (guint pl = 0; pl < h->num_planes; pl++) {
    gsize size = (gsize) p->planeParams.pitch[pl] * p->planeParams.height[pl];

    h->im.pitch[pl] = p->planeParams.pitch[pl];
    h->im.plane[pl] = g_memdup2 (p->mappedAddr.addr[pl], size);
    h->row_bytes[pl] = p->planeParams.width[pl] * p->planeParams.bytesPerPix[pl];
    h->rows[pl] = p->planeParams.height[pl];
  }
  g_assert_cmpint (rds_buf_surface_unmap (s, idx, -1), ==, 0);
}

static inline void
host_image_clear (HostImage * h)
{
  for (guint pl = 0; pl < h->num_planes; pl++)
    g_free (h->im.plane[pl]);
  memset (h, 0, sizeof (*h));
}

/* Largest |a - b| over the pixel bytes of every plane. */
static inline guint
max_diff (const HostImage * a, const HostImage * b, gboolean verbose)
{
  guint m = 0;

  g_assert_cmpuint (a->num_planes, ==, b->num_planes);
  for (guint pl = 0; pl < a->num_planes; pl++) {
    for (guint y = 0; y < a->rows[pl]; y++) {
      const guint8 *ra = a->im.plane[pl] + (gsize) y * a->im.pitch[pl];
      const guint8 *rb = b->im.plane[pl] + (gsize) y * b->im.pitch[pl];
      for (guint x = 0; x < a->row_bytes[pl]; x++) {
        guint d = (guint) ABS ((gint) ra[x] - (gint) rb[x]);
        if (verbose && d > 1 && m <= 1)
          g_test_message ("first large diff: plane %u row %u byte %u: %u vs %u", pl, y, x, ra[x], rb[x]);
        m = MAX (m, d);
      }
    }
  }
  return m;
}

#endif /* RDS_TEST_SURFACE_H */
