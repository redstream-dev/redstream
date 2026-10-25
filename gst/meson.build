# Plugin targets; tests depend on them because `meson test` only builds what the tests need.
rds_plugins = []
subdir('common')
subdir('rdsvideo')
subdir('rdsmux')
if rds_infer_enabled
  subdir('rdsinfer')
endif
if rds_tracker_enabled
  subdir('rdstracker')
endif
