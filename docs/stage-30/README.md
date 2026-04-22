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
- `USING` policy contracts on `CREATE/ALTER NAMESPACE POLICY`, so a policy
  can require or deny capability tags explicitly.

The implementation is future-safe by design:

- catalog rows can store explicit tool contract fields when present;
- helper code derives sensible defaults when the fields are absent;
- authorization code validates the derived contract before building the runtime-facing authorization payload.
- `CREATE AGENT` validates the namespace policy/tool contract up front, before
  the agent row is admitted.

The currently supported policy contract keys are:

- `require_capabilities=tag|tag`
- `require_capability=tag`
- `deny_capabilities=tag|tag`
- `deny_capability=tag`

Example:

```sql
CREATE NAMESPACE POLICY host_runtime_only FOR SCHEMA public
  TOOLS (summarize)
  USING 'require_capabilities=runtime:host'
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;
```

## What was intentionally deferred at landing time

This stage did not claim real OS-level enforcement by itself.

What was still missing at landing time:
- container and microVM backend launch was outside this stage's landing scope;
- no kernel-level sandbox policy is enforced by the database server itself;
- no remote attestation pipeline is validated end to end inside the repo;
- no OS-level OCI/VM policy compiler rewrites capability tags into kernel or
  hypervisor rules yet.

So Stage 30 is now an engine-enforced capability policy contract, not the final
kernel/container/hypervisor enforcement layer.

Post-stage integration note:
- the runtime can now launch real Docker and QEMU backend paths for
  `container://` and `microvm://` providers;
- capability tags, runtime class, and sandbox ceiling remain the policy inputs
  that future OCI, VM lifecycle, and attestation hardening should consume;
- the remaining gap is deeper enforcement and provenance, not absence of a
  real backend launch path.
- Windows QEMU/TCG validation uses the real 120000 ms brokered microVM timeout
  floor so the guest can boot, emit receipt evidence, and shut down without the
  child wrapper killing QEMU early.

## Why this stage matters

- it lets planner, executor, runtime, and security code talk about tool risk
  in a shared vocabulary instead of one-off strings;
- it makes container, microVM, and attestation stages enforceable without
  redesigning tool rows again;
- it narrows future policy work to enforcement and compilation instead of
  catalog-shape churn.

## Catalog assumptions

- `pg_qx_tool` is the source of truth for tool metadata.
- `qxtoolruntimeclass`, `qxtoolsandboxceiling`, and `qxtoolcapabilitytags` are optional catalog columns.
- If a tool row omits one of those columns, the security layer derives the value from the bound principal and provider.
- Capability tags are serialized as a simple pipe-delimited list for now, but the contract code treats them as an ordered tag set, not as a fixed string.

## Validation

- Focused `qx_stage3_agentic` regression covers both positive tool admission
  and explicit negative capability policy checks.
- The regression creates policies with `require_capabilities=runtime:host` and
  `deny_capabilities=runtime:microvm`, then verifies incompatible tool
  contracts fail during `CREATE AGENT`.
- The same focused sweep also exercises real Docker and real QEMU `microvm`
  paths when the required local backends are available.

## Next stage handoff

- Stage 31 can now attach richer verified execution metadata to semantic
  payloads because the capability contract is explicit.
- Later sandboxing work can consume runtime class and sandbox ceiling as
  engine-owned inputs instead of inventing another policy surface.
