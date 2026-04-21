# Stage 31 - Semantic Payload v2

## Stage intent

Stage 31 evolves the semantic replication surface from a Stage 9 lifecycle
payload into a schema-aware payload that can carry verified execution
observations without rewriting the decoder model.

## What landed

- `stage9.semantic.v1` remains the baseline payload for lifecycle messages that do not carry execution-specific metadata.
- `stage31.semantic.v2` is emitted when the runtime has verified external execution metadata.
- v2 carries the fields needed to describe provider kind, principal runtime, sandbox identity, launch mode, receipt schema, attestation mode, and budget / wall-time observations.
- The payload is still logical-message text, not a reinterpretation layer and not a synthetic protobuf.

## What is intentionally deferred

- This stage does not introduce a new binary replication format.
- This stage does not add remote attestation transport or a container /
  microVM supervisor inside the database backend.
- This stage does not backfill old records with synthetic execution metadata.

## Why this stage matters

- it lets downstream consumers distinguish lifecycle-only events from
  execution-verified events without schema guessing;
- it keeps the decoder honest by forwarding the original payload instead of
  inventing normalized fields;
- it creates a stable replication boundary for later container/microVM
  hardening, provider supervision, and attestation work.

## Compatibility assumptions

- Consumers that only understand v1 can continue to read v1 records unchanged.
- Consumers that understand v2 must treat v1 and v2 as siblings, not as a replacement hierarchy with hidden defaults.
- Fields that are not present in v1 are intentionally absent; the decoder must not infer them.
- Execution metadata is attached only when the runtime has observed it directly.

## Decoder Boundary

- `contrib/qhapaqxian_output` is a passthrough logical decoder.
- It validates that each message belongs to an accepted semantic schema.
- It does not rewrite payloads, normalize fields, or invent missing metadata.
- It does not claim that v1 messages contain provider / principal / sandbox observations they never carried.

## Current coverage

- Queue, dispatch, authorization, session lifecycle, and other non-external messages may remain on v1.
- External execution submit / resume, execution checkpoints, and completion records move to v2 when runtime observations are available.
- The output plugin accepts both schemas and forwards the original JSON text unchanged.

## Validation

- No separate stage-local harness is defined for this payload layer.
- Validation is expected to ride the repository's normal logical-decoding and
  PostgreSQL regression coverage when v2 events are present.

## Next stage handoff

- Stage 32 can treat semantic payload consumers as stable enough to start
  consolidating catalog snapshots behind shared helpers.
- Later replication work can deepen the transport boundary without having to
  redesign the v1 versus v2 meaning split first.
