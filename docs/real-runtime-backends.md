# Real Runtime Backends

This runbook documents the first real container and microVM execution paths for
QhapaqXian DB. It is the operational companion to the Stage 22 runtime-class
contract and the Stage 24/25 backend scaffolds.

For the durable architecture decision, see
`docs/adr/0005-real-runtime-backends.md`. For common failures, see
`docs/runtime-troubleshooting.md`.

Current state:
- `container://` principals can be executed through a real Docker backend.
- `microvm://` principals can be executed through a real QEMU `microvm`
  backend.
- Windows is validated with QEMU `tcg`.
- WSL/Linux is validated with QEMU `kvm` when `/dev/kvm` is available.
- CI uses QEMU `tcg` because hosted runners should not be assumed to expose
  KVM.

This is no longer only a brokered class marker. The runtime now builds backend
requests, launches provider-specific execution, verifies the response, and
records trace evidence that identifies the actual runtime class.

## Runtime Contract

Container execution is controlled by these environment variables:

```sh
QX_CONTAINER_IMAGE=alpine:3.20
QX_DOCKER_CLI=/path/to/docker
DOCKER_HOST=npipe:////./pipe/dockerDesktopLinuxEngine
```

`QX_DOCKER_CLI` is optional when `docker` is already on `PATH`.
`DOCKER_HOST` is only needed when the selected Docker client needs an explicit
daemon endpoint, such as WSL calling Docker Desktop through the Windows named
pipe.

MicroVM execution is controlled by these environment variables:

```sh
QX_MICROVM_QEMU=/path/to/qemu-system-x86_64
QX_MICROVM_KERNEL=/path/to/vmlinuz-virt
QX_MICROVM_INITRD=/path/to/initramfs-qx-microvm.cpio.gz
QX_MICROVM_ACCEL=tcg
```

`QX_MICROVM_ACCEL` should be:
- `tcg` on Windows or generic CI.
- `kvm` on Linux/WSL when `/dev/kvm` is present and usable.

If `QX_MICROVM_KERNEL` or `QX_MICROVM_INITRD` are omitted, the runtime attempts
to resolve assets from:

```text
$top_builddir/microvm-assets/vmlinuz-virt
$top_builddir/microvm-assets/initramfs-qx-microvm.cpio.gz
$top_builddir/microvm-assets/initramfs-qx-microvm2.cpio.gz
```

`initramfs-qx-microvm.cpio.gz` is the preferred stable local asset. The
`microvm2` name is kept only as a fallback for older build directories.

## MicroVM Assets

The validated asset shape uses Alpine Linux:

- kernel: `vmlinuz-virt`
- base initramfs: Alpine `minirootfs`
- injected `/init` that prints `QX-MICROVM-BOOT-OK` and exits cleanly

The CI workflow prepares these assets before running the microVM smoke tests.
Local builds can use the same shape under `builddir/microvm-assets`.

## Windows Path

Windows is validated as a real backend path with:

- Docker Desktop for container execution.
- QEMU `microvm` with `-accel tcg` for microVM execution.

This path is stable but not fast. It is useful as a correctness path and for
developer machines that do not expose a Linux KVM device.

The important design decision is that Windows should not pretend to provide a
stable WHPX/KVM-equivalent microVM route for this fork today. The stable
microVM path on Windows is QEMU `microvm` plus TCG.

## WSL/Linux KVM Path

The accelerated path is WSL or Linux with KVM:

```powershell
wsl --install Ubuntu-24.04 --no-launch
```

Inside the Ubuntu distro:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential pkg-config bison flex libreadline-dev zlib1g-dev \
  libxml2-dev libxslt1-dev libssl-dev perl python3 python3-pip \
  ninja-build meson qemu-system-x86 qemu-utils curl cpio ca-certificates git
sudo usermod -aG kvm qx
test -e /dev/kvm
```

Build on the WSL ext4 filesystem, not directly under `/mnt/c`, when running the
Linux test matrix. The validated workspace copy was:

```text
/home/qx/qhapaqxian-db-wsl
```

This avoids Windows line-ending and filesystem-behavior issues in generated
build files. The Windows checkout does not need to be line-ending normalized to
run the WSL path; normalize only the isolated WSL copy if needed.

## Validation Commands

Windows build and suites:

```powershell
meson compile -C build-stage4-codex -j 2
meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs
meson test -C build-stage4-codex --suite postgresql:qhapaqxian_output --print-errorlogs
```

Windows focused real-backend regression:

```powershell
$env:QX_CONTAINER_IMAGE='alpine:3.20'
$env:QX_MICROVM_ACCEL='tcg'
$env:QX_MICROVM_KERNEL="$pwd\build-stage4-codex\microvm-assets\vmlinuz-virt"
$env:QX_MICROVM_INITRD="$pwd\build-stage4-codex\microvm-assets\initramfs-qx-microvm.cpio.gz"
```

Run the focused `qx_stage3_agentic` regression database with Docker real and
QEMU/TCG real backends enabled.

WSL/Linux build and suites:

```sh
meson compile -C build-kvm -j 4
meson test -C build-kvm --suite postgresql:setup --print-errorlogs
QX_MICROVM_ACCEL=kvm meson test -C build-kvm --suite postgresql:qhapaqxian_output --print-errorlogs
```

WSL focused real-backend regression:

```sh
export top_builddir=/home/qx/qhapaqxian-db-wsl/build-kvm
export QX_MICROVM_ACCEL=kvm
export QX_CONTAINER_IMAGE=alpine:3.20
export QX_DOCKER_CLI='/mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe'
export DOCKER_HOST='npipe:////./pipe/dockerDesktopLinuxEngine'
```

Expected backend evidence includes:

```text
PRINCIPAL_RUNTIME=microvm
PROVIDER_KIND=microvm
ATTESTATION=microvm_receipt_verified
backend_launch=qemu
microvm_accel=kvm
```

## Test Matrix

CI smoke lane:
- prepares `microvm-assets` from Alpine Linux;
- runs the real Docker runner with `QX_CONTAINER_IMAGE=alpine:3.20`;
- runs the real QEMU `microvm` runner with `QX_MICROVM_ACCEL=tcg`;
- requires `STATUS=ok`, runtime/provider class echo, verified attestation, and
  `backend_launch=docker` or `backend_launch=qemu`;
- runs focused `qx_stage3_agentic` and `qhapaqxian_output` regressions.

Integrated regression lane:
- `src/test/regress/sql/qx_stage3_agentic.sql` creates `host`, `container`, and
  `microvm` principals;
- it verifies provider/runtime compatibility in catalog state;
- it runs submit and resume paths through the runtime;
- it checks trace detail for sandbox, provider, provider kind, principal
  runtime, receipt algorithm, receipt nonce, verified signature, attestation,
  launch mode, restricted identity, and wall time;
- it exercises runtime-class observability through `pg_stat_qx_runtime_classes`.

Semantic decoder lane:
- `contrib/qhapaqxian_output/sql/semantic_messages.sql` verifies that semantic
  logical messages remain decodeable after provider/runtime metadata is emitted;
- event ordering is made explicit so output stays portable across collations.

Manual platform lane:
- Windows proves Docker plus QEMU `microvm`/`tcg`.
- WSL/Linux proves QEMU `microvm`/`kvm` when `/dev/kvm` exists.
- WSL may reuse Docker Desktop through Windows `docker.exe` when native WSL
  Docker integration is not configured.

## Trace Normalization

Regression output normalizes host-dependent trace fields:

- `microvm_accel=<accel>`
- `microvm_kernel=<kernel>`
- `restricted_identity=<identity>`
- `task=<task>`
- `wall_ms=<wall>`

This keeps regression fixtures portable while still asserting that backend
evidence exists.

## Operational Notes

Do not run PostgreSQL regression tests as root.

Do not assume `/dev/kvm` exists in GitHub-hosted CI. Use `tcg` there unless the
runner is explicitly self-hosted with KVM exposed.

When running WSL with Docker Desktop, it is acceptable to use Windows
`docker.exe` through the named-pipe daemon if native WSL Docker integration is
not enabled.

The runtime intentionally relaxes child-side resource limits for brokered
container and microVM launches when host identity preservation is required.
The parent still owns timeout enforcement, while Docker/QEMU receive enough
room to start real provider processes. The microVM runner also consumes the
same brokered timeout value, so QEMU is not killed earlier than the parent
profile allows.

For microVM providers the runtime applies a 120000 ms timeout floor to the
brokered execution profile. This is intentionally higher than the small
`isolated` profile default because Windows QEMU `microvm` with TCG can boot,
print `QX-MICROVM-BOOT-OK`, and shut down cleanly while still exceeding a
60000 ms parent wait on slower local runs. Host and container execution keep
their normal profile-specific timeout values.
