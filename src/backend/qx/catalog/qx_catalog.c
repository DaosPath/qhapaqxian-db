/*-------------------------------------------------------------------------
 *
 * qx_catalog.c
 *	  catalog helper snapshots for QhapaqXian Engine
 *
 *-------------------------------------------------------------------------*/
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_attempt.h"
#include "catalog/pg_qx_checkpoint.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_memory.h"
#include "catalog/pg_qx_namespace.h"
#include "catalog/pg_qx_principal.h"
#include "catalog/pg_qx_provider.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_event.h"
#include "catalog/pg_qx_scheduler_heartbeat.h"
#include "catalog/pg_qx_scheduler_lease.h"
#include "catalog/pg_qx_scheduler_queue.h"
#include "catalog/pg_qx_step.h"
#include "catalog/pg_qx_task.h"
#include "catalog/pg_qx_tool.h"
#include "catalog/pg_qx_trace.h"
#include "miscadmin.h"
#include "qx/qx_catalog.h"
#include "qx/qx_observe.h"
#include "qx/qx_scheduler.h"
#include "qx/qx_semantic_log.h"
#include "qx/qx_stat.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void qx_catalog_fill_agent_info(HeapTuple tup,
									   QxCatalogAgentInfo *info);
static void qx_catalog_fill_identity_info(HeapTuple tup,
										  QxCatalogIdentityInfo *info);
static void qx_catalog_fill_provider_info(HeapTuple tup,
										  QxCatalogProviderInfo *info);
static void qx_catalog_fill_principal_info(HeapTuple tup,
										   QxCatalogPrincipalInfo *info);
static void qx_catalog_fill_namespace_policy_info(HeapTuple tup,
												  QxCatalogNamespacePolicyInfo *info);
static void qx_catalog_fill_tool_info(HeapTuple tup, QxCatalogToolInfo *info);
static void qx_catalog_fill_session_info(HeapTuple tup,
										 QxCatalogSessionInfo *info);
static void qx_catalog_fill_task_info(HeapTuple tup, QxCatalogTaskInfo *info);
static void qx_catalog_fill_attempt_info(HeapTuple tup,
										 QxCatalogAttemptInfo *info);
static void qx_catalog_fill_checkpoint_info(HeapTuple tup,
											QxCatalogCheckpointInfo *info);
static HeapTuple qx_catalog_lookup_agent_tuple(Oid namespaceoid,
											   const char *name);
static HeapTuple qx_catalog_lookup_identity_tuple(Oid namespaceoid,
												  const char *name);
static HeapTuple qx_catalog_lookup_provider_tuple(Oid namespaceoid,
												  const char *name);
static HeapTuple qx_catalog_lookup_principal_tuple(Oid namespaceoid,
												   const char *name);
static HeapTuple qx_catalog_lookup_namespace_policy_tuple(Oid namespaceoid,
														  const char *name);
static HeapTuple qx_catalog_lookup_tool_tuple(Oid namespaceoid,
											  const char *name);
static bool qx_catalog_matches_filter(Oid rowdbid, Oid rowownerid,
									  Oid databaseoid, Oid ownerid);
static bool qx_catalog_matches_namespace_filter(Oid rownamespaceoid,
												Oid rowownerid,
												Oid namespaceoid, Oid ownerid);
static void qx_catalog_set_text_datum(Datum *values, bool *nulls,
									  AttrNumber attnum, const char *value);
static void qx_catalog_set_nodetree_datum(Datum *values, bool *nulls,
										  AttrNumber attnum, const void *node);
static char *qx_catalog_merge_optional_pair(const char *left,
											const char *right);

char *
QxCatalogTextAttr(HeapTuple tup, AttrNumber attnum, int cacheid)
{
	bool		isnull;
	Datum		datum;

	if (!HeapTupleIsValid(tup))
		return NULL;

	datum = SysCacheGetAttr(cacheid, tup, attnum, &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

char *
QxCatalogNameCopy(const NameData *name)
{
	if (name == NULL)
		return NULL;

	return pstrdup(NameStr(*name));
}

void
QxCatalogFreeString(char **ptr)
{
	if (ptr != NULL && *ptr != NULL)
	{
		pfree(*ptr);
		*ptr = NULL;
	}
}

static char *
qx_catalog_merge_optional_pair(const char *left, const char *right)
{
	if (left != NULL && left[0] != '\0')
		return pstrdup(left);

	if (right != NULL && right[0] != '\0')
		return pstrdup(right);

	return NULL;
}

static bool
qx_catalog_matches_filter(Oid rowdbid, Oid rowownerid,
						  Oid databaseoid, Oid ownerid)
{
	if (OidIsValid(databaseoid) && rowdbid != databaseoid)
		return false;

	if (OidIsValid(ownerid) && rowownerid != ownerid)
		return false;

	return true;
}

static bool
qx_catalog_matches_namespace_filter(Oid rownamespaceoid, Oid rowownerid,
									Oid namespaceoid, Oid ownerid)
{
	if (OidIsValid(namespaceoid) && rownamespaceoid != namespaceoid)
		return false;

	if (OidIsValid(ownerid) && rowownerid != ownerid)
		return false;

	return true;
}

static void
qx_catalog_set_text_datum(Datum *values, bool *nulls, AttrNumber attnum,
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
qx_catalog_set_nodetree_datum(Datum *values, bool *nulls, AttrNumber attnum,
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
qx_catalog_fill_agent_info(HeapTuple tup, QxCatalogAgentInfo *info)
{
	Form_pg_qx_agent form;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	form = (Form_pg_qx_agent) GETSTRUCT(tup);
	info->oid = form->oid;
	info->name = QxCatalogNameCopy(&form->qxagentname);
	info->namespaceoid = form->qxagentnamespace;
	info->namespacepolicyoid = form->qxnamespacepolicyid;
	info->identityoid = form->qxidentityid;
	info->ownerid = form->qxagentowner;
	info->identity = QxCatalogTextAttr(tup, Anum_pg_qx_agent_qxidentity,
									   QXAGENTOID);
	info->model_uri = QxCatalogTextAttr(tup, Anum_pg_qx_agent_qxmodeluri,
										QXAGENTOID);
	info->memory_profile = QxCatalogTextAttr(tup,
											 Anum_pg_qx_agent_qxmemoryprofile,
											 QXAGENTOID);
	info->policy = QxCatalogTextAttr(tup, Anum_pg_qx_agent_qxpolicy,
									 QXAGENTOID);
	info->tools = QxCatalogTextAttr(tup, Anum_pg_qx_agent_qxtools,
									QXAGENTOID);
	info->budget = QxCatalogTextAttr(tup, Anum_pg_qx_agent_qxbudget,
									 QXAGENTOID);
}

static void
qx_catalog_fill_identity_info(HeapTuple tup, QxCatalogIdentityInfo *info)
{
	Form_pg_qx_identity form;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	form = (Form_pg_qx_identity) GETSTRUCT(tup);
	info->oid = form->oid;
	info->name = QxCatalogNameCopy(&form->qxidentityname);
	info->namespaceoid = form->qxidentitynamespace;
	info->ownerid = form->qxidentityowner;
	info->authrole = form->qxidentityauthrole;
	info->policy = QxCatalogTextAttr(tup, Anum_pg_qx_identity_qxidentitypolicy,
									 QXIDENTITYOID);
	info->budget = QxCatalogTextAttr(tup, Anum_pg_qx_identity_qxidentitybudget,
									 QXIDENTITYOID);
}

static void
qx_catalog_fill_provider_info(HeapTuple tup, QxCatalogProviderInfo *info)
{
	Form_pg_qx_provider form;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	form = (Form_pg_qx_provider) GETSTRUCT(tup);
	info->oid = form->oid;
	info->name = QxCatalogNameCopy(&form->qxprovidername);
	info->namespaceoid = form->qxprovidernamespace;
	info->ownerid = form->qxproviderowner;
	info->enabled = form->qxproviderenabled;
	info->attestation_required = form->qxproviderattestationrequired;
	info->kind = QxCatalogTextAttr(tup, Anum_pg_qx_provider_qxproviderkind,
								   QXPROVIDEROID);
	info->endpoint = QxCatalogTextAttr(tup,
									   Anum_pg_qx_provider_qxproviderendpoint,
									   QXPROVIDEROID);
	info->receipt_alg = QxCatalogTextAttr(tup,
										  Anum_pg_qx_provider_qxproviderreceiptalg,
										  QXPROVIDEROID);
	info->receipt_key = QxCatalogTextAttr(tup,
										  Anum_pg_qx_provider_qxproviderreceiptkey,
										  QXPROVIDEROID);
	info->attestation_profile = QxCatalogTextAttr(tup,
												  Anum_pg_qx_provider_qxproviderattestationprofile,
												  QXPROVIDEROID);
	info->attestation_version = QxCatalogTextAttr(tup,
												  Anum_pg_qx_provider_qxproviderattestationversion,
												  QXPROVIDEROID);
	info->attestation_policy = QxCatalogTextAttr(tup,
												 Anum_pg_qx_provider_qxproviderattestationpolicy,
												 QXPROVIDEROID);
}

static void
qx_catalog_fill_principal_info(HeapTuple tup, QxCatalogPrincipalInfo *info)
{
	Form_pg_qx_principal form;
	QxCatalogProviderInfo provider;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	MemSet(&provider, 0, sizeof(provider));
	form = (Form_pg_qx_principal) GETSTRUCT(tup);
	info->oid = form->oid;
	info->name = QxCatalogNameCopy(&form->qxprincipalname);
	info->namespaceoid = form->qxprincipalnamespace;
	info->ownerid = form->qxprincipalowner;
	info->provideroid = form->qxprincipalproviderid;
	info->enabled = form->qxprincipalenabled;
	info->sandbox = QxCatalogTextAttr(tup, Anum_pg_qx_principal_qxprincipalsandbox,
									  QXPRINCIPALOID);
	info->program = QxCatalogTextAttr(tup, Anum_pg_qx_principal_qxprincipalprogram,
									  QXPRINCIPALOID);
	info->provider_name = QxCatalogTextAttr(tup,
											Anum_pg_qx_principal_qxprincipalprovider,
											QXPRINCIPALOID);
	info->runtime_class = QxCatalogTextAttr(tup,
											Anum_pg_qx_principal_qxprincipalruntimeclass,
											QXPRINCIPALOID);
	info->receipt_signer = QxCatalogTextAttr(tup,
											 Anum_pg_qx_principal_qxprincipalreceiptsigner,
											 QXPRINCIPALOID);
	info->attestation_profile = QxCatalogTextAttr(tup,
												  Anum_pg_qx_principal_qxprincipalattestationprofile,
												  QXPRINCIPALOID);
	info->attestation_version = QxCatalogTextAttr(tup,
												  Anum_pg_qx_principal_qxprincipalattestationversion,
												  QXPRINCIPALOID);
	info->attestation_policy = QxCatalogTextAttr(tup,
												 Anum_pg_qx_principal_qxprincipalattestationpolicy,
												 QXPRINCIPALOID);

	if (OidIsValid(info->provideroid) &&
		QxCatalogLookupProviderByOid(info->provideroid, &provider))
	{
		info->provider_name = qx_catalog_merge_optional_pair(info->provider_name,
															 provider.name);
		info->provider_kind = provider.kind;
		info->provider_endpoint = provider.endpoint;
		info->provider_receipt_alg = provider.receipt_alg;
		info->provider_enabled = provider.enabled;
		info->provider_attestation_required = provider.attestation_required;
		info->attestation_profile = qx_catalog_merge_optional_pair(info->attestation_profile,
																   provider.attestation_profile);
		info->attestation_version = qx_catalog_merge_optional_pair(info->attestation_version,
																   provider.attestation_version);
		info->attestation_policy = qx_catalog_merge_optional_pair(info->attestation_policy,
																  provider.attestation_policy);
		provider.name = NULL;
		provider.kind = NULL;
		provider.endpoint = NULL;
		provider.receipt_alg = NULL;
		provider.attestation_profile = NULL;
		provider.attestation_version = NULL;
		provider.attestation_policy = NULL;
		QxCatalogFreeProviderInfo(&provider);
	}
}

static HeapTuple
qx_catalog_lookup_agent_tuple(Oid namespaceoid, const char *name)
{
	return SearchSysCache2(QXAGENTNAMENSP,
						   CStringGetDatum(name),
						   ObjectIdGetDatum(namespaceoid));
}

static HeapTuple
qx_catalog_lookup_identity_tuple(Oid namespaceoid, const char *name)
{
	return SearchSysCache2(QXIDENTITYNAMENSP,
						   CStringGetDatum(name),
						   ObjectIdGetDatum(namespaceoid));
}

static HeapTuple
qx_catalog_lookup_provider_tuple(Oid namespaceoid, const char *name)
{
	return SearchSysCache2(QXPROVIDERNAMENSP,
						   CStringGetDatum(name),
						   ObjectIdGetDatum(namespaceoid));
}

static HeapTuple
qx_catalog_lookup_principal_tuple(Oid namespaceoid, const char *name)
{
	return SearchSysCache2(QXPRINCIPALNAMENSP,
						   CStringGetDatum(name),
						   ObjectIdGetDatum(namespaceoid));
}

void
QxCatalogFreeAgentInfo(QxCatalogAgentInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->name);
	QxCatalogFreeString(&info->identity);
	QxCatalogFreeString(&info->model_uri);
	QxCatalogFreeString(&info->memory_profile);
	QxCatalogFreeString(&info->policy);
	QxCatalogFreeString(&info->tools);
	QxCatalogFreeString(&info->budget);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeIdentityInfo(QxCatalogIdentityInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->name);
	QxCatalogFreeString(&info->policy);
	QxCatalogFreeString(&info->budget);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeProviderInfo(QxCatalogProviderInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->name);
	QxCatalogFreeString(&info->kind);
	QxCatalogFreeString(&info->endpoint);
	QxCatalogFreeString(&info->receipt_alg);
	QxCatalogFreeString(&info->receipt_key);
	QxCatalogFreeString(&info->attestation_profile);
	QxCatalogFreeString(&info->attestation_version);
	QxCatalogFreeString(&info->attestation_policy);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreePrincipalInfo(QxCatalogPrincipalInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->name);
	QxCatalogFreeString(&info->sandbox);
	QxCatalogFreeString(&info->program);
	QxCatalogFreeString(&info->provider_name);
	QxCatalogFreeString(&info->provider_kind);
	QxCatalogFreeString(&info->provider_endpoint);
	QxCatalogFreeString(&info->provider_receipt_alg);
	QxCatalogFreeString(&info->runtime_class);
	QxCatalogFreeString(&info->receipt_signer);
	QxCatalogFreeString(&info->attestation_profile);
	QxCatalogFreeString(&info->attestation_version);
	QxCatalogFreeString(&info->attestation_policy);
	MemSet(info, 0, sizeof(*info));
}

bool
QxCatalogLookupAgentByOid(Oid agentoid, QxCatalogAgentInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(agentoid))
		return false;

	tup = SearchSysCache1(QXAGENTOID, ObjectIdGetDatum(agentoid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_agent_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupAgentByName(Oid namespaceoid, const char *name,
						   QxCatalogAgentInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(namespaceoid) || name == NULL || name[0] == '\0')
		return false;

	tup = qx_catalog_lookup_agent_tuple(namespaceoid, name);
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_agent_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupIdentityByOid(Oid identityoid, QxCatalogIdentityInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(identityoid))
		return false;

	tup = SearchSysCache1(QXIDENTITYOID, ObjectIdGetDatum(identityoid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_identity_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupIdentityByName(Oid namespaceoid, const char *name,
							  QxCatalogIdentityInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(namespaceoid) || name == NULL || name[0] == '\0')
		return false;

	tup = qx_catalog_lookup_identity_tuple(namespaceoid, name);
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_identity_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupProviderByOid(Oid provideroid, QxCatalogProviderInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(provideroid))
		return false;

	tup = SearchSysCache1(QXPROVIDEROID, ObjectIdGetDatum(provideroid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_provider_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupProviderByName(Oid namespaceoid, const char *name,
							  QxCatalogProviderInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(namespaceoid) || name == NULL || name[0] == '\0')
		return false;

	tup = qx_catalog_lookup_provider_tuple(namespaceoid, name);
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_provider_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupPrincipalByOid(Oid principaloid, QxCatalogPrincipalInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(principaloid))
		return false;

	tup = SearchSysCache1(QXPRINCIPALOID, ObjectIdGetDatum(principaloid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_principal_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupPrincipalByName(Oid namespaceoid, const char *name,
							   QxCatalogPrincipalInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(namespaceoid) || name == NULL || name[0] == '\0')
		return false;

	tup = qx_catalog_lookup_principal_tuple(namespaceoid, name);
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_principal_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

static void
qx_catalog_fill_namespace_policy_info(HeapTuple tup,
									  QxCatalogNamespacePolicyInfo *info)
{
	Form_pg_qx_namespace form;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	form = (Form_pg_qx_namespace) GETSTRUCT(tup);
	info->oid = form->oid;
	info->name = QxCatalogNameCopy(&form->qxnamespacepolicyname);
	info->namespaceoid = form->qxnamespaceid;
	info->ownerid = form->qxnamespaceowner;
	info->authrole = form->qxnamespaceauthrole;
	info->require_known_tools = form->qxrequireknowntools;
	info->enforce_budgets = form->qxenforcebudgets;
	info->policy = QxCatalogTextAttr(tup,
									 Anum_pg_qx_namespace_qxnamespacepolicy,
									 QXNAMESPACEOID);
	info->allowed_tools = QxCatalogTextAttr(tup,
											Anum_pg_qx_namespace_qxallowedtools,
											QXNAMESPACEOID);
}

static void
qx_catalog_fill_tool_info(HeapTuple tup, QxCatalogToolInfo *info)
{
	Form_pg_qx_tool form;
	QxCatalogPrincipalInfo principal;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	MemSet(&principal, 0, sizeof(principal));

	form = (Form_pg_qx_tool) GETSTRUCT(tup);
	info->oid = form->oid;
	info->name = QxCatalogNameCopy(&form->qxtoolname);
	info->namespaceoid = form->qxtoolnamespace;
	info->ownerid = form->qxtoolowner;
	info->principaloid = form->qxtoolprincipalid;
	info->enabled = form->qxtoolenabled;
	info->token_cost = form->qxtooltokencost;
	info->cost_units = form->qxtoolcostunits;
	info->handler = QxCatalogTextAttr(tup, Anum_pg_qx_tool_qxtoolhandler,
									  QXTOOLOID);
	info->sandbox = QxCatalogTextAttr(tup, Anum_pg_qx_tool_qxtoolsandbox,
									  QXTOOLOID);
	info->runtime_class = QxCatalogTextAttr(tup,
											Anum_pg_qx_tool_qxtoolruntimeclass,
											QXTOOLOID);
	info->sandbox_ceiling = QxCatalogTextAttr(tup,
											 Anum_pg_qx_tool_qxtoolsandboxceiling,
											 QXTOOLOID);
	info->capability_tags = QxCatalogTextAttr(tup,
											  Anum_pg_qx_tool_qxtoolcapabilitytags,
											  QXTOOLOID);
	info->principal_name = QxCatalogTextAttr(tup,
											Anum_pg_qx_tool_qxtoolprincipal,
											QXTOOLOID);
	info->policy = QxCatalogTextAttr(tup, Anum_pg_qx_tool_qxtoolpolicy,
									 QXTOOLOID);

	if (OidIsValid(info->principaloid) &&
		QxCatalogLookupPrincipalByOid(info->principaloid, &principal))
	{
		info->principal_name = qx_catalog_merge_optional_pair(info->principal_name,
															  principal.name);
		info->principal_enabled = principal.enabled;
		info->principal_sandbox = principal.sandbox;
		info->principal_runtime_class = principal.runtime_class;
		info->principal_program = principal.program;
		info->principal_receipt_signer = principal.receipt_signer;
		info->provideroid = principal.provideroid;
		info->provider_name = principal.provider_name;
		info->provider_kind = principal.provider_kind;
		info->provider_endpoint = principal.provider_endpoint;
		info->provider_receipt_alg = principal.provider_receipt_alg;
		info->provider_enabled = principal.provider_enabled;
		info->provider_attestation_required = principal.provider_attestation_required;
		principal.name = NULL;
		principal.sandbox = NULL;
		principal.runtime_class = NULL;
		principal.program = NULL;
		principal.receipt_signer = NULL;
		principal.provider_name = NULL;
		principal.provider_kind = NULL;
		principal.provider_endpoint = NULL;
		principal.provider_receipt_alg = NULL;
		principal.attestation_profile = NULL;
		principal.attestation_version = NULL;
		principal.attestation_policy = NULL;
		QxCatalogFreePrincipalInfo(&principal);
	}
}

static void
qx_catalog_fill_session_info(HeapTuple tup, QxCatalogSessionInfo *info)
{
	Form_pg_qx_session form;
	QxCatalogAgentInfo agent;
	QxCatalogIdentityInfo identity;
	QxCatalogNamespacePolicyInfo policy;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	MemSet(&agent, 0, sizeof(agent));
	MemSet(&identity, 0, sizeof(identity));
	MemSet(&policy, 0, sizeof(policy));

	form = (Form_pg_qx_session) GETSTRUCT(tup);
	info->oid = form->oid;
	info->dbid = form->qxsessiondbid;
	info->agentoid = form->qxsessionagentid;
	info->namespacepolicyoid = form->qxsessionnamespacepolicyid;
	info->identityoid = form->qxsessionidentityid;
	info->ownerid = form->qxsessionowner;
	info->status = form->qxsessionstatus;
	info->context = QxCatalogTextAttr(tup, Anum_pg_qx_session_qxcontext,
									  QXSESSIONOID);

	if (OidIsValid(info->agentoid) &&
		QxCatalogLookupAgentByOid(info->agentoid, &agent))
	{
		info->agent_name = agent.name;
		agent.name = NULL;
		QxCatalogFreeAgentInfo(&agent);
	}

	if (OidIsValid(info->identityoid) &&
		QxCatalogLookupIdentityByOid(info->identityoid, &identity))
	{
		info->identity_name = identity.name;
		identity.name = NULL;
		QxCatalogFreeIdentityInfo(&identity);
	}

	if (OidIsValid(info->namespacepolicyoid) &&
		QxCatalogLookupNamespacePolicyByOid(info->namespacepolicyoid, &policy))
	{
		info->policy_name = policy.name;
		policy.name = NULL;
		QxCatalogFreeNamespacePolicyInfo(&policy);
	}
}

static void
qx_catalog_fill_task_info(HeapTuple tup, QxCatalogTaskInfo *info)
{
	Form_pg_qx_task form;
	QxCatalogAgentInfo agent;
	QxCatalogIdentityInfo identity;
	QxCatalogNamespacePolicyInfo policy;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	MemSet(&agent, 0, sizeof(agent));
	MemSet(&identity, 0, sizeof(identity));
	MemSet(&policy, 0, sizeof(policy));

	form = (Form_pg_qx_task) GETSTRUCT(tup);
	info->oid = form->oid;
	info->dbid = form->qxtaskdbid;
	info->sessionoid = form->qxtasksessionid;
	info->agentoid = form->qxtaskagentid;
	info->namespacepolicyoid = form->qxtasknamespacepolicyid;
	info->identityoid = form->qxtaskidentityid;
	info->ownerid = form->qxtaskowner;
	info->lastattemptid = form->qxtasklastattemptid;
	info->lastcheckpointid = form->qxtasklastcheckpointid;
	info->budget_tokens = form->qxtaskbudgettokens;
	info->budget_cost = form->qxtaskbudgetcost;
	info->authorized_tool_tokens = form->qxtaskauthorizedtooltokens;
	info->authorized_tool_cost = form->qxtaskauthorizedtoolcost;
	info->estimated_tokens = form->qxtaskestimatedtokens;
	info->estimated_cost = form->qxtaskestimatedcost;
	info->consumed_tokens = form->qxtaskconsumedtokens;
	info->consumed_cost = form->qxtaskconsumedcost;
	info->state = form->qxtaskstate;
	info->name = QxCatalogTextAttr(tup, Anum_pg_qx_task_qxtaskname, QXTASKOID);
	info->goal = QxCatalogTextAttr(tup, Anum_pg_qx_task_qxtaskgoal, QXTASKOID);
	info->input = QxCatalogTextAttr(tup, Anum_pg_qx_task_qxtaskinput, QXTASKOID);
	info->priority = QxCatalogTextAttr(tup, Anum_pg_qx_task_qxtaskpriority,
									   QXTASKOID);
	info->authorized_tools = QxCatalogTextAttr(tup,
											  Anum_pg_qx_task_qxtaskauthorizedtools,
											  QXTASKOID);
	info->submit_contract = QxCatalogTextAttr(tup,
											  Anum_pg_qx_task_qxtasksubmitcontract,
											  QXTASKOID);
	info->resume_contract = QxCatalogTextAttr(tup,
											  Anum_pg_qx_task_qxtaskresumecontract,
											  QXTASKOID);

	if (OidIsValid(info->agentoid) &&
		QxCatalogLookupAgentByOid(info->agentoid, &agent))
	{
		info->agent_name = agent.name;
		agent.name = NULL;
		QxCatalogFreeAgentInfo(&agent);
	}

	if (OidIsValid(info->identityoid) &&
		QxCatalogLookupIdentityByOid(info->identityoid, &identity))
	{
		info->identity_name = identity.name;
		identity.name = NULL;
		QxCatalogFreeIdentityInfo(&identity);
	}

	if (OidIsValid(info->namespacepolicyoid) &&
		QxCatalogLookupNamespacePolicyByOid(info->namespacepolicyoid, &policy))
	{
		info->policy_name = policy.name;
		policy.name = NULL;
		QxCatalogFreeNamespacePolicyInfo(&policy);
	}
}

static void
qx_catalog_fill_attempt_info(HeapTuple tup, QxCatalogAttemptInfo *info)
{
	Form_pg_qx_attempt form;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	form = (Form_pg_qx_attempt) GETSTRUCT(tup);
	info->oid = form->oid;
	info->dbid = form->qxattemptdbid;
	info->sessionoid = form->qxattemptsessionid;
	info->taskoid = form->qxattempttaskid;
	info->ownerid = form->qxattemptowner;
	info->resumecheckpointid = form->qxattemptresumecheckpointid;
	info->seqno = form->qxattemptseqno;
	info->state = form->qxattemptstate;
	info->strategy = QxCatalogTextAttr(tup,
									   Anum_pg_qx_attempt_qxattemptstrategy,
									   QXATTEMPTOID);
}

static void
qx_catalog_fill_checkpoint_info(HeapTuple tup, QxCatalogCheckpointInfo *info)
{
	Form_pg_qx_checkpoint form;

	Assert(HeapTupleIsValid(tup));
	Assert(info != NULL);

	form = (Form_pg_qx_checkpoint) GETSTRUCT(tup);
	info->oid = form->oid;
	info->dbid = form->qxcheckpointdbid;
	info->sessionoid = form->qxcheckpointsessionid;
	info->taskoid = form->qxcheckpointtaskid;
	info->attemptoid = form->qxcheckpointattemptid;
	info->stepoid = form->qxcheckpointstepid;
	info->ownerid = form->qxcheckpointowner;
	info->state = form->qxcheckpointstate;
	info->task_state = form->qxcheckpointtaskstate;
	info->next_step_seqno = form->qxcheckpointnextstepseqno;
	info->lsn = form->qxcheckpointlsn;
	info->label = QxCatalogTextAttr(tup,
									Anum_pg_qx_checkpoint_qxcheckpointlabel,
									QXCHECKPOINTOID);
	info->data = QxCatalogTextAttr(tup,
								   Anum_pg_qx_checkpoint_qxcheckpointdata,
								   QXCHECKPOINTOID);
}

static HeapTuple
qx_catalog_lookup_namespace_policy_tuple(Oid namespaceoid, const char *name)
{
	return SearchSysCache2(QXNAMESPACENAMENSP,
						   CStringGetDatum(name),
						   ObjectIdGetDatum(namespaceoid));
}

static HeapTuple
qx_catalog_lookup_tool_tuple(Oid namespaceoid, const char *name)
{
	return SearchSysCache2(QXTOOLNAMENSP,
						   CStringGetDatum(name),
						   ObjectIdGetDatum(namespaceoid));
}

void
QxCatalogFreeNamespacePolicyInfo(QxCatalogNamespacePolicyInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->name);
	QxCatalogFreeString(&info->policy);
	QxCatalogFreeString(&info->allowed_tools);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeToolInfo(QxCatalogToolInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->name);
	QxCatalogFreeString(&info->handler);
	QxCatalogFreeString(&info->sandbox);
	QxCatalogFreeString(&info->runtime_class);
	QxCatalogFreeString(&info->sandbox_ceiling);
	QxCatalogFreeString(&info->capability_tags);
	QxCatalogFreeString(&info->principal_name);
	QxCatalogFreeString(&info->principal_sandbox);
	QxCatalogFreeString(&info->principal_runtime_class);
	QxCatalogFreeString(&info->principal_program);
	QxCatalogFreeString(&info->principal_receipt_signer);
	QxCatalogFreeString(&info->provider_name);
	QxCatalogFreeString(&info->provider_kind);
	QxCatalogFreeString(&info->provider_endpoint);
	QxCatalogFreeString(&info->provider_receipt_alg);
	QxCatalogFreeString(&info->policy);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeSessionInfo(QxCatalogSessionInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->context);
	QxCatalogFreeString(&info->agent_name);
	QxCatalogFreeString(&info->identity_name);
	QxCatalogFreeString(&info->policy_name);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeTaskInfo(QxCatalogTaskInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->name);
	QxCatalogFreeString(&info->goal);
	QxCatalogFreeString(&info->input);
	QxCatalogFreeString(&info->priority);
	QxCatalogFreeString(&info->authorized_tools);
	QxCatalogFreeString(&info->submit_contract);
	QxCatalogFreeString(&info->resume_contract);
	QxCatalogFreeString(&info->agent_name);
	QxCatalogFreeString(&info->identity_name);
	QxCatalogFreeString(&info->policy_name);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeAttemptInfo(QxCatalogAttemptInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->strategy);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeCheckpointInfo(QxCatalogCheckpointInfo *info)
{
	Assert(info != NULL);

	QxCatalogFreeString(&info->label);
	QxCatalogFreeString(&info->data);
	MemSet(info, 0, sizeof(*info));
}

void
QxCatalogFreeTaskInfoList(List *tasks)
{
	ListCell   *lc;

	foreach(lc, tasks)
	{
		QxCatalogTaskInfo *info = lfirst(lc);

		QxCatalogFreeTaskInfo(info);
		pfree(info);
	}

	list_free(tasks);
}

void
QxCatalogFreeAttemptInfoList(List *attempts)
{
	ListCell   *lc;

	foreach(lc, attempts)
	{
		QxCatalogAttemptInfo *info = lfirst(lc);

		QxCatalogFreeAttemptInfo(info);
		pfree(info);
	}

	list_free(attempts);
}

void
QxCatalogFreeCheckpointInfoList(List *checkpoints)
{
	ListCell   *lc;

	foreach(lc, checkpoints)
	{
		QxCatalogCheckpointInfo *info = lfirst(lc);

		QxCatalogFreeCheckpointInfo(info);
		pfree(info);
	}

	list_free(checkpoints);
}

void
QxCatalogFreeProviderInfoList(List *providers)
{
	ListCell   *lc;

	foreach(lc, providers)
	{
		QxCatalogProviderInfo *info = lfirst(lc);

		QxCatalogFreeProviderInfo(info);
		pfree(info);
	}

	list_free(providers);
}

void
QxCatalogFreePrincipalInfoList(List *principals)
{
	ListCell   *lc;

	foreach(lc, principals)
	{
		QxCatalogPrincipalInfo *info = lfirst(lc);

		QxCatalogFreePrincipalInfo(info);
		pfree(info);
	}

	list_free(principals);
}

void
QxCatalogFreeStringList(List *strings)
{
	ListCell   *lc;

	foreach(lc, strings)
	{
		char	   *value = lfirst(lc);

		if (value != NULL)
			pfree(value);
	}

	list_free(strings);
}

bool
QxCatalogLookupNamespacePolicyByOid(Oid policyoid,
									QxCatalogNamespacePolicyInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(policyoid))
		return false;

	tup = SearchSysCache1(QXNAMESPACEOID, ObjectIdGetDatum(policyoid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_namespace_policy_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupNamespacePolicyByName(Oid namespaceoid, const char *name,
									 QxCatalogNamespacePolicyInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(namespaceoid) || name == NULL || name[0] == '\0')
		return false;

	tup = qx_catalog_lookup_namespace_policy_tuple(namespaceoid, name);
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_namespace_policy_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupToolByOid(Oid tooloid, QxCatalogToolInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(tooloid))
		return false;

	tup = SearchSysCache1(QXTOOLOID, ObjectIdGetDatum(tooloid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_tool_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupToolByName(Oid namespaceoid, const char *name,
						  QxCatalogToolInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(namespaceoid) || name == NULL || name[0] == '\0')
		return false;

	tup = qx_catalog_lookup_tool_tuple(namespaceoid, name);
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_tool_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupSessionByOid(Oid sessionoid, QxCatalogSessionInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(sessionoid))
		return false;

	tup = SearchSysCache1(QXSESSIONOID, ObjectIdGetDatum(sessionoid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_session_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupTaskByOid(Oid taskoid, QxCatalogTaskInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(taskoid))
		return false;

	tup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_task_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupAttemptByOid(Oid attemptoid, QxCatalogAttemptInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(attemptoid))
		return false;

	tup = SearchSysCache1(QXATTEMPTOID, ObjectIdGetDatum(attemptoid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_attempt_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

bool
QxCatalogLookupCheckpointByOid(Oid checkpointoid,
							   QxCatalogCheckpointInfo *info)
{
	HeapTuple	tup;

	Assert(info != NULL);
	MemSet(info, 0, sizeof(*info));

	if (!OidIsValid(checkpointoid))
		return false;

	tup = SearchSysCache1(QXCHECKPOINTOID, ObjectIdGetDatum(checkpointoid));
	if (!HeapTupleIsValid(tup))
		return false;

	qx_catalog_fill_checkpoint_info(tup, info);
	ReleaseSysCache(tup);
	return true;
}

List *
QxCatalogBuildTaskInfoList(Oid databaseoid, Oid ownerid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *tasks = NIL;

	rel = table_open(QxTaskRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_task form = (Form_pg_qx_task) GETSTRUCT(tup);
		QxCatalogTaskInfo *info;

		if (!qx_catalog_matches_filter(form->qxtaskdbid,
									   form->qxtaskowner,
									   databaseoid,
									   ownerid))
			continue;

		info = palloc0(sizeof(QxCatalogTaskInfo));
		qx_catalog_fill_task_info(tup, info);
		tasks = lappend(tasks, info);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return tasks;
}

List *
QxCatalogBuildAttemptInfoList(Oid databaseoid, Oid ownerid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *attempts = NIL;

	rel = table_open(QxAttemptRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_attempt form = (Form_pg_qx_attempt) GETSTRUCT(tup);
		QxCatalogAttemptInfo *info;

		if (!qx_catalog_matches_filter(form->qxattemptdbid,
									   form->qxattemptowner,
									   databaseoid,
									   ownerid))
			continue;

		info = palloc0(sizeof(QxCatalogAttemptInfo));
		qx_catalog_fill_attempt_info(tup, info);
		attempts = lappend(attempts, info);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return attempts;
}

List *
QxCatalogBuildCheckpointInfoList(Oid databaseoid, Oid ownerid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *checkpoints = NIL;

	rel = table_open(QxCheckpointRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_checkpoint form = (Form_pg_qx_checkpoint) GETSTRUCT(tup);
		QxCatalogCheckpointInfo *info;

		if (!qx_catalog_matches_filter(form->qxcheckpointdbid,
									   form->qxcheckpointowner,
									   databaseoid,
									   ownerid))
			continue;

		info = palloc0(sizeof(QxCatalogCheckpointInfo));
		qx_catalog_fill_checkpoint_info(tup, info);
		checkpoints = lappend(checkpoints, info);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return checkpoints;
}

List *
QxCatalogBuildProviderInfoList(Oid namespaceoid, Oid ownerid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *providers = NIL;

	rel = table_open(QxProviderRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_provider form = (Form_pg_qx_provider) GETSTRUCT(tup);
		QxCatalogProviderInfo *info;

		if (!qx_catalog_matches_namespace_filter(form->qxprovidernamespace,
												 form->qxproviderowner,
												 namespaceoid,
												 ownerid))
			continue;

		info = palloc0(sizeof(QxCatalogProviderInfo));
		qx_catalog_fill_provider_info(tup, info);
		providers = lappend(providers, info);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return providers;
}

List *
QxCatalogBuildPrincipalInfoList(Oid namespaceoid, Oid ownerid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	List	   *principals = NIL;

	rel = table_open(QxPrincipalRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_principal form = (Form_pg_qx_principal) GETSTRUCT(tup);
		QxCatalogPrincipalInfo *info;

		if (!qx_catalog_matches_namespace_filter(form->qxprincipalnamespace,
												 form->qxprincipalowner,
												 namespaceoid,
												 ownerid))
			continue;

		info = palloc0(sizeof(QxCatalogPrincipalInfo));
		qx_catalog_fill_principal_info(tup, info);
		principals = lappend(principals, info);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return principals;
}

List *
QxCatalogBuildDistinctRuntimeClassList(void)
{
	List	   *principals;
	List	   *runtime_classes = NIL;
	ListCell   *lc;

	principals = QxCatalogBuildPrincipalInfoList(InvalidOid, InvalidOid);
	foreach(lc, principals)
	{
		QxCatalogPrincipalInfo *info = lfirst(lc);
		ListCell   *seen;
		bool		already_seen = false;

		if (info->runtime_class == NULL || info->runtime_class[0] == '\0')
			continue;

		foreach(seen, runtime_classes)
		{
			const char *existing = (const char *) lfirst(seen);

			if (strcmp(existing, info->runtime_class) == 0)
			{
				already_seen = true;
				break;
			}
		}

		if (!already_seen)
			runtime_classes = lappend(runtime_classes,
									  pstrdup(info->runtime_class));
	}

	QxCatalogFreePrincipalInfoList(principals);
	return runtime_classes;
}

static char *
qx_catalog_heap_text_attr(Relation rel, HeapTuple tup, AttrNumber attnum)
{
	Datum		datum;
	bool		isnull;

	datum = heap_getattr(tup, attnum, RelationGetDescr(rel), &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static void
qx_catalog_copy_scheduler_queue_snapshot(Relation rel, HeapTuple tup,
										 QxSchedulerQueueSnapshot *snapshot)
{
	Form_pg_qx_scheduler_queue form;

	Assert(snapshot != NULL);

	form = (Form_pg_qx_scheduler_queue) GETSTRUCT(tup);
	MemSet(snapshot, 0, sizeof(QxSchedulerQueueSnapshot));
	snapshot->queueoid = form->oid;
	snapshot->ownerid = form->qxqueueledgerowner;
	snapshot->namespace_policy_oid = form->qxqueueledgernamespaceid;
	snapshot->agentoid = form->qxqueueledgeragentid;
	snapshot->sessionoid = form->qxqueueledgersessionid;
	snapshot->taskoid = form->qxqueueledgertaskid;
	snapshot->attemptoid = form->qxqueueledgerattemptid;
	snapshot->queue_kind = (QxSchedulerQueueKind) form->qxqueueledgerkind;
	snapshot->runnable_count = form->qxqueueledgerrunnablecount;
	snapshot->leased_count = form->qxqueueledgerleasedcount;
	snapshot->blocked_count = form->qxqueueledgerblockedcount;
	snapshot->retry_count = form->qxqueueledgerretrycount;
	snapshot->enqueued_at = form->qxqueueledgerenqueuedat;
	snapshot->eligible_at = form->qxqueueledgereligibleat;
	snapshot->updated_at = form->qxqueueledgerupdatedat;
	snapshot->queue_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgername);
	snapshot->agent_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgeragentname);
	snapshot->identity_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgeridentityname);
	snapshot->namespace_policy_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgernamespacepolicyname);
	snapshot->priority = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgerpriority);
	snapshot->principal_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgerprincipalname);
	snapshot->provider_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgerprovidername);
	snapshot->provider_kind = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgerproviderkind);
	snapshot->principal_runtime = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_queue_qxqueueledgerprincipalruntime);
}

static void
qx_catalog_copy_scheduler_lease_snapshot(Relation rel, HeapTuple tup,
										 QxSchedulerLeaseSnapshot *snapshot,
										 Oid *queueoid)
{
	Form_pg_qx_scheduler_lease form;

	Assert(snapshot != NULL);

	form = (Form_pg_qx_scheduler_lease) GETSTRUCT(tup);
	MemSet(snapshot, 0, sizeof(QxSchedulerLeaseSnapshot));
	snapshot->leaseoid = form->oid;
	snapshot->queueoid = form->qxleaseledgerqueueid;
	snapshot->ownerid = form->qxleaseledgerowner;
	snapshot->workeroid = form->qxleaseledgerworkerid;
	snapshot->sessionoid = form->qxleaseledgersessionid;
	snapshot->taskoid = form->qxleaseledgertaskid;
	snapshot->attemptoid = form->qxleaseledgerattemptid;
	snapshot->state = (QxSchedulerLeaseState) form->qxleaseledgerstate;
	snapshot->acquired_at = form->qxleaseledgeracquiredat;
	snapshot->renewed_at = form->qxleaseledgerrenewedat;
	snapshot->expires_at = form->qxleaseledgerexpiresat;
	snapshot->last_heartbeat_at = form->qxleaseledgerlastheartbeatat;
	snapshot->renewal_count = form->qxleaseledgerrenewalcount;
	snapshot->needs_recovery = form->qxleaseledgerneedsrecovery;
	snapshot->queue_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_lease_qxleaseledgerqueuename);
	snapshot->worker_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_lease_qxleaseledgerworkername);
	snapshot->lease_token = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_lease_qxleaseledgerleasetoken);
	snapshot->principal_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_lease_qxleaseledgerprincipalname);
	snapshot->provider_name = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_lease_qxleaseledgerprovidername);
	snapshot->provider_kind = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_lease_qxleaseledgerproviderkind);
	snapshot->principal_runtime = qx_catalog_heap_text_attr(rel, tup,
		Anum_pg_qx_scheduler_lease_qxleaseledgerprincipalruntime);

	if (queueoid != NULL)
		*queueoid = form->qxleaseledgerqueueid;
}

bool
QxCatalogLookupLatestSchedulerQueue(Oid dboid, Oid ownerid, Oid taskoid,
									Oid attemptoid,
									QxSchedulerQueueSnapshot *snapshot)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	HeapTuple	best = NULL;
	Oid			bestoid = InvalidOid;

	Assert(snapshot != NULL);

	rel = table_open(QxSchedulerQueueRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_scheduler_queue form =
			(Form_pg_qx_scheduler_queue) GETSTRUCT(tup);

		if (form->qxqueueledgerdbid != dboid ||
			form->qxqueueledgertaskid != taskoid ||
			form->qxqueueledgerattemptid != attemptoid)
			continue;

		if (OidIsValid(ownerid) && form->qxqueueledgerowner != ownerid)
			continue;

		if (!OidIsValid(bestoid) || form->oid > bestoid)
		{
			if (best != NULL)
				heap_freetuple(best);
			best = heap_copytuple(tup);
			bestoid = form->oid;
		}
	}
	table_endscan(scan);

	if (best == NULL)
	{
		table_close(rel, AccessShareLock);
		return false;
	}

	qx_catalog_copy_scheduler_queue_snapshot(rel, best, snapshot);
	heap_freetuple(best);
	table_close(rel, AccessShareLock);
	return true;
}

bool
QxCatalogLookupLatestSchedulerLease(Oid dboid, Oid ownerid, Oid taskoid,
									Oid attemptoid,
									QxSchedulerLeaseSnapshot *snapshot,
									Oid *queueoid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	HeapTuple	best = NULL;
	Oid			bestoid = InvalidOid;

	Assert(snapshot != NULL);

	rel = table_open(QxSchedulerLeaseRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_scheduler_lease form =
			(Form_pg_qx_scheduler_lease) GETSTRUCT(tup);

		if (form->qxleaseledgerdbid != dboid ||
			form->qxleaseledgertaskid != taskoid ||
			form->qxleaseledgerattemptid != attemptoid)
			continue;

		if (OidIsValid(ownerid) && form->qxleaseledgerowner != ownerid)
			continue;

		if (!OidIsValid(bestoid) || form->oid > bestoid)
		{
			if (best != NULL)
				heap_freetuple(best);
			best = heap_copytuple(tup);
			bestoid = form->oid;
		}
	}
	table_endscan(scan);

	if (best == NULL)
	{
		table_close(rel, AccessShareLock);
		return false;
	}

	qx_catalog_copy_scheduler_lease_snapshot(rel, best, snapshot, queueoid);
	heap_freetuple(best);
	table_close(rel, AccessShareLock);
	return true;
}

int16
QxCatalogMaxStepSeqnoForTask(Oid dboid, Oid taskoid)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;
	int16		maxseqno = 0;

	rel = table_open(QxStepRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);
	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_step form = (Form_pg_qx_step) GETSTRUCT(tup);

		if (form->qxstepdbid != dboid || form->qxsteptaskid != taskoid)
			continue;

		if (form->qxstepseqno > maxseqno)
			maxseqno = form->qxstepseqno;
	}
	table_endscan(scan);
	table_close(rel, AccessShareLock);

	return maxseqno + 1;
}

void
QxCatalogFreeSchedulerQueueSnapshot(QxSchedulerQueueSnapshot *snapshot)
{
	if (snapshot == NULL)
		return;

	QxCatalogFreeString(&snapshot->queue_name);
	QxCatalogFreeString(&snapshot->agent_name);
	QxCatalogFreeString(&snapshot->identity_name);
	QxCatalogFreeString(&snapshot->namespace_policy_name);
	QxCatalogFreeString(&snapshot->priority);
	QxCatalogFreeString(&snapshot->principal_name);
	QxCatalogFreeString(&snapshot->provider_name);
	QxCatalogFreeString(&snapshot->provider_kind);
	QxCatalogFreeString(&snapshot->principal_runtime);
	MemSet(snapshot, 0, sizeof(QxSchedulerQueueSnapshot));
}

void
QxCatalogFreeSchedulerLeaseSnapshot(QxSchedulerLeaseSnapshot *snapshot)
{
	if (snapshot == NULL)
		return;

	QxCatalogFreeString(&snapshot->queue_name);
	QxCatalogFreeString(&snapshot->worker_name);
	QxCatalogFreeString(&snapshot->lease_token);
	QxCatalogFreeString(&snapshot->principal_name);
	QxCatalogFreeString(&snapshot->provider_name);
	QxCatalogFreeString(&snapshot->provider_kind);
	QxCatalogFreeString(&snapshot->principal_runtime);
	MemSet(snapshot, 0, sizeof(QxSchedulerLeaseSnapshot));
}

void
QxCatalogUpdateTaskRuntime(Relation taskrel, Oid taskoid, char state,
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

void
QxCatalogUpdateAttemptState(Relation attemptrel, Oid attemptoid, char state)
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

void
QxCatalogChargeTaskBudget(Relation taskrel, Oid taskoid,
						  int32 token_delta, int32 cost_delta,
						  const char *charge_name)
{
	QxCatalogTaskInfo task;
	HeapTuple	tasktup;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_task];
	bool		nulls[Natts_pg_qx_task];
	bool		replaces[Natts_pg_qx_task];
	int32		new_tokens;
	int32		new_cost;

	if (!QxCatalogLookupTaskByOid(taskoid, &task))
		elog(ERROR, "cache lookup failed for QhapaqXian task %u", taskoid);

	new_tokens = task.consumed_tokens + token_delta;
	new_cost = task.consumed_cost + cost_delta;

	if (task.budget_tokens > 0 && new_tokens > task.budget_tokens)
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("runtime token budget exceeded for task %u", taskoid),
				 errdetail("Charge \"%s\" would move token usage to %d, above the ceiling %d.",
						   charge_name, new_tokens, task.budget_tokens)));
	}

	if (task.budget_cost > 0 && new_cost > task.budget_cost)
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("runtime cost budget exceeded for task %u", taskoid),
				 errdetail("Charge \"%s\" would move cost usage to %d, above the ceiling %d.",
						   charge_name, new_cost, task.budget_cost)));
	}

	QxCatalogFreeTaskInfo(&task);

	tasktup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tasktup))
		elog(ERROR, "cache lookup failed for QhapaqXian task %u", taskoid);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[Anum_pg_qx_task_qxtaskconsumedtokens - 1] = Int32GetDatum(new_tokens);
	values[Anum_pg_qx_task_qxtaskconsumedcost - 1] = Int32GetDatum(new_cost);
	replaces[Anum_pg_qx_task_qxtaskconsumedtokens - 1] = true;
	replaces[Anum_pg_qx_task_qxtaskconsumedcost - 1] = true;

	newtup = heap_modify_tuple(tasktup, RelationGetDescr(taskrel),
							   values, nulls, replaces);
	CatalogTupleUpdate(taskrel, &tasktup->t_self, newtup);

	heap_freetuple(newtup);
	ReleaseSysCache(tasktup);
	CommandCounterIncrement();
}

Oid
QxCatalogInsertAttempt(Relation rel, Oid sessionoid, Oid taskoid, Oid ownerid,
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
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_attempt_qxattemptstrategy,
							  strategy);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return attemptoid;
}

Oid
QxCatalogInsertStep(Relation rel, Oid sessionoid, Oid taskoid, int16 seqno,
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
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_step_qxstepname, name);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_step_qxstepdetail, detail);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return stepoid;
}

Oid
QxCatalogInsertEvent(Relation rel, Oid sessionoid, Oid taskoid, Oid stepoid,
					 Oid ownerid, const QxSemanticExecutionMetadata *metadata,
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
	values[Anum_pg_qx_event_qxeventtaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_event_qxeventstepid - 1] = ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_event_qxeventowner - 1] = ObjectIdGetDatum(ownerid);
	if (metadata != NULL)
		eventlsn = QxEmitSemanticEventRecordV2(eventoid, sessionoid, taskoid,
											   stepoid, ownerid, metadata,
											   kind, payload);
	else
		eventlsn = QxEmitSemanticEventRecord(eventoid, sessionoid, taskoid,
											 stepoid, ownerid, kind, payload);
	values[Anum_pg_qx_event_qxeventlsn - 1] = LSNGetDatum(eventlsn);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventkind, kind);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventpayload, payload);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return eventoid;
}

Oid
QxCatalogInsertTrace(Relation rel, Oid sessionoid, Oid taskoid, Oid stepoid,
					 Oid ownerid, const QxSemanticExecutionMetadata *metadata,
					 char trace_state, const char *name, const char *detail)
{
	Datum		values[Natts_pg_qx_trace];
	bool		nulls[Natts_pg_qx_trace];
	Oid			traceoid;
	HeapTuple	tup;
	XLogRecPtr	tracelsn;
	const char *trace_detail = detail;
	char	   *stripped_detail = NULL;

	if (detail != NULL &&
		name != NULL &&
		(strcmp(name, "runtime.external_submit") == 0 ||
		 strcmp(name, "runtime.external_resume") == 0))
	{
		stripped_detail = QxObserveNormalizeExternalTraceDetail(detail);
		trace_detail = stripped_detail;
	}

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
		CharGetDatum(trace_state);
	if (metadata != NULL)
		tracelsn = QxEmitSemanticTraceRecordV2(traceoid, sessionoid, taskoid,
											   stepoid, ownerid, metadata,
											   trace_state, name,
											   trace_detail);
	else
		tracelsn = QxEmitSemanticTraceRecord(traceoid, sessionoid, taskoid,
											 stepoid, ownerid,
											 trace_state, name,
											 trace_detail);
	values[Anum_pg_qx_trace_qxtracelsn - 1] = LSNGetDatum(tracelsn);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracename, name);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracedetail, trace_detail);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	if (qhapaqxian_track_stats)
		QxStatReportTrace(name, trace_detail);

	if (stripped_detail != NULL)
		pfree(stripped_detail);

	return traceoid;
}

Oid
QxCatalogInsertCheckpoint(Relation rel, Oid sessionoid, Oid taskoid, Oid attemptoid,
						  Oid stepoid, Oid ownerid,
						  const QxSemanticExecutionMetadata *metadata,
						  char taskstate, int16 nextstepseqno,
						  const char *label, const char *data)
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
	if (metadata != NULL)
		checkpointlsn = QxEmitSemanticCheckpointRecordV2(checkpointoid,
														 sessionoid, taskoid,
														 attemptoid, stepoid,
														 ownerid, metadata,
														 QX_CHECKPOINT_STATE_DURABLE,
														 taskstate, nextstepseqno,
														 label, data);
	else
		checkpointlsn = QxEmitSemanticCheckpointRecord(checkpointoid,
													   sessionoid, taskoid,
													   attemptoid, stepoid,
													   ownerid,
													   QX_CHECKPOINT_STATE_DURABLE,
													   taskstate, nextstepseqno,
													   label, data);
	values[Anum_pg_qx_checkpoint_qxcheckpointlsn - 1] =
		LSNGetDatum(checkpointlsn);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_checkpoint_qxcheckpointlabel,
							  label);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_checkpoint_qxcheckpointdata,
							  data);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return checkpointoid;
}

Oid
QxCatalogInsertSchedulerQueue(Relation rel,
							  const QxSchedulerQueueSnapshot *snapshot)
{
	Datum		values[Natts_pg_qx_scheduler_queue];
	bool		nulls[Natts_pg_qx_scheduler_queue];
	Oid			queueoid;
	HeapTuple	tup;

	Assert(snapshot != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	queueoid = GetNewOidWithIndex(rel, QxSchedulerQueueOidIndexId,
								  Anum_pg_qx_scheduler_queue_oid);
	values[Anum_pg_qx_scheduler_queue_oid - 1] = ObjectIdGetDatum(queueoid);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerdbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerowner - 1] =
		ObjectIdGetDatum(snapshot->ownerid);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgernamespaceid - 1] =
		ObjectIdGetDatum(snapshot->namespace_policy_oid);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgeragentid - 1] =
		ObjectIdGetDatum(snapshot->agentoid);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgersessionid - 1] =
		ObjectIdGetDatum(snapshot->sessionoid);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgertaskid - 1] =
		ObjectIdGetDatum(snapshot->taskoid);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerattemptid - 1] =
		ObjectIdGetDatum(snapshot->attemptoid);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerkind - 1] =
		Int16GetDatum((int16) snapshot->queue_kind);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerrunnablecount - 1] =
		Int32GetDatum(snapshot->runnable_count);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerleasedcount - 1] =
		Int32GetDatum(snapshot->leased_count);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerblockedcount - 1] =
		Int32GetDatum(snapshot->blocked_count);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerretrycount - 1] =
		Int32GetDatum(snapshot->retry_count);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerenqueuedat - 1] =
		TimestampTzGetDatum(snapshot->enqueued_at);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgereligibleat - 1] =
		TimestampTzGetDatum(snapshot->eligible_at);
	values[Anum_pg_qx_scheduler_queue_qxqueueledgerupdatedat - 1] =
		TimestampTzGetDatum(snapshot->updated_at);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgername,
							  snapshot->queue_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgeragentname,
							  snapshot->agent_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgeridentityname,
							  snapshot->identity_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgernamespacepolicyname,
							  snapshot->namespace_policy_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgerpriority,
							  snapshot->priority);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgerprincipalname,
							  snapshot->principal_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgerprovidername,
							  snapshot->provider_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgerproviderkind,
							  snapshot->provider_kind);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_queue_qxqueueledgerprincipalruntime,
							  snapshot->principal_runtime);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return queueoid;
}

Oid
QxCatalogInsertSchedulerLease(Relation rel, Oid queueoid,
							  const QxSchedulerLeaseSnapshot *snapshot)
{
	Datum		values[Natts_pg_qx_scheduler_lease];
	bool		nulls[Natts_pg_qx_scheduler_lease];
	Oid			leaseoid;
	HeapTuple	tup;

	Assert(snapshot != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	leaseoid = GetNewOidWithIndex(rel, QxSchedulerLeaseOidIndexId,
								  Anum_pg_qx_scheduler_lease_oid);
	values[Anum_pg_qx_scheduler_lease_oid - 1] = ObjectIdGetDatum(leaseoid);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerdbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerowner - 1] =
		ObjectIdGetDatum(snapshot->ownerid);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerqueueid - 1] =
		ObjectIdGetDatum(queueoid);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerworkerid - 1] =
		ObjectIdGetDatum(snapshot->workeroid);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgersessionid - 1] =
		ObjectIdGetDatum(snapshot->sessionoid);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgertaskid - 1] =
		ObjectIdGetDatum(snapshot->taskoid);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerattemptid - 1] =
		ObjectIdGetDatum(snapshot->attemptoid);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerstate - 1] =
		Int16GetDatum((int16) snapshot->state);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgeracquiredat - 1] =
		TimestampTzGetDatum(snapshot->acquired_at);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerrenewedat - 1] =
		TimestampTzGetDatum(snapshot->renewed_at);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerexpiresat - 1] =
		TimestampTzGetDatum(snapshot->expires_at);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerlastheartbeatat - 1] =
		TimestampTzGetDatum(snapshot->last_heartbeat_at);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerrenewalcount - 1] =
		Int32GetDatum(snapshot->renewal_count);
	values[Anum_pg_qx_scheduler_lease_qxleaseledgerneedsrecovery - 1] =
		BoolGetDatum(snapshot->needs_recovery);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_lease_qxleaseledgerqueuename,
							  snapshot->queue_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_lease_qxleaseledgerworkername,
							  snapshot->worker_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_lease_qxleaseledgerleasetoken,
							  snapshot->lease_token);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_lease_qxleaseledgerprincipalname,
							  snapshot->principal_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_lease_qxleaseledgerprovidername,
							  snapshot->provider_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_lease_qxleaseledgerproviderkind,
							  snapshot->provider_kind);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_lease_qxleaseledgerprincipalruntime,
							  snapshot->principal_runtime);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return leaseoid;
}

Oid
QxCatalogInsertSchedulerHeartbeat(Relation rel, Oid leaseoid,
								  const QxSchedulerHeartbeatSnapshot *snapshot)
{
	Datum		values[Natts_pg_qx_scheduler_heartbeat];
	bool		nulls[Natts_pg_qx_scheduler_heartbeat];
	Oid			heartbeatoid;
	HeapTuple	tup;

	Assert(snapshot != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	heartbeatoid = GetNewOidWithIndex(rel, QxSchedulerHeartbeatOidIndexId,
									  Anum_pg_qx_scheduler_heartbeat_oid);
	values[Anum_pg_qx_scheduler_heartbeat_oid - 1] =
		ObjectIdGetDatum(heartbeatoid);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerdbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerowner - 1] =
		ObjectIdGetDatum(snapshot->ownerid);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerleaseid - 1] =
		ObjectIdGetDatum(leaseoid);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerworkerid - 1] =
		ObjectIdGetDatum(snapshot->workeroid);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgersessionid - 1] =
		ObjectIdGetDatum(snapshot->sessionoid);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgertaskid - 1] =
		ObjectIdGetDatum(snapshot->taskoid);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerattemptid - 1] =
		ObjectIdGetDatum(snapshot->attemptoid);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerstate - 1] =
		Int16GetDatum((int16) snapshot->state);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerlagms - 1] =
		Int32GetDatum(snapshot->lag_ms);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerobservedat - 1] =
		TimestampTzGetDatum(snapshot->observed_at);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerexpectedbefore - 1] =
		TimestampTzGetDatum(snapshot->expected_before);
	values[Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerstale - 1] =
		BoolGetDatum(snapshot->stale);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerworkername,
							  snapshot->worker_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerqueuename,
							  snapshot->queue_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerprincipalruntime,
							  snapshot->principal_runtime);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerproviderkind,
							  snapshot->provider_kind);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_scheduler_heartbeat_qxheartbeatledgerreceiptmode,
							  snapshot->receipt_mode);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return heartbeatoid;
}

Oid
QxCatalogInsertTask(Relation taskrel, const QxCatalogTaskInsertParams *params)
{
	Datum		values[Natts_pg_qx_task];
	bool		nulls[Natts_pg_qx_task];
	HeapTuple	tup;
	Oid			taskoid;
	ObjectAddress myself;
	ObjectAddress referenced;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	taskoid = GetNewOidWithIndex(taskrel, QxTaskOidIndexId,
								 Anum_pg_qx_task_oid);
	values[Anum_pg_qx_task_oid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_task_qxtaskdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_task_qxtasksessionid - 1] =
		ObjectIdGetDatum(params->sessionoid);
	values[Anum_pg_qx_task_qxtaskagentid - 1] =
		ObjectIdGetDatum(params->agentoid);
	values[Anum_pg_qx_task_qxtasknamespacepolicyid - 1] =
		ObjectIdGetDatum(params->namespace_policy_oid);
	values[Anum_pg_qx_task_qxtaskidentityid - 1] =
		ObjectIdGetDatum(params->identityoid);
	values[Anum_pg_qx_task_qxtaskowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_task_qxtasklastattemptid - 1] =
		ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_task_qxtasklastcheckpointid - 1] =
		ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_task_qxtaskbudgettokens - 1] =
		Int32GetDatum(params->budget_tokens);
	values[Anum_pg_qx_task_qxtaskbudgetcost - 1] =
		Int32GetDatum(params->budget_cost);
	values[Anum_pg_qx_task_qxtaskauthorizedtooltokens - 1] =
		Int32GetDatum(params->authorized_tool_tokens);
	values[Anum_pg_qx_task_qxtaskauthorizedtoolcost - 1] =
		Int32GetDatum(params->authorized_tool_cost);
	values[Anum_pg_qx_task_qxtaskestimatedtokens - 1] =
		Int32GetDatum(params->estimated_tokens);
	values[Anum_pg_qx_task_qxtaskestimatedcost - 1] =
		Int32GetDatum(params->estimated_cost);
	values[Anum_pg_qx_task_qxtaskstate - 1] =
		CharGetDatum(QX_TASK_STATE_QUEUED);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskname,
							  params->task_name);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskgoal,
							  params->goal);
	qx_catalog_set_nodetree_datum(values, nulls, Anum_pg_qx_task_qxtaskinput,
								  params->input);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskpriority,
							  params->priority);
	qx_catalog_set_nodetree_datum(values, nulls,
								  Anum_pg_qx_task_qxtaskauthorizedtools,
								  params->authorized_tools);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_task_qxtasksubmitcontract,
							  params->submit_contract);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_task_qxtaskresumecontract,
							  params->resume_contract);

	tup = heap_form_tuple(RelationGetDescr(taskrel), values, nulls);
	CatalogTupleInsert(taskrel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, QxTaskRelationId, taskoid);
	recordDependencyOnOwner(QxTaskRelationId, taskoid, params->ownerid);
	ObjectAddressSet(referenced, QxSessionRelationId, params->sessionoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxAgentRelationId, params->agentoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxNamespaceRelationId, params->namespace_policy_oid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxIdentityRelationId, params->identityoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxTaskRelationId, taskoid, 0);

	return taskoid;
}

Oid
QxCatalogInsertIdentity(Relation rel, const QxCatalogIdentityInsertParams *params)
{
	Datum		values[Natts_pg_qx_identity];
	bool		nulls[Natts_pg_qx_identity];
	HeapTuple	tup;
	Oid			identityoid;
	ObjectAddress myself;
	ObjectAddress referenced;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	identityoid = GetNewOidWithIndex(rel, QxIdentityOidIndexId,
									 Anum_pg_qx_identity_oid);
	values[Anum_pg_qx_identity_oid - 1] = ObjectIdGetDatum(identityoid);
	values[Anum_pg_qx_identity_qxidentityname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(params->name));
	values[Anum_pg_qx_identity_qxidentitynamespace - 1] =
		ObjectIdGetDatum(params->namespaceoid);
	values[Anum_pg_qx_identity_qxidentityowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_identity_qxidentityauthrole - 1] =
		ObjectIdGetDatum(params->authrole);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_identity_qxidentitypolicy,
							  params->policy_name);
	qx_catalog_set_nodetree_datum(values, nulls,
								  Anum_pg_qx_identity_qxidentitybudget,
								  params->budget_options);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, QxIdentityRelationId, identityoid);
	recordDependencyOnOwner(QxIdentityRelationId, identityoid, params->ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, params->namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, AuthIdRelationId, params->authrole);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxIdentityRelationId, identityoid, 0);

	return identityoid;
}

Oid
QxCatalogInsertAgent(Relation rel, const QxCatalogAgentInsertParams *params)
{
	Datum		values[Natts_pg_qx_agent];
	bool		nulls[Natts_pg_qx_agent];
	HeapTuple	tup;
	Oid			agentoid;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	agentoid = GetNewOidWithIndex(rel, QxAgentOidIndexId,
								  Anum_pg_qx_agent_oid);
	values[Anum_pg_qx_agent_oid - 1] = ObjectIdGetDatum(agentoid);
	values[Anum_pg_qx_agent_qxagentname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(params->name));
	values[Anum_pg_qx_agent_qxagentnamespace - 1] =
		ObjectIdGetDatum(params->namespaceoid);
	values[Anum_pg_qx_agent_qxnamespacepolicyid - 1] =
		ObjectIdGetDatum(params->namespacepolicyoid);
	values[Anum_pg_qx_agent_qxidentityid - 1] =
		ObjectIdGetDatum(params->identityoid);
	values[Anum_pg_qx_agent_qxagentowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_agent_qxidentity,
							  params->identity_name);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_agent_qxmodeluri,
							  params->model_uri);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_agent_qxmemoryprofile,
							  params->memory_profile);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_agent_qxpolicy,
							  params->policy_name);
	qx_catalog_set_nodetree_datum(values, nulls, Anum_pg_qx_agent_qxtools,
								  params->tools);
	qx_catalog_set_nodetree_datum(values, nulls, Anum_pg_qx_agent_qxbudget,
								  params->budget_options);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return agentoid;
}

Oid
QxCatalogInsertSession(Relation rel, const QxCatalogSessionInsertParams *params)
{
	Datum		values[Natts_pg_qx_session];
	bool		nulls[Natts_pg_qx_session];
	HeapTuple	tup;
	Oid			sessionoid;
	ObjectAddress myself;
	ObjectAddress referenced;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	sessionoid = GetNewOidWithIndex(rel, QxSessionOidIndexId,
									Anum_pg_qx_session_oid);
	values[Anum_pg_qx_session_oid - 1] = ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_session_qxsessiondbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_session_qxsessionagentid - 1] =
		ObjectIdGetDatum(params->agentoid);
	values[Anum_pg_qx_session_qxsessionnamespacepolicyid - 1] =
		ObjectIdGetDatum(params->namespacepolicyoid);
	values[Anum_pg_qx_session_qxsessionidentityid - 1] =
		ObjectIdGetDatum(params->identityoid);
	values[Anum_pg_qx_session_qxsessionowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_session_qxsessionstatus - 1] =
		CharGetDatum(params->status);

	if (params->context_serialized != NULL)
		qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_session_qxcontext,
								  params->context_serialized);
	else
		nulls[Anum_pg_qx_session_qxcontext - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, QxSessionRelationId, sessionoid);
	recordDependencyOnOwner(QxSessionRelationId, sessionoid, params->ownerid);
	ObjectAddressSet(referenced, QxAgentRelationId, params->agentoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxNamespaceRelationId,
					 params->namespacepolicyoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxIdentityRelationId, params->identityoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxSessionRelationId, sessionoid, 0);

	return sessionoid;
}

Oid
QxCatalogInsertMemory(Relation memoryrel,
					  const QxCatalogMemoryInsertParams *params)
{
	Datum		values[Natts_pg_qx_memory];
	bool		nulls[Natts_pg_qx_memory];
	HeapTuple	tup;
	Oid			memoryoid;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	memoryoid = GetNewOidWithIndex(memoryrel, QxMemoryOidIndexId,
								   Anum_pg_qx_memory_oid);
	values[Anum_pg_qx_memory_oid - 1] = ObjectIdGetDatum(memoryoid);
	values[Anum_pg_qx_memory_qxmemorydbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_memory_qxmemoryagentid - 1] =
		ObjectIdGetDatum(params->agentoid);
	values[Anum_pg_qx_memory_qxmemorysessionid - 1] =
		ObjectIdGetDatum(params->sessionoid);
	values[Anum_pg_qx_memory_qxmemorytaskid - 1] = ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_memory_qxmemoryowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_memory_qxmemoryscope - 1] =
		CharGetDatum(params->scope);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_memory_qxmemorykey,
							  params->memory_key);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_memory_qxmemoryvalue,
							  params->serialized_value);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_memory_qxmemorytags,
							  params->serialized_tags);

	tup = heap_form_tuple(RelationGetDescr(memoryrel), values, nulls);
	CatalogTupleInsert(memoryrel, tup);
	heap_freetuple(tup);

	return memoryoid;
}

Oid
QxCatalogInsertNamespacePolicy(Relation rel,
							   const QxCatalogNamespacePolicyInsertParams *params)
{
	Datum		values[Natts_pg_qx_namespace];
	bool		nulls[Natts_pg_qx_namespace];
	HeapTuple	tup;
	Oid			policyoid;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	policyoid = GetNewOidWithIndex(rel, QxNamespaceOidIndexId,
								   Anum_pg_qx_namespace_oid);
	values[Anum_pg_qx_namespace_oid - 1] = ObjectIdGetDatum(policyoid);
	values[Anum_pg_qx_namespace_qxnamespacepolicyname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(params->name));
	values[Anum_pg_qx_namespace_qxnamespaceid - 1] =
		ObjectIdGetDatum(params->namespaceoid);
	values[Anum_pg_qx_namespace_qxnamespaceowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_namespace_qxnamespaceauthrole - 1] =
		ObjectIdGetDatum(params->authrole);
	values[Anum_pg_qx_namespace_qxrequireknowntools - 1] =
		BoolGetDatum(params->require_known_tools);
	values[Anum_pg_qx_namespace_qxenforcebudgets - 1] =
		BoolGetDatum(params->enforce_budgets);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_namespace_qxnamespacepolicy,
							  params->policy_contract);
	qx_catalog_set_nodetree_datum(values, nulls,
								  Anum_pg_qx_namespace_qxallowedtools,
								  params->allowed_tools);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return policyoid;
}

Oid
QxCatalogInsertProvider(Relation rel,
						const QxCatalogProviderInsertParams *params)
{
	Datum		values[Natts_pg_qx_provider];
	bool		nulls[Natts_pg_qx_provider];
	HeapTuple	tup;
	Oid			provideroid;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	provideroid = GetNewOidWithIndex(rel, QxProviderOidIndexId,
									 Anum_pg_qx_provider_oid);
	values[Anum_pg_qx_provider_oid - 1] = ObjectIdGetDatum(provideroid);
	values[Anum_pg_qx_provider_qxprovidername - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(params->name));
	values[Anum_pg_qx_provider_qxprovidernamespace - 1] =
		ObjectIdGetDatum(params->namespaceoid);
	values[Anum_pg_qx_provider_qxproviderowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_provider_qxproviderenabled - 1] =
		BoolGetDatum(params->enabled);
	values[Anum_pg_qx_provider_qxproviderattestationrequired - 1] =
		BoolGetDatum(params->attestation_required);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_provider_qxproviderkind,
							  params->kind);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_provider_qxproviderendpoint,
							  params->endpoint);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_provider_qxproviderreceiptalg,
							  params->receipt_alg);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_provider_qxproviderreceiptkey,
							  params->receipt_key);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_provider_qxproviderattestationprofile,
							  params->attestation_profile);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_provider_qxproviderattestationversion,
							  params->attestation_version);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_provider_qxproviderattestationpolicy,
							  params->attestation_policy);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return provideroid;
}

Oid
QxCatalogInsertPrincipal(Relation rel,
						 const QxCatalogPrincipalInsertParams *params)
{
	Datum		values[Natts_pg_qx_principal];
	bool		nulls[Natts_pg_qx_principal];
	HeapTuple	tup;
	Oid			principaloid;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	principaloid = GetNewOidWithIndex(rel, QxPrincipalOidIndexId,
									  Anum_pg_qx_principal_oid);
	values[Anum_pg_qx_principal_oid - 1] = ObjectIdGetDatum(principaloid);
	values[Anum_pg_qx_principal_qxprincipalname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(params->name));
	values[Anum_pg_qx_principal_qxprincipalnamespace - 1] =
		ObjectIdGetDatum(params->namespaceoid);
	values[Anum_pg_qx_principal_qxprincipalowner - 1] =
		ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_principal_qxprincipalproviderid - 1] =
		ObjectIdGetDatum(params->provideroid);
	values[Anum_pg_qx_principal_qxprincipalenabled - 1] =
		BoolGetDatum(params->enabled);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalsandbox,
							  params->sandbox_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalprogram,
							  params->program_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalprovider,
							  params->provider_name);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalruntimeclass,
							  params->runtime_class);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalreceiptsigner,
							  params->receipt_signer);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalattestationprofile,
							  params->attestation_profile);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalattestationversion,
							  params->attestation_version);
	qx_catalog_set_text_datum(values, nulls,
							  Anum_pg_qx_principal_qxprincipalattestationpolicy,
							  params->attestation_policy);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return principaloid;
}

Oid
QxCatalogInsertTool(Relation rel, const QxCatalogToolInsertParams *params)
{
	Datum		values[Natts_pg_qx_tool];
	bool		nulls[Natts_pg_qx_tool];
	HeapTuple	tup;
	Oid			tooloid;

	Assert(params != NULL);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	tooloid = GetNewOidWithIndex(rel, QxToolOidIndexId,
								 Anum_pg_qx_tool_oid);
	values[Anum_pg_qx_tool_oid - 1] = ObjectIdGetDatum(tooloid);
	values[Anum_pg_qx_tool_qxtoolname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(params->name));
	values[Anum_pg_qx_tool_qxtoolnamespace - 1] =
		ObjectIdGetDatum(params->namespaceoid);
	values[Anum_pg_qx_tool_qxtoolowner - 1] = ObjectIdGetDatum(params->ownerid);
	values[Anum_pg_qx_tool_qxtoolprincipalid - 1] =
		ObjectIdGetDatum(params->principaloid);
	values[Anum_pg_qx_tool_qxtoolenabled - 1] = BoolGetDatum(params->enabled);
	values[Anum_pg_qx_tool_qxtooltokencost - 1] =
		Int32GetDatum(params->token_cost);
	values[Anum_pg_qx_tool_qxtoolcostunits - 1] =
		Int32GetDatum(params->cost_units);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_tool_qxtoolhandler,
							  params->handler_name);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_tool_qxtoolsandbox,
							  params->sandbox_name);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_tool_qxtoolruntimeclass,
							  NULL);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_tool_qxtoolsandboxceiling,
							  NULL);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_tool_qxtoolcapabilitytags,
							  NULL);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_tool_qxtoolprincipal,
							  params->principal_name);
	qx_catalog_set_text_datum(values, nulls, Anum_pg_qx_tool_qxtoolpolicy,
							  params->policy_name);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return tooloid;
}
