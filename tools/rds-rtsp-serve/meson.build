# SPDX-License-Identifier: LGPL-2.1-or-later
# The RTSP test server and its TCP fault relay: a static library for the tests (rtsp_test_server and the rdsurisrcbin
# stream tests) and rds-rtsp-serve. Without gst-rtsp-server the library is a stub whose constructors return NULL with
# the reason, so the tests still build and skip; the tool is built only with it.
rtsp_test_server_deps = [gst_dep, dependency('gio-2.0')]
if gst_rtsp_server_dep.found()
  rtsp_test_server_deps += gst_rtsp_server_dep
endif
rtsp_test_server_lib = static_library('rtsp-test-server', 'rtsp-test-server.c',
  c_args : ['-DG_LOG_DOMAIN="rds-rtsp-serve"']
    + (gst_rtsp_server_dep.found() ? ['-DRDS_HAVE_RTSP_SERVER'] : []),
  dependencies : rtsp_test_server_deps)
rtsp_test_server_dep = declare_dependency(link_with : rtsp_test_server_lib,
  include_directories : include_directories('.'),
  dependencies : rtsp_test_server_deps)

if gst_rtsp_server_dep.found()
  rds_rtsp_serve_exe = executable('rds-rtsp-serve', 'rds-rtsp-serve.c',
    c_args : ['-DG_LOG_DOMAIN="rds-rtsp-serve"'],
    dependencies : [rtsp_test_server_dep],
    install : true)
endif
