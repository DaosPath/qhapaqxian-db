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
- `pg_stat_qx_providers`, `pg_stat_qx_principals`, and
  `pg_stat_qx_runtime_classes` now reuse the builtin
  `pg_qx_trace_detail_value()` extractor instead of open-coding
  `position('key=value;')` parsing in SQL.
- scheduler queue/worker/activity/ledger views now derive their state from the
  durable scheduler ledger, while provider/principal/runtime-class views keep
  using the shared trace-detail extractor.
- `pg_stat_qx_scheduler_activity` now gives operators a single queue/runtime/
  provider rollup for current task state plus renew/reclaim/release/retry
  counts without opening the raw scheduler ledger tables directly.
- `pg_stat_qx_scheduler_retry_backoff` now exposes current retry-delay and
  eligibility state per task, including the owning scheduler slot.
- `pg_stat_qx_scheduler_worker_balance` now exposes per-slot owned task counts
  per queue/runtime/provider key, including empty slots, so balancing skew is
  visible without replaying the raw ledger by hand.

Integration debt:
- richer stats collection and a dedicated operator dashboard are still open.
- the `pg_stat_qx_*` views still aggregate directly over `pg_qx_trace` or the
  scheduler ledger and do not yet consume pre-aggregated observe state or a
  dedicated stats collector.
