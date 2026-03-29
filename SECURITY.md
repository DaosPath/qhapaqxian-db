# Security Policy

QhapaqXian DB is still in bootstrap status and should be treated as pre-release software.

## Reporting

Do not file unredacted security issues in public trackers.

Until a dedicated security mailbox exists, report security problems directly to the current repository owner and include:
- affected branch or commit;
- reproduction steps;
- whether the issue impacts upstream PostgreSQL behavior, fork-owned agentic subsystems, or both;
- whether secrets, agent isolation, runtime recovery, or replication semantics are involved.

## Scope notes

The most security-sensitive fork-specific areas today are:
- native agent/session/task catalogs;
- runtime resume and checkpoint handling;
- memory persistence and trace visibility;
- semantic logical messages and the `qhapaqxian_output` decoder;
- ownership and visibility checks around `pg_qx_*` data and `pg_stat_qx_*` views.

## Current limitations

This project does not yet claim:
- per-agent authentication realms;
- finalized multi-tenant isolation guarantees;
- hardened secret management for tool credentials;
- audited replication filtering for all semantic payloads.

Treat those areas as active engineering work, not solved guarantees.
