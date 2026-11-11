# SPDX-License-Identifier: LGPL-2.1-or-later
# Plugin-private code shared by several plugins (never installed). The config value parsers (GLib only) are always
# built, for rdsinfer, rdstracker and rdsanalytics; the YAML config reader only for rdsinfer and rdstracker (both
# need libyaml, so it is found here).

# Header-only helpers (rds-video-convert-caps.h: the caps of rdsvideoconvert and the bins that wrap it).
rds_plugin_headers_dep = declare_dependency(include_directories : include_directories('.'))

rds_config_parse = static_library('rds-config-parse',
  files('rds-config-parse.c'),
  dependencies : [dependency('glib-2.0')],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rds_config_parse_dep = declare_dependency(link_with : rds_config_parse,
  include_directories : include_directories('.'),
  dependencies : [dependency('glib-2.0')])

if rds_infer_enabled or rds_tracker_enabled
  rds_plugin_common = static_library('rds-plugin-common',
    files('rds-keyfile-yaml.c'),
    dependencies : [dependency('glib-2.0'), yaml_dep],
    gnu_symbol_visibility : 'hidden',
    pic : true)
  rds_plugin_common_dep = declare_dependency(link_with : rds_plugin_common,
    include_directories : include_directories('.'),
    dependencies : [dependency('glib-2.0'), rds_config_parse_dep])
endif
