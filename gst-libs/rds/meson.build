rds_headers = files('rds-prelude.h', 'rds-compat-remap.h', 'rds_compat.h', 'rds-buf-surface.h',
  'rds-memory.h', 'rds-buffer-pool.h', 'rds-osd-types.h', 'rds-meta.h',
  'rds-gst-meta.h')
install_headers(rds_headers, subdir : 'redstream-' + api_version / 'rds')

# HIP is linked privately; public headers never include it. gst-video is public (GstVideoFormat).
librds = library('gstrds-' + api_version,
  files('rds-compat-remap.c', 'rds-buf-surface.c', 'rds-memory.c',
    'rds-buffer-pool.c', 'rds-meta.c', 'rds-meta-copy.c', 'rds-gst-meta.c'),
  c_args : ['-DG_LOG_DOMAIN="rds"', '-DBUILDING_RDS'],
  include_directories : include_directories('..'),
  dependencies : [gst_dep, gst_video_dep, hip_dep],
  gnu_symbol_visibility : 'hidden',
  version : meson.project_version(),
  install : true)

rds_dep = declare_dependency(link_with : librds,
  include_directories : include_directories('..'),
  dependencies : [gst_dep, gst_video_dep])

import('pkgconfig').generate(librds,
  name : 'redstream-' + api_version,
  filebase : 'redstream-' + api_version,
  description : 'Redstream: DeepStream-compatible video analytics on AMD ROCm',
  subdirs : 'redstream-' + api_version,
  requires : ['gstreamer-1.0 >= 1.28', 'gstreamer-video-1.0 >= 1.28'])
