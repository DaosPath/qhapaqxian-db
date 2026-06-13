/*-------------------------------------------------------------------------
 *
 * tracecmds.c
 *	  trace command entry points for QhapaqXian DB
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/commands/tracecmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/pg_qx_trace.h"
#include "catalog/pg_type.h"
#include "commands/tracecmds.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "qx/qx_catalog.h"
#include "qx/qx_observe.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"

static Oid qx_extract_oid_literal(Node *expr, const char *subject,
								  const char *detail, const char *hint);
static const char *qx_trace_state_label(char state);
static char *qx_get_trace_text_attr(TupleDesc tupdesc, HeapTuple tup,
									AttrNumber attnum);
static char *qx_replace_trace_fragment(const char *source, const char *needle,
										 const char *replacement);
static char *qx_replace_trace_numeric_value(const char *source, const char *key,
											 const char *replacement);
static char *qx_normalize_trace_detail(const char *detail, Oid taskoid);

static Oid
qx_extract_oid_literal(Node *expr, const char *subject, const char *detail,
					   const char *hint)
{
	A_Const    *con;
	int32		value;

	if (!IsA(expr, A_Const))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s currently requires a literal identifier", subject),
				 errdetail("%s", detail),
				 errhint("%s", hint)));

	con = castNode(A_Const, expr);
	if (nodeTag(&con->val) != T_Integer)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s identifier must be an integer literal", subject)));

	value = intVal(&con->val);
	if (value <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s identifier must be greater than zero", subject)));

	return (Oid) value;
}

static const char *
qx_trace_state_label(char state)
{
	switch (state)
	{
		case QX_TRACE_STATE_OPEN:
			return "open";
		case QX_TRACE_STATE_CLOSED:
			return "closed";
	}

	return "unknown";
}

static char *
qx_get_trace_text_attr(TupleDesc tupdesc, HeapTuple tup, AttrNumber attnum)
{
	bool		isnull;
	Datum		datum;

	datum = heap_getattr(tup, attnum, tupdesc, &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static char *
qx_replace_trace_fragment(const char *source, const char *needle,
						  const char *replacement)
{
	const char *cursor;
	const char *match;
	char	   *result;
	char	   *writeptr;
	size_t		source_len;
	size_t		needle_len;
	size_t		replacement_len;
	size_t		match_count = 0;
	size_t		result_len;

	if (source == NULL || needle == NULL || replacement == NULL)
		return NULL;

	needle_len = strlen(needle);
	if (needle_len == 0)
		return pstrdup(source);

	replacement_len = strlen(replacement);
	source_len = strlen(source);
	cursor = source;
	while ((match = strstr(cursor, needle)) != NULL)
	{
		match_count++;
		cursor = match + needle_len;
	}

	if (match_count == 0)
		return pstrdup(source);

	result_len = source_len + match_count * (replacement_len - needle_len);
	result = palloc(result_len + 1);
	writeptr = result;
	cursor = source;
	while ((match = strstr(cursor, needle)) != NULL)
	{
		size_t		prefix_len = match - cursor;

		memcpy(writeptr, cursor, prefix_len);
		writeptr += prefix_len;
		memcpy(writeptr, replacement, replacement_len);
		writeptr += replacement_len;
		cursor = match + needle_len;
	}

	strcpy(writeptr, cursor);
	return result;
}

static char *
qx_replace_trace_numeric_value(const char *source, const char *key,
							   const char *replacement)
{
	const char *match;
	const char *value_start;
	const char *value_end;
	StringInfoData rewritten;

	if (source == NULL || key == NULL || replacement == NULL)
		return NULL;

	match = strstr(source, key);
	if (match == NULL)
		return pstrdup(source);

	value_start = match + strlen(key);
	value_end = value_start;
	while (*value_end >= '0' && *value_end <= '9')
		value_end++;

	if (value_end == value_start)
		return pstrdup(source);

	initStringInfo(&rewritten);
	appendBinaryStringInfo(&rewritten, source, match - source);
	appendStringInfoString(&rewritten, key);
	appendStringInfoString(&rewritten, replacement);
	appendStringInfoString(&rewritten, value_end);

	return rewritten.data;
}

static char *
qx_replace_trace_delimited_value(const char *source, const char *key,
								 const char *replacement)
{
	const char *match;
	const char *value_start;
	const char *value_end;
	StringInfoData rewritten;

	if (source == NULL || key == NULL || replacement == NULL)
		return NULL;

	match = strstr(source, key);
	if (match == NULL)
		return pstrdup(source);

	value_start = match + strlen(key);
	value_end = value_start;
	while (*value_end != '\0' && *value_end != ';')
		value_end++;

	if (value_end == value_start)
		return pstrdup(source);

	initStringInfo(&rewritten);
	appendBinaryStringInfo(&rewritten, source, match - source);
	appendStringInfoString(&rewritten, key);
	appendStringInfoString(&rewritten, replacement);
	appendStringInfoString(&rewritten, value_end);

	return rewritten.data;
}

static char *
qx_replace_trace_delimited_value_all(const char *source, const char *key,
									 const char *replacement)
{
	const char *cursor;
	StringInfoData rewritten;
	bool		changed = false;
	size_t		key_len;

	if (source == NULL || key == NULL || replacement == NULL)
		return NULL;

	key_len = strlen(key);
	cursor = source;
	initStringInfo(&rewritten);

	while (*cursor != '\0')
	{
		const char *match = strstr(cursor, key);
		const char *value_start;
		const char *value_end;
		size_t		value_len;

		if (match == NULL)
		{
			appendStringInfoString(&rewritten, cursor);
			break;
		}

		appendBinaryStringInfo(&rewritten, cursor, match - cursor);
		appendStringInfoString(&rewritten, key);
		value_start = match + key_len;
		value_end = value_start;
		while (*value_end != '\0' && *value_end != ';')
			value_end++;

		value_len = value_end - value_start;
		if (value_len == 0 ||
			(value_len == strlen(replacement) &&
			 strncmp(value_start, replacement, value_len) == 0))
		{
			appendBinaryStringInfo(&rewritten, value_start, value_len);
		}
		else
		{
			appendStringInfoString(&rewritten, replacement);
			changed = true;
		}

		appendStringInfoString(&rewritten, value_end);
		cursor = value_end;
	}

	if (!changed)
		return pstrdup(source);

	return rewritten.data;
}

static char *
qx_normalize_trace_detail(const char *detail, Oid taskoid)
{
	char	   *task_fragment;
	char	   *task_equals_fragment;
	char	   *normalized;
	char	   *rewritten;
	char	   *microvm_accel;
	char	   *microvm_kernel;
	char	   *container_id;
	char	   *vm_id;
	char	   *restricted_identity;
	char	   *final_detail;

	if (detail == NULL)
		return NULL;

	task_fragment = psprintf("task %u", taskoid);
	task_equals_fragment = psprintf("task=%u", taskoid);
	normalized = qx_replace_trace_fragment(detail, task_fragment, "task <task>");
	rewritten = qx_replace_trace_fragment(normalized, task_equals_fragment,
										  "task=<task>");
	microvm_accel = qx_replace_trace_delimited_value(rewritten,
													 "microvm_accel=",
													 "<accel>");
	microvm_kernel = qx_replace_trace_delimited_value(microvm_accel,
													  "microvm_kernel=",
													  "<kernel>");
	container_id = qx_replace_trace_delimited_value_all(microvm_kernel,
													  "container_id=",
													  "<container_id>");
	vm_id = qx_replace_trace_delimited_value_all(container_id,
												 "vm_id=",
												 "<vm_id>");
	restricted_identity = qx_replace_trace_delimited_value(vm_id,
														   "restricted_identity=",
														   "<identity>");
	final_detail = qx_replace_trace_numeric_value(restricted_identity, "wall_ms=", "<ms>");
	pfree(task_fragment);
	pfree(task_equals_fragment);
	pfree(normalized);
	pfree(rewritten);
	pfree(microvm_accel);
	pfree(microvm_kernel);
	pfree(container_id);
	pfree(vm_id);
	pfree(restricted_identity);

	return final_detail;
}

TupleDesc
ShowTraceResultDesc(void)
{
	TupleDesc	tupdesc;

	tupdesc = CreateTemplateTupleDesc(6);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "trace_state",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "trace_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "trace_detail",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "trace_summary",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "has_step",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "has_semantic_lsn",
					   BOOLOID, -1, 0);

	return tupdesc;
}

void
ShowTraceCommand(ShowTraceStmt *stmt, DestReceiver *dest)
{
	Oid			taskoid;
	QxCatalogTaskInfo task;
	Relation	rel;
	ScanKeyData scankey[1];
	SysScanDesc scan;
	HeapTuple	tup;
	TupOutputState *tstate;
	int			emitted = 0;

	taskoid = qx_extract_oid_literal(
		stmt->task_id,
		"SHOW TRACE",
		"Stage 10 exposes engine-owned trace rows directly, but task identifiers still require literal OID input on the utility path.",
		"Pass a numeric task OID literal, or resolve the task OID in the client before invoking SHOW TRACE.");

	if (!QxCatalogLookupTaskByOid(taskoid, &task))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("task %u does not exist", taskoid)));

	if (!has_privs_of_role(GetUserId(), task.ownerid))
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to inspect trace for task %u", taskoid),
				 errdetail("Only the task owner or a member of that role may read the engine-owned trace stream.")));
	}

	rel = table_open(QxTraceRelationId, AccessShareLock);
	ScanKeyInit(&scankey[0],
				Anum_pg_qx_trace_qxtracetaskid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(taskoid));
	scan = systable_beginscan(rel, QxTraceTaskIdIndexId, true, NULL, 1, scankey);
	tstate = begin_tup_output_tupdesc(dest, ShowTraceResultDesc(),
									  &TTSOpsVirtual);

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_qx_trace form = (Form_pg_qx_trace) GETSTRUCT(tup);
		Datum		values[6];
		bool		nulls[6];
		char	   *name;
		char	   *detail;
		char	   *normalized_detail;
		char	   *summary;

		if (form->qxtracedbid != MyDatabaseId)
			continue;

		name = qx_get_trace_text_attr(RelationGetDescr(rel), tup,
									  Anum_pg_qx_trace_qxtracename);
		detail = qx_get_trace_text_attr(RelationGetDescr(rel), tup,
										Anum_pg_qx_trace_qxtracedetail);
		normalized_detail = qx_normalize_trace_detail(detail, taskoid);
		summary = QxObserveSummarizeTraceDetail(name, normalized_detail);

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));

		values[0] = CStringGetTextDatum(qx_trace_state_label(form->qxtracestate));
		if (name != NULL)
			values[1] = CStringGetTextDatum(name);
		else
			nulls[1] = true;
		if (normalized_detail != NULL)
			values[2] = CStringGetTextDatum(normalized_detail);
		else
			nulls[2] = true;
		if (summary != NULL)
			values[3] = CStringGetTextDatum(summary);
		else
			nulls[3] = true;
		values[4] = BoolGetDatum(OidIsValid(form->qxtracestepid));
		values[5] = BoolGetDatum(form->qxtracelsn != InvalidXLogRecPtr);

		do_tup_output(tstate, values, nulls);
		emitted++;

		if (name != NULL)
			pfree(name);
		if (detail != NULL)
			pfree(detail);
		if (normalized_detail != NULL)
			pfree(normalized_detail);
		if (summary != NULL)
			pfree(summary);

		if (emitted >= stmt->limit_count)
			break;
	}

	end_tup_output(tstate);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	QxCatalogFreeTaskInfo(&task);
}
