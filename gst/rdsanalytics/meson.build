# SPDX-License-Identifier: LGPL-2.1-or-later
# Plugin-private code shared by the plugin and the tests: the config parser (a GLib key file, read from one key
# table). GLib only, always built.
rdsanalytics_core_sources = files('rds-analytics-config.c')
rdsanalytics_core = static_library('rdsanalytics-core',
  rdsanalytics_core_sources,
  c_args : ['-DG_LOG_DOMAIN="rdsanalytics"'],
  dependencies : [dependency('glib-2.0'), rds_config_parse_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsanalytics_core_dep = declare_dependency(link_with : rdsanalytics_core,
  include_directories : include_directories('.'),
  dependencies : [dependency('glib-2.0'), rds_config_parse_dep])
