# Stage 23: Provider / Principal Attestation Contracts

Stage 23 hardens the attestation bundle between providers and principals so
the backend can reject incomplete contracts early.

## What landed

- `CREATE PROVIDER` and `ALTER PROVIDER` now carry attestation contract
  metadata through parser, AST, command validation, and catalog writes.
- `CREATE PRINCIPAL` and `ALTER PRINCIPAL` now carry matching attestation
  contract metadata and validate it against the selected provider.
- `pg_qx_provider` and `pg_qx_principal` now persist `attestation_profile`,
  `attestation_version`, and `attestation_policy` as first-class catalog
  fields.
- `catversion` was bumped so catalog drift is explicit and cannot be ignored
  by a bootstrap or upgrade path.
- Validation is intentionally strict:
  - attestation metadata must be provided as an atomic bundle
  - container and microVM provider kinds require attestation metadata
  - principal attestation must stay aligned with provider attestation when a
    provider exposes one

## What this stage did not claim at landing time

- Real container launch was outside this stage's landing scope.
- Real microVM launch was outside this stage's landing scope.
- Enclave attestation services and host trust chains were outside this stage's
  landing scope.
- WAL, recovery, replication, and scheduling semantics were unchanged here.
- Stage-specific regression schedules were outside this stage's landing scope.

## Post-stage integration note

- The attestation bundle is now consumed by the real Docker and QEMU backend
  integration path.
- Container and microVM provider receipts now carry runtime-class and
  attestation evidence through the backend response.
- The remaining gap is stronger provenance, such as certificate chains,
  hardware roots, or platform-specific attestation services, not absence of a
  real backend launch.

## Follow-on integration points

- Runtime backends consume the attestation bundle as an execution contract,
  not as free-form metadata.
- Provider drivers map `attestation_profile` and `attestation_version` to their
  concrete launch / attestation protocol.
- Receipts should eventually become provenance artifacts emitted by the
  runtime backend, not just validated input strings.
- Principals should inherit provider attestation defaults only when the
  provider contract is explicit and stable.
- Future work should add broader regression coverage for provider/principal
  contract permutations without changing the shared schedules.

## Compatibility notes

- The new columns are additive at the catalog level, but catalog versioning
  was still bumped because the tuple layout changed.
- Existing DDL remains valid when the new attestation fields are omitted for
  non-container and non-microVM providers.
- The current contract deliberately rejects partial attestation bundles to
  avoid ambiguous future backend behavior.

## Validation

- No dedicated stage-local regression harness was added at landing time.
- Current validation is indirect through the integrated `qx_stage3_agentic`
  regression and the real-backend smoke lanes documented in
  `../testing-bootstrap.md`.
