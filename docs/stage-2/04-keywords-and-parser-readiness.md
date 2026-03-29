# Stage 2 Keywords And Parser Readiness

Scope:
- This document defines keyword strategy and parser readiness for Etapa 3.
- It is not a parser implementation note.
- Etapa 2 freezes lexical intent, keyword classification, and parser boundaries.

Goal:
- minimize SQL grammar conflicts before touching `gram.y` and `scan.l`;
- define which words must exist in the language surface;
- define which words should stay reserved, unreserved, or deferred;
- make the Stage 3 parser work predictable and low-risk.

## 1. Keyword inventory

These words are required by the initial agentic surface:
- `AGENT`
- `SESSION`
- `TASK`
- `IDENTITY`
- `MODEL`
- `MEMORY`
- `PROFILE`
- `TOOL`
- `TOOLS`
- `POLICY`
- `BUDGET`
- `PRIORITY`
- `REMEMBER`
- `FETCH`
- `CHECKPOINT`
- `RESUME`
- `TRACE`
- `EXPLAIN`
- `START`
- `RUN`
- `SHOW`

These words are likely to appear in parser productions or command tags:
- `CREATE`
- `ALTER`
- `DROP`
- `FOR`
- `IN`
- `WITH`
- `GOAL`
- `INPUT`
- `RETURNING`
- `SCOPE`
- `KEY`
- `VALUE`
- `LABEL`
- `FROM`
- `MATCH`
- `LIMIT`
- `ADD`
- `SET`
- `DELETE`

## 2. Initial `kwlist.h` classification

Recommended initial classification strategy:

### Keep as `UNRESERVED_KEYWORD` for Stage 3 bootstrap
These terms should be available to the parser without immediately destabilizing ordinary SQL:
- `AGENT`
- `SESSION`
- `TASK`
- `IDENTITY`
- `MEMORY`
- `PROFILE`
- `POLICY`
- `BUDGET`
- `PRIORITY`
- `REMEMBER`
- `FETCH`
- `CHECKPOINT`
- `RESUME`
- `TRACE`
- `START`
- `RUN`
- `SHOW`

Reason:
- these words are specific to the new agentic command set;
- keeping them unreserved reduces the chance of early SQL breakage while the grammar is still being introduced;
- the parser can still claim them explicitly in dedicated productions.

### Keep as existing SQL keywords or command words
These should reuse existing PostgreSQL vocabulary where possible:
- `CREATE`
- `ALTER`
- `DROP`
- `FOR`
- `IN`
- `WITH`
- `FROM`
- `MATCH`
- `LIMIT`
- `ADD`
- `SET`
- `DELETE`

Reason:
- they already have clear SQL meaning;
- reusing existing tokens is safer than introducing new lexical categories;
- the agentic language should extend SQL, not fight it.

### Avoid introducing reserved status too early
Do not make the following reserved in Etapa 2 unless a concrete grammar conflict forces it:
- `AGENT`
- `SESSION`
- `TASK`
- `MEMORY`
- `POLICY`
- `BUDGET`
- `TRACE`

Reason:
- reserved status increases incompatibility with existing SQL text;
- the first parser pass should favor controlled ambiguity resolution inside dedicated productions rather than global keyword lock-in.

## 3. SQL conflict risks

The main conflicts are not semantic; they are lexical and syntactic.

### Risk 1: `TASK` or `SESSION` colliding with user column names
If these words become reserved too early, they can break ordinary SQL that uses them as identifiers.

Mitigation:
- keep them unreserved in `kwlist.h` initially;
- only claim them in dedicated agent command productions;
- do not alter unrelated SQL productions in Stage 2.

### Risk 2: `START SESSION` and `RUN TASK` ambiguity with existing SQL verbs
`START` and `RUN` are not part of standard PostgreSQL command syntax as top-level utility commands in the desired form.

Mitigation:
- bind them only in new utility-statement productions;
- keep the command surface narrow and explicit;
- do not overload generic expression parsing.

### Risk 3: `EXPLAIN AGENT` needing nested statement parsing
The `EXPLAIN` family already has special handling in PostgreSQL.

Mitigation:
- treat `EXPLAIN AGENT` as a dedicated wrapper around agentic utility statements;
- do not attempt to parse it as a generic SQL expression;
- keep the first implementation narrow enough that the explain path can remain a clean utility extension.

### Risk 4: `FETCH MEMORY` looking like a SQL `FETCH`
`FETCH` already exists in SQL and can imply cursor behavior.

Mitigation:
- treat `FETCH MEMORY` as a two-token agentic command that is only recognized in the agentic command path;
- do not reuse cursor semantics;
- keep the parser decision local to the new command family.

### Risk 5: `MEMORY` and `POLICY` being too broad
These terms can appear in many future agentic subcommands.

Mitigation:
- keep them unreserved first;
- introduce them only in the specific productions that need them;
- avoid broad grammar hooks until Stage 3 proves the command shape.

## 4. Parser readiness plan

Etapa 2 should prepare the parser, but not modify it yet.

### What Etapa 2 should define
- keyword list and intended scope;
- command list and terminal vocabulary;
- parser risk envelope;
- AST family boundaries;
- command tag inventory;
- error model for future parser work.

### What Etapa 2 should not do
- do not edit `gram.y`;
- do not edit `scan.l`;
- do not add AST nodes yet;
- do not add command tags yet;
- do not change `cmdtaglist.h` yet;
- do not introduce catalog DDL yet;
- do not wire utility handlers yet.

Reason:
- Stage 2 exists to freeze meaning and lexical intent;
- Stage 3 will bind syntax and AST;
- Stage 4 will bind persistent catalogs and runtime state.

## 5. Planned Stage 3 file touch list

These are the files that should be touched first when the parser work starts:
- `src/backend/parser/gram.y`
- `src/backend/parser/scan.l`
- `src/include/parser/kwlist.h`
- `src/include/nodes/parsenodes.h`
- `src/include/nodes/agentnodes.h`
- `src/backend/nodes/copyfuncs.c`
- `src/backend/nodes/equalfuncs.c`
- `src/backend/nodes/outfuncs.c`
- `src/backend/nodes/readfuncs.c`
- `src/backend/parser/analyze.c`
- `src/backend/tcop/utility.c`
- `src/include/tcop/cmdtaglist.h`

### Suggested order
1. add or confirm keyword tokens in `scan.l` and `kwlist.h`;
2. add AST node definitions;
3. add parser productions in `gram.y`;
4. add node serialization helpers;
5. add semantic analysis rules;
6. add utility dispatch and command tags.

### Why this order
- lexing first reduces grammar churn;
- AST first-class types make parser output stable;
- utility dispatch can be wired only after parse trees exist;
- analysis should follow syntax, not lead it.

## 6. AST readiness boundaries

The first AST family should be narrow and explicit.

Recommended top-level nodes:
- `CreateAgentStmt`
- `AlterAgentStmt`
- `StartSessionStmt`
- `RunTaskStmt`
- `RememberStmt`
- `FetchMemoryStmt`
- `CheckpointTaskStmt`
- `ResumeTaskStmt`
- `ShowTraceStmt`
- `ExplainAgentStmt`

Do not overgeneralize the AST in Stage 2.

Avoid:
- one giant `AgentStmt` node with too many subtypes;
- deeply nested variant trees before the command surface is stable;
- generic expression wrappers that hide command intent.

Reason:
- the engine needs clear command semantics later for catalogs, runtime, and explain;
- small dedicated nodes make parser code and tests easier to reason about.

## 7. `cmdtaglist.h` readiness

Command tags should exist conceptually in Stage 2 even though they are not implemented yet.

Recommended first tags:
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

Rules:
- do not add the tags before parser shape is fixed;
- do not add tags for future subcommands that are not part of the first slice;
- keep the first tag list aligned with the Stage 2 semantic contract.

## 8. Stage 2 exit criteria

Stage 2 is complete when all of these are true:
- keyword inventory is frozen;
- `kwlist.h` classification strategy is decided;
- conflict risks are documented;
- parser touch order is documented;
- AST node list is frozen for Stage 3;
- `cmdtaglist.h` target set is frozen;
- no parser source file has been modified yet.

## 9. Non-goals

This document does not:
- implement keyword changes;
- implement parser productions;
- implement AST nodes;
- implement catalog storage;
- implement runtime behavior;
- change upstream PostgreSQL SQL behavior.

That work belongs to Stage 3 and later.
