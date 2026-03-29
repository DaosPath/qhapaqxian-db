-- QhapaqXian DB Stage 11 operability and compatibility slice

CREATE AGENT archivist
  IDENTITY imperial
  MODEL 'openai:gpt-5.4'
  MEMORY PROFILE episodic
  TOOLS (search, summarize)
  POLICY guarded
  BUDGET (tokens 4096, cost 12);

SELECT qxagentname::text AS agent_name,
       qxagentnamespace = 'public'::regnamespace AS in_public_schema,
       qxagentowner = (SELECT oid FROM pg_roles WHERE rolname = current_user) AS owned_by_current_user,
       qxidentity::text AS identity_name,
       qxmodeluri::text AS model_uri,
       qxmemoryprofile::text AS memory_profile,
       qxpolicy::text AS policy_name,
       qxtools IS NOT NULL AS has_tools,
       qxbudget IS NOT NULL AS has_budget
FROM pg_qx_agent
WHERE qxagentname = 'archivist';

CREATE AGENT archivist
  IDENTITY imperial;

START SESSION FOR AGENT archivist
  WITH CONTEXT jsonb_build_object('mission', 'index');

SELECT qxsessionagentid = (SELECT oid FROM pg_qx_agent WHERE qxagentname = 'archivist') AS linked_agent,
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
       qxtaskinput IS NOT NULL AS has_input,
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

SELECT agent_name::text AS agent_name,
       active_session_count,
       task_count,
       completed_task_count,
       memory_count
FROM pg_stat_qx_agents
ORDER BY agent_name;

SELECT session_oid = :qx_session_oid AS session_match,
       agent_name::text AS agent_name,
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
       task_name::text AS task_name,
       task_state,
       has_attempt,
       has_checkpoint,
       attempt_count,
       step_count,
       event_count,
       trace_count,
       checkpoint_count
FROM pg_stat_qx_tasks
ORDER BY task_oid;

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
