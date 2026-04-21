# Stage 28: Observability Scaffold

Stage 28 introduces the observability foundation for provider, principal, and
runtime-class surfaces. It started as scaffold-only work, but it now has real
callers in `SHOW TRACE` and the shared extractor used by `pg_stat_qx_*`.

What landed:
- `src/include/qx/qx_observe.h` defines canonical provider, principal,
  runtime-class, and trace-summary contracts;
- `src/backend/qx/observe/qx_observe.c` provides init, accumulation, and
  rendering helpers for those contracts;
- the trace helpers normalize operator-facing contracts into a stable
  `key=value;key=value` summary form;
- `SHOW TRACE` now consumes `QxObserveSummarizeTraceDetail()` and exposes a
  compact `trace_summary` column;
- `pg_qx_trace_detail_value()` now exposes shared trace-detail extraction for
  SQL-facing observability views, replacing repeated `position(...)` parsing in
  `pg_stat_qx_providers`, `pg_stat_qx_principals`, and
  `pg_stat_qx_runtime_classes`;
- `pg_stat_qx_scheduler_activity` now rolls the durable scheduler ledger into
  an operator-facing queue/runtime/provider surface with live task counts plus
  renew/reclaim/release/retry activity counters;
- provider/principal/runtime-class stats are modeled as explicit structures so
  future catalog-backed reporting does not need to reopen the shape debate.

Observability model:
- provider stats are the top-level execution boundary for runtime receipts,
  attestation, and charge accounting;
- principal stats are the execution identity boundary for sandbox, signer, and
  runtime class;
- runtime-class stats are the cross-cutting aggregation boundary that will later
  feed system views and operator dashboards;
- trace summaries are the operator-facing contract for task, submit, resume,
  checkpoint, and receipt evidence.

Current links:
- `pg_stat_qx_providers`
- `pg_stat_qx_principals`
- `pg_stat_qx_runtime_classes`
- `pg_stat_qx_scheduler_activity`
- scheduler ledger views for queue, lease, and heartbeat snapshots, including
  focused lease-renewal, startup recovery, and failover rebuild evidence
- richer `SHOW TRACE` output

Expected future links:
- `EXPLAIN AGENT`
- future recovery and replication diagnostics that consume the same summary
  contracts instead of inventing separate formats

Known deferrals:
- no catalog-backed statistics collector exists yet for these contracts;
- the helper layer is intentionally format-first, not query-engine-driven;
- this stage does not change planner, runtime, or WAL semantics directly.

Validation target:
- current real coverage is indirect through:
  - `meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs`
  - `meson test -C build-stage4-codex --suite postgresql:qhapaqxian_output --print-errorlogs`
  - focused real smoke on Windows with `initdb`, `pg_ctl`, `RUN TASK`, lease
    renewal, startup recovery, failover rebuild, and live reads from
    `pg_stat_qx_*` plus `SHOW TRACE`
  - focused real `qx_stage3_agentic` assertions that the container scheduler
    activity row reports renew/reclaim/release/retry history and stays hidden
    from an unprivileged observer role
