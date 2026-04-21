/*-------------------------------------------------------------------------
 *
 * memorycmds.c
 *	  memory command entry points for QhapaqXian DB
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/commands/memorycmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_qx_session.h"
#include "commands/memorycmds.h"
#include "miscadmin.h"
#include "qx/qx_catalog.h"
#include "qx/qx_memory.h"
#include "utils/acl.h"

static bool qx_lookup_agent_info(const char *agent_name,
								 QxCatalogAgentInfo *agent);
static Oid qx_extract_oid_literal(Node *expr, const char *subject,
								  const char *detail, const char *hint);

static bool
qx_lookup_agent_info(const char *agent_name, QxCatalogAgentInfo *agent)
{
	List	   *search_path;
	ListCell   *lc;
	bool		found = false;

	search_path = fetch_search_path(false);

	foreach(lc, search_path)
	{
		Oid			namespaceoid = lfirst_oid(lc);

		if (QxCatalogLookupAgentByName(namespaceoid, agent_name, agent))
		{
			found = true;
			break;
		}
	}

	list_free(search_path);

	return found;
}

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

void
RememberMemoryCommand(RememberStmt *stmt)
{
	Oid			sessionoid;
	QxCatalogSessionInfo session;
	Oid			ownerid;
	QxRememberRequest request;

	sessionoid = qx_extract_oid_literal(
		stmt->session_id,
		"REMEMBER",
		"Stage 10 persists agent memory in engine-owned storage, but session identifiers still require literal OID input on the utility path.",
		"Pass a numeric session OID literal, or resolve the session OID in the client before invoking REMEMBER.");

	if (!QxCatalogLookupSessionByOid(sessionoid, &session))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("session %u does not exist", sessionoid)));

	ownerid = GetUserId();

	if (session.dbid != MyDatabaseId)
	{
		QxCatalogFreeSessionInfo(&session);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("session %u belongs to a different database", sessionoid)));
	}

	if (session.status != QX_SESSION_STATUS_ACTIVE)
	{
		QxCatalogFreeSessionInfo(&session);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("session %u is not active", sessionoid),
				 errdetail("REMEMBER currently accepts active sessions only.")));
	}

	if (!has_privs_of_role(ownerid, session.ownerid))
	{
		QxCatalogFreeSessionInfo(&session);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to remember data in session %u",
						sessionoid),
				 errdetail("Only the session owner or a member of that role may persist agent memory for the session.")));
	}

	memset(&request, 0, sizeof(request));
	request.sessionoid = sessionoid;
	request.agentoid = session.agentoid;
	request.ownerid = ownerid;
	request.scope = stmt->scope;
	request.memory_key = stmt->memory_key;
	request.memory_value = stmt->memory_value;
	request.tags = stmt->tags;

	(void) QxRememberSessionMemory(&request);

	QxCatalogFreeSessionInfo(&session);
}

void
FetchMemoryCommand(FetchMemoryStmt *stmt, DestReceiver *dest)
{
	QxCatalogAgentInfo agent;
	Oid			ownerid;

	if (!qx_lookup_agent_info(stmt->agent_name, &agent))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("agent \"%s\" does not exist", stmt->agent_name)));

	ownerid = GetUserId();

	if (!has_privs_of_role(ownerid, agent.ownerid))
	{
		QxCatalogFreeAgentInfo(&agent);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to fetch memory for agent \"%s\"",
						stmt->agent_name),
				 errdetail("Only the agent owner or a member of that role may inspect engine-owned memory for the agent.")));
	}

	QxFetchMemoryRecords(agent.oid, ownerid, stmt->scopes,
						 stmt->match_text, stmt->limit_count, dest);

	QxCatalogFreeAgentInfo(&agent);
}

TupleDesc
FetchMemoryResultDesc(void)
{
	return QxFetchMemoryResultDesc();
}
