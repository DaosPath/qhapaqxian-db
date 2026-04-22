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
#include <ctype.h>
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/readfuncs.h"
#include "nodes/value.h"
#include "qx/qx_catalog.h"
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
static bool qx_tool_name_in_list(List *tools, const char *tool_name);
static int	qx_sandbox_rank(const char *sandbox_name);
static const char *qx_default_runtime_for_provider_kind(const char *provider_kind);
static char *qx_join_string_list(List *values, const char *separator);
static List *qx_split_capability_tags(const char *serialized);
static bool qx_string_list_contains(List *values, const char *needle);
static char *qx_policy_contract_value(const char *contract, const char *key);
static List *qx_default_capability_tags(const char *tool_name,
									   const char *handler_name,
									   const char *tool_sandbox,
									   const char *tool_sandbox_ceiling,
									   const char *principal_runtime,
									   const char *provider_kind,
									   const char *provider_attestation,
									   const char *receipt_alg);
static void qx_validate_principal_runtime_binding(const char *principal_name,
												  const char *provider_kind,
												  const char *runtime_class,
												  const char *sandbox_name,
												  const char *receipt_signer,
												  const char *receipt_alg,
												  const char *context_name);
static char *qx_tool_runtime_class_from_info(const QxCatalogToolInfo *tool);
static char *qx_tool_sandbox_ceiling_from_info(const QxCatalogToolInfo *tool);
static char *qx_tool_capability_tags_from_info(const QxCatalogToolInfo *tool,
											   const char *tool_runtime_class,
											   const char *tool_sandbox_ceiling);
static char *qx_tool_contract_for_info(const QxCatalogToolInfo *tool,
									   const char *tool_runtime_class,
									   const char *tool_sandbox_ceiling,
									   const char *tool_capability_tags);
static void qx_validate_tool_capability_policy(const QxCatalogNamespacePolicyInfo *policy,
											   const char *tool_name,
											   const char *tool_capability_tags);

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
qx_join_string_list(List *values, const char *separator)
{
	StringInfoData buf;
	ListCell   *lc;
	bool		first = true;

	initStringInfo(&buf);
	foreach(lc, values)
	{
		Node	   *node = lfirst(lc);

		if (!IsA(node, String))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("capability tag list contains a non-string entry")));

		if (!first)
			appendStringInfoString(&buf, separator);

		appendStringInfoString(&buf, strVal(node));
		first = false;
	}

	return buf.data;
}

static List *
qx_split_capability_tags(const char *serialized)
{
	List	   *tags = NIL;
	char	   *copy;
	char	   *cursor;

	if (serialized == NULL || serialized[0] == '\0')
		return NIL;

	copy = pstrdup(serialized);
	cursor = copy;
	while (cursor != NULL && *cursor != '\0')
	{
		char	   *separator;
		char	   *tag;

		separator = strchr(cursor, '|');
		if (separator != NULL)
			*separator = '\0';

		tag = cursor;
		while (*tag != '\0' && isspace((unsigned char) *tag))
			tag++;
		if (*tag != '\0')
		{
			char	   *end = tag + strlen(tag);

			while (end > tag && isspace((unsigned char) *(end - 1)))
				*(--end) = '\0';

			tags = lappend(tags, makeString(pstrdup(tag)));
		}

		if (separator == NULL)
			break;

		cursor = separator + 1;
	}

	pfree(copy);
	return tags;
}

static bool
qx_string_list_contains(List *values, const char *needle)
{
	ListCell   *lc;

	if (needle == NULL)
		return false;

	foreach(lc, values)
	{
		Node	   *node = lfirst(lc);

		if (IsA(node, String) && strcmp(strVal(node), needle) == 0)
			return true;
	}

	return false;
}

static char *
qx_policy_contract_value(const char *contract, const char *key)
{
	char	   *copy;
	char	   *cursor;
	char	   *result = NULL;
	size_t		key_len;

	if (contract == NULL || contract[0] == '\0' ||
		key == NULL || key[0] == '\0')
		return NULL;

	copy = pstrdup(contract);
	cursor = copy;
	key_len = strlen(key);
	while (cursor != NULL && *cursor != '\0')
	{
		char	   *separator;
		char	   *entry;
		char	   *end;

		separator = strchr(cursor, ';');
		if (separator != NULL)
			*separator = '\0';

		entry = cursor;
		while (*entry != '\0' && isspace((unsigned char) *entry))
			entry++;
		end = entry + strlen(entry);
		while (end > entry && isspace((unsigned char) *(end - 1)))
			*(--end) = '\0';

		if (strlen(entry) > key_len &&
			strncmp(entry, key, key_len) == 0 &&
			entry[key_len] == '=')
		{
			char	   *value = entry + key_len + 1;
			char	   *value_end;

			while (*value != '\0' && isspace((unsigned char) *value))
				value++;
			value_end = value + strlen(value);
			while (value_end > value &&
				   isspace((unsigned char) *(value_end - 1)))
				*(--value_end) = '\0';
			result = pstrdup(value);
			break;
		}

		if (separator == NULL)
			break;
		cursor = separator + 1;
	}

	pfree(copy);
	return result;
}

static List *
qx_default_capability_tags(const char *tool_name,
						   const char *handler_name,
						   const char *tool_sandbox,
						   const char *tool_sandbox_ceiling,
						   const char *principal_runtime,
						   const char *provider_kind,
						   const char *provider_attestation,
						   const char *receipt_alg)
{
	List	   *tags = NIL;
	const char *effective_tool = (tool_name != NULL && tool_name[0] != '\0') ?
		tool_name : "<unknown>";
	const char *effective_handler = (handler_name != NULL && handler_name[0] != '\0') ?
		handler_name : "<none>";
	const char *effective_sandbox = (tool_sandbox != NULL && tool_sandbox[0] != '\0') ?
		tool_sandbox : "builtin";
	const char *effective_ceiling = (tool_sandbox_ceiling != NULL &&
									 tool_sandbox_ceiling[0] != '\0') ?
		tool_sandbox_ceiling : effective_sandbox;
	const char *effective_runtime = (principal_runtime != NULL &&
									 principal_runtime[0] != '\0') ?
		principal_runtime : "host";
	const char *effective_provider = (provider_kind != NULL &&
									  provider_kind[0] != '\0') ?
		provider_kind : "loopback";
	const char *effective_attestation = (provider_attestation != NULL &&
										 provider_attestation[0] != '\0') ?
		provider_attestation : "optional";
	const char *effective_alg = (receipt_alg != NULL && receipt_alg[0] != '\0') ?
		receipt_alg : "hmac-sha256";

	tags = lappend(tags, makeString(psprintf("tool:%s", effective_tool)));
	tags = lappend(tags, makeString(psprintf("handler:%s", effective_handler)));
	tags = lappend(tags, makeString(psprintf("sandbox:%s", effective_sandbox)));
	tags = lappend(tags, makeString(psprintf("sandbox_ceiling:%s", effective_ceiling)));
	tags = lappend(tags, makeString(psprintf("runtime:%s", effective_runtime)));
	tags = lappend(tags, makeString(psprintf("provider_kind:%s", effective_provider)));
	tags = lappend(tags, makeString(psprintf("attestation:%s", effective_attestation)));
	tags = lappend(tags, makeString(psprintf("receipt_alg:%s", effective_alg)));

	return tags;
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
qx_tool_runtime_class_from_info(const QxCatalogToolInfo *tool)
{
	if (tool->runtime_class != NULL && tool->runtime_class[0] != '\0')
		return pstrdup(tool->runtime_class);
	if (tool->principal_runtime_class != NULL &&
		tool->principal_runtime_class[0] != '\0')
		return pstrdup(tool->principal_runtime_class);

	return pstrdup(qx_default_runtime_for_provider_kind(tool->provider_kind));
}

static char *
qx_tool_sandbox_ceiling_from_info(const QxCatalogToolInfo *tool)
{
	if (tool->sandbox_ceiling != NULL && tool->sandbox_ceiling[0] != '\0')
		return pstrdup(tool->sandbox_ceiling);
	if (tool->principal_sandbox != NULL && tool->principal_sandbox[0] != '\0')
		return pstrdup(tool->principal_sandbox);

	return pstrdup("builtin");
}

static char *
qx_tool_capability_tags_from_info(const QxCatalogToolInfo *tool,
								  const char *tool_runtime_class,
								  const char *tool_sandbox_ceiling)
{
	List	   *tags;

	if (tool->capability_tags != NULL && tool->capability_tags[0] != '\0')
	{
		tags = qx_split_capability_tags(tool->capability_tags);
		if (tags != NIL)
			return qx_join_string_list(tags, "|");
	}

	tags = qx_default_capability_tags(tool->name,
									  tool->handler,
									  tool->sandbox,
									  tool_sandbox_ceiling,
									  tool_runtime_class,
									  tool->provider_kind,
									  tool->provider_attestation_required ?
									  "required" : "optional",
									  tool->provider_receipt_alg);

	return qx_join_string_list(tags, "|");
}

static char *
qx_tool_contract_for_info(const QxCatalogToolInfo *tool,
						  const char *tool_runtime_class,
						  const char *tool_sandbox_ceiling,
						  const char *tool_capability_tags)
{
	char	   *provider_oid;
	char	   *contract;

	provider_oid = psprintf("%u", tool->provideroid);
	contract = psprintf("tool=%s;handler=%s;tool_sandbox=%s;tool_sandbox_ceiling=%s;tool_capability_tags=%s;principal=%s;principal_sandbox=%s;principal_runtime=%s;program=%s;receipt_signer=%s;provider=%s;provider_oid=%s;provider_kind=%s;provider_endpoint=%s;provider_attestation=%s;receipt_schema=qx.receipt.v1;receipt_alg=%s",
						tool->name != NULL ? tool->name : "<unknown>",
						tool->handler != NULL ? tool->handler : "<none>",
						tool->sandbox != NULL ? tool->sandbox : "builtin",
						tool_sandbox_ceiling != NULL ? tool_sandbox_ceiling : "builtin",
						tool_capability_tags != NULL ? tool_capability_tags : "",
						tool->principal_name != NULL ? tool->principal_name : "<unknown>",
						tool->principal_sandbox != NULL ? tool->principal_sandbox : "builtin",
						tool_runtime_class != NULL ? tool_runtime_class : "host",
						tool->principal_program != NULL ? tool->principal_program : "",
						tool->principal_receipt_signer != NULL ?
						tool->principal_receipt_signer : "",
						tool->provider_name != NULL ? tool->provider_name : "<unknown>",
						provider_oid,
						tool->provider_kind != NULL ? tool->provider_kind : "loopback",
						tool->provider_endpoint != NULL ?
						tool->provider_endpoint : "local://qhapaqxian-tool-runner",
						tool->provider_attestation_required ? "required" : "optional",
						tool->provider_receipt_alg != NULL ?
						tool->provider_receipt_alg : "hmac-sha256");

	pfree(provider_oid);
	return contract;
}

static void
qx_validate_tool_capability_policy(const QxCatalogNamespacePolicyInfo *policy,
								   const char *tool_name,
								   const char *tool_capability_tags)
{
	char	   *required_serialized;
	char	   *denied_serialized;
	List	   *tool_tags;
	List	   *required_tags;
	List	   *denied_tags;
	ListCell   *lc;
	const char *policy_name;
	const char *authorized_tags;

	Assert(policy != NULL);

	required_serialized =
		qx_policy_contract_value(policy->policy, "require_capabilities");
	if (required_serialized == NULL)
		required_serialized =
			qx_policy_contract_value(policy->policy, "require_capability");
	denied_serialized =
		qx_policy_contract_value(policy->policy, "deny_capabilities");
	if (denied_serialized == NULL)
		denied_serialized =
			qx_policy_contract_value(policy->policy, "deny_capability");

	if (required_serialized == NULL && denied_serialized == NULL)
		return;

	policy_name = policy->name != NULL ? policy->name : "<unknown>";
	authorized_tags = tool_capability_tags != NULL &&
		tool_capability_tags[0] != '\0' ? tool_capability_tags : "<none>";
	tool_tags = qx_split_capability_tags(tool_capability_tags);
	required_tags = qx_split_capability_tags(required_serialized);
	denied_tags = qx_split_capability_tags(denied_serialized);

	foreach(lc, required_tags)
	{
		const char *tag = strVal(lfirst(lc));

		if (!qx_string_list_contains(tool_tags, tag))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("tool \"%s\" does not satisfy capability requirement \"%s\" in namespace policy \"%s\"",
							tool_name != NULL ? tool_name : "<unknown>",
							tag,
							policy_name),
					 errdetail("Authorized capability tags: %s.",
							   authorized_tags)));
	}

	foreach(lc, denied_tags)
	{
		const char *tag = strVal(lfirst(lc));

		if (qx_string_list_contains(tool_tags, tag))
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("tool \"%s\" uses denied capability \"%s\" in namespace policy \"%s\"",
							tool_name != NULL ? tool_name : "<unknown>",
							tag,
							policy_name),
					 errdetail("Authorized capability tags: %s.",
							   authorized_tags)));
	}

	list_free_deep(tool_tags);
	list_free_deep(required_tags);
	list_free_deep(denied_tags);
	if (required_serialized != NULL)
		pfree(required_serialized);
	if (denied_serialized != NULL)
		pfree(denied_serialized);
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
	QxCatalogNamespacePolicyInfo policy;
	Oid			policyoid;

	if (policy_name == NULL || policy_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("namespace policy name must not be empty")));

	if (!QxCatalogLookupNamespacePolicyByName(namespaceoid, policy_name, &policy))
	{
		if (missing_ok)
			return InvalidOid;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("namespace policy \"%s\" does not exist in schema \"%s\"",
						policy_name, get_namespace_name(namespaceoid)),
				 errdetail("Create the namespace policy before binding agents or tools to it.")));
	}

	policyoid = policy.oid;
	QxCatalogFreeNamespacePolicyInfo(&policy);

	return policyoid;
}

Oid
QxLookupPrincipal(Oid namespaceoid, const char *principal_name,
				  bool missing_ok)
{
	QxCatalogPrincipalInfo principal;
	Oid			principaloid;

	if (principal_name == NULL || principal_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("principal name must not be empty")));

	if (!QxCatalogLookupPrincipalByName(namespaceoid, principal_name, &principal))
	{
		if (missing_ok)
			return InvalidOid;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("principal \"%s\" does not exist in schema \"%s\"",
						principal_name, get_namespace_name(namespaceoid)),
				 errdetail("Create the principal before binding it to a tool.")));
	}

	principaloid = principal.oid;
	QxCatalogFreePrincipalInfo(&principal);

	return principaloid;
}

Oid
QxLookupProvider(Oid namespaceoid, const char *provider_name,
				 bool missing_ok)
{
	QxCatalogProviderInfo provider;
	Oid			provideroid;

	if (provider_name == NULL || provider_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("provider name must not be empty")));

	if (!QxCatalogLookupProviderByName(namespaceoid, provider_name, &provider))
	{
		if (missing_ok)
			return InvalidOid;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("provider \"%s\" does not exist in schema \"%s\"",
						provider_name, get_namespace_name(namespaceoid)),
				 errdetail("Create the provider before binding it to a principal.")));
	}

	provideroid = provider.oid;
	QxCatalogFreeProviderInfo(&provider);

	return provideroid;
}

Oid
QxEnsureOperationalIdentity(Oid namespaceoid, Oid ownerid,
							const char *identity_name, Oid authrole,
							const char *policy_name, List *budget_options)
{
	QxCatalogIdentityInfo existing;
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

	if (QxCatalogLookupIdentityByName(namespaceoid, resolved_name, &existing))
	{
		if (!has_privs_of_role(ownerid, existing.ownerid))
		{
			QxCatalogFreeIdentityInfo(&existing);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to use identity \"%s\"",
							resolved_name),
					 errdetail("Only the identity owner or a member of that role may bind the identity to an agent.")));
		}

		identityoid = existing.oid;
		QxCatalogFreeIdentityInfo(&existing);
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
		QxCatalogToolInfo tool;

		if (!QxCatalogLookupToolByName(namespaceoid, tool_name, &tool))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tool \"%s\" is not registered in schema \"%s\"",
							tool_name, get_namespace_name(namespaceoid)),
					 errdetail("Register the tool with CREATE TOOL before referencing it from a namespace policy or agent.")));

		if (require_enabled)
		{
			char	   *tool_runtime_class;
			char	   *tool_sandbox_ceiling;
			char	   *tool_capability_tags;
			char	   *principal_runtime;

			if (!tool.enabled)
			{
				QxCatalogFreeToolInfo(&tool);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("tool \"%s\" is disabled in schema \"%s\"",
								tool_name, get_namespace_name(namespaceoid)),
						 errdetail("Enable the tool before binding it to an agent runtime.")));
			}

			if (!OidIsValid(tool.principaloid))
			{
				QxCatalogFreeToolInfo(&tool);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("tool \"%s\" has no principal binding", tool_name),
						 errdetail("Bind the tool to a namespace principal before enabling agent execution.")));
			}

			if (!tool.principal_enabled)
			{
				QxCatalogFreeToolInfo(&tool);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("principal for tool \"%s\" is disabled", tool_name),
						 errdetail("Enable the principal before binding the tool to an agent runtime.")));
			}

			if (!OidIsValid(tool.provideroid))
			{
				QxCatalogFreeToolInfo(&tool);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("principal for tool \"%s\" has no provider binding", tool_name),
						 errdetail("Bind the principal to a provider before enabling agent execution.")));
			}

			if (!tool.provider_enabled)
			{
				QxCatalogFreeToolInfo(&tool);
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("provider for tool \"%s\" is disabled", tool_name),
						 errdetail("Enable the provider before binding the tool to an agent runtime.")));
			}

			tool_runtime_class = qx_tool_runtime_class_from_info(&tool);
			tool_sandbox_ceiling = qx_tool_sandbox_ceiling_from_info(&tool);
			tool_capability_tags = qx_tool_capability_tags_from_info(&tool,
																	 tool_runtime_class,
																	 tool_sandbox_ceiling);
			principal_runtime = tool.principal_runtime_class != NULL &&
				tool.principal_runtime_class[0] != '\0' ?
				pstrdup(tool.principal_runtime_class) :
				pstrdup(qx_default_runtime_for_provider_kind(tool.provider_kind));
			if (principal_runtime == NULL || principal_runtime[0] == '\0')
			{
				if (principal_runtime != NULL)
					pfree(principal_runtime);
				principal_runtime =
					pstrdup(qx_default_runtime_for_provider_kind(tool.provider_kind));
			}
			if (qx_sandbox_rank(tool.sandbox) > qx_sandbox_rank(tool_sandbox_ceiling))
			{
				if (tool_runtime_class != NULL)
					pfree(tool_runtime_class);
				if (tool_sandbox_ceiling != NULL)
					pfree(tool_sandbox_ceiling);
				if (tool_capability_tags != NULL)
					pfree(tool_capability_tags);
				if (principal_runtime != NULL)
					pfree(principal_runtime);
				QxCatalogFreeToolInfo(&tool);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("tool \"%s\" exceeds its declared sandbox ceiling",
								tool_name),
						 errdetail("Tool sandbox \"%s\" is stricter than ceiling \"%s\".",
								   tool.sandbox != NULL ? tool.sandbox : "builtin",
								   tool_sandbox_ceiling != NULL ? tool_sandbox_ceiling : "builtin")));
			}
			if (strcmp(tool_runtime_class, principal_runtime) != 0)
			{
				if (tool_runtime_class != NULL)
					pfree(tool_runtime_class);
				if (tool_sandbox_ceiling != NULL)
					pfree(tool_sandbox_ceiling);
				if (tool_capability_tags != NULL)
					pfree(tool_capability_tags);
				if (principal_runtime != NULL)
					pfree(principal_runtime);
				QxCatalogFreeToolInfo(&tool);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("tool \"%s\" runtime class \"%s\" is incompatible with principal runtime \"%s\"",
								tool_name,
								   tool_runtime_class,
								   principal_runtime != NULL ? principal_runtime : "host"),
						 errdetail("Namespace authorization requires runtime-class alignment between the tool and the bound principal.")));
			}
			qx_validate_principal_runtime_binding(tool.principal_name,
												 tool.provider_kind,
												 principal_runtime,
												 tool.principal_sandbox,
												 tool.principal_receipt_signer,
												 tool.provider_receipt_alg,
												 "tool registration validation");

			if (tool_runtime_class != NULL)
				pfree(tool_runtime_class);
			if (tool_sandbox_ceiling != NULL)
				pfree(tool_sandbox_ceiling);
			if (tool_capability_tags != NULL)
				pfree(tool_capability_tags);
			if (principal_runtime != NULL)
				pfree(principal_runtime);
		}

		QxCatalogFreeToolInfo(&tool);
	}
}

char *
QxIdentityNameById(Oid identityoid)
{
	QxCatalogIdentityInfo identity;
	char	   *name;

	if (!QxCatalogLookupIdentityByOid(identityoid, &identity))
		elog(ERROR, "cache lookup failed for QhapaqXian identity %u", identityoid);

	name = pstrdup(identity.name);
	QxCatalogFreeIdentityInfo(&identity);

	return name;
}

char *
QxNamespacePolicyNameById(Oid policyoid)
{
	QxCatalogNamespacePolicyInfo policy;
	char	   *name;

	if (!QxCatalogLookupNamespacePolicyByOid(policyoid, &policy))
		elog(ERROR, "cache lookup failed for QhapaqXian namespace policy %u", policyoid);

	name = pstrdup(policy.name);
	QxCatalogFreeNamespacePolicyInfo(&policy);

	return name;
}

void
QxAuthorizeToolsForNamespace(Oid namespacepolicyoid, Oid namespaceoid,
							 Oid ownerid, List *tools,
							 QxToolAuthorization *authz)
{
	QxCatalogNamespacePolicyInfo policy;
	List	   *allowed_tools;
	ListCell   *lc;

	memset(authz, 0, sizeof(*authz));
	MemSet(&policy, 0, sizeof(policy));

	if (!QxCatalogLookupNamespacePolicyByOid(namespacepolicyoid, &policy))
		elog(ERROR, "cache lookup failed for QhapaqXian namespace policy %u",
			 namespacepolicyoid);

	if (!has_privs_of_role(ownerid, policy.ownerid) &&
		!has_privs_of_role(ownerid, policy.authrole))
	{
		QxCatalogFreeNamespacePolicyInfo(&policy);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to authorize tools in schema \"%s\"",
						get_namespace_name(namespaceoid)),
				 errdetail("The caller must be able to assume the namespace policy owner or auth role.")));
	}

	allowed_tools = QxDeserializeToolList(policy.allowed_tools);

	authz->namespace_policy_oid = policy.oid;
	authz->namespace_policy_name = pstrdup(policy.name);
	authz->require_known_tools = policy.require_known_tools;
	authz->enforce_budgets = policy.enforce_budgets;

	foreach(lc, tools)
	{
		const char *tool_name = strVal(lfirst(lc));
		QxCatalogToolInfo tool;
		char	   *tool_runtime_class;
		char	   *tool_sandbox_ceiling;
		char	   *tool_capability_tags;
		char	   *principal_runtime;
		char	   *contract;

		MemSet(&tool, 0, sizeof(tool));

		if (policy.require_known_tools &&
			!qx_tool_name_in_list(allowed_tools, tool_name))
		{
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("tool \"%s\" is not allowed by namespace policy \"%s\"",
							tool_name,
							authz->namespace_policy_name != NULL ?
							authz->namespace_policy_name : "<unknown>"),
					 errdetail("Register the tool in the namespace policy before executing the agent task.")));
		}

		if (!QxCatalogLookupToolByName(namespaceoid, tool_name, &tool))
		{
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("tool \"%s\" is not registered in schema \"%s\"",
							tool_name, get_namespace_name(namespaceoid))));
		}

		if (!tool.enabled)
		{
			QxCatalogFreeToolInfo(&tool);
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tool \"%s\" is disabled in schema \"%s\"",
							tool_name, get_namespace_name(namespaceoid))));
		}

		if (!OidIsValid(tool.principaloid))
		{
			QxCatalogFreeToolInfo(&tool);
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tool \"%s\" has no principal binding", tool_name)));
		}

		if (!tool.principal_enabled)
		{
			QxCatalogFreeToolInfo(&tool);
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("principal \"%s\" is disabled in schema \"%s\"",
							tool.principal_name != NULL ?
							tool.principal_name : "<unknown>",
							get_namespace_name(namespaceoid))));
		}

		if (!OidIsValid(tool.provideroid))
		{
			QxCatalogFreeToolInfo(&tool);
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("principal \"%s\" has no provider binding",
							tool.principal_name != NULL ?
							tool.principal_name : "<unknown>")));
		}

		if (!tool.provider_enabled)
		{
			QxCatalogFreeToolInfo(&tool);
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("provider \"%s\" is disabled in schema \"%s\"",
							tool.provider_name != NULL ?
							tool.provider_name : "<unknown>",
							get_namespace_name(namespaceoid))));
		}

		tool_runtime_class = qx_tool_runtime_class_from_info(&tool);
		tool_sandbox_ceiling = qx_tool_sandbox_ceiling_from_info(&tool);
		tool_capability_tags = qx_tool_capability_tags_from_info(&tool,
																 tool_runtime_class,
																 tool_sandbox_ceiling);
		principal_runtime = tool.principal_runtime_class != NULL &&
			tool.principal_runtime_class[0] != '\0' ?
			pstrdup(tool.principal_runtime_class) :
			pstrdup(qx_default_runtime_for_provider_kind(tool.provider_kind));
		if (principal_runtime == NULL || principal_runtime[0] == '\0')
		{
			if (principal_runtime != NULL)
				pfree(principal_runtime);
			principal_runtime =
				pstrdup(qx_default_runtime_for_provider_kind(tool.provider_kind));
		}
		if (qx_sandbox_rank(tool.sandbox) > qx_sandbox_rank(tool_sandbox_ceiling))
		{
			QxCatalogFreeToolInfo(&tool);
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool \"%s\" exceeds its declared sandbox ceiling",
							tool_name),
					 errdetail("Tool sandbox \"%s\" is stricter than ceiling \"%s\".",
							   tool.sandbox != NULL ? tool.sandbox : "builtin",
							   tool_sandbox_ceiling != NULL ? tool_sandbox_ceiling : "builtin")));
		}
		if (strcmp(tool_runtime_class, principal_runtime) != 0)
		{
			QxCatalogFreeToolInfo(&tool);
			QxCatalogFreeNamespacePolicyInfo(&policy);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool \"%s\" runtime class \"%s\" is incompatible with principal runtime \"%s\"",
							tool_name,
							tool_runtime_class,
							principal_runtime != NULL ? principal_runtime : "host"),
					 errdetail("Tool authorization requires runtime-class alignment before the contract is emitted.")));
		}
		qx_validate_principal_runtime_binding(tool.principal_name,
											 tool.provider_kind,
											 principal_runtime,
											 tool.principal_sandbox,
											 tool.principal_receipt_signer,
											 tool.provider_receipt_alg,
											 "namespace tool authorization");
		qx_validate_tool_capability_policy(&policy,
										   tool_name,
										   tool_capability_tags);

		contract = qx_tool_contract_for_info(&tool,
											 tool_runtime_class,
											 tool_sandbox_ceiling,
											 tool_capability_tags);
		authz->tool_count += 1;
		authz->tool_token_cost += tool.token_cost;
		authz->tool_cost_units += tool.cost_units;
		authz->tool_oids = lappend_oid(authz->tool_oids, tool.oid);
		authz->tool_contracts = lappend(authz->tool_contracts,
										makeString(contract));
		authz->tool_runtime_classes = lappend(authz->tool_runtime_classes,
											 makeString(pstrdup(tool_runtime_class)));
		authz->tool_sandbox_ceilings = lappend(authz->tool_sandbox_ceilings,
											  makeString(pstrdup(tool_sandbox_ceiling)));
		authz->tool_capability_tags = lappend(authz->tool_capability_tags,
											  makeString(tool_capability_tags));
		pfree(tool_runtime_class);
		pfree(tool_sandbox_ceiling);
		pfree(principal_runtime);
		QxCatalogFreeToolInfo(&tool);
	}

	QxCatalogFreeNamespacePolicyInfo(&policy);
}
