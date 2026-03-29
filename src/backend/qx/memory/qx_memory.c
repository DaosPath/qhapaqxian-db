/*-------------------------------------------------------------------------
 *
 * qx_memory.c
 *	  Stage 10 memory storage subsystem for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/memory/qx_memory.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_qx_event.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_memory.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/value.h"
#include "qx/qx_memory.h"
#include "qx/qx_semantic_log.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"

static void qx_set_text_datum(Datum *values, bool *nulls, AttrNumber attnum,
							  const char *value);
static char *qx_serialize_memory_value(Node *expr);
static char *qx_serialize_memory_tags(List *tags);
static char *qx_get_text_attr(TupleDesc tupdesc, HeapTuple tup, AttrNumber attnum);
static bool qx_scope_is_requested(char scope, List *scopes);
static bool qx_memory_matches_filter(const char *match_text, const char *key,
									 const char *value, const char *tags);
static Oid qx_insert_memory_event(Relation rel, Oid sessionoid, Oid ownerid,
								  const char *kind, const char *payload);

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

const char *
QxMemoryScopeLabel(char scope)
{
	switch (scope)
	{
		case QX_MEMORY_SCOPE_WORKING:
			return "working";
		case QX_MEMORY_SCOPE_EPISODIC:
			return "episodic";
		case QX_MEMORY_SCOPE_SEMANTIC:
			return "semantic";
	}

	return "unknown";
}

static char *
qx_serialize_memory_value(Node *expr)
{
	if (expr == NULL)
		return NULL;

	if (IsA(expr, A_Const))
	{
		A_Const    *con = castNode(A_Const, expr);

		if (con->isnull)
			return pstrdup("null");

		switch (nodeTag(&con->val))
		{
			case T_String:
				return pstrdup(strVal(&con->val));
			case T_Integer:
				return psprintf("%d", intVal(&con->val));
			case T_Float:
				return pstrdup(strVal(&con->val));
			case T_Boolean:
				return pstrdup(boolVal(&con->val) ? "true" : "false");
			default:
				break;
		}
	}

	return nodeToString(expr);
}

static char *
qx_serialize_memory_tags(List *tags)
{
	StringInfoData buf;
	ListCell   *lc;

	if (tags == NIL)
		return NULL;

	initStringInfo(&buf);

	foreach(lc, tags)
	{
		String	   *tag = lfirst_node(String, lc);

		if (buf.len > 0)
			appendStringInfoChar(&buf, ',');
		appendStringInfoString(&buf, strVal(tag));
	}

	return buf.data;
}

static char *
qx_get_text_attr(TupleDesc tupdesc, HeapTuple tup, AttrNumber attnum)
{
	bool		isnull;
	Datum		datum;

	datum = heap_getattr(tup, attnum, tupdesc, &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static bool
qx_scope_is_requested(char scope, List *scopes)
{
	ListCell   *lc;

	if (scopes == NIL)
		return true;

	foreach(lc, scopes)
	{
		if ((char) intVal(lfirst(lc)) == scope)
			return true;
	}

	return false;
}

static bool
qx_memory_matches_filter(const char *match_text, const char *key,
						 const char *value, const char *tags)
{
	if (match_text == NULL)
		return true;

	if (key != NULL && strstr(key, match_text) != NULL)
		return true;
	if (value != NULL && strstr(value, match_text) != NULL)
		return true;
	if (tags != NULL && strstr(tags, match_text) != NULL)
		return true;

	return false;
}

static Oid
qx_insert_memory_event(Relation rel, Oid sessionoid, Oid ownerid,
					   const char *kind, const char *payload)
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
	values[Anum_pg_qx_event_qxeventtaskid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_event_qxeventstepid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_event_qxeventowner - 1] = ObjectIdGetDatum(ownerid);
	eventlsn = QxEmitSemanticEventRecord(eventoid, sessionoid, InvalidOid,
										 InvalidOid, ownerid, kind, payload);
	values[Anum_pg_qx_event_qxeventlsn - 1] = LSNGetDatum(eventlsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventkind, kind);
	qx_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventpayload, payload);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return eventoid;
}

Oid
QxRememberSessionMemory(const QxRememberRequest *request)
{
	Relation	memoryrel;
	Relation	eventrel;
	Datum		values[Natts_pg_qx_memory];
	bool		nulls[Natts_pg_qx_memory];
	HeapTuple	tup;
	Oid			memoryoid;
	ObjectAddress myself;
	ObjectAddress referenced;
	char	   *serialized_value;
	char	   *serialized_tags;
	char	   *event_payload;

	memoryrel = table_open(QxMemoryRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	memoryoid = GetNewOidWithIndex(memoryrel, QxMemoryOidIndexId,
								   Anum_pg_qx_memory_oid);
	serialized_value = qx_serialize_memory_value(request->memory_value);
	serialized_tags = qx_serialize_memory_tags(request->tags);

	values[Anum_pg_qx_memory_oid - 1] = ObjectIdGetDatum(memoryoid);
	values[Anum_pg_qx_memory_qxmemorydbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_memory_qxmemoryagentid - 1] =
		ObjectIdGetDatum(request->agentoid);
	values[Anum_pg_qx_memory_qxmemorysessionid - 1] =
		ObjectIdGetDatum(request->sessionoid);
	values[Anum_pg_qx_memory_qxmemorytaskid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_memory_qxmemoryowner - 1] =
		ObjectIdGetDatum(request->ownerid);
	values[Anum_pg_qx_memory_qxmemoryscope - 1] =
		CharGetDatum(request->scope);
	qx_set_text_datum(values, nulls, Anum_pg_qx_memory_qxmemorykey,
					  request->memory_key);
	qx_set_text_datum(values, nulls, Anum_pg_qx_memory_qxmemoryvalue,
					  serialized_value);
	qx_set_text_datum(values, nulls, Anum_pg_qx_memory_qxmemorytags,
					  serialized_tags);

	tup = heap_form_tuple(RelationGetDescr(memoryrel), values, nulls);
	CatalogTupleInsert(memoryrel, tup);
	heap_freetuple(tup);

	event_payload = psprintf("memory=%u;scope=%s;key=%s",
							 memoryoid,
							 QxMemoryScopeLabel(request->scope),
							 request->memory_key);
	(void) qx_insert_memory_event(eventrel, request->sessionoid,
								  request->ownerid,
								  "MEMORY_RECORDED", event_payload);
	pfree(event_payload);

	ObjectAddressSet(myself, QxMemoryRelationId, memoryoid);
	recordDependencyOnOwner(QxMemoryRelationId, memoryoid, request->ownerid);
	ObjectAddressSet(referenced, QxSessionRelationId, request->sessionoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxAgentRelationId, request->agentoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxMemoryRelationId, memoryoid, 0);

	table_close(eventrel, RowExclusiveLock);
	table_close(memoryrel, RowExclusiveLock);

	if (serialized_value != NULL)
		pfree(serialized_value);
	if (serialized_tags != NULL)
		pfree(serialized_tags);

	return memoryoid;
}

TupleDesc
QxFetchMemoryResultDesc(void)
{
	TupleDesc	tupdesc;

	tupdesc = CreateTemplateTupleDesc(4);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "scope",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "memory_key",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "memory_value",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "memory_tags",
					   TEXTOID, -1, 0);

	return tupdesc;
}

void
QxFetchMemoryRecords(Oid agentoid, Oid ownerid, List *scopes,
					 const char *match_text, int limit_count,
					 DestReceiver *dest)
{
	Relation	rel;
	ScanKeyData scankey[1];
	SysScanDesc scan;
	HeapTuple	tup;
	TupOutputState *tstate;
	int			emitted = 0;

	rel = table_open(QxMemoryRelationId, AccessShareLock);
	ScanKeyInit(&scankey[0],
				Anum_pg_qx_memory_qxmemoryagentid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(agentoid));
	scan = systable_beginscan(rel, QxMemoryAgentScopeIndexId, true,
							  NULL, 1, scankey);
	tstate = begin_tup_output_tupdesc(dest, QxFetchMemoryResultDesc(),
									  &TTSOpsVirtual);

	while ((tup = systable_getnext(scan)) != NULL)
	{
		Form_pg_qx_memory form = (Form_pg_qx_memory) GETSTRUCT(tup);
		Datum		values[4];
		bool		nulls[4];
		char	   *key;
		char	   *value;
		char	   *tags;

		if (form->qxmemorydbid != MyDatabaseId)
			continue;
		if (!has_privs_of_role(ownerid, form->qxmemoryowner))
			continue;
		if (!qx_scope_is_requested(form->qxmemoryscope, scopes))
			continue;

		key = qx_get_text_attr(RelationGetDescr(rel), tup,
							   Anum_pg_qx_memory_qxmemorykey);
		value = qx_get_text_attr(RelationGetDescr(rel), tup,
								 Anum_pg_qx_memory_qxmemoryvalue);
		tags = qx_get_text_attr(RelationGetDescr(rel), tup,
								Anum_pg_qx_memory_qxmemorytags);

		if (!qx_memory_matches_filter(match_text, key, value, tags))
		{
			if (key != NULL)
				pfree(key);
			if (value != NULL)
				pfree(value);
			if (tags != NULL)
				pfree(tags);
			continue;
		}

		memset(values, 0, sizeof(values));
		memset(nulls, false, sizeof(nulls));

		values[0] = CStringGetTextDatum(QxMemoryScopeLabel(form->qxmemoryscope));
		if (key != NULL)
			values[1] = CStringGetTextDatum(key);
		else
			nulls[1] = true;
		if (value != NULL)
			values[2] = CStringGetTextDatum(value);
		else
			nulls[2] = true;
		if (tags != NULL)
			values[3] = CStringGetTextDatum(tags);
		else
			nulls[3] = true;

		do_tup_output(tstate, values, nulls);
		emitted++;

		if (key != NULL)
			pfree(key);
		if (value != NULL)
			pfree(value);
		if (tags != NULL)
			pfree(tags);

		if (emitted >= limit_count)
			break;
	}

	end_tup_output(tstate);
	systable_endscan(scan);
	table_close(rel, AccessShareLock);
}
