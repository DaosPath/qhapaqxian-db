# Rebase Strategy

Branching model:
- upstream tracking branch: `upstream/REL_17_STABLE`
- active fork bootstrap branch: `codex/bootstrap`
- future release branches: `qx/release/<version>`

Rules:
- import upstream minor fixes and CVEs on a fixed cadence;
- freeze upstream intake during high-risk milestones in parser/catalog/runtime/recovery;
- tag every local deep patch as one of:
  - `TEMP_PATCH`
  - `PERM_SUBSYSTEM`
  - `UPSTREAMABLE_CANDIDATE`

Patch review questions:
- Does this patch change public fork semantics?
- Does this patch deepen divergence against upstream?
- Can the behavior be isolated under `src/backend/qx` or adjacent new files?
- Does the patch require dedicated tests before merge?
