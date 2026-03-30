# Stage 18: Restricted Identity Execution and Parent-Observed Launch Receipts

Stage 18 closes the remaining engine-controlled sandbox debt from Stage 17.

What landed:
- non-`builtin` principal execution on Windows now runs under a restricted
  token instead of inheriting the server process token;
- the parent launcher now records parent-observed launch evidence, including
  wall-clock execution time and whether restricted identity launch was applied;
- runtime traces now carry `launch_mode=profiled_process`,
  `restricted_identity=true`, and `wall_ms=...` in addition to the Stage 17
  sandbox-profile fields;
- regression coverage now asserts that external submit/resume traces expose
  launch-mode and restricted-identity evidence, not only child-reported
  sandbox metadata.

Why this stage matters:
- Stage 17 hardened the launcher, but the child still inherited the server
  identity on Windows;
- Stage 18 moves non-`builtin` principals under a less privileged token and
  makes launch evidence parent-observed, which removes the last engine-local
  ambiguity about whether the stronger launch path really happened.

What this stage does not pretend to solve:
- it is still not container or microVM isolation;
- provider-confirmed billing remains outside the fork because that requires an
  external runtime/provider boundary, not just core server code.

Debt status after this stage:
- engine-local launcher debt is closed;
- remaining work is product scope beyond the local fork: external runtime
  providers, remote attestation, or microVM/container execution.

Validation:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`

Result:
- `postgresql:setup` passed
- `postgresql:qhapaqxian_output` passed (`1 subtests passed`)
- `postgresql:regress` passed (`225 subtests passed`)
