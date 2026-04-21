# QhapaqXian Backend Subsystems

This tree owns fork-specific backend code. Keep subsystem boundaries narrow and
prefer helper layers under `src/backend/qx/` before widening upstream edits.

Subsystem map:
- `catalog/`: snapshot helpers over fork-owned catalogs; partial but real
- `security/`: identity, namespace, tool, and budget enforcement
- `runtime/`: embedded task runtime and external launch boundary
- `planner/`: `AgentPlan` shaping and cost model hooks
- `executor/`: step execution and tool orchestration
- `memory/`: working, episodic, and semantic memory helpers
- `scheduler/`: admission and worker assignment
- `recovery/`: restart scan and orphan/task repair
- `observe/`: trace, explain, and status surfaces

Rule of thumb:
- add fork behavior here first;
- touch upstream core only when parser, catalogs, runtime lifecycle, or
  recovery semantics must change.
