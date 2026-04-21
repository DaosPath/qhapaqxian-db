# ADR 0003: Branding Boundary In Bootstrap

Status: accepted

Note:
- This ADR records a historical bootstrap decision, not the current implementation status.
- Use `STATUS.md` for landed state and stage docs for implementation details.

Decision:
- The bootstrap keeps upstream PostgreSQL binary names and operational layout.
- The repository and product identity are branded as QhapaqXian DB.

Why:
- early binary renames would increase packaging, docs, and ecosystem breakage before the fork proves its core semantics;
- repository-level identity is enough for the first maintenance-safe bootstrap.

Implications:
- product docs should say QhapaqXian DB;
- technical attribution to PostgreSQL remains explicit;
- binary renames are deferred to a later packaging milestone.
