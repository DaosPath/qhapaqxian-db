/*-------------------------------------------------------------------------
 *
 * qx_security.c
 *	  identity, namespace, principal, tool, and budget helpers
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
#include "catalog/pg_qx_provider.h"
#include "catalog/pg_qx_principal.h"
#include "catalog/pg_qx_tool.h"
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
static int	qx_sandbox_rank(const char *sandbox_name);
static const char *qx_default_runtime_for_provider_kind(const char *provider_kind);
static void qx_validate_principal_runtime_binding(const char *principal_name,
												  const char *provider_kind,
												  const char *runtime_class,
												  const char *sandbox_name,
												  const char *receipt_signer,
												  const char *receipt_alg,
												  const char *context_name);
static char *qx_tool_contract_for_tuples(HeapTuple tooltup,
										 HeapTuple principaltup,
										 HeapTuple providertup);

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

static int
qx_sandbox_rank(const char *sandbox_name)
{
	if (sandbox_name == NULL || strcmp(sandbox_name, "builtin") == 0)
		return 0;
	if (strcmp(sandbox_name, "restricted") == 0)
		return 1;
	if (strcmp(sandbox_name, "isolated") == 0)
		return 2;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported sandbox \"%s\"", sandbox_name)));
	return -1;
}

static const char *
qx_default_runtime_for_provider_kind(const char *provider_kind)
{
	if (provider_kind != NULL && strcmp(provider_kind, "container") == 0)
		return "container";
	if (provider_kind != NULL && strcmp(provider_kind, "microvm") == 0)
		return "microvm";

	return "host";
}

static void
qx_validate_principal_runtime_binding(const char *principal_name,
										 const char *provider_kind,
										 const char *runtime_class,
										 const char *sandbox_name,
										 const char *receipt_signer,
										 const char *receipt_alg,
										 const char *context_name)
{
	const char *effective_kind;
	const char *effective_runtime;
	const char *effective_alg;

	effective_kind = (provider_kind != NULL && provider_kind[0] != '\0') ?
		provider_kind : "loopback";
	effective_runtime = (runtime_class != NULL && runtime_class[0] != '\0') ?
		runtime_class : qx_default_runtime_for_provider_kind(effective_kind);
	effective_alg = (receipt_alg != NULL && receipt_alg[0] != '\0') ?
		receipt_alg : "hmac-sha256";

	if (strcmp(effective_runtime, "host") != 0 &&
		strcmp(effective_runtime, "container") != 0 &&
		strcmp(effective_runtime, "microvm") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s saw unsupported runtime class \"%s\" on principal \"%s\"",
						context_name, effective_runtime,
						principal_name != NULL ? principal_name : "<unknown>")));

	if ((strcmp(effective_kind, "loopback") == 0 ||
		 strcmp(effective_kind, "remote") == 0) &&
		strcmp(effective_runtime, "host") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("principal \"%s\" runtime class \"%s\" is incompatible with provider kind \"%s\"",
						principal_name != NULL ? principal_name : "<unknown>",
						effective_runtime, effective_kind),
				 errdetail("Loopback and remote providers currently back only host principals.")));

	if (strcmp(effective_kind, "container") == 0 &&
		strcmp(effective_runtime, "container") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("principal \"%s\" runtime class \"%s\" is incompatible with provider kind \"%s\"",
						principal_name != NULL ? principal_name : "<unknown>",
						effective_runtime, effective_kind),
				 errdetail("Container providers require principals declared with runtime class \"container\".")));

	if (strcmp(effective_kind, "microvm") == 0 &&
		strcmp(effective_runtime, "microvm") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("principal \"%s\" runtime class \"%s\" is incompatible with provider kind \"%s\"",
						principal_name != NULL ? principal_name : "<unknown>",
						effective_runtime, effective_kind),
				 errdetail("MicroVM providers require principals declared with runtime class \"microvm\".")));

	if (strcmp(effective_runtime, "host") != 0 &&
		(sandbox_name == NULL || strcmp(sandbox_name, "isolated") != 0))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("principal \"%s\" runtime class \"%s\" requires sandbox \"isolated\"",
						principal_name != NULL ? principal_name : "<unknown>",
						effective_runtime)));

	if (strcmp(effective_alg, "ed25519") == 0 &&
		(receipt_signer == NULL || receipt_signer[0] == '\0'))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("principal \"%s\" has no signer for ed25519 receipts",
						principal_name != NULL ? principal_name : "<unknown>"),
				 errdetail("Bind SIGNER on the principal before authorizing tools under an ed25519 provider.")));
}

static char *
qx_tool_contract_for_tuples(HeapTuple tooltup, HeapTuple principaltup,
							HeapTuple providertup)
{
	Form_pg_qx_tool toolform = (Form_pg_qx_tool) GETSTRUCT(tooltup);
	Form_pg_qx_principal principalform = (Form_pg_qx_principal) GETSTRUCT(principaltup);
	Form_pg_qx_provider providerform = (Form_pg_qx_provider) GETSTRUCT(providertup);
	char	   *handler_name;
	char	   *tool_sandbox;
	char	   *principal_sandbox;
	char	   *principal_runtime;
	char	   *principal_program;
	char	   *principal_receipt_signer;
	char	   *provider_oid;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *provider_receipt_alg;
	char	   *contract;

	handler_name = qx_security_text_attr(tooltup,
										 Anum_pg_qx_tool_qxtoolhandler,
										 QXTOOLOID);
	tool_sandbox = qx_security_text_attr(tooltup,
										 Anum_pg_qx_tool_qxtoolsandbox,
										 QXTOOLOID);
	principal_sandbox = qx_security_text_attr(principaltup,
											  Anum_pg_qx_principal_qxprincipalsandbox,
											  QXPRINCIPALOID);
	principal_runtime = qx_security_text_attr(principaltup,
											  Anum_pg_qx_principal_qxprincipalruntimeclass,
											  QXPRINCIPALOID);
	principal_program = qx_security_text_attr(principaltup,
											  Anum_pg_qx_principal_qxprincipalprogram,
											  QXPRINCIPALOID);
	principal_receipt_signer = qx_security_text_attr(principaltup,
													 Anum_pg_qx_principal_qxprincipalreceiptsigner,
													 QXPRINCIPALOID);
	provider_oid = psprintf("%u", providerform->oid);
	provider_kind = qx_security_text_attr(providertup,
										  Anum_pg_qx_provider_qxproviderkind,
										  QXPROVIDEROID);
	provider_endpoint = qx_security_text_attr(providertup,
											  Anum_pg_qx_provider_qxproviderendpoint,
											  QXPROVIDEROID);
	provider_receipt_alg = qx_security_text_attr(providertup,
												 Anum_pg_qx_provider_qxproviderreceiptalg,
												 QXPROVIDEROID);
	if (principal_runtime == NULL || principal_runtime[0] == '\0')
	{
		if (principal_runtime != NULL)
			pfree(principal_runtime);
		principal_runtime =
			pstrdup(qx_default_runtime_for_provider_kind(provider_kind));
	}

	contract = psprintf("tool=%s;handler=%s;tool_sandbox=%s;principal=%s;principal_sandbox=%s;principal_runtime=%s;program=%s;receipt_signer=%s;provider=%s;provider_oid=%s;provider_kind=%s;provider_endpoint=%s;provider_attestation=%s;receipt_schema=qx.receipt.v1;receipt_alg=%s",
						NameStr(toolform->qxtoolname),
						handler_name != NULL ? handler_name : "<none>",
						tool_sandbox != NULL ? tool_sandbox : "builtin",
						NameStr(principalform->qxprincipalname),
						principal_sandbox != NULL ? principal_sandbox : "builtin",
						principal_runtime != NULL ? principal_runtime : "host",
						principal_program != NULL ? principal_program : "",
						principal_receipt_signer != NULL ? principal_receipt_signer : "",
						NameStr(providerform->qxprovidername),
						provider_oid,
						provider_kind != NULL ? provider_kind : "loopback",
						provider_endpoint != NULL ? provider_endpoint : "local://qhapaqxian-tool-runner",
						providerform->qxproviderattestationrequired ? "required" : "optional",
						provider_receipt_alg != NULL ? provider_receipt_alg : "hmac-sha256");

	if (handler_name != NULL)
		pfree(handler_name);
	if (tool_sandbox != NULL)
		pfree(tool_sandbox);
	if (principal_sandbox != NULL)
		pfree(principal_sandbox);
	if (principal_runtime != NULL)
		pfree(principal_runtime);
	if (principal_program != NULL)
		pfree(principal_program);
	if (principal_receipt_signer != NULL)
		pfree(principal_receipt_signer);
	if (provider_oid != NULL)
		pfree(provider_oid);
	if (provider_kind != NULL)
		pfree(provider_kind);
	if (provider_endpoint != NULL)
		pfree(provider_endpoint);
	if (provider_receipt_alg != NULL)
		pfree(provider_receipt_alg);

	return contract;
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
QxLookupNamespacePolicy(Oid namespaceoid, const char *policy_name,
						bool missing_ok)
{
	HeapTuple	policytup;
	Oid			policyoid;

	if (policy_name == NULL || policy_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("namespace policy name must not be empty")));

	policytup = SearchSysCache2(QXNAMESPACENAMENSP,
								CStringGetDatum(policy_name),
								ObjectIdGetDatum(namespaceoid));
	if (!HeapTupleIsValid(policytup))
	{
		if (missing_ok)
			return InvalidOid;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("namespace policy \"%s\" does not exist in schema \"%s\"",
						policy_name, get_namespace_name(namespaceoid)),
				 errdetail("Create the namespace policy before binding agents or tools to it.")));
	}

	policyoid = ((Form_pg_qx_namespace) GETSTRUCT(policytup))->oid;
	ReleaseSysCache(policytup);

	return policyoid;
}

Oid
QxLookupPrincipal(Oid namespaceoid, const char *principal_name,
				  bool missing_ok)
{
	HeapTuple	principaltup;
	Oid			principaloid;

	if (principal_name == NULL || principal_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("principal name must not be empty")));

	principaltup = SearchSysCache2(QXPRINCIPALNAMENSP,
								   CStringGetDatum(principal_name),
								   ObjectIdGetDatum(namespaceoid));
	if (!HeapTupleIsValid(principaltup))
	{
		if (missing_ok)
			return InvalidOid;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("principal \"%s\" does not exist in schema \"%s\"",
						principal_name, get_namespace_name(namespaceoid)),
				 errdetail("Create the principal before binding it to a tool.")));
	}

	principaloid = ((Form_pg_qx_principal) GETSTRUCT(principaltup))->oid;
	ReleaseSysCache(principaltup);

	return principaloid;
}

Oid
QxLookupProvider(Oid namespaceoid, const char *provider_name,
				 bool missing_ok)
{
	HeapTuple	providertup;
	Oid			provideroid;

	if (provider_name == NULL || provider_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("provider name must not be empty")));

	providertup = SearchSysCache2(QXPROVIDERNAMENSP,
								  CStringGetDatum(provider_name),
								  ObjectIdGetDatum(namespaceoid));
	if (!HeapTupleIsValid(providertup))
	{
		if (missing_ok)
			return InvalidOid;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("provider \"%s\" does not exist in schema \"%s\"",
						provider_name, get_namespace_name(namespaceoid)),
				 errdetail("Create the provider before binding it to a principal.")));
	}

	provideroid = ((Form_pg_qx_provider) GETSTRUCT(providertup))->oid;
	ReleaseSysCache(providertup);

	return provideroid;
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
QxValidateRegisteredTools(Oid namespaceoid, List *tools, bool require_enabled)
{
	ListCell   *lc;

	foreach(lc, tools)
	{
		const char *tool_name = strVal(lfirst(lc));
		HeapTuple	tooltup;
		Form_pg_qx_tool toolform;

		tooltup = SearchSysCache2(QXTOOLNAMENSP,
								  CStringGetDatum(tool_name),
								  ObjectIdGetDatum(namespaceoid));
		if (!HeapTupleIsValid(tooltup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tool \"%s\" is not registered in schema \"%s\"",
							tool_name, get_namespace_name(namespaceoid)),
					 errdetail("Register the tool with CREATE TOOL before referencing it from a namespace policy or agent.")));

		toolform = (Form_pg_qx_tool) GETSTRUCT(tooltup);
		if (require_enabled)
		{
			HeapTuple	principaltup;
			HeapTuple	providertup;
			char	   *tool_sandbox;
			char	   *principal_sandbox;
			char	   *principal_runtime;
			char	   *principal_receipt_signer;
			char	   *provider_kind;
			char	   *provider_receipt_alg;
			Form_pg_qx_principal principalform;
			Form_pg_qx_provider providerform;

			if (!toolform->qxtoolenabled)
			{
				ReleaseSysCache(tooltup);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("tool \"%s\" is disabled in schema \"%s\"",
								tool_name, get_namespace_name(namespaceoid)),
						 errdetail("Enable the tool before binding it to an agent runtime.")));
			}

			if (!OidIsValid(toolform->qxtoolprincipalid))
			{
				ReleaseSysCache(tooltup);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("tool \"%s\" has no principal binding", tool_name),
						 errdetail("Bind the tool to a namespace principal before enabling agent execution.")));
			}

			principaltup = SearchSysCache1(QXPRINCIPALOID,
										   ObjectIdGetDatum(toolform->qxtoolprincipalid));
			if (!HeapTupleIsValid(principaltup))
			{
				ReleaseSysCache(tooltup);
				elog(ERROR, "cache lookup failed for QhapaqXian principal %u",
					 toolform->qxtoolprincipalid);
			}

			principalform = (Form_pg_qx_principal) GETSTRUCT(principaltup);
			if (!principalform->qxprincipalenabled)
			{
				ReleaseSysCache(principaltup);
				ReleaseSysCache(tooltup);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("principal for tool \"%s\" is disabled", tool_name),
						 errdetail("Enable the principal before binding the tool to an agent runtime.")));
			}

			if (!OidIsValid(principalform->qxprincipalproviderid))
			{
				ReleaseSysCache(principaltup);
				ReleaseSysCache(tooltup);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("principal for tool \"%s\" has no provider binding", tool_name),
						 errdetail("Bind the principal to a provider before enabling agent execution.")));
			}

			providertup = SearchSysCache1(QXPROVIDEROID,
										  ObjectIdGetDatum(principalform->qxprincipalproviderid));
			if (!HeapTupleIsValid(providertup))
			{
				ReleaseSysCache(principaltup);
				ReleaseSysCache(tooltup);
				elog(ERROR, "cache lookup failed for QhapaqXian provider %u",
					 principalform->qxprincipalproviderid);
			}

			providerform = (Form_pg_qx_provider) GETSTRUCT(providertup);
			if (!providerform->qxproviderenabled)
			{
				ReleaseSysCache(providertup);
				ReleaseSysCache(principaltup);
				ReleaseSysCache(tooltup);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("provider for tool \"%s\" is disabled", tool_name),
						 errdetail("Enable the provider before binding the tool to an agent runtime.")));
			}

			tool_sandbox = qx_security_text_attr(tooltup,
												 Anum_pg_qx_tool_qxtoolsandbox,
												 QXTOOLOID);
			principal_sandbox = qx_security_text_attr(principaltup,
													  Anum_pg_qx_principal_qxprincipalsandbox,
													  QXPRINCIPALOID);
			principal_runtime = qx_security_text_attr(principaltup,
													 Anum_pg_qx_principal_qxprincipalruntimeclass,
													 QXPRINCIPALOID);
			principal_receipt_signer = qx_security_text_attr(principaltup,
															 Anum_pg_qx_principal_qxprincipalreceiptsigner,
															 QXPRINCIPALOID);
			provider_kind = qx_security_text_attr(providertup,
												 Anum_pg_qx_provider_qxproviderkind,
												 QXPROVIDEROID);
			provider_receipt_alg = qx_security_text_attr(providertup,
														 Anum_pg_qx_provider_qxproviderreceiptalg,
														 QXPROVIDEROID);
			if (qx_sandbox_rank(tool_sandbox) > qx_sandbox_rank(principal_sandbox))
			{
				if (tool_sandbox != NULL)
					pfree(tool_sandbox);
				if (principal_sandbox != NULL)
					pfree(principal_sandbox);
				if (principal_runtime != NULL)
					pfree(principal_runtime);
				if (principal_receipt_signer != NULL)
					pfree(principal_receipt_signer);
				if (provider_kind != NULL)
					pfree(provider_kind);
				if (provider_receipt_alg != NULL)
					pfree(provider_receipt_alg);
				ReleaseSysCache(providertup);
				ReleaseSysCache(principaltup);
				ReleaseSysCache(tooltup);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("tool \"%s\" exceeds its principal sandbox ceiling",
								tool_name)));
			}
			qx_validate_principal_runtime_binding(NameStr(principalform->qxprincipalname),
												 provider_kind,
												 principal_runtime,
												 principal_sandbox,
												 principal_receipt_signer,
												 provider_receipt_alg,
												 "tool registration validation");

			if (tool_sandbox != NULL)
				pfree(tool_sandbox);
			if (principal_sandbox != NULL)
				pfree(principal_sandbox);
			if (principal_runtime != NULL)
				pfree(principal_runtime);
			if (principal_receipt_signer != NULL)
				pfree(principal_receipt_signer);
			if (provider_kind != NULL)
				pfree(provider_kind);
			if (provider_receipt_alg != NULL)
				pfree(provider_receipt_alg);
			ReleaseSysCache(providertup);
			ReleaseSysCache(principaltup);
		}

		ReleaseSysCache(tooltup);
	}
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
	Form_pg_qx_namespace policyform;
	char	   *name;

	policytup = SearchSysCache1(QXNAMESPACEOID, ObjectIdGetDatum(policyoid));
	if (!HeapTupleIsValid(policytup))
		elog(ERROR, "cache lookup failed for QhapaqXian namespace policy %u", policyoid);

	policyform = (Form_pg_qx_namespace) GETSTRUCT(policytup);
	name = pstrdup(NameStr(policyform->qxnamespacepolicyname));
	ReleaseSysCache(policytup);

	return name;
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
	authz->namespace_policy_name = pstrdup(NameStr(policyform->qxnamespacepolicyname));
	authz->require_known_tools = policyform->qxrequireknowntools;
	authz->enforce_budgets = policyform->qxenforcebudgets;

	foreach(lc, tools)
	{
		const char *tool_name = strVal(lfirst(lc));
		HeapTuple	tooltup;
		HeapTuple	principaltup;
		HeapTuple	providertup;
		Form_pg_qx_tool toolform;
		Form_pg_qx_principal principalform;
		Form_pg_qx_provider providerform;
		char	   *tool_sandbox;
		char	   *principal_sandbox;
		char	   *principal_runtime;
		char	   *principal_receipt_signer;
		char	   *provider_kind;
		char	   *provider_receipt_alg;
		char	   *contract;

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
							authz->namespace_policy_name : "<unknown>"),
					 errdetail("Register the tool in the namespace policy before executing the agent task.")));
		}

		tooltup = SearchSysCache2(QXTOOLNAMENSP,
								  CStringGetDatum(tool_name),
								  ObjectIdGetDatum(namespaceoid));
		if (!HeapTupleIsValid(tooltup))
		{
			if (serialized_allowed_tools != NULL)
				pfree(serialized_allowed_tools);
			ReleaseSysCache(policytup);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tool \"%s\" is not registered in schema \"%s\"",
							tool_name, get_namespace_name(namespaceoid))));
		}

		toolform = (Form_pg_qx_tool) GETSTRUCT(tooltup);
		if (!toolform->qxtoolenabled)
		{
			ReleaseSysCache(tooltup);
			if (serialized_allowed_tools != NULL)
				pfree(serialized_allowed_tools);
			ReleaseSysCache(policytup);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tool \"%s\" is disabled in schema \"%s\"",
							tool_name, get_namespace_name(namespaceoid))));
		}

		if (!OidIsValid(toolform->qxtoolprincipalid))
		{
			ReleaseSysCache(tooltup);
			if (serialized_allowed_tools != NULL)
				pfree(serialized_allowed_tools);
			ReleaseSysCache(policytup);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tool \"%s\" has no principal binding", tool_name)));
		}

		principaltup = SearchSysCache1(QXPRINCIPALOID,
									   ObjectIdGetDatum(toolform->qxtoolprincipalid));
		if (!HeapTupleIsValid(principaltup))
		{
			ReleaseSysCache(tooltup);
			elog(ERROR, "cache lookup failed for QhapaqXian principal %u",
				 toolform->qxtoolprincipalid);
		}

		principalform = (Form_pg_qx_principal) GETSTRUCT(principaltup);
		if (!principalform->qxprincipalenabled)
		{
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("principal \"%s\" is disabled in schema \"%s\"",
							NameStr(principalform->qxprincipalname),
							get_namespace_name(namespaceoid))));
		}

		if (!OidIsValid(principalform->qxprincipalproviderid))
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("principal \"%s\" has no provider binding",
							NameStr(principalform->qxprincipalname))));

		providertup = SearchSysCache1(QXPROVIDEROID,
									  ObjectIdGetDatum(principalform->qxprincipalproviderid));
		if (!HeapTupleIsValid(providertup))
		{
			ReleaseSysCache(principaltup);
			ReleaseSysCache(tooltup);
			elog(ERROR, "cache lookup failed for QhapaqXian provider %u",
				 principalform->qxprincipalproviderid);
		}

		providerform = (Form_pg_qx_provider) GETSTRUCT(providertup);
		if (!providerform->qxproviderenabled)
		{
			ReleaseSysCache(providertup);
			ReleaseSysCache(principaltup);
			ReleaseSysCache(tooltup);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("provider \"%s\" is disabled in schema \"%s\"",
							NameStr(providerform->qxprovidername),
							get_namespace_name(namespaceoid))));
		}

		tool_sandbox = qx_security_text_attr(tooltup,
												 Anum_pg_qx_tool_qxtoolsandbox,
												 QXTOOLOID);
		principal_sandbox = qx_security_text_attr(principaltup,
													  Anum_pg_qx_principal_qxprincipalsandbox,
													  QXPRINCIPALOID);
		principal_runtime = qx_security_text_attr(principaltup,
												 Anum_pg_qx_principal_qxprincipalruntimeclass,
												 QXPRINCIPALOID);
		principal_receipt_signer = qx_security_text_attr(principaltup,
														 Anum_pg_qx_principal_qxprincipalreceiptsigner,
														 QXPRINCIPALOID);
		provider_kind = qx_security_text_attr(providertup,
											 Anum_pg_qx_provider_qxproviderkind,
											 QXPROVIDEROID);
		provider_receipt_alg = qx_security_text_attr(providertup,
													 Anum_pg_qx_provider_qxproviderreceiptalg,
													 QXPROVIDEROID);
		if (qx_sandbox_rank(tool_sandbox) > qx_sandbox_rank(principal_sandbox))
		{
			if (tool_sandbox != NULL)
				pfree(tool_sandbox);
			if (principal_sandbox != NULL)
				pfree(principal_sandbox);
			if (principal_runtime != NULL)
				pfree(principal_runtime);
			if (principal_receipt_signer != NULL)
				pfree(principal_receipt_signer);
			if (provider_kind != NULL)
				pfree(provider_kind);
			if (provider_receipt_alg != NULL)
				pfree(provider_receipt_alg);
			ReleaseSysCache(providertup);
			ReleaseSysCache(principaltup);
			ReleaseSysCache(tooltup);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool \"%s\" exceeds the sandbox ceiling of principal \"%s\"",
							tool_name, NameStr(principalform->qxprincipalname))));
		}
		qx_validate_principal_runtime_binding(NameStr(principalform->qxprincipalname),
											 provider_kind,
											 principal_runtime,
											 principal_sandbox,
											 principal_receipt_signer,
											 provider_receipt_alg,
											 "namespace tool authorization");

		if (tool_sandbox != NULL)
			pfree(tool_sandbox);
		if (principal_sandbox != NULL)
			pfree(principal_sandbox);
		if (principal_runtime != NULL)
			pfree(principal_runtime);
		if (principal_receipt_signer != NULL)
			pfree(principal_receipt_signer);
		if (provider_kind != NULL)
			pfree(provider_kind);
		if (provider_receipt_alg != NULL)
			pfree(provider_receipt_alg);

		contract = qx_tool_contract_for_tuples(tooltup, principaltup, providertup);
		authz->tool_count += 1;
		authz->tool_token_cost += toolform->qxtooltokencost;
		authz->tool_cost_units += toolform->qxtoolcostunits;
		authz->tool_oids = lappend_oid(authz->tool_oids, toolform->oid);
		authz->tool_contracts = lappend(authz->tool_contracts,
										makeString(contract));
		ReleaseSysCache(providertup);
		ReleaseSysCache(principaltup);
		ReleaseSysCache(tooltup);
	}

	if (serialized_allowed_tools != NULL)
		pfree(serialized_allowed_tools);
	ReleaseSysCache(policytup);
}
