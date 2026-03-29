/*-------------------------------------------------------------------------
 *
 * qx_security.c
 *	  identity, namespace, tool, and budget helpers for QhapaqXian Engine
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_namespace.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/readfuncs.h"
#include "nodes/value.h"
#include "qx/qx_security.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void qx_security_set_text(Datum *values, bool *nulls, AttrNumber attnum,
								 const char *value);
static void qx_security_set_nodetree(Datum *values, bool *nulls,
									 AttrNumber attnum, const void *node);
static char *qx_security_text_attr(HeapTuple tup, AttrNumber attnum,
								   int cacheid);
static bool qx_tool_name_in_list(List *tools, const char *tool_name);

static void
qx_security_set_text(Datum *values, bool *nulls, AttrNumber attnum,
					 const char *value)
{
	if (value == NULL)
	{
		nulls[attnum - 1] = true;
		return;
	}

	nulls[attnum - 1] = false;
	values[attnum - 1] = CStringGetTextDatum(value);
}

static void
qx_security_set_nodetree(Datum *values, bool *nulls, AttrNumber attnum,
						 const void *node)
{
	char	   *serialized;

	if (node == NULL)
	{
		nulls[attnum - 1] = true;
		return;
	}

	nulls[attnum - 1] = false;
	serialized = nodeToString(node);
	values[attnum - 1] = CStringGetTextDatum(serialized);
	pfree(serialized);
}

static char *
qx_security_text_attr(HeapTuple tup, AttrNumber attnum, int cacheid)
{
	bool		isnull;
	Datum		datum;

	datum = SysCacheGetAttr(cacheid, tup, attnum, &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static bool
qx_tool_name_in_list(List *tools, const char *tool_name)
{
	ListCell   *lc;

	foreach(lc, tools)
	{
		Node	   *tool = lfirst(lc);

		if (IsA(tool, String) &&
			strcmp(strVal(tool), tool_name) == 0)
			return true;
	}

	return false;
}

void
QxValidateToolList(List *tools)
{
	ListCell   *lc;
	List	   *seen = NIL;

	foreach(lc, tools)
	{
		Node	   *tool = lfirst(lc);
		const char *name;
		ListCell   *seen_lc;

		if (!IsA(tool, String))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool list contains a non-string entry")));

		name = strVal(tool);
		if (name == NULL || name[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool names must not be empty")));

		foreach(seen_lc, seen)
		{
			if (strcmp(strVal(lfirst(seen_lc)), name) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("tool \"%s\" is listed more than once", name)));
		}

		seen = lappend(seen, makeString(pstrdup(name)));
	}

	list_free_deep(seen);
}

void
QxBudgetPolicyFromDefList(List *budget_options, QxBudgetPolicy *policy)
{
	ListCell   *lc;

	memset(policy, 0, sizeof(*policy));

	foreach(lc, budget_options)
	{
		DefElem    *defel = lfirst_node(DefElem, lc);

		if (strcmp(defel->defname, "tokens") == 0)
		{
			policy->token_limit = defGetInt32(defel);
			policy->has_token_limit = true;
		}
		else if (strcmp(defel->defname, "cost") == 0)
		{
			policy->cost_limit = defGetInt32(defel);
			policy->has_cost_limit = true;
		}
	}
}

void
QxBudgetPolicyFromSerialized(const char *serialized, QxBudgetPolicy *policy)
{
	Node	   *node;

	memset(policy, 0, sizeof(*policy));

	if (serialized == NULL)
		return;

	node = stringToNode(serialized);
	if (node == NULL)
		return;
	if (!IsA(node, List))
		elog(ERROR, "QhapaqXian budget payload was not a List");

	QxBudgetPolicyFromDefList(castNode(List, node), policy);
}

List *
QxDeserializeToolList(const char *serialized)
{
	Node	   *node;

	if (serialized == NULL)
		return NIL;

	node = stringToNode(serialized);
	if (node == NULL)
		return NIL;
	if (!IsA(node, List))
		elog(ERROR, "QhapaqXian tool payload was not a List");

	return castNode(List, node);
}

Oid
QxEnsureNamespacePolicy(Oid namespaceoid, Oid ownerid, Oid authrole,
						const char *policy_name, List *allowed_tools)
{
	HeapTuple	existing;
	Relation	rel;
	Datum		values[Natts_pg_qx_namespace];
	bool		nulls[Natts_pg_qx_namespace];
	HeapTuple	tup;
	Oid			policyoid;
	ObjectAddress myself;
	ObjectAddress referenced;
	char	   *stored_policy_name = NULL;
	char	   *resolved_policy_name;
	char	   *stored_allowed_tools = NULL;
	List	   *allowed = NIL;
	ListCell   *lc;

	resolved_policy_name = pstrdup((policy_name != NULL && policy_name[0] != '\0') ?
								   policy_name : "namespace.default");

	existing = SearchSysCache1(QXNAMESPACENSPID, ObjectIdGetDatum(namespaceoid));
	if (HeapTupleIsValid(existing))
	{
		Form_pg_qx_namespace policyform = (Form_pg_qx_namespace) GETSTRUCT(existing);

		if (!has_privs_of_role(ownerid, policyform->qxnamespaceowner))
		{
			ReleaseSysCache(existing);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to use namespace policy for schema \"%s\"",
							get_namespace_name(namespaceoid)),
					 errdetail("Only the namespace policy owner or a member of that role may bind agents in the schema.")));
		}

		stored_policy_name = qx_security_text_attr(existing,
												   Anum_pg_qx_namespace_qxnamespacepolicy,
												   QXNAMESPACENSPID);
		stored_allowed_tools = qx_security_text_attr(existing,
													 Anum_pg_qx_namespace_qxallowedtools,
													 QXNAMESPACENSPID);
		allowed = QxDeserializeToolList(stored_allowed_tools);

		if (policyform->qxrequireknowntools)
		{
			foreach(lc, allowed_tools)
			{
				const char *tool_name = strVal(lfirst(lc));

				if (!qx_tool_name_in_list(allowed, tool_name))
				{
					char	   *policy_label = pstrdup(stored_policy_name != NULL ?
													 stored_policy_name :
													 resolved_policy_name);

					if (stored_policy_name != NULL)
						pfree(stored_policy_name);
					if (stored_allowed_tools != NULL)
						pfree(stored_allowed_tools);
					ReleaseSysCache(existing);
					ereport(ERROR,
							(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
							 errmsg("tool \"%s\" is not allowed by namespace policy \"%s\"",
									tool_name,
									policy_label),
							 errdetail("Register the tool in the namespace policy before binding it to an agent in this schema.")));
				}
			}
		}

		policyoid = policyform->oid;
		if (stored_policy_name != NULL)
			pfree(stored_policy_name);
		if (stored_allowed_tools != NULL)
			pfree(stored_allowed_tools);
		ReleaseSysCache(existing);
		pfree(resolved_policy_name);

		return policyoid;
	}

	rel = table_open(QxNamespaceRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	policyoid = GetNewOidWithIndex(rel, QxNamespaceOidIndexId,
								   Anum_pg_qx_namespace_oid);
	values[Anum_pg_qx_namespace_oid - 1] = ObjectIdGetDatum(policyoid);
	values[Anum_pg_qx_namespace_qxnamespaceid - 1] =
		ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_qx_namespace_qxnamespaceowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_namespace_qxnamespaceauthrole - 1] =
		ObjectIdGetDatum(authrole);
	values[Anum_pg_qx_namespace_qxrequireknowntools - 1] = BoolGetDatum(true);
	values[Anum_pg_qx_namespace_qxenforcebudgets - 1] = BoolGetDatum(true);
	qx_security_set_text(values, nulls,
						 Anum_pg_qx_namespace_qxnamespacepolicy,
						 resolved_policy_name);
	qx_security_set_nodetree(values, nulls,
							 Anum_pg_qx_namespace_qxallowedtools,
							 allowed_tools);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(myself, QxNamespaceRelationId, policyoid);
	recordDependencyOnOwner(QxNamespaceRelationId, policyoid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, AuthIdRelationId, authrole);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxNamespaceRelationId, policyoid, 0);

	pfree(resolved_policy_name);

	return policyoid;
}

Oid
QxEnsureOperationalIdentity(Oid namespaceoid, Oid ownerid,
							const char *identity_name, Oid authrole,
							const char *policy_name, List *budget_options)
{
	HeapTuple	existing;
	Relation	rel;
	Datum		values[Natts_pg_qx_identity];
	bool		nulls[Natts_pg_qx_identity];
	HeapTuple	tup;
	Oid			identityoid;
	ObjectAddress myself;
	ObjectAddress referenced;
	const char *resolved_name;

	resolved_name = (identity_name != NULL && identity_name[0] != '\0') ?
		identity_name : "default";

	existing = SearchSysCache2(QXIDENTITYNAMENSP,
							   CStringGetDatum(resolved_name),
							   ObjectIdGetDatum(namespaceoid));
	if (HeapTupleIsValid(existing))
	{
		Form_pg_qx_identity identityform = (Form_pg_qx_identity) GETSTRUCT(existing);

		if (!has_privs_of_role(ownerid, identityform->qxidentityowner))
		{
			ReleaseSysCache(existing);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to use identity \"%s\"",
							resolved_name),
					 errdetail("Only the identity owner or a member of that role may bind the identity to an agent.")));
		}

		identityoid = identityform->oid;
		ReleaseSysCache(existing);
		return identityoid;
	}

	rel = table_open(QxIdentityRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	identityoid = GetNewOidWithIndex(rel, QxIdentityOidIndexId,
									 Anum_pg_qx_identity_oid);
	values[Anum_pg_qx_identity_oid - 1] = ObjectIdGetDatum(identityoid);
	values[Anum_pg_qx_identity_qxidentityname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(resolved_name));
	values[Anum_pg_qx_identity_qxidentitynamespace - 1] =
		ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_qx_identity_qxidentityowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_identity_qxidentityauthrole - 1] =
		ObjectIdGetDatum(authrole);
	qx_security_set_text(values, nulls, Anum_pg_qx_identity_qxidentitypolicy,
						 policy_name);
	qx_security_set_nodetree(values, nulls,
							 Anum_pg_qx_identity_qxidentitybudget,
							 budget_options);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	ObjectAddressSet(myself, QxIdentityRelationId, identityoid);
	recordDependencyOnOwner(QxIdentityRelationId, identityoid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, AuthIdRelationId, authrole);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxIdentityRelationId, identityoid, 0);

	return identityoid;
}

void
QxEnsureToolCatalogEntries(Oid namespaceoid, Oid ownerid, List *tools)
{
	(void) namespaceoid;
	(void) ownerid;
	(void) tools;
}

char *
QxIdentityNameById(Oid identityoid)
{
	HeapTuple	identitytup;
	Form_pg_qx_identity identityform;
	char	   *name;

	identitytup = SearchSysCache1(QXIDENTITYOID, ObjectIdGetDatum(identityoid));
	if (!HeapTupleIsValid(identitytup))
		elog(ERROR, "cache lookup failed for QhapaqXian identity %u", identityoid);

	identityform = (Form_pg_qx_identity) GETSTRUCT(identitytup);
	name = pstrdup(NameStr(identityform->qxidentityname));
	ReleaseSysCache(identitytup);

	return name;
}

char *
QxNamespacePolicyNameById(Oid policyoid)
{
	HeapTuple	policytup;
	char	   *name;

	policytup = SearchSysCache1(QXNAMESPACEOID, ObjectIdGetDatum(policyoid));
	if (!HeapTupleIsValid(policytup))
		elog(ERROR, "cache lookup failed for QhapaqXian namespace policy %u", policyoid);

	name = qx_security_text_attr(policytup,
								 Anum_pg_qx_namespace_qxnamespacepolicy,
								 QXNAMESPACEOID);
	ReleaseSysCache(policytup);

	return name != NULL ? name : pstrdup("namespace.default");
}

void
QxAuthorizeToolsForNamespace(Oid namespacepolicyoid, Oid namespaceoid,
							 Oid ownerid, List *tools,
							 QxToolAuthorization *authz)
{
	HeapTuple	policytup;
	Form_pg_qx_namespace policyform;
	char	   *serialized_allowed_tools;
	List	   *allowed_tools;
	ListCell   *lc;

	memset(authz, 0, sizeof(*authz));

	policytup = SearchSysCache1(QXNAMESPACEOID, ObjectIdGetDatum(namespacepolicyoid));
	if (!HeapTupleIsValid(policytup))
		elog(ERROR, "cache lookup failed for QhapaqXian namespace policy %u",
			 namespacepolicyoid);

	policyform = (Form_pg_qx_namespace) GETSTRUCT(policytup);

	if (!has_privs_of_role(ownerid, policyform->qxnamespaceowner) &&
		!has_privs_of_role(ownerid, policyform->qxnamespaceauthrole))
	{
		ReleaseSysCache(policytup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to authorize tools in schema \"%s\"",
						get_namespace_name(namespaceoid)),
				 errdetail("The caller must be able to assume the namespace policy owner or auth role.")));
	}

	serialized_allowed_tools = qx_security_text_attr(policytup,
													 Anum_pg_qx_namespace_qxallowedtools,
													 QXNAMESPACEOID);
	allowed_tools = QxDeserializeToolList(serialized_allowed_tools);

	authz->namespace_policy_oid = policyform->oid;
	authz->namespace_policy_name = qx_security_text_attr(policytup,
														 Anum_pg_qx_namespace_qxnamespacepolicy,
														 QXNAMESPACEOID);
	authz->require_known_tools = policyform->qxrequireknowntools;
	authz->enforce_budgets = policyform->qxenforcebudgets;

	foreach(lc, tools)
	{
		const char *tool_name = strVal(lfirst(lc));

		if (policyform->qxrequireknowntools &&
			!qx_tool_name_in_list(allowed_tools, tool_name))
		{
			if (serialized_allowed_tools != NULL)
				pfree(serialized_allowed_tools);
			ReleaseSysCache(policytup);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("tool \"%s\" is not allowed by namespace policy \"%s\"",
							tool_name,
							authz->namespace_policy_name != NULL ?
							authz->namespace_policy_name : "namespace.default")));
		}

		authz->tool_count += 1;
		authz->tool_token_cost += 8;
		authz->tool_cost_units += 4;
	}

	if (serialized_allowed_tools != NULL)
		pfree(serialized_allowed_tools);
	ReleaseSysCache(policytup);
}
