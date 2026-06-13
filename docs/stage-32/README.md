# Stage 32 - Catalog Snapshot Helper Layer

## Stage intent

Stage 32 introduces a shared catalog snapshot layer for QhapaqXian Engine.
It is a consolidation step, not a new runtime feature by itself.

## What landed

- public snapshot structs under `src/include/qx/qx_catalog.h` for:
  - agents
  - identities
  - providers
  - principals
  - namespace policies
  - tools
  - sessions
  - tasks
  - attempts
  - checkpoints
- backend lookup/fill/free helpers under `src/backend/qx/catalog/`;
- a single place for translating `syscache` and tuple reads into engine-owned C structs;
- provider/principal merge logic so callers can observe the effective provider contract without open-coding repeated joins;
- list snapshot builders for task, attempt, and checkpoint catalog rows,
  filtered by database and owner;
- the recovery scanner now consumes `qx_catalog` snapshots instead of
  open-coding task/attempt/checkpoint table scans;
- the agent planner now resolves RUN TASK and RESUME TASK catalog state
  through `qx_catalog` snapshots instead of direct syscache/tuple reads;
- security read helpers and namespace tool authorization now consume
  `qx_catalog` snapshots for namespace policies, principals, providers, and
  tool/provider/principal contract metadata;
- security operational identity lookup and registered-tool validation now also
  consume `qx_catalog` snapshots instead of open-coding syscache walks across
  identity, tool, principal, and provider rows;
- runtime receipt-key lookup, submit-time agent name lookup, attempt sequence
  lookup, budget validation, and RESUME TASK task/checkpoint reads now consume
  `qx_catalog` snapshots;
- runtime insert paths for tasks, attempts, steps, events, traces, and
  checkpoints, plus scheduler queue/lease/heartbeat ledger writes, now route
  through `QxCatalogInsert*` helpers instead of open-coded tuple insertion in
  `qx_runtime.c`;
- provider, principal, and distinct runtime-class list snapshot builders now
  back `pg_qx_stat_get_provider_stats`, `pg_qx_stat_get_principal_stats`, and
  `pg_qx_stat_get_runtime_class_stats` instead of open-coded catalog scans;
- recovery task/attempt summaries now also consult the durable scheduler
  queue/lease ledgers before requesting runtime requeue/fence hooks, so
  startup/failover scans stay idempotent once the autonomous supervisor has
  already written the latest recovery evidence;
- `SHOW TRACE` task authorization and memory command session/agent lookups now
  consume `qx_catalog` snapshots before scanning or returning command data;
- `SHOW TRACE` now also emits an operator-oriented `trace_summary` column built
  through `qx_observe` so the observability scaffold is connected to a real
  command surface instead of remaining helper-only;
- `pg_stat_qx_providers`, `pg_stat_qx_principals`, and
  `pg_stat_qx_runtime_classes` now share the builtin
  `pg_qx_trace_detail_value()` extractor so those observability views no longer
  open-code raw `position('key=value;')` matching against trace contracts;
- the microVM asset resolver now prefers the stable
  `initramfs-qx-microvm.cpio.gz` before the legacy `microvm2` fallback;
- the real microVM execution profile now uses the 120000 ms brokered timeout
  floor end to end so Windows QEMU `microvm` with TCG can boot and shut down
  cleanly without weakening host/container sandbox profiles;
- build wiring so the catalog helper layer is part of the fork-owned `src/backend/qx` boundary.

## Why this exists

- runtime, security, planner, and recovery were each beginning to grow their own catalog access code;
- duplicating `SearchSysCache*`, `GETSTRUCT`, and text-attribute extraction across subsystems increases rebase cost and drift risk;
- a shared snapshot layer makes later refactors safer when catalog layouts evolve again.

## What is intentionally deferred

- no new catalog objects were added in this stage;
- no `syscache` definitions were changed in this stage;
- recovery flow now depends on this helper layer for task, attempt, and
  checkpoint snapshots;
- planner flow now depends on this helper layer for session, agent, identity,
  task, and checkpoint snapshots used by `EXPLAIN AGENT`;
- security DDL/write paths for identity, agent, session, memory, and policy
  objects now route tuple inserts through `QxCatalogInsert*` helpers;
- dedicated `qx_stage32_catalog` regression exercises the catalog layer
  indirectly through CREATE/START SESSION and stats-backed observability views.

## Why this stage matters

- it reduces duplicate catalog-read code across fork-owned subsystems;
- it gives later scheduler, recovery, and observability work a stable
  snapshot shape to consume;
- it lowers rebase cost when catalog layouts evolve again.

## Integration direction

- remaining runtime write/update helpers should stay explicit about tuple
  mutation, while pure reads should keep moving onto `qx_catalog`;
- security DDL/write validation should migrate where it benefits from shared
  snapshots without hiding catalog-update semantics;
- scheduler code should use snapshot structs when it needs a stable
  cross-subsystem view of task state;
- observability code should use the same layer before new `pg_stat_qx_*` surfaces are expanded.
- SQL-facing observability views should prefer shared trace-detail extraction
  helpers over repeated ad-hoc contract parsing.

## Known gaps

- this stage is mainly an internal maintainability improvement;
- recovery, planner, security authorization/validation, runtime read/write
  paths, and DDL tuple inserts for core catalog objects are now migrated
  consumers;
- ALTER-command catalog updates remain in command layers by design.

## Validation

- `src/test/regress/sql/qx_stage32_catalog.sql` provides dedicated helper-layer
  regression coverage through CREATE NAMESPACE POLICY/PROVIDER/PRINCIPAL/TOOL,
  CREATE AGENT, START SESSION, and stats-backed observability assertions.
- Validation also remains indirect through build and higher-level integration.
- Current migration validation:
  `meson compile -C build-stage4-codex -j 2`.
- Stage 32 also reran `meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs`,
  which passed 3/3.
- The downstream focused `qx_stage3_agentic` regression now also validates the
  shared-observe/catalog consumers behind
  `pg_qx_scheduler_worker_slot_count()`,
  `pg_stat_qx_scheduler_retry_backoff`, and
  `pg_stat_qx_scheduler_worker_balance` on real Docker/QEMU lanes.
- `planner` and `recovery` were checked for remaining direct catalog reads;
  no `SearchSysCache`, `ReleaseSysCache`, `HeapTuple`, `GETSTRUCT`,
  `SysCacheGetAttr`, `table_open`, or `heap_getnext` references remain in
  those two consumers.
- `security` authorization/read helpers, operational identity lookup,
  registered-tool validation, and identity tuple insertion were migrated to
  `qx_catalog`.
- `agentcmds`, `sessioncmds`, `qxpolicycmds` CREATE paths, and `qx_memory`
  now route tuple inserts and duplicate checks through `qx_catalog` helpers.
- `runtime` now uses `qx_catalog` for pure resume/read metadata, budget
  validation, provider receipt-key reads, and all durable insert paths for
  tasks, attempts, steps, events, traces, checkpoints, and scheduler ledgers.
- `tracecmds` and `memorycmds` no longer open-code syscache task, session, or
  agent authorization reads; `SHOW TRACE` still scans `pg_qx_trace` rows to
  return trace output, but now also renders `trace_summary` through
  `qx_observe`.
- `pg_stat_qx_providers`, `pg_stat_qx_principals`, and
  `pg_stat_qx_runtime_classes` now call `pg_qx_trace_detail_value()` instead of
  repeating raw `position(...)` contract parsing inside each view.
- Windows QEMU/TCG microVM execution is validated with the 120000 ms brokered
  microVM timeout floor; an earlier child-side timeout could still kill the
  wrapper even after a successful guest boot marker under real TCG.
- the Windows launcher now skips `CREATE_SUSPENDED` for real
  `container`/`microvm` backend launches when no sandbox job object is being
  attached; this removed a reproducible false timeout where the exact saved
  microVM resume request succeeded under direct replay but hung only through
  the runtime launcher path.
- Focused real `qx_stage3_agentic` validation was run with direct `pg_regress`
  and no `parallel_schedule`, using Docker Desktop 29.3.1,
  `QX_MICROVM_ACCEL=tcg`,
  `QX_MICROVM_KERNEL=build-stage4-codex/microvm-assets/vmlinuz-virt`, and
  `QX_MICROVM_INITRD=build-stage4-codex/microvm-assets/initramfs-qx-microvm.cpio.gz`;
  it passed 1/1 with the real Docker container and QEMU microVM paths, plus
  explicit assertions that post-reclaim startup/failover recovery leaves one
  recovery queue row and one reclaimed lease row for the repaired checkpointed
  attempt, and that a stale attempt with no durable checkpoint first fails
  closed as a queued task with a `failed` attempt, zero checkpoints, one
  `RETRY` queue row, and one reclaimed lease row, then later re-enters the real
  submit path through autonomous retry intake, dispatches the `urgent` retry
  before the competing `high` retry on durable trace order, and reaches a fresh
  checkpoint.

## Next stage handoff

- scheduler, runtime, observability, and remaining security write
  work should migrate practical open-coded read access onto `qx_catalog`;
- future refactors should prefer expanding the snapshot layer instead of
  adding new scattered `syscache` helpers in each subsystem.
