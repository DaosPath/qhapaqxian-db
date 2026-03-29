# ADR 0004: CATVERSION Policy During Bootstrap

Status: accepted

Decision:
- Do not change `src/include/catalog/catversion.h` during bootstrap-only branding,
  CI, docs, or scaffold work.

Why:
- the fork has not introduced catalog or cluster bootstrap divergence yet;
- a premature `CATVERSION` fork would create operational churn without technical need.

Policy:
- keep upstream `CATVERSION` until the first real change to fork-owned catalog
  definitions, bootstrap relations, or cluster initialization semantics;
- once such divergence exists, move to a fork-owned monotonic `CATVERSION`
  sequence and document every bump.
