# QhapaqXian DB Stage 2

Stage 2 freezes the public language surface of the first agentic dialect
before parser and catalog work begin.

Scope of this stage:
- language syntax v0
- command semantics v0
- SQLSTATE and error policy
- keyword strategy and parser-readiness notes
- command matrix and compatibility boundary
- Stage 3 handoff

Deliverables in this directory:
- `01-language-syntax.md`
- `02-command-semantics.md`
- `03-sqlstates-and-errors.md`
- `04-keywords-and-parser-readiness.md`
- `05-command-matrix-and-compatibility.md`
- `06-stage-3-handoff.md`

Stage boundary:
- this stage does not change PostgreSQL parser files yet;
- this stage defines the contract that Stage 3 and Stage 4 must implement.

Exit criteria:
- syntax and semantics are stable enough to drive parser work;
- keyword policy is explicit enough to avoid accidental grammar churn;
- public error contract is explicit enough to avoid ad hoc SQLSTATE sprawl;
- Stage 3 file map, stubs, and tests are concrete.
