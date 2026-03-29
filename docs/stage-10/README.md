# QhapaqXian DB Stage 10

Stage 10 introduces the first storage specialization that is justified without inventing a new table access method: engine-owned agent memory persisted in native catalogs plus operator-facing retrieval and trace inspection commands.

Implemented in this stage:
- new native system catalog `pg_qx_memory`
- engine-owned memory storage subsystem under `src/backend/qx/memory`
- native utility commands:
  - `REMEMBER`
  - `FETCH MEMORY`
  - `SHOW TRACE`
- utility execution plumbing for tuple-returning memory and trace inspection
- privilege checks for memory persistence and retrieval against session/agent ownership
- `pg_qx_memory` catalog foreign-key coverage in `oidjoins`
- regression coverage for memory persistence, memory retrieval, trace inspection, and literal-identifier error paths

What is intentionally simple in this stage:
- memory still lives on ordinary heap storage and btree indexes
- `REMEMBER` is scoped to literal session OIDs on the current utility path
- `FETCH MEMORY` is agent-scoped and operator-facing, not yet a full semantic retrieval planner node
- `SHOW TRACE` is a stable operator view over the runtime trace stream, not a debugger protocol
- memory payloads and tags are stored as serialized text, not as a specialized vector or semantic index structure

Why this is a fork-justified step:
- agent memory is now persisted in an engine-owned catalog, not in application tables or extension-owned relations
- the command surface is native to the server protocol and utility pipeline
- memory persistence and runtime traces now share ownership, privileges, and system-catalog observability boundaries

Accepted debt:
- no custom access method or compression strategy yet
- no semantic ANN index, embedding storage, or learned retrieval path yet
- `pg_qx_memory` is still a straightforward heap relation until benchmarks justify deeper storage work
- runtime trace details are optimized for operator stability, not yet for downstream tracing export

Tests run:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`
