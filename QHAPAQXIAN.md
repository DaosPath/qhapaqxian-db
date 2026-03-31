# QhapaqXian DB Bootstrap

This file defines the first practical fork boundary for QhapaqXian DB.

Implementation status note:
- the canonical current-state ledger now lives in `STATUS.md`
- the canonical documentation map now lives in `docs/README.md`
- this file should describe fork boundary and governance, not duplicate the full stage ledger

Product naming:
- brand: QhapaqXian
- product: QhapaqXian DB
- engine/core: QhapaqXian Engine

Bootstrap decisions:
- upstream base: PostgreSQL 17.x stable
- branch model starts with `bootstrap`
- global binary renames are deferred
- parser/catalog/runtime changes are staged after upstream import and CI stabilization

What this repository is aiming for:
- a fork with native agent language
- engine-owned agent identity and policy
- engine-owned sessions, tasks, checkpoints, and runtime
- agent-aware recovery, observability, and eventually semantic replication

What this repository is not aiming for:
- a thin extension over stock PostgreSQL
- a middleware runtime that leaves the engine unchanged
- a product marketed as PostgreSQL under another logo

Implementation status is tracked in `STATUS.md`; the document tree is mapped in `docs/README.md`; this file should not duplicate the stage ledger.
