# Release Bootstrap

This document marks the repository as publishable on GitHub, not as production-ready software.

## Baseline

- upstream imported from PostgreSQL `REL_17_STABLE`
- fork identity established as QhapaqXian DB / QhapaqXian Engine
- stages 2 through 11 implemented in-tree
- regression suite green on the current Windows bootstrap environment

## What "publishable" means here

- the repository can be cloned and built with the documented bootstrap toolchain;
- fork-specific engine work is documented;
- native agentic syntax, catalogs, runtime slices, memory storage, semantic logging, and operator views are present;
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
4. Add CI matrices beyond the bootstrap smoke coverage.
5. Decide when to rename binaries, if at all.
