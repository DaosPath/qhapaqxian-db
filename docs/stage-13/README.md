# Stage 13: Operational Identities and Budget Snapshots

Stage 13 turns agent identity from descriptive text into a first-class engine object.

What landed:
- native `pg_qx_identity` catalog with namespace, owner, auth role, policy label, and serialized budget policy;
- `CREATE AGENT` now resolves or materializes an operational identity and stores `qxidentityid` in `pg_qx_agent`;
- `START SESSION` snapshots the agent identity into `pg_qx_session` and enforces that the caller can assume the identity auth role;
- `RUN TASK` snapshots identity, authorized tools, token budget, and cost budget into `pg_qx_task`;
- `EXPLAIN AGENT` now shows identity, authorized tools, estimated tokens, and budget ceilings;
- planner-side budget enforcement now rejects task plans that exceed the configured token/cost limits before runtime submission;
- task/session dependencies now point at the identity object, not only the agent row.

What this stage does not claim:
- there is still no dedicated tool-execution subsystem; Stage 13 only snapshots and surfaces the tool allowlist in planner/runtime metadata;
- namespaces are inherited from the agent schema; there is not yet a separate identity-namespace policy engine;
- auth-role enforcement is still owner/member based, not a full agent principal/session token model;
- budget enforcement is plan-admission enforcement, not runtime token metering against a live model provider.

Why this matters:
- identities now survive as engine-owned objects with catalog semantics, dependencies, and foreign-key visibility;
- sessions/tasks no longer rely on free-form identity text once execution starts;
- budget policy is no longer only configuration decoration on the agent row, because the planner blocks over-budget work before it enters the runtime path.

Regression coverage added or extended:
- `pg_qx_identity` creation and linkage to agents;
- session/task linkage to identity OIDs;
- `EXPLAIN AGENT` output for identity, tools, and budget fields;
- rejection path for a low-budget agent (`tiny_budget`);
- `oidjoins` coverage for new catalog foreign keys.
