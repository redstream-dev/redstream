# Plugin-private code shared by the plugin, rds-compile and the tests: the config parser, the parser loader with the
# built-in YOLO parser, and the detection post-processing. The loader is C++, so whatever links this library links
# with the C++ linker (link_language : 'cpp').
glib_dep = dependency('glib-2.0')
dl_dep = dependency('dl')
# rds/rds-infer*.h only: the loader calls nothing in libgstrds.
rds_headers_dep = rds_dep.partial_dependency(compile_args : true, includes : true)
rdsinfer_core = static_library('rdsinfer-core',
  files('rds-infer-config.c', 'rds-keyfile-yaml.c', 'rds-infer-parse.cpp', 'rds-infer-post.c'),
  c_args : ['-DG_LOG_DOMAIN="rdsinfer"'],
  cpp_args : ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [glib_dep, yaml_dep, dl_dep, rds_headers_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsinfer_core_dep = declare_dependency(link_with : rdsinfer_core,
  include_directories : include_directories('.'),
  dependencies : [glib_dep, rds_headers_dep])

gstrdsinfer = library('gstrdsinfer',
  files('plugin.c', 'gstrdsinfer.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep, migraphx_dep, yaml_dep, json_glib_dep,
    rdsinfer_core_dep],
  gnu_symbol_visibility : 'hidden',
  link_language : 'cpp',
  install_rpath : rds_infer_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsinfer
