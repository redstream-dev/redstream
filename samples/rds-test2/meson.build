# SPDX-License-Identifier: MIT
# The YAML reader that rdsinfer and rdstracker use (plugin-internal, linked in statically) reads the .yml PGIE and SGIE
# configs for the label file and the classifier's gie-unique-id; it is built whenever rdsinfer is. Without it, only
# key-file configs are read. It is an in-tree helper, not installed: an app built outside this tree uses key-file
# configs or its own YAML reader.
rds_test2_deps = [rds_dep, cc.find_library('m', required : false)]
rds_test2_have_yaml = is_variable('rds_plugin_common_dep')
if rds_test2_have_yaml
  rds_test2_deps += rds_plugin_common_dep
endif
rds_test2_exe = executable('rds-test2', 'rds_test2_app.c',
  dependencies : rds_test2_deps,
  # the default configs: the sample is run from the build tree (not installed), its configs stay in the source
  c_args : ['-DRDS_TEST2_SOURCE_DIR="@0@"'.format(meson.current_source_dir()),
    '-DRDS_TEST2_HAVE_YAML=@0@'.format(rds_test2_have_yaml ? 1 : 0)],
  install : false)
