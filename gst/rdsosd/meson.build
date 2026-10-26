# The drawing core (shapes as draw commands shaded onto a CPU RGBA plane, Pango for text masks; no GStreamer, no GPU;
# the meta structs come from the libgstrds headers), shared by the plugin and the tests. No fused multiply-add: the
# blend must round as the same code does compiled for the GPU (rds-osd-shapes.h). GCC ignores the header's
# contraction pragma, so everything that includes it (through rdsosd_core_dep too) builds with -ffp-contract=off.
rdsosd_core = static_library('rdsosd-core',
  files('rds-osd-draw.c', 'rds-osd-plan.c', 'rds-osd-shapes.c'),
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

# The GPU drawing kernels, built like rdsinfer's (gst/rdsinfer/meson.build), with contraction off: the kernel blends
# with the same code as the CPU path and must round alike (amdclang's default is fast-honor-pragmas). The context
# (rds-osd-gpu.c) and the kernel object make the static rdsosd-gpu, for the plugin and the tests.
rdsosd_kernels_raw = custom_target('rds-osd-gpu-raw',
  input : 'rds-osd-gpu.hip',
  output : 'rds-osd-gpu-raw.o',
  depfile : 'rds-osd-gpu-raw.d',
  depend_files : files('rds-osd-gpu.h', 'rds-osd-shapes.h'),
  command : [amdclang, '-x', 'hip', hip_arch_args, '-fPIC', '-O2', '-std=c++17', '-fvisibility=hidden',
    '-ffp-contract=off', '-D__HIP_PLATFORM_AMD__', '-I' + meson.current_source_dir(), '-MD', '-MF', '@DEPFILE@',
    '-c', '@INPUT@', '-o', '@OUTPUT@'])
rdsosd_kernels = custom_target('rds-osd-gpu-kernels',
  input : rdsosd_kernels_raw,
  output : 'rds-osd-gpu-kernels.o',
  command : [objcopy, '--wildcard', '--localize-symbol=__hip_cuid_*', '@INPUT@', '@OUTPUT@'])
rdsosd_gpu = static_library('rdsosd-gpu',
  files('rds-osd-gpu.c'),
  objects : rdsosd_kernels,
  c_args : ['-DG_LOG_DOMAIN="rdsosd"'],
  dependencies : [rdsosd_core_dep, hip_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsosd_gpu_dep = declare_dependency(link_with : rdsosd_gpu,
  dependencies : [rdsosd_core_dep, hip_dep])
