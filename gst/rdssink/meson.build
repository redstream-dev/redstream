# rdslogger (per-stream FPS) and rdsvideosink (display bin); no dependencies beyond GStreamer and libgstrds, so always
# built.
gstrdssink = library('gstrdssink',
  files('plugin.c', 'gstrdslogger.c', 'gstrdsvideosink.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdssink"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdssink
