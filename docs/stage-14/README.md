# Stage 14: Namespace Policy, Tool Authorization, and Runtime Budget Metering

Stage 14 deepens the security line from planner admission into engine-owned runtime state.

What landed:
- native `pg_qx_namespace` catalog with schema binding, owner, auth role, policy label, allowlisted tools, and budget-enforcement switches;
- `pg_qx_agent`, `pg_qx_session`, and `pg_qx_task` now persist namespace-policy linkage instead of treating schema ownership as an implicit policy boundary;
- `CREATE AGENT` materializes or reuses a namespace policy and binds the agent to it at catalog level;
- `START SESSION` snapshots the namespace policy into `pg_qx_session`;
- `RUN TASK` authorizes tools against namespace policy before submission, snapshots authorized tool budget into `pg_qx_task`, and exposes that budget in `EXPLAIN AGENT`;
- the embedded runtime now meters `qxtaskconsumedtokens` and `qxtaskconsumedcost` during task progression instead of only relying on planner-side rejection;
- `pg_stat_qx_agents`, `pg_stat_qx_sessions`, and `pg_stat_qx_tasks` now expose namespace policy and runtime budget consumption;
- semantic WAL output now includes the additional authorization event/trace boundary (`TASK_TOOLS_AUTHORIZED`, `runtime.authorize_tools`).

What this stage does not claim:
- `pg_qx_tool` exists only as a bootstrap scaffold; runtime authorization is currently enforced from `pg_qx_namespace.qxallowedtools`, not from engine-owned executable tool descriptors;
- tool execution is still synthetic and internal to the fork; there is no external provider sandbox or per-tool process isolation yet;
- namespace policy is schema-bound and owner/auth-role based, not yet an independent multi-tenant principal model;
- budget metering is engine-owned accounting, not provider-confirmed token billing.

Accepted technical debt:
- the bootstrap keeps tool registration shallow to avoid pushing unstable execution semantics into the catalog before a real tool subsystem exists;
- namespace policy defaults are created on demand during `CREATE AGENT`, which is practical for the fork bootstrap but not yet a full DDL surface like `CREATE NAMESPACE POLICY`.

Why this matters:
- authorization is now durable engine state, not only planner decoration;
- task rows record both authorized budget and consumed budget, which gives crash recovery, observability, and replication a consistent accounting surface;
- namespace policy becomes a real fork justification point because it spans parser semantics, catalogs, planner output, runtime enforcement, system views, and semantic replication.

Regression coverage added or extended:
- namespace policy linkage in agent/session/task catalogs;
- runtime budget counters and authorized-tool snapshots in `pg_qx_task`;
- `pg_stat_qx_*` outputs for namespace policy and remaining budget;
- semantic logical decoding output for the new authorization event and trace;
- `rules.out` / `oidjoins.out` updates for new system-view joins and catalog foreign keys.
