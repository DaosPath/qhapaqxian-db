# Runtime Troubleshooting

This guide covers common failures in the real Docker and QEMU backend paths.
The setup and validation runbook lives in `docs/real-runtime-backends.md`.

## Quick Checks

Confirm the selected backend is not silently falling back to a simulated path:

```text
backend_launch=docker
backend_launch=qemu
ATTESTATION=container_receipt_verified
ATTESTATION=microvm_receipt_verified
QX-MICROVM-BOOT-OK
```

If those markers are absent, do not count the run as a real backend pass.

## Docker

If Docker launch fails on Windows:
- verify Docker Desktop is running;
- verify `docker version` works from PowerShell;
- set `QX_DOCKER_CLI` only when `docker` is not on `PATH`;
- use `QX_CONTAINER_IMAGE=alpine:3.20` for the known smoke image.

If Docker launch fails from WSL while Docker Desktop works on Windows:
- either enable Docker Desktop WSL integration for the distro;
- or call Windows Docker explicitly with
  `QX_DOCKER_CLI='/mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe'`;
- set `DOCKER_HOST='npipe:////./pipe/dockerDesktopLinuxEngine'` when using the
  Windows named-pipe daemon.

If image pull fails:
- run `docker pull alpine:3.20` manually;
- check proxy, DNS, registry, and Docker Desktop network settings;
- keep test images small and explicit so smoke failures are easy to reproduce.

## QEMU MicroVM

If QEMU is not found:
- set `QX_MICROVM_QEMU` to the full `qemu-system-x86_64` path;
- on Linux/WSL, install `qemu-system-x86`;
- on Windows, confirm the QEMU install directory is on `PATH` or set the env var.

If microVM assets are missing:
- check `$top_builddir/microvm-assets/vmlinuz-virt`;
- check `$top_builddir/microvm-assets/initramfs-qx-microvm2.cpio.gz`;
- set `QX_MICROVM_KERNEL` and `QX_MICROVM_INITRD` explicitly when using a custom
  asset directory.

If the guest does not print `QX-MICROVM-BOOT-OK`:
- verify the initramfs contains an executable `/init`;
- verify the initramfs was created with `cpio -o -H newc | gzip -9`;
- try `QX_MICROVM_ACCEL=tcg` to separate accelerator problems from asset
  problems.

If QEMU times out:
- use the microVM profile timeout from the runbook;
- prefer WSL/Linux KVM for faster local validation;
- keep TCG expectations modest because it is a correctness path, not a
  performance path.

## KVM In WSL/Linux

If `QX_MICROVM_ACCEL=kvm` fails:
- verify `/dev/kvm` exists;
- verify the user belongs to the `kvm` group;
- start a new shell or WSL session after adding group membership;
- run the direct QEMU boot smoke before running PostgreSQL regressions.

If `/dev/kvm` is absent in WSL:
- confirm the Windows host supports virtualization and it is enabled in BIOS;
- update WSL and use a distro that exposes KVM;
- fall back to `QX_MICROVM_ACCEL=tcg` for correctness validation.

## Build And Workspace

Do not run PostgreSQL regression tests as root.

For Linux/WSL validation, build on an ext4-backed WSL path such as:

```text
/home/qx/qhapaqxian-db-wsl
```

Avoid building directly under `/mnt/c` for the Linux matrix. If line endings
break generated files in the WSL copy, normalize only the WSL copy and leave
the Windows checkout policy unchanged.

## CI

Hosted CI should use `QX_MICROVM_ACCEL=tcg`.

Use a self-hosted Linux runner for a required KVM lane. That runner should
expose `/dev/kvm`, install QEMU, and run the same smoke markers documented in
`docs/real-runtime-backends.md`.

