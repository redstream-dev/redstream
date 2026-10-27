# SPDX-License-Identifier: LGPL-2.1-or-later
# Plugin-private code shared by several plugins (never installed): the YAML config reader and the config value
# parsers, for rdsinfer and rdstracker. Built only for those plugins (both need libyaml, so it is found here).
if rds_infer_enabled or rds_tracker_enabled
  rds_plugin_common = static_library('rds-plugin-common',
    files('rds-keyfile-yaml.c', 'rds-config-parse.c'),
    dependencies : [dependency('glib-2.0'), yaml_dep],
    gnu_symbol_visibility : 'hidden',
    pic : true)
  rds_plugin_common_dep = declare_dependency(link_with : rds_plugin_common,
    include_directories : include_directories('.'),
    dependencies : [dependency('glib-2.0')])
endif
