# Stage 15: Namespace Policy DDL, Tool Registry, and Historical-Debt Cleanup

Stage 15 turns namespace policy and tool metadata into explicit engine DDL instead of bootstrap side effects.

What landed:
- native `CREATE NAMESPACE POLICY` and `ALTER NAMESPACE POLICY` statements wired through parser, AST, command tags, and `ProcessUtility`;
- native `CREATE TOOL` and `ALTER TOOL` statements backed by `pg_qx_tool`;
- `pg_qx_namespace` now keys policies by `(schema, policy_name)` instead of one row per schema, which fixes the historical collision where multiple policy labels in the same schema silently mapped to the same policy row;
- `CREATE AGENT` no longer auto-creates namespace policy rows; it requires an explicit existing policy binding;
- `QxAuthorizeToolsForNamespace()` now resolves real tool rows from `pg_qx_tool`, enforces enabled-state checks, snapshots tool OIDs/contracts, and derives token/cost metering from catalog values instead of synthetic constants;
- `EXPLAIN AGENT`, `pg_qx_task`, traces, and semantic messages now reflect tool contracts and real registered tool budgets.

Historical debt removed:
- implicit namespace-policy creation during `CREATE AGENT`;
- `pg_qx_tool` as a no-op scaffold;
- one-policy-per-schema behavior hidden behind policy labels.

What remains deferred:
- tool execution is still metadata-driven inside the fork; sandbox and principal are catalog/runtime descriptors, not OS-level isolation;
- provider-confirmed billing is still absent; runtime metering is engine-owned accounting derived from registered tool costs;
- namespace policy is still schema-scoped, not yet a standalone multi-tenant principal/namespace subsystem.

Why the fork boundary is justified here:
- this stage touches parser grammar, parse nodes, utility dispatch, core system catalogs, planner output, executor requests, runtime accounting, regression fixtures, and semantic replication payloads;
- the resulting behavior cannot be delivered as a thin extension or SQL wrapper without re-implementing native command handling and engine-owned authorization state.

Validation:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`

Result:
- `postgresql:setup` passed
- `postgresql:qhapaqxian_output` passed (`1 subtests passed`)
- `postgresql:regress` passed (`225 subtests passed`)
