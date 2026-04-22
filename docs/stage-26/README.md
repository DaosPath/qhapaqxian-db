# Stage 26: Scheduler Scaffold

Stage 26 creates the first dedicated scheduler subsystem boundary for
QhapaqXian DB. It started as the durable-data and API scaffold, and it now
also includes the first real autonomous supervisor loop that consumes those
ledger rows from background workers.

What landed:
- scheduler-specific public headers under `src/include/qx/`;
- backend scheduler scaffolding under `src/backend/qx/scheduler/`;
- durable state structs for task envelopes, queue snapshots, lease snapshots,
  heartbeat snapshots, and a simple scheduler ledger;
- helper APIs for queue key construction, priority weighting, lease/heartbeat
  expiry checks, and human-readable snapshots;
- runtime task submission and resume paths now materialize scheduler envelopes
  and snapshots before emitting `runtime.queue`, `runtime.dispatch`,
  `runtime.resume`, and `runtime.resume_dispatch` traces;
- runtime task submission and resume paths now also persist a first durable
  scheduler ledger in `pg_qx_scheduler_queue`,
  `pg_qx_scheduler_lease`, and `pg_qx_scheduler_heartbeat`;
- runtime now appends durable lease-release and scheduler-release heartbeat
  snapshots when a worker hands off at checkpoint or completes an attempt;
- runtime startup recovery now appends durable recovery-queue rows for
  checkpointed tasks before resume and reclaim lease/heartbeat rows for fenced
  stale attempts;
- runtime failover rebuild now uses the same durable recovery queue and
  reclaim ledger path;
- a focused worker tick now renews held leases by appending durable lease and
  `scheduler-renew` heartbeat snapshots;
- postmaster startup now registers a static scheduler launcher bgworker, and
  that launcher spawns one dynamic scheduler worker per connectable database;
- pg_regress temp clusters constrain the autonomous launcher to the canonical
  `regression` database so scheduler workers do not hold connections to
  short-lived core-test databases such as `regression_utf8`;
- each database worker now wakes periodically, scans running tasks against the
  durable lease ledger, renews a held lease once, then reclaims stale work and
  appends the matching recovery queue / reclaim heartbeat rows;
- checkpoint-backed stale running attempts are now repaired in place by moving
  task and attempt state back to `checkpointed` before startup/failover hooks
  see them;
- the integrated real-backend regression now exercises both checkpointed-task
  startup requeue, autonomous lease renewal, checkpoint-backed stale-running
  reclaim repair, and failover rebuild against durable scheduler ledger rows;
- `pg_stat_qx_tasks` now exposes live scheduler queue/runtime/provider/worker
  state by reusing the scheduler trace contract already emitted by runtime;
- `pg_stat_qx_scheduler_queues` now rolls those same runtime traces up into
  queue-level operator state, now sourced from the latest durable scheduler
  ledger row per task, including retry windows and lease/heartbeat timing;
- `pg_stat_qx_scheduler_workers` now exposes the dispatched worker lanes seen
  by runtime, now sourced from the latest durable lease/heartbeat ledger rows
  per task;
- `pg_stat_qx_scheduler_activity` now adds an operator-oriented queue/runtime/
  provider rollup over the durable ledger, exposing current queued/running/
  checkpointed state plus renewal, reclaim, release, retry, and heartbeat
  activity counts without opening the raw ledger tables by hand;
- `pg_stat_qx_scheduler_ledger_queues`,
  `pg_stat_qx_scheduler_ledger_leases`, and
  `pg_stat_qx_scheduler_ledger_heartbeats` now expose the durable per-attempt
  queue/lease/heartbeat rows directly from scheduler-owned catalogs;
- `SHOW TRACE` summaries now retain scheduler queue/runtime/provider/worker/
  retry fields instead of collapsing them away in operator output;
- build wiring in `src/backend/qx/meson.build` and `src/backend/qx/Makefile`;
- a phase document that defines acceptance criteria and integration debt.

Scheduler responsibilities defined by this stage:
- turn agent/runtime task requests into schedulable envelopes;
- model queue class, lease state, and heartbeat health explicitly;
- provide future hooks for fairness, retries, reclaims, and worker lanes;
- expose readable state snapshots for later runtime and observability code.

Acceptance criteria for the scaffold:
- scheduler data structures compile as standalone engine-owned types;
- queue/lease/heartbeat snapshots can be created, copied, and described;
- the subsystem has a clear build boundary under `src/backend/qx/scheduler/`;
- runtime and operator surfaces can consume the scheduler contract without
  inventing parallel ad hoc fields;
- the phase document says what is real and what is still deferred.

Known gaps:
- no worker assignment loop yet;
- no queue fairness or preemption policy yet;
- no multi-worker ownership, balancing, or contention model yet;
- no dedicated scheduler stats collector or daemon-owned dashboard yet.

Integration debt left for later phases:
- teach recovery to consume the durable scheduler ledger directly;
- extend the current autonomous renew/reclaim loop into fuller fairness,
  duplicate-suppression, and broader failure lifecycle transitions;
- connect retries, heartbeats, and reclaims to richer worker-lane ownership
  instead of a single embedded-runtime lane;
- move the current SQL rollups toward a dedicated scheduler-owned stats plane
  when the operator surface needs lower-latency or lower-cost reporting.
