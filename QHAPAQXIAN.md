# QhapaqXian DB Bootstrap

This file defines the first practical fork boundary for QhapaqXian DB.

Product naming:
- brand: QhapaqXian
- product: QhapaqXian DB
- engine/core: QhapaqXian Engine

Bootstrap decisions:
- upstream base: PostgreSQL 17.x stable
- branch model starts with `codex/bootstrap`
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

Current implementation boundary:
- imported upstream tree
- fork documentation and governance files
- CI bootstrap for source build verification
- QhapaqXian subsystem directory scaffold under `src/backend/qx` and `src/include/qx`

Next implementation boundary:
- parser and AST scaffolding
- agent catalogs
- runtime launcher/scheduler bootstrap
- crash-safe vertical slice
