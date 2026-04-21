# ADR 0005: Real Container And MicroVM Backends

Status: accepted

Note:
- This ADR records the first real backend execution decision for
  `container://` and `microvm://` principals.
- Use `STATUS.md` for landed state and `docs/real-runtime-backends.md` for
  operational commands.

Decision:
- `container://` principals use Docker as the first real container backend.
- `microvm://` principals use QEMU `microvm` as the first real microVM backend.
- Windows validates QEMU `microvm` through `tcg`.
- WSL/Linux validates QEMU `microvm` through `kvm` when `/dev/kvm` is present.
- Hosted CI validates QEMU `microvm` through `tcg`; KVM requires a self-hosted
  Linux runner or a developer WSL/Linux environment with `/dev/kvm`.

Why:
- Docker gives the fork a real, widely available container launch path without
  inventing an OCI runtime inside PostgreSQL.
- QEMU `microvm` gives the fork a concrete VM-shaped execution path with a
  small machine model and deterministic smoke assets.
- Windows WHPX was not adopted as the stable route because the reliable path
  observed in this bootstrap is QEMU `microvm` plus TCG.
- WSL/Linux with KVM is the accelerated path that preserves the same QEMU
  backend contract while avoiding the cost of TCG.
- Hosted CI should not assume `/dev/kvm`; TCG keeps the smoke lane portable.

Consequences:
- The runtime can now require real backend evidence such as
  `backend_launch=docker` or `backend_launch=qemu`.
- Regression output must normalize host-dependent fields such as accelerator,
  kernel path, restricted identity, task id, and wall time.
- Docker/QEMU launch is real backend execution, but not yet the final
  isolation product.
- Future work should harden OCI policy, cgroups, seccomp, rootless mode,
  microVM lifecycle supervision, image/snapshot policy, and stronger
  attestation roots.

Rejected alternatives:
- Treating `container://` and `microvm://` as simulated broker-only classes was
  rejected once the runtime had typed backend contracts.
- Making WHPX the Windows microVM baseline was rejected for this bootstrap in
  favor of the stable QEMU `microvm` plus TCG route.
- Requiring KVM everywhere was rejected because hosted CI and many Windows
  developer environments do not expose `/dev/kvm`.

