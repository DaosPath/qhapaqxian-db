# Stage 2 Command Semantics

Scope:
- This document defines the semantic contract for the initial agentic language of QhapaqXian DB.
- It is a product contract, not a parser implementation note.
- Stage 2 freezes meaning; Stage 3 will bind syntax and AST; Stage 4 will bind persistent catalogs and runtime state.

General rules:
- Every command must have a deterministic engine-visible effect.
- Every command must expose a clear valid-state precondition.
- Every command must specify whether it is idempotent, retryable, or resumable.
- Any behavior not listed here is deferred, not implied.

State model used in this stage:
- `AGENT_DEFINED`: agent metadata exists and is enabled for use.
- `AGENT_DISABLED`: agent metadata exists but cannot start new sessions.
- `SESSION_ACTIVE`: session exists and may accept tasks and memory writes.
- `SESSION_CLOSED`: session exists but cannot accept new work.
- `TASK_QUEUED`: task is accepted but not yet running.
- `TASK_RUNNING`: task execution is in progress.
- `TASK_CHECKPOINTED`: task has a durable resume point.
- `TASK_SUSPENDED`: task is paused intentionally and may be resumed.
- `TASK_COMPLETED`: task finished successfully.
- `TASK_FAILED`: task finished with error.
- `TASK_CANCELLED`: task was cancelled and will not resume automatically.

## `CREATE AGENT`

Purpose:
- define a durable agent principal with policy, identity, tool access, and budget defaults.

Preconditions:
- caller has agent administration privilege or equivalent ownership rights.
- referenced identity exists or is resolvable.
- referenced policy exists or is resolvable.
- referenced tools exist or are resolvable.
- agent name is unique within its namespace.

Effects:
- creates an agent definition.
- binds defaults for identity, policy, tool allowlist, memory profile, and budget envelope.
- registers the agent as `AGENT_DEFINED`.

Valid states:
- no prior agent with the same name in the same namespace.

Expected output:
- command completion on success.
- optionally `RETURNING` may expose the new agent identifier once Etapa 3 defines result shape.

Deferred to Etapa 3/4:
- parser shape and AST node design.
- system catalog layout for persistent storage.
- default catalog bootstrap rows.
- identity resolution mechanics.

## `ALTER AGENT`

Purpose:
- mutate a defined agent without replacing its identity.

Preconditions:
- agent exists.
- agent is not in a terminal state that forbids mutation by policy.
- caller has ownership or admin privilege over the agent.

Effects:
- updates mutable attributes such as policy binding, tool allowlist, memory profile, model binding, or budget defaults.
- preserves agent identity unless an explicit future command says otherwise.

Valid states:
- `AGENT_DEFINED`
- `AGENT_DISABLED`

Expected output:
- command completion on success.
- the altered agent remains addressable under the same name and identifier.

Deferred to Etapa 3/4:
- fine-grained alter subcommands.
- catalog invalidation rules.
- versioned schema migration of agent metadata.

## `START SESSION`

Purpose:
- open a durable execution session for one agent.

Preconditions:
- target agent exists.
- target agent is `AGENT_DEFINED`.
- caller is allowed to act for the agent under the active SQL role and namespace policy.
- session context, if provided, must be serializable by engine rules.

Effects:
- creates a session tied to a single agent.
- assigns a session identifier.
- snapshots initial session context and policy view.
- transitions to `SESSION_ACTIVE`.

Valid states:
- agent must be active and enabled.
- no active session lock may already exist for the same session identity.

Expected output:
- new session identifier.
- session metadata suitable for later task submission and trace lookup.

Deferred to Etapa 3/4:
- exact row shape for `RETURNING`.
- catalog-backed session lifecycle.
- background-worker attachment semantics.

## `RUN TASK`

Purpose:
- accept a task goal into the engine and make it runnable under a session.

Preconditions:
- session exists and is `SESSION_ACTIVE`.
- session belongs to the calling principal or to an authorized delegate.
- task goal is syntactically valid and semantically serializable.
- requested tool usage does not violate policy.
- requested budget does not exceed the session or agent ceiling.

Effects:
- creates a task.
- creates an initial attempt for the task.
- marks the task as `TASK_QUEUED`.
- records task goal, input payload, priority, and policy snapshot.

Valid states:
- session must be `SESSION_ACTIVE`.
- task name, if provided, must be unique within its scope or treated as an optional label only.

Expected output:
- task identifier.
- task status visible as queued.
- trace entry for task admission.

Deferred to Etapa 3/4:
- exact task graph shape.
- planner integration.
- worker dispatch.
- step decomposition.
- retry and replan machinery.

## `REMEMBER`

Purpose:
- write agent-scoped memory as a first-class engine action.

Preconditions:
- session exists and is `SESSION_ACTIVE`.
- caller has write permission for the selected memory scope.
- memory key is valid for the target scope.
- value is serializable under the chosen storage format.

Effects:
- appends or upserts memory under the selected scope.
- emits an auditable memory event.
- may update working memory, episodic memory, or semantic memory depending on scope.

Valid states:
- `SESSION_ACTIVE`
- optionally `TASK_RUNNING` if the task owns the session and policy permits writes during execution.

Expected output:
- acknowledgement of write success.
- a memory identifier or logical key reference once result shape is finalized.

Deferred to Etapa 3/4:
- physical placement across catalogs or relations.
- semantic indexing.
- retention policy.
- deduplication and compaction rules.

## `FETCH MEMORY`

Purpose:
- retrieve memory visible to an agent under policy and scope constraints.

Preconditions:
- target agent or session exists and is visible to the caller.
- requested scopes are allowed by policy.
- match expression is valid.

Effects:
- returns a ranked or filtered memory result set.
- does not mutate memory.
- may be semantically approximate when semantic memory is queried.

Valid states:
- `AGENT_DEFINED`
- `SESSION_ACTIVE`
- `TASK_RUNNING`

Expected output:
- zero or more memory records.
- stable projection rules for the returned fields.

Deferred to Etapa 3/4:
- ranking algorithm.
- ANN or vector index behavior.
- exact return schema.
- planner cost model for memory retrieval.

## `CHECKPOINT TASK`

Purpose:
- persist a resumable boundary for a running or suspended task.

Preconditions:
- task exists.
- task is `TASK_RUNNING` or `TASK_SUSPENDED`.
- the current execution state is checkpointable.
- the caller or runtime has permission to materialize the checkpoint.

Effects:
- writes a durable checkpoint.
- marks the task as `TASK_CHECKPOINTED`.
- records enough state to resume execution without replaying already committed step effects.

Valid states:
- `TASK_RUNNING`
- `TASK_SUSPENDED`

Expected output:
- checkpoint identifier.
- durable task state that can later be resumed.

Deferred to Etapa 3/4:
- checkpoint storage format.
- delta checkpointing.
- checkpoint compaction.
- runtime/worker protocol for autonomous checkpoint emission.

## `RESUME TASK`

Purpose:
- restart a task from a durable checkpoint.

Preconditions:
- task exists.
- a valid checkpoint exists for the task.
- task is `TASK_CHECKPOINTED`, `TASK_SUSPENDED`, or another recoverable state defined by policy.
- no conflicting live lease is held by another worker.

Effects:
- creates a new execution attempt or reactivates the next attempt under engine control.
- restores the execution context bound to the checkpoint.
- transitions the task to `TASK_RUNNING`.

Valid states:
- `TASK_CHECKPOINTED`
- `TASK_SUSPENDED`
- a recoverable crash-recovered state once Etapa 4 defines recovery behavior

Expected output:
- confirmation that the task has resumed or re-entered runnable state.
- resumed task identity and checkpoint reference.

Deferred to Etapa 3/4:
- lease fencing.
- attempt numbering.
- crash recovery reattachment.
- replan decisions after resume failure.

## `SHOW TRACE`

Purpose:
- inspect the trace history of a task, session, or agent according to visibility rules.

Preconditions:
- target object exists or is resolvable.
- caller is authorized to view the trace scope.
- trace retention has not expired for the requested window.

Effects:
- returns trace records.
- does not mutate task, session, or memory state.

Valid states:
- `AGENT_DEFINED`
- `SESSION_ACTIVE`
- `TASK_QUEUED`
- `TASK_RUNNING`
- `TASK_CHECKPOINTED`
- `TASK_SUSPENDED`
- `TASK_COMPLETED`
- `TASK_FAILED`

Expected output:
- ordered trace events or spans.
- optional filtering by limit, step, or status when later stages expose it.

Deferred to Etapa 3/4:
- stable trace schema.
- span identifiers.
- streaming trace transport.
- integration with `pg_stat_*` or explain surfaces.

## `EXPLAIN AGENT`

Purpose:
- explain the agentic meaning of a command before execution.

Preconditions:
- the wrapped statement is one of the agentic commands defined in this stage.
- the wrapped statement is syntactically valid enough to analyze.

Effects:
- produces an explanatory plan or semantic summary.
- does not execute the wrapped command.
- may reveal estimated state transitions, policy checks, and deferred execution steps.

Valid states:
- any state in which the underlying command would otherwise be parseable.

Expected output:
- semantic explanation of:
  - target agent/session/task
  - preconditions
  - state transitions
  - budget and policy checks
  - deferred engine work

Deferred to Etapa 3/4:
- `AgentPlan` structure.
- cost model.
- deep planner/executor integration.
- trace-linked explain output.

## Stage 2 boundary

In scope:
- command meaning
- command preconditions
- command effects
- valid state model
- output contract
- explicit deferrals to Etapa 3/4

Out of scope:
- parser grammar implementation
- AST node definitions
- catalog definitions
- scheduler implementation
- recovery implementation
- replication implementation

Exit criteria for Stage 2:
- every command above has a non-overlapping semantic contract.
- no command depends on undefined behavior for its primary effect.
- every deferred area is explicitly named and assigned to Etapa 3 or Etapa 4.
