/*-------------------------------------------------------------------------
 *
 * qx_semantic_log.c
 *	  semantic WAL/logical message helpers for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/replication/qx_semantic_log.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "qx/qx_semantic_log.h"
#include "replication/message.h"
#include "utils/json.h"

#define QX_SEMANTIC_PREFIX "qhapaqxian"
#define QX_SEMANTIC_SCHEMA "stage9.semantic.v1"

static void qx_json_append_string_field(StringInfo buf, const char *key,
										const char *value, bool trailing_comma);
static void qx_json_append_oid_field(StringInfo buf, const char *key, Oid value,
									 bool trailing_comma);
static void qx_json_append_char_field(StringInfo buf, const char *key, char value,
									  bool trailing_comma);
static XLogRecPtr qx_emit_semantic_json(StringInfo buf);

static void
qx_json_append_string_field(StringInfo buf, const char *key, const char *value,
							bool trailing_comma)
{
	appendStringInfo(buf, "\"%s\":", key);

	if (value == NULL)
		appendStringInfoString(buf, "null");
	else
		escape_json(buf, value);

	if (trailing_comma)
		appendStringInfoChar(buf, ',');
}

static void
qx_json_append_oid_field(StringInfo buf, const char *key, Oid value,
						 bool trailing_comma)
{
	appendStringInfo(buf, "\"%s\":%u", key, value);
	if (trailing_comma)
		appendStringInfoChar(buf, ',');
}

static void
qx_json_append_char_field(StringInfo buf, const char *key, char value,
						  bool trailing_comma)
{
	appendStringInfo(buf, "\"%s\":\"%c\"", key, value);
	if (trailing_comma)
		appendStringInfoChar(buf, ',');
}

static XLogRecPtr
qx_emit_semantic_json(StringInfo buf)
{
	return LogLogicalMessage(QX_SEMANTIC_PREFIX, buf->data, buf->len, true, false);
}

XLogRecPtr
QxEmitSemanticEventRecord(Oid eventoid, Oid sessionoid, Oid taskoid, Oid stepoid,
						  Oid ownerid, const char *kind, const char *payload)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '{');
	qx_json_append_string_field(&buf, "schema", QX_SEMANTIC_SCHEMA, true);
	qx_json_append_string_field(&buf, "record_kind", "event", true);
	qx_json_append_string_field(&buf, "channel", "qx.event", true);
	qx_json_append_oid_field(&buf, "record_oid", eventoid, true);
	qx_json_append_oid_field(&buf, "session_oid", sessionoid, true);
	qx_json_append_oid_field(&buf, "task_oid", taskoid, true);
	qx_json_append_oid_field(&buf, "step_oid", stepoid, true);
	qx_json_append_oid_field(&buf, "owner_oid", ownerid, true);
	qx_json_append_string_field(&buf, "kind", kind, true);
	qx_json_append_string_field(&buf, "payload", payload, false);
	appendStringInfoChar(&buf, '}');

	return qx_emit_semantic_json(&buf);
}

XLogRecPtr
QxEmitSemanticTraceRecord(Oid traceoid, Oid sessionoid, Oid taskoid, Oid stepoid,
						  Oid ownerid, char state, const char *name,
						  const char *detail)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '{');
	qx_json_append_string_field(&buf, "schema", QX_SEMANTIC_SCHEMA, true);
	qx_json_append_string_field(&buf, "record_kind", "trace", true);
	qx_json_append_string_field(&buf, "channel", "qx.trace", true);
	qx_json_append_oid_field(&buf, "record_oid", traceoid, true);
	qx_json_append_oid_field(&buf, "session_oid", sessionoid, true);
	qx_json_append_oid_field(&buf, "task_oid", taskoid, true);
	qx_json_append_oid_field(&buf, "step_oid", stepoid, true);
	qx_json_append_oid_field(&buf, "owner_oid", ownerid, true);
	qx_json_append_char_field(&buf, "state", state, true);
	qx_json_append_string_field(&buf, "name", name, true);
	qx_json_append_string_field(&buf, "detail", detail, false);
	appendStringInfoChar(&buf, '}');

	return qx_emit_semantic_json(&buf);
}

XLogRecPtr
QxEmitSemanticCheckpointRecord(Oid checkpointoid, Oid sessionoid, Oid taskoid,
							   Oid attemptoid, Oid stepoid, Oid ownerid,
							   char checkpoint_state, char task_state,
							   int16 nextstepseqno, const char *label,
							   const char *data)
{
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfoChar(&buf, '{');
	qx_json_append_string_field(&buf, "schema", QX_SEMANTIC_SCHEMA, true);
	qx_json_append_string_field(&buf, "record_kind", "checkpoint", true);
	qx_json_append_string_field(&buf, "channel", "qx.checkpoint", true);
	qx_json_append_oid_field(&buf, "record_oid", checkpointoid, true);
	qx_json_append_oid_field(&buf, "session_oid", sessionoid, true);
	qx_json_append_oid_field(&buf, "task_oid", taskoid, true);
	qx_json_append_oid_field(&buf, "attempt_oid", attemptoid, true);
	qx_json_append_oid_field(&buf, "step_oid", stepoid, true);
	qx_json_append_oid_field(&buf, "owner_oid", ownerid, true);
	qx_json_append_char_field(&buf, "checkpoint_state", checkpoint_state, true);
	qx_json_append_char_field(&buf, "task_state", task_state, true);
	appendStringInfo(&buf, "\"next_step_seqno\":%d,", nextstepseqno);
	qx_json_append_string_field(&buf, "label", label, true);
	qx_json_append_string_field(&buf, "data", data, false);
	appendStringInfoChar(&buf, '}');

	return qx_emit_semantic_json(&buf);
}
