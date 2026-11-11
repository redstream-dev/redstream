# SPDX-License-Identifier: LGPL-2.1-or-later
# Plugin-private code shared by the plugin and the tests: the property mapping (8.0's nvv4l2h26xenc values to the
# VA encoders') and the QP string parsers. GLib only, always built.
rdsenc_props = static_library('rdsenc-props',
  files('rds-enc-props.c'),
  c_args : ['-DG_LOG_DOMAIN="rdsenc"'],
  dependencies : [dependency('glib-2.0')],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsenc_props_dep = declare_dependency(link_with : rdsenc_props,
  include_directories : include_directories('.'),
  dependencies : [dependency('glib-2.0')])

# The Annex-B helpers (filler stripping, the H.265 SPS window fix, the timestamp shift, intra pictures) and the caps
# decisions (the preferred profile, Baseline + B frames, the downstream level): GStreamer core only, so the cpu tests
# link it without any plugin.
rdsenc_gst = static_library('rdsenc-gst',
  files('rds-enc-nal.c', 'rds-enc-caps.c'),
  c_args : ['-DG_LOG_DOMAIN="rdsenc"'],
  dependencies : [gst_dep, rdsenc_props_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsenc_gst_dep = declare_dependency(link_with : rdsenc_gst,
  include_directories : include_directories('.'),
  dependencies : [gst_dep, rdsenc_props_dep])

# rdsh264enc and rdsh265enc: bins over the stock VA encoders (needed at run time only).
gstrdsenc = library('gstrdsenc',
  files('plugin.c', 'gstrdsencbin.c', 'gstrdsencbin-probes.c', 'gstrdsencbin-va.c', 'gstrdsh264enc.c',
    'gstrdsh265enc.c'),
  c_args : rds_plugin_c_args + ['-DG_LOG_DOMAIN="rdsenc"'],
  dependencies : [rds_dep, gst_dep, rdsenc_props_dep, rdsenc_gst_dep, rds_plugin_headers_dep],
  gnu_symbol_visibility : 'hidden',
  install_rpath : rds_install_rpath,
  install : true,
  install_dir : get_option('libdir') / 'gstreamer-1.0')
rds_plugins += gstrdsenc
