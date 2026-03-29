# Copyright (c) 2026, QhapaqXian DB contributors

qhapaqxian_launcher_sources = files(
  'qhapaqxian_db_launcher.c',
)

qhapaqxian_launcher_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
  '--NAME', 'qhapaqxian-db',
  '--FILEDESC', 'QhapaqXian DB Windows launcher',])

qhapaqxian_launcher = executable('qhapaqxian-db',
  qhapaqxian_launcher_sources,
  kwargs: default_bin_args,
)
bin_targets += qhapaqxian_launcher
