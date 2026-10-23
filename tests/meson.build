test_env = environment()
test_env.set('LSAN_OPTIONS', 'suppressions=@0@:print_suppressions=0'.format(
  meson.current_source_dir() / 'lsan.supp'))
test_env.set('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')

# GPU-free tests (hosted CI runs `meson test --suite cpu`).
cpu_tests = ['compat_remap', 'compat_header', 'compat_header_nomacros']
foreach t : cpu_tests
  test(t, executable(t, 'check' / t + '.c', dependencies : [rds_dep, gst_check_dep]),
    suite : 'cpu', env : test_env, protocol : 'tap')
endforeach

# Tests that need a ROCm device; each skips itself when none is visible.
gpu_tests = ['gpu_smoke']
foreach t : gpu_tests
  test(t, executable(t, 'check' / t + '.c', dependencies : [rds_dep, gst_check_dep, hip_dep]),
    suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false)
endforeach

test('rds_test3_dry_run',
  find_program('samples/rds_test3_dry_run.sh'),
  args : [rds_test3_exe], suite : 'cpu', env : test_env)
