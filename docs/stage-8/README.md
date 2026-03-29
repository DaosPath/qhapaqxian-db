# QhapaqXian DB Stage 8

Stage 8 introduces the first native planner/executor boundary for agent commands instead of sending `RUN TASK` and `RESUME TASK` straight from utility handlers into the runtime.

Implemented in this stage:
- `QxAgentPlan` metadata structures under `src/include/qx/qx_agent_plan.h`
- planner entrypoints under `src/backend/qx/planner` for `RUN TASK` and `RESUME TASK`
- executor entrypoint under `src/backend/qx/executor` that turns `QxAgentPlan` into runtime requests
- `EXPLAIN AGENT` utility syntax and dispatch for explainable agent statements
- `taskcmds.c` now routes execution through planner -> executor instead of directly calling runtime primitives
- runtime step and checkpoint labels aligned to the Stage 8 plan surface (`stage8.*`)
- regression coverage for `EXPLAIN AGENT RUN TASK` and `EXPLAIN AGENT RESUME TASK`

What is still intentionally deferred:
- integration with the core PostgreSQL optimizer cost model
- branch/replan operators inside a deep agent executor tree
- asynchronous scheduler workers that consume `AgentPlan` queues independently of the utility path
- tuple-returning execution for `RUN TASK ... RETURNING TASK`
- `EXPLAIN AGENT` coverage for commands beyond `RUN TASK` and `RESUME TASK`

Why this matters:
- QhapaqXian DB now has a fork-owned planning abstraction for agent work, not just parser syntax and runtime side effects
- explainability is native to the server for agent commands and can evolve independently from traditional `EXPLAIN`
- the planner now owns semantic validation for resumable execution boundaries, which is the correct place to grow cost, retries, branching, and tool semantics later

Accepted debt in this stage:
- `AgentPlan` is metadata-driven, not yet integrated into the relational planner/executor internals
- estimated costs are heuristic constants, not cardinality- or statistics-driven estimates
- runtime execution still materializes a single-node deterministic path after the planner approves it

Tests run:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`
