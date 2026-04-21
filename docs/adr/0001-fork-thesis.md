# ADR 0001: QhapaqXian DB Exists As A Fork

Status: accepted

Note:
- This ADR records a historical architecture decision, not the current implementation status.
- Use `STATUS.md` for landed state and stage docs for implementation details.

Decision:
- QhapaqXian DB will be built as a fork of PostgreSQL, not as an extension-only or middleware-only system.

Why:
- agent identity, sessions, tasks, checkpoints, runtime control, and observability must become engine primitives;
- extension and middleware approaches are acceptable as temporary scaffolding, not as the final architecture.

Implications:
- parser, catalogs, runtime, security context, and recovery surfaces are in scope for future fork work;
- maintenance against upstream PostgreSQL is a permanent engineering concern.
