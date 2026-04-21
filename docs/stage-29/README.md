# Stage 29: Capability-Aware Agent Planning

Stage 29 makes the planner and executor reason explicitly about runtime class
and tool capability instead of carrying only flat authorized-tool contracts.
The runtime handoff still uses the existing contract string path, but the plan
object now keeps structured capability decisions that the executor can verify.

What landed:
- `QxAgentPlan` now carries a structured runtime-decision summary and a list of
  structured per-tool decisions;
- each tool decision records the tool, handler, principal, provider, provider
  kind, principal runtime class, endpoint, receipt metadata, and derived
  capability class;
- `EXPLAIN AGENT` now emits the execution surface, unique provider kinds,
  unique principal runtime classes, capability tags, and per-tool decisions;
- the executor now rejects plans whose capability decisions are incomplete or
  internally inconsistent before handing them to the runtime boundary.

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
- the executor only validates consistency today; it does not yet route to
  different physical backends per tool.

Runtime integration status:
- the runtime still receives tool-contract strings for compatibility with
  earlier stages;
- Stage 29 did not introduce a new runtime ABI by itself;
- the structured plan data now complements the real container/microVM backend
  integration by making runtime class and provider capability visible before
  runtime handoff.

Validation:
- compile and regression validation are still pending for this stage at the
  time of this document update.
