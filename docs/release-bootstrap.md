# Release Bootstrap

This document marks the repository as publishable on GitHub, not as production-ready software.

## Scope

- This is the operational publishability checklist.
- Implementation progress belongs in `STATUS.md`.
- Versioning and compatibility rules belong in `docs/release-policy.md`.
- Test ownership and suite order belong in `docs/testing-bootstrap.md`.

## What "publishable" means here

- the repository can be cloned and built with the documented bootstrap toolchain;
- fork-specific engine work is documented;
- native agentic syntax, catalogs, runtime slices, memory storage, semantic logging, and operator views are present;
- real Docker and QEMU backend smoke paths are documented and test-gated;
- a temporary Windows launcher `qhapaqxian-db.exe` can be produced next to the built `postgres.exe`;
- maintenance boundaries against upstream are recorded.

## What it does not mean

- no claim of production hardening;
- no claim of full packaging/release automation;
- no claim of final upgrade path or on-disk compatibility policy across future fork releases.

## Next release-engineering steps

1. Choose a public default branch strategy and branch protection rules.
2. Publish issue labels and milestone conventions.
3. Define versioning for QhapaqXian DB releases relative to upstream PostgreSQL baselines.
4. Add CI matrices beyond the bootstrap smoke coverage, especially a
   self-hosted Linux KVM lane for accelerated microVM validation.
5. Decide when to rename binaries, if at all.
