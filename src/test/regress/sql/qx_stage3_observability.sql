-- Stage 33 observability collector smoke test (unique objects; standalone)

CREATE PROVIDER public.stats_loopback_provider
  KIND 'loopback'
  ENDPOINT 'local://qhapaqxian-tool-runner'
  RECEIPT KEY 'stats-loopback-key'
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest
  ATTESTATION ENABLE;

CREATE PRINCIPAL public.stats_search_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'restricted'
  RUNTIME 'host'
  PROVIDER public.stats_loopback_provider
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest;

CREATE TOOL public.stats_search
  HANDLER 'builtin.search'
  SANDBOX 'builtin'
  PRINCIPAL 'stats_search_runner'
  TOKEN COST 11
  COST 7;

CREATE NAMESPACE POLICY stats_only FOR SCHEMA public
  TOOLS (stats_search)
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;

CREATE AGENT stats_probe
  IDENTITY stats_probe_identity
  TOOLS (stats_search)
  POLICY stats_only
  BUDGET (tokens 4096, cost 128);

START SESSION FOR AGENT stats_probe
  WITH CONTEXT jsonb_build_object('mission', 'stats');

SELECT oid AS qx_stats_session_oid
FROM pg_qx_session
WHERE qxsessionagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'stats_probe')
ORDER BY oid DESC
LIMIT 1
\gset

SELECT pg_qx_stat_reset('all');

RUN TASK stats_probe_task IN SESSION :qx_stats_session_oid
  GOAL 'stats probe'
  INPUT jsonb_build_object('topic', 'observability')
  PRIORITY normal;

SELECT submit_count > 0 AS provider_submit_seen
FROM pg_qx_stat_get_provider_stats() AS s(provider_oid oid,
                                          provider_name text,
                                          provider_kind text,
                                          provider_endpoint text,
                                          enabled boolean,
                                          attestation_required boolean,
                                          submit_count bigint,
                                          resume_count bigint,
                                          verified_receipts bigint,
                                          rejected_receipts bigint,
                                          checkpoint_count bigint,
                                          execution_count bigint,
                                          task_count bigint,
                                          reserved bigint)
WHERE provider_name = 'stats_loopback_provider';

SELECT submit_count > 0 AS provider_view_submit_seen
FROM pg_stat_qx_providers
WHERE provider_name = 'stats_loopback_provider';

SELECT submit_count > 0 AS principal_submit_seen
FROM pg_qx_stat_get_principal_stats() AS s(principal_oid oid,
                                           principal_name text,
                                           provider_name text,
                                           provider_kind text,
                                           runtime_class text,
                                           sandbox_name text,
                                           program_name text,
                                           enabled boolean,
                                           has_signer boolean,
                                           submit_count bigint,
                                           resume_count bigint,
                                           verified_receipts bigint,
                                           rejected_receipts bigint,
                                           checkpoint_count bigint,
                                           execution_count bigint)
WHERE principal_name = 'stats_search_runner';

SELECT submit_count > 0 AS principal_view_submit_seen
FROM pg_stat_qx_principals
WHERE principal_name = 'stats_search_runner';

SELECT submit_count > 0 AS runtime_class_submit_seen
FROM pg_qx_stat_get_runtime_class_stats() AS s(runtime_class text,
                                               submit_count bigint,
                                               resume_count bigint,
                                               verified_receipts bigint,
                                               rejected_receipts bigint,
                                               checkpoint_count bigint,
                                               execution_count bigint,
                                               reserved bigint)
WHERE runtime_class = 'host';

SELECT submit_count > 0 AS runtime_class_view_submit_seen
FROM pg_stat_qx_runtime_classes
WHERE runtime_class = 'host';

SELECT pg_qx_stat_reset('provider');

SELECT COALESCE(max(submit_count), 0) = 0 AS provider_stats_cleared
FROM pg_qx_stat_get_provider_stats() AS s(provider_oid oid,
                                          provider_name text,
                                          provider_kind text,
                                          provider_endpoint text,
                                          enabled boolean,
                                          attestation_required boolean,
                                          submit_count bigint,
                                          resume_count bigint,
                                          verified_receipts bigint,
                                          rejected_receipts bigint,
                                          checkpoint_count bigint,
                                          execution_count bigint,
                                          task_count bigint,
                                          reserved bigint)
WHERE provider_name = 'stats_loopback_provider';

SELECT COALESCE(max(submit_count), 0) > 0 AS principal_stats_survive_provider_reset
FROM pg_qx_stat_get_principal_stats() AS s(principal_oid oid,
                                             principal_name text,
                                             provider_name text,
                                             provider_kind text,
                                             runtime_class text,
                                             sandbox_name text,
                                             program_name text,
                                             enabled boolean,
                                             has_signer boolean,
                                             submit_count bigint,
                                             resume_count bigint,
                                             verified_receipts bigint,
                                             rejected_receipts bigint,
                                             checkpoint_count bigint,
                                             execution_count bigint)
WHERE principal_name = 'stats_search_runner';

SELECT pg_qx_stat_reset('principal');

SELECT COALESCE(max(submit_count), 0) = 0 AS principal_stats_cleared
FROM pg_qx_stat_get_principal_stats() AS s(principal_oid oid,
                                             principal_name text,
                                             provider_name text,
                                             provider_kind text,
                                             runtime_class text,
                                             sandbox_name text,
                                             program_name text,
                                             enabled boolean,
                                             has_signer boolean,
                                             submit_count bigint,
                                             resume_count bigint,
                                             verified_receipts bigint,
                                             rejected_receipts bigint,
                                             checkpoint_count bigint,
                                             execution_count bigint)
WHERE principal_name = 'stats_search_runner';

SELECT pg_qx_stat_reset('runtime_class');

SELECT COALESCE(max(submit_count), 0) = 0 AS runtime_class_stats_cleared
FROM pg_qx_stat_get_runtime_class_stats() AS s(runtime_class text,
                                               submit_count bigint,
                                               resume_count bigint,
                                               verified_receipts bigint,
                                               rejected_receipts bigint,
                                               checkpoint_count bigint,
                                               execution_count bigint,
                                               reserved bigint)
WHERE runtime_class = 'host';