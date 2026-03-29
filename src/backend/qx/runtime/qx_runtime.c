/*-------------------------------------------------------------------------
 *
 * qx_runtime.c
 *	  Stage 7 embedded runtime entry points for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/runtime/qx_runtime.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_attempt.h"
#include "catalog/pg_qx_checkpoint.h"
#include "catalog/pg_qx_event.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_step.h"
#include "catalog/pg_qx_task.h"
#include "catalog/pg_qx_trace.h"
#include "miscadmin.h"
#include "qx/qx_runtime.h"
#include "qx/qx_semantic_log.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void
qx_set_text_datum(Datum *values, bool *nulls, AttrNumber attnum,
				  const char *value)
{
	if (value == NULL)
	{
		nulls[attnum - 1] = true;
		return;
	}

	values[attnum - 1] = CStringGetTextDatum(value);
}

static void
qx_set_nodetree_datum(Datum *values, bool *nulls, AttrNumber attnum,
					  const void *node)
{
	char	   *serialized;

	if (node == NULL)
	{
		nulls[attnum - 1] = true;
		return;
	}

	serialized = nodeToString(node);
	values[attnum - 1] = CStringGetTextDatum(serialized);
	pfree(serialized);
}

static void
qx_update_task_runtime(Relation taskrel, Oid taskoid, char state,
					   Oid lastattemptid, bool replace_attempt,
					   Oid lastcheckpointid, bool replace_checkpoint)
{
	HeapTuple	tasktup;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_task];
	bool		nulls[Natts_pg_qx_task];
	bool		replaces[Natts_pg_qx_task];

	tasktup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tasktup))
		elog(ERROR, "cache lookup failed for QhapaqXian task %u", taskoid);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[Anum_pg_qx_task_qxtaskstate - 1] = CharGetDatum(state);
	replaces[Anum_pg_qx_task_qxtaskstate - 1] = true;

	if (replace_attempt)
	{
		values[Anum_pg_qx_task_qxtasklastattemptid - 1] =
			ObjectIdGetDatum(lastattemptid);
		replaces[Anum_pg_qx_task_qxtasklastattemptid - 1] = true;
	}

	if (replace_checkpoint)
	{
		values[Anum_pg_qx_task_qxtasklastcheckpointid - 1] =
			ObjectIdGetDatum(lastcheckpointid);
		replaces[Anum_pg_qx_task_qxtasklastcheckpointid - 1] = true;
	}

	newtup = heap_modify_tuple(tasktup, RelationGetDescr(taskrel),
							   values, nulls, replaces);
	CatalogTupleUpdate(taskrel, &tasktup->t_self, newtup);

	heap_freetuple(newtup);
	ReleaseSysCache(tasktup);
	CommandCounterIncrement();
}

static void
qx_update_attempt_state(Relation attemptrel, Oid attemptoid, char state)
{
	HeapTuple	attempttup;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_attempt];
	bool		nulls[Natts_pg_qx_attempt];
	bool		replaces[Natts_pg_qx_attempt];

	attempttup = SearchSysCache1(QXATTEMPTOID, ObjectIdGetDatum(attemptoid));
	if (!HeapTupleIsValid(attempttup))
		elog(ERROR, "cache lookup failed for QhapaqXian attempt %u", attemptoid);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[Anum_pg_qx_attempt_qxattemptstate - 1] = CharGetDatum(state);
	replaces[Anum_pg_qx_attempt_qxattemptstate - 1] = true;

	newtup = heap_modify_tuple(attempttup, RelationGetDescr(attemptrel),
							   values, nulls, replaces);
	CatalogTupleUpdate(attemptrel, &attempttup->t_self, newtup);

	heap_freetuple(newtup);
	ReleaseSysCache(attempttup);
	CommandCounterIncrement();
}

static Oid
qx_insert_attempt(Relation rel, Oid sessionoid, Oid taskoid, Oid ownerid,
				  Oid resumecheckpointid, int16 seqno, char state,
				  const char *strategy)
{
	Datum		values[Natts_pg_qx_attempt];
	bool		nulls[Natts_pg_qx_attempt];
	Oid			attemptoid;
	HeapTuple	tup;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	attemptoid = GetNewOidWithIndex(rel, QxAttemptOidIndexId,
									Anum_pg_qx_attempt_oid);
	values[Anum_pg_qx_attempt_oid - 1] = ObjectIdGetDatum(attemptoid);
	values[Anum_pg_qx_attempt_qxattemptdbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_attempt_qxattemptsessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_attempt_qxattempttaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_attempt_qxattemptowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_attempt_qxattemptresumecheckpointid - 1] =
		ObjectIdGetDatum(resumecheckpointid);
	values[Anum_pg_qx_attempt_qxattemptseqno - 1] = Int16GetDatum(seqno);
	values[Anum_pg_qx_attempt_qxattemptstate - 1] = CharGetDatum(state);
	qx_set_text_datum(values, nulls, Anum_pg_qx_attempt_qxattemptstrategy,
					  strategy);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return attemptoid;
}

static Oid
qx_insert_step(Relation rel, Oid sessionoid, Oid taskoid, int16 seqno,
			   const char *name, const char *detail)
{
	Datum		values[Natts_pg_qx_step];
	bool		nulls[Natts_pg_qx_step];
	Oid			stepoid;
	HeapTuple	tup;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	stepoid = GetNewOidWithIndex(rel, QxStepOidIndexId,
								 Anum_pg_qx_step_oid);
	values[Anum_pg_qx_step_oid - 1] = ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_step_qxstepdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_step_qxstepsessionid - 1] = ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_step_qxsteptaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_step_qxstepseqno - 1] = Int16GetDatum(seqno);
	values[Anum_pg_qx_step_qxstepstate - 1] =
		CharGetDatum(QX_STEP_STATE_COMPLETED);
	qx_set_text_datum(values, nulls, Anum_pg_qx_step_qxstepname, name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_step_qxstepdetail, detail);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return stepoid;
}

static Oid
qx_insert_event(Relation rel, Oid sessionoid, Oid taskoid, Oid stepoid,
				Oid ownerid, const char *kind, const char *payload)
{
	Datum		values[Natts_pg_qx_event];
	bool		nulls[Natts_pg_qx_event];
	Oid			eventoid;
	HeapTuple	tup;
	XLogRecPtr	eventlsn;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	eventoid = GetNewOidWithIndex(rel, QxEventOidIndexId,
								  Anum_pg_qx_event_oid);
	values[Anum_pg_qx_event_oid - 1] = ObjectIdGetDatum(eventoid);
	values[Anum_pg_qx_event_qxeventdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_event_qxeventsessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_event_qxeventtaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_event_qxeventstepid - 1] = ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_event_qxeventowner - 1] = ObjectIdGetDatum(ownerid);
	eventlsn = QxEmitSemanticEventRecord(eventoid, sessionoid, taskoid, stepoid,
										 ownerid, kind, payload);
	values[Anum_pg_qx_event_qxeventlsn - 1] = LSNGetDatum(eventlsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventkind, kind);
	qx_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventpayload, payload);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return eventoid;
}

static Oid
qx_insert_trace(Relation rel, Oid sessionoid, Oid taskoid, Oid stepoid,
				Oid ownerid, const char *name, const char *detail)
{
	Datum		values[Natts_pg_qx_trace];
	bool		nulls[Natts_pg_qx_trace];
	Oid			traceoid;
	HeapTuple	tup;
	XLogRecPtr	tracelsn;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	traceoid = GetNewOidWithIndex(rel, QxTraceOidIndexId,
								  Anum_pg_qx_trace_oid);
	values[Anum_pg_qx_trace_oid - 1] = ObjectIdGetDatum(traceoid);
	values[Anum_pg_qx_trace_qxtracedbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_trace_qxtracesessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_trace_qxtracetaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_trace_qxtracestepid - 1] = ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_trace_qxtraceowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_trace_qxtracestate - 1] =
		CharGetDatum(QX_TRACE_STATE_CLOSED);
	tracelsn = QxEmitSemanticTraceRecord(traceoid, sessionoid, taskoid, stepoid,
										 ownerid, QX_TRACE_STATE_CLOSED,
										 name, detail);
	values[Anum_pg_qx_trace_qxtracelsn - 1] = LSNGetDatum(tracelsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracename, name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracedetail, detail);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return traceoid;
}

static Oid
qx_insert_checkpoint(Relation rel, Oid sessionoid, Oid taskoid, Oid attemptoid,
					 Oid stepoid, Oid ownerid, char taskstate,
					 int16 nextstepseqno, const char *label, const char *data)
{
	Datum		values[Natts_pg_qx_checkpoint];
	bool		nulls[Natts_pg_qx_checkpoint];
	Oid			checkpointoid;
	HeapTuple	tup;
	XLogRecPtr	checkpointlsn;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	checkpointoid = GetNewOidWithIndex(rel, QxCheckpointOidIndexId,
									   Anum_pg_qx_checkpoint_oid);
	values[Anum_pg_qx_checkpoint_oid - 1] = ObjectIdGetDatum(checkpointoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointdbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_checkpoint_qxcheckpointsessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointtaskid - 1] =
		ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointattemptid - 1] =
		ObjectIdGetDatum(attemptoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointstepid - 1] =
		ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_checkpoint_qxcheckpointstate - 1] =
		CharGetDatum(QX_CHECKPOINT_STATE_DURABLE);
	values[Anum_pg_qx_checkpoint_qxcheckpointtaskstate - 1] =
		CharGetDatum(taskstate);
	values[Anum_pg_qx_checkpoint_qxcheckpointnextstepseqno - 1] =
		Int16GetDatum(nextstepseqno);
	checkpointlsn = QxEmitSemanticCheckpointRecord(checkpointoid, sessionoid,
												   taskoid, attemptoid, stepoid,
												   ownerid,
												   QX_CHECKPOINT_STATE_DURABLE,
												   taskstate, nextstepseqno,
												   label, data);
	values[Anum_pg_qx_checkpoint_qxcheckpointlsn - 1] =
		LSNGetDatum(checkpointlsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_checkpoint_qxcheckpointlabel,
					  label);
	qx_set_text_datum(values, nulls, Anum_pg_qx_checkpoint_qxcheckpointdata,
					  data);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return checkpointoid;
}

static Oid
qx_runtime_insert_task(Relation taskrel, const QxRuntimeTaskRequest *request)
{
	Datum		values[Natts_pg_qx_task];
	bool		nulls[Natts_pg_qx_task];
	HeapTuple	tup;
	Oid			taskoid;
	ObjectAddress myself;
	ObjectAddress referenced;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	taskoid = GetNewOidWithIndex(taskrel, QxTaskOidIndexId,
								 Anum_pg_qx_task_oid);
	values[Anum_pg_qx_task_oid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_task_qxtaskdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_task_qxtasksessionid - 1] =
		ObjectIdGetDatum(request->sessionoid);
	values[Anum_pg_qx_task_qxtaskagentid - 1] =
		ObjectIdGetDatum(request->agentoid);
	values[Anum_pg_qx_task_qxtaskowner - 1] =
		ObjectIdGetDatum(request->ownerid);
	values[Anum_pg_qx_task_qxtasklastattemptid - 1] =
		ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_task_qxtasklastcheckpointid - 1] =
		ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_task_qxtaskstate - 1] =
		CharGetDatum(QX_TASK_STATE_QUEUED);
	qx_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskname,
					  request->task_name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskgoal,
					  request->goal);
	qx_set_nodetree_datum(values, nulls, Anum_pg_qx_task_qxtaskinput,
						  request->input);
	qx_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskpriority,
					  request->priority);

	tup = heap_form_tuple(RelationGetDescr(taskrel), values, nulls);
	CatalogTupleInsert(taskrel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, QxTaskRelationId, taskoid);
	recordDependencyOnOwner(QxTaskRelationId, taskoid, request->ownerid);
	ObjectAddressSet(referenced, QxSessionRelationId, request->sessionoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxAgentRelationId, request->agentoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxTaskRelationId, taskoid, 0);

	return taskoid;
}

static char *
qx_fetch_checkpoint_label(HeapTuple checkpointtup)
{
	bool		isnull;
	Datum		datum;

	datum = SysCacheGetAttr(QXCHECKPOINTOID, checkpointtup,
							Anum_pg_qx_checkpoint_qxcheckpointlabel,
							&isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static int16
qx_fetch_attempt_seqno(Oid attemptoid)
{
	HeapTuple	attempttup;
	Form_pg_qx_attempt attemptform;
	int16		seqno;

	attempttup = SearchSysCache1(QXATTEMPTOID, ObjectIdGetDatum(attemptoid));
	if (!HeapTupleIsValid(attempttup))
		elog(ERROR, "cache lookup failed for QhapaqXian attempt %u", attemptoid);

	attemptform = (Form_pg_qx_attempt) GETSTRUCT(attempttup);
	seqno = attemptform->qxattemptseqno;
	ReleaseSysCache(attempttup);

	return seqno;
}

Oid
QxRuntimeSubmitTask(const QxRuntimeTaskRequest *request)
{
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	checkpointrel;
	Oid			taskoid;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			checkpointoid;
	char	   *payload;
	char	   *checkpoint_data;

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	checkpointrel = table_open(QxCheckpointRelationId, RowExclusiveLock);

	taskoid = qx_runtime_insert_task(taskrel, request);
	CommandCounterIncrement();

	attemptoid = qx_insert_attempt(attemptrel,
								   request->sessionoid,
								   taskoid,
								   request->ownerid,
								   InvalidOid,
								   1,
								   QX_ATTEMPT_STATE_RUNNING,
								   "initial");
	CommandCounterIncrement();

	payload = psprintf("goal=%s;priority=%s",
					   request->goal,
					   request->priority != NULL ? request->priority : "normal");
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "TASK_QUEUED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "runtime.queue", payload);
	pfree(payload);

	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 1,
							 "stage8.scheduler_admit",
							 "Embedded scheduler admitted attempt 1 into runtime");
	payload = psprintf("attempt_opened;state=%c",
					   QX_TASK_STATE_RUNNING);
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_DISPATCHED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.dispatch", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 2,
							 "stage8.capture_input",
							 "Attempt 1 captured task goal and raw input");
	payload = psprintf("input_present=%s;task_name=%s",
					   request->input != NULL ? "true" : "false",
					   request->task_name != NULL ? request->task_name : "<anonymous>");
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_INPUT_CAPTURED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.capture_input", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 3,
							 "stage8.checkpoint_barrier",
							 "Attempt 1 reached a resumable checkpoint barrier");
	payload = psprintf("checkpoint=stage8.after_capture;task_state=%c",
					   QX_TASK_STATE_CHECKPOINTED);
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_CHECKPOINTED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.checkpoint", payload);

	checkpoint_data = psprintf("task=%u;attempt=%u;session=%u;next_step=%d",
							   taskoid, attemptoid, request->sessionoid, 4);
	checkpointoid = qx_insert_checkpoint(checkpointrel,
										 request->sessionoid,
										 taskoid,
										 attemptoid,
										 stepoid,
										 request->ownerid,
										 QX_TASK_STATE_CHECKPOINTED,
										 4,
										 "stage8.after_capture",
										 checkpoint_data);
	pfree(checkpoint_data);

	qx_update_attempt_state(attemptrel, attemptoid, QX_ATTEMPT_STATE_CHECKPOINTED);
	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_CHECKPOINTED,
						   attemptoid, true, checkpointoid, true);

	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "TASK_READY_FOR_RESUME", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "runtime.pause",
						   "Task paused at durable checkpoint and awaits RESUME TASK");
	pfree(payload);

	table_close(checkpointrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	return taskoid;
}

Oid
QxRuntimeResumeTask(Oid taskoid, const char *checkpoint_label, Oid ownerid)
{
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	checkpointrel;
	HeapTuple	tasktup;
	Form_pg_qx_task taskform;
	HeapTuple	checkpointtup;
	Form_pg_qx_checkpoint checkpointform;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			finalcheckpointoid;
	int16		nextattemptseqno;
	char	   *stored_label;
	char	   *payload;
	char	   *checkpoint_data;

	tasktup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tasktup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("task %u does not exist", taskoid)));

	taskform = (Form_pg_qx_task) GETSTRUCT(tasktup);

	if (!has_privs_of_role(ownerid, taskform->qxtaskowner))
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to resume task %u", taskoid),
				 errdetail("Only the task owner or a member of that role may resume the task.")));
	}

	if (taskform->qxtaskstate != QX_TASK_STATE_CHECKPOINTED)
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u is not resumable", taskoid),
				 errdetail("RESUME TASK only accepts tasks in checkpointed state.")));
	}

	if (!OidIsValid(taskform->qxtasklastcheckpointid))
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no checkpoint to resume from", taskoid)));
	}

	checkpointtup = SearchSysCache1(QXCHECKPOINTOID,
									ObjectIdGetDatum(taskform->qxtasklastcheckpointid));
	if (!HeapTupleIsValid(checkpointtup))
	{
		ReleaseSysCache(tasktup);
		elog(ERROR, "cache lookup failed for QhapaqXian checkpoint %u",
			 taskform->qxtasklastcheckpointid);
	}

	checkpointform = (Form_pg_qx_checkpoint) GETSTRUCT(checkpointtup);
	stored_label = qx_fetch_checkpoint_label(checkpointtup);

	if (checkpoint_label != NULL &&
		(stored_label == NULL || strcmp(checkpoint_label, stored_label) != 0))
	{
		if (stored_label != NULL)
			pfree(stored_label);
		ReleaseSysCache(checkpointtup);
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("checkpoint label \"%s\" does not match the current resumable checkpoint for task %u",
						checkpoint_label, taskoid)));
	}

	nextattemptseqno = 1;
	if (OidIsValid(taskform->qxtasklastattemptid))
		nextattemptseqno = qx_fetch_attempt_seqno(taskform->qxtasklastattemptid) + 1;

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	checkpointrel = table_open(QxCheckpointRelationId, RowExclusiveLock);

	attemptoid = qx_insert_attempt(attemptrel,
								   taskform->qxtasksessionid,
								   taskoid,
								   ownerid,
								   checkpointform->oid,
								   nextattemptseqno,
								   QX_ATTEMPT_STATE_RUNNING,
								   "resume");
	CommandCounterIncrement();

	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	payload = psprintf("checkpoint=%s;resume_requested;next_step=%d",
					   stored_label != NULL ? stored_label : "<unnamed>",
					   checkpointform->qxcheckpointnextstepseqno);
	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "TASK_RESUMED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "runtime.resume", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, taskform->qxtasksessionid, taskoid, 4,
							 "stage8.resume_dispatch",
							 "Attempt 2 resumed execution from the last durable checkpoint");
	payload = psprintf("resume_from=%s;state=%c",
					   stored_label != NULL ? stored_label : "<unnamed>",
					   QX_TASK_STATE_RUNNING);
	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "TASK_RESUME_DISPATCHED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "runtime.resume_dispatch", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, taskform->qxtasksessionid, taskoid, 5,
							 "stage8.complete",
							 "Attempt 2 completed the task after resuming");
	payload = psprintf("checkpoint=stage8.final;task_state=%c",
					   QX_TASK_STATE_COMPLETED);
	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "TASK_CHECKPOINTED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "runtime.final_checkpoint", payload);

	checkpoint_data = psprintf("task=%u;attempt=%u;session=%u;next_step=%d",
							   taskoid, attemptoid, taskform->qxtasksessionid, 0);
	finalcheckpointoid = qx_insert_checkpoint(checkpointrel,
											  taskform->qxtasksessionid,
											  taskoid,
											  attemptoid,
											  stepoid,
											  ownerid,
											  QX_TASK_STATE_COMPLETED,
											  0,
											  "stage8.final",
											  checkpoint_data);
	pfree(checkpoint_data);

	qx_update_attempt_state(attemptrel, attemptoid, QX_ATTEMPT_STATE_COMPLETED);
	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_COMPLETED,
						   attemptoid, true, finalcheckpointoid, true);

	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "TASK_COMPLETED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "runtime.complete",
						   "Task completed after resumable attempt handoff");
	pfree(payload);

	if (stored_label != NULL)
		pfree(stored_label);

	ReleaseSysCache(checkpointtup);
	ReleaseSysCache(tasktup);
	table_close(checkpointrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	return taskoid;
}
