# QhapaqXian DB - Architectural Blueprint v0

Status: initial architecture seed for an empty repository on 2026-03-26.

Current-state note:
- this blueprint is the architecture seed and target-design document;
- the canonical repository implementation status now lives in `STATUS.md`.

Explicit inferences:
- This repository is currently empty, so this document defines the first engineering baseline rather than describing existing code.
- The first fork should start from a clean upstream PostgreSQL 17.x stable import. This is a deliberate compatibility choice, not a claim that 17.x is the latest release.
- File and module paths below refer to the standard upstream PostgreSQL source layout.

Required thesis:
> QhapaqXian DB exists because a real AgentDB needs agents, memory, tasks, execution, recovery, and observability to be engine primitives, not only an external layer over SQL.

Throughout this document:
- `MVP del fork` = the narrowest implementation that still justifies a fork.
- `Arquitectura objetivo` = the intended durable design.
- `Deuda tecnica aceptada` = shortcuts allowed while the fork is still validating product shape.

## 1. Manifiesto tecnico del fork

### Que problema resuelve QhapaqXian DB
QhapaqXian DB is not trying to be "PostgreSQL plus jobs". It addresses a different execution model:
- agents have durable identity, policy, and budget;
- tasks are long-lived, multi-step, and resumable;
- memory is not just application data, but operational state used by execution;
- observability must follow agent/session/task identity, not only backend PID;
- recovery must reconstruct agent progress, not only row-level state.

Vanilla PostgreSQL is excellent at short transactional units over relational state. It does not natively model:
- task lifecycle across minutes or hours;
- checkpoint/restart semantics for multi-step execution;
- tool calls with retries, budgets, and compensation;
- semantic memory retrieval as part of planning;
- runtime state that survives crash/failover and resumes correctly.

### Por que una extension o plataforma externa no bastan como objetivo final

| Approach | Works for | Fails for QhapaqXian DB |
|---|---|---|
| Extension only | new types, functions, hooks, indexes | cannot make agent identity, task lifecycle, runtime recovery, and observability first-class engine semantics without fighting the core execution model |
| External middleware | orchestration, model routing, tool execution | splits truth across app and database, weakens crash recovery, duplicates authz/audit, and treats agent state as tables rather than engine primitives |
| SQL wrapper over tables | quick prototype | gives syntax sugar only; it does not change planner, runtime, recovery, security context, or semantic replication |
| Fork | parser, catalogs, runtime, planner, WAL hooks, observability, packaging | higher maintenance cost, but it is the only path that can make agents first-class citizens of the server |

Extension or middleware can be useful during early validation, but only as temporary scaffolding. They cannot be the final product architecture because they leave the hardest semantics outside the engine boundary.

### Que capacidades deben ser nativas del motor
The following must become engine primitives:
- agent DDL and operational commands;
- agent identity and policy enforcement;
- persistent session/task lifecycle;
- runtime scheduler and worker control;
- checkpoints and resumable execution state;
- budget enforcement and retry classes;
- event stream and semantic replication hooks;
- agent-aware observability and explainability.

The following can start simpler, but still belong to the fork:
- semantic memory storage on normal heap relations before custom indexes;
- runtime built on background workers before it becomes a deeper subsystem;
- logical decoding of agent events before dedicated WAL record families.

### Que diferencia a QhapaqXian DB de PostgreSQL + extensiones
QhapaqXian DB differs in kind, not only in packaging:
- `PostgreSQL + extensions` still sees agents as data.
- `QhapaqXian DB` sees agents as operational principals with lifecycle, policy, runtime, and recovery semantics.

Concrete boundary:
- If a capability needs new grammar, new system catalogs, new ACL semantics, new runtime state, or recovery semantics, it belongs in the fork.
- If a capability only accelerates an internal implementation without changing public semantics, it may begin as a temporary patch.

### MVP del fork
- Native grammar and AST for agent commands.
- Native catalogs for agent metadata and runtime metadata.
- Background-worker runtime with durable task state.
- Checkpoint/resume semantics at task-step boundaries.

### Arquitectura objetivo
- Dedicated agent runtime subsystem.
- Agent-aware planner/executor and `EXPLAIN AGENT`.
- Semantic replication stream and recovery-aware resume.
- Storage/indexing specialized only where measured pressure justifies it.

### Deuda tecnica aceptada
- Runtime over bgworkers.
- Heap tables for most memory/event/trace data.
- Logical decoding for semantic events before deeper WAL work.

## 2. Arquitectura de alto nivel

### Componentes principales del sistema
1. SQL Frontend
   - retains normal SQL, DDL, DML, transactions, wire protocol.
2. Agentic Frontend
   - parses agent commands and compiles them into native agent statements.
3. Catalog Layer
   - stores agents, identities, tools, policies, sessions, tasks, checkpoints, budgets.
4. Agent Runtime
   - scheduler, dispatcher, workers, heartbeats, retries, cancellation, recovery scanner.
5. Agent Planner
   - produces `AgentPlan` graphs for multi-step tasks.
6. Memory Subsystem
   - working memory, episodic memory, semantic memory, trace and event persistence.
7. Recovery and Replication Layer
   - task recovery after crash, semantic event emission, failover reconstruction.
8. Security and Identity Layer
   - maps SQL principals to agent principals and enforces policy.
9. Observability Layer
   - traces, task state, budgets, `SHOW TRACE`, `EXPLAIN AGENT`, `pg_stat_agent_*`.

### Que se mantiene de PostgreSQL
- parser, planner, executor, and MVCC for ordinary SQL;
- heap/table access, buffer manager, storage manager, WAL, and crash recovery baseline;
- FE/BE protocol and common drivers;
- role system, ACL base, GUC infrastructure, stats framework;
- background worker framework as the initial runtime substrate;
- test harnesses: `pg_regress`, isolation tests, TAP/recovery tests.

### Que se reemplaza o amplia
- parser grammar and keywords for agentic commands;
- command tags and utility statement dispatch;
- system catalogs and internal relations for agent metadata and runtime state;
- stats views, trace surfaces, and explain output;
- planner and executor paths for agent tasks;
- recovery scanner and semantic event emission.

### Que subsistemas nuevos se introducen
- `Agent Language Compiler`
- `Agent Catalog Manager`
- `Agent Runtime`
- `Agent Scheduler`
- `Agent Recovery Scanner`
- `Agent Memory Manager`
- `Agent Event Stream`
- `Agent Security Context`
- `Agent Explain and Trace`

### Relacion entre SQL tradicional y lenguaje agentic
The relationship is layered, not adversarial:
- SQL remains the relational substrate.
- Agentic commands define operational behavior over that substrate.
- `AgentPlan` nodes may embed normal SQL subplans when a step is relational.
- Agent tasks are not compiled into one giant SQL query. They are compiled into a task graph with step-level transactions.

Execution model:
- SQL statement -> standard planner/executor path.
- Agent statement -> agent parser/analyzer -> task state machine -> runtime scheduler -> per-step execution.

### MVP del fork
- Agentic commands go through `ProcessUtility`.
- Agent runtime executes step-transactions and persists state to internal relations.
- SQL remains untouched except where embedded inside agent steps.

### Arquitectura objetivo
- Agent planner and executor become native, inspectable subsystems.
- Semantic retrieval, tool calls, checkpoints, branching, and retries become planned nodes.
- Observability and replication operate on agent semantics rather than only row changes.

### Deuda tecnica aceptada
- Separate utility-path execution before deeper planner integration.
- Runtime queues mirrored in tables plus shared memory.

## 3. Mapa del codigo base a tocar

The fork should isolate most new code under `src/backend/qx/` and `src/include/qx/`, while still touching core files where unavoidable.

| Subsystem | Upstream files/modules | Type of change | Why fork is justified |
|---|---|---|---|
| Parser / grammar / AST | `src/backend/parser/gram.y`, `scan.l`, `src/include/parser/kwlist.h`, `src/include/nodes/parsenodes.h`, `src/backend/nodes/{copyfuncs,equalfuncs,outfuncs,readfuncs}.c` | new syntax, keywords, AST nodes | extensions cannot own first-class grammar and AST lifecycle cleanly enough for this product |
| Protocol / command lifecycle | `src/backend/tcop/postgres.c`, `src/backend/tcop/utility.c`, `src/include/tcop/cmdtaglist.h`, `src/backend/access/common/printtup.c` | command tags, utility dispatch, result shaping | agent commands must look like engine-native commands |
| System catalogs | `src/include/catalog/*.h`, `*.dat`, `src/backend/catalog/*`, `genbki.pl`, `system_views.sql`, `src/backend/utils/cache/*` | new catalogs, relcache/syscache entries, admin views | agent identity and policy must be server metadata, not user tables |
| Planner | `src/backend/optimizer/{plan,path,util}/*`, `src/include/optimizer/*` | `AgentPlan`, costing, planning hooks | task graphs are not representable as normal relational plans |
| Executor | `src/backend/executor/*`, `src/include/executor/*`, `src/backend/executor/spi.c` | step execution, budget checks, context propagation | tool calls, checkpoints, retries, and task state are not plain SQL execution |
| Background workers / scheduler | `src/backend/postmaster/{postmaster,bgworker}.c`, `src/backend/storage/ipc/{shmem,latch,procsignal}.c`, new `src/backend/qx/runtime/*` | runtime launcher, scheduler, worker lifecycle | orchestration must survive inside the server boundary |
| Storage / access methods | `src/backend/access/{heap,table,index}/*`, maybe later new AM/index files, `src/include/access/*` | initially none or minimal; deeper only if measured | custom storage is expensive and should be justified by real pressure |
| WAL / logical decoding / replication | `src/backend/access/transam/*`, `src/backend/replication/logical/*`, `src/backend/replication/{slot,walsender}.c`, `src/include/access/xlog*.h` | semantic event decoding first; deeper WAL families later | task recovery and semantic replication eventually need more than row-level CDC |
| Security / identity | `src/backend/catalog/aclchk.c`, `src/backend/commands/user.c`, `src/backend/utils/init/miscinit.c`, `src/backend/libpq/auth.c`, `src/include/utils/acl.h` | agent principal, ACLs, policy checks | roles alone are insufficient to represent agent execution identity |
| Observability / tracing / explain | `src/backend/commands/explain.c`, `src/backend/utils/activity/pgstat*.c`, `backend_status.c`, `src/backend/utils/error/*` | new views, trace APIs, explain output | backend-centric stats are insufficient for task/session identity |
| Tooling / build / tests | `GNUmakefile`, `meson.build`, `src/test/regress`, `src/test/isolation`, `src/test/recovery`, `src/test/subscription`, `src/test/perl`, packaging scripts | build, CI, test suites, release pipeline | fork maintenance requires operational discipline from day 1 |

### Recommended new module layout
- `src/backend/qx/catalog/`
- `src/backend/qx/runtime/`
- `src/backend/qx/scheduler/`
- `src/backend/qx/recovery/`
- `src/backend/qx/memory/`
- `src/backend/qx/planner/`
- `src/backend/qx/executor/`
- `src/backend/qx/security/`
- `src/backend/qx/observe/`
- `src/include/qx/`

### MVP del fork
- Touch parser, utility dispatch, catalogs, bgworkers, basic observability, tests.

### Arquitectura objetivo
- Add planner/executor integration, semantic replication, and deeper identity enforcement.

### Deuda tecnica aceptada
- Many agent commands start as utility statements before planner integration.
- Runtime reconstruction scans durable tables at startup before richer WAL-first recovery.

## 4. Diseno del lenguaje agentic inicial

### Filosofia del lenguaje
The language should be:
- operational, not decorative;
- explicit about identity, session, task, and checkpoint boundaries;
- inspectable by the engine;
- deterministic around control flow, while allowing non-deterministic reasoning inside controlled steps;
- interoperable with SQL, not a replacement for SQL.

The first version should avoid pretending to be a natural-language shell. It should be a strict declarative/operational language with SQL-like surface and engine-defined state transitions.

### Sintaxis inicial

```sql
CREATE AGENT archivist
  IDENTITY svc_archivist
  MODEL 'builtin://reasoner/default'
  MEMORY PROFILE default_mem
  TOOLS (doc_fetch, sql_readonly)
  POLICY strict_default
  BUDGET TOKENS 200000 COST_LIMIT 25.00 TIME_LIMIT '30 min';

ALTER AGENT archivist
  SET MODEL 'builtin://reasoner/v2'
  ADD TOOL summary_tool
  DROP TOOL sql_readonly;

START SESSION FOR AGENT archivist
  WITH CONTEXT '{"case_id":"42","tenant":"qhapaq"}'
  RETURNING SESSION;

RUN TASK summarize_case
  IN SESSION 1001
  GOAL 'produce a concise, auditable case summary'
  INPUT '{"case_id":"42"}'
  PRIORITY HIGH
  RETURNING TASK;

REMEMBER
  IN SESSION 1001
  SCOPE EPISODIC
  KEY 'case.42.summary'
  VALUE '{"summary":"partition repaired"}'
  TAGS ('incident','case42');

FETCH MEMORY
  FOR AGENT archivist
  SCOPE EPISODIC, SEMANTIC
  MATCH 'partition repaired for case 42'
  LIMIT 10;

CHECKPOINT TASK 7001
  LABEL 'after_summary';

RESUME TASK 7001
  FROM CHECKPOINT 'after_summary';

SHOW TRACE FOR TASK 7001
  LIMIT 100;

EXPLAIN AGENT
RUN TASK summarize_case
  IN SESSION 1001
  GOAL 'produce a concise, auditable case summary'
  INPUT '{"case_id":"42"}';
```

### Gramatica inicial

```ebnf
CreateAgentStmt:
  CREATE AGENT ColId OptAgentIdentity OptAgentModel OptAgentMemoryProfile
  OptAgentTools OptAgentPolicy OptAgentBudget

AlterAgentStmt:
  ALTER AGENT ColId AlterAgentCmdList

StartSessionStmt:
  START SESSION FOR AGENT qualified_name OptSessionContext OptReturningClause

RunTaskStmt:
  RUN TASK OptTaskName IN SESSION a_expr GOAL Sconst
  OptTaskInput OptTaskPriority OptReturningClause

RememberStmt:
  REMEMBER IN SESSION a_expr SCOPE MemoryScope KEY Sconst
  VALUE a_expr OptTagsClause

FetchMemoryStmt:
  FETCH MEMORY FOR AGENT qualified_name OptMemoryScopeList
  MATCH a_expr OptLimitClause

CheckpointTaskStmt:
  CHECKPOINT TASK a_expr OptCheckpointLabel

ResumeTaskStmt:
  RESUME TASK a_expr OptCheckpointRef

ShowTraceStmt:
  SHOW TRACE FOR TASK a_expr OptLimitClause

ExplainAgentStmt:
  EXPLAIN AGENT utility_statement
```

### Comandos minimos
- `CREATE AGENT`
- `ALTER AGENT`
- `START SESSION`
- `RUN TASK`
- `REMEMBER`
- `FETCH MEMORY`
- `CHECKPOINT TASK`
- `RESUME TASK`
- `SHOW TRACE`
- `EXPLAIN AGENT`

### AST conceptual
- `CreateAgentStmt`
  - `agent_name`
  - `identity_name`
  - `model_uri`
  - `memory_profile`
  - `tools`
  - `policy_name`
  - `budget_spec`
- `AlterAgentStmt`
  - `agent_name`
  - `cmds`
- `StartSessionStmt`
  - `agent`
  - `context_expr`
  - `returning`
- `RunTaskStmt`
  - `task_name`
  - `session_id_expr`
  - `goal`
  - `input_expr`
  - `priority`
  - `returning`
- `RememberStmt`
  - `session_id_expr`
  - `scope`
  - `key`
  - `value_expr`
  - `tags`
- `FetchMemoryStmt`
  - `agent`
  - `scopes`
  - `match_expr`
  - `limit`
- `CheckpointTaskStmt`
  - `task_id_expr`
  - `label`
- `ResumeTaskStmt`
  - `task_id_expr`
  - `checkpoint_label`
- `ShowTraceStmt`
  - `task_id_expr`
  - `limit`
- `ExplainAgentStmt`
  - `stmt`
  - `flags`

### Validaciones semanticas
- agent identity, policy, and tools must exist at `CREATE AGENT` time;
- only authorized SQL roles may start a session for a given agent;
- `RUN TASK` requires an active session in the same namespace and with sufficient budget;
- `REMEMBER` requires serializable payload and a valid memory scope;
- `FETCH MEMORY` must be scoped to accessible agent memory only;
- `CHECKPOINT TASK` only works for tasks in checkpointable states;
- `RESUME TASK` requires a resumable checkpoint and no live active lease;
- `EXPLAIN AGENT` accepts only agent statements.

### Errores principales
- `ERRCODE_UNDEFINED_AGENT`
- `ERRCODE_UNDEFINED_AGENT_IDENTITY`
- `ERRCODE_AGENT_POLICY_VIOLATION`
- `ERRCODE_AGENT_BUDGET_EXCEEDED`
- `ERRCODE_INVALID_AGENT_STATE`
- `ERRCODE_INVALID_CHECKPOINT`
- `ERRCODE_AGENT_TOOL_NOT_ALLOWED`
- `ERRCODE_AGENT_NAMESPACE_VIOLATION`
- `ERRCODE_AGENT_RESUME_CONFLICT`

### MVP del fork
- Native parser, AST, semantic validation, command tags, utility handlers.

### Arquitectura objetivo
- Agent statements compile into `AgentPlan`, not only direct utility actions.

### Deuda tecnica aceptada
- `RUN TASK` can start as a utility-driven task graph creation statement before it becomes planner-driven.

## 5. Modelo de datos nativo de agentes

The data model should split stable metadata from high-churn runtime state.

### Principios
- true engine metadata goes into true catalogs;
- high-churn runtime data goes into system-managed internal relations, not catcache-heavy catalogs;
- all relations remain engine-owned and non-user-managed even if stored as ordinary heap relations at first.

### Entidades base

| Entity | Engine primitive | Initial physical form | Target form | Notes |
|---|---|---|---|---|
| Agents | yes | catalog `pg_agent` | durable catalog | stable metadata, owner, namespace, defaults |
| Agent identities | yes | catalog `pg_agent_identity` | durable catalog + security hooks | maps SQL principals to operational agent principals |
| Namespaces | yes | catalog `pg_agent_namespace` | durable catalog | tenant/isolation boundary |
| Sessions | yes | system-managed relation `pg_agent_session` | runtime-aware relation + shmem cache | high churn, not ideal for catcache |
| Tasks | yes | system-managed relation `pg_agent_task` | same + planner linkage | includes status, goal, policy, budget snapshot |
| Task attempts | yes | relation `pg_agent_attempt` | same + recovery/WAL hooks | one row per execution attempt |
| Steps | yes | relation `pg_agent_step` | same + explain linkage | persistent step graph and outcomes |
| Working memory | yes | relation `pg_agent_workmem` | relation + shmem hot cache | mutable, short-lived, TOASTed payloads |
| Episodic memory | yes | relation `pg_agent_episode` | append-optimized relation if needed | append-heavy, partitionable |
| Semantic memory | yes | relation `pg_agent_semantic` | relation + native ANN index/type later | should begin simple |
| Tools | yes | catalog `pg_agent_tool` | durable catalog | whitelist, timeout, cost, idempotency class |
| Policies | yes | catalog `pg_agent_policy` | durable catalog | runtime and authz rules |
| Events | yes | relation `pg_agent_event` | relation + semantic event stream | canonical event log |
| Traces | yes | relation `pg_agent_trace` | partitioned relation or custom append store later | for observability and audit |
| Budgets | yes | relation `pg_agent_budget_ledger` | same + fast counters in shmem | durable accounting |
| Checkpoints | yes | relation `pg_agent_checkpoint` | same + dedicated WAL later if justified | resume boundary |

### Cuales son primitivas del motor
These are engine primitives, even if they begin on ordinary relations:
- agent;
- agent identity;
- session;
- task;
- task attempt;
- checkpoint;
- budget ledger;
- event.

Reason: each of them affects execution semantics, not only storage.

### Cuales son tablas o catalogos internos
True catalogs:
- `pg_agent`
- `pg_agent_identity`
- `pg_agent_namespace`
- `pg_agent_tool`
- `pg_agent_policy`

System-managed internal relations:
- `pg_agent_session`
- `pg_agent_task`
- `pg_agent_attempt`
- `pg_agent_step`
- `pg_agent_workmem`
- `pg_agent_episode`
- `pg_agent_semantic`
- `pg_agent_event`
- `pg_agent_trace`
- `pg_agent_budget_ledger`
- `pg_agent_checkpoint`

### Cuales requieren formatos especiales
Start simple unless there is measured evidence:
- working memory snapshot payload: versioned binary or `jsonb` initially;
- checkpoint state blob: versioned binary payload;
- semantic memory embedding field: `float4[]` or built-in `qx_embedding` later;
- trace payload: structured `jsonb` initially, compacted format later.

### Cuales podrian empezar simples y luego migrar a algo mas profundo
- semantic memory: heap + sequential or filtered search -> native ANN index later;
- traces/events: append-only heap + partitions -> custom append store only if write volume proves it necessary;
- checkpoints: TOASTed blobs -> delta or binary snapshot store later;
- working memory: heap + shmem cache -> specialized hot/cold split only after profiling.

### MVP del fork
- Catalogs for identities, tools, policies, agents.
- Internal relations for sessions, tasks, attempts, steps, memories, events, traces, checkpoints, budgets.

### Arquitectura objetivo
- Agent metadata remains catalog-backed.
- Runtime state becomes tightly integrated with scheduler, recovery, and explain surfaces.

### Deuda tecnica aceptada
- Most operational state begins on ordinary logged relations.
- No custom type or AM is required on day 1.

## 6. Runtime embebido y scheduler

### Diseno del runtime interno
The runtime should have two planes:
- control plane: durable state and policy;
- execution plane: runnable queue, workers, heartbeats, and live leases.

### Workers
- `QX Launcher`
  - boots the runtime and starts supervisor workers.
- `QX Scheduler`
  - computes runnable order from priority, budget, retry state, and fairness.
- `QX Dispatcher`
  - assigns work to free workers.
- `QX Executor Worker`
  - executes one task-step attempt inside a normal SQL transaction.
- `QX Reaper`
  - detects lease expiry and orphaned attempts.
- `QX Budget Keeper`
  - debits budgets and can force degrade or stop.
- `QX Recovery Scanner`
  - reconstructs runnable state at startup or after failover.

### Colas
Initial design:
- durable queue state in internal relations with status values:
  - `READY`
  - `RUNNING`
  - `WAITING_RETRY`
  - `BLOCKED`
  - `CHECKPOINTED`
  - `DONE`
  - `FAILED`
  - `CANCELLED`
- hot priority queue in shared memory to avoid constant polling.

Target design:
- server-owned scheduler queue in shared memory with latch wakeups and deterministic reconstruction from durable state.

### Scheduling
Effective priority:
`effective_priority = base_priority + aging + namespace_weight - budget_pressure - retry_penalty`

Scheduling constraints:
- max concurrency per agent;
- max concurrency per namespace;
- tool class throttles;
- fairness across sessions;
- starvation protection through aging.

### Retries
Retry unit = `attempt`, not whole task.

Retry policy fields:
- `max_attempts`
- `retry_backoff`
- `retry_jitter`
- `retry_class` = `TRANSIENT | TOOL | RESOURCE | SEMANTIC`

Semantic failures should not retry blindly. They should allow replan or compensation.

### Timeouts
Timeout classes:
- hard wall-clock timeout;
- soft wall-clock timeout;
- tool call timeout;
- idle heartbeat timeout;
- queue wait timeout.

### Cancelacion
Cooperative path:
- mark attempt `CANCEL_REQUESTED`;
- signal backend interrupt;
- worker stops at a safe point and persists failure or checkpoint state.

Forced path:
- terminate backend after hard timeout;
- recovery scanner decides resume or terminal failure.

### Heartbeats
Each worker persists:
- current step;
- heartbeat timestamp;
- current lease token;
- current budget counters;
- last durable checkpoint.

Reaper uses lease expiration, not heartbeat timestamp alone.

### Limites de presupuesto
Budget types:
- wall time;
- CPU-equivalent;
- IO-equivalent;
- tokens/model cost;
- tool calls;
- memory bytes.

Enforcement points:
- pre-admission in scheduler;
- safe points inside step execution;
- durable budget ledger for audit and replay.

### Control de concurrencia
Normal MVCC is necessary but insufficient.

Additional semantic coordination is needed for:
- `agent_id`
- `session_id`
- `task_id`
- `checkpoint_id`
- selected memory keys

Goal:
- prevent dual resume;
- prevent stale worker takeover;
- prevent concurrent mutation of the same task state machine.

### Debe vivir sobre background workers al inicio o ser subsistema propio
- `MVP del fork`: build the runtime on background workers plus shared memory plus internal relations.
- `Arquitectura objetivo`: evolve into a dedicated `AgentRuntime` subsystem with clearer APIs, but still managed by postmaster lifecycle.
- `Deuda tecnica aceptada`: table-backed queue reconstruction at startup.

## 7. Semantica transaccional orientada a agentes

### Punto de partida
PostgreSQL transactions are short-lived ACID units. Agent tasks are long-lived workflows. QhapaqXian DB should not pretend a one-hour agent task is one SQL transaction.

### Lo que hereda del modelo transaccional tradicional de PostgreSQL
- MVCC and isolation levels for each step transaction;
- WAL durability and crash recovery for committed internal state;
- savepoints within one step;
- ordinary row/index locking where relational work happens.

### Lo que QhapaqXian DB tiene que redefinir
- unit of progress: `task lifecycle`, not one open transaction;
- partial durability: checkpoint boundary, not partial SQL commit;
- recovery target: task state machine, not just relation pages;
- compensation rules for external side effects;
- semantic leases to fence resumptions.

### Checkpoints
Checkpoint is a durable barrier between step transactions.

It should persist:
- task id and attempt id;
- next runnable step or branch edge;
- working memory snapshot or delta;
- outputs required for resume;
- remaining budget;
- policy snapshot;
- current trace boundary.

Checkpoint is not:
- a long-running SQL snapshot;
- an open transaction continuation;
- a promise of serializable world state across hours.

### Commit parcial o equivalente
There should be no fake partial SQL commit primitive.

Equivalent concept:
- each step runs in its own short SQL transaction;
- when the step finishes, task state and checkpoint are committed;
- task-level progress is therefore durable in stages.

This is saga-style durable progress, not ACID over the whole task.

### Compensacion
Compensation is required for tool calls and other external side effects.

Model:
- each side-effectful step may declare a compensation action or be marked non-compensable;
- compensation steps are durable and auditable;
- SQL rollback only handles internal uncommitted changes.

### Reanudacion
`RESUME TASK` means:
- load last committed checkpoint;
- validate there is no active live lease;
- open a fresh attempt;
- reconstruct the execution context;
- continue from a known state boundary.

### Recovery despues de crash
Crash recovery should work in two layers:
1. PostgreSQL WAL replay restores committed internal relations.
2. Agent recovery scanner reclassifies task attempts:
   - `RUNNING` with dead backend -> `ORPHANED`
   - `ORPHANED` with valid checkpoint -> `READY_FOR_RESUME`
   - `ORPHANED` without resumable boundary -> `FAILED_RECOVERABLE` or `FAILED_TERMINAL`

### Aislamiento
Per-step isolation uses normal PostgreSQL levels:
- `READ COMMITTED`
- `REPEATABLE READ`
- `SERIALIZABLE`

Task-level isolation must be weaker and explicit:
- session can pin selected reference inputs into working memory or checkpoint payload;
- task cannot claim a single serializable snapshot across long duration without unacceptable cost.

### Consistencia
Consistency promises should be stated precisely:
- relational consistency is guaranteed at each committed step;
- agent task consistency is guaranteed at checkpoint boundaries;
- external tools are only consistent to the degree they are idempotent or compensable.

### Limites del modelo
- no full ACID transaction across a long-running multi-tool task;
- no automatic rollback of external effects;
- no perfect snapshot continuity across long wall-clock durations;
- semantic retries need declared policies and idempotency hints.

### MVP del fork
- step-transactions, checkpoints, resume, compensation metadata, crash scanner.

### Arquitectura objetivo
- richer checkpoint WAL semantics, lease fencing, and agent-aware recovery on failover.

### Deuda tecnica aceptada
- recovery scanner reconstructs from internal relations before dedicated WAL families exist.

## 8. Planner y executor agent-aware

### Como se representa un AgentPlan
`AgentPlan` should be a persistent execution graph, not a disguised SQL query plan.

Conceptual shape:

```text
AgentPlan
  agent_oid
  session_id
  task_id
  goal
  policy_oid
  budget_snapshot
  resume_policy
  trace_policy
  steps[]
  edges[]
  plan_version
  replan_count
```

Step node types:
- `AgentScanMemoryStep`
- `AgentSqlStep`
- `AgentToolCallStep`
- `AgentReasonStep`
- `AgentCheckpointStep`
- `AgentBranchStep`
- `AgentCompensateStep`
- `AgentReturnStep`

### Como se costean pasos
Traditional SQL costing is insufficient.

Agent cost dimensions:
- expected latency;
- SQL IO cost;
- token/model cost;
- failure risk;
- retry probability;
- checkpoint recovery cost;
- trace/event emission cost.

Conceptual formula:

```text
agent_cost =
  w_latency * expected_ms +
  w_io      * rel_io_cost +
  w_token   * expected_token_cost +
  w_tool    * failure_risk_penalty +
  w_resume  * checkpoint_recovery_cost
```

### Como se modelan tool calls
Tool calls are external, effectful operations.

Each `AgentToolCallStep` needs:
- tool id;
- timeout;
- retry class;
- idempotency class;
- compensation hook;
- expected cost and latency;
- budget debit rule.

Tool calls must not be smuggled through normal SQL costing as if they were stable functions.

### Como se modela recuperacion semantica
Semantic retrieval is its own step type.

`AgentScanMemoryStep` should include:
- memory scope;
- similarity predicate;
- metadata filters;
- top-k;
- confidence threshold;
- expected recall/latency stats.

In MVP it may use simple physical storage and estimation. In the target architecture it should participate in planning and cost estimation like any other access strategy.

### Como se modelan retries, branching y replanificacion
- retries are edges with backoff policy;
- branching is a graph edge conditioned on step output or policy;
- replanification creates a new `plan_version` from a checkpoint boundary;
- failure classes determine whether to retry, compensate, branch, or replan.

### Como seria EXPLAIN AGENT
`EXPLAIN AGENT` should show:
- agent and policy identity;
- task goal;
- budget snapshot;
- step graph;
- checkpoint boundaries;
- retry and compensation rules;
- embedded SQL subplans where relevant;
- estimated latency/token/failure cost.

Example shape:

```text
AgentPlan v3
  Agent: renewal_analyst
  Policy: strict_finance
  Budget: 12000 tokens, 15 tool calls
  Resume: checkpointable
  Steps:
    1. ScanSemanticMemory
       expected_rows: 12
       cost: latency=18ms token=0 risk=low
    2. SqlStep
       relplan: Index Scan using invoices_customer_id_idx
       cost: 2.13..18.44
    3. ToolCall
       tool: risk_model_v2
       timeout: 1500ms
       retry: 2 exponential
       cost: latency=900ms token=350 risk=medium
    4. Checkpoint
       persist: workmem, step_output, trace_span
    5. Branch
       if score > 0.8 -> step 6 else step 7
```

### MVP del fork
- persistent task graph + utility-driven execution + explain skeleton.

### Arquitectura objetivo
- native `AgentPlan`, agent-aware costing, replanning, branch-aware execution, SQL subplans as embedded children.

### Deuda tecnica aceptada
- first explain output may be metadata-driven before cost estimation becomes rich.

## 9. Estrategia de storage y memoria

### Memoria de trabajo
- `MVP del fork`
  - ordinary logged relation `pg_agent_workmem`
  - payload in `jsonb` or versioned binary blob
  - hot cache in shared memory
- `Arquitectura objetivo`
  - typed hot/cold split if mutation pressure justifies it
- `Deuda tecnica aceptada`
  - TOAST-heavy payloads and coarse invalidation

### Memoria episodica
- `MVP del fork`
  - append-heavy heap relation partitioned by time or agent
  - indexes:
    - btree `(agent_id, created_at desc)`
    - GIN on metadata payload
    - BRIN if time-local scans dominate
- `Arquitectura objetivo`
  - keep on ordinary relations unless profiling proves otherwise

### Memoria semantica
- `MVP del fork`
  - relation `pg_agent_semantic`
  - embedding stored as `float4[]` or simple built-in type
  - metadata filters + sequential or filtered search
- `Arquitectura objetivo`
  - native ANN index AM or index implementation with MVCC-aware maintenance
- `Deuda tecnica aceptada`
  - slower top-k retrieval while product semantics are being proven

### Trazas
- `MVP del fork`
  - append-only partitioned relation `pg_agent_trace`
  - columns:
    - `trace_id`
    - `span_id`
    - `task_id`
    - `step_id`
    - `event_ts`
    - `payload`
- `Arquitectura objetivo`
  - compressed append store only if trace volume dominates write path

### Snapshots
- `MVP del fork`
  - checkpoint rows in `pg_agent_checkpoint`
  - stable serialized `AgentExecutionState`
- `Arquitectura objetivo`
  - binary versioned format with delta checkpoints if size becomes a problem

### Eventos
- `MVP del fork`
  - append-only relation `pg_agent_event`
  - canonical event log for recovery and replication
- `Arquitectura objetivo`
  - semantic event stream with LSN-aware ordering

### Que puede vivir sobre tablas normales al inicio
- agents
- identities
- namespaces
- sessions
- tasks
- attempts
- steps
- working memory
- episodic memory
- semantic memory
- tools
- policies
- events
- traces
- budgets
- checkpoints

### Que podria requerir access methods propios
- semantic memory ANN indexing;
- trace/event append store only if heap/partitioning stops being adequate;
- checkpoint delta store only if checkpoint size and write amplification become material.

### Que podria requerir indices o estructuras nuevas
- native ANN index for semantic retrieval;
- hybrid metadata + semantic ranking support;
- lightweight sequence/fencing structure for task lease ownership.

### Cuando tiene sentido cambiar realmente el almacenamiento base
Only when all of the following are true:
- there is a measured bottleneck in production-like workloads;
- planner/runtime semantics are already stable enough to preserve;
- heap/partitioning/index tuning is no longer sufficient;
- the rebase cost is justified by repeatable benchmark evidence.

## 10. WAL, eventos y replicacion semantica

### Eventos de tareas
Canonical events should include:
- `AGENT_CREATED`
- `AGENT_ALTERED`
- `SESSION_STARTED`
- `TASK_ENQUEUED`
- `STEP_STARTED`
- `STEP_COMPLETED`
- `STEP_FAILED`
- `TASK_CHECKPOINTED`
- `TASK_RESUMED`
- `TASK_CANCELLED`
- `TASK_COMPLETED`

### Eventos de memoria
- `WORKMEM_UPDATED`
- `EPISODE_APPENDED`
- `SEMANTIC_MEMORY_UPSERTED`

### Checkpoints
Checkpoint event payload should include:
- task id;
- checkpoint id;
- attempt id;
- next step pointer;
- budget remainder;
- resume token;
- LSN or logical ordering marker.

### Trazas
Trace spans should be durable enough for audit, but can be emitted with sampling or batching policy.

### Reanudacion
Resume should be reconstructable from:
- internal relations restored by WAL replay;
- durable checkpoint row;
- attempt state;
- event ordering.

### Failover y reconstruccion de estado
At promotion time:
1. shared-memory queues are discarded;
2. agent recovery scanner rebuilds runnable state from durable relations;
3. stale leases are fenced;
4. resumable tasks are requeued from last committed checkpoint.

### Que se puede resolver con logical decoding al principio
- semantic event streaming from internal relations;
- CDC integration for observability;
- downstream audit consumers;
- external replicas that consume agent event semantics.

Initial design:
- keep internal event log relation canonical;
- add a QhapaqXian logical output plugin or semantic decoder;
- do not change base WAL record format yet.

### Que justificaria tocar WAL o replicacion de forma mas profunda
Touch deeper WAL only if one or more of these become true:
- relation-based reconstruction is too slow or ambiguous;
- checkpoint ordering needs a more atomic replay model;
- exact-once resume semantics require tighter write ordering;
- semantic event decoding cost becomes too high;
- failover recovery needs direct redo of task state machine transitions.

Potential deeper work:
- dedicated WAL record families:
  - `XLOG_AGENT_TASK_CREATE`
  - `XLOG_AGENT_STEP_STATE`
  - `XLOG_AGENT_CHECKPOINT`
  - `XLOG_AGENT_EVENT_APPEND`
  - `XLOG_AGENT_BUDGET_DEBIT`

### MVP del fork
- WAL durability via ordinary logged internal relations.
- Semantic stream via logical decoding.
- Recovery scanner after restart or failover.

### Arquitectura objetivo
- optional dedicated WAL families for agent task state and checkpoints.
- semantic replication stream as a first-class server feature.

### Deuda tecnica aceptada
- no specialized rmgr on day 1.
- failover reconstruction by scanning durable tables.

## 11. Seguridad e identidad de agentes

### Principio
An agent is an operational entity, not only a stored row.

### Autenticacion
Early phases:
- client authenticates as a normal PostgreSQL role;
- server maps that role to allowed agent principals;
- `START SESSION FOR AGENT ...` creates an effective `AgentPrincipal`.

Later option:
- direct agent-aware authentication or session assumption protocol if there is clear product value.

### Autorizacion
Authorization must combine:
- SQL role privileges;
- agent namespace membership;
- agent ownership/admin rights;
- tool allowlist;
- memory access policy;
- budget ceilings;
- task/session overrides within policy maxima.

### Namespaces
Namespaces should be first-class:
- isolation boundary for tenants or domains;
- default scope for agents, tools, and memory;
- scheduler fairness boundary;
- policy boundary.

### Acceso a tools
Each tool definition should include:
- owner;
- risk class;
- timeout ceiling;
- idempotency class;
- cost model;
- allowlist of agents/namespaces.

### Auditoria
Every sensitive action should log:
- SQL principal;
- effective agent principal;
- namespace;
- session id;
- task id;
- step id;
- policy applied;
- budget debit;
- outcome.

### Budget enforcement
Budget enforcement belongs inside runtime/executor, not in app logic.

Enforced dimensions:
- wall time;
- tool calls;
- token/model cost;
- CPU/IO-equivalent;
- memory footprint.

### Separacion entre agentes
Required boundaries:
- no cross-agent session/task mutation without privilege;
- no cross-agent memory access without policy;
- no tool access outside allowlist;
- no dual active lease on the same task.

### Politicas por sesion y por tarea
Policy layering:
- namespace default policy;
- agent default policy;
- session override within allowed maxima;
- task override within session maxima.

### MVP del fork
- SQL role authentication plus mapped agent identity.
- Agent ACLs, tool ACLs, namespace scoping, audit rows, budget ledger.

### Arquitectura objetivo
- deep executor/runtime propagation of `AgentPrincipal`.
- agent-aware stats, explain, and replication metadata.

### Deuda tecnica aceptada
- no wire-protocol authentication changes initially.
- leverage existing SQL roles rather than inventing a new auth stack immediately.

## 12. Compatibilidad y estrategia de adopcion

### Matriz de compatibilidad

| Area | Sigue funcionando | Cambia | Se rompe o limita | Se depreca |
|---|---|---|---|---|
| SQL core | `SELECT/INSERT/UPDATE/DELETE`, joins, indexes, views, short transactions | new agentic commands and catalogs | nothing by default if agent mode is unused | nothing in MVP |
| DDL clasico | tables, schemas, roles | adds `CREATE AGENT`, `CREATE TOOL`, `CREATE POLICY` | possible keyword collisions if grammar is careless | direct writes to internal agent relations |
| Wire protocol | libpq and common PG drivers | new command tags and SQLSTATEs | rigid tools that assume a fixed command-tag list may need updates | none initially |
| Extensions | many SQL-level extensions | deep internal extensions must recompile | extensions tightly bound to exact planner/executor internals break sooner | extensions that only emulate agent runtime externally |
| Physical replication | basebackup, streaming replication | fork-specific WAL/catalog semantics | stock PostgreSQL cannot be a replication peer for the fork | using stock PostgreSQL replicas for fork WAL |
| Logical replication | base facilities still useful initially | semantic event streams are added | decoders unaware of new payloads will miss semantics | DIY CDC over internal task tables |
| Security | roles, ACL base, schemas, RLS helpers | agent principals and namespace policy overlay | assuming `agent = SQL user` becomes invalid | modeling agents only as app rows |
| Observability | logs, `pg_stat_activity`, basic `EXPLAIN` | adds `SHOW TRACE`, `EXPLAIN AGENT`, `pg_stat_agent_*` | backend-only monitoring loses task context | ad hoc runtime logging outside engine |
| Backup/restore | `pg_dump`, `pg_basebackup` with care | must include agent catalogs and runtime consistency | restore to stock PostgreSQL is not supported | partial dumps that ignore runtime state |
| Operations | startup, config, recovery patterns | new GUCs, workers, queues, budgets | PG-only playbooks become incomplete | external orchestrator as sole source of task truth |

### Estrategia de adopcion
- keep wire compatibility and SQL surface compatibility as long as possible;
- ship the first release as a PostgreSQL-compatible superset, not as a general PostgreSQL replacement;
- target one or two canonical workloads first:
  - long-running agents with memory and checkpoint/resume;
  - tool pipelines with durable execution and audit.

### Rebase strategy y mantenibilidad contra upstream
Recommended branch model:
- `upstream/postgresql-17-stable`
- `qx/main`
- `qx/release/<version>`

Patch classification:
- `TEMP_PATCH`
- `PERM_SUBSYSTEM`
- `UPSTREAMABLE_CANDIDATE`

Rules:
- monthly intake of upstream minor fixes and CVEs;
- freeze upstream during high-risk storage/recovery milestones;
- maintain a patch ledger with:
  - touched files
  - reason
  - fork justification
  - expected rebase cost
  - replacement plan if temporary

### Versionado
Suggested scheme:
- `QhapaqXian DB <major>.<minor>.<patch> (based on PostgreSQL 17.x)`

Rules:
- major = incompatible catalog or recovery/wire behavior changes;
- minor = new agent primitives and compatibility expansions;
- patch = fixes and upstream backports.

### Compatibilidad operativa inicial
- keep `psql` working;
- allow side-by-side deployment with stock PostgreSQL;
- do not promise in-place upgrade from very early agent catalog versions;
- do not promise full extension compatibility.

### MVP del fork
- maximize SQL and driver compatibility.
- keep deeper divergence behind new commands and server-owned relations.

### Arquitectura objetivo
- accept selective incompatibility where runtime/recovery semantics require it.

### Deuda tecnica aceptada
- coexistence of two observability worlds for some time: SQL-centric and agent-centric.

## 13. Roadmap por etapas

### Etapa 0: tesis del fork
- Objetivo: freeze the architectural thesis and the extension-vs-fork boundary.
- Cambios tecnicos: ADR set, subsystem map, upstream baseline choice, patch ledger format.
- Riesgos: over-scoping deep core work too early.
- Pruebas: architectural review checklist, no code yet.
- Entregables: manifesto, subsystem map, maintenance strategy.
- Criterio de salida: approved thesis and explicit temporary vs permanent boundaries.

### Etapa 1: fork base y build
- Objetivo: import upstream, build, test, and brand the fork.
- Cambios tecnicos: upstream import, branch structure, CI, packaging, product naming, `CATVERSION`.
- Riesgos: breaking baseline compatibility too soon.
- Pruebas: compile matrix, smoke startup, baseline PG regression subset.
- Entregables: buildable repo, CI pipeline, internal alpha binary/container.
- Criterio de salida: reproducible build and stable SQL baseline.

### Etapa 2: lenguaje agentic
- Objetivo: stabilize the initial command surface before deeper implementation.
- Cambios tecnicos: syntax spec, SQLSTATE list, semantics matrix, keyword strategy.
- Riesgos: syntax that collides with SQL or blocks future planner work.
- Pruebas: parser golden cases, negative syntax cases.
- Entregables: language spec v0.
- Criterio de salida: command set frozen for vertical slice.

### Etapa 3: parser y analisis
- Objetivo: make agent statements parse and reach engine handlers.
- Cambios tecnicos: grammar, scanner, AST nodes, semantic validation, command tags, utility dispatch.
- Riesgos: grammar conflicts and poor parse/execute separation.
- Pruebas: `pg_regress` parser suite, conflict checks, compatibility tests with ordinary SQL.
- Entregables: agent statement parser/analyzer and execution stubs.
- Criterio de salida: `CREATE AGENT`, `START SESSION`, and `RUN TASK` parse and dispatch correctly.

### Etapa 4: catalogos de agentes
- Objetivo: persist metadata and runtime state under engine ownership.
- Cambios tecnicos: `pg_agent*` catalogs and internal relations, relcache/syscache hooks, admin views, ACL basics.
- Riesgos: rigid catalog shape or too much churn in true system catalogs.
- Pruebas: create/alter/drop, restart persistence, permission checks.
- Entregables: persistent metadata layer.
- Criterio de salida: metadata survives restart and is inspectable through system views.

### Etapa 5: vertical slice inicial
- Objetivo: end-to-end single-node workflow with create/start/run/remember/checkpoint/resume.
- Cambios tecnicos: simple dispatcher, internal event log, trace rows, checkpoint persistence, synchronous or minimally async step execution.
- Riesgos: too much ad hoc logic and non-idempotent resume.
- Pruebas: end-to-end demo, restart recovery, idempotent resume.
- Entregables: reproducible vertical slice.
- Criterio de salida: one node can run the complete demo flow reliably.

### Etapa 6: runtime y scheduler
- Objetivo: move from direct command execution to a real internal runtime.
- Cambios tecnicos: launcher, scheduler, dispatcher, executor workers, priorities, retries, heartbeats, cancellation, budgets.
- Riesgos: starvation, double execution, weak observability.
- Pruebas: concurrency, retry, cancel, saturation, fairness.
- Entregables: runtime v1 and runtime state views.
- Criterio de salida: concurrent tasks run and recover without an external orchestrator.

### Etapa 7: transacciones agentic
- Objetivo: formalize step transactions, checkpoints, compensation, and crash recovery.
- Cambios tecnicos: task state machine, attempt model, checkpoint barriers, compensation metadata, resume rules.
- Riesgos: confusing checkpoint with SQL commit, inconsistent external side effects.
- Pruebas: crash matrix, repeated resume, compensation tests, isolation behavior.
- Entregables: agent transaction model v1.
- Criterio de salida: crash recovery preserves or resumes internal task state correctly.

### Etapa 8: planner/executor agent-aware
- Objetivo: stop treating tasks as only queue metadata around SQL.
- Cambios tecnicos: `AgentPlan`, step types, cost model, branch/retry edges, `EXPLAIN AGENT`, executor integration.
- Riesgos: fake cost model, regression against ordinary SQL executor paths.
- Pruebas: explain snapshots, replanning tests, SQL regression benchmarks.
- Entregables: planner/executor v1 for agent tasks.
- Criterio de salida: the engine produces and exposes non-trivial agent plans.

### Etapa 9: WAL y replicacion semantica
- Objetivo: make agent state durable and semantically replicable beyond table CDC.
- Cambios tecnicos: canonical event stream, semantic logical decoder, failover reconstruction, optional dedicated WAL families for critical transitions.
- Riesgos: duplicate truth between tables and event stream, ambiguous replay.
- Pruebas: replica lag, promote/failover, state reconstruction, semantic CDC tests.
- Entregables: semantic event stream and failover-aware recovery.
- Criterio de salida: confirmed agent state survives failover with controlled resume behavior.

### Etapa 10: storage mas profundo si se justifica
- Objetivo: add new access methods or formats only when workload evidence justifies divergence.
- Cambios tecnicos: native ANN index, append-optimized trace/event store, checkpoint delta store if proven necessary.
- Riesgos: very high rebase cost and maintenance burden.
- Pruebas: comparative benchmarks, recovery correctness, compaction/maintenance tests.
- Entregables: RFC plus implementation only if threshold is met.
- Criterio de salida: measured production-like evidence shows standard storage is insufficient.

### Etapa 11: compatibilidad, endurecimiento y release inicial
- Objetivo: make the fork installable, operable, auditable, and supportable.
- Cambios tecnicos: hardening, docs, migration story, packaging, observability polish, security review, release process.
- Riesgos: trying to close too many features before operational maturity.
- Pruebas: soak, upgrade, backup/restore, security, performance baseline.
- Entregables: beta/RC/GA candidate and operator guide.
- Criterio de salida: an external team can install, run the vertical slice, and handle basic failures without core-team intervention.

## 14. Vertical slice

### Objetivo
Demonstrate that the fork can:
- accept agentic syntax;
- persist agent/session/task state;
- run multi-step work;
- write memory, events, and traces;
- checkpoint progress;
- recover from crash;
- resume from last committed checkpoint.

### Flujo minimo reproducible

1. Client connects through ordinary PostgreSQL protocol.

2. Client creates an agent:

```sql
CREATE AGENT archivist
  IDENTITY svc_archivist
  MODEL 'builtin://reasoner/default'
  MEMORY PROFILE default_mem
  TOOLS (sql_readonly, summary_tool)
  POLICY strict_default
  BUDGET TOKENS 50000 COST_LIMIT 5.00 TIME_LIMIT '10 min';
```

3. Client starts a session:

```sql
START SESSION FOR AGENT archivist
  WITH CONTEXT '{"case_id":"42","tenant":"qhapaq"}'
  RETURNING SESSION;
```

Expected durable effects:
- row in `pg_agent_session`;
- `SESSION_STARTED` event;
- trace root opened for the session.

4. Client launches a task:

```sql
RUN TASK summarize_case
  IN SESSION 1001
  GOAL 'read case data, summarize it, persist result'
  INPUT '{"case_id":"42"}'
  PRIORITY HIGH
  RETURNING TASK;
```

Expected durable effects:
- row in `pg_agent_task`;
- step graph rows in `pg_agent_step`;
- `TASK_ENQUEUED` event.

5. Runtime executes steps:
- Step 1: `AgentSqlStep`
  - read case data from application tables.
- Step 2: `REMEMBER`
  - persist working or episodic memory.
- Step 3: `AgentToolCallStep`
  - run summary tool.
- Step 4: `AgentSqlStep`
  - persist summary result into relational table.
- Step 5: `AgentCheckpointStep`
  - write checkpoint row and finalize task state.

6. Explicit memory write during the run:

```sql
REMEMBER
  IN SESSION 1001
  SCOPE EPISODIC
  KEY 'case.42.raw'
  VALUE '{"status":"loaded"}'
  TAGS ('case42','load');
```

7. Inspect trace while the task is active:

```sql
SHOW TRACE FOR TASK 7001 LIMIT 50;
```

8. Force a checkpoint after summary tool output:

```sql
CHECKPOINT TASK 7001 LABEL 'after_tool_output';
```

Expected durable effects:
- row in `pg_agent_checkpoint`;
- `TASK_CHECKPOINTED` event;
- trace span boundary persisted.

9. Simulate crash:
- run immediate server stop, equivalent to `pg_ctl stop -m immediate`.

10. Restart server.

11. Recovery scanner actions on restart:
- WAL replay restores internal rows;
- runtime finds `RUNNING` or `CHECKPOINTED` task state;
- stale lease is fenced;
- task becomes `READY_FOR_RESUME`.

12. Resume task:

```sql
RESUME TASK 7001 FROM CHECKPOINT 'after_tool_output';
```

13. Query final result and task state:

```sql
SHOW TRACE FOR TASK 7001 LIMIT 100;
FETCH MEMORY
  FOR AGENT archivist
  SCOPE EPISODIC
  MATCH 'case.42'
  LIMIT 10;
```

### Estado esperado del sistema durante la demo

| Phase | Durable state |
|---|---|
| After `CREATE AGENT` | `pg_agent`, `pg_agent_identity` references, optional policy/tool links |
| After `START SESSION` | `pg_agent_session`, event row, root trace |
| After `RUN TASK` | task, steps, initial attempt, event rows |
| After memory write | episodic/work memory rows |
| After checkpoint | checkpoint row, trace/event rows, budget ledger update |
| After crash/restart | durable rows restored by WAL replay |
| After resume | new attempt row, resumed step execution, final task outcome |

### Que prueba realmente este vertical slice
- native grammar exists;
- engine owns agent metadata and runtime state;
- task execution is durable across crash;
- checkpoint/resume semantics exist inside the server boundary;
- memory, events, and traces are not only application tables.

## 15. Mapa de implementacion concreto

### Que carpetas o modulos tocar primero
1. Import upstream and preserve structure.
2. Touch parser and command lifecycle:
   - `src/backend/parser/`
   - `src/backend/tcop/`
   - `src/include/nodes/`
3. Add catalogs and internal relations:
   - `src/include/catalog/`
   - `src/backend/catalog/`
   - `src/backend/utils/cache/`
4. Create new QhapaqXian subsystem roots:
   - `src/backend/qx/catalog/`
   - `src/backend/qx/runtime/`
   - `src/backend/qx/recovery/`
   - `src/backend/qx/memory/`
   - `src/include/qx/`
5. Add observability surfaces:
   - `src/backend/commands/explain.c`
   - `src/backend/utils/activity/`
6. Add planner/executor work only after the vertical slice is stable:
   - `src/backend/qx/planner/`
   - `src/backend/qx/executor/`
   - `src/backend/optimizer/`
   - `src/backend/executor/`

### Que tipos de archivo crear
- new catalog headers:
  - `pg_agent.h`
  - `pg_agent_identity.h`
  - `pg_agent_namespace.h`
  - `pg_agent_tool.h`
  - `pg_agent_policy.h`
- internal relation definitions and helpers
- AST headers:
  - `agentnodes.h`
- command handlers:
  - `agentcmds.c`
  - `sessioncmds.c`
  - `taskcmds.c`
  - `memorycmds.c`
  - `tracecmds.c`
- runtime modules:
  - `qx_launcher.c`
  - `qx_scheduler.c`
  - `qx_dispatcher.c`
  - `qx_executor_worker.c`
  - `qx_recovery_scanner.c`
  - `qx_budget_keeper.c`
- planner/executor modules later:
  - `agent_planner.c`
  - `agent_executor.c`
  - `agent_explain.c`
- documentation:
  - architecture docs
  - ADRs
  - operator guide

### Que pruebas automatizadas agregar
- `src/test/regress`
  - grammar and command behavior
  - catalog DDL and visibility
  - memory commands
  - trace commands
- `src/test/isolation`
  - concurrent resume
  - lease fencing
  - concurrent session/task mutation
- `src/test/recovery`
  - crash before checkpoint
  - crash after checkpoint
  - resume correctness
- TAP tests
  - runtime startup/shutdown
  - bgworker lifecycle
  - failover and semantic recovery
- performance tests
  - scheduler throughput
  - overhead when agent runtime is idle

### Que puntos dejar como parche temporal
- agent command execution through `ProcessUtility`;
- runtime over background workers;
- internal durable queue on heap relations;
- logical decoding plugin for semantic events;
- semantic memory on ordinary relations;
- metadata-driven `EXPLAIN AGENT` before deeper cost modeling.

### Que puntos convertir despues en core permanente
- agent grammar and AST;
- agent catalogs and internal relation ownership;
- task/session state machine;
- checkpoint/resume semantics;
- agent security context and policy enforcement;
- agent observability surfaces;
- `AgentPlan` and agent-aware execution;
- semantic replication path if recovery/CDC pressure justifies it.

### Orden de implementacion recomendado
1. Upstream import, build, CI, branding, patch ledger.
2. Grammar, AST, command tags, and utility dispatch.
3. Catalogs and internal relations.
4. Vertical slice command handlers and simple dispatcher.
5. Runtime workers and recovery scanner.
6. Checkpoint/resume semantics and crash tests.
7. Observability and trace views.
8. Planner/executor integration.
9. Semantic replication and deeper WAL work only if proven necessary.
10. Storage/index specialization only after measured pressure.

### Cierre ingenieril
QhapaqXian DB should not fork PostgreSQL everywhere on day 1. It should fork exactly where agent semantics require engine authority:
- syntax;
- metadata;
- runtime;
- checkpoints;
- recovery;
- identity;
- observability.

Everything else should remain as close to upstream as possible until measured workload evidence forces deeper divergence.
