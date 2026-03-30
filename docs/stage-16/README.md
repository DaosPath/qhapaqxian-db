# Stage 16: Principal Runtime, External Tool Execution, and Metering

Stage 16 converts tool principals from catalog metadata into a real execution path.

What landed:
- native `CREATE PRINCIPAL` and `ALTER PRINCIPAL` statements wired through parser, AST, command tags, and `ProcessUtility`;
- new `pg_qx_principal` catalog for namespace-scoped principals, sandbox ceilings, owner binding, and executable program registration;
- `pg_qx_tool` now stores a principal OID in addition to the principal display name, with dependency tracking and validation at DDL time;
- namespace authorization now resolves tool rows and principal rows together, enforcing enabled-state checks and sandbox ceilings before task admission;
- runtime task execution now launches a shipped external helper, `qhapaqxian-tool-runner`, without going through a shell;
- task metering is now charged from the external runner response instead of from synthetic descriptor-only runtime constants;
- `RESUME TASK` now honors the durable checkpoint `next_step` boundary and continues with new runtime steps instead of colliding with the original checkpoint step.

Runtime shape in this stage:
- submit path: `stage15.authorize_tools -> stage16.external_submit -> stage8.capture_input -> stage8.checkpoint_barrier`;
- resume path: `stage8.resume_dispatch -> stage16.external_resume -> stage8.complete`;
- semantic WAL/logical decoding now emits both submit-phase and resume-phase `TASK_TOOL_EXECUTED` records.

Accepted debt:
- the runner is still a deterministic fork-owned helper, not a provider-specific sandbox or billing adapter;
- sandbox enforcement is stronger than Stage 15 because execution is out-of-process and bindir-scoped, but it is still not microVM/container isolation;
- metering is now tied to external execution, but it is still engine-managed accounting rather than provider-confirmed invoices.

Why the fork boundary is justified here:
- this stage touches parser grammar, utility dispatch, new engine catalogs, dependency handling, runtime step sequencing, external process launch, semantic replication payloads, and regression fixtures;
- moving principals and tool execution into engine-owned DDL and runtime control cannot be delivered as a thin extension over untouched PostgreSQL command handling.

Validation:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`

Result:
- `postgresql:setup` passed
- `postgresql:qhapaqxian_output` passed (`1 subtests passed`)
- `postgresql:regress` passed (`225 subtests passed`)
