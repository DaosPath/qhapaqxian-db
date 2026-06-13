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
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_namespace.h"
#include "commands/defrem.h"
#include "commands/agentcmds.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "qx/qx_catalog.h"
#include "qx/qx_security.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

void
CreateAgentCommand(CreateAgentStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	ObjectAddress referenced;
	Oid			agentoid;
	Oid			identityoid;
	Oid			namespacepolicyoid;
	Oid			namespaceoid;
	Oid			ownerid;
	AclResult	aclresult;
	List	   *names;
	char	   *agentname;
	QxCatalogAgentInfo existing_agent;
	QxCatalogAgentInsertParams insert_params;
	QxToolAuthorization authz;

	ownerid = GetUserId();
	names = list_make1(makeString(pstrdup(stmt->agent_name)));
	namespaceoid = QualifiedNameGetCreationNamespace(names, &agentname);

	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, ownerid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(namespaceoid));

	if (QxCatalogLookupAgentByName(namespaceoid, agentname, &existing_agent))
	{
		QxCatalogFreeAgentInfo(&existing_agent);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("agent \"%s\" already exists in schema \"%s\"",
						agentname, get_namespace_name(namespaceoid))));
	}

	QxValidateToolList(stmt->tools);
	QxValidateRegisteredTools(namespaceoid, stmt->tools, true);
	if (stmt->policy_name == NULL || stmt->policy_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("agent \"%s\" requires an explicit namespace policy", agentname),
				 errdetail("QhapaqXian no longer auto-creates namespace policies during CREATE AGENT."),
				 errhint("Create the policy first with CREATE NAMESPACE POLICY ... FOR SCHEMA %s, then reference it with POLICY <name>.",
						 get_namespace_name(namespaceoid))));
	namespacepolicyoid = QxLookupNamespacePolicy(namespaceoid,
												 stmt->policy_name,
												 false);
	QxAuthorizeToolsForNamespace(namespacepolicyoid, namespaceoid, ownerid,
								 stmt->tools, &authz);
	identityoid = QxEnsureOperationalIdentity(namespaceoid, ownerid,
											  stmt->identity_name,
											  ownerid,
											  stmt->policy_name,
											  stmt->budget_options);

	rel = table_open(QxAgentRelationId, RowExclusiveLock);
	insert_params.name = agentname;
	insert_params.namespaceoid = namespaceoid;
	insert_params.namespacepolicyoid = namespacepolicyoid;
	insert_params.identityoid = identityoid;
	insert_params.ownerid = ownerid;
	insert_params.identity_name = stmt->identity_name;
	insert_params.model_uri = stmt->model_uri;
	insert_params.memory_profile = stmt->memory_profile;
	insert_params.policy_name = stmt->policy_name;
	insert_params.tools = stmt->tools;
	insert_params.budget_options = stmt->budget_options;
	agentoid = QxCatalogInsertAgent(rel, &insert_params);
	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(myself, QxAgentRelationId, agentoid);
	recordDependencyOnOwner(QxAgentRelationId, agentoid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxNamespaceRelationId, namespacepolicyoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxIdentityRelationId, identityoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxAgentRelationId, agentoid, 0);

	list_free_deep(names);
}