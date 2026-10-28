# SPDX-License-Identifier: LGPL-2.1-or-later
project('redstream', 'c',
  version : '0.0.1',
  license : 'LGPL-2.1-or-later AND MIT',
  license_files : ['LICENSE', 'samples/LICENSE'],
  meson_version : '>= 1.4',
  default_options : ['c_std=gnu11', 'cpp_std=c++17', 'warning_level=2', 'werror=true'])

# C++: the MIGraphX engine (C++ API) and the compat header's C++ checks.
add_languages('cpp', native : false)

api_version = '1.0'
cc = meson.get_compiler('c')
cxx = meson.get_compiler('cpp')
gst_dep = dependency('gstreamer-1.0', version : '>= 1.28')
gst_base_dep = dependency('gstreamer-base-1.0', version : '>= 1.28')
gst_video_dep = dependency('gstreamer-video-1.0', version : '>= 1.28')
gst_check_dep = dependency('gstreamer-check-1.0', version : '>= 1.28')
gst_app_dep = dependency('gstreamer-app-1.0', version : '>= 1.28')   # tests only (appsrc/appsink helpers)
gst_rtsp_dep = dependency('gstreamer-rtsp-1.0', version : '>= 1.28')   # rdsurisrcbin (RTSP lower transports)
# Optional: an in-process RTSP server for tests.
gst_rtsp_server_dep = dependency('gstreamer-rtsp-server-1.0', version : '>= 1.28', required : false)

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

# libyaml reads the YAML configs of rdsinfer (nvinfer-style) and rdstracker (low-level tracker configs), through the
# shared reader in gst/common.
yaml_dep = dependency('yaml-0.1', required : false)

# rdsinfer needs MIGraphX (C++ API over libmigraphx_c), libyaml (nvinfer-style YAML configs) and json-glib (compiled
# program manifests). With -Dinfer=auto, a missing one disables rdsinfer, rds-compile and their tests.
infer_opt = get_option('infer')
rds_infer_enabled = false
if not infer_opt.disabled()
  infer_missing = []
  migraphx_path = get_option('migraphx_path')
  migraphx_dep = dependency('', required : false)
  migraphx_libdir = ''
  foreach dir : migraphx_path != '' ? [migraphx_path] : [rocm_path / 'extras-10', rocm_path]
    if not migraphx_dep.found() and fs.exists(dir / 'include/migraphx/migraphx.hpp')
      migraphx_lib = cxx.find_library('migraphx_c', dirs : [dir / 'lib'], required : false)
      if migraphx_lib.found()
        migraphx_dep = declare_dependency(
          include_directories : include_directories(dir / 'include', is_system : true),
          dependencies : migraphx_lib)
        migraphx_libdir = dir / 'lib'
      endif
    endif
  endforeach
  if not migraphx_dep.found()
    infer_missing += 'MIGraphX (include/migraphx/migraphx.hpp + libmigraphx_c; set -Dmigraphx_path)'
  endif
  if not yaml_dep.found()
    infer_missing += 'libyaml (yaml-0.1)'
  endif
  json_glib_dep = dependency('json-glib-1.0', required : false)
  if not json_glib_dep.found()
    infer_missing += 'json-glib (json-glib-1.0)'
  endif
  if infer_missing.length() == 0
    rds_infer_enabled = true
    # MIGraphX may live outside the linker path (ROCm 10 puts it in <rocm>/extras-10/lib): rdsinfer and rds-compile
    # get an install rpath to it, next to the ROCm one.
    rds_infer_install_rpath = ':'.join([rds_install_rpath,
      migraphx_libdir == rocm_path_norm / 'lib' ? '' : migraphx_libdir]).strip(':')
  elif infer_opt.enabled()
    error('rdsinfer: missing ' + ', '.join(infer_missing))
  else
    message('rdsinfer disabled: missing ' + ', '.join(infer_missing))
  endif
endif

# rdstracker needs only libyaml. With -Dtracker=auto, a missing libyaml disables rdstracker and its tests.
tracker_opt = get_option('tracker')
rds_tracker_enabled = false
if not tracker_opt.disabled()
  if yaml_dep.found()
    rds_tracker_enabled = true
  elif tracker_opt.enabled()
    error('rdstracker: missing libyaml (yaml-0.1)')
  else
    message('rdstracker disabled: missing libyaml (yaml-0.1)')
  endif
endif

# rdsosd draws with Cairo and Pango. With -Dosd=auto, a missing one disables rdsosd and its tests.
osd_opt = get_option('osd')
rds_osd_enabled = false
if not osd_opt.disabled()
  osd_missing = []
  cairo_dep = dependency('cairo', required : false)
  if not cairo_dep.found()
    osd_missing += 'cairo'
  endif
  pangocairo_dep = dependency('pangocairo', required : false)
  if not pangocairo_dep.found()
    osd_missing += 'pangocairo'
  endif
  if osd_missing.length() == 0
    rds_osd_enabled = true
  elif osd_opt.enabled()
    error('rdsosd: missing ' + ', '.join(osd_missing))
  else
    message('rdsosd disabled: missing ' + ', '.join(osd_missing))
  endif
endif

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
subdir('tools')
subdir('tests')
