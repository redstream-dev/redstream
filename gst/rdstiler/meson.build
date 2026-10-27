# SPDX-License-Identifier: LGPL-2.1-or-later
# The layout core (grid, cells, meta scaling; GLib only, the meta structs come from the libgstrds headers), shared by
# the plugin and the tests.
rdstiler_core = static_library('rdstiler-core',
  files('rds-tiler-layout.c'),
  c_args : ['-DG_LOG_DOMAIN="rdstiler"'],
  dependencies : [dependency('glib-2.0'), rds_dep.partial_dependency(includes : true)],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdstiler_core_dep = declare_dependency(link_with : rdstiler_core,
  include_directories : include_directories('.'),
  dependencies : [dependency('glib-2.0'), rds_dep.partial_dependency(includes : true)])

gstrdstiler = library('gstrdstiler',
  files('plugin.c', 'gstrdsmultistreamtiler.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdstiler"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep, rdstiler_core_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdstiler
