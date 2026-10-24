/* Launches the placeholder HIP kernel linked into libgstrds and checks its output.
 * SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"
#include <rds/rds-transform-private.h>

static void
test_kernel_fill (void)
{
  const guint n = 4099;         /* not a multiple of the block size */
  guint8 *dev = NULL;
  guint8 *host;
  guint i;

  RDS_TEST_REQUIRE_GPU ();
  g_assert_cmpint (hipMalloc ((void **) &dev, n + 1), ==, hipSuccess);
  g_assert_cmpint (hipMemset (dev, 0, n + 1), ==, hipSuccess);

  g_assert_cmpint (rds_transform_kernel_smoke (dev, n, 0xa5, NULL), ==, 0);
  g_assert_cmpint (hipDeviceSynchronize (), ==, hipSuccess);

  host = g_malloc (n + 1);
  g_assert_cmpint (hipMemcpy (host, dev, n + 1, hipMemcpyDeviceToHost), ==, hipSuccess);
  for (i = 0; i < n; i++)
    g_assert_cmpuint (host[i], ==, 0xa5);
  g_assert_cmpuint (host[n], ==, 0);    /* nothing written past the end */

  g_free (host);
  g_assert_cmpint (hipFree (dev), ==, hipSuccess);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/kernel/smoke-fill", test_kernel_fill);
  return g_test_run ();
}
