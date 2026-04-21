# Release Policy

This file owns versioning, compatibility, and release-shape policy only. Operational publishability lives in `docs/release-bootstrap.md`; test gating lives in `docs/testing-bootstrap.md`.

Initial release shape:
- `alpha`: upstream import + bootstrap docs + CI + subsystem scaffold
- `beta`: parser/catalog/runtime vertical slice
- `rc`: crash recovery, checkpoint/resume, observability hardening
- `1.0`: first operable AgentDB release, not a feature-complete final architecture

Versioning rule:
- fork version string should map explicitly to the upstream PostgreSQL major/minor base
- example format: `QhapaqXian DB 0.x (based on PostgreSQL 17.x)`

Compatibility rule:
- preserve PostgreSQL SQL and protocol compatibility where agent semantics are not involved
- document every intentional incompatibility
- release notes must state which real backend lanes were validated: Docker,
  QEMU/TCG, and QEMU/KVM when available
