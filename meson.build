project('redstream', 'c',
  version : '0.0.1',
  license : 'LGPL-2.1-or-later',
  meson_version : '>= 1.4',
  default_options : ['c_std=gnu11', 'warning_level=2', 'werror=true'])

api_version = '1.0'
cc = meson.get_compiler('c')
gst_dep = dependency('gstreamer-1.0', version : '>= 1.28')
gst_base_dep = dependency('gstreamer-base-1.0', version : '>= 1.28')
gst_video_dep = dependency('gstreamer-video-1.0', version : '>= 1.28')
gst_check_dep = dependency('gstreamer-check-1.0', version : '>= 1.28')
gst_app_dep = dependency('gstreamer-app-1.0', version : '>= 1.28')   # tests only (appsrc/appsink helpers)

# The ROCm image ships no hip.pc; locate the HIP runtime directly.
fs = import('fs')
rocm_path = get_option('rocm_path')
if not fs.is_absolute(rocm_path)
  error('rocm_path must be an absolute path; pass -Drocm_path=/path/to/rocm')
endif
if not fs.exists(rocm_path / 'include/hip/hip_runtime_api.h')
  error('HIP headers not found in @0@/include/hip; pass -Drocm_path=/path/to/rocm'.format(rocm_path))
endif
amdhip_lib = cc.find_library('amdhip64', dirs : [rocm_path / 'lib'], required : false)
if not amdhip_lib.found()
  error('libamdhip64 not found in @0@/lib; pass -Drocm_path=/path/to/rocm'.format(rocm_path))
endif
# Link by name (not the find_library path) so the generated .pc says -L<rocm>/lib -lamdhip64.
hip_dep = declare_dependency(
  compile_args : ['-D__HIP_PLATFORM_AMD__'],
  include_directories : include_directories(rocm_path / 'include', is_system : true),
  link_args : ['-L' + rocm_path / 'lib', '-lamdhip64'])

# HIP kernels: meson has no HIP language, so gst-libs/rds compiles them with amdclang++ in a custom_target.
amdclang = find_program(rocm_path / 'bin/amdclang++', required : false)
if not amdclang.found()
  error('amdclang++ not found in @0@/bin; pass -Drocm_path=/path/to/rocm'.format(rocm_path))
endif

# ROCm's llvm-objcopy understands the offload bundle sections; binutils objcopy is the fallback.
objcopy = find_program(rocm_path / 'llvm/bin/llvm-objcopy', 'objcopy')

rocdecode_lib = cc.find_library('rocdecode', dirs : [rocm_path / 'lib'], required : false)
if not rocdecode_lib.found() or not fs.exists(rocm_path / 'include/rocdecode/rocdecode.h')
  error('rocDecode (librocdecode, include/rocdecode/rocdecode.h) not found in @0@; pass -Drocm_path=/path/to/rocm'
    .format(rocm_path))
endif
rocdecode_dep = declare_dependency(
  include_directories : include_directories(rocm_path / 'include', is_system : true),
  dependencies : rocdecode_lib)

# The runtime linker finds ROCm in /opt/rocm/lib through ld.so.conf; other prefixes get an rpath.
rocm_path_norm = rocm_path.endswith('/') and rocm_path != '/' ? rocm_path.substring(0, -1) : rocm_path
rds_install_rpath = rocm_path_norm == '/opt/rocm' ? '' : rocm_path_norm / 'lib'

# Plugin metadata. RDS_ORIGIN is a placeholder until the public repository exists (TODO before release).
rds_origin = 'https://example.invalid/redstream'
rds_plugin_c_args = [
  '-DPACKAGE="redstream"',
  '-DPACKAGE_VERSION="@0@"'.format(meson.project_version()),
  '-DGST_PACKAGE_NAME="redstream"',
  '-DGST_PACKAGE_ORIGIN="@0@"'.format(rds_origin),
]

subdir('gst-libs/rds')
subdir('gst')
subdir('samples')
subdir('tests')
