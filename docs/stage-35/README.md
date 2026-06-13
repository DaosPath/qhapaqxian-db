# Stage 35 — CI loopback path, backend IDs in traces, catalog runtime writes

## Stage intent

Stage 35 stabilizes hosted CI for `qhapaqxian_output`, surfaces real
container/microVM instance IDs in runtime traces, and moves the first runtime
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
- `qx_stage3_agentic` asserts `has_container_id` and
  `container_id_populated` on the real Docker submit trace.

## What is intentionally deferred

- Full OCI cgroup/seccomp policy compilation.
- Deeper cross-process microVM lifecycle ownership beyond supervisor
  register/release.
- Historical/SLO accounting and operator dashboards (Stage 34 follow-up).
- Remaining runtime insert paths (`qx_insert_*`) and scheduler ledger writes
  still live in `qx_runtime.c`.

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
  (full 225-subtest sweep)

## Files touched

- `contrib/qhapaqxian_output/{sql/semantic_messages.sql,expected/semantic_messages.out,meson.build}`
- `meson.build`
- `src/backend/qx/runtime/qx_runtime.c`
- `src/backend/qx/catalog/qx_catalog.c`
- `src/include/qx/qx_catalog.h`
- `src/backend/commands/tracecmds.c`
- `src/test/regress/{sql/qx_stage3_agentic.sql,expected/qx_stage3_agentic.out}`

## Next stage handoff

- Migrate remaining runtime catalog inserts and scheduler ledger writes into
  `qx_catalog`.
- Extend receipt/trace contracts so runner receipts include `container_id` /
  `vm_id` at the top level consistently.
- Add historical stats persistence on top of the Stage 34 collector surfaces.