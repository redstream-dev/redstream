rds_headers = files('rds-compat-remap.h')
install_headers(rds_headers, subdir : 'redstream-' + api_version / 'rds')

librds = library('gstrds-' + api_version,
  files('rds-compat-remap.c'),
  c_args : ['-DG_LOG_DOMAIN="rds"'],
  include_directories : include_directories('..'),
  dependencies : [gst_dep],
  version : meson.project_version(),
  install : true)

rds_dep = declare_dependency(link_with : librds,
  include_directories : include_directories('..'),
  dependencies : [gst_dep])

import('pkgconfig').generate(librds,
  name : 'redstream-' + api_version,
  filebase : 'redstream-' + api_version,
  description : 'Redstream: DeepStream-compatible video analytics on AMD ROCm',
  subdirs : 'redstream-' + api_version,
  requires : ['gstreamer-1.0'])
