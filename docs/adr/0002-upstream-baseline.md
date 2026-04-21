# ADR 0002: Upstream Baseline

Status: accepted

Note:
- This ADR records a historical bootstrap decision, not the current implementation status.
- Use `STATUS.md` for landed state and stage docs for implementation details.

Decision:
- The bootstrap fork starts from PostgreSQL `REL_17_STABLE`.

Why:
- mature ecosystem and source layout;
- lower operational risk for the first fork bootstrap than chasing a moving major transition;
- aligns with the architecture blueprint already committed in this repository.

Implications:
- versioning and release notes must state the upstream base explicitly;
- future major rebases are deliberate product decisions, not background churn.
