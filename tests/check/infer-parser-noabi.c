/* Test modules that are not Redstream parsers: they never include rds/rds-infer-custom-impl.h. Built
 * - as is: no rds_infer_parser_abi_version at all;
 * - with -DRDS_TEST_ABI_VERSION=<n>: the module defines that symbol itself, with a version rdsinfer does not support;
 * - with -DRDS_TEST_ABI_HELPER -DRDS_TEST_ABI_VERSION=1: a helper library (not a parser) that has the symbol;
 * - with -DRDS_TEST_ABI_FROM_DEP, linked to that helper: the module has the symbol only through a dependency.
 * Not installed.
 * SPDX-License-Identifier: LGPL-2.1-or-later */

int NoAbiParse (void);
int rds_test_abi_helper (void);

#ifdef RDS_TEST_ABI_HELPER
int
rds_test_abi_helper (void)
{
  return 0;
}
#endif

int
NoAbiParse (void)
{
#ifdef RDS_TEST_ABI_FROM_DEP
  return rds_test_abi_helper ();
#else
  return 0;
#endif
}

#ifdef RDS_TEST_ABI_VERSION
unsigned int rds_infer_parser_abi_version (void);

unsigned int
rds_infer_parser_abi_version (void)
{
  return RDS_TEST_ABI_VERSION;
}
#endif
