# Stage 32 - Catalog Snapshot Helper Layer

## Stage intent

Stage 32 introduces a shared catalog snapshot layer for QhapaqXian Engine.
It is a consolidation step, not a new runtime feature by itself.

## What landed

- public snapshot structs under `src/include/qx/qx_catalog.h` for:
  - agents
  - identities
  - providers
  - principals
  - namespace policies
  - tools
  - sessions
  - tasks
  - attempts
  - checkpoints
- backend lookup/fill/free helpers under `src/backend/qx/catalog/`;
- a single place for translating `syscache` and tuple reads into engine-owned C structs;
- provider/principal merge logic so callers can observe the effective provider contract without open-coding repeated joins;
- build wiring so the catalog helper layer is part of the fork-owned `src/backend/qx` boundary.

## Why this exists

- runtime, security, planner, and recovery were each beginning to grow their own catalog access code;
- duplicating `SearchSysCache*`, `GETSTRUCT`, and text-attribute extraction across subsystems increases rebase cost and drift risk;
- a shared snapshot layer makes later refactors safer when catalog layouts evolve again.

## What is intentionally deferred

- no new catalog objects were added in this stage;
- no `syscache` definitions were changed in this stage;
- no runtime, scheduler, or recovery flow was rewritten yet to depend exclusively on this helper layer;
- no dedicated regression schedule was added yet for the helper API itself.

## Why this stage matters

- it reduces duplicate catalog-read code across fork-owned subsystems;
- it gives later scheduler, recovery, and observability work a stable
  snapshot shape to consume;
- it lowers rebase cost when catalog layouts evolve again.

## Integration direction

- security and runtime code should gradually stop open-coding catalog lookups and move onto `qx_catalog`;
- recovery and scheduler code should use snapshot structs when they need a stable cross-subsystem view of task state;
- observability code should use the same layer before new `pg_stat_qx_*` surfaces are expanded.

## Known gaps

- this stage is mainly an internal maintainability improvement;
- some callers still use direct catalog access and need migration in later
  cleanup phases.

## Validation

- There is no dedicated unit-style coverage for the helper layer yet.
- Validation remains indirect through build and higher-level integration.

## Next stage handoff

- scheduler, runtime, recovery, and observability work should migrate their
  open-coded catalog access onto `qx_catalog`;
- future refactors should prefer expanding the snapshot layer instead of
  adding new scattered `syscache` helpers in each subsystem.
