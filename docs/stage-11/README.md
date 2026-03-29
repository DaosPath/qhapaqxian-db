# QhapaqXian DB Stage 11

Stage 11 closes the initial bootstrap roadmap with operator-facing compatibility and hardening work, without pretending that packaging, renamed binaries, or deep privilege isolation are already finished.

Implemented in this stage:
- new system views:
  - `pg_stat_qx_agents`
  - `pg_stat_qx_sessions`
  - `pg_stat_qx_tasks`
- regression coverage for the Stage 11 operator slice in `qx_stage3_agentic`
- deparser/rules golden updates required by new system-view definitions
- documentation updates for compatibility, operability, and release discipline

What these views make visible:
- per-agent session, task, checkpoint, and memory counts
- per-session task, event, trace, checkpoint, and memory counts
- per-task attempt, step, event, trace, checkpoint, and memory counts
- stable operator-facing state names instead of raw internal state codes where practical

Compatibility state at Stage 11:
- ordinary PostgreSQL SQL, catalogs, and tooling still remain the technical base
- native agent commands coexist with upstream SQL instead of replacing it
- binary naming and packaging still follow upstream defaults
- agentic execution is still primarily utility-path driven, even though planning/runtime boundaries now exist

What is intentionally not claimed in this stage:
- no full product rebrand of binaries or client protocol fields
- no final security model for per-agent authentication domains
- no custom storage engine or access method for agent memory
- no dedicated semantic WAL rmgr or physical replication fork
- no tuple-returning `START SESSION ... RETURNING SESSION` or `RUN TASK ... RETURNING TASK`

Why this stage matters for the fork:
- the fork now has engine-owned operator views over agent runtime state, instead of requiring ad hoc catalog joins in clients
- observability is exposed through stable SQL objects that fit PostgreSQL operational habits
- the bootstrap roadmap now ends in a state that is installable, testable, and supportable as an internal release candidate

Accepted debt:
- `pg_stat_qx_*` views currently aggregate directly over heap-backed engine catalogs; they are correct but not optimized for large fleet-scale telemetry
- ownership and visibility still follow relation privileges and command-time checks more than a dedicated per-agent security domain
- packaging, version strings, upgrade tooling, and binary-brand boundaries still need dedicated release engineering work

Tests run:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`
