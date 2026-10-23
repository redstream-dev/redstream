/* Shared helpers for Redstream unit tests.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef RDS_TEST_UTIL_H
#define RDS_TEST_UTIL_H

/* GLib/GStreamer first: HIP's headers define __noinline__, which breaks GLib if HIP comes first. */
#include <gst/gst.h>
#include <hip/hip_runtime_api.h>

static inline gboolean
rds_test_have_gpu (void)
{
  int n = 0;

  return hipGetDeviceCount (&n) == hipSuccess && n > 0;
}

#define RDS_TEST_REQUIRE_GPU() G_STMT_START {        \
    if (!rds_test_have_gpu ()) {                     \
      g_test_skip ("no ROCm device visible");        \
      return;                                        \
    }                                                \
  } G_STMT_END

#endif /* RDS_TEST_UTIL_H */
