# SPDX-License-Identifier: LGPL-2.1-or-later
# rdsurisrcbin (file and RTSP sources to memory:RDS frames); needs gstreamer-rtsp for the RTSP lower transports, so
# always built with GStreamer's base libraries.
gstrdsurisrcbin = library('gstrdsurisrcbin',
  files('plugin.c', 'gstrdsurisrcbin.c', 'rds-urisrc-session.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsurisrcbin"'],
  dependencies : [rds_dep, gst_dep, gst_rtsp_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsurisrcbin
