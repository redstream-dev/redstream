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

# The Annex-B helpers (filler stripping, the H.265 SPS window fix, the timestamp shift): GStreamer core only, so the
# cpu tests link it without any plugin.
rdsenc_nal = static_library('rdsenc-nal',
  files('rds-enc-nal.c'),
  c_args : ['-DG_LOG_DOMAIN="rdsenc"'],
  dependencies : [gst_dep],
  gnu_symbol_visibility : 'hidden',
  pic : true)
rdsenc_nal_dep = declare_dependency(link_with : rdsenc_nal,
  include_directories : include_directories('.'),
  dependencies : [gst_dep])
