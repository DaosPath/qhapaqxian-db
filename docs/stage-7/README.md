# QhapaqXian DB Stage 7

Stage 7 formalizes resumable task execution on top of the embedded runtime introduced in Stage 6.

Implemented in this stage:
- new system catalog `pg_qx_attempt` for durable attempt history
- `pg_qx_task` now records `lastattemptid` and `lastcheckpointid`
- `pg_qx_checkpoint` now captures the writing attempt, task state, and next resumable step
- runtime submission now parks a task at a durable checkpoint instead of auto-completing it
- new `RESUME TASK` utility command resumes the task from the latest durable checkpoint
- regression coverage now verifies checkpointed tasks, resumed attempts, final completion, and failure cases

What is still intentionally deferred:
- asynchronous launcher/bgworker runtime
- background recovery scan that auto-resumes or requeues crash-interrupted tasks
- compensation workflows and multi-branch recovery policies
- planner/executor-native step scheduling
- user-visible tuple-returning utility execution for `RUN TASK` or `RESUME TASK`

Why this matters:
- tasks now have a durable attempt chain instead of a single opaque execution
- checkpoints now carry explicit resume metadata inside core catalogs
- resume semantics are a first-class utility path in the fork, not an external orchestrator convention

Tests run:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`
