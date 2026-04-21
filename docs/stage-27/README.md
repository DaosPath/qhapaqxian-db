# Stage 27: Recovery Scanner Scaffold

Stage 27 establishes the recovery subsystem boundary for QhapaqXian DB.
It does not yet claim full crash recovery semantics, but it does define the
engine-owned API shape that later runtime and replication integration will use.

What landed:
- a public recovery API in `src/include/qx/qx_recovery.h`;
- an internal recovery scanner implementation in
  `src/backend/qx/recovery/qx_recovery.c`;
- a backend-private header in `src/backend/qx/recovery/qx_recovery.h` for
  future wiring into runtime startup and failover control paths;
- recovery-scanner logic that reconstructs task, attempt, and checkpoint
  summaries from the durable `pg_qx_*` catalogs;
- startup and failover rebuild entry points that accept hook structs instead of
  hard-wiring runtime behavior too early.
- runtime startup now consumes the startup scanner and appends durable
  scheduler recovery rows for checkpoint requeue and stale-running-attempt
  reclaim.
- runtime-owned scheduler bgworkers now append durable lease-renewal and
  `scheduler-renew` heartbeat snapshots, then reclaim and repair a stale
  checkpoint-backed running attempt before startup/failover hooks run.
- completed tasks are not requeued solely because they still have older
  durable checkpoints.
- failover rebuild can now be invoked through a runtime-owned internal hook
  that reuses the same real scheduler ledger write path as startup recovery.
- `qx_stage3_agentic` now validates autonomous lease renewal, checkpoint-backed
  reclaim repair, startup recovery, and failover rebuild through a fresh
  backend with real scheduler ledger writes.

Recovery responsibilities defined by this stage:
- reconstruct task state from `pg_qx_task`;
- derive attempt history from `pg_qx_attempt`;
- derive checkpoint history and semantic LSN evidence from `pg_qx_checkpoint`;
- classify tasks that need requeue or fencing;
- classify attempts that are still resumable or need fencing after failover;
- expose a failover rebuild boundary that is now connected to runtime scheduler
  writes and can later be extended with semantic replication replay.

Crash/failover assumptions:
- the first scanner works from ordinary logged catalogs, not from a dedicated
  recovery WAL family;
- task state is reconstructed from catalog rows first, then later runtime and
  replication layers can consume the summaries;
- failover rebuild is treated as a higher-level orchestration step over the
  same task/attempt/checkpoint summaries and currently emits the same durable
  scheduler requeue/reclaim evidence as startup recovery;
- startup/failover recovery can now encounter work that an autonomous
  scheduler worker already renewed, reclaimed, and repaired back to
  `checkpointed`;
- recovery still does not replay semantic logs by itself.

Integration points to come:
- extend startup/failover recovery from checkpoint-backed repair into broader
  task/attempt state transitions for non-checkpointed or externally failed
  work;
- semantic replication should eventually feed a richer replay path into the
  failover rebuild hook set;
- observability should later expose the recovery report so operators can see
  how many tasks were scanned, fenced, or requeued.

Known gaps:
- hooks are intentionally narrow and do not yet integrate with replication
  entrypoints;
- the scanner is catalog-driven only and still assumes the catalogs are the
  authoritative reconstruction source;
- repeated startup/failover passes can still append additional recovery queue
  snapshots for already repaired work until broader dedupe policy lands.

Validation plan:
- keep `qx_stage3_agentic` as the real-backend recovery regression for
  autonomous lease renewal, startup checkpoint requeue, checkpoint-backed
  stale-running reclaim repair, and failover rebuild;
- add richer failover coverage once semantic replication replay grows a deeper
  orchestration path.
