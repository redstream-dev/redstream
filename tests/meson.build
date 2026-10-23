test('compat_remap',
  executable('compat_remap', 'check/compat_remap.c', dependencies : [rds_dep]))
test('compat_header',
  executable('compat_header', 'check/compat_header.c', dependencies : [rds_dep]))
test('compat_header_nomacros',
  executable('compat_header_nomacros', 'check/compat_header_nomacros.c', dependencies : [rds_dep]))
test('rds_test3_dry_run',
  find_program('samples/rds_test3_dry_run.sh'),
  args : [rds_test3_exe])
