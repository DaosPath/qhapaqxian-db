# QhapaqXian Runtime Layer

Role:
- embedded task runtime, dispatcher, worker launch boundary, and budget
  enforcement at execution time.

Current state:
- runtime owns the current agent/task launch and external provider boundary.
- runtime submit/resume now persist scheduler queue/lease/heartbeat ledger rows
  alongside the existing task/attempt/event/trace/checkpoint catalogs.
- runtime submit now also persists explicit `submit` and `resume` contracts on
  each task, so later resume/retry/recovery paths can reuse the planned
  capability route instead of guessing from allowlist order.
- checkpoint and complete paths now append durable lease-release and
  scheduler-release heartbeat rows for the finished worker handoff.
- startup recovery scans now append durable recovery-queue rows for
  checkpointed work and reclaim lease/heartbeat rows for fenced stale
  attempts.
- failover rebuild scans can now be driven through a runtime-owned internal hook
  that uses the same real queue/lease/heartbeat ledger write path.
- postmaster now registers a runtime-owned scheduler launcher bgworker, and
  that launcher spawns a bounded slot set of scheduler database workers per
  connectable database. `QX_SCHEDULER_DB_WORKER_SLOTS` configures the slot
  count at postmaster startup, clamped to 1..8, with default `2`.
- in pg_regress temp clusters, the launcher intentionally limits autonomous
  scheduler workers to the canonical `regression` database; this keeps the
  daemon real for QX tests without holding persistent connections to transient
  upstream regression databases that are created and dropped inside core tests.
- each scheduler database worker now wakes periodically, scans running tasks
  against the durable lease ledger, renews a held lease once, then reclaims
  stale checkpoint-backed work, repairs task/attempt state back to
  `checkpointed`, and appends the matching recovery queue / heartbeat rows.
- worker slots now own tasks deterministically by task OID hash, and reclaim
  lease / heartbeat rows are attributed to the specific scheduler slot that
  performed the repair.
- stale work with no durable checkpoint now fails closed: the running attempt is
  marked `failed`, the task returns to `queued`, no checkpoint is synthesized,
  and the ledger records a durable `RETRY` queue row plus reclaimed lease /
  heartbeat evidence.
- the same autonomous database worker now also consumes eligible queued retry
  rows after a durable backoff window, ranks competing candidates by priority,
  eligibility time, enqueue time, failed attempt sequence, and task OID, then
  opens the selected next attempt and re-runs the real `submit` path so the
  task can return to `checkpointed` without manual intervention.
- retry backoff is now priority-aware and exponential with deterministic jitter
  so `urgent` retries re-enter earlier than `high`, while repeated failures
  still stretch the durable eligibility window.
- exhausted retries now fail closed into durable dead-letter state: the task is
  marked `failed`, the last attempt remains `failed`, the scheduler appends a
  blocked `MAINTENANCE` queue row, and trace/event evidence is emitted.
- the focused scheduler worker tick remains as a narrow regression hook, but
  the main path is now the autonomous bgworker loop.
- `qx_stage3_agentic` now exercises startup checkpoint requeue, autonomous
  lease renewal, checkpoint-backed stale-running reclaim repair, and failover
  rebuild with real catalog/ledger writes in a fresh backend.
- `container://` principals have a real Docker-backed execution path.
- `microvm://` principals have a real QEMU `microvm` execution path, validated
  with Windows `tcg` and WSL/Linux `kvm`.
- on Windows, real `container://` and `microvm://` launches that already
  preserve host identity now skip `CREATE_SUSPENDED`; that keeps the launcher
  aligned with the direct runner replay path and avoids false timeouts during
  real backend resume execution.
- operational setup and validation commands live in
  `../../../../docs/real-runtime-backends.md`.

Integration debt:
- scheduler/recovery ownership is still converging with the active runtime path;
- runtime now writes normal release snapshots, autonomous renewal/reclaim
  snapshots, checkpoint-backed repair writes, non-checkpoint fail-closed retry
  writes, autonomous retry dispatch after backoff, and max-retry dead-letter
  writes; remaining scheduler debt is load-adaptive slot scaling, broader
  dedupe policy across startup/failover/autonomous paths, and explicit
  external-process cleanup ownership beyond the current runner behavior;
- Docker/QEMU launch is real, but deeper OCI policy, VM lifecycle supervision,
  and self-hosted KVM CI coverage remain active hardening work.
