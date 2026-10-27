# SPDX-License-Identifier: LGPL-2.1-or-later
rdsvideo_util_src = files('rds-video-util.c')
gstrdsvideo = library('gstrdsvideo',
  files('plugin.c', 'gstrdsvideoconvert.c', 'gstrdsvideodec.c') + rdsvideo_util_src,
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsvideo"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep, rocdecode_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsvideo
