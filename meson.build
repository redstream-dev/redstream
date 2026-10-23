project('redstream', 'c',
  version : '0.0.1',
  license : 'LGPL-2.1-or-later',
  meson_version : '>= 1.4',
  default_options : ['c_std=gnu11', 'warning_level=2', 'werror=true'])

api_version = '1.0'
cc = meson.get_compiler('c')
gst_dep = dependency('gstreamer-1.0', version : '>= 1.28')

subdir('gst-libs/rds')
subdir('samples')
subdir('tests')
