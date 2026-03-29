# QhapaqXian Engine Backend Scaffold

This directory is reserved for fork-owned backend subsystems that should remain
as isolated from upstream churn as practical.

Planned children:
- `catalog/`
- `runtime/`
- `scheduler/`
- `recovery/`
- `memory/`
- `planner/`
- `executor/`
- `security/`
- `observe/`

Bootstrap rule:
- prefer adding new fork behavior here before editing broad upstream files;
- touch upstream core only where parser, catalogs, runtime lifecycle, or recovery semantics require it.
