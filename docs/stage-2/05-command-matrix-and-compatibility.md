# Stage 2 Command Matrix and Compatibility

This matrix is the decision aid for the boundary between a PostgreSQL extension
and a real QhapaqXian DB fork.

Principle:
- if a command only wraps existing SQL and metadata, it can start as a temporary
  bridge;
- if a command introduces engine-owned identity, lifecycle, recovery, or
  scheduler semantics, it belongs in the fork.

## Command Matrix

| Command | Compatibility | Expected command tags | Output | PostgreSQL tooling impact | Security implications | Needs fork? |
|---|---|---|---|---|---|---|
| `CREATE AGENT` | New command, not SQL-compatible in stock PostgreSQL | `CREATE AGENT` | new agent OID or name-bound identifier, plus command completion | `psql` and libpq still work, but generic SQL parsers and admin tools must learn a new command tag | creates an operational principal; must bind owner, namespace, policy, and budget | Yes |
| `ALTER AGENT` | New command, agent DDL only | `ALTER AGENT` | updated agent metadata and command completion | existing tooling can transport it as SQL text, but introspection tools need new catalog awareness | policy, tools, and budget changes require authorization checks | Yes |
| `DROP AGENT` | New command, destructive lifecycle action | `DROP AGENT` | drop completion and cascade or restrict result | backup/restore and migration tools must learn agent dependencies | must prevent orphaned sessions, tasks, and policies | Yes |
| `START SESSION` | New operational command | `START SESSION` | session id, effective principal, runtime state bootstrap | `psql` can issue it, but result decoding and admin views need new semantics | maps SQL role to agent principal; session scoping is mandatory | Yes |
| `END SESSION` | New operational command | `END SESSION` | closed session state | ordinary tooling can transmit it, but session lifecycle is agent-owned | must stop outstanding task leases and cancel or fence work | Yes |
| `RUN TASK` | New operational command, not plain SQL | `RUN TASK` | task id, attempt id, queue state, optional returned task row | generic SQL tools can send it, but scheduling and tracing tools need new views | enforces budget, tool ACLs, namespace rules, and execution identity | Yes |
| `CANCEL TASK` | New operational command | `CANCEL TASK` | cancellation acknowledgement and final task state | no native PostgreSQL equivalent; admin tooling needs new task-state views | must honor task ownership and cancel privileges | Yes |
| `CHECKPOINT TASK` | New operational command | `CHECKPOINT TASK` | checkpoint id and durable task state | backup tools and logical decoding may need special handling later | checkpoint payload may contain sensitive memory or trace context | Yes |
| `RESUME TASK` | New operational command | `RESUME TASK` | resumed attempt or failure reason | monitoring tools need to distinguish resume from new task creation | must fence stale leases and validate checkpoint ownership | Yes |
| `REMEMBER` | New semantic DML | `REMEMBER` | stored memory record or acknowledgment | `psql` can issue it, but normal ORM tooling will not understand it as standard DML | writes agent memory, so access scope and retention policy matter | Yes |
| `FETCH MEMORY` | New semantic read command | `FETCH MEMORY` | memory rows or ranked retrieval set | read-only tooling can transport it, but result ranking and explainability are new | must enforce namespace and agent-level read policy | Yes |
| `SHOW TRACE` | New introspection command | `SHOW TRACE` | trace rows, spans, or events | admin tools need new agent-aware views; ordinary `EXPLAIN` is insufficient | traces may expose prompts, tool calls, and secrets if not filtered | Yes |
| `EXPLAIN AGENT` | New explain/introspection command | `EXPLAIN AGENT` | structured agent plan and estimated costs | existing `EXPLAIN` consumers will not parse it without updates | can leak policy, budget, and tool topology; must respect visibility rules | Yes |
| `CREATE POLICY` | Could be extension-backed initially, but agent-aware policy needs fork ownership | `CREATE POLICY` or fork-specific policy tag | policy metadata | some policy management tooling may adapt, but not enough for runtime enforcement | policy must be tied to agent/session/task enforcement paths | Probably yes |
| `CREATE TOOL` | Could begin as metadata-only, but runtime enforcement points push toward fork ownership | `CREATE TOOL` | tool definition and ACL metadata | tooling can manage catalog rows, but executor/runtime must understand it | tool allowlists, timeouts, and idempotency class must be enforced | Probably yes |
| `CREATE MEMORY PROFILE` | Could start as metadata, but semantic memory planning likely needs fork ownership | `CREATE MEMORY PROFILE` | profile id and retrieval defaults | standard SQL tooling can issue it, but result semantics are custom | controls retention, scope, and retrieval policy | Probably yes |

## Interpretation

### Commands that clearly require the fork
- `CREATE AGENT`
- `ALTER AGENT`
- `DROP AGENT`
- `START SESSION`
- `END SESSION`
- `RUN TASK`
- `CANCEL TASK`
- `CHECKPOINT TASK`
- `RESUME TASK`
- `REMEMBER`
- `FETCH MEMORY`
- `SHOW TRACE`
- `EXPLAIN AGENT`

These commands define identity, lifecycle, runtime, memory, recovery, or
observability semantics that PostgreSQL extensions cannot make first-class
without fighting the engine boundary.

### Commands that can begin as bridge metadata
- `CREATE POLICY`
- `CREATE TOOL`
- `CREATE MEMORY PROFILE`

These can start as ordinary catalog-backed features, but they still point toward
fork ownership once the runtime consumes them directly.

## Tooling Boundary

PostgreSQL tooling can remain useful at the transport and session layer:
- `psql`
- `libpq`
- existing connection pools
- backup orchestration
- log collectors

But the following will need QhapaqXian-aware updates:
- admin views for sessions, tasks, and budgets
- trace inspection and explain consumers
- migration tools that assume only relational DDL/DML
- observability systems that key only on backend PID
- crash/recovery playbooks that do not understand task checkpoints

## Stage 2 Exit Criterion

Stage 2 is complete when:
- the command set is frozen at the syntax/spec level;
- every command has a compatibility decision in this matrix;
- every command is labeled as `fork` or `bridge` with a reason;
- the parser work in Stage 3 can proceed without reopening the boundary.
