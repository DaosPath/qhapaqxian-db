# Stage 34 — observability collector v2 and historical/SLO surfaces

Stage 34 extends the Stage 33 `qx_stat` collector with principal/runtime-class
SRFs, collector-backed views, and a minimal historical/SLO operator layer.

## Collector-backed live stats

- `pg_qx_stat_get_principal_stats()` and
  `pg_qx_stat_get_runtime_class_stats()` expose shared-memory counters keyed by
  principal OID and runtime class.
- `pg_stat_qx_principals` and `pg_stat_qx_runtime_classes` read the collector
  instead of scanning traces.
- Stage 26–27 follow-up views `pg_stat_qx_scheduler_collected` and
  `pg_stat_qx_recovery` reuse the same collector plane.

## Historical snapshots (Stage 34 v3)

- durable catalog `pg_qx_stat_history` stores point-in-time collector rollups;
- `pg_qx_stat_snapshot(text scope)` captures live counters for
  `provider`, `principal`, `runtime_class`, `scheduler`, `recovery`, or `all`;
- `pg_qx_stat_get_history(text scope, timestamptz since)` reads durable rows;
- `pg_qx_stat_prune_history(timestamptz cutoff)` removes old rows for the
  current database so operators can enforce retention from SQL or a scheduler;
- `pg_stat_qx_stat_history` exposes the history SRF to operators.

## SLO and dashboard surfaces

- `pg_stat_qx_slo_providers` derives receipt-verification and checkpoint
  coverage percentages from live collector counters;
- `pg_stat_qx_operator_dashboard` rolls up entity counts, execution totals,
  receipt outcomes, and the latest snapshot timestamp per surface.
- `pg_stat_qx_operator_export` adds a stable JSONB payload per dashboard
  surface for downstream operator UIs or export jobs.

## Tests

- `src/test/regress/sql/qx_stage3_observability.sql` asserts collector reset
  behavior, snapshot/history persistence, SLO visibility, dashboard rows,
  JSON export visibility, and history pruning.

## Intentional deferrals

- automatic snapshot scheduling remains open;
- external graphical operator UI remains future work, but the SQL export
  contract is now present.
