QhapaqXian DB
=============

This repository is the bootstrap fork for QhapaqXian DB.

Naming boundary:
- brand: QhapaqXian
- product: QhapaqXian DB
- engine/core: QhapaqXian Engine
- repository: qhapaqxian-db

Current bootstrap status:
- upstream base imported from PostgreSQL `REL_17_STABLE`
- first fork branch: `codex/bootstrap`
- architecture seed stored in `docs/`
- no global product rebrand of upstream binaries yet
- Stage 3 agentic parser/AST/utility patches are in-tree
- Stage 4 native agent/session/task catalogs are in-tree
- Stage 5 single-node task/event/trace/checkpoint slice is in-tree
- Stage 6 embedded runtime boundary is in-tree under `src/backend/qx`
- Stage 7 resumable runtime semantics are in-tree with `pg_qx_attempt` and `RESUME TASK`
- Stage 8 planner/executor boundary is in-tree with `QxAgentPlan` and `EXPLAIN AGENT`
- Stage 9 semantic WAL/logical-message boundary is in-tree with persisted semantic `LSN`s and the `qhapaqxian_output` logical decoder
- Stage 10 engine-owned memory storage and operator-facing `REMEMBER` / `FETCH MEMORY` / `SHOW TRACE` commands are in-tree
- Stage 11 compatibility and operability surface is in-tree with `pg_stat_qx_*` system views and hardened regression coverage
- Stage 12 security-isolation seed is in-tree with owner-filtered `pg_stat_qx_*` views and revoked raw catalog access for non-privileged roles

What this fork is:
- a real fork target for an AgentDB
- not a PostgreSQL extension
- not a middleware-only orchestration layer
- not a SQL wrapper over application tables

What stays intentionally close to upstream in this bootstrap:
- server and client binary names
- build layout
- test harnesses
- core storage and replication behavior

Immediate fork governance files:
- `QHAPAQXIAN.md`
- `docs/qhapaqxian-architectural-blueprint.md`
- `docs/stage-2/README.md`
- `docs/stage-3/README.md`
- `docs/stage-5/README.md`
- `docs/stage-6/README.md`
- `docs/stage-7/README.md`
- `docs/stage-8/README.md`
- `docs/stage-9/README.md`
- `docs/stage-10/README.md`
- `docs/stage-11/README.md`
- `docs/stage-12/README.md`
- `docs/bootstrap-plan.md`
- `docs/rebase-strategy.md`
- `docs/patch-ledger.md`
- `docs/adr/`

Upstream note
-------------

This tree still contains the PostgreSQL source distribution as its
technical base. The product should be presented as QhapaqXian DB, while
retaining clear attribution to the upstream PostgreSQL project.

Original upstream summary
-------------------------

This directory contains the source code distribution of the PostgreSQL
database management system.

PostgreSQL is an advanced object-relational database management system
that supports an extended subset of the SQL standard, including
transactions, foreign keys, subqueries, triggers, user-defined types
and functions. This distribution also contains C language bindings.

Copyright and license information can be found in the file COPYRIGHT.

General documentation about this upstream version of PostgreSQL can be
found at <https://www.postgresql.org/docs/17/>. In particular,
information about building PostgreSQL from the source code can be found
at <https://www.postgresql.org/docs/17/installation.html>.

The latest upstream PostgreSQL releases can be obtained at
<https://www.postgresql.org/download/>. For more information see
<https://www.postgresql.org/>.
