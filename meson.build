project('redstream', 'c',
  version : '0.0.1',
  license : 'LGPL-2.1-or-later',
  meson_version : '>= 1.4',
  default_options : ['c_std=gnu11', 'warning_level=2', 'werror=true'])

api_version = '1.0'
cc = meson.get_compiler('c')
gst_dep = dependency('gstreamer-1.0', version : '>= 1.28')
gst_video_dep = dependency('gstreamer-video-1.0', version : '>= 1.28')
gst_check_dep = dependency('gstreamer-check-1.0', version : '>= 1.28')

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
hip_dep = declare_dependency(
  compile_args : ['-D__HIP_PLATFORM_AMD__'],
  include_directories : include_directories(rocm_path / 'include', is_system : true),
  dependencies : amdhip_lib)

subdir('gst-libs/rds')
subdir('samples')
subdir('tests')
