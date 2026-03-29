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
#include "catalog/pg_qx_task.h"
#include "catalog/pg_qx_trace.h"
#include "catalog/pg_type.h"
#include "commands/tracecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid qx_extract_oid_literal(Node *expr, const char *subject,
								  const char *detail, const char *hint);
static const char *qx_trace_state_label(char state);
static char *qx_get_trace_text_attr(TupleDesc tupdesc, HeapTuple tup,
									AttrNumber attnum);

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

TupleDesc
ShowTraceResultDesc(void)
{
	TupleDesc	tupdesc;

	tupdesc = CreateTemplateTupleDesc(5);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "trace_state",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "trace_name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "trace_detail",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "has_step",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "has_semantic_lsn",
					   BOOLOID, -1, 0);

	return tupdesc;
}

void
ShowTraceCommand(ShowTraceStmt *stmt, DestReceiver *dest)
{
	Oid			taskoid;
	HeapTuple	tasktup;
	Form_pg_qx_task taskform;
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

	tasktup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tasktup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("task %u does not exist", taskoid)));

	taskform = (Form_pg_qx_task) GETSTRUCT(tasktup);
	if (!has_privs_of_role(GetUserId(), taskform->qxtaskowner))
	{
		ReleaseSysCache(tasktup);
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
		Datum		values[5];
		bool		nulls[5];
		char	   *name;
		char	   *detail;

		if (form->qxtracedbid != MyDatabaseId)
			continue;

		name = qx_get_trace_text_attr(RelationGetDescr(rel), tup,
									  Anum_pg_qx_trace_qxtracename);
		detail = qx_get_trace_text_attr(RelationGetDescr(rel), tup,
										Anum_pg_qx_trace_qxtracedetail);

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));

		values[0] = CStringGetTextDatum(qx_trace_state_label(form->qxtracestate));
		if (name != NULL)
			values[1] = CStringGetTextDatum(name);
		else
			nulls[1] = true;
		if (detail != NULL)
			values[2] = CStringGetTextDatum(detail);
		else
			nulls[2] = true;
		values[3] = BoolGetDatum(OidIsValid(form->qxtracestepid));
		values[4] = BoolGetDatum(form->qxtracelsn != InvalidXLogRecPtr);

		do_tup_output(tstate, values, nulls);
		emitted++;

		if (name != NULL)
			pfree(name);
		if (detail != NULL)
			pfree(detail);

		if (emitted >= stmt->limit_count)
			break;
	}

	end_tup_output(tstate);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
	ReleaseSysCache(tasktup);
}
