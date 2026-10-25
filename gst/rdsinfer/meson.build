# MIGraphX may live outside the default linker path (ROCm 10 puts it in <rocm>/extras-10/lib): give the plugin an
# install rpath to it, next to the ROCm one.
rdsinfer_rpaths = [rds_install_rpath, migraphx_libdir == '/opt/rocm/lib' ? '' : migraphx_libdir]
rdsinfer_install_rpath = ':'.join(rdsinfer_rpaths).strip(':')
gstrdsinfer = library('gstrdsinfer',
  files('plugin.c', 'gstrdsinfer.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep, migraphx_dep, yaml_dep, json_glib_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rdsinfer_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsinfer
