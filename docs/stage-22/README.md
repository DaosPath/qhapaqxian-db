# Stage 22: Container and MicroVM Principal Classes

Stage 22 adds explicit principal runtime classes to the fork and binds them to
provider kinds that represent deeper execution planes than the Stage 20/21
single-host broker. The engine now distinguishes `host`, `container`, and
`microvm` principals in catalog state, DDL, authorization contracts, receipts,
and operator traces.

What landed:
- `CREATE/ALTER PRINCIPAL` now accepts `RUNTIME 'host' | 'container' | 'microvm'`;
- `pg_qx_principal` persists `qxprincipalruntimeclass`, so principal execution
  identity is no longer inferred only from provider kind;
- `CREATE/ALTER PROVIDER` now accepts `KIND 'container'` and
  `KIND 'microvm'` in addition to the existing `loopback` and `remote` kinds;
- provider endpoint validation is now kind-aware:
  - `local://` for `loopback`
  - `remote://` for `remote`
  - `container://` for `container`
  - `microvm://` for `microvm`
- provider/runtime compatibility is enforced before catalog writes and again
  during namespace/tool authorization:
  - `loopback` and `remote` principals must run as `host`
  - `container` principals must run as `container`
  - `microvm` principals must run as `microvm`
- non-`host` principal runtimes require `SANDBOX 'isolated'`;
- provider isolation contracts are stricter for `container` and `microvm`
  kinds: they require `USING 'ed25519'` and `ATTESTATION ENABLE`;
- authorized tool contracts now carry `principal_runtime=...` so planner,
  executor, runtime, and operator traces all see the same runtime identity;
- receipt requests, signed payloads, and runtime verification now include the
  expected `principal_runtime`, preventing a broker from replaying a valid
  receipt under the wrong runtime class;
- semantic traces now expose runtime-class evidence for submit/resume external
  tool calls.

Why this stage matters:
- it gives the engine a first-class notion of runtime class instead of treating
  all external principals as one generic brokered process;
- it makes the container/microVM boundary visible in catalog state, DDL,
  planner authorization, receipts, and traces;
- at landing time, it prepared the fork for real container or microVM launchers
  without forcing the backend to pretend it already owned those platform
  primitives.

What this stage did not pretend to solve at landing time:
- Stage 22 introduced runtime classes before backend launch ownership;
- `container://` and `microvm://` were initially brokered provider contracts
  backed by the shipped runner, not direct kernel or hypervisor integrations;
- OS-level isolation initially came from the existing launcher/sandbox layer,
  not from a new container runtime embedded in PostgreSQL.

Post-stage integration note:
- a later integration pass connected these runtime classes to real backend
  launch paths;
- `container://` now has a Docker-backed path;
- `microvm://` now has a QEMU `microvm` path with Windows `tcg` and WSL/Linux
  `kvm` validation;
- the historical Stage 22 caveat remains useful as a boundary marker: catalog
  runtime classes came first, and backend ownership/hardening is still evolving.
- operational setup and validation commands live in
  `../real-runtime-backends.md`.

Validation:
- `meson test -C build-stage4 --no-rebuild --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --no-rebuild --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --no-rebuild --suite postgresql:regress --print-errorlogs`
