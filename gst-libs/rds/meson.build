rds_headers = files('rds-prelude.h', 'rds-compat-remap.h', 'rds_compat.h', 'rds-buf-surface.h',
  'rds-memory.h', 'rds-buffer-pool.h', 'rds-osd-types.h', 'rds-meta.h',
  'rds-gst-meta.h', 'rds-event.h', 'rds-transform.h', 'rds.h', 'rds-compat-aliases.h',
  'rds-infer.h', 'rds-infer-custom-impl.h', 'rds_compat_infer.h')
install_headers(rds_headers, subdir : 'redstream-' + api_version / 'rds')

hip_arch_args = []
foreach a : get_option('offload_arch').split(',')
  if a.strip() != ''
    hip_arch_args += '--offload-arch=' + a.strip()
  endif
endforeach
if hip_arch_args.length() == 0
  error('offload_arch is empty; pass e.g. -Doffload_arch=gfx1030')
endif
# Headers every HIP kernel includes (also rdsinfer's, gst/rdsinfer/meson.build).
rds_kernel_headers = files('rds-transform-private.h', 'rds-kernel-common.h')
# Not built with b_sanitize: a custom_target doesn't inherit it.
rds_kernels_raw = custom_target('rds-transform-kernels-raw',
  input : 'rds-transform-kernels.hip',
  output : 'rds-transform-kernels-raw.o',
  depfile : 'rds-transform-kernels-raw.d',
  depend_files : rds_kernel_headers,
  command : [amdclang, '-x', 'hip', hip_arch_args, '-fPIC', '-O2', '-std=c++17', '-fvisibility=hidden',
    '-D__HIP_PLATFORM_AMD__', '-I' + meson.current_source_dir(), '-MD', '-MF', '@DEPFILE@',
    '-c', '@INPUT@', '-o', '@OUTPUT@'])
# clang gives the HIP compilation-unit marker __hip_cuid_<hash> default visibility whatever -fvisibility says;
# make it local so libgstrds exports only its public API.
rds_kernels = custom_target('rds-transform-kernels',
  input : rds_kernels_raw,
  output : 'rds-transform-kernels.o',
  command : [objcopy, '--wildcard', '--localize-symbol=__hip_cuid_*', '@INPUT@', '@OUTPUT@'])

rds_sources = files('rds-compat-remap.c', 'rds-buf-surface.c', 'rds-memory.c',
  'rds-buffer-pool.c', 'rds-meta.c', 'rds-meta-copy.c', 'rds-gst-meta.c', 'rds-event.c', 'rds-transform.c')
rds_c_args = ['-DG_LOG_DOMAIN="rds"', '-DBUILDING_RDS']
rds_private_deps = [gst_dep, gst_video_dep, hip_dep]

# HIP is linked privately; public headers never include it. gst-video is public (GstVideoFormat).
librds = library('gstrds-' + api_version,
  rds_sources,
  objects : rds_kernels,
  c_args : rds_c_args,
  include_directories : include_directories('..'),
  dependencies : rds_private_deps,
  gnu_symbol_visibility : 'hidden',
  version : meson.project_version(),
  install_rpath : rds_install_rpath,
  install : true)

# Static copy of the same library for tests that call private (hidden) symbols.
librds_internal = static_library('gstrds-internal',
  rds_sources,
  objects : rds_kernels,
  c_args : rds_c_args,
  include_directories : include_directories('..'),
  dependencies : rds_private_deps,
  gnu_symbol_visibility : 'hidden',
  pic : true,
  install : false)

rds_dep = declare_dependency(link_with : librds,
  include_directories : include_directories('..'),
  dependencies : [gst_dep, gst_video_dep])

rds_internal_dep = declare_dependency(link_with : librds_internal,
  include_directories : include_directories('..'),
  dependencies : [gst_dep, gst_video_dep, hip_dep])

import('pkgconfig').generate(librds,
  name : 'redstream-' + api_version,
  filebase : 'redstream-' + api_version,
  description : 'Redstream: DeepStream-compatible video analytics on AMD ROCm',
  subdirs : 'redstream-' + api_version,
  requires : ['gstreamer-1.0 >= 1.28', 'gstreamer-video-1.0 >= 1.28'])
