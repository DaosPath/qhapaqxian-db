# QhapaqXian DB Status

This file is the canonical implementation status for the repository.

Rules:
- use this file as the source of truth for "what is actually landed";
- `README.md` should stay as repository entrypoint, not the detailed status ledger;
- `QHAPAQXIAN.md` should stay as fork-boundary/governance context;
- `docs/qhapaqxian-architectural-blueprint.md` remains the architecture seed and target design, not the current-state tracker.

Documentation hierarchy:
- `README.md` = entrypoint and quick orientation
- `STATUS.md` = canonical current-state ledger
- `docs/README.md` = canonical documentation map
- `QHAPAQXIAN.md` = naming, fork boundary, and governance
- `docs/qhapaqxian-architectural-blueprint.md` = target architecture thesis
- `docs/stage-*/README.md` = stage-local landed contracts and notes

Snapshot date:
- `2026-06-12`

Last validated test sweep:
- Windows: `meson compile -C build-stage4-codex -j 2`
- Windows: `meson test -C build-stage4-codex --suite postgresql:setup --print-errorlogs`
- Windows: `meson test -C build-stage4-codex --suite postgresql:qhapaqxian_output --print-errorlogs`
- Windows: focused `qx_stage3_agentic` regression with Docker real, QEMU
  `microvm`/`tcg`, autonomous scheduler launcher/database bgworkers,
  durable lease renewal, checkpoint-backed reclaim repair, idempotent
  startup/failover recovery, failover rebuild coverage, retry-priority
  fairness assertions (`urgent` retry dispatch before `high` on the durable
  trace order), deterministic multi-worker slot ownership, configurable
  bounded scheduler slot count with default 2, priority-aware retry backoff,
  retry dead-letter at max retries, worker-balance visibility, and
  `pg_stat_qx_scheduler_activity` observability assertions, plus Stage 23
  attestation contract permutation checks and Stage 30 namespace policy
  `USING` require/deny capability checks at `CREATE AGENT`
- WSL Ubuntu 24.04: direct QEMU/KVM boot marker `QX-MICROVM-BOOT-OK`
- WSL Ubuntu 24.04: runner smoke with `microvm_accel=kvm`
- WSL Ubuntu 24.04: `meson compile -C build-kvm -j 4`
- WSL Ubuntu 24.04: `meson test -C build-kvm --suite postgresql:setup --print-errorlogs`
- WSL Ubuntu 24.04: `QX_MICROVM_ACCEL=kvm meson test -C build-kvm --suite postgresql:qhapaqxian_output --print-errorlogs`
- WSL Ubuntu 24.04: focused `qx_stage3_agentic` regression with microVM KVM and Docker real
- Windows sweep on 2026-06-12 (Stage 35 follow-up): Docker Desktop + QEMU/TCG focused
  `qx_stage3_agentic` passed after fixing `SHOW TRACE` normalization
  (`qx_replace_trace_delimited_value_all` suffix duplication) and refreshing
  `expected/qx_stage3_agentic.out`; full `regress/regress` passed 226 subtests after
  refreshing `expected/rules.out` for stats-backed principal/provider/runtime views;
  `postgresql:qhapaqxian_output` passed on re-run (2026-06-12 evening sweep)
- result: Windows Stage 35 follow-up complete on this machine: `qx_stage3_agentic`,
  full `regress/regress` (226), and `qhapaqxian_output` all OK with Docker real and
  QEMU/TCG microVM; WSL setup/output OK from prior sweep; real-backend lanes remain
  fail-closed when Docker Desktop or microVM assets are unavailable
- Windows sweep on 2026-06-12 (Stage 35 close-out after Stage 32 catalog migration):
  `postgresql:qhapaqxian_output` OK; `regress/regress` passed 227/227 with Docker/TCG
  env including new `vm_id` trace assertions on microVM resume in `qx_stage3_agentic`
- result: Stage 35 close-out complete on this machine; catalog insert migration absorbed
  from Stage 32; OCI/VM lifecycle ownership remains future platform work
- Windows sweep on 2026-06-12 (Stages 26–27): collector-backed scheduler/recovery stats
  plane, `pg_qx_recovery_scan()`, `qx_stage26_scheduler`, and `qx_stage27_recovery`
  landed; load-adaptive slot scaling and semantic replay remain intentional deferrals

Status legend:
- `yes` = present and wired
- `partial` = present but incomplete, missing dedicated doc, or still transitional
- `spec` = documented contract only
- `no` = not present

| Stage | Docs | Code | Tests | Demo | Known gaps |
|---|---|---|---|---|---|
| 0 fork thesis | yes | no | no | no | thesis lives mainly in blueprint/ADRs; no dedicated executable artifact |
| 1 upstream fork/build | yes | yes | yes | no | bootstrap packaging still thin; no release-grade distribution matrix |
| 2 language contract | yes | spec | no | no | spec frozen, but stage exists as contract rather than executable feature |
| 3 parser/AST/utility | yes | yes | yes | partial | agent surface still enters through utility path, not deep core execution |
| 4 engine catalogs | yes | yes | yes | partial | durable agent/session/task catalogs are in-tree, and the stage now has a dedicated doc; the remaining gap is a standalone runtime demo |
| 5 vertical slice | yes | yes | yes | yes | still single-node and synthetic in shape compared to final runtime model |
| 6 embedded runtime boundary | yes | yes | yes | yes | runtime is embedded, but not yet a full autonomous scheduler plane |
| 7 checkpoints/resume | yes | yes | yes | yes | semantics are durable, but compensation/long-horizon recovery are still shallow |
| 8 planner/executor boundary | yes | yes | yes | yes | `AgentPlan` is real, but still narrower than a full agent-aware optimizer |
| 9 semantic WAL/decoder | yes | yes | yes | yes | logical-message boundary exists; no dedicated WAL record family or downstream subscription control |
| 10 memory/trace surface | yes | yes | yes | yes | memory still lives on ordinary catalogs/storage; no deeper AM/vector specialization |
| 11 operability surface | yes | yes | yes | partial | system views now include scheduler queue/worker/activity rollups plus retry-backoff and worker-balance visibility, but there is still no dedicated stats collector or broader operator dashboard |
| 12 security seed | yes | yes | yes | partial | owner filtering/revocation landed, but not full namespace isolation |
| 13 identity snapshots | yes | yes | yes | yes | identity is durable, but policy evaluation still snapshots rather than fully dynamic |
| 14 namespace policy/runtime metering | yes | yes | yes | yes | metering is engine-owned; not yet reconciled with external provider billing truth |
| 15 policy/tool DDL cleanup | yes | yes | yes | yes | tool model is real; later stages supersede the original broker-only execution boundary |
| 16 principal-backed execution | yes | yes | yes | yes | external tool execution is real, but still via shipped runner rather than dedicated remote plane |
| 17 stronger sandboxing | yes | yes | yes | yes | OS-level launch controls exist; later stages add container/microVM runtime classes and real Docker/QEMU launch paths |
| 18 restricted-identity launch | yes | yes | yes | yes | restricted-identity evidence is strongest on Windows; cross-platform parity is weaker |
| 19 provider receipts | yes | yes | yes | yes | provider receipts exist, but provider trust is still local/brokered rather than independently attested |
| 20 brokered remote/HMAC | yes | yes | yes | yes | shared-key receipt model remains weaker than asymmetric or hardware-rooted attestation |
| 21 asymmetric receipts | yes | yes | yes | yes | signer material is still repo/bindir local; no certificate chain or hardware root |
| 22 container/microVM runtime classes | yes | yes | yes | yes | runtime classes now have real Docker and QEMU launch paths; deeper OCI policy and VM lifecycle ownership remain future work |
| 23 attestation contracts | yes | yes | yes | partial | attestation bundles are consumed by real backend paths and regression now covers partial/missing/mismatched/inherited contract permutations; stronger provenance roots remain future work |
| 24 container backend scaffold | yes | yes | yes | yes | OCI policy compilation from capability tags (`readonly_rootfs`, `seccomp_mode`, `oci_profile`), image allowlist enforcement, runner `docker run` honors compiled policy, and `qx_stage24_container_policy` regress; full rootless OCI and cgroup namespace ownership remain intentional deferrals |
| 25 microVM backend scaffold | yes | yes | yes | yes | MicroVM asset policy resolves kernel/initrd/snapshot refs with allowlist validation, launch payloads emit `kernel_ref`/`initrd_ref`, `qx_stage25_microvm_policy` regress, and self-hosted KVM CI; hypervisor process ownership and snapshot lifecycle remain intentional deferrals |
| 26 scheduler scaffold | yes | yes | yes | yes | durable queue/lease/heartbeat catalogs, autonomous launcher/database bgworkers, configurable bounded per-database slot ownership with default 2, lease renewal/reclaim/retry/dead-letter supervision, ledger-backed queue/worker/activity/backoff/balance views, dedicated collector-backed scheduler stats via `pg_qx_stat_get_scheduler_activity_stats()` and `pg_stat_qx_scheduler_collected`, and `qx_stage26_scheduler` regress; load-adaptive slot scaling remains an intentional deferral |
| 27 recovery scanner scaffold | yes | yes | yes | yes | startup recovery, failover rebuild, and autonomous scheduler supervision drive real scheduler requeue/reclaim writes with ledger-local dedupe, read-only `pg_qx_recovery_scan()` exposes suppressed requeue/fence counts, collector-backed recovery stats via `pg_qx_stat_get_recovery_stats()` and `pg_stat_qx_recovery`, and `qx_stage27_recovery` regress; deeper semantic replay and replication-fed supervision remain intentional deferrals |
| 28 observability scaffold | yes | yes | partial | partial | `qx_observe` backs `SHOW TRACE` summaries and scheduler activity views; Stage 33–34 add shared-memory `qx_stat`, provider/principal/runtime-class SRFs, collector-backed `pg_stat_qx_*` views, and `qx_stage3_observability`; historical/SLO accounting remains open |
| 29 capability-aware planner/executor | yes | yes | yes | yes | planner/executor now materialize explicit submit/resume capability routes, persist them in `pg_qx_task`, and runtime/retry/recovery consume those routes instead of first/last contract heuristics |
| 30 security/tool capability contract | yes | yes | yes | yes | namespace policy `USING` contracts now enforce required/denied capability tags during tool authorization and `CREATE AGENT`; OS-level OCI/VM policy compilation and stronger provenance remain future hardening |
| 31 semantic payload v2 | yes | yes | yes | yes | v2 payloads cover verified execution events, but the replication surface is still logical-message text rather than a deeper WAL family |
| 32 catalog snapshot helper layer | yes | yes | yes | partial | recovery, planner, security, commands, runtime scheduler reads/writes, DDL tuple inserts for identity/agent/session/memory/policy objects, provider/principal/runtime-class stats SRFs, and `qx_stage32_catalog` regression now use `qx_catalog` helpers; ALTER-command catalog updates remain in command layers |
| 33 stats collector and runtime hardening | yes | yes | partial | partial | `qx_stat` shared-memory collector, `qhapaqxian.track_stats`, catalog scheduler helpers, runtime policy/supervisor, typed launch requests, KVM CI workflow, and `qx_stage3_observability` landed; historical stats and full OCI/VM policy compilation remain open |
| 34 observability collector v2 | yes | yes | partial | partial | `pg_qx_stat_get_principal_stats`, `pg_qx_stat_get_runtime_class_stats`, collector-backed `pg_stat_qx_principals`/`pg_stat_qx_runtime_classes`, and extended `qx_stage3_observability` landed; historical/SLO accounting and operator dashboards remain open |
| 35 CI loopback path, backend IDs, catalog runtime writes | yes | yes | yes | partial | `qhapaqxian_output` loopback resume path, real `container_id`/`vm_id` trace evidence (runner top-level IDs + trace assertions), meson microVM/container env wiring, `qx_catalog` runtime update/insert helpers (Stage 32 follow-up), duplicate receipt-tail stripping in runtime/`SHOW TRACE`, and post-Stage-32 validation sweep landed; full OCI cgroup/seccomp and cross-process VM lifecycle ownership remain future work |

Canonical next-gap summary:
- strongest remaining platform gap: deepen OCI cgroup/seccomp policy compilation and cross-process VM lifecycle ownership beyond the new supervisor registry;
- strongest observability gap: add historical/SLO accounting and operator dashboards on top of the Stage 34 collector-backed principal/runtime-class surfaces;
- strongest documentation gap: older stage docs still vary in depth and shape; use `docs/stage-template.md` and `docs/README.md` as the cleanup baseline.
