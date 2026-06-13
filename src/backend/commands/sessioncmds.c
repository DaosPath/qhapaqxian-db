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
#include "catalog/namespace.h"
#include "catalog/pg_qx_event.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_trace.h"
#include "commands/sessioncmds.h"
#include "miscadmin.h"
#include "qx/qx_catalog.h"
#include "qx/qx_security.h"
#include "utils/acl.h"
#include "utils/rel.h"

static bool
qx_lookup_agent_info(const char *agent_name, QxCatalogAgentInfo *info)
{
	List	   *search_path;
	ListCell   *lc;

	search_path = fetch_search_path(false);

	foreach(lc, search_path)
	{
		Oid			namespaceoid = lfirst_oid(lc);

		if (QxCatalogLookupAgentByName(namespaceoid, agent_name, info))
		{
			list_free(search_path);
			return true;
		}
	}

	list_free(search_path);
	return false;
}

void
StartSessionCommand(StartSessionStmt *stmt)
{
	QxCatalogAgentInfo agent;
	QxCatalogIdentityInfo identity;
	Relation	eventrel;
	Relation	tracerel;
	Relation	rel;
	Oid			sessionoid;
	Oid			ownerid;
	char	   *serialized_context = NULL;
	char	   *event_payload;
	char	   *trace_detail;
	char	   *namespace_policy_name;
	QxCatalogSessionInsertParams insert_params;

	if (stmt->returning)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("START SESSION ... RETURNING SESSION is not executable yet"),
				 errdetail("Etapa 4 persists session rows in pg_qx_session, but tuple-returning utility execution is still deferred."),
				 errhint("Proceed with Etapa 5 to expose executable session creation results.")));
	}

	if (!qx_lookup_agent_info(stmt->agent_name, &agent))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("agent \"%s\" does not exist", stmt->agent_name)));

	ownerid = GetUserId();

	if (!QxCatalogLookupIdentityByOid(agent.identityoid, &identity))
	{
		QxCatalogFreeAgentInfo(&agent);
		elog(ERROR, "cache lookup failed for QhapaqXian identity %u",
			 agent.identityoid);
	}

	if (!has_privs_of_role(ownerid, agent.ownerid))
	{
		QxCatalogFreeIdentityInfo(&identity);
		QxCatalogFreeAgentInfo(&agent);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to start a session for agent \"%s\"",
						stmt->agent_name),
				 errdetail("Only the agent owner or a member of that role may start agent sessions."),
				 errhint("Use SET ROLE to the owning role or create a different agent.")));
	}

	if (!has_privs_of_role(ownerid, identity.authrole))
	{
		QxCatalogFreeIdentityInfo(&identity);
		QxCatalogFreeAgentInfo(&agent);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to start a session as identity \"%s\"",
						identity.name),
				 errdetail("The session owner must be able to assume the identity auth role bound to the agent."),
				 errhint("Use SET ROLE to a member of the identity auth role or recreate the agent under a different owner.")));
	}

	if (stmt->context != NULL)
		serialized_context = nodeToString(stmt->context);

	rel = table_open(QxSessionRelationId, RowExclusiveLock);
	insert_params.agentoid = agent.oid;
	insert_params.namespacepolicyoid = agent.namespacepolicyoid;
	insert_params.identityoid = identity.oid;
	insert_params.ownerid = ownerid;
	insert_params.status = QX_SESSION_STATUS_ACTIVE;
	insert_params.context_serialized = serialized_context;
	sessionoid = QxCatalogInsertSession(rel, &insert_params);
	table_close(rel, RowExclusiveLock);

	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);

	event_payload = psprintf("agent=%s;owner=%u", stmt->agent_name, ownerid);
	namespace_policy_name = QxNamespacePolicyNameById(agent.namespacepolicyoid);
	trace_detail = psprintf("session root opened for agent=%s;identity=%s;namespace_policy=%s",
							stmt->agent_name, identity.name,
							namespace_policy_name);

	(void) QxCatalogInsertEvent(eventrel, sessionoid, InvalidOid, InvalidOid,
								ownerid, NULL, "SESSION_STARTED", event_payload);
	(void) QxCatalogInsertTrace(tracerel, sessionoid, InvalidOid, InvalidOid,
								ownerid, NULL, QX_TRACE_STATE_OPEN,
								"session.root", trace_detail);

	pfree(event_payload);
	pfree(trace_detail);
	pfree(namespace_policy_name);

	table_close(eventrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);

	if (serialized_context != NULL)
		pfree(serialized_context);

	QxCatalogFreeIdentityInfo(&identity);
	QxCatalogFreeAgentInfo(&agent);
}