# The YAML reader that rdsinfer and rdstracker use (plugin-internal, linked in statically) reads a .yml PGIE config
# for the label file; it is built whenever rdsinfer is. Without it, only key-file PGIE configs give labels.
rds_test3_deps = [rds_dep, cc.find_library('m', required : false)]
rds_test3_have_yaml = is_variable('rds_plugin_common_dep')
if rds_test3_have_yaml
  rds_test3_deps += rds_plugin_common_dep
endif
rds_test3_exe = executable('rds-test3', 'rds_test3_app.c',
  dependencies : rds_test3_deps,
  # the default --pgie-config: the sample is run from the build tree (not installed), its config stays in the source
  c_args : ['-DRDS_TEST3_SOURCE_DIR="@0@"'.format(meson.current_source_dir()),
    '-DRDS_TEST3_HAVE_YAML=@0@'.format(rds_test3_have_yaml ? 1 : 0)],
  install : false)
