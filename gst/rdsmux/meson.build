# SPDX-License-Identifier: LGPL-2.1-or-later
# The scaling mux compiles in the rdsvideo helpers (system-memory upload); they hold no GTypes.
gstrdsmux = library('gstrdsmux',
  files('plugin.c', 'gstrdsstreammux.c', 'gstrdsstreamdemux.c') + rdsvideo_util_src,
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsmux"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsmux
