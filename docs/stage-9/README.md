# QhapaqXian DB Stage 9

Stage 9 opens the semantic WAL boundary for QhapaqXian DB without pretending that the fork already owns a dedicated WAL resource manager or a custom replication protocol.

Implemented in this stage:
- new semantic logging subsystem under `src/backend/qx/replication`
- transactional logical WAL messages emitted with PostgreSQL's native `LogLogicalMessage()`
- `pg_qx_event`, `pg_qx_trace`, and `pg_qx_checkpoint` now persist the semantic message `LSN`
- session lifecycle, runtime events, traces, and checkpoints now emit semantic messages in the same transaction that persists their catalog rows
- regression coverage now verifies that semantic `LSN`s are present and monotonic across event, trace, and checkpoint histories
- fork-owned logical output plugin under `contrib/qhapaqxian_output`
- logical decoding regression coverage now validates end-to-end slot consumption of semantic JSON messages under `wal_level=logical`

What is still intentionally deferred:
- slot management, downstream semantic subscriptions, and semantic failover workers
- a new WAL resource manager for agent state transitions
- semantic replication of agent DDL beyond the current runtime/session slice
- promotion-time recovery scanners that rebuild runnable queues from the semantic stream alone

Why this matters:
- QhapaqXian DB now has a real semantic replication hook at the engine boundary, not just row-level CDC over internal tables
- durable catalog rows and semantic WAL messages are now correlated by persisted `LSN`
- fork-owned logical decoders can consume task/session/checkpoint semantics without reverse-engineering heap tuples

Accepted debt in this stage:
- the fork still relies on ordinary logged relations as canonical state
- semantic payloads are JSON strings over the generic logical-message rmgr, not a fork-owned WAL family
- the current output plugin streams raw semantic JSON payloads and does not yet expose a richer framed protocol or downstream subscription control surface

Tests run:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`
