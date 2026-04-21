# QhapaqXian DB Bootstrap Plan

This file is the forward-looking plan only. Canonical implementation status lives in `STATUS.md`; do not mirror detailed progress here.

This repository was empty before the upstream import. The bootstrap plan is:

1. Import upstream PostgreSQL `REL_17_STABLE`.
2. Preserve fork identity in repository-level docs before deep core edits.
3. Add CI that proves the imported tree still configures and builds.
4. Create subsystem roots for QhapaqXian Engine under `src/backend/qx` and `src/include/qx`.
5. Add ADRs and a patch ledger before invasive parser/catalog changes.
6. Freeze Stage 2 language artifacts before changing parser or catalogs.
7. Implement parser/catalog/runtime work only after the fork has a stable maintenance boundary.

Current horizon:
- keep `STATUS.md` as the implementation ledger;
- converge the scheduler, recovery, observe, and catalog helper scaffolds into the active runtime path;
- add dedicated regression coverage for the remaining scaffolds;
- harden the real Docker and QEMU backend paths behind the existing container
  and microVM provider classes, including OCI policy, VM lifecycle supervision,
  and self-hosted KVM CI coverage.

Immediate non-goals:
- renaming all PostgreSQL binaries
- large-scale doc rebrand
- speculative storage or WAL divergence
- pretending Docker/QEMU launch alone is a complete isolation product without
  explicit policy, lifecycle, and attestation hardening
