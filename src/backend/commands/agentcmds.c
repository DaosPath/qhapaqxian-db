/*-------------------------------------------------------------------------
 *
 * agentcmds.c
 *	  stub implementation for QhapaqXian DB agent commands
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/commands/agentcmds.c
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
#include "catalog/pg_namespace.h"
#include "catalog/pg_qx_agent.h"
#include "commands/agentcmds.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
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

void
CreateAgentCommand(CreateAgentStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	ObjectAddress referenced;
	Datum		values[Natts_pg_qx_agent];
	bool		nulls[Natts_pg_qx_agent];
	Oid			agentoid;
	Oid			namespaceoid;
	Oid			ownerid;
	AclResult	aclresult;
	List	   *names;
	char	   *agentname;
	HeapTuple	tup;

	ownerid = GetUserId();
	names = list_make1(makeString(pstrdup(stmt->agent_name)));
	namespaceoid = QualifiedNameGetCreationNamespace(names, &agentname);

	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, ownerid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	rel = table_open(QxAgentRelationId, RowExclusiveLock);

	if (SearchSysCacheExists2(QXAGENTNAMENSP,
							  CStringGetDatum(agentname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("agent \"%s\" already exists in schema \"%s\"",
						agentname, get_namespace_name(namespaceoid))));

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	agentoid = GetNewOidWithIndex(rel, QxAgentOidIndexId,
								  Anum_pg_qx_agent_oid);
	values[Anum_pg_qx_agent_oid - 1] = ObjectIdGetDatum(agentoid);
	values[Anum_pg_qx_agent_qxagentname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(agentname));
	values[Anum_pg_qx_agent_qxagentnamespace - 1] =
		ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_qx_agent_qxagentowner - 1] = ObjectIdGetDatum(ownerid);

	qx_set_text_datum(values, nulls, Anum_pg_qx_agent_qxidentity,
					  stmt->identity_name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_agent_qxmodeluri,
					  stmt->model_uri);
	qx_set_text_datum(values, nulls, Anum_pg_qx_agent_qxmemoryprofile,
					  stmt->memory_profile);
	qx_set_text_datum(values, nulls, Anum_pg_qx_agent_qxpolicy,
					  stmt->policy_name);
	qx_set_nodetree_datum(values, nulls, Anum_pg_qx_agent_qxtools,
						  stmt->tools);
	qx_set_nodetree_datum(values, nulls, Anum_pg_qx_agent_qxbudget,
						  stmt->budget_options);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(myself, QxAgentRelationId, agentoid);
	recordDependencyOnOwner(QxAgentRelationId, agentoid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxAgentRelationId, agentoid, 0);

	list_free_deep(names);
}
