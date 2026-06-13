-- Stage 27 recovery scanner observability regression (loopback, no Docker)

CREATE PROVIDER public.rec_loopback_provider
  KIND 'loopback'
  ENDPOINT 'local://qhapaqxian-tool-runner'
  RECEIPT KEY 'rec-loopback-key'
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest
  ATTESTATION ENABLE;

CREATE PRINCIPAL public.rec_search_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'restricted'
  RUNTIME 'host'
  PROVIDER public.rec_loopback_provider
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest;

CREATE TOOL public.rec_search
  HANDLER 'builtin.search'
  SANDBOX 'builtin'
  PRINCIPAL 'rec_search_runner'
  TOKEN COST 5
  COST 3;

CREATE NAMESPACE POLICY rec_only FOR SCHEMA public
  TOOLS (rec_search)
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;

CREATE AGENT rec_probe
  IDENTITY rec_probe_identity
  TOOLS (rec_search)
  POLICY rec_only
  BUDGET (tokens 2048, cost 128);

START SESSION FOR AGENT rec_probe
  WITH CONTEXT jsonb_build_object('mission', 'recovery');

SELECT oid AS qx_rec_session_oid
FROM pg_qx_session
WHERE qxsessionagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'rec_probe')
ORDER BY oid DESC
LIMIT 1
\gset

RUN TASK rec_probe_task IN SESSION :qx_rec_session_oid
  GOAL 'recovery scan probe'
  INPUT jsonb_build_object('topic', 'recovery')
  PRIORITY normal;

SELECT oid AS qx_rec_task_oid
FROM pg_qx_task
WHERE qxtaskagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'rec_probe')
ORDER BY oid DESC
LIMIT 1
\gset

SELECT pg_qx_test_start_recovery_attempt(:qx_rec_task_oid) AS qx_rec_attempt_oid \gset

SELECT pg_qx_stat_reset('recovery');

SELECT startup_scan
   AND tasks_scanned >= 1
   AND (tasks_requeued >= 1 OR tasks_requeue_suppressed >= 1) AS pre_write_scan_actionable
FROM pg_qx_recovery_scan() AS s(startup_scan boolean,
                                 failover_rebuild boolean,
                                 tasks_scanned int,
                                 attempts_scanned int,
                                 checkpoints_scanned int,
                                 tasks_requeued int,
                                 attempts_fenced int,
                                 checkpoints_replayed int,
                                 orphan_attempts int,
                                 semantic_replay_candidates int,
                                 tasks_requeue_suppressed int,
                                 attempts_fence_suppressed int);

SELECT pg_qx_test_run_startup_recovery() LIKE '%recovery_tasks_requeued=%' AS startup_recovery_ran;

SELECT pg_qx_test_run_failover_rebuild() LIKE '%recovery_failover_rebuild=true%' AS failover_rebuild_ran;

SELECT failover_rebuild_count >= 1
   AND tasks_requeue_suppressed >= 1 AS recovery_stats_after_failover
FROM pg_stat_qx_recovery;