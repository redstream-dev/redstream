# Plugin-private code shared by the plugin, rds-compile and the tests: the config parser.
glib_dep = dependency('glib-2.0')
rdsinfer_core = static_library('rdsinfer-core',
  files('rds-infer-config.c', 'rds-keyfile-yaml.c'),
  c_args : ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [glib_dep, yaml_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsinfer_core_dep = declare_dependency(link_with : rdsinfer_core,
  include_directories : include_directories('.'),
  dependencies : [glib_dep])

gstrdsinfer = library('gstrdsinfer',
  files('plugin.c', 'gstrdsinfer.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep, migraphx_dep, yaml_dep, json_glib_dep,
    rdsinfer_core_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_infer_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsinfer
