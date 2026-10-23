test_env = environment()
test_env.set('LSAN_OPTIONS', 'suppressions=@0@:print_suppressions=0'.format(
  meson.current_source_dir() / 'lsan.supp'))
# Let impossible allocations return NULL under ASan, as they do with glibc (buf_surface tests this).
test_env.set('ASAN_OPTIONS', 'halt_on_error=1:abort_on_error=1:print_summary=1:allocator_may_return_null=1')
test_env.set('UBSAN_OPTIONS', 'halt_on_error=1:abort_on_error=1:print_stacktrace=1')

# gst_check_dep is for GstHarness-based tests in later tasks.
test_deps = [rds_dep, gst_check_dep, hip_dep]

# GPU-free tests (hosted CI runs `meson test --suite cpu`).
cpu_tests = ['compat_remap', 'compat_header', 'compat_header_nomacros', 'buf_surface', 'memory', 'buffer_pool',
  'allocation']
foreach t : cpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : test_deps),
    suite : 'cpu', env : test_env, protocol : 'tap')
endforeach

# Tests that need a ROCm device; each skips itself when none is visible.
gpu_tests = ['gpu_smoke', 'buf_surface_hip', 'memory_hip', 'buffer_pool_hip']
foreach t : gpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : test_deps),
    suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120)
endforeach

test('rds_test3_dry_run',
  find_program('samples/rds_test3_dry_run.sh'),
  args : [rds_test3_exe], suite : 'cpu', env : test_env)
