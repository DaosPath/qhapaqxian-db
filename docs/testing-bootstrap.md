# Testing Bootstrap

The first testing layer for QhapaqXian DB should stay aligned with upstream PostgreSQL tooling.

Priority order:
1. source build smoke in CI
2. parser and command regression tests
3. catalog persistence tests
4. runtime lifecycle tests
5. crash and resume recovery tests

Planned test ownership:
- `src/test/regress/`
  - grammar and command behavior
  - metadata visibility
- `src/test/isolation/`
  - concurrent session/task mutations
  - lease fencing and dual resume protection
- `src/test/recovery/`
  - crash before checkpoint
  - crash after checkpoint
  - resume correctness
- TAP tests
  - runtime startup and shutdown
  - recovery scanner behavior
  - failover reconstruction once replication exists

Bootstrap rule:
- no deep fork patch should land without a matching automated test plan in one
  of the upstream PostgreSQL harnesses above.
- asymmetric remote-receipt coverage requires OpenSSL-enabled builds; Stage 21
  suites should run with `--with-openssl` or equivalent Meson `-Dssl=openssl`
  so `ed25519` provider verification is exercised rather than compiled out.
- brokered `container://` and `microvm://` provider coverage should stay in the
  same regression harnesses; Stage 22 does not claim kernel-level isolation, so
  tests must assert provider/runtime compatibility, receipt payload integrity,
  and trace-visible runtime class rather than fake container launches.
