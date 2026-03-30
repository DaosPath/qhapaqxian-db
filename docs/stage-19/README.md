# Stage 19: Native Providers and Verifiable Execution Receipts

Stage 19 crosses the boundary that Stage 18 intentionally left open: the fork
now has a native provider catalog and verifiable execution receipts in the
embedded runtime, not just principal metadata and local launcher evidence.

What landed:
- `CREATE/ALTER PROVIDER` is native DDL, backed by `pg_qx_provider`;
- principals now bind to providers through `qxprincipalproviderid` and
  `qxprincipalprovider`;
- authorized tool contracts now carry provider identity, provider kind,
  endpoint, attestation requirement, and receipt schema;
- the runtime request/response protocol now exchanges provider identity,
  receipt schema, receipt nonce, and attestation mode;
- the runtime verifies provider identity, receipt schema, deterministic receipt
  nonce, and required attestation before charging budget or recording task
  execution;
- operator-facing traces now expose `provider=...`,
  `receipt_schema=qx.receipt.v1`, `receipt_nonce=...`, and
  `attestation=loopback_verified` alongside the Stage 17/18 sandbox evidence.

Why this stage matters:
- Stage 16 through Stage 18 proved engine-owned principals and local launch
  isolation, but the execution contract still stopped at launcher metadata;
- Stage 19 turns providers into first-class engine objects and adds a receipt
  boundary the runtime can verify before it accepts external tool work as
  budget-consumable execution;
- this is still a bootstrap provider boundary, but it is now an explicit
  fork-owned subsystem instead of an undocumented future idea.

What this stage does not pretend to solve:
- provider kinds are still intentionally narrow: only the bootstrap
  `loopback` provider is accepted;
- attestation is still local-provider attestation, not remote cryptographic
  attestation from a distributed runtime;
- container or microVM execution is still a later step if the product chooses
  to leave the single-host runtime boundary.

Debt status after this stage:
- engine-local provider and receipt debt is closed for the loopback/local
  runtime model;
- remaining work is deeper product scope: remote providers, signed receipts,
  provider billing proofs, or stronger isolation domains such as containers or
  microVMs.

Validation:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`

Result:
- `postgresql:setup` passed
- `postgresql:qhapaqxian_output` passed (`1 subtests passed`)
- `postgresql:regress` passed (`225 subtests passed`)
