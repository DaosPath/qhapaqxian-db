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
- Stage 12 security isolation has started with owner-filtered `pg_stat_qx_*` views, direct catalog revocation for non-privileged roles, and regression coverage for non-owner visibility;
- Stage 13 operational identity and budget-admission work is implemented with native `pg_qx_identity`, identity snapshots in `pg_qx_agent` / `pg_qx_session` / `pg_qx_task`, and planner-side budget rejection before runtime submission;
- Stage 14 namespace policy, runtime tool authorization, and live budget metering are implemented with `pg_qx_namespace`, task-level authorization snapshots, runtime consumption counters, and expanded `pg_stat_qx_*` visibility;
- Stage 15 explicit namespace-policy DDL, real tool registry, and historical-debt cleanup are implemented with `CREATE/ALTER NAMESPACE POLICY`, `CREATE/ALTER TOOL`, `pg_qx_tool`-backed authorization/metering, and mandatory explicit policy binding in `CREATE AGENT`;
- Stage 16 principal-backed execution is implemented with `CREATE/ALTER PRINCIPAL`, `pg_qx_principal`, bindir-scoped external tool runners, resume-safe runtime sequencing, and metering charged from external runner responses;
- Stage 17 stronger runtime isolation is implemented with OS-level sandbox profiles, minimal-environment execution, sealed runtime workdirs, and launcher-enforced timeout/process ceilings for principal programs;
- Stage 18 restricted-identity execution is implemented with restricted-token launch for non-`builtin` principals on Windows and parent-observed launch receipts in runtime traces;
- Stage 19 provider-backed runtime receipts are implemented with native `CREATE/ALTER PROVIDER`, `pg_qx_provider`, principal-to-provider binding, and runtime verification of provider identity, receipt schema, receipt nonce, and attestation mode before budget charging;
- the next execution step is no longer local provider debt but broader external-runtime scope: remote providers, signed/remote attestation, or container/microVM execution when the product chooses to leave the single-host fork boundary.

Immediate non-goals:
- renaming all PostgreSQL binaries
- large-scale doc rebrand
- speculative storage or WAL divergence
- pretending the runtime already exists
