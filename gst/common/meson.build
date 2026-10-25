# Plugin-private code shared by several plugins (never installed): the YAML config reader, for rdsinfer and
# rdstracker. Built only with libyaml; the plugins that need it are disabled without it.
if yaml_dep.found()
  rds_plugin_common = static_library('rds-plugin-common',
    files('rds-keyfile-yaml.c'),
    dependencies : [dependency('glib-2.0'), yaml_dep],
    gnu_symbol_visibility : 'hidden',
    pic : true)
  rds_plugin_common_dep = declare_dependency(link_with : rds_plugin_common,
    include_directories : include_directories('.'),
    dependencies : [dependency('glib-2.0')])
endif
