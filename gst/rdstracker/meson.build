# Plugin-private code shared by the plugin and the tests: the config parser (YAML read through rds-plugin-common) and
# the tracking core (GLib only).
rdstracker_core = static_library('rdstracker-core',
  files('rds-tracker-config.c', 'rds-tracker-core.c', 'rds-tracker-kf.c'),
  c_args : ['-DG_LOG_DOMAIN="rdstracker"'],
  dependencies : [dependency('glib-2.0'), rds_plugin_common_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdstracker_core_dep = declare_dependency(link_with : rdstracker_core,
  include_directories : include_directories('.'),
  dependencies : [dependency('glib-2.0'), rds_plugin_common_dep])

gstrdstracker = library('gstrdstracker',
  files('plugin.c', 'gstrdstracker.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdstracker"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, rdstracker_core_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdstracker
