SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('qx_semantic_slot', 'qhapaqxian_output');

CREATE AGENT archivist
  IDENTITY imperial
  MODEL 'openai:gpt-5.4'
  MEMORY PROFILE episodic
  TOOLS (search, summarize)
  POLICY guarded
  BUDGET (tokens 4096, cost 128);

START SESSION FOR AGENT archivist
  WITH CONTEXT jsonb_build_object('mission', 'index');

SELECT oid AS qx_session_oid
FROM pg_qx_session
ORDER BY oid DESC
LIMIT 1
\gset

RUN TASK summarize_docs IN SESSION :qx_session_oid
  GOAL 'summarize docs'
  INPUT jsonb_build_object('topic', 'parser')
  PRIORITY high;

SELECT oid AS qx_task_oid
FROM pg_qx_task
ORDER BY oid DESC
LIMIT 1
\gset

RESUME TASK :qx_task_oid FROM CHECKPOINT 'stage8.after_capture';

WITH decoded AS (
  SELECT lsn, xid, data::jsonb AS msg
  FROM pg_logical_slot_peek_changes('qx_semantic_slot', NULL, NULL)
)
SELECT count(*) AS total_messages,
       count(*) FILTER (WHERE msg->>'schema' = 'stage9.semantic.v1') AS schema_v1_messages
FROM decoded;

WITH decoded AS (
  SELECT data::jsonb AS msg
  FROM pg_logical_slot_peek_changes('qx_semantic_slot', NULL, NULL)
)
SELECT msg->>'record_kind' AS record_kind,
       count(*) AS nrecords
FROM decoded
GROUP BY 1
ORDER BY 1;

SELECT msg->>'kind' AS event_kind
FROM (
  SELECT data::jsonb AS msg
  FROM pg_logical_slot_peek_changes('qx_semantic_slot', NULL, NULL)
) decoded
WHERE msg->>'record_kind' = 'event'
ORDER BY event_kind;

WITH decoded AS (
  SELECT data::jsonb AS msg
  FROM pg_logical_slot_get_changes('qx_semantic_slot', NULL, NULL)
)
SELECT count(*) AS drained_messages
FROM decoded;

WITH decoded AS (
  SELECT data::jsonb AS msg
  FROM pg_logical_slot_get_changes('qx_semantic_slot', NULL, NULL)
)
SELECT count(*) AS get_after_drain
FROM decoded;

SELECT 'cleanup' FROM pg_drop_replication_slot('qx_semantic_slot');
