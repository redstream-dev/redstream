# Command-line tools.
if rds_infer_enabled
  # rds-compile builds rdsinfer's compiled program ahead of time (trtexec's role for nvinfer), with the element's own
  # config parser and engine.
  rds_compile_exe = executable('rds-compile', 'rds-compile' / 'rds-compile.c',
    c_args : ['-DG_LOG_DOMAIN="rds-compile"'],
    dependencies : [rdsinfer_core_dep],
    link_language : 'cpp',
    install_rpath : rds_infer_install_rpath,
    install : true)
endif
