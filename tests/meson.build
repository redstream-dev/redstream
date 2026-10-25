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
  # MIGraphX dlopens its GPU target (<libdir>/migraphx/lib/libmigraphx_gpu.so) through its own RUNPATH
  # ($ORIGIN/../lib/migraphx/lib). ASan's dlopen interceptor makes the runtime the caller, so that RUNPATH isn't
  # searched: name the directory explicitly.
  if rds_infer_enabled
    common_test_env += {'LD_LIBRARY_PATH' : migraphx_libdir / 'migraphx' / 'lib'}
  endif
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
  'meta_copy', 'event', 'compat_meta', 'compat_aliases', 'transform_params']
foreach t : cpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : test_deps),
    suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
endforeach

# plugin_load also checks that rdsinfer loads when the build has it.
test('plugin_load', executable('plugin_load', 'check' / 'plugin_load.c', dependencies : test_deps,
    c_args : ['-DRDS_HAVE_INFER=@0@'.format(rds_infer_enabled ? 1 : 0)]),
  suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)

# GPU-free tests that call private library symbols: they link only the static internal copy of libgstrds
# (never rds_dep too, see internal_gpu_tests below).
internal_cpu_tests = ['transform_validate']
foreach t : internal_cpu_tests
  test(t, executable(t, 'check' / (t + '.c'), dependencies : [rds_internal_dep]),
    suite : 'cpu', env : internal_test_env, protocol : 'tap')
endforeach

# DeepStream apps are often C++: check that the compat header builds and works there too.
test('compat_cpp', executable('compat_cpp', 'check' / 'compat_cpp.cpp', dependencies : [rds_dep]),
  suite : 'cpu', env : test_env, protocol : 'tap')

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

if rds_infer_enabled
  test('element_infer_cpu', executable('element_infer', 'check' / 'element_infer.c', dependencies : test_deps),
    suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
  # The config parser on its own, against the corpus in check/infer-configs. It uses no GStreamer, so it gets the
  # environment without plugin paths.
  test('infer_config', executable('infer_config', 'check' / 'infer_config.c',
      dependencies : [rdsinfer_core_dep],
      link_language : 'cpp',
      c_args : ['-DRDS_INFER_CONFIGS="@0@"'.format(meson.current_source_dir() / 'check' / 'infer-configs')]),
    suite : 'cpu', env : internal_test_env, protocol : 'tap')
  # The built-in YOLO parser and the detection post-processing, against hand-computed cases and a naive reference
  # (check/infer-post-ref.h). Prints the parser and post-processing times on a YOLO11n-sized output.
  test('infer_post', executable('infer_post', 'check' / 'infer_post.c',
      dependencies : [rdsinfer_core_dep],
      link_language : 'cpp'),
    suite : 'cpu', env : internal_test_env, protocol : 'tap', timeout : 120)
  # The preprocessing kernel against its CPU reference (check/infer-preprocess-ref.h) and against
  # rds_buf_surf_transform; prints the max differences and the batch timing. Internal: the static libgstrds only.
  test('infer_preprocess', executable('infer_preprocess', 'check' / 'infer_preprocess.c',
      dependencies : [rds_internal_dep, rdsinfer_core_dep, rdsinfer_preprocess_dep, hip_dep,
        cc.find_library('m', required : false)],
      link_language : 'cpp'),
    suite : 'gpu', env : internal_test_env, protocol : 'tap', is_parallel : false, timeout : 300)
  # The MIGraphX engine and its program cache on the committed scale2 models (gpu), and its ONNX reader (cpu). The
  # YOLO11n cases (their own entry) run only once tests/models/fetch-yolo11n.sh has fetched the model; they cache
  # its programs in the build directory. A first YOLO11n compile takes about 30 s on a small GPU.
  infer_engine_exe = executable('infer_engine', 'check' / 'infer_engine.c',
    dependencies : [rdsinfer_core_dep, hip_dep, gst_dep, cc.find_library('m', required : false)],
    link_language : 'cpp',
    c_args : [
      '-DRDS_TEST_MODELS="@0@"'.format(meson.current_source_dir() / 'models'),
      '-DRDS_TEST_CACHE_MODELS="@0@"'.format(meson.project_source_root() / '.cache' / 'models'),
      '-DRDS_TEST_BUILD_DIR="@0@"'.format(meson.current_build_dir()),
    ])
  test('infer_engine_cpu', infer_engine_exe, args : ['-p', '/cpu/infer-engine'],
    suite : 'cpu', env : internal_test_env, protocol : 'tap')
  test('infer_engine', infer_engine_exe, args : ['-p', '/gpu/infer-engine'],
    suite : 'gpu', env : internal_test_env, protocol : 'tap', is_parallel : false, timeout : 600)
  test('infer_engine_yolo', infer_engine_exe, args : ['-p', '/gpu/infer-yolo'],
    suite : ['gpu', 'yolo'], env : internal_test_env, protocol : 'tap', is_parallel : false, timeout : 900)

  # Test parser modules (never installed), loaded by path. The fixture is a DeepStream-style parser compiled against
  # the compat header, from two translation units that both include the parser header; the same sources are built
  # again with hidden visibility. The no-ABI module lacks the version symbol, the bad-ABI one has a wrong version,
  # and the dep-ABI one gets it only from a helper library it links. compat-only is C++ that includes only
  # rds_compat.h.
  fixture_parser_src = files('check' / 'infer-parser-fixture.cpp', 'check' / 'infer-parser-fixture-boxes.cpp')
  fixture_parser_deps = [rds_dep.partial_dependency(compile_args : true, includes : true)]
  noabi_src = files('check' / 'infer-parser-noabi.c')
  infer_parser_fixture = shared_module('infer-parser-fixture', fixture_parser_src,
    cpp_args : ['-DUSE_REDSTREAM'], dependencies : fixture_parser_deps)
  infer_parser_hidden = shared_module('infer-parser-hidden', fixture_parser_src,
    cpp_args : ['-DUSE_REDSTREAM'], dependencies : fixture_parser_deps, gnu_symbol_visibility : 'hidden')
  infer_parser_noabi = shared_module('infer-parser-noabi', noabi_src)
  infer_parser_badabi = shared_module('infer-parser-badabi', noabi_src, c_args : ['-DRDS_TEST_ABI_VERSION=999'])
  infer_parser_abihelper = shared_library('infer-parser-abihelper', noabi_src,
    c_args : ['-DRDS_TEST_ABI_HELPER', '-DRDS_TEST_ABI_VERSION=1'])
  infer_parser_depabi = shared_module('infer-parser-depabi', noabi_src, c_args : ['-DRDS_TEST_ABI_FROM_DEP'],
    link_with : infer_parser_abihelper)
  infer_compat_only = shared_module('infer-compat-only', 'check' / 'infer-compat-only.cpp',
    dependencies : fixture_parser_deps)
  # export_dynamic: this executable's own copy of the version symbol is visible to dlsym, so the no-ABI test
  # checks that the loader looks only in the module.
  test('infer_abi', executable('infer_abi', 'check' / 'infer_abi.cpp', 'check' / 'infer_abi_noleak.cpp',
      dependencies : [rds_dep, rdsinfer_core_dep],
      export_dynamic : true,
      cpp_args : [
        '-DRDS_TEST_PARSER_FIXTURE="@0@"'.format(infer_parser_fixture.full_path()),
        '-DRDS_TEST_PARSER_HIDDEN="@0@"'.format(infer_parser_hidden.full_path()),
        '-DRDS_TEST_PARSER_NOABI="@0@"'.format(infer_parser_noabi.full_path()),
        '-DRDS_TEST_PARSER_BADABI="@0@"'.format(infer_parser_badabi.full_path()),
        '-DRDS_TEST_PARSER_DEPABI="@0@"'.format(infer_parser_depabi.full_path()),
        '-DRDS_TEST_COMPAT_ONLY="@0@"'.format(infer_compat_only.full_path()),
      ]),
    suite : 'cpu', env : test_env, protocol : 'tap',
    depends : [infer_parser_fixture, infer_parser_hidden, infer_parser_noabi, infer_parser_badabi,
      infer_parser_depabi, infer_compat_only])
endif

element_streamdemux_exe = executable('element_streamdemux', 'check' / 'element_streamdemux.c',
  dependencies : test_deps + [gst_app_dep, gst_video_dep, cc.find_library('m', required : false)])
test('element_streamdemux_cpu', element_streamdemux_exe, args : ['-p', '/cpu/demux'],
  suite : 'cpu', env : test_env, protocol : 'tap', depends : rds_plugins)
test('element_streamdemux', element_streamdemux_exe, args : ['-p', '/gpu/demux'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120, depends : rds_plugins)

# Decode + convert integration: autoplugging, end to end against vah264dec + the CPU reference, and the soak.
# The soak (3000 1280x720 frames, leak check) is its own test entry, `meson test -C build pipeline_soak`; it takes
# about 9 s (also under ASan), so it stays in the gpu suite.
pipeline_deps = test_deps + [gst_app_dep, gst_video_dep, cc.find_library('m', required : false)]
pipeline_exe = executable('pipeline_decode_convert', 'check' / 'pipeline_decode_convert.c',
  dependencies : pipeline_deps)
test('pipeline_decode_convert', pipeline_exe, args : ['-p', '/gpu/pipeline'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 120, depends : rds_plugins)
test('pipeline_soak', pipeline_exe, args : ['-p', '/gpu/soak'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 300, depends : rds_plugins)

# Multi-source mux/demux integration: mixed sources against the CPU reference, a stalled source, dynamic sources,
# and (its own entry, `pipeline_mux_soak`) four looped 720p sources with the leak check.
pipeline_mux_exe = executable('pipeline_mux', 'check' / 'pipeline_mux.c', dependencies : pipeline_deps)
test('pipeline_mux', pipeline_mux_exe, args : ['-p', '/gpu/mux-pipeline'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 180, depends : rds_plugins)
test('pipeline_mux_soak', pipeline_mux_exe, args : ['-p', '/gpu/mux-soak'],
  suite : 'gpu', env : test_env, protocol : 'tap', is_parallel : false, timeout : 300, depends : rds_plugins)

# The dry run checks the compat mapping; gst-inspect-1.0 (when found) checks that the mapped decoder and
# converter (and rdsinfer, when built) resolve to real elements from the build tree. Not in ASan builds: the
# uninstrumented gst-inspect-1.0 can't load sanitized plugins in-process (plugin_load covers loading there).
gst_inspect = find_program('gst-inspect-1.0', required : false)
test('rds_test3_dry_run',
  find_program('samples/rds_test3_dry_run.sh'),
  args : [rds_test3_exe, gst_inspect.found() and not asan ? gst_inspect.full_path() : '',
    rds_infer_enabled ? 'infer' : ''],
  suite : 'cpu', env : test_env, depends : rds_plugins)
