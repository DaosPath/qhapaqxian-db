# Stage 21: Asymmetric Remote Receipts

Stage 21 replaces the brokered remote-provider shared-key boundary from Stage
20 with asymmetric receipt verification. The fork now accepts `ed25519`
provider receipts, binds remote principals to signer material, and verifies the
returned signature inside the embedded runtime before traces or budget charges
are accepted.

What landed:
- `CREATE/ALTER PROVIDER` now accepts `USING 'ed25519'` in addition to the
  existing `hmac-sha256` default;
- `pg_qx_provider` keeps the provider receipt algorithm while reusing
  `RECEIPT KEY` as the provider-side verification material; for `ed25519` that
  key is a PEM-encoded public key;
- `CREATE/ALTER PRINCIPAL` now accepts `SIGNER '...'` and persists
  `qxprincipalreceiptsigner` in `pg_qx_principal`;
- namespace/tool authorization rejects `ed25519` principals that do not bind a
  signer, so runtime never receives an incomplete contract;
- the runtime now parses `receipt_signer` from the authorized tool contract,
  resolves it under `bindir`, and verifies asymmetric receipt signatures with
  OpenSSL;
- the shipped tool runner now signs remote receipts with the configured
  principal signer when the provider receipt algorithm is `ed25519`;
- on Windows, the Meson packaging for `qhapaqxian-tool-runner` now stages the
  required OpenSSL runtime DLLs into `bindir` so minimal-environment execution
  still works after `PATH` is stripped by the sandbox launcher.
- `qx_remote_ed25519_private.pem` is a deterministic regression-only fixture;
  the test setup copies it into the temporary test `bindir`, while production
  `meson install` and `make install` exclude it.

Why this stage matters:
- Stage 20 proved the provider boundary with HMAC, but that still assumed a
  shared secret between provider and verifier;
- Stage 21 moves the brokered remote-provider model onto asymmetric receipts,
  which is a better fit for future provider isolation or delegated attestation;
- the fork remains honest about scope: this is still a brokered single-host
  runtime, not yet a container or microVM executor.

What this stage still does not pretend to solve:
- production signer provisioning remains external to this bootstrap; the
  regression fixture is not a hardware root of trust or deployable credential;
- the backend still brokers remote-provider execution through the shipped tool
  runner, not through a direct remote transport;
- containerized principals and microVM-backed providers remain the next deeper
  isolation boundary.

Validation:
- `meson test -C build-stage4 --no-rebuild --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --no-rebuild --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --no-rebuild --suite postgresql:regress --print-errorlogs`
