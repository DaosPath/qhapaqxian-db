# QhapaqXian DB Stage 5

Stage 5 delivers the first executable vertical slice of the fork on a single node.

Implemented in this stage:
- `RUN TASK` now creates durable rows in `pg_qx_task`.
- `START SESSION` now emits durable session-scoped `pg_qx_event` and `pg_qx_trace` rows.
- New internal runtime relations exist for:
  - `pg_qx_step`
  - `pg_qx_event`
  - `pg_qx_trace`
  - `pg_qx_checkpoint`
- `RUN TASK` synchronously materializes:
  - a task row
  - three completed synthetic step rows
  - task events
  - task traces
  - one durable checkpoint row

Important limitations kept explicit:
- `RUN TASK ... RETURNING TASK` is still not executable.
- `START SESSION ... RETURNING SESSION` is still not executable.
- `IN SESSION` currently accepts only a literal OID.
- there is still no embedded scheduler, bgworker runtime, retry loop, or resume command path
- task steps are synthetic Stage 5 slice steps, not planner-produced `AgentPlan` steps

Why this still justifies the fork:
- agent runtime state is now owned by engine relations, not application tables
- task/session observability is durable and addressable by engine identity
- the server now persists task/step/event/trace/checkpoint state inside core catalogs

Exit criteria satisfied:
- one node can `CREATE AGENT`, `START SESSION`, and `RUN TASK`
- runtime state survives initdb/startup test flow and is regression-tested
- the slice is observable through internal `pg_qx_*` catalogs

Tests run:
- `postgresql:setup`
- targeted `pg_regress` run for `qx_stage3_agentic`
- full `postgresql:regress` suite with `225 subtests passed`

Next step:
- Stage 6 should move from direct utility-driven task materialization to a real embedded runtime and scheduler.
