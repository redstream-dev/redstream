/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "rds-test-util.h"

static void
test_device_alloc (void)
{
  void *ptr = NULL;

  RDS_TEST_REQUIRE_GPU ();
  g_assert_cmpint (hipMalloc (&ptr, 1 << 20), ==, hipSuccess);
  g_assert_nonnull (ptr);
  g_assert_cmpint (hipMemset (ptr, 0x5a, 1 << 20), ==, hipSuccess);
  g_assert_cmpint (hipFree (ptr), ==, hipSuccess);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/gpu/smoke/device-alloc", test_device_alloc);
  return g_test_run ();
}
