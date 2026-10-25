gstrdsinfer = library('gstrdsinfer',
  files('plugin.c', 'gstrdsinfer.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep, migraphx_dep, yaml_dep, json_glib_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_infer_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsinfer
