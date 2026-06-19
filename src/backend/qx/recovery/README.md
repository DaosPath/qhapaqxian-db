# QhapaqXian Recovery Layer

Role:
- scan durable task state after restart, fence stale leases, classify orphaned
  attempts, and requeue resumable work.

Current state:
- recovery startup scans now drive runtime-owned scheduler writes for
  checkpointed-task requeue snapshots and stale-attempt reclaim snapshots.
- failover rebuild scans now have a runtime-owned internal hook that drives the
  same durable scheduler requeue/reclaim write path.
- repeated startup/failover scans are now idempotent against durable scheduler
  evidence: tasks whose latest queue snapshot is already `RECOVERY` are not
  requeued again, and attempts whose latest lease snapshot is already
  `RECLAIMED` are not fenced again.
- completed tasks are no longer requeued just because an older durable
  checkpoint exists.
- the focused `qx_stage3_agentic` regression now covers checkpointed-task
  requeue, durable lease renewal before reclaim, stale-running-attempt reclaim,
  stale no-checkpoint fail-closed retry repair, autonomous retry intake back to
  `checkpointed`, recovery/failover idempotence, and failover rebuild in a
  fresh backend.

Integration debt:
- recovery still emits scheduler snapshots through the runtime-owned autonomous
  daemon rather than owning supervision itself;
- semantic checkpoint replay is now wired through failover rebuild and a focused
  regression hook, but decoder-fed orchestration and repeated no-checkpoint
  semantic replay remain future work.
