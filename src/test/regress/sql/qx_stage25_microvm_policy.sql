-- Stage 25 microVM policy regression (no QEMU required)

SELECT pg_qx_policy_validate_microvm_assets(
  '/opt/qx/vmlinuz-virt',
  '/opt/qx/initramfs-qx-microvm.cpio.gz'
);

SELECT pg_qx_policy_validate_microvm_assets(
  'vmlinuz-virt',
  'initramfs-qx-microvm.cpio.gz'
);