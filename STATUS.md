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
- `2026-03-30`

Last validated test sweep:
- `meson test -C build-stage4 --no-rebuild --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --no-rebuild --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --no-rebuild --suite postgresql:regress --print-errorlogs`
- result: `setup` OK, `qhapaqxian_output` OK (`1 subtests passed`), `regress` OK (`225 subtests passed`)

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
| 11 operability surface | yes | yes | yes | partial | system views exist, but no dedicated stats collector or richer operator dashboard |
| 12 security seed | yes | yes | yes | partial | owner filtering/revocation landed, but not full namespace isolation |
| 13 identity snapshots | yes | yes | yes | yes | identity is durable, but policy evaluation still snapshots rather than fully dynamic |
| 14 namespace policy/runtime metering | yes | yes | yes | yes | metering is engine-owned; not yet reconciled with external provider billing truth |
| 15 policy/tool DDL cleanup | yes | yes | yes | yes | tool model is real, but provider-backed execution remains brokered |
| 16 principal-backed execution | yes | yes | yes | yes | external tool execution is real, but still via shipped runner rather than dedicated remote plane |
| 17 stronger sandboxing | yes | yes | yes | yes | OS-level launch controls exist; no container or microVM isolation yet |
| 18 restricted-identity launch | yes | yes | yes | yes | restricted-identity evidence is strongest on Windows; cross-platform parity is weaker |
| 19 provider receipts | yes | yes | yes | yes | provider receipts exist, but provider trust is still local/brokered rather than independently attested |
| 20 brokered remote/HMAC | yes | yes | yes | yes | shared-key receipt model remains weaker than asymmetric or hardware-rooted attestation |
| 21 asymmetric receipts | yes | yes | yes | yes | signer material is still repo/bindir local; no certificate chain or hardware root |
| 22 container/microVM runtime classes | yes | yes | yes | yes | `container://` and `microvm://` are brokered runtime classes, not real backend-launched containers/microVMs |
| 23 attestation contracts | yes | yes | no | partial | attestation bundles are catalog-validated, but no runtime verifier or dedicated regression matrix exists yet |
| 24 container backend scaffold | yes | yes | no | partial | request/response contracts exist, but `qx_runtime.c` does not launch a real container backend yet |
| 25 microVM backend scaffold | yes | yes | no | partial | scaffold is broker-facing only; no real microVM launcher or provider integration yet |
| 26 scheduler scaffold | yes | yes | no | no | queue/lease/heartbeat shapes compile, but no runtime wiring, worker loop, or durable scheduler store exists yet |
| 27 recovery scanner scaffold | yes | partial | no | no | recovery scanner API and build wiring exist, but runtime/scheduler integration and test coverage are still pending |
| 28 observability scaffold | yes | yes | no | partial | helpers exist, but no callers or system views are wired yet |
| 29 capability-aware planner/executor | yes | partial | no | partial | structured capability decisions exist, but runtime handoff still uses existing contract strings |
| 30 security/tool capability contract | yes | yes | no | partial | capability tags and ceilings flow through authorization, but policy compilation and OS-level enforcement are still separate concerns |
| 31 semantic payload v2 | yes | yes | yes | yes | v2 payloads cover verified execution events, but the replication surface is still logical-message text rather than a deeper WAL family |
| 32 catalog snapshot helper layer | yes | yes | no | no | shared lookup layer exists, but callers are only partially migrated and helper coverage is still indirect |

Canonical next-gap summary:
- strongest remaining platform gap: real container or microVM backends behind Stage 22 provider/runtime classes;
- strongest observability gap: richer operator-facing status beyond the current system views and traces;
- strongest documentation gap: older stage docs still vary in depth and shape; use `docs/stage-template.md` and `docs/README.md` as the cleanup baseline.
