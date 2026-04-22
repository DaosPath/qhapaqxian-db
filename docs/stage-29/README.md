# Stage 29: Capability-Aware Agent Planning

Stage 29 makes the planner, executor, and runtime handoff reason explicitly
about runtime class and tool capability instead of relying on the incidental
order of flat authorized-tool contracts.

What landed:
- `QxAgentPlan` now carries a structured runtime-decision summary and a list of
  structured per-tool decisions;
- each tool decision records the tool, handler, principal, provider, provider
  kind, principal runtime class, endpoint, receipt metadata, and derived
  capability class;
- `EXPLAIN AGENT` now emits the execution surface, unique provider kinds,
  unique principal runtime classes, capability tags, and per-tool decisions;
- the executor now rejects plans whose capability decisions are incomplete or
  internally inconsistent before handing them to the runtime boundary;
- the executor now materializes an explicit `submit` route and `resume` route
  from those structured tool decisions and hands both to the runtime request;
- `pg_qx_task` now persists those phase routes as
  `qxtasksubmitcontract`/`qxtaskresumecontract`, so later runtime paths do not
  have to guess from `first`/`last` authorized-tool ordering;
- runtime resume, retry, startup recovery, failover rebuild, and stale-attempt
  reclaim now reuse the persisted phase routes before falling back to legacy
  heuristics.

Planning model:
- the planner still reads the catalog-backed authorized-tool contracts produced
  by the security layer;
- it derives a capability class for each tool, such as `host-local`,
  `container-brokered`, `microvm-brokered`, or `remote-brokered`;
- it records a top-level execution surface summary:
  - `host-local` when the plan is fully local
  - `container-brokered` when the plan stays on container-backed principals
  - `microvm-brokered` when the plan stays on microVM-backed principals
  - `mixed-brokered` when the task spans multiple provider/runtime classes
- at landing time, the planner did not select a real container or microVM
  backend by itself; after the backend integration, its runtime-class decisions
  are consumed by the runtime path that launches Docker or QEMU.

Cost and selection assumptions:
- the existing token and cost accounting remains unchanged;
- capability decisions are derived from the authorized tool contracts, not from
  a separate optimizer;
- mixed provider kinds are allowed, because a single task may need both host and
  isolated execution surfaces in one plan;
- phase selection is explicit but intentionally simple:
  - `submit` prefers the least isolated capability route
  - `resume` prefers the most isolated capability route
  - ties stay stable by original authorized-tool order
- the runtime ABI still carries contract strings for physical launch
  compatibility, but those strings are now chosen from structured capability
  decisions rather than raw list position.

Runtime integration status:
- the runtime still executes the selected tool via contract strings for
  compatibility with earlier stages;
- Stage 29 still did not introduce a new physical backend ABI by itself;
- the structured plan data now drives the runtime handoff by choosing and
  persisting the per-phase contracts that real Docker/QEMU-backed execution
  should use.

Validation:
- `ninja -C build-stage4-codex -j 1 src/backend/postgres.exe`
- `meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs`
- focused direct `pg_regress qx_stage3_agentic` on Windows with:
  - real Docker provider (`PG_TEST_EXTRA=docker`, `QX_CONTAINER_IMAGE=alpine:3.20`)
  - real QEMU `microvm` provider (`QX_MICROVM_ACCEL=tcg` plus kernel/initrd assets)
- regression coverage now asserts that the task row stores a host `submit`
  route (`search`) and a microVM `resume` route (`summarize`) for the mixed
  planner case.
