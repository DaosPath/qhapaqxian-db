# QhapaqXian Catalog Layer

Role:
- snapshot helpers over fork-owned catalogs for agents, identities, providers,
  principals, namespace policies, tools, sessions, tasks, attempts, and
  checkpoints.

Current state:
- lookup/free layer exists in `qx_catalog.c` and is the preferred read path for
  fork-owned runtime, security, scheduler, recovery, and observability code.
- recovery now consumes task, attempt, and checkpoint snapshots through
  `qx_catalog` list builders instead of open-coded table scans.
- planner now consumes session, agent, identity, task, and checkpoint snapshots
  through `qx_catalog` helpers for agent plan construction.
- security read helpers and namespace tool authorization now consume namespace
  policy, provider, principal, and tool snapshots through `qx_catalog`.
- security now also consumes catalog snapshots for operational identity lookup
  and registered-tool validation before write-time insertion paths run.
- runtime now consumes catalog snapshots for provider receipt-key validation,
  submit-time agent naming, attempt sequence lookup, budget validation, and
  RESUME TASK task/checkpoint metadata.
- command surfaces now consume catalog snapshots for `SHOW TRACE` task
  authorization and `REMEMBER`/`FETCH MEMORY` session or agent ownership
  checks.
- `SHOW TRACE` is now also a live observability consumer through
  `QxObserveSummarizeTraceDetail`, which emits compact summaries next to the
  normalized trace detail payload.

Integration debt:
- runtime write/update helpers still use direct tuple access because they
  mutate task and attempt rows;
- scheduler and broader observability consumers still need migration off
  open-coded syscache or catalog scans where practical;
- security write paths still use direct catalog access where rows are being
  inserted or mutated;
- bootstrap catalog definitions remain in `src/include/catalog/` and upstream
  catalog machinery.
