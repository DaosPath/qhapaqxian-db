# QhapaqXian Observability Layer

Planned responsibility:
- trace helpers
- task/session metrics
- budget visibility
- `EXPLAIN AGENT` support
- future `pg_stat_agent_*` surfaces

Bootstrap note:
- backend-centric PostgreSQL stats remain intact during bootstrap;
- agent-aware visibility is added incrementally and explicitly.
