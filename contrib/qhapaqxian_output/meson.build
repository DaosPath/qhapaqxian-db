# Copyright (c) 2022-2024, PostgreSQL Global Development Group

qhapaqxian_output_sources = files(
  'qhapaqxian_output.c',
)

if host_system == 'windows'
  qhapaqxian_output_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'qhapaqxian_output',
    '--FILEDESC', 'qhapaqxian_output - QhapaqXian semantic logical output plugin',])
endif

qhapaqxian_output = shared_module('qhapaqxian_output',
  qhapaqxian_output_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += qhapaqxian_output

tests += {
  'name': 'qhapaqxian_output',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'semantic_messages',
    ],
    'regress_args': [
      '--temp-config', files('logical.conf'),
    ],
    'env': {
      'QX_CONTAINER_IMAGE': 'alpine:3.20',
      'QX_MICROVM_ACCEL': 'tcg',
      'QX_MICROVM_KERNEL': meson.build_root() / 'microvm-assets' / 'vmlinuz-virt',
      'QX_MICROVM_INITRD': meson.build_root() / 'microvm-assets' / 'initramfs-qx-microvm.cpio.gz',
    },
    'runningcheck': false,
  },
}
