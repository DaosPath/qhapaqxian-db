QhapaqXian DB
=============

Repository entrypoint for the QhapaqXian DB fork.

Canonical docs, in order:
- [STATUS.md](STATUS.md) - current implementation state and stage matrix
- [docs/README.md](docs/README.md) - canonical documentation map
- [QHAPAQXIAN.md](QHAPAQXIAN.md) - fork boundary, naming, and governance
- [docs/qhapaqxian-architectural-blueprint.md](docs/qhapaqxian-architectural-blueprint.md) - target architecture and thesis
- [docs/stage-template.md](docs/stage-template.md) - canonical stage README shape
- [docs/stage-32/README.md](docs/stage-32/README.md) - latest landed stage-local contract

Stage docs worth checking early:
- [docs/stage-2/README.md](docs/stage-2/README.md) - language contract and parser-readiness boundary
- [docs/stage-4/README.md](docs/stage-4/README.md) - first-class engine catalogs for agent state

Short summary:
- upstream base imported from PostgreSQL `REL_17_STABLE`
- active fork branch is `bootstrap`
- product identity is QhapaqXian DB, not PostgreSQL as a product name
- staged implementation work is tracked in `STATUS.md`, not duplicated here
- the documentation tree is mapped in `docs/README.md`
- real container/microVM backend setup and validation live in
  `docs/real-runtime-backends.md`
- stage 2 language contract and stage 4 engine-catalog docs now exist to close the historical bootstrap-doc gap

What this repository is:
- a real fork target for an AgentDB
- not a PostgreSQL extension
- not a middleware-only orchestration layer
- not a SQL wrapper over application tables

What stays intentionally close to upstream in this bootstrap:
- server and client binary names
- build layout
- test harnesses
- core storage and replication behavior

For implementation details, use `STATUS.md` first. For fork naming and boundary questions, use `QHAPAQXIAN.md`.
