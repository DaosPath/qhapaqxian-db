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
#include "catalog/namespace.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_attempt.h"
#include "catalog/pg_qx_checkpoint.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_namespace.h"
#include "catalog/pg_qx_principal.h"
#include "catalog/pg_qx_provider.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_scheduler_lease.h"
#include "catalog/pg_qx_scheduler_queue.h"
#include "catalog/pg_qx_step.h"
#include "catalog/pg_qx_task.h"
#include "catalog/pg_qx_tool.h"
#include "qx/qx_catalog.h"
#include "qx/qx_scheduler.h"
#include "utils/builtins.h"
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
QxCatalogLookupLatestSchedulerQueue(Oid dboid, Oid taskoid, Oid attemptoid,
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
QxCatalogLookupLatestSchedulerLease(Oid dboid, Oid taskoid, Oid attemptoid,
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
