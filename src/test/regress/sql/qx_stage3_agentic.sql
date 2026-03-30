-- QhapaqXian DB Stage 22 brokered container/microvm principal slice

CREATE PROVIDER public.loopback_provider
  KIND 'loopback'
  ENDPOINT 'local://qhapaqxian-tool-runner'
  RECEIPT KEY 'loopback-stage20-key'
  ATTESTATION ENABLE;

CREATE PROVIDER public.container_provider
  KIND 'container'
  ENDPOINT 'container://broker/pool'
  USING 'ed25519'
  RECEIPT KEY $$-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEATQR0xvmOgsHyUp6bWkQ7xlKHg40piOubNdB+Ew80TOs=
-----END PUBLIC KEY-----$$
  ATTESTATION ENABLE;

CREATE PROVIDER public.microvm_provider
  KIND 'microvm'
  ENDPOINT 'microvm://broker/primary'
  USING 'ed25519'
  RECEIPT KEY $$-----BEGIN PUBLIC KEY-----
MCowBQYDK2VwAyEATQR0xvmOgsHyUp6bWkQ7xlKHg40piOubNdB+Ew80TOs=
-----END PUBLIC KEY-----$$
  ATTESTATION ENABLE;

ALTER PROVIDER public.loopback_provider
  ENDPOINT 'local://qhapaqxian-tool-runner'
  RECEIPT KEY 'loopback-stage20-key'
  ATTESTATION ENABLE
  ENABLE;

CREATE PRINCIPAL public.search_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'restricted'
  RUNTIME 'host'
  PROVIDER public.loopback_provider;

CREATE PRINCIPAL public.extract_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'isolated'
  RUNTIME 'container'
  PROVIDER public.container_provider
  SIGNER 'qx_remote_ed25519_private.pem';

CREATE PRINCIPAL public.summarize_runner
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'isolated'
  RUNTIME 'microvm'
  PROVIDER public.microvm_provider
  SIGNER 'qx_remote_ed25519_private.pem';

ALTER PRINCIPAL public.search_runner
  PROVIDER public.loopback_provider
  PROGRAM 'qhapaqxian-tool-runner'
  SANDBOX 'restricted'
  RUNTIME 'host';

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
       qxtracedetail LIKE '%wall_ms=%' AS has_wall_time
FROM pg_qx_trace
WHERE qxtracename IN ('runtime.external_submit', 'runtime.external_resume')
ORDER BY oid;

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
       checkpoint_count
FROM pg_stat_qx_tasks
ORDER BY task_oid;

CREATE ROLE qx_observer LOGIN;

SET SESSION AUTHORIZATION qx_observer;

SELECT current_user, session_user;

SELECT count(*) AS visible_agents
FROM pg_stat_qx_agents;

SELECT count(*) AS visible_sessions
FROM pg_stat_qx_sessions;

SELECT count(*) AS visible_tasks
FROM pg_stat_qx_tasks;

SELECT has_table_privilege(current_user, 'pg_qx_memory', 'SELECT') AS can_select_memory;

SELECT count(*) FROM pg_qx_memory;

RESET SESSION AUTHORIZATION;

DROP ROLE qx_observer;

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
