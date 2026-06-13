# QhapaqXian Observability Layer

Role:
- trace helpers, task/session metrics, budget visibility, and agent-aware
  `EXPLAIN` / `pg_stat_*` surfaces.

Current state:
- traces and operator views exist, but the observability plane is still
  thinner than the target state.
- `SHOW TRACE` now consumes `QxObserveSummarizeTraceDetail()` and returns a
  compact `trace_summary` column alongside normalized trace detail.
- scheduler-backed traces now summarize queue/runtime/provider/worker/retry
  fields too, so operator output no longer drops the live scheduler contract
  on `runtime.queue`, `runtime.dispatch`, `runtime.resume`, and
  `runtime.resume_dispatch`.
- `qx_stat` (Stage 33 v1) collects provider/principal/runtime-class/scheduler
  counters in shared memory when `qhapaqxian.track_stats` is enabled.
- `QxObserveRecord*` delegates to `QxStatReport*` for live counter updates.
- `pg_qx_stat_get_provider_stats()`, `pg_qx_stat_get_principal_stats()`,
  `pg_qx_stat_get_runtime_class_stats()`, and `pg_qx_stat_reset(text)` expose the
  collector to SQL callers.
- `pg_stat_qx_providers` now joins the shared-memory collector for submit,
  resume, and verified-receipt counters.
- `pg_stat_qx_principals` and `pg_stat_qx_runtime_classes` still use trace
  extraction for several counters until principal/runtime-class SRFs land.
- scheduler queue/worker/activity/ledger views derive state from the durable
  scheduler ledger.
- `pg_stat_qx_scheduler_activity` gives operators a single queue/runtime/
  provider rollup for current task state plus renew/reclaim/release/retry
  counts without opening the raw scheduler ledger tables directly.

Integration debt:
- principal/runtime-class stats SRFs and historical/SLO accounting remain open.
- richer operator dashboards and pre-aggregated long-horizon rollups are still
  future work.