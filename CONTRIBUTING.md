# Contributing to QhapaqXian DB

QhapaqXian DB is a fork of PostgreSQL with engine-owned agentic subsystems. Contributions must preserve two constraints:
- keep the fork technically honest about what is native, temporary, or still deferred;
- keep the maintenance delta against upstream PostgreSQL reviewable.

## Branching

- use topic branches from `codex/bootstrap` or the current maintained integration branch;
- keep branch names under the `codex/` prefix for local Codex-driven work;
- do not force-push shared integration branches.

## Patch classes

Every non-trivial patch should state which class it belongs to:
- `TEMP_PATCH`: short-lived bridge toward a later subsystem;
- `PERM_SUBSYSTEM`: durable fork-owned subsystem;
- `UPSTREAMABLE_CANDIDATE`: patch that might be proposed upstream later.

Record durable or costly divergence in [docs/patch-ledger.md](E:/HijosDelSol/qhapaqxian-db/docs/patch-ledger.md).

## Engineering rules

- do not market temporary bridges as final engine capabilities;
- if a change touches parser, catalogs, planner, executor, WAL, replication, or storage, describe the fork justification explicitly;
- prefer narrow, test-backed deltas over broad speculative rewrites;
- preserve upstream behavior where QhapaqXian DB does not intentionally diverge.

## Tests

Before opening a PR, run the relevant subset at minimum:

```powershell
$env:PATH='C:\Program Files\Git\usr\bin;' + $env:PATH
meson test -C build-stage4 --suite postgresql:setup --print-errorlogs
meson test -C build-stage4 --suite postgresql:regress --print-errorlogs
```

If you touch the logical decoder, also run:

```powershell
$env:PATH='C:\Program Files\Git\usr\bin;' + $env:PATH
meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs
```

## Documentation

When a stage closes, update:
- the relevant `docs/stage-*/README.md`;
- [README.md](E:/HijosDelSol/qhapaqxian-db/README.md);
- [docs/bootstrap-plan.md](E:/HijosDelSol/qhapaqxian-db/docs/bootstrap-plan.md);
- [docs/patch-ledger.md](E:/HijosDelSol/qhapaqxian-db/docs/patch-ledger.md).
