# QhapaqXian DB Stage 3

Stage 3 turns the Stage 2 language contract into a real core patch of the fork.

What is implemented in this stage:
- native parser tokens and keywords for the first agentic surface
- native AST nodes in `parsenodes.h`
- command tags for `CREATE AGENT`, `START SESSION`, and `RUN TASK`
- `ProcessUtility` dispatch into fork-owned command handlers
- regression coverage that fixes the parser/stub contract in `src/test/regress/`

What is deliberately not implemented in this stage:
- persistent agent, session, or task catalogs
- executable runtime behavior
- tuple-returning `RETURNING SESSION` / `RETURNING TASK`
- planner/executor changes beyond utility dispatch

Accepted syntax in this stage:
- `CREATE AGENT name [IDENTITY name] [MODEL '...'] [MEMORY PROFILE name] [TOOLS (...)] [POLICY name] [BUDGET (...)]`
- `START SESSION FOR AGENT name [WITH CONTEXT expr] [RETURNING SESSION]`
- `RUN TASK [name] IN SESSION expr GOAL '...' [INPUT expr] [PRIORITY name] [RETURNING TASK]`

Important limitation:
- clause order is fixed in this first parser pass to keep the fork diff small and maintainable against upstream PostgreSQL

Execution boundary:
- all three commands are parsed, typed, tagged, and routed by the server
- execution stops in explicit `ERRCODE_FEATURE_NOT_SUPPORTED` stubs
- this is intentional: Stage 3 proves the fork boundary in parser/utility code without pretending that catalogs or runtime already exist

Core files touched:
- `src/include/parser/kwlist.h`
- `src/backend/parser/gram.y`
- `src/include/nodes/parsenodes.h`
- `src/include/tcop/cmdtaglist.h`
- `src/backend/tcop/utility.c`
- `src/backend/commands/{agentcmds.c,sessioncmds.c,taskcmds.c}`
- `src/include/commands/{agentcmds.h,sessioncmds.h,taskcmds.h}`

Next stage handoff:
- Stage 4 must add first-class catalogs for agents, identities, sessions, and tasks
- only after that should `CREATE AGENT` stop failing and begin persisting metadata
