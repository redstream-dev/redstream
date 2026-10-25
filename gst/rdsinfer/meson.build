# Plugin-private code shared by the plugin, rds-compile and the tests: the config parser (YAML read through
# rds-plugin-common), the parser loader with the built-in YOLO parser, the detection post-processing, and the
# MIGraphX engine with its program cache (and the ONNX reader it uses). The loader and the engine are C++, so whatever
# links this library links with the C++ linker (link_language : 'cpp').
glib_dep = dependency('glib-2.0')
dl_dep = dependency('dl')
# rds/rds-infer*.h only: the loader calls nothing in libgstrds.
rds_headers_dep = rds_dep.partial_dependency(compile_args : true, includes : true)
rdsinfer_core = static_library('rdsinfer-core',
  files('rds-infer-config.c', 'rds-infer-parse.cpp', 'rds-infer-post.c', 'rds-infer-engine.cpp',
    'rds-infer-onnx.c'),
  c_args : ['-DG_LOG_DOMAIN="rdsinfer"'],
  cpp_args : ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [glib_dep, rds_plugin_common_dep, dl_dep, rds_headers_dep, hip_dep, migraphx_dep, json_glib_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsinfer_core_dep = declare_dependency(link_with : rdsinfer_core,
  include_directories : include_directories('.'),
  dependencies : [glib_dep, rds_plugin_common_dep, rds_headers_dep, hip_dep, migraphx_dep, json_glib_dep])

# The preprocessing kernel, built like the transform kernels (gst-libs/rds/meson.build): amdclang++ for the
# offload_arch targets, then the HIP compilation-unit marker made local. It shares rds-kernel-common.h with them.
rdsinfer_kernels_raw = custom_target('rds-infer-preprocess-raw',
  input : 'rds-infer-preprocess.hip',
  output : 'rds-infer-preprocess-raw.o',
  depfile : 'rds-infer-preprocess-raw.d',
  depend_files : [files('rds-infer-preprocess.h'), rds_kernel_headers],
  command : [amdclang, '-x', 'hip', hip_arch_args, '-fPIC', '-O2', '-std=c++17', '-fvisibility=hidden',
    '-D__HIP_PLATFORM_AMD__', '-I' + meson.current_source_dir(),
    '-I' + meson.project_source_root() / 'gst-libs', '-MD', '-MF', '@DEPFILE@',
    '-c', '@INPUT@', '-o', '@OUTPUT@'])
rdsinfer_kernels = custom_target('rds-infer-preprocess',
  input : rdsinfer_kernels_raw,
  output : 'rds-infer-preprocess.o',
  command : [objcopy, '--wildcard', '--localize-symbol=__hip_cuid_*', '@INPUT@', '@OUTPUT@'])
# The kernel object as a library, for the plugin and the preprocessing test; it needs the HIP runtime.
rdsinfer_preprocess = static_library('rdsinfer-preprocess',
  objects : rdsinfer_kernels,
  pic : true)
rdsinfer_preprocess_dep = declare_dependency(link_with : rdsinfer_preprocess,
  include_directories : include_directories('.'),
  dependencies : [hip_dep])

gstrdsinfer = library('gstrdsinfer',
  files('plugin.c', 'gstrdsinfer.c', 'rds-infer-output.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsinfer"'],
  dependencies : [rds_dep, gst_dep, gst_base_dep, gst_video_dep, hip_dep, migraphx_dep, json_glib_dep,
    rdsinfer_core_dep, rdsinfer_preprocess_dep],
  gnu_symbol_visibility : 'hidden',
  link_language : 'cpp',
  install_rpath : rds_infer_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsinfer
