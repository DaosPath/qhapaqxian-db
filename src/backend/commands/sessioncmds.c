/*-------------------------------------------------------------------------
 *
 * sessioncmds.c
 *	  stub implementation for QhapaqXian DB session commands
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/commands/sessioncmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_event.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_namespace.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_trace.h"
#include "commands/sessioncmds.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "qx/qx_security.h"
#include "qx/qx_semantic_log.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static HeapTuple
qx_lookup_agent_tuple(const char *agent_name)
{
	List	   *search_path;
	ListCell   *lc;
	HeapTuple	tup = NULL;

	search_path = fetch_search_path(false);

	foreach(lc, search_path)
	{
		Oid			namespaceoid = lfirst_oid(lc);

		tup = SearchSysCache2(QXAGENTNAMENSP,
							  CStringGetDatum(agent_name),
							  ObjectIdGetDatum(namespaceoid));
		if (HeapTupleIsValid(tup))
			break;
	}

	list_free(search_path);

	return tup;
}

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

static Oid
qx_insert_session_event(Relation rel, Oid sessionoid, Oid ownerid,
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

static Oid
qx_insert_session_trace(Relation rel, Oid sessionoid, Oid ownerid,
						const char *name, const char *detail)
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
	values[Anum_pg_qx_trace_qxtracetaskid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_trace_qxtracestepid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_trace_qxtraceowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_trace_qxtracestate - 1] =
		CharGetDatum(QX_TRACE_STATE_OPEN);
	tracelsn = QxEmitSemanticTraceRecord(traceoid, sessionoid, InvalidOid,
										 InvalidOid, ownerid,
										 QX_TRACE_STATE_OPEN, name, detail);
	values[Anum_pg_qx_trace_qxtracelsn - 1] = LSNGetDatum(tracelsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracename, name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracedetail, detail);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return traceoid;
}

void
StartSessionCommand(StartSessionStmt *stmt)
{
	HeapTuple	agenttup;
	Form_pg_qx_agent agentform;
	HeapTuple	identitytup;
	Form_pg_qx_identity identityform;
	Relation	rel;
	Relation	eventrel;
	Relation	tracerel;
	ObjectAddress myself;
	ObjectAddress referenced;
	Datum		values[Natts_pg_qx_session];
	bool		nulls[Natts_pg_qx_session];
	Oid			sessionoid;
	Oid			ownerid;
	HeapTuple	tup;
	char	   *serialized_context = NULL;
	char	   *event_payload;
	char	   *trace_detail;
	char	   *namespace_policy_name;

	if (stmt->returning)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("START SESSION ... RETURNING SESSION is not executable yet"),
				 errdetail("Etapa 4 persists session rows in pg_qx_session, but tuple-returning utility execution is still deferred."),
				 errhint("Proceed with Etapa 5 to expose executable session creation results.")));
	}

	agenttup = qx_lookup_agent_tuple(stmt->agent_name);
	if (!HeapTupleIsValid(agenttup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("agent \"%s\" does not exist", stmt->agent_name)));

	agentform = (Form_pg_qx_agent) GETSTRUCT(agenttup);
	ownerid = GetUserId();

	identitytup = SearchSysCache1(QXIDENTITYOID,
								  ObjectIdGetDatum(agentform->qxidentityid));
	if (!HeapTupleIsValid(identitytup))
	{
		ReleaseSysCache(agenttup);
		elog(ERROR, "cache lookup failed for QhapaqXian identity %u",
			 agentform->qxidentityid);
	}

	identityform = (Form_pg_qx_identity) GETSTRUCT(identitytup);

	if (!has_privs_of_role(ownerid, agentform->qxagentowner))
	{
		Oid			agentoid = agentform->oid;

		ReleaseSysCache(agenttup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to start a session for agent \"%s\"",
						stmt->agent_name),
				 errdetail("Only the agent owner or a member of that role may start agent sessions."),
				 errhint("Use SET ROLE to the owning role or create a different agent.")));
			(void) agentoid;
	}

	if (!has_privs_of_role(ownerid, identityform->qxidentityauthrole))
	{
		ReleaseSysCache(identitytup);
		ReleaseSysCache(agenttup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to start a session as identity \"%s\"",
						NameStr(identityform->qxidentityname)),
				 errdetail("The session owner must be able to assume the identity auth role bound to the agent."),
				 errhint("Use SET ROLE to a member of the identity auth role or recreate the agent under a different owner.")));
	}

	rel = table_open(QxSessionRelationId, RowExclusiveLock);
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	sessionoid = GetNewOidWithIndex(rel, QxSessionOidIndexId,
									Anum_pg_qx_session_oid);
	values[Anum_pg_qx_session_oid - 1] = ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_session_qxsessiondbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_session_qxsessionagentid - 1] =
		ObjectIdGetDatum(agentform->oid);
	values[Anum_pg_qx_session_qxsessionnamespacepolicyid - 1] =
		ObjectIdGetDatum(agentform->qxnamespacepolicyid);
	values[Anum_pg_qx_session_qxsessionidentityid - 1] =
		ObjectIdGetDatum(identityform->oid);
	values[Anum_pg_qx_session_qxsessionowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_session_qxsessionstatus - 1] =
		CharGetDatum(QX_SESSION_STATUS_ACTIVE);

	if (stmt->context != NULL)
	{
		serialized_context = nodeToString(stmt->context);
		values[Anum_pg_qx_session_qxcontext - 1] =
			CStringGetTextDatum(serialized_context);
	}
	else
		nulls[Anum_pg_qx_session_qxcontext - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);

	event_payload = psprintf("agent=%s;owner=%u", stmt->agent_name, ownerid);
	namespace_policy_name = QxNamespacePolicyNameById(agentform->qxnamespacepolicyid);
	trace_detail = psprintf("session root opened for agent=%s;identity=%s;namespace_policy=%s",
							stmt->agent_name, NameStr(identityform->qxidentityname),
							namespace_policy_name);

	(void) qx_insert_session_event(eventrel, sessionoid, ownerid,
								   "SESSION_STARTED", event_payload);
	(void) qx_insert_session_trace(tracerel, sessionoid, ownerid,
								   "session.root", trace_detail);

	pfree(event_payload);
	pfree(trace_detail);
	pfree(namespace_policy_name);

	table_close(eventrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);

	ObjectAddressSet(myself, QxSessionRelationId, sessionoid);
	recordDependencyOnOwner(QxSessionRelationId, sessionoid, ownerid);
	ObjectAddressSet(referenced, QxAgentRelationId, agentform->oid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxNamespaceRelationId, agentform->qxnamespacepolicyid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxIdentityRelationId, identityform->oid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxSessionRelationId, sessionoid, 0);

	if (serialized_context != NULL)
		pfree(serialized_context);

	ReleaseSysCache(identitytup);
	ReleaseSysCache(agenttup);
}
