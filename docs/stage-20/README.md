# Stage 20: Brokered Remote Providers and Signed Receipts

Stage 20 closes the gap that Stage 19 left open: the fork no longer stops at
loopback-only provider metadata and unsigned receipt fields. QhapaqXian DB now
accepts brokered remote providers and verifies HMAC-signed execution receipts
before runtime traces or budget charging are accepted.

What landed:
- `CREATE/ALTER PROVIDER` now accepts a provider receipt key through
  `RECEIPT KEY '...'`;
- `pg_qx_provider` now persists provider receipt algorithm and receipt key in
  addition to provider kind and endpoint;
- provider kinds now accept both `loopback` and brokered `remote`, with
  matching endpoint validation for `local://...` and `remote://...`;
- authorized tool contracts now snapshot provider kind, endpoint, receipt
  schema, and receipt algorithm while keeping provider lookup stable through
  provider OID;
- the runtime now fetches the provider receipt key from `pg_qx_provider`,
  computes the expected HMAC-SHA256 receipt signature, and rejects external
  execution if the returned `RECEIPT_SIG` does not match;
- the shipped tool runner now signs receipts and emits provider-kind-specific
  attestation modes:
  `loopback_verified` for loopback and `remote_broker_verified` for remote;
- operator-facing traces now expose `provider_kind`, `receipt_alg`, and
  `receipt_sig=verified` without leaking the internal provider OID.

Why this stage matters:
- Stage 19 made providers first-class, but receipts still depended on unsigned
  response fields;
- Stage 20 turns the receipt into a cryptographically verifiable boundary,
  even though the remote provider is still brokered by a local runner process;
- this is the first stage where a remote provider kind is honest and testable
  inside the fork without pretending that the backend itself is a container or
  microVM orchestrator.

What this stage still does not pretend to solve:
- receipts are signed with a shared provider key (`hmac-sha256`), not an
  asymmetric provider certificate;
- `remote://` is still a brokered runtime model, not a direct distributed RPC
  transport from backend to provider;
- container and microVM execution remain a separate isolation boundary and are
  not simulated inside this stage.

Debt status after this stage:
- the fork-local debt around remote provider admission and signed receipts is
  closed for the brokered runtime model;
- remaining work is deeper than catalog/runtime glue: asymmetric receipts,
  containerized principals, or microVM-backed providers.

Validation:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`
