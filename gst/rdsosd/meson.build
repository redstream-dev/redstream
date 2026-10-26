# The drawing core (shapes as draw commands shaded onto a CPU RGBA plane, Pango for text masks; no GStreamer, no GPU;
# the meta structs come from the libgstrds headers), shared by the plugin and the tests. No fused multiply-add: the
# blend must round as the same code does compiled for the GPU (rds-osd-shapes.h). GCC ignores the header's
# contraction pragma, so everything that includes it (through rdsosd_core_dep too) builds with -ffp-contract=off.
rdsosd_core = static_library('rdsosd-core',
  files('rds-osd-draw.c', 'rds-osd-shapes.c'),
  c_args : ['-DG_LOG_DOMAIN="rdsosd"', '-ffp-contract=off'],
  dependencies : [dependency('glib-2.0'), rds_dep.partial_dependency(includes : true), cairo_dep, pangocairo_dep,
    cc.find_library('m', required : false)],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsosd_core_dep = declare_dependency(link_with : rdsosd_core,
  include_directories : include_directories('.'),
  compile_args : ['-ffp-contract=off'],
  dependencies : [dependency('glib-2.0'), rds_dep.partial_dependency(includes : true), cairo_dep, pangocairo_dep,
    cc.find_library('m', required : false)])

gstrdsosd = library('gstrdsosd',
  files('plugin.c', 'gstrdsosd.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsosd"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, rdsosd_core_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsosd
