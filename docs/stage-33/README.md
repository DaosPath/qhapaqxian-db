# Stage 33 — catalog helpers, stats collector, runtime hardening, KVM CI

Stage 33 lands five coordinated slices:

## Catalog scheduler/step helpers

- `QxCatalogLookupLatestSchedulerQueue`, `QxCatalogLookupLatestSchedulerLease`,
  `QxCatalogMaxStepSeqnoForTask`, and snapshot free helpers now live in
  `qx_catalog`.
- `qx_runtime` shed duplicate table-scan helpers and calls the catalog layer
  instead.

## Shared-memory stats collector (v1)

- `qx_stat` stores counters keyed by `(dboid, kind, entity_oid/runtime_class)`.
- GUC `qhapaqxian.track_stats` (default `true`) gates collection.
- SQL: `pg_qx_stat_reset(text)`, `pg_qx_stat_get_provider_stats()`.
- `qx_insert_trace` and `QxObserveRecord*` feed the collector.
- `pg_stat_qx_providers` now joins the collector instead of scanning all traces.

## Runtime hardening

- `qx_runtime_policy` maps capability tags to `allow_network`,
  `allow_privilege_escalation`, and `image_ref`, with
  `QX_CONTAINER_IMAGE_ALLOWLIST` enforcement.
- `qx_backend_supervisor` tracks container/microVM leases in shared memory.
- Typed launch requests are written to `LAUNCH_REQUEST_FILE` for the tool
  runner, which returns `CONTAINER_ID` / `VM_ID` and uses supervised waits on
  Unix.

## CI

- `scripts/ci/kvm-probe.sh` and `.github/workflows/qhapaqxian-kvm.yml` add a
  self-hosted KVM lane.
- `qhapaqxian-bootstrap.yml` runs `qx_stage3_observability` and an optional
  KVM probe step.

## Tests

- `src/test/regress/sql/qx_stage3_observability.sql` asserts provider
  `submit_count > 0` and `pg_qx_stat_reset` behavior.