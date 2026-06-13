# Stage 25: MicroVM Backend Scaffold

Stage 25 adds a broker-facing microVM backend scaffold under the runtime
subsystem. It landed before the later real QEMU-backed integration and owns the
contract shape, validation helpers, and receipt/attestation vocabulary that
`qx_runtime.c` consumes.

What landed:
- `qx_microvm_backend.c` and `qx_microvm_backend.h` define the microVM backend
  request/response contract;
- `qx_runtime_policy_resolve_microvm_assets()` maps capability tags and env
  defaults to `kernel_ref`, `initrd_ref`, and optional `snapshot_ref`;
- broker-facing request data is normalized into a stable launch payload;
- provider/runtime compatibility checks are centralized for `microvm` flows;
- receipt payloads include provider, principal, runtime class, sandbox,
  profile, image, snapshot, and timing fields in one serialization;
- attestation placeholders are explicit rather than implied;
- the runtime build now owns the microVM backend object file so the scaffold is
  part of the fork build.

Asset policy (Stage 25 gap closure):
- capability tags `kernel:`, `initrd:`, and `snapshot:` override env defaults;
- `QX_MICROVM_KERNEL` and `QX_MICROVM_INITRD` provide fallback paths;
- `QX_MICROVM_KERNEL_ALLOWLIST` and `QX_MICROVM_INITRD_ALLOWLIST` are enforced
  before microVM launch;
- optional `QX_MICROVM_SNAPSHOT_ALLOWLIST` validates `snapshot_ref` when present;
- `kernel_ref` and `initrd_ref` are emitted in `QxMicrovmBackendRequest` launch
  payloads;
- `qx_microvm_backend_validate_request()` rejects malformed snapshot references.

Backend responsibilities:
- validate `microvm://` provider contracts before the runtime accepts them;
- keep the provider kind and principal runtime class aligned;
- provide a stable receipt payload format for future signing and verification;
- expose a response contract that higher layers can inspect and normalize;
- keep launch mode and profile metadata separate from actual execution logic.

Explicit deferrals at landing time:
- no kernel-level container or VM launch;
- no hypervisor integration;
- no direct guest lifecycle management;
- no replacement for the existing brokered runner path;
- no claim that this is the final microVM runtime boundary.

How this maps to `provider kind = microvm`:
- `pg_qx_provider` still stores the catalog-side provider identity;
- `qx_microvm_backend` is the execution-contract layer that consumes that
  provider identity for the QEMU launch path;
- `qx_runtime.c` remains the orchestrator, while this backend holds the
  microVM-specific contract and validation logic;
- the scaffold is now the adapter between catalog policy and the actual QEMU
  backend provider.

Validation:
- build should include `src/backend/qx/runtime/qx_microvm_backend.c`;
- current runtime integration covers provider-contract validation, receipt
  serialization, attestation mode mapping, and real QEMU launch evidence;
- SQL regress helpers: `pg_qx_policy_validate_microvm_assets(text, text)`;
- `src/test/regress/sql/qx_stage25_microvm_policy.sql`.

Post-stage integration note:
- a later integration pass wired this contract into `qx_runtime.c` as a real
  QEMU `microvm` execution path;
- Windows is validated with QEMU `microvm` plus `tcg`;
- WSL/Linux is validated with QEMU `microvm` plus `kvm` when `/dev/kvm` is
  available;
- hosted CI should continue to use `tcg` unless a self-hosted runner exposes
  KVM;
- the remaining gap is not "no real microVM launch" anymore, but stronger VM
  lifecycle ownership, hypervisor process ownership, and dedicated snapshot
  lifecycle management.
- operational setup and validation commands live in
  `../real-runtime-backends.md`.