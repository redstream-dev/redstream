gstrdstracker = library('gstrdstracker',
  files('plugin.c', 'gstrdstracker.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdstracker"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, rds_plugin_common_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdstracker
