# Testing Bootstrap

This file is the test runbook and harness map only. Implementation status lives in `STATUS.md`; release rules live in `docs/release-policy.md`.

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

Concrete coverage map:
- `.github/workflows/qhapaqxian-bootstrap.yml` owns the CI smoke lane. It builds
  the tree, prepares Alpine-based microVM assets, smoke-tests the real Docker
  runner, smoke-tests the real QEMU `microvm` runner, then runs the focused
  `qx_stage3_agentic` and `qhapaqxian_output` regressions.
- `src/test/regress/sql/qx_stage3_agentic.sql` owns the integrated engine lane.
  It creates loopback, container, and microVM providers; binds principals to
  `host`, `container`, and `microvm` runtime classes; runs submit/resume paths;
  verifies traces, receipts, attestation strings, runtime-class stats,
  autonomous scheduler renewal/reclaim evidence, `pg_stat_qx_scheduler_activity`
  rollups, checkpoint-backed task/attempt repair, startup/failover rebuild
  evidence, and security visibility.
- `contrib/qhapaqxian_output/sql/semantic_messages.sql` owns the semantic
  logical-decoder lane. It checks that emitted semantic messages remain ordered
  and decodeable after runtime/provider metadata is added.
- `docs/real-runtime-backends.md` owns the manual platform lane for Windows
  Docker/QEMU-TCG and WSL/Linux QEMU-KVM validation.

Real backend validation:
- `container://` coverage now has a real Docker path. Use
  `QX_CONTAINER_IMAGE=alpine:3.20` and set `QX_DOCKER_CLI` only when `docker`
  is not discoverable on `PATH`.
- Windows Docker coverage requires Docker Desktop to be running before the
  regression starts. `docker info` should succeed; otherwise the real runner
  fails instead of silently downgrading to a fake container backend.
- `microvm://` coverage now has a real QEMU `microvm` path. Use
  `QX_MICROVM_ACCEL=tcg` on Windows and hosted CI; use `QX_MICROVM_ACCEL=kvm`
  on Linux/WSL only when `/dev/kvm` exists.
- microVM assets should live under `$top_builddir/microvm-assets` unless the
  test explicitly sets `QX_MICROVM_KERNEL` and `QX_MICROVM_INITRD`.
- The default local initrd is `initramfs-qx-microvm.cpio.gz`; `microvm2`
  remains only a fallback for older build directories.
- focused local `qx_stage3_agentic` runs launched directly through `pg_regress`
  must export both `QX_MICROVM_KERNEL` and `QX_MICROVM_INITRD`, or pass request
  rows containing `MICROVM_KERNEL` and `MICROVM_INITRD`; without those assets,
  the non-simulated runner must fail the resume path instead of downgrading to
  synthetic microVM success.
- Windows correctness runs should validate Docker plus QEMU/TCG. WSL/Linux
  acceleration runs should validate QEMU/KVM and may call Docker Desktop through
  Windows `docker.exe` plus `DOCKER_HOST=npipe:////./pipe/dockerDesktopLinuxEngine`.
- Real microVM regressions expect the runtime's 120000 ms brokered timeout
  floor. The runner uses the same value for its QEMU process wait, avoiding an
  earlier child-side timeout while preserving the smaller host/container
  profile values.
- backend traces should assert normalized evidence for runtime class,
  attestation mode, backend launch mode, accelerator, and kernel path instead
  of comparing host-specific absolute paths.

Non-simulated pass criteria:
- container runner smoke must report `STATUS=ok`,
  `PRINCIPAL_RUNTIME=container`, `PROVIDER_KIND=container`,
  `ATTESTATION=container_receipt_verified`, and `backend_launch=docker`.
- microVM runner smoke must report `STATUS=ok`,
  `PRINCIPAL_RUNTIME=microvm`, `PROVIDER_KIND=microvm`,
  `ATTESTATION=microvm_receipt_verified`, and `backend_launch=qemu`.
- QEMU boot validation must expose the guest marker `QX-MICROVM-BOOT-OK`.
- focused regressions must assert runtime-class traces and verified receipt
  evidence instead of accepting synthetic provider success.
- focused recovery coverage must assert real scheduler ledger rows: recovery
  queue kind, reclaimed lease state, `scheduler-reclaim` receipt mode, and
  missed heartbeat state for stale running attempts. The same focused lane now
  also invokes failover rebuild through the runtime-owned hook and expects
  additional durable recovery snapshots, not simulated counters.
- focused scheduler-worker coverage must assert autonomous bgworker renewal
  before reclaim: a held lease row with `renewal_count=1`, a
  `scheduler-renew` heartbeat, and checkpoint-backed task/attempt repair once
  the reclaim lands.
- the same focused lane should also assert the non-checkpoint stale path: no
  new checkpoint row appears, the stale running attempt becomes `failed`, the
  task returns to `queued`, and exactly one durable `RETRY` queue row is
  emitted alongside the reclaimed lease evidence.
- after an additional wait, the same lane should assert the autonomous retry
  intake: attempt 2 reaches `checkpointed`, the task regains a durable
  checkpoint, the retry queue ledger count increases to 2, and the retry
  attempt emits `scheduler-release` evidence.
- the same focused lane should also assert retry fairness for the autonomous
  intake: when a stale `urgent` retry task and a stale `high` retry task become
  eligible together, both eventually reach retry attempt 2, but the earliest
  durable `runtime.retry_dispatch` trace must belong to the `urgent` task.
- the same focused lane should also assert multi-worker ownership and richer
  retry policy evidence: `pg_qx_scheduler_worker_slot_count()` should expose
  the fixed worker-slot count, `pg_stat_qx_scheduler_retry_backoff` should show
  the stale high-priority retry row with positive durable delay, and reclaim
  lease/heartbeat rows should carry the slotted scheduler worker name.
- the same focused lane should also assert the aggregated operator view:
  `pg_stat_qx_scheduler_activity` must expose the container queue/runtime/
  provider row with positive renew/reclaim/release/retry counts and renewed/
  reclaimed/released lease evidence after the autonomous flow completes.
- the same focused lane should also assert `pg_stat_qx_scheduler_worker_balance`
  so both worker slots stay visible even when only one currently owns the
  queue's live work.
- pg_regress temp clusters constrain autonomous scheduler database workers to
  the canonical `regression` database. This preserves real daemon behavior for
  QX coverage while avoiding persistent scheduler connections to transient
  upstream test databases created by core regression cases.
- heartbeat `needs_attention` is timing-derived, so focused regressions should
  normalize it against `stale` instead of expecting active lease rows to remain
  unexpired for the whole test wall clock.
- a real-backend lane should fail rather than silently downgrade to a fake
  container, fake microVM, or broker-only success path.

Focused local command shape:
- Run `meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs`
  first so the temp install and initdb template exist.
- The Meson `regress/regress --test-args qx_stage3_agentic` form appends the
  QX test after `parallel_schedule`; that runs the fixture twice and causes
  duplicate provider/principal/tool rows. Use direct `pg_regress` without
  `--schedule` when validating only `qx_stage3_agentic`.
- Direct `pg_regress` uses `build-stage4-codex/tmp_install/usr/local/pgsql/bin`;
  after relinking `build-stage4-codex/src/backend/postgres.exe`, sync the
  updated binary into `tmp_install` or rerun the install step before trusting
  the focused result.
- On Windows, direct focused `pg_regress` should point `INITDB_TEMPLATE` at
  `build-stage4-codex/tmp_install/initdb-template`; pointing it at the shared
  install tree alone leaves `pg_regress` with an invalid copied data
  directory.
- On Windows, direct focused `pg_regress` also needs `%SystemRoot%\\System32`
  preserved in `PATH`; otherwise the bootstrap copy step cannot find
  `robocopy` and fails before `postgresql.conf` is patched.
- On Windows, direct focused `pg_regress` also needs a real `diff.exe` in
  `PATH` (for example `C:\\Program Files\\Git\\usr\\bin`); otherwise the test
  body may run successfully but result comparison still aborts at the end.
- A direct focused run still requires real microVM assets. On Windows/TCG, set
  `QX_MICROVM_ACCEL=tcg`, `QX_MICROVM_KERNEL=<kernel>`, and
  `QX_MICROVM_INITRD=<initrd>`. On WSL/Linux/KVM, set `QX_MICROVM_ACCEL=kvm`
  only when `/dev/kvm` is present.
- The current focused regression also asserts recovery idempotence after the
  autonomous scheduler repair path: the repaired container attempt should still
  show exactly one `RECOVERY` queue row and one `RECLAIMED` lease row after
  explicit startup recovery and failover rebuild passes.
- The focused Windows run validated during Stage 32 used Docker Desktop
  29.3.1 and the prebuilt QEMU assets under
  `build-stage4-codex/microvm-assets/`.
- Stage 31 follow-up validation also ran the full `regress/regress` suite in
  the Windows temp cluster and passed all 225 subtests after the scheduler
  launcher stopped targeting transient core-test databases.

Validated real-backend sweep on 2026-04-20:
- Windows: `meson compile -C build-stage4-codex -j 2`
- Windows: `meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs`
- Windows: `meson test -C build-stage4-codex --suite postgresql:qhapaqxian_output --print-errorlogs`
- Windows: focused `qx_stage3_agentic` regression with Docker real, QEMU
  `microvm`/`tcg`, autonomous scheduler launcher/database bgworkers, durable
  lease renewal, checkpoint-backed reclaim repair, non-checkpoint fail-closed
  retry repair, autonomous retry intake back to checkpoint, retry-priority
  fairness, deterministic worker-slot ownership, retry-backoff visibility,
  worker-balance visibility, startup recovery, failover rebuild, and scheduler
  activity-view assertions
- WSL Ubuntu 24.04: direct QEMU/KVM boot marker `QX-MICROVM-BOOT-OK`
- WSL Ubuntu 24.04: runner smoke with `PRINCIPAL_RUNTIME=microvm`,
  `PROVIDER_KIND=microvm`, `ATTESTATION=microvm_receipt_verified`, and
  `microvm_accel=kvm`
- WSL Ubuntu 24.04: `meson compile -C build-kvm -j 4`
- WSL Ubuntu 24.04: `meson test -C build-kvm --suite postgresql:setup --print-errorlogs`
- WSL Ubuntu 24.04: `QX_MICROVM_ACCEL=kvm meson test -C build-kvm --suite postgresql:qhapaqxian_output --print-errorlogs`
- WSL Ubuntu 24.04: focused `qx_stage3_agentic` regression with microVM KVM and
  Docker real

See `docs/real-runtime-backends.md` for the full operational runbook.
