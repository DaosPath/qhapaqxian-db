# QhapaqXian Scheduler Layer

Role:
- admit tasks, rank priority, enforce fairness, and assign workers.

Current state:
- scheduler is now a background-worker-based autonomous scaffold: postmaster
  registers a static launcher and that launcher spawns a fixed worker-slot set
  per connectable database.
- runtime submit/resume already emits scheduler queue/lease/heartbeat contract
  fields.
- runtime submit/resume now also persist first-class queue/lease/heartbeat
  ledger rows in scheduler-owned catalogs.
- runtime now appends explicit release snapshots so the durable ledger shows
  when a worker lane stops holding a lease.
- scheduler database workers now scan held lease snapshots continuously and
  append durable renewal snapshots plus `scheduler-renew` heartbeats before
  reclaiming stale checkpoint-backed work.
- task ownership is now deterministic across those workers: each task hashes to
  a slot, and each slot only renews/reclaims/retries the work it owns.
- reclaim now repairs checkpoint-backed task/attempt rows back to
  `checkpointed` and appends a recovery queue snapshot plus
  `scheduler-reclaim` heartbeat.
- reclaim lease/heartbeat rows now carry the specific slotted scheduler worker
  name so ownership is visible in the durable ledger.
- retry intake now scans all eligible durable retry rows in a cycle and
  dispatches the best candidate first by priority weight, `eligible_at`,
  `enqueued_at`, failed attempt sequence, then task OID.
- retry backoff is now durable policy, not just queue delay bookkeeping:
  priority affects the base delay, repeated failures grow exponentially, and a
  deterministic jitter window avoids fixed retry bursts.
- `pg_stat_qx_tasks`, `pg_stat_qx_scheduler_queues`, and
  `pg_stat_qx_scheduler_workers` now surface that live scheduler state without
  inventing a second scheduler contract in SQL.
- `pg_stat_qx_scheduler_activity` now adds a queue/runtime/provider health
  rollup over the durable ledger, including current task counts plus renew,
  reclaim, release, retry, and heartbeat evidence.
- `pg_stat_qx_scheduler_retry_backoff` exposes the current queued retry lane
  with owner slot, durable delay, remaining backoff, and eligibility status.
- `pg_stat_qx_scheduler_worker_balance` exposes per-slot owned task counts for
  each queue/runtime/provider key, including empty slots, so operators can see
  balancing and skew directly.
- `pg_stat_qx_scheduler_ledger_queues`,
  `pg_stat_qx_scheduler_ledger_leases`, and
  `pg_stat_qx_scheduler_ledger_heartbeats` expose the durable scheduler ledger
  directly for per-attempt inspection.
- the queue/worker rollups now consume the durable ledger as their primary
  source instead of reconstructing operator state from trace payloads.
- the focused real regression now asserts that the aggregated activity row for
  the container queue reflects renew/reclaim/release/retry history after the
  autonomous scheduler flow completes.

Integration debt:
- retry policy and planner/recovery coupling still need hardening beyond the
  current priority/exponential baseline.
- dynamic slot scaling, lease-contention rules, and richer balancing policy
  still need a clearer scheduler-owned policy.
- reclaim/failure/fencing paths still need broader handling for
  non-checkpointed attempts and richer dedupe policy across autonomous and
  recovery-triggered repair paths.
