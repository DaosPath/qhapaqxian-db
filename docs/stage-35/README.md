# Stage 35 — CI loopback path, backend IDs in traces, catalog runtime writes

## Stage intent

Stage 35 stabilizes hosted CI for `qhapaqxian_output`, surfaces real
container/microVM instance IDs in runtime traces, and moves runtime catalog
write paths behind `qx_catalog` helpers.

## What landed

- `contrib/qhapaqxian_output/sql/semantic_messages.sql` now resumes a
  loopback `search_docs` task instead of a TCG microVM `summarize_docs` task,
  removing the Windows/CI flake on `RESUME TASK`.
- `contrib/qhapaqxian_output/meson.build` exports `QX_CONTAINER_IMAGE`,
  `QX_MICROVM_ACCEL`, `QX_MICROVM_KERNEL`, and `QX_MICROVM_INITRD` into the
  regress environment; root `meson.build` applies per-test `env` overrides.
- `qx_runtime.c` records `container_id` and `vm_id` in
  `runtime.external_{submit,resume}` traces and uses the real runner ID for
  supervisor register/release.
- `tracecmds.c` normalizes `container_id` / `vm_id` placeholders in
  `SHOW TRACE`, strips duplicated receipt tails from long trace payloads, and
  keeps multi-occurrence `vm_id` replacement stable.
- `qx_catalog` gained `QxCatalogUpdateTaskRuntime`,
  `QxCatalogUpdateAttemptState`, and `QxCatalogChargeTaskBudget`; runtime no
  longer open-codes those syscache writes.
- Stage 32 follow-up migrated all runtime insert paths and scheduler ledger
  writes to `QxCatalogInsert*` helpers.
- Runner responses emit top-level `CONTAINER_ID` / `VM_ID` lines; runtime
  parses them into trace payloads and supervisor lease registration.
- `qx_stage3_agentic` asserts `has_container_id` / `container_id_populated`
  on the real Docker submit trace and `has_vm_id` / `vm_id_populated` on the
  real QEMU microVM resume trace.

## Stage 35 follow-up (supervisor + cgroup)

- container policy compilation now emits `cgroup=host|private|isolated` in the OCI
  profile and forwards `cgroup_mode` through launch requests to the runner;
- seccomp compilation now accepts `no-new-privileges`, `runtime-default`,
  `strict`, and `unconfined`, so hardened profiles can be asserted without
  Docker availability;
- the tool runner applies `--cgroupns host|private` on real Docker launches;
- `pg_qx_backend_supervisor_list()`, `pg_qx_stat_get_backend_supervisor_stats()`,
  and `pg_stat_qx_backend_supervisor` expose shared-memory active backend leases and
  register/release/fence counters across database processes;
- `qx_stage35_supervisor` uses a second `dblink` session to validate cross-process
  visibility plus list/stats/idempotence without Docker.

## What is intentionally deferred

- Full cgroup namespace ownership beyond Docker CLI flags.
- External container/microVM termination and orphan-process reaping beyond
  shared lease register/release/fence.
- External graphical operator UI beyond the Stage 34 SQL export surface.

## Why this stage matters

Hosted CI can trust `qhapaqxian_output` without microVM wall-clock flakes,
operators can correlate traces with real Docker/QEMU instance IDs, and runtime
catalog writes now follow the same consolidation direction as Stage 32 reads.

## Validation

- Windows: `meson compile -C build-stage4-codex -j 2`
- Windows: `meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs`
- Windows: `meson test -C build-stage4-codex --suite postgresql:qhapaqxian_output --print-errorlogs`
- Windows: focused `qx_stage3_agentic` with Docker Desktop running,
  `PG_TEST_EXTRA=docker`, `QX_CONTAINER_IMAGE=alpine:3.20`, and TCG microVM
  assets under `build-stage4-codex/microvm-assets/`
- Windows: `meson test -C build-stage4-codex regress/regress --print-errorlogs`

## Files touched

- `contrib/qhapaqxian_output/{sql/semantic_messages.sql,expected/semantic_messages.out,meson.build}`
- `meson.build`
- `src/backend/qx/runtime/qx_runtime.c`
- `src/backend/qx/catalog/qx_catalog.c`
- `src/include/qx/qx_catalog.h`
- `src/backend/commands/tracecmds.c`
- `src/test/regress/{sql/qx_stage3_agentic.sql,expected/qx_stage3_agentic.out}`

## Next stage handoff

- Materialize full OCI/rootless profiles and kernel-level namespace ownership.
- Add external VM/container termination/reaping beyond shared-memory fence
  accounting when a portable process-control plane is available.
