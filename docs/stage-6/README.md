# QhapaqXian DB Stage 6

Stage 6 moves task execution out of `taskcmds.c` and into an embedded runtime boundary.

Implemented in this stage:
- new runtime header in `src/include/qx/qx_runtime.h`
- new runtime module in `src/backend/qx/runtime/qx_runtime.c`
- backend build integration for `src/backend/qx/*`
- `RUN TASK` now validates the session and submits a runtime request instead of materializing every row directly
- task lifecycle now passes through runtime-owned states:
  - `queued`
  - `running`
  - `completed`
- runtime-owned observability now shows:
  - `TASK_QUEUED`
  - `TASK_DISPATCHED`
  - runtime traces
  - Stage 6 checkpoint labels

What is still intentionally deferred:
- launcher/bgworker process model
- shared-memory scheduler queue
- retries, heartbeats, cancellation, reaper, budget keeper
- concurrent worker execution
- crash/recovery orchestration outside ordinary relational durability

Why this matters:
- the engine now has a stable runtime ownership boundary
- later scheduler/bgworker work can land in `src/backend/qx/runtime` without re-growing command-layer glue
- `RUN TASK` is now a submit path into the runtime, not the runtime itself

Tests run:
- `postgresql:setup`
- targeted `pg_regress` run for `qx_stage3_agentic`
- full `postgresql:regress` suite

Next step:
- Stage 7 should formalize task attempts, checkpoints, resume semantics, and crash-aware agent transaction rules on top of this runtime boundary.
