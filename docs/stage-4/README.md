# QhapaqXian DB Stage 4

Stage 4 adds the first first-class engine catalogs for agent state so later
runtime stages can persist agents, sessions, and tasks instead of keeping that
state in utility-layer stubs.

What landed:
- native `pg_qx_agent` catalog for durable agent records
- native `pg_qx_session` catalog for durable session records
- native `pg_qx_task` catalog for durable task records
- engine-owned foreign-key and dependency wiring for the agent/session/task
  chain
- catalog bootstrap and regression coverage for the new system relations

What is intentionally deferred:
- executable runtime behavior
- embedded scheduler or worker loop
- resumable task attempts and checkpoints
- planner/executor capability reasoning

Why this stage matters:
- it moves agent state into the engine instead of leaving it as an extension-
  or application-owned concern
- it gives later stages a durable identity graph to build execution semantics
  on top of
- it makes `CREATE AGENT`, `START SESSION`, and `RUN TASK` point at real
  catalogs rather than transient command stubs

Validation:
- covered by the later bootstrap regression sweeps that rely on the catalog
  surface introduced in this stage

Next stage handoff:
- Stage 5 can now build a real single-node execution slice on top of these
  catalogs without changing the ownership model again
