test_env = environment()
test_env.set('LSAN_OPTIONS', 'suppressions=@0@:print_suppressions=0'.format(
  meson.current_source_dir() / 'lsan.supp'))
# Let impossible allocations return NULL under ASan, as they do with glibc (buf_surface tests this).
test_env.set('ASAN_OPTIONS', 'halt_on_error=1:abort_on_error=1:print_summary=1:allocator_may_return_null=1')
test_env.set('UBSAN_OPTIONS', 'halt_on_error=1:abort_on_error=1:print_stacktrace=1')
# Element tests load our plugins from the build tree (scanned recursively) next to the stock system plugins,
# with a private registry so the user's cache is never touched.
test_env.set('GST_PLUGIN_PATH_1_0', meson.project_build_root() / 'gst')
test_env.set('GST_REGISTRY_1_0', meson.current_build_dir() / 'registry.bin')
# Sanitized plugins can't be loaded by gst-plugin-scanner (not linked with the ASan runtime): scan in-process.
if get_option('b_sanitize') != 'none'
  test_env.set('GST_REGISTRY_FORK', 'no')
endif

# gst_check_dep: GstHarness-based tests (meta_copy, event).
test_deps = [rds_dep, gst_check_dep, hip_dep]

# GPU-free tests (hosted CI runs `meson test --suite cpu`): no device needed at run time, but building still
# needs the ROCm headers and libamdhip64.
cpu_tests = ['compat_remap', 'compat_header', 'compat_header_nomacros', 'buf_surface', 'memory', 'buffer_pool',
  'allocation', 'meta_batch', 'meta_children',
  'meta_copy', 'event', 'compat_meta', 'compat_aliases', 'plugin_load']
foreach t : cpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : test_deps),
    suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
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
internal_gpu_tests = ['kernel_smoke']
foreach t : internal_gpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : [rds_internal_dep, gst_check_dep, hip_dep]),
    suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120, depends : rds_plugins)
endforeach

test('rds_test3_dry_run',
  find_program('samples/rds_test3_dry_run.sh'),
  args : [rds_test3_exe], suite : 'cpu', env : test_env)
