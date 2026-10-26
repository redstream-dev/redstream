rds_test3_exe = executable('rds-test3', 'rds_test3_app.c',
  dependencies : [rds_dep, cc.find_library('m', required : false)],
  # the default --pgie-config: the sample is run from the build tree (not installed), its config stays in the source
  c_args : ['-DRDS_TEST3_SOURCE_DIR="@0@"'.format(meson.current_source_dir())],
  install : false)
