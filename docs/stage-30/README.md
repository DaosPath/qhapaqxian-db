# Stage 30 - Security / Tool Capability Contract

## Stage intent

Stage 30 deepens the security contract between namespaces, principals,
providers, and tools so authorization can reason about more than a single
allowlist.

## What landed

The contract now carries:

- `runtime class` for each tool and principal, so authorization can require alignment instead of treating all tools as equivalent.
- `sandbox ceiling` for each tool, so a namespace policy can reject a tool whose execution profile exceeds the ceiling declared by the binding.
- `capability tags` for each tool, so future policy logic can filter by semantic capabilities instead of raw names only.

The current implementation is future-safe by design:

- catalog rows can store explicit tool contract fields when present;
- helper code derives sensible defaults when the fields are absent;
- authorization code validates the derived contract before building the runtime-facing authorization payload.

## What is intentionally deferred

This stage does not claim real OS-level enforcement.

What is still missing:
- no container or microVM broker is launched from the backend;
- no kernel-level sandbox policy is enforced by the database server itself;
- no remote attestation pipeline is validated end to end inside the repo;
- no policy compiler rewrites capability tags into a formal rule engine yet.

So Stage 30 is a contract hardening step, not the final enforcement layer.

## Why this stage matters

- it lets planner, executor, runtime, and security code talk about tool risk
  in a shared vocabulary instead of one-off strings;
- it makes later container, microVM, and attestation stages enforceable
  without redesigning tool rows again;
- it narrows future policy work to enforcement and compilation instead of
  catalog-shape churn.

## Catalog assumptions

- `pg_qx_tool` is the source of truth for tool metadata.
- `qxtoolruntimeclass`, `qxtoolsandboxceiling`, and `qxtoolcapabilitytags` are optional catalog columns.
- If a tool row omits one of those columns, the security layer derives the value from the bound principal and provider.
- Capability tags are serialized as a simple pipe-delimited list for now, but the contract code treats them as an ordered tag set, not as a fixed string.

## Validation

- No dedicated stage-local regression harness has been added yet.
- Validation remains indirect through authorization paths and the repository's
  broader PostgreSQL regression sweep.

## Next stage handoff

- Stage 31 can now attach richer verified execution metadata to semantic
  payloads because the capability contract is explicit.
- Later sandboxing work can consume runtime class and sandbox ceiling as
  engine-owned inputs instead of inventing another policy surface.
