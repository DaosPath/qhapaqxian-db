-- QhapaqXian DB integrated runtime slice with real backend evidence

CREATE PROVIDER public.loopback_provider
  KIND 'loopback'
  ENDPOINT 'local://qhapaqxian-tool-runner'
  RECEIPT KEY 'loopback-stage20-key'
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest
  ATTESTATION ENABLE;

CREATE PROVIDER public.container_provider
  KIND 'container'
  ENDPOINT 'container://broker/pool'
  USING 'ed25519'
  RECEIPT KEY $$-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEATQR0xvmOgsHyUp6bWkQ7xlKHg40piOubNdB+Ew80TOs=
-----END PUBLIC KEY-----$$
  ATTESTATION PROFILE 'container.broker'
  VERSION 'v1'
  POLICY container_attest
  ATTESTATION ENABLE;

CREATE PROVIDER public.microvm_provider
  KIND 'microvm'
  ENDPOINT 'microvm://broker/primary'
  USING 'ed25519'
  RECEIPT KEY $$-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEATQR0xvmOgsHyUp6bWkQ7xlKHg40piOubNdB+Ew80TOs=
-----END PUBLIC KEY-----$$
  ATTESTATION PROFILE 'microvm.broker'
  VERSION 'v1'
  POLICY microvm_attest
  ATTESTATION ENABLE;

ALTER PROVIDER public.loopback_provider
  ENDPOINT 'local://qhapaqxian-tool-runner'
  RECEIPT KEY 'loopback-stage20-key'
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest
  ATTESTATION ENABLE
  ENABLE;

CREATE PRINCIPAL public.search_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'restricted'
  RUNTIME 'host'
  PROVIDER public.loopback_provider
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest;

CREATE PRINCIPAL public.extract_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'isolated'
  RUNTIME 'container'
  PROVIDER public.container_provider
  SIGNER 'qx_remote_ed25519_private.pem'
  ATTESTATION PROFILE 'container.broker'
  VERSION 'v1'
  POLICY container_attest;

CREATE PRINCIPAL public.summarize_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'isolated'
  RUNTIME 'microvm'
  PROVIDER public.microvm_provider
  SIGNER 'qx_remote_ed25519_private.pem'
  ATTESTATION PROFILE 'microvm.broker'
  VERSION 'v1'
  POLICY microvm_attest;

ALTER PRINCIPAL public.search_runner
  PROVIDER public.loopback_provider
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'restricted'
  RUNTIME 'host'
  ATTESTATION PROFILE 'loopback.local'
  VERSION 'v1'
  POLICY loopback_attest;

CREATE PROVIDER public.bad_partial_attestation_provider
  KIND 'container'
  ENDPOINT 'container://bad/partial'
  USING 'ed25519'
  RECEIPT KEY $$-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEATQR0xvmOgsHyUp6bWkQ7xlKHg40piOubNdB+Ew80TOs=
-----END PUBLIC KEY-----$$
  ATTESTATION PROFILE 'container.partial'
  VERSION 'v1'
  ATTESTATION ENABLE;

CREATE PROVIDER public.bad_missing_attestation_provider
  KIND 'microvm'
  ENDPOINT 'microvm://bad/missing'
  USING 'ed25519'
  RECEIPT KEY $$-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEATQR0xvmOgsHyUp6bWkQ7xlKHg40piOubNdB+Ew80TOs=
-----END PUBLIC KEY-----$$;

CREATE PRINCIPAL public.bad_attestation_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'isolated'
  RUNTIME 'microvm'
  PROVIDER public.microvm_provider
  SIGNER 'qx_remote_ed25519_private.pem'
  ATTESTATION PROFILE 'container.broker'
  VERSION 'v1'
  POLICY microvm_attest;

BEGIN;
CREATE PRINCIPAL public.inherited_container_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'isolated'
  RUNTIME 'container'
  PROVIDER public.container_provider
  SIGNER 'qx_remote_ed25519_private.pem';

SELECT qxprincipalattestationprofile::text = 'container.broker' AS inherited_profile,
       qxprincipalattestationversion::text = 'v1' AS inherited_version,
       qxprincipalattestationpolicy::text = 'container_attest' AS inherited_policy
FROM pg_qx_principal
WHERE qxprincipalname = 'inherited_container_runner';
ROLLBACK;

CREATE TOOL public.extract
  HANDLER 'builtin.extract'
  SANDBOX 'isolated'
  PRINCIPAL 'extract_runner'
  TOKEN COST 15
  COST 10;

CREATE TOOL public.search
  HANDLER 'builtin.search'
  SANDBOX 'restricted'
  PRINCIPAL 'search_runner'
  TOKEN COST 9
  COST 5;

CREATE TOOL public.summarize
  HANDLER 'builtin.summarize'
  SANDBOX 'builtin'
  PRINCIPAL 'summarize_runner'
  TOKEN COST 13
  COST 9;

CREATE NAMESPACE POLICY guarded FOR SCHEMA public
  TOOLS (search)
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;

ALTER NAMESPACE POLICY guarded FOR SCHEMA public
  SET TOOLS (extract, search, summarize)
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;

CREATE NAMESPACE POLICY strict FOR SCHEMA public
  TOOLS (search)
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;

ALTER TOOL public.search
  TOKEN COST 11
  COST 7;

ALTER TOOL public.summarize
  SANDBOX 'isolated'
  POLICY guarded;

CREATE AGENT archivist
  IDENTITY imperial
  MODEL 'openai:gpt-5.4'
  MEMORY PROFILE episodic
  TOOLS (search, summarize)
  POLICY guarded
  BUDGET (tokens 4096, cost 128);

SELECT qxagentname::text AS agent_name,
       qxagentnamespace = 'public'::regnamespace AS in_public_schema,
       qxnamespacepolicyid <> 0 AS has_namespace_policy_oid,
       qxidentityid <> 0 AS has_identity_oid,
       qxagentowner = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS owned_by_current_user,
       qxidentity::text AS identity_name,
       qxmodeluri::text AS model_uri,
       qxmemoryprofile::text AS memory_profile,
       qxpolicy::text AS policy_name,
       qxtools IS NOT NULL AS has_tools,
       qxbudget IS NOT NULL AS has_budget
FROM pg_qx_agent
WHERE qxagentname = 'archivist';

SELECT qxnamespacepolicyname::text AS policy_row_name,
       qxnamespacepolicy::text AS policy_name,
       qxnamespaceid = 'public'::regnamespace AS binds_public_schema,
       qxnamespaceowner = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS owned_by_current_user,
       qxnamespaceauthrole = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS auth_role_is_current_user,
       qxrequireknowntools AS require_known_tools,
       qxenforcebudgets AS enforce_budgets,
       qxallowedtools IS NOT NULL AS has_allowed_tools
FROM pg_qx_namespace
WHERE qxnamespaceid = 'public'::regnamespace
ORDER BY qxnamespacepolicyname;

SELECT qxidentityname::text AS identity_name,
       qxidentitynamespace = 'public'::regnamespace AS in_public_schema,
       qxidentityowner = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS owned_by_current_user,
       qxidentityauthrole = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS auth_role_is_current_user,
       qxidentitypolicy::text AS policy_name,
       qxidentitybudget IS NOT NULL AS has_budget
FROM pg_qx_identity
WHERE qxidentityname = 'imperial';

SELECT qxprovidername::text AS provider_name,
       qxprovidernamespace = 'public'::regnamespace AS in_public_schema,
       qxproviderenabled AS enabled,
       qxproviderattestationrequired AS attestation_required,
       qxproviderkind::text AS provider_kind,
       qxproviderendpoint::text AS provider_endpoint,
       qxproviderreceiptalg::text AS receipt_alg,
       qxproviderreceiptkey IS NOT NULL AS has_receipt_key
FROM pg_qx_provider
ORDER BY qxprovidername;

SELECT qxprincipalname::text AS principal_name,
       qxprincipalnamespace = 'public'::regnamespace AS in_public_schema,
       qxprincipalenabled AS enabled,
       qxprincipalsandbox::text AS sandbox_name,
       qxprincipalruntimeclass::text AS runtime_class,
       qxprincipalprogram::text AS program_name,
       qxprincipalreceiptsigner IS NOT NULL AS has_receipt_signer,
       qxprincipalproviderid <> 0 AS has_provider_oid,
       qxprincipalprovider::text AS provider_name
FROM pg_qx_principal
ORDER BY qxprincipalname;

SELECT qxtoolname::text AS tool_name,
       qxtoolenabled AS enabled,
       qxtooltokencost AS token_cost,
       qxtoolcostunits AS cost_units,
       qxtoolsandbox::text AS sandbox_name,
       qxtoolprincipalid <> 0 AS has_principal_oid,
       qxtoolprincipal::text AS principal_name,
       qxtoolpolicy IS NULL AS policy_is_null
FROM pg_qx_tool
ORDER BY qxtoolname;

CREATE AGENT archivist
  IDENTITY imperial;

START SESSION FOR AGENT archivist
  WITH CONTEXT jsonb_build_object('mission', 'index');

SELECT qxsessionagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'archivist') AS linked_agent,
       qxsessionnamespacepolicyid = (SELECT qxnamespacepolicyid FROM pg_qx_agent WHERE qxagentname = 'archivist') AS linked_policy,
       qxsessionidentityid = (SELECT qxidentityid FROM pg_qx_agent WHERE qxagentname = 'archivist') AS linked_identity,
       qxsessiondbid = (SELECT oid FROM pg_database WHERE datname = current_database()) AS current_db,
       qxsessionowner = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS owned_by_current_user,
       qxsessionstatus AS status,
       qxcontext IS NOT NULL AS has_context
FROM pg_qx_session
ORDER BY oid;

SELECT qxeventkind::text AS event_kind,
       qxeventlsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxeventlsn > lag(qxeventlsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxeventtaskid = 0 AS session_scoped,
       qxeventstepid = 0 AS without_step
FROM pg_qx_event
ORDER BY oid;

SELECT qxtracename::text AS trace_name,
       qxtracestate AS trace_state,
       qxtracelsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxtracelsn > lag(qxtracelsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxtracetaskid = 0 AS session_scoped
FROM pg_qx_trace
ORDER BY oid;

SELECT oid AS qx_session_oid
FROM pg_qx_session
ORDER BY oid DESC
LIMIT 1
\gset

EXPLAIN AGENT RUN TASK summarize_docs IN SESSION :qx_session_oid
  GOAL 'summarize docs'
  INPUT jsonb_build_object('topic', 'parser')
  PRIORITY high;

RUN TASK summarize_docs IN SESSION :qx_session_oid
  GOAL 'summarize docs'
  INPUT jsonb_build_object('topic', 'parser')
  PRIORITY high;

SELECT oid AS qx_task_oid
FROM pg_qx_task
ORDER BY oid DESC
LIMIT 1
\gset

SELECT qxtaskname::text AS task_name,
       qxtaskgoal::text AS goal,
       qxtaskpriority::text AS priority,
       qxtaskstate AS state,
       qxtasknamespacepolicyid = (SELECT qxnamespacepolicyid FROM pg_qx_agent WHERE qxagentname = 'archivist') AS linked_policy,
       qxtaskidentityid = (SELECT qxidentityid FROM pg_qx_agent WHERE qxagentname = 'archivist') AS linked_identity,
       qxtaskinput IS NOT NULL AS has_input,
       qxtaskbudgettokens AS budget_tokens,
       qxtaskbudgetcost AS budget_cost,
       qxtaskauthorizedtooltokens AS authorized_tool_tokens,
       qxtaskauthorizedtoolcost AS authorized_tool_cost,
       qxtaskestimatedtokens AS estimated_tokens,
       qxtaskestimatedcost AS estimated_cost,
       qxtaskconsumedtokens AS consumed_tokens,
       qxtaskconsumedcost AS consumed_cost,
       qxtaskauthorizedtools IS NOT NULL AS has_authorized_tools,
       qxtasksessionid = :qx_session_oid AS linked_session,
       qxtasklastattemptid <> 0 AS has_attempt,
       qxtasklastcheckpointid <> 0 AS has_checkpoint
FROM pg_qx_task
ORDER BY oid;

SELECT qxtasksubmitcontract::text LIKE '%tool=search%' AS submit_route_is_search,
       qxtasksubmitcontract::text LIKE '%principal_runtime=host%' AS submit_route_is_host,
       qxtaskresumecontract::text LIKE '%tool=summarize%' AS resume_route_is_summarize,
       qxtaskresumecontract::text LIKE '%principal_runtime=microvm%' AS resume_route_is_microvm,
       qxtasksubmitcontract <> qxtaskresumecontract AS routes_diverge
FROM pg_qx_task
WHERE oid = :qx_task_oid;

SELECT qxattemptseqno AS seqno,
       qxattemptstate AS state,
       qxattemptresumecheckpointid = 0 AS starts_fresh,
       qxattemptstrategy::text AS strategy
FROM pg_qx_attempt
ORDER BY qxattemptseqno;

SELECT qxstepseqno AS seqno,
       qxstepname::text AS step_name,
       qxstepstate AS state
FROM pg_qx_step
ORDER BY qxstepseqno;

SELECT qxeventkind::text AS event_kind,
       qxeventlsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxeventlsn > lag(qxeventlsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxeventtaskid = (SELECT oid FROM pg_qx_task ORDER BY oid DESC LIMIT 1) AS linked_task,
       qxeventstepid <> 0 AS has_step
FROM pg_qx_event
ORDER BY oid;

SELECT qxtracename::text AS trace_name,
       qxtracestate AS trace_state,
       qxtracelsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxtracelsn > lag(qxtracelsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxtracetaskid = (SELECT oid FROM pg_qx_task ORDER BY oid DESC LIMIT 1) AS linked_task
FROM pg_qx_trace
ORDER BY oid;

SELECT qxcheckpointlabel::text AS checkpoint_label,
       qxcheckpointstate AS checkpoint_state,
       qxcheckpointtaskstate AS task_state,
       qxcheckpointlsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxcheckpointlsn > lag(qxcheckpointlsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxcheckpointnextstepseqno AS next_step,
       qxcheckpointattemptid <> 0 AS linked_attempt,
       qxcheckpointtaskid = (SELECT oid FROM pg_qx_task ORDER BY oid DESC LIMIT 1) AS linked_task,
       qxcheckpointdata::text LIKE 'task=%' AS has_data
FROM pg_qx_checkpoint
ORDER BY oid;

\c -

EXPLAIN AGENT RESUME TASK :qx_task_oid FROM CHECKPOINT 'stage8.after_capture';

RESUME TASK :qx_task_oid FROM CHECKPOINT 'stage8.after_capture';

SELECT qxtaskname::text AS task_name,
       qxtaskstate AS state,
       qxtaskconsumedtokens AS consumed_tokens,
       qxtaskconsumedcost AS consumed_cost,
       qxtasklastattemptid <> 0 AS has_attempt,
       qxtasklastcheckpointid <> 0 AS has_checkpoint
FROM pg_qx_task
ORDER BY oid;

SELECT qxattemptseqno AS seqno,
       qxattemptstate AS state,
       qxattemptresumecheckpointid <> 0 AS resumes_from_checkpoint,
       qxattemptstrategy::text AS strategy
FROM pg_qx_attempt
ORDER BY qxattemptseqno;

SELECT qxstepseqno AS seqno,
       qxstepname::text AS step_name,
       qxstepstate AS state
FROM pg_qx_step
ORDER BY qxstepseqno;

SELECT qxeventkind::text AS event_kind,
       qxeventlsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxeventlsn > lag(qxeventlsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxeventtaskid = (SELECT oid FROM pg_qx_task ORDER BY oid DESC LIMIT 1) AS linked_task,
       qxeventstepid <> 0 AS has_step
FROM pg_qx_event
ORDER BY oid;

SELECT qxtracename::text AS trace_name,
       qxtracestate AS trace_state,
       qxtracelsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxtracelsn > lag(qxtracelsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxtracetaskid = (SELECT oid FROM pg_qx_task ORDER BY oid DESC LIMIT 1) AS linked_task
FROM pg_qx_trace
ORDER BY oid;

SELECT qxcheckpointlabel::text AS checkpoint_label,
       qxcheckpointstate AS checkpoint_state,
       qxcheckpointtaskstate AS task_state,
       qxcheckpointlsn <> '0/0'::pg_lsn AS has_semantic_lsn,
       COALESCE(qxcheckpointlsn > lag(qxcheckpointlsn) OVER (ORDER BY oid), true) AS lsn_monotonic,
       qxcheckpointnextstepseqno AS next_step,
       qxcheckpointattemptid <> 0 AS linked_attempt
FROM pg_qx_checkpoint
ORDER BY oid;

REMEMBER IN SESSION :qx_session_oid
  SCOPE semantic
  KEY 'parser.notes'
  VALUE 'stable grammar'
  TAGS ('docs', 'stage10');

REMEMBER IN SESSION :qx_session_oid
  SCOPE episodic
  KEY 'resume.status'
  VALUE 'completed'
  TAGS ('resume', 'success');

SELECT qxmemorysessionid = :qx_session_oid AS linked_session,
       qxmemoryscope AS scope,
       qxmemorykey::text AS memory_key,
       qxmemoryvalue::text AS memory_value,
       qxmemorytags::text AS tags,
       qxmemoryowner = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS owned_by_current_user
FROM pg_qx_memory
ORDER BY oid;

FETCH MEMORY FOR AGENT archivist
  SCOPE semantic
  MATCH 'parser'
  LIMIT 5;

FETCH MEMORY FOR AGENT archivist
  SCOPE episodic
  MATCH 'completed'
  LIMIT 5;

SHOW TRACE FOR TASK :qx_task_oid LIMIT 20;

SELECT qxtracename::text AS trace_name,
       CASE
         WHEN qxtracename = 'runtime.external_submit' THEN qxtracedetail LIKE '%effective_sandbox=restricted%'
         WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%effective_sandbox=isolated%'
         ELSE false
       END AS expected_sandbox,
       CASE
        WHEN qxtracename = 'runtime.external_submit' THEN qxtracedetail LIKE '%provider=loopback_provider%'
        WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%provider=microvm_provider%'
        ELSE false
      END AS expected_provider,
      CASE
        WHEN qxtracename = 'runtime.external_submit' THEN qxtracedetail LIKE '%provider_kind=loopback%'
        WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%provider_kind=microvm%'
        ELSE false
      END AS expected_provider_kind,
      CASE
        WHEN qxtracename = 'runtime.external_submit' THEN qxtracedetail LIKE '%principal_runtime=host%'
        WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%principal_runtime=microvm%'
        ELSE false
      END AS expected_principal_runtime,
      qxtracedetail LIKE '%profile=%' AS has_profile,
       qxtracedetail LIKE '%env=minimal%' AS minimal_env,
       qxtracedetail LIKE '%cwd=pg_qx_runtime%' AS runtime_workdir,
       qxtracedetail LIKE '%process_limit=1%' AS single_process_cap,
       qxtracedetail LIKE '%timeout_ms=%' AS has_timeout,
       qxtracedetail LIKE '%receipt_schema=qx.receipt.v1%' AS has_receipt_schema,
       CASE
         WHEN qxtracename = 'runtime.external_submit' THEN qxtracedetail LIKE '%receipt_alg=hmac-sha256%'
         WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%receipt_alg=ed25519%'
         ELSE false
       END AS has_receipt_alg,
       CASE
         WHEN qxtracename = 'runtime.external_submit' THEN qxtracedetail LIKE '%receipt_nonce=submit:search:search_runner%'
         WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%receipt_nonce=resume:summarize:summarize_runner%'
         ELSE false
       END AS expected_receipt_nonce,
       qxtracedetail LIKE '%receipt_sig=verified%' AS has_receipt_sig,
      CASE
        WHEN qxtracename = 'runtime.external_submit' THEN qxtracedetail LIKE '%attestation=loopback_verified%'
        WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%attestation=microvm_receipt_verified%'
        ELSE false
      END AS expected_attestation,
       qxtracedetail LIKE '%launch_mode=profiled_process%' AS has_launch_mode,
       qxtracedetail LIKE '%restricted_identity=%' AS has_restricted_identity,
       qxtracedetail LIKE '%wall_ms=%' AS has_wall_time,
       CASE
         WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail LIKE '%vm_id=%'
         ELSE true
       END AS has_vm_id,
       CASE
         WHEN qxtracename = 'runtime.external_resume' THEN qxtracedetail NOT LIKE '%vm_id=;%'
         ELSE true
       END AS vm_id_populated
FROM pg_qx_trace
WHERE qxtracename IN ('runtime.external_submit', 'runtime.external_resume')
ORDER BY oid;

CREATE AGENT extractor
  IDENTITY imperial
  MODEL 'openai:gpt-5.4-mini'
  TOOLS (extract)
  POLICY guarded
  BUDGET (tokens 1024, cost 128);

START SESSION FOR AGENT extractor
  WITH CONTEXT jsonb_build_object('mission', 'extract');

SELECT oid AS qx_container_session_oid
FROM pg_qx_session
WHERE qxsessionagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'extractor')
ORDER BY oid DESC
LIMIT 1
\gset

RUN TASK extract_docs IN SESSION :qx_container_session_oid
  GOAL 'extract docs'
  INPUT jsonb_build_object('topic', 'archive')
  PRIORITY high;

SELECT oid AS qx_container_task_oid
FROM pg_qx_task
WHERE qxtaskagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'extractor')
ORDER BY oid DESC
LIMIT 1
\gset

SELECT qxtaskname::text AS task_name,
       qxtaskstate AS state,
       qxtaskconsumedtokens > 0 AS charged_tokens,
       qxtaskconsumedcost > 0 AS charged_cost,
       qxtasklastattemptid <> 0 AS has_attempt,
       qxtasklastcheckpointid <> 0 AS has_checkpoint
FROM pg_qx_task
WHERE oid = :qx_container_task_oid;

SELECT qxtracename::text AS trace_name,
       qxtracedetail LIKE '%effective_sandbox=isolated%' AS expected_sandbox,
       qxtracedetail LIKE '%principal=extract_runner%' AS expected_principal,
       qxtracedetail LIKE '%principal_runtime=container%' AS expected_principal_runtime,
       qxtracedetail LIKE '%provider=container_provider%' AS expected_provider,
       qxtracedetail LIKE '%provider_kind=container%' AS expected_provider_kind,
       qxtracedetail LIKE '%receipt_alg=ed25519%' AS expected_receipt_alg,
       qxtracedetail LIKE '%receipt_sig=verified%' AS has_receipt_sig,
       qxtracedetail LIKE '%attestation=container_receipt_verified%' AS expected_attestation,
       qxtracedetail LIKE '%container_id=%' AS has_container_id,
       qxtracedetail NOT LIKE '%container_id=;%' AS container_id_populated
FROM pg_qx_trace
WHERE qxtracetaskid = :qx_container_task_oid
  AND qxtracename = 'runtime.external_submit';

SELECT pg_qx_test_start_recovery_attempt(:qx_container_task_oid) AS qx_container_recovery_attempt_oid
\gset

SELECT qxtaskstate AS task_state,
       qxtasklastattemptid = :qx_container_recovery_attempt_oid AS helper_attempt_recorded,
       qxtasklastcheckpointid <> 0 AS still_has_checkpoint
FROM pg_qx_task
WHERE oid = :qx_container_task_oid;

SELECT qxattemptseqno AS seqno,
       qxattemptstate AS state,
       qxattemptresumecheckpointid <> 0 AS resumes_from_checkpoint,
       qxattemptstrategy::text AS strategy
FROM pg_qx_attempt
WHERE oid = :qx_container_recovery_attempt_oid;

SELECT pg_sleep(12);

SELECT EXISTS (
         SELECT 1
         FROM pg_stat_qx_scheduler_ledger_heartbeats
         WHERE task_oid = :qx_container_task_oid
           AND attempt_seqno = 2
           AND receipt_mode = 'scheduler-renew'
       ) AS saw_scheduler_renew,
       EXISTS (
         SELECT 1
         FROM pg_stat_qx_scheduler_ledger_heartbeats
         WHERE task_oid = :qx_container_task_oid
           AND attempt_seqno = 2
           AND receipt_mode = 'scheduler-reclaim'
       ) AS saw_scheduler_reclaim,
       EXISTS (
         SELECT 1
         FROM pg_qx_task
         WHERE oid = :qx_container_task_oid
           AND qxtaskstate = 'k'
       ) AS task_checkpointed,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE oid = :qx_container_recovery_attempt_oid
           AND qxattemptstate = 'k'
       ) AS attempt_checkpointed;

SELECT qxtaskstate AS task_state_after_supervision,
       qxtasklastattemptid = :qx_container_recovery_attempt_oid AS reclaimed_attempt_recorded,
       qxtasklastcheckpointid <> 0 AS still_has_checkpoint
FROM pg_qx_task
WHERE oid = :qx_container_task_oid;

SELECT qxattemptseqno AS seqno,
       qxattemptstate AS state,
       qxattemptresumecheckpointid <> 0 AS resumes_from_checkpoint,
       qxattemptstrategy::text AS strategy
FROM pg_qx_attempt
WHERE oid = :qx_container_recovery_attempt_oid;

\c -

SELECT pg_qx_test_run_startup_recovery() AS recovery_report;

SELECT pg_qx_test_run_failover_rebuild() AS recovery_report;

SELECT count(*) AS recovery_queue_rows
FROM pg_stat_qx_scheduler_ledger_queues
WHERE task_oid = :qx_container_task_oid
  AND attempt_seqno = 2
  AND queue_kind = 2;

SELECT count(*) AS reclaimed_lease_rows
FROM pg_stat_qx_scheduler_ledger_leases
WHERE task_oid = :qx_container_task_oid
  AND attempt_seqno = 2
  AND lease_state = 4;

CREATE AGENT tiny_budget
  IDENTITY sentinel
  MODEL 'openai:gpt-5.4-mini'
  TOOLS (search)
  POLICY strict
  BUDGET (tokens 10, cost 5);

START SESSION FOR AGENT tiny_budget;

SELECT oid AS qx_tiny_session_oid
FROM pg_qx_session
WHERE qxsessionagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'tiny_budget')
ORDER BY oid DESC
LIMIT 1
\gset

RUN TASK budget_probe IN SESSION :qx_tiny_session_oid
  GOAL 'should exceed budget'
  INPUT jsonb_build_object('topic', 'over budget');

SELECT task_oid = :qx_container_task_oid AS task_match,
       attempt_seqno,
       queue_name::text AS queue_name,
       queue_kind,
       priority::text AS priority,
       runtime_class::text AS runtime_class,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       runnable_count,
       leased_count,
       blocked_count,
       retry_count
FROM pg_stat_qx_scheduler_ledger_queues
WHERE task_oid = :qx_container_task_oid
ORDER BY attempt_seqno, queue_kind, retry_count;

SELECT task_oid = :qx_container_task_oid AS task_match,
       attempt_seqno,
       regexp_replace(worker_name::text,
                      'slot [0-9]+/[0-9]+$',
                      'slot <slot>/<slots>') AS worker_name,
       queue_name::text AS queue_name,
       runtime_class::text AS runtime_class,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       lease_state,
       renewal_count,
       needs_recovery,
       lease_expired
FROM pg_stat_qx_scheduler_ledger_leases
WHERE task_oid = :qx_container_task_oid
ORDER BY attempt_seqno, lease_state, renewal_count, worker_name, provider_name;

SELECT task_oid = :qx_container_task_oid AS task_match,
       attempt_seqno,
       regexp_replace(worker_name::text,
                      'slot [0-9]+/[0-9]+$',
                      'slot <slot>/<slots>') AS worker_name,
       queue_name::text AS queue_name,
       runtime_class::text AS runtime_class,
       provider_kind::text AS provider_kind,
       receipt_mode::text AS receipt_mode,
       heartbeat_state,
       lag_ms,
       stale,
       needs_attention AND stale AS needs_attention
FROM pg_stat_qx_scheduler_ledger_heartbeats
WHERE task_oid = :qx_container_task_oid
ORDER BY attempt_seqno, receipt_mode, worker_name, provider_kind;

SELECT agent_name::text AS agent_name,
       namespace_policy_name::text AS namespace_policy_name,
       active_session_count,
       task_count,
       completed_task_count,
       memory_count
FROM pg_stat_qx_agents
ORDER BY agent_name;

SELECT session_oid = :qx_session_oid AS session_match,
       agent_name::text AS agent_name,
       identity_name::text AS identity_name,
       namespace_policy_name::text AS namespace_policy_name,
       session_status,
       has_context,
       task_count,
       completed_task_count,
       memory_count,
       trace_count
FROM pg_stat_qx_sessions
ORDER BY session_oid;

SELECT task_oid = :qx_task_oid AS task_match,
       agent_name::text AS agent_name,
       identity_name::text AS identity_name,
       namespace_policy_name::text AS namespace_policy_name,
       task_name::text AS task_name,
       task_state,
       budget_tokens,
       budget_cost,
       consumed_tokens,
       consumed_cost,
       remaining_tokens,
       remaining_cost,
       has_attempt,
       has_checkpoint,
       attempt_count,
       step_count,
       event_count,
       trace_count,
       checkpoint_count,
       scheduler_queue::text AS scheduler_queue,
       scheduler_runtime::text AS scheduler_runtime,
       scheduler_provider::text AS scheduler_provider,
       scheduler_worker::text AS scheduler_worker,
       scheduler_retry::text AS scheduler_retry
FROM pg_stat_qx_tasks
ORDER BY task_oid;

SELECT queue_name::text AS queue_name,
       namespace_policy_name::text AS namespace_policy_name,
       queue_kind,
       priority_weight,
       runtime_class::text AS runtime_class,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       task_count,
       queued_task_count,
       running_task_count,
       checkpointed_task_count,
       completed_task_count,
       retrying_task_count,
       max_retries,
       heartbeat_interval_ms,
       lease_ttl_ms
FROM pg_stat_qx_scheduler_queues
ORDER BY queue_name;

SELECT regexp_replace(worker_name::text,
                      'slot [0-9]+/[0-9]+$',
                      'slot <slot>/<slots>') AS worker_name,
       runtime_class::text AS runtime_class,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       dispatched_task_count,
       queue_count,
       running_task_count,
       checkpointed_task_count,
       completed_task_count,
       retrying_task_count,
       expired_lease_count,
       stale_heartbeat_count,
       lease_state,
       heartbeat_state
FROM pg_stat_qx_scheduler_workers
ORDER BY worker_name, runtime_class, provider_name;

SELECT task_oid = :qx_task_oid AS task_match,
       attempt_seqno,
       queue_name::text AS queue_name,
       queue_kind,
       priority::text AS priority,
       runtime_class::text AS runtime_class,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       runnable_count,
       leased_count,
       blocked_count,
       retry_count
FROM pg_stat_qx_scheduler_ledger_queues
ORDER BY attempt_seqno, queue_name, queue_kind, retry_count, runtime_class, provider_name;

SELECT task_oid = :qx_task_oid AS task_match,
       attempt_seqno,
       regexp_replace(worker_name::text,
                      'slot [0-9]+/[0-9]+$',
                      'slot <slot>/<slots>') AS worker_name,
       queue_name::text AS queue_name,
       runtime_class::text AS runtime_class,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       lease_state,
       renewal_count,
       needs_recovery,
       lease_expired
FROM pg_stat_qx_scheduler_ledger_leases
ORDER BY attempt_seqno, lease_state, renewal_count, worker_name, provider_name;

SELECT task_oid = :qx_task_oid AS task_match,
       attempt_seqno,
       regexp_replace(worker_name::text,
                      'slot [0-9]+/[0-9]+$',
                      'slot <slot>/<slots>') AS worker_name,
       queue_name::text AS queue_name,
       runtime_class::text AS runtime_class,
       provider_kind::text AS provider_kind,
       receipt_mode::text AS receipt_mode,
       heartbeat_state,
       lag_ms,
       stale,
       needs_attention AND stale AS needs_attention
FROM pg_stat_qx_scheduler_ledger_heartbeats
ORDER BY attempt_seqno, receipt_mode, worker_name, provider_kind;

SELECT provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       principal_count,
       task_count,
       submit_count,
       resume_count,
       verified_receipt_count
FROM pg_stat_qx_providers
ORDER BY provider_name;

SELECT principal_name::text AS principal_name,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       runtime_class::text AS runtime_class,
       sandbox_name::text AS sandbox_name,
       has_signer,
       task_count,
       submit_count,
       resume_count,
       verified_receipt_count
FROM pg_stat_qx_principals
ORDER BY principal_name;

SELECT runtime_class::text AS runtime_class,
       principal_count,
       provider_count,
       task_count,
       submit_count,
       resume_count,
       verified_receipt_count
FROM pg_stat_qx_runtime_classes
ORDER BY runtime_class;

SELECT pg_qx_test_start_uncheckpointed_task(:qx_container_session_oid) AS qx_stale_task_oid \gset

SELECT qxtaskname,
       qxtaskstate,
       qxtasklastcheckpointid = 0 AS has_no_checkpoint,
       qxtasklastattemptid <> 0 AS has_attempt
FROM pg_qx_task
WHERE oid = :qx_stale_task_oid;

SELECT qxattemptseqno,
       qxattemptstate,
       qxattemptresumecheckpointid = 0 AS starts_fresh,
       qxattemptstrategy
FROM pg_qx_attempt
WHERE qxattempttaskid = :qx_stale_task_oid
ORDER BY qxattemptseqno;

SELECT pg_sleep(12);

SELECT qxtaskstate,
       qxtasklastcheckpointid = 0 AS has_no_checkpoint,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE qxattempttaskid = :qx_stale_task_oid
           AND qxattemptstate = 'f'
       ) AS has_failed_attempt,
       EXISTS (
         SELECT 1
         FROM pg_stat_qx_scheduler_ledger_heartbeats
         WHERE task_oid = :qx_stale_task_oid
           AND receipt_mode = 'scheduler-renew'
       ) AS saw_scheduler_renew,
       EXISTS (
         SELECT 1
         FROM pg_stat_qx_scheduler_ledger_heartbeats
         WHERE task_oid = :qx_stale_task_oid
           AND receipt_mode = 'scheduler-reclaim'
       ) AS saw_scheduler_reclaim
FROM pg_qx_task
WHERE oid = :qx_stale_task_oid;

SELECT count(*) AS checkpoint_rows
FROM pg_qx_checkpoint
WHERE qxcheckpointtaskid = :qx_stale_task_oid;

SELECT count(*) AS retry_queue_rows
FROM pg_stat_qx_scheduler_ledger_queues
WHERE task_oid = :qx_stale_task_oid
  AND queue_kind = 1;

SELECT count(*) AS reclaimed_lease_rows
FROM pg_stat_qx_scheduler_ledger_leases
WHERE task_oid = :qx_stale_task_oid
  AND lease_state = 4;

SELECT pg_qx_scheduler_worker_slot_count() = 2 AS slot_count_is_two;

SELECT task_oid = :qx_stale_task_oid AS task_match,
       owner_slot BETWEEN 1 AND slot_count AS owner_slot_valid,
       slot_count = 2 AS slot_count_is_two,
       retry_count = 1 AS retry_count_is_one,
       priority::text = 'high' AS is_high_priority,
       retry_delay_ms >= 3000 AS backoff_applied,
       remaining_backoff_ms >= 0 AS remaining_nonnegative
FROM pg_stat_qx_scheduler_retry_backoff
WHERE task_oid = :qx_stale_task_oid;

SELECT EXISTS (
         SELECT 1
         FROM pg_stat_qx_scheduler_ledger_leases
         WHERE task_oid = :qx_stale_task_oid
           AND worker_name LIKE 'qhapaqxian scheduler db % slot %/%'
       ) AS saw_slotted_scheduler_worker;

SELECT pg_sleep(6);

SELECT qxtaskstate,
       qxtasklastcheckpointid <> 0 AS has_checkpoint,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE qxattempttaskid = :qx_stale_task_oid
           AND qxattemptseqno = 2
           AND qxattemptstate = 'k'
           AND qxattemptstrategy = 'retry'
       ) AS retry_attempt_checkpointed,
       EXISTS (
         SELECT 1
         FROM pg_qx_trace
         WHERE qxtracetaskid = :qx_stale_task_oid
           AND qxtracename = 'runtime.retry_dispatch'
       ) AS saw_retry_dispatch,
       EXISTS (
         SELECT 1
         FROM pg_qx_trace
         WHERE qxtracetaskid = :qx_stale_task_oid
           AND qxtracename = 'runtime.external_submit'
       ) AS saw_retry_submit
FROM pg_qx_task
WHERE oid = :qx_stale_task_oid;

SELECT qxattemptseqno,
       qxattemptstate,
       qxattemptresumecheckpointid = 0 AS starts_fresh,
       qxattemptstrategy
FROM pg_qx_attempt
WHERE qxattempttaskid = :qx_stale_task_oid
ORDER BY qxattemptseqno;

SELECT count(*) AS retry_queue_rows_after_retry
FROM pg_stat_qx_scheduler_ledger_queues
WHERE task_oid = :qx_stale_task_oid
  AND queue_kind = 1;

SELECT count(*) AS released_retry_lease_rows
FROM pg_stat_qx_scheduler_ledger_leases
WHERE task_oid = :qx_stale_task_oid
  AND attempt_seqno = 2
  AND lease_state = 3;

SELECT count(*) AS scheduler_release_retry_rows
FROM pg_stat_qx_scheduler_ledger_heartbeats
WHERE task_oid = :qx_stale_task_oid
  AND attempt_seqno = 2
  AND receipt_mode = 'scheduler-release';

SELECT queue_name::text AS queue_name,
       runtime_class::text AS runtime_class,
       provider_name::text AS provider_name,
       provider_kind::text AS provider_kind,
       checkpointed_task_count > 0 AS has_checkpointed_tasks,
       renew_event_count > 0 AS saw_renew_activity,
       reclaim_event_count > 0 AS saw_reclaim_activity,
       release_event_count > 0 AS saw_release_activity,
       retry_queue_snapshot_count > 0 AS saw_retry_history,
       released_lease_snapshot_count > 0 AS saw_released_leases,
       reclaimed_lease_snapshot_count > 0 AS saw_reclaimed_leases,
       max_lease_renewal_count > 0 AS saw_renewal_depth
FROM pg_stat_qx_scheduler_activity
WHERE queue_name = 'guarded:high:extractor'
  AND runtime_class = 'container'
  AND provider_name = 'container_provider';

SELECT count(*) = 2 AS saw_two_slots,
       min(owner_slot) = 1 AS has_slot_one,
       max(owner_slot) = 2 AS has_slot_two,
       max(slot_count) = 2 AS slot_count_is_two,
       bool_and(worker_name LIKE 'qhapaqxian scheduler db % slot %/%') AS names_ok,
       sum(owned_task_count) >= 1 AS saw_owned_tasks
FROM pg_stat_qx_scheduler_worker_balance
WHERE queue_name = 'guarded:high:extractor'
  AND runtime_class = 'container'
  AND provider_name = 'container_provider';

SELECT pg_qx_test_start_uncheckpointed_task_priority(:qx_container_session_oid, 'high') AS qx_fair_high_task_oid \gset
SELECT pg_qx_test_start_uncheckpointed_task_priority(:qx_container_session_oid, 'urgent') AS qx_fair_urgent_task_oid \gset

SELECT qxtaskpriority::text AS high_priority
FROM pg_qx_task
WHERE oid = :qx_fair_high_task_oid;

SELECT qxtaskpriority::text AS urgent_priority
FROM pg_qx_task
WHERE oid = :qx_fair_urgent_task_oid;

SELECT pg_sleep(12);

SELECT (SELECT qxtaskstate
        FROM pg_qx_task
        WHERE oid = :qx_fair_high_task_oid) AS high_state,
       (SELECT qxtaskstate
        FROM pg_qx_task
        WHERE oid = :qx_fair_urgent_task_oid) AS urgent_state,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE qxattempttaskid = :qx_fair_high_task_oid
           AND qxattemptstate = 'f'
       ) AS high_failed,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE qxattempttaskid = :qx_fair_urgent_task_oid
           AND qxattemptstate = 'f'
       ) AS urgent_failed;

SELECT pg_sleep(6);

SELECT (SELECT qxtaskstate
        FROM pg_qx_task
        WHERE oid = :qx_fair_high_task_oid) AS high_state,
       (SELECT qxtaskstate
        FROM pg_qx_task
        WHERE oid = :qx_fair_urgent_task_oid) AS urgent_state,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE qxattempttaskid = :qx_fair_high_task_oid
          AND qxattemptseqno = 2
       ) AS high_retried,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE qxattempttaskid = :qx_fair_urgent_task_oid
          AND qxattemptseqno = 2
       ) AS urgent_retried,
       EXISTS (
         SELECT 1
         FROM pg_qx_trace
         WHERE qxtracetaskid = :qx_fair_high_task_oid
          AND qxtracename = 'runtime.retry_dispatch'
       ) AS high_retry_dispatch,
       EXISTS (
         SELECT 1
         FROM pg_qx_trace
         WHERE qxtracetaskid = :qx_fair_urgent_task_oid
          AND qxtracename = 'runtime.retry_dispatch'
       ) AS urgent_retry_dispatch,
       COALESCE(
         (SELECT min(oid)
          FROM pg_qx_trace
          WHERE qxtracetaskid = :qx_fair_urgent_task_oid
            AND qxtracename = 'runtime.retry_dispatch') <
         (SELECT min(oid)
          FROM pg_qx_trace
          WHERE qxtracetaskid = :qx_fair_high_task_oid
            AND qxtracename = 'runtime.retry_dispatch'),
         false
       ) AS urgent_dispatched_first;

SELECT 'high'::text AS task_bucket,
       qxattemptseqno,
       qxattemptstate,
       qxattemptstrategy::text AS qxattemptstrategy
FROM pg_qx_attempt
WHERE qxattempttaskid = :qx_fair_high_task_oid
UNION ALL
SELECT 'urgent'::text AS task_bucket,
       qxattemptseqno,
       qxattemptstate,
       qxattemptstrategy::text AS qxattemptstrategy
FROM pg_qx_attempt
WHERE qxattempttaskid = :qx_fair_urgent_task_oid
ORDER BY task_bucket, qxattemptseqno;

SELECT pg_qx_test_start_exhausted_retry_task(:qx_container_session_oid) AS qx_deadletter_task_oid \gset

SELECT pg_sleep(6);

SELECT qxtaskstate AS deadletter_state,
       qxtasklastcheckpointid = 0 AS no_checkpoint,
       EXISTS (
         SELECT 1
         FROM pg_qx_attempt
         WHERE qxattempttaskid = :qx_deadletter_task_oid
           AND qxattemptseqno = 1
           AND qxattemptstate = 'f'
       ) AS failed_attempt_recorded,
       EXISTS (
         SELECT 1
         FROM pg_qx_trace
         WHERE qxtracetaskid = :qx_deadletter_task_oid
           AND qxtracename = 'runtime.dead_letter'
       ) AS saw_dead_letter_trace,
       NOT EXISTS (
         SELECT 1
         FROM pg_qx_trace
         WHERE qxtracetaskid = :qx_deadletter_task_oid
           AND qxtracename = 'runtime.retry_dispatch'
       ) AS retry_dispatch_blocked
FROM pg_qx_task
WHERE oid = :qx_deadletter_task_oid;

SELECT count(*) AS deadletter_queue_rows
FROM pg_stat_qx_scheduler_ledger_queues
WHERE task_oid = :qx_deadletter_task_oid
  AND queue_kind = 3
  AND runnable_count = 0
  AND blocked_count = 1
  AND retry_count = 3;

SELECT task_state, has_checkpoint
FROM pg_stat_qx_tasks
WHERE task_oid = :qx_deadletter_task_oid;

CREATE ROLE qx_observer LOGIN;

SET SESSION AUTHORIZATION qx_observer;

SELECT current_user, session_user;

SELECT count(*) AS visible_agents
FROM pg_stat_qx_agents;

SELECT count(*) AS visible_sessions
FROM pg_stat_qx_sessions;

SELECT count(*) AS visible_tasks
FROM pg_stat_qx_tasks;

SELECT count(*) AS visible_scheduler_queues
FROM pg_stat_qx_scheduler_queues;

SELECT count(*) AS visible_scheduler_workers
FROM pg_stat_qx_scheduler_workers;

SELECT count(*) AS visible_scheduler_activity
FROM pg_stat_qx_scheduler_activity;

SELECT count(*) AS visible_scheduler_ledger_queues
FROM pg_stat_qx_scheduler_ledger_queues;

SELECT count(*) AS visible_scheduler_ledger_leases
FROM pg_stat_qx_scheduler_ledger_leases;

SELECT count(*) AS visible_scheduler_ledger_heartbeats
FROM pg_stat_qx_scheduler_ledger_heartbeats;

SELECT count(*) AS visible_providers
FROM pg_stat_qx_providers;

SELECT count(*) AS visible_principals
FROM pg_stat_qx_principals;

SELECT count(*) AS visible_runtime_classes
FROM pg_stat_qx_runtime_classes;

SELECT has_table_privilege(current_user, 'pg_qx_memory', 'SELECT') AS can_select_memory;

SELECT count(*) FROM pg_qx_memory;

RESET SESSION AUTHORIZATION;

DROP ROLE qx_observer;

CREATE NAMESPACE POLICY host_runtime_only FOR SCHEMA public
  TOOLS (summarize)
  USING 'require_capabilities=runtime:host'
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;

CREATE AGENT host_runtime_probe
  IDENTITY imperial
  MODEL 'openai:gpt-5.4-mini'
  TOOLS (summarize)
  POLICY host_runtime_only
  BUDGET (tokens 256, cost 32);

CREATE NAMESPACE POLICY no_microvm_runtime FOR SCHEMA public
  TOOLS (summarize)
  USING 'deny_capabilities=runtime:microvm'
  KNOWN TOOLS ENABLE
  BUDGET ENABLE;

CREATE AGENT no_microvm_probe
  IDENTITY imperial
  MODEL 'openai:gpt-5.4-mini'
  TOOLS (summarize)
  POLICY no_microvm_runtime
  BUDGET (tokens 256, cost 32);

START SESSION FOR AGENT archivist
  RETURNING SESSION;

START SESSION FOR AGENT missing_agent;

RUN TASK IN SESSION 42
  GOAL 'index docs';

RUN TASK summarize_docs IN SESSION current_setting('server_version_num')
  GOAL 'reject dynamic session expr';

RESUME TASK 42;

RESUME TASK current_setting('server_version_num');

REMEMBER IN SESSION current_setting('server_version_num')
  SCOPE semantic
  KEY 'reject.dynamic'
  VALUE 'nope';

FETCH MEMORY FOR AGENT missing_agent;

SHOW TRACE FOR TASK current_setting('server_version_num');

RUN TASK summarize_docs IN SESSION :qx_session_oid
  GOAL 'summarize docs'
  INPUT jsonb_build_object('topic', 'parser')
  PRIORITY high
  RETURNING TASK;
