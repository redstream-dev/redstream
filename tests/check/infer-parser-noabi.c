/* Test module that is not a Redstream parser: it never includes rds/rds-infer-custom-impl.h, so it has no
 * rds_infer_parser_abi_version. Built a second time with -DRDS_TEST_ABI_VERSION=<n>, it defines that symbol
 * itself with a version rdsinfer does not support. Not installed.
 * SPDX-License-Identifier: LGPL-2.1-or-later */

int NoAbiParse (void);

int
NoAbiParse (void)
{
  return 0;
}

#ifdef RDS_TEST_ABI_VERSION
unsigned int rds_infer_parser_abi_version (void);

unsigned int
rds_infer_parser_abi_version (void)
{
  return RDS_TEST_ABI_VERSION;
}
#endif
