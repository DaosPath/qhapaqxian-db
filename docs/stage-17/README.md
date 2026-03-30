# Stage 17: Stronger Runtime Sandbox Enforcement

Stage 17 hardens external principal execution from "out-of-process" into
"out-of-process with OS-level execution envelopes".

What landed:
- Stage 16 sandbox labels now resolve to engine-owned runtime profiles with
  fixed timeout, memory, file-size, open-file, and process-count ceilings;
- the external principal launcher now runs with a minimal environment, a
  sealed runtime working directory, and no shell involvement;
- Windows execution is attached to a Job Object with kill-on-close, memory,
  and active-process limits before the child thread resumes;
- POSIX execution now applies `setrlimit()` ceilings, closes inherited file
  descriptors, changes into the runtime directory, and kills timed-out
  children explicitly;
- the shipped `qhapaqxian-tool-runner` now reports the enforced profile,
  environment mode, workdir basename, timeout, and process limit back to the
  engine;
- runtime traces and events now expose `effective_sandbox`, `profile`,
  `env=minimal`, `cwd=pg_qx_runtime`, `process_limit`, and `timeout_ms`;
- regression coverage now asserts that submit-phase and resume-phase external
  execution actually ran under the stronger sandbox profile, not just under a
  catalog descriptor.

Runtime shape in this stage:
- submit path remains
  `stage15.authorize_tools -> stage16.external_submit -> stage8.capture_input -> stage8.checkpoint_barrier`;
- resume path remains
  `stage8.resume_dispatch -> stage16.external_resume -> stage8.complete`;
- the difference is that `stage16.external_*` now enforces runtime profiles in
  the OS process launcher instead of only validating catalog metadata.

What this stage does not claim:
- it is not a microVM, container, seccomp, pledge, AppContainer, or brokered
  provider runtime;
- it does not yet execute principals under alternate OS users or tokens;
- metering is still fork-owned accounting based on the runner response, even
  though that response now comes from a sandboxed child process.

Why the fork boundary is justified here:
- the change touches engine runtime launch code, process supervision, trace and
  event semantics, regression fixtures, and build/install surfaces for shipped
  principal helpers;
- stronger agent execution isolation at the engine layer cannot be delivered as
  SQL-only metadata or as an external middleware wrapper while preserving task
  checkpoints, task budgets, and semantic observability in-core.

Validation:
- `meson test -C build-stage4 --suite postgresql:setup --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:qhapaqxian_output --print-errorlogs`
- `meson test -C build-stage4 --suite postgresql:regress --print-errorlogs`

Result:
- `postgresql:setup` passed
- `postgresql:qhapaqxian_output` passed (`1 subtests passed`)
- `postgresql:regress` passed (`225 subtests passed`)
