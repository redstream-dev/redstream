common_test_env = {
  'LSAN_OPTIONS' : 'suppressions=@0@:print_suppressions=0'.format(meson.current_source_dir() / 'lsan.supp'),
  # Let impossible allocations return NULL under ASan, as they do with glibc (buf_surface tests this).
  # use_sigaltstack=0: Mesa's LLVM (loaded by the VA driver) installs its own malloc'd alternate signal stack on
  # the threads it runs on, and ASan then tries to munmap that buffer when the thread exits ("unable to unmmap",
  # EINVAL).
  'ASAN_OPTIONS' : 'halt_on_error=1:abort_on_error=1:print_summary=1:allocator_may_return_null=1:use_sigaltstack=0',
  'UBSAN_OPTIONS' : 'halt_on_error=1:abort_on_error=1:print_stacktrace=1',
}
# Sanitized plugins can't be loaded by gst-plugin-scanner (not linked with the ASan runtime): scan in-process.
# b_sanitize was a string option ('none', 'address,undefined') before meson 1.8 and is an array option since;
# formatting it to a string and looking for 'address' works with both.
asan = '@0@'.format(get_option('b_sanitize')).contains('address')
if asan
  common_test_env += {'GST_REGISTRY_FORK' : 'no'}
endif
# Element tests load our plugins from the build tree (scanned recursively) next to the stock system plugins,
# with a private registry so the user's cache is never touched.
test_env = environment(common_test_env + {
  'GST_PLUGIN_PATH_1_0' : meson.project_build_root() / 'gst',
  'GST_REGISTRY_1_0' : meson.current_build_dir() / 'registry.bin',
})
# Tests linking rds_internal_dep (a static libgstrds) must never load our plugin, which pulls in the shared
# libgstrds: with a stale registry and GST_REGISTRY_FORK=no, gst_init would scan (load) it in-process. They use
# no elements at all, so they get no plugin paths (neither ours nor the system's) and a registry of their own.
internal_test_env = environment(common_test_env + {
  'GST_PLUGIN_PATH_1_0' : '',
  'GST_PLUGIN_SYSTEM_PATH_1_0' : '',
  'GST_REGISTRY_1_0' : meson.current_build_dir() / 'registry-internal.bin',
})

# gst_check_dep: GstHarness-based tests (meta_copy, event).
test_deps = [rds_dep, gst_check_dep, hip_dep]

# GPU-free tests (hosted CI runs `meson test --suite cpu`): no device needed at run time, but building still
# needs the ROCm headers and libamdhip64.
cpu_tests = ['compat_remap', 'compat_header', 'compat_header_nomacros', 'buf_surface', 'memory', 'surface_view', 'buffer_pool',
  'allocation', 'meta_batch', 'meta_children',
  'meta_copy', 'event', 'compat_meta', 'compat_aliases', 'plugin_load', 'transform_params']
foreach t : cpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : test_deps),
    suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
endforeach

# GPU-free tests that call private library symbols: they link only the static internal copy of libgstrds
# (never rds_dep too, see internal_gpu_tests below).
internal_cpu_tests = ['transform_validate']
foreach t : internal_cpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : [rds_internal_dep]),
    suite : 'cpu', env : internal_test_env, protocol : 'tap')
endforeach

# DeepStream apps are often C++: check that the compat header builds and works there too.
if add_languages('cpp', required : false, native : false)
  test('compat_cpp', executable('compat_cpp', 'check' / 'compat_cpp.cpp', dependencies : [rds_dep],
      override_options : ['cpp_std=c++17']),
    suite : 'cpu', env : test_env, protocol : 'tap')
endif

# Tests that need a ROCm device; each skips itself when none is visible.
gpu_tests = ['gpu_smoke', 'buf_surface_hip', 'memory_hip', 'buffer_pool_hip']
foreach t : gpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : test_deps),
    suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120, depends : rds_plugins)
endforeach

# GPU tests that call private library symbols link the static internal copy of libgstrds instead.
# Such a test must never also link rds_dep, nor load one of our plugins in-process (the plugin pulls in the
# shared libgstrds): two copies of the library register the same GTypes twice. internal_test_env makes sure.
# transform_hip's CPU reference (transform-ref.h) needs libm.
internal_gpu_tests = ['kernel_smoke', 'transform_hip']
foreach t : internal_gpu_tests
  test(t, executable(t, 'check' / (t + '.c'),
      dependencies : [rds_internal_dep, gst_check_dep, hip_dep, cc.find_library('m', required : false)]),
    suite : 'gpu', env : internal_test_env, protocol : 'tap', is_parallel : false, timeout : 120)
endforeach

# Element tests that also exercise plugin-internal helpers (compiled in; they hold no GTypes, so linking the shared
# libgstrds next to the loaded plugin is fine). One executable, split by GLib test path into the two suites.
element_videoconvert_exe = executable('element_videoconvert',
  'check' / 'element_videoconvert.c', rdsvideo_util_src,
  dependencies : test_deps + [gst_base_dep, gst_video_dep, cc.find_library('m', required : false)])
test('element_videoconvert_cpu', element_videoconvert_exe, args : ['-p', '/cpu/videoconvert'],
  suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
test('element_videoconvert', element_videoconvert_exe, args : ['-p', '/gpu/videoconvert'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120, depends : rds_plugins)

element_videodec_exe = executable('element_videodec', 'check' / 'element_videodec.c',
  dependencies : test_deps + [gst_app_dep, gst_video_dep])
test('element_videodec_cpu', element_videodec_exe, args : ['-p', '/cpu/videodec'],
  suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
test('element_videodec', element_videodec_exe, args : ['-p', '/gpu/videodec'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 300, depends : rds_plugins)

element_streammux_exe = executable('element_streammux', 'check' / 'element_streammux.c',
  dependencies : test_deps + [gst_base_dep, gst_app_dep, gst_video_dep, cc.find_library('m', required : false)])
test('element_streammux_cpu', element_streammux_exe, args : ['-p', '/cpu/mux'],
  suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
test('element_streammux', element_streammux_exe, args : ['-p', '/gpu/mux'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 180, depends : rds_plugins)

element_streamdemux_exe = executable('element_streamdemux', 'check' / 'element_streamdemux.c',
  dependencies : test_deps + [gst_app_dep, gst_video_dep, cc.find_library('m', required : false)])
test('element_streamdemux_cpu', element_streamdemux_exe, args : ['-p', '/cpu/demux'],
  suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
test('element_streamdemux', element_streamdemux_exe, args : ['-p', '/gpu/demux'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120, depends : rds_plugins)

# Decode + convert integration: autoplugging, end to end against vah264dec + the CPU reference, and the soak.
# The soak (3000 1280x720 frames, leak check) is its own test entry, `meson test -C build pipeline_soak`; it takes
# about 9 s (also under ASan), so it stays in the gpu suite.
# The soak runs from its own build of the file, which (under ASan only) shrinks ASan's quarantine so VmRSS stays
# meaningful; the other tests keep the default quarantine.
pipeline_deps = test_deps + [gst_app_dep, gst_video_dep, cc.find_library('m', required : false)]
pipeline_exe = executable('pipeline_decode_convert', 'check' / 'pipeline_decode_convert.c',
  dependencies : pipeline_deps)
pipeline_soak_exe = executable('pipeline_soak', 'check' / 'pipeline_decode_convert.c',
  c_args : ['-DRDS_TEST_SMALL_QUARANTINE'], dependencies : pipeline_deps)
test('pipeline_decode_convert', pipeline_exe, args : ['-p', '/gpu/pipeline'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120, depends : rds_plugins)
test('pipeline_soak', pipeline_soak_exe, args : ['-p', '/gpu/soak'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 300, depends : rds_plugins)

# The dry run checks the compat mapping; gst-inspect-1.0 (when found) checks that the mapped decoder and
# converter resolve to real elements from the build tree. Not in ASan builds: the uninstrumented gst-inspect-1.0
# can't load sanitized plugins in-process (plugin_load covers loading there).
gst_inspect = find_program('gst-inspect-1.0', required : false)
test('rds_test3_dry_run',
  find_program('samples/rds_test3_dry_run.sh'),
  args : [rds_test3_exe, gst_inspect.found() and not asan ? gst_inspect.full_path() : ''],
  suite : 'cpu', env : test_env, depends : rds_plugins)
