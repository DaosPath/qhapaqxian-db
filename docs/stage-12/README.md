# QhapaqXian DB Stage 12

Stage 12 starts the security and isolation work with a narrow, defensible goal: non-privileged roles should not see engine-owned agent state by default just because the data lives in system catalogs.

Implemented in this stage:
- `pg_stat_qx_agents`, `pg_stat_qx_sessions`, and `pg_stat_qx_tasks` now use `security_barrier`
- those views filter rows to the owner role by default
- members of `pg_read_all_stats` can still inspect the full operator surface
- direct `SELECT` access from `PUBLIC` is revoked on:
  - `pg_qx_agent`
  - `pg_qx_session`
  - `pg_qx_task`
  - `pg_qx_attempt`
  - `pg_qx_step`
  - `pg_qx_event`
  - `pg_qx_trace`
  - `pg_qx_checkpoint`
  - `pg_qx_memory`
- regression coverage now includes a non-owner role that sees zero rows in `pg_stat_qx_*` and is denied raw catalog access

What this stage does not claim:
- no per-agent authentication realm yet
- no namespace-level policy engine yet
- no tool authorization model yet
- no budget-enforcement role model yet
- no full separation between operator roles and agent execution identities yet

Why this stage is worth doing now:
- QhapaqXian DB stores agent state in engine-owned catalogs, so visibility must be controlled before more observability surfaces appear
- owner-aware C command paths already existed for memory and trace reads; this stage aligns the SQL observability layer with that principle
- it reduces the chance that future dashboards or ad hoc SQL leak cross-agent state by default

Accepted debt:
- visibility is still owner-role based, not identity-policy based
- `pg_read_all_stats` is used as the broad observer escape hatch because it already exists upstream; that is pragmatic, not the final QhapaqXian security model
- raw catalog revocation does not yet come with replacement security-definer helper functions for every introspection need

Tests run:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`
