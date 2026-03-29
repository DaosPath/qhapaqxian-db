# QhapaqXian DB Bootstrap Plan

This repository was empty before the upstream import. The bootstrap plan is:

1. Import upstream PostgreSQL `REL_17_STABLE`.
2. Preserve fork identity in repository-level docs before deep core edits.
3. Add CI that proves the imported tree still configures and builds.
4. Create subsystem roots for QhapaqXian Engine under `src/backend/qx` and `src/include/qx`.
5. Add ADRs and a patch ledger before invasive parser/catalog changes.
6. Freeze Stage 2 language artifacts before changing parser or catalogs.
7. Implement parser/catalog/runtime work only after the fork has a stable maintenance boundary.

Current state:
- bootstrap fork is imported and versioned;
- Stage 2 language artifacts live under `docs/stage-2/`;
- Stage 3 parser/AST/utility dispatch is implemented under core PostgreSQL subsystems;
- Stage 4 agent/session/task catalogs are implemented under core PostgreSQL subsystems;
- Stage 5 single-node vertical slice is implemented with durable task/step/event/trace/checkpoint rows;
- Stage 6 embedded runtime boundary is implemented under `src/backend/qx/runtime`;
- Stage 7 agent transaction and resume semantics are implemented with `pg_qx_attempt`, richer checkpoints, and `RESUME TASK`;
- Stage 8 agent-aware planning/execution is implemented with `QxAgentPlan`, planner/executor modules, and `EXPLAIN AGENT`;
- Stage 9 semantic WAL/event replication boundary is implemented with logical messages and persisted semantic `LSN`s;
- Stage 10 engine-owned memory storage and operator-facing retrieval/trace commands are implemented with `pg_qx_memory`, `REMEMBER`, `FETCH MEMORY`, and `SHOW TRACE`;
- Stage 11 compatibility, hardening, and release discipline are implemented with `pg_stat_qx_*` operator views and aligned regression/deparser coverage;
- the next execution step is post-bootstrap release engineering: packaging polish, tighter security isolation, and upgrade discipline unless deeper storage is justified by benchmarks.

Immediate non-goals:
- renaming all PostgreSQL binaries
- large-scale doc rebrand
- speculative storage or WAL divergence
- pretending the runtime already exists
