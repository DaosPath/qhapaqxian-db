/*-------------------------------------------------------------------------
 *
 * qxpolicycmds.c
 *	  QhapaqXian namespace policy, principal, and tool DDL
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/tableam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_qx_namespace.h"
#include "catalog/pg_qx_provider.h"
#include "catalog/pg_qx_principal.h"
#include "catalog/pg_qx_tool.h"
#include "commands/defrem.h"
#include "commands/qxpolicycmds.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "nodes/value.h"
#include "port.h"
#include "qx/qx_security.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void qxpolicy_set_text(Datum *values, bool *nulls, AttrNumber attnum,
							  const char *value);
static void qxpolicy_set_nodetree(Datum *values, bool *nulls, AttrNumber attnum,
								  const void *node);
static char *qxpolicy_text_attr(HeapTuple tup, AttrNumber attnum, int cacheid);
static void qx_validate_tool_costs(int32 token_cost, int32 cost_units,
								   const char *command_name);
static void qx_validate_principal_program(const char *program_name,
										  const char *command_name);
static void qx_validate_provider_kind(const char *provider_kind,
									  const char *command_name);
static void qx_validate_provider_receipt_alg(const char *receipt_alg,
											 const char *command_name);
static void qx_validate_provider_isolation_contract(const char *provider_kind,
													const char *receipt_alg,
													bool attestation_required,
													const char *command_name);
static void qx_validate_provider_endpoint(const char *provider_kind,
										  const char *endpoint_name,
										  const char *command_name);
static void qx_validate_provider_receipt_key(const char *receipt_alg,
											 const char *receipt_key,
											 const char *command_name);
static char *qx_provider_receipt_alg_by_oid(Oid provideroid);
static char *qx_provider_kind_by_oid(Oid provideroid);
static void qx_validate_principal_receipt_signer(const char *receipt_signer,
												 const char *command_name);
static const char *qx_default_runtime_for_provider_kind(const char *provider_kind);
static void qx_validate_principal_runtime_class(const char *runtime_class,
												const char *command_name);
static void qx_validate_principal_runtime_binding_values(const char *provider_kind,
														 const char *receipt_alg,
														 const char *runtime_class,
														 const char *sandbox_name,
														 const char *receipt_signer,
														 const char *command_name);
static void qx_validate_principal_runtime_binding(Oid provideroid,
												  const char *runtime_class,
												  const char *sandbox_name,
												  const char *receipt_signer,
												  const char *command_name);
static void qx_validate_provider_bound_principals(Oid provideroid,
												  const char *provider_kind,
												  const char *receipt_alg,
												  const char *command_name);
static void qx_validate_tool_sandbox(const char *sandbox_name);
static int	qx_sandbox_rank(const char *sandbox_name);
static HeapTuple qx_lookup_tool_tuple(RangeVar *tool_name, Oid *namespaceoid);
static HeapTuple qx_lookup_provider_tuple(RangeVar *provider_name,
										  Oid *namespaceoid);
static HeapTuple qx_lookup_principal_tuple(RangeVar *principal_name,
										   Oid *namespaceoid);
static void qx_record_namespace_policy_dependencies(Oid policyoid,
													Oid ownerid,
													Oid namespaceoid,
													Oid authrole);
static void qx_record_provider_dependencies(Oid provideroid, Oid ownerid,
											Oid namespaceoid);
static void qx_record_principal_dependencies(Oid principaloid, Oid ownerid,
											 Oid namespaceoid,
											 Oid provideroid);
static void qx_record_tool_dependencies(Oid tooloid, Oid ownerid,
										Oid namespaceoid, Oid principaloid);
static void qx_record_tool_principal_dependency(Oid tooloid, Oid principaloid);
static Oid qx_validate_principal_provider_binding(Oid namespaceoid,
												  Oid ownerid,
												  RangeVar *provider_name);
static Oid qx_validate_tool_principal_binding(Oid namespaceoid, Oid ownerid,
											  const char *principal_name,
											  const char *tool_sandbox);

static void
qxpolicy_set_text(Datum *values, bool *nulls, AttrNumber attnum,
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
qxpolicy_set_nodetree(Datum *values, bool *nulls, AttrNumber attnum,
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
qxpolicy_text_attr(HeapTuple tup, AttrNumber attnum, int cacheid)
{
	bool		isnull;
	Datum		datum;

	datum = SysCacheGetAttr(cacheid, tup, attnum, &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static void
qx_validate_tool_costs(int32 token_cost, int32 cost_units,
					   const char *command_name)
{
	if (token_cost < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s token cost must not be negative", command_name)));

	if (cost_units < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s cost units must not be negative", command_name)));
}

static void
qx_validate_principal_program(const char *program_name, const char *command_name)
{
	if (program_name == NULL || program_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s program must not be empty", command_name)));

	if (strchr(program_name, '\n') != NULL ||
		strchr(program_name, '\r') != NULL ||
		strchr(program_name, ';') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s program contains unsupported characters",
						command_name),
				 errdetail("Programs must be bare executable names or absolute paths.")));

	if (!is_absolute_path(program_name) &&
		(first_dir_separator(program_name) != NULL ||
		 path_contains_parent_reference(program_name)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s program must be a bare executable name or an absolute path",
						command_name),
				 errdetail("Relative paths and parent-directory references are rejected for principal programs.")));

	if (path_contains_parent_reference(program_name))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s program must not contain parent-directory references",
						command_name)));
}

static void
qx_validate_provider_kind(const char *provider_kind, const char *command_name)
{
	if (provider_kind == NULL || provider_kind[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s kind must not be empty", command_name)));

	if (strcmp(provider_kind, "loopback") != 0 &&
		strcmp(provider_kind, "remote") != 0 &&
		strcmp(provider_kind, "container") != 0 &&
		strcmp(provider_kind, "microvm") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported provider kind \"%s\"", provider_kind),
				 errdetail("QhapaqXian providers currently support loopback, remote, container, and microvm execution drivers.")));
}

static void
qx_validate_provider_isolation_contract(const char *provider_kind,
										 const char *receipt_alg,
										 bool attestation_required,
										 const char *command_name)
{
	const char *effective_kind;
	const char *effective_alg;

	effective_kind = (provider_kind != NULL && provider_kind[0] != '\0') ?
		provider_kind : "loopback";
	effective_alg = (receipt_alg != NULL && receipt_alg[0] != '\0') ?
		receipt_alg : "hmac-sha256";

	if (strcmp(effective_kind, "container") != 0 &&
		strcmp(effective_kind, "microvm") != 0)
		return;

	if (strcmp(effective_alg, "ed25519") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s kind \"%s\" requires ed25519 receipts",
						command_name, effective_kind),
				 errdetail("Container and microVM providers must use asymmetric receipts so the runtime can verify brokered isolation evidence.")));

	if (!attestation_required)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s kind \"%s\" requires attestation",
						command_name, effective_kind),
				 errdetail("Container and microVM providers must declare ATTESTATION ENABLE so the runtime rejects unsigned broker responses.")));
}

static void
qx_validate_provider_receipt_alg(const char *receipt_alg,
								 const char *command_name)
{
	const char *effective_alg;

	effective_alg = (receipt_alg != NULL && receipt_alg[0] != '\0') ?
		receipt_alg : "hmac-sha256";

	if (strcmp(effective_alg, "hmac-sha256") == 0)
		return;

	if (strcmp(effective_alg, "ed25519") == 0)
	{
#ifndef USE_OPENSSL
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s receipt algorithm \"%s\" requires OpenSSL support",
						command_name, effective_alg),
				 errdetail("Build QhapaqXian DB with OpenSSL to enable asymmetric receipt verification.")));
#endif
		return;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported provider receipt algorithm \"%s\"",
					effective_alg),
			 errdetail("QhapaqXian providers currently support only hmac-sha256 and ed25519 receipts.")));
}

static void
qx_validate_provider_endpoint(const char *provider_kind,
							  const char *endpoint_name,
							  const char *command_name)
{
	if (endpoint_name == NULL || endpoint_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s endpoint must not be empty", command_name)));

	if (strchr(endpoint_name, '\n') != NULL ||
		strchr(endpoint_name, '\r') != NULL ||
		strchr(endpoint_name, ';') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s endpoint contains unsupported characters",
						command_name)));

	if (provider_kind != NULL &&
		strcmp(provider_kind, "remote") == 0)
	{
		if (strncmp(endpoint_name, "remote://", 9) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s endpoint must start with \"remote://\" for remote providers",
							command_name),
					 errdetail("The brokered remote provider subsystem accepts only remote:// endpoints.")));
		return;
	}

	if (provider_kind != NULL &&
		strcmp(provider_kind, "container") == 0)
	{
		if (strncmp(endpoint_name, "container://", 12) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s endpoint must start with \"container://\" for container providers",
							command_name),
					 errdetail("Container providers accept only container:// broker endpoints.")));
		return;
	}

	if (provider_kind != NULL &&
		strcmp(provider_kind, "microvm") == 0)
	{
		if (strncmp(endpoint_name, "microvm://", 10) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s endpoint must start with \"microvm://\" for microvm providers",
							command_name),
					 errdetail("MicroVM providers accept only microvm:// broker endpoints.")));
		return;
	}

	if (strncmp(endpoint_name, "local://", 8) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s endpoint must start with \"local://\"",
						command_name),
				 errdetail("Loopback providers accept only local:// endpoints.")));
}

static void
qx_validate_provider_receipt_key(const char *receipt_alg,
								 const char *receipt_key,
								 const char *command_name)
{
	const char *effective_alg;

	effective_alg = (receipt_alg != NULL && receipt_alg[0] != '\0') ?
		receipt_alg : "hmac-sha256";

	if (receipt_key == NULL || receipt_key[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s receipt key must not be empty", command_name),
				 errdetail("Providers need verification material for runtime receipts.")));

	if (strcmp(effective_alg, "ed25519") == 0)
	{
		if (strstr(receipt_key, "-----BEGIN PUBLIC KEY-----") == NULL ||
			strstr(receipt_key, "-----END PUBLIC KEY-----") == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("%s receipt key is not a PEM public key",
							command_name),
					 errdetail("Ed25519 providers require a PEM-encoded public key in RECEIPT KEY.")));
		return;
	}

	if (strlen(receipt_key) < 8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s receipt key is too short", command_name),
				 errdetail("Use at least 8 characters for the bootstrap HMAC receipt key.")));

	if (strchr(receipt_key, '\n') != NULL ||
		strchr(receipt_key, '\r') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s receipt key contains unsupported characters",
						command_name)));
}

static char *
qx_provider_receipt_alg_by_oid(Oid provideroid)
{
	HeapTuple	providertup;
	char	   *receipt_alg;

	providertup = SearchSysCache1(QXPROVIDEROID, ObjectIdGetDatum(provideroid));
	if (!HeapTupleIsValid(providertup))
		elog(ERROR, "cache lookup failed for QhapaqXian provider %u", provideroid);

	receipt_alg = qxpolicy_text_attr(providertup,
									 Anum_pg_qx_provider_qxproviderreceiptalg,
									 QXPROVIDEROID);
	ReleaseSysCache(providertup);

	if (receipt_alg == NULL || receipt_alg[0] == '\0')
		return pstrdup("hmac-sha256");

	return receipt_alg;
}

static void
qx_validate_principal_runtime_class(const char *runtime_class,
									 const char *command_name)
{
	if (runtime_class == NULL || runtime_class[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s runtime class must not be empty", command_name)));

	if (strcmp(runtime_class, "host") != 0 &&
		strcmp(runtime_class, "container") != 0 &&
		strcmp(runtime_class, "microvm") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported principal runtime class \"%s\"", runtime_class),
				 errdetail("QhapaqXian principals currently support host, container, or microvm runtime classes.")));
}

static char *
qx_provider_kind_by_oid(Oid provideroid)
{
	HeapTuple	providertup;
	char	   *provider_kind;

	providertup = SearchSysCache1(QXPROVIDEROID, ObjectIdGetDatum(provideroid));
	if (!HeapTupleIsValid(providertup))
		elog(ERROR, "cache lookup failed for QhapaqXian provider %u", provideroid);

	provider_kind = qxpolicy_text_attr(providertup,
									   Anum_pg_qx_provider_qxproviderkind,
									   QXPROVIDEROID);
	ReleaseSysCache(providertup);

	if (provider_kind == NULL || provider_kind[0] == '\0')
		return pstrdup("loopback");

	return provider_kind;
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
qx_validate_principal_receipt_signer(const char *receipt_signer,
									 const char *command_name)
{
	if (receipt_signer == NULL || receipt_signer[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s signer must not be empty", command_name),
				 errdetail("Asymmetric receipt principals need a signer file reference.")));

	if (strchr(receipt_signer, '\n') != NULL ||
		strchr(receipt_signer, '\r') != NULL ||
		strchr(receipt_signer, ';') != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s signer contains unsupported characters",
						command_name),
				 errdetail("Signer references must be bare filenames or absolute paths.")));

	if (!is_absolute_path(receipt_signer) &&
		(first_dir_separator(receipt_signer) != NULL ||
		 path_contains_parent_reference(receipt_signer)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s signer must be a bare filename or an absolute path",
						command_name),
				 errdetail("Relative paths and parent-directory references are rejected for receipt signers.")));

	if (path_contains_parent_reference(receipt_signer))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s signer must not contain parent-directory references",
						command_name)));
}

static void
qx_validate_principal_runtime_binding_values(const char *provider_kind,
											  const char *receipt_alg,
											  const char *runtime_class,
											  const char *sandbox_name,
											  const char *receipt_signer,
											  const char *command_name)
{
	const char *effective_runtime;

	qx_validate_provider_kind(provider_kind, command_name);
	qx_validate_provider_receipt_alg(receipt_alg, command_name);
	effective_runtime = (runtime_class != NULL && runtime_class[0] != '\0') ?
		runtime_class : qx_default_runtime_for_provider_kind(provider_kind);
	qx_validate_principal_runtime_class(effective_runtime, command_name);

	if (receipt_signer != NULL)
		qx_validate_principal_receipt_signer(receipt_signer, command_name);

	if ((strcmp(provider_kind, "loopback") == 0 ||
		 strcmp(provider_kind, "remote") == 0) &&
		strcmp(effective_runtime, "host") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s runtime class \"%s\" is incompatible with provider kind \"%s\"",
						command_name, effective_runtime, provider_kind),
				 errdetail("Loopback and remote providers currently back only host principals. Use KIND 'container' or KIND 'microvm' for isolated principals.")));

	if (strcmp(provider_kind, "container") == 0 &&
		strcmp(effective_runtime, "container") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s runtime class \"%s\" is incompatible with provider kind \"%s\"",
						command_name, effective_runtime, provider_kind),
				 errdetail("Container providers require principals declared with RUNTIME 'container'.")));

	if (strcmp(provider_kind, "microvm") == 0 &&
		strcmp(effective_runtime, "microvm") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s runtime class \"%s\" is incompatible with provider kind \"%s\"",
						command_name, effective_runtime, provider_kind),
				 errdetail("MicroVM providers require principals declared with RUNTIME 'microvm'.")));

	if (strcmp(effective_runtime, "host") != 0 &&
		(sandbox_name == NULL || strcmp(sandbox_name, "isolated") != 0))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s runtime class \"%s\" requires sandbox \"isolated\"",
						command_name, effective_runtime),
				 errdetail("Containerized and microVM-backed principals must run at the strongest local sandbox ceiling before broker handoff.")));

	if (strcmp(receipt_alg, "ed25519") == 0 &&
		(receipt_signer == NULL || receipt_signer[0] == '\0'))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("%s requires SIGNER for principals bound to ed25519 providers",
						command_name),
				 errdetail("The provider verifies receipts with a public key, so the principal must reference a private signer file.")));
}

static void
qx_validate_principal_runtime_binding(Oid provideroid,
									  const char *runtime_class,
									  const char *sandbox_name,
									  const char *receipt_signer,
									  const char *command_name)
{
	char	   *provider_kind;
	char	   *receipt_alg;

	provider_kind = qx_provider_kind_by_oid(provideroid);
	receipt_alg = qx_provider_receipt_alg_by_oid(provideroid);
	qx_validate_principal_runtime_binding_values(provider_kind,
												 receipt_alg,
												 runtime_class,
												 sandbox_name,
												 receipt_signer,
												 command_name);

	pfree(provider_kind);
	pfree(receipt_alg);
}

static void
qx_validate_provider_bound_principals(Oid provideroid,
									  const char *provider_kind,
									  const char *receipt_alg,
									  const char *command_name)
{
	Relation	rel;
	TableScanDesc scan;
	HeapTuple	tup;

	rel = table_open(QxPrincipalRelationId, AccessShareLock);
	scan = table_beginscan_catalog(rel, 0, NULL);

	while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
	{
		Form_pg_qx_principal principalform = (Form_pg_qx_principal) GETSTRUCT(tup);
		char	   *sandbox_name;
		char	   *runtime_class;
		char	   *receipt_signer;
		char	   *scope;

		if (principalform->qxprincipalproviderid != provideroid)
			continue;

		sandbox_name = qxpolicy_text_attr(tup,
										  Anum_pg_qx_principal_qxprincipalsandbox,
										  QXPRINCIPALOID);
		runtime_class = qxpolicy_text_attr(tup,
										   Anum_pg_qx_principal_qxprincipalruntimeclass,
										   QXPRINCIPALOID);
		receipt_signer = qxpolicy_text_attr(tup,
											Anum_pg_qx_principal_qxprincipalreceiptsigner,
											QXPRINCIPALOID);
		scope = psprintf("%s principal \"%s\"",
						 command_name,
						 NameStr(principalform->qxprincipalname));
		qx_validate_principal_runtime_binding_values(provider_kind,
													 receipt_alg,
													 runtime_class,
													 sandbox_name,
													 receipt_signer,
													 scope);
		pfree(scope);
		if (sandbox_name != NULL)
			pfree(sandbox_name);
		if (runtime_class != NULL)
			pfree(runtime_class);
		if (receipt_signer != NULL)
			pfree(receipt_signer);
	}

	table_endscan(scan);
	table_close(rel, AccessShareLock);
}

static void
qx_validate_tool_sandbox(const char *sandbox_name)
{
	if (sandbox_name == NULL)
		return;

	if (strcmp(sandbox_name, "builtin") == 0 ||
		strcmp(sandbox_name, "restricted") == 0 ||
		strcmp(sandbox_name, "isolated") == 0)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported tool sandbox \"%s\"", sandbox_name),
			 errdetail("QhapaqXian sandboxes currently accept only builtin, restricted, or isolated.")));
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

static HeapTuple
qx_lookup_tool_tuple(RangeVar *tool_name, Oid *namespaceoid)
{
	HeapTuple	tup = NULL;

	if (tool_name->schemaname != NULL)
	{
		*namespaceoid = LookupExplicitNamespace(tool_name->schemaname, false);
		tup = SearchSysCache2(QXTOOLNAMENSP,
							  CStringGetDatum(tool_name->relname),
							  ObjectIdGetDatum(*namespaceoid));
	}
	else
	{
		List	   *search_path;
		ListCell   *lc;

		search_path = fetch_search_path(false);
		foreach(lc, search_path)
		{
			Oid			candidate_nsp = lfirst_oid(lc);

			tup = SearchSysCache2(QXTOOLNAMENSP,
								  CStringGetDatum(tool_name->relname),
								  ObjectIdGetDatum(candidate_nsp));
			if (HeapTupleIsValid(tup))
			{
				*namespaceoid = candidate_nsp;
				break;
			}
		}
		list_free(search_path);
	}

	return tup;
}

static HeapTuple
qx_lookup_provider_tuple(RangeVar *provider_name, Oid *namespaceoid)
{
	HeapTuple	tup = NULL;

	if (provider_name->schemaname != NULL)
	{
		*namespaceoid = LookupExplicitNamespace(provider_name->schemaname, false);
		tup = SearchSysCache2(QXPROVIDERNAMENSP,
							  CStringGetDatum(provider_name->relname),
							  ObjectIdGetDatum(*namespaceoid));
	}
	else
	{
		List	   *search_path;
		ListCell   *lc;

		search_path = fetch_search_path(false);
		foreach(lc, search_path)
		{
			Oid			candidate_nsp = lfirst_oid(lc);

			tup = SearchSysCache2(QXPROVIDERNAMENSP,
								  CStringGetDatum(provider_name->relname),
								  ObjectIdGetDatum(candidate_nsp));
			if (HeapTupleIsValid(tup))
			{
				*namespaceoid = candidate_nsp;
				break;
			}
		}
		list_free(search_path);
	}

	return tup;
}

static HeapTuple
qx_lookup_principal_tuple(RangeVar *principal_name, Oid *namespaceoid)
{
	HeapTuple	tup = NULL;

	if (principal_name->schemaname != NULL)
	{
		*namespaceoid = LookupExplicitNamespace(principal_name->schemaname, false);
		tup = SearchSysCache2(QXPRINCIPALNAMENSP,
							  CStringGetDatum(principal_name->relname),
							  ObjectIdGetDatum(*namespaceoid));
	}
	else
	{
		List	   *search_path;
		ListCell   *lc;

		search_path = fetch_search_path(false);
		foreach(lc, search_path)
		{
			Oid			candidate_nsp = lfirst_oid(lc);

			tup = SearchSysCache2(QXPRINCIPALNAMENSP,
								  CStringGetDatum(principal_name->relname),
								  ObjectIdGetDatum(candidate_nsp));
			if (HeapTupleIsValid(tup))
			{
				*namespaceoid = candidate_nsp;
				break;
			}
		}
		list_free(search_path);
	}

	return tup;
}

static void
qx_record_namespace_policy_dependencies(Oid policyoid, Oid ownerid,
										Oid namespaceoid, Oid authrole)
{
	ObjectAddress myself;
	ObjectAddress referenced;

	ObjectAddressSet(myself, QxNamespaceRelationId, policyoid);
	recordDependencyOnOwner(QxNamespaceRelationId, policyoid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, AuthIdRelationId, authrole);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

static void
qx_record_provider_dependencies(Oid provideroid, Oid ownerid, Oid namespaceoid)
{
	ObjectAddress myself;
	ObjectAddress referenced;

	ObjectAddressSet(myself, QxProviderRelationId, provideroid);
	recordDependencyOnOwner(QxProviderRelationId, provideroid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

static void
qx_record_principal_dependencies(Oid principaloid, Oid ownerid,
								 Oid namespaceoid, Oid provideroid)
{
	ObjectAddress myself;
	ObjectAddress referenced;

	ObjectAddressSet(myself, QxPrincipalRelationId, principaloid);
	recordDependencyOnOwner(QxPrincipalRelationId, principaloid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	if (OidIsValid(provideroid))
	{
		ObjectAddressSet(referenced, QxProviderRelationId, provideroid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
}

static void
qx_record_tool_principal_dependency(Oid tooloid, Oid principaloid)
{
	ObjectAddress myself;
	ObjectAddress referenced;

	if (!OidIsValid(principaloid))
		return;

	ObjectAddressSet(myself, QxToolRelationId, tooloid);
	ObjectAddressSet(referenced, QxPrincipalRelationId, principaloid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

static void
qx_record_tool_dependencies(Oid tooloid, Oid ownerid, Oid namespaceoid,
							Oid principaloid)
{
	ObjectAddress myself;
	ObjectAddress referenced;

	ObjectAddressSet(myself, QxToolRelationId, tooloid);
	recordDependencyOnOwner(QxToolRelationId, tooloid, ownerid);
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	qx_record_tool_principal_dependency(tooloid, principaloid);
}

static Oid
qx_validate_principal_provider_binding(Oid namespaceoid, Oid ownerid,
									   RangeVar *provider_name)
{
	HeapTuple	providertup;
	Form_pg_qx_provider providerform;
	Oid			provideroid;

	if (provider_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("principal provider must not be omitted"),
				 errdetail("Bind the principal to a CREATE PROVIDER object.")));

	provideroid = QxLookupProvider(namespaceoid, provider_name->relname, false);
	providertup = SearchSysCache1(QXPROVIDEROID, ObjectIdGetDatum(provideroid));
	if (!HeapTupleIsValid(providertup))
		elog(ERROR, "cache lookup failed for QhapaqXian provider %u", provideroid);

	providerform = (Form_pg_qx_provider) GETSTRUCT(providertup);
	if (!providerform->qxproviderenabled)
	{
		ReleaseSysCache(providertup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("provider \"%s\" is disabled in schema \"%s\"",
						provider_name->relname, get_namespace_name(namespaceoid))));
	}

	if (!has_privs_of_role(ownerid, providerform->qxproviderowner))
	{
		ReleaseSysCache(providertup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to bind provider \"%s\"",
						provider_name->relname),
				 errdetail("Only the provider owner or a member of that role may bind it to a principal.")));
	}

	ReleaseSysCache(providertup);
	return provideroid;
}

static Oid
qx_validate_tool_principal_binding(Oid namespaceoid, Oid ownerid,
								   const char *principal_name,
								   const char *tool_sandbox)
{
	HeapTuple	principaltup;
	Form_pg_qx_principal principalform;
	char	   *principal_sandbox;
	Oid			principaloid;

	principaloid = QxLookupPrincipal(namespaceoid, principal_name, false);
	principaltup = SearchSysCache1(QXPRINCIPALOID, ObjectIdGetDatum(principaloid));
	if (!HeapTupleIsValid(principaltup))
		elog(ERROR, "cache lookup failed for QhapaqXian principal %u", principaloid);

	principalform = (Form_pg_qx_principal) GETSTRUCT(principaltup);
	if (!principalform->qxprincipalenabled)
	{
		ReleaseSysCache(principaltup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("principal \"%s\" is disabled in schema \"%s\"",
						principal_name, get_namespace_name(namespaceoid))));
	}

	if (!has_privs_of_role(ownerid, principalform->qxprincipalowner))
	{
		ReleaseSysCache(principaltup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to bind principal \"%s\"",
						principal_name),
				 errdetail("Only the principal owner or a member of that role may bind it to a tool.")));
	}

	principal_sandbox = qxpolicy_text_attr(principaltup,
										   Anum_pg_qx_principal_qxprincipalsandbox,
										   QXPRINCIPALOID);
	if (qx_sandbox_rank(tool_sandbox) > qx_sandbox_rank(principal_sandbox))
	{
		if (principal_sandbox != NULL)
			pfree(principal_sandbox);
		ReleaseSysCache(principaltup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("tool sandbox \"%s\" exceeds principal \"%s\" sandbox ceiling",
						tool_sandbox, principal_name),
				 errdetail("A tool may only run at or below the sandbox configured on its principal.")));
	}

	if (principal_sandbox != NULL)
		pfree(principal_sandbox);
	ReleaseSysCache(principaltup);

	return principaloid;
}

void
CreateNamespacePolicyCommand(CreateNamespacePolicyStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_qx_namespace];
	bool		nulls[Natts_pg_qx_namespace];
	HeapTuple	tup;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			authrole;
	Oid			policyoid;
	AclResult	aclresult;
	ObjectAddress myself;

	namespaceoid = LookupExplicitNamespace(stmt->schema_name, false);
	ownerid = GetUserId();
	authrole = stmt->auth_role != NULL ? get_rolespec_oid(stmt->auth_role, false) : ownerid;

	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, ownerid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, get_namespace_name(namespaceoid));

	if (!has_privs_of_role(ownerid, authrole))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to use auth role \"%s\"",
						GetUserNameFromId(authrole, false)),
				 errdetail("The namespace policy owner must be able to assume the auth role.")));

	QxValidateToolList(stmt->allowed_tools);
	QxValidateRegisteredTools(namespaceoid, stmt->allowed_tools, false);

	if (SearchSysCacheExists2(QXNAMESPACENAMENSP,
							  CStringGetDatum(stmt->policy_name),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("namespace policy \"%s\" already exists in schema \"%s\"",
						stmt->policy_name, get_namespace_name(namespaceoid))));

	rel = table_open(QxNamespaceRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	policyoid = GetNewOidWithIndex(rel, QxNamespaceOidIndexId,
								   Anum_pg_qx_namespace_oid);
	values[Anum_pg_qx_namespace_oid - 1] = ObjectIdGetDatum(policyoid);
	values[Anum_pg_qx_namespace_qxnamespacepolicyname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->policy_name));
	values[Anum_pg_qx_namespace_qxnamespaceid - 1] =
		ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_qx_namespace_qxnamespaceowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_namespace_qxnamespaceauthrole - 1] =
		ObjectIdGetDatum(authrole);
	values[Anum_pg_qx_namespace_qxrequireknowntools - 1] =
		BoolGetDatum(stmt->require_known_tools);
	values[Anum_pg_qx_namespace_qxenforcebudgets - 1] =
		BoolGetDatum(stmt->enforce_budgets);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_namespace_qxnamespacepolicy,
					  stmt->policy_name);
	qxpolicy_set_nodetree(values, nulls,
						  Anum_pg_qx_namespace_qxallowedtools,
						  stmt->allowed_tools);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	qx_record_namespace_policy_dependencies(policyoid, ownerid, namespaceoid,
											authrole);
	ObjectAddressSet(myself, QxNamespaceRelationId, policyoid);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxNamespaceRelationId, policyoid, 0);
}

void
AlterNamespacePolicyCommand(AlterNamespacePolicyStmt *stmt)
{
	HeapTuple	oldtup;
	Form_pg_qx_namespace oldform;
	Relation	rel;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_namespace];
	bool		nulls[Natts_pg_qx_namespace];
	bool		replaces[Natts_pg_qx_namespace];
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			authrole = InvalidOid;

	if (!stmt->set_auth_role &&
		!stmt->set_allowed_tools &&
		!stmt->set_require_known_tools &&
		!stmt->set_enforce_budgets)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ALTER NAMESPACE POLICY requires at least one change")));

	namespaceoid = LookupExplicitNamespace(stmt->schema_name, false);
	ownerid = GetUserId();

	oldtup = SearchSysCache2(QXNAMESPACENAMENSP,
							 CStringGetDatum(stmt->policy_name),
							 ObjectIdGetDatum(namespaceoid));
	if (!HeapTupleIsValid(oldtup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("namespace policy \"%s\" does not exist in schema \"%s\"",
						stmt->policy_name, get_namespace_name(namespaceoid))));

	oldform = (Form_pg_qx_namespace) GETSTRUCT(oldtup);
	if (!has_privs_of_role(ownerid, oldform->qxnamespaceowner))
	{
		ReleaseSysCache(oldtup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter namespace policy \"%s\"",
						stmt->policy_name),
				 errdetail("Only the namespace policy owner or a member of that role may alter it.")));
	}

	if (stmt->set_auth_role)
	{
		authrole = get_rolespec_oid(stmt->auth_role, false);
		if (!has_privs_of_role(ownerid, authrole))
		{
			ReleaseSysCache(oldtup);
			ereport(ERROR,
					(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
					 errmsg("permission denied to use auth role \"%s\"",
							GetUserNameFromId(authrole, false)),
					 errdetail("The namespace policy owner must be able to assume the auth role.")));
		}
	}

	if (stmt->set_allowed_tools)
	{
		QxValidateToolList(stmt->allowed_tools);
		QxValidateRegisteredTools(namespaceoid, stmt->allowed_tools, false);
	}

	rel = table_open(QxNamespaceRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (stmt->set_auth_role)
	{
		values[Anum_pg_qx_namespace_qxnamespaceauthrole - 1] =
			ObjectIdGetDatum(authrole);
		replaces[Anum_pg_qx_namespace_qxnamespaceauthrole - 1] = true;
	}

	if (stmt->set_allowed_tools)
	{
		qxpolicy_set_nodetree(values, nulls,
							  Anum_pg_qx_namespace_qxallowedtools,
							  stmt->allowed_tools);
		replaces[Anum_pg_qx_namespace_qxallowedtools - 1] = true;
	}

	if (stmt->set_require_known_tools)
	{
		values[Anum_pg_qx_namespace_qxrequireknowntools - 1] =
			BoolGetDatum(stmt->require_known_tools);
		replaces[Anum_pg_qx_namespace_qxrequireknowntools - 1] = true;
	}

	if (stmt->set_enforce_budgets)
	{
		values[Anum_pg_qx_namespace_qxenforcebudgets - 1] =
			BoolGetDatum(stmt->enforce_budgets);
		replaces[Anum_pg_qx_namespace_qxenforcebudgets - 1] = true;
	}

	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   values, nulls, replaces);
	CatalogTupleUpdate(rel, &oldtup->t_self, newtup);
	heap_freetuple(newtup);
	table_close(rel, RowExclusiveLock);

	deleteDependencyRecordsForClass(QxNamespaceRelationId, oldform->oid,
									NamespaceRelationId, DEPENDENCY_NORMAL);
	deleteDependencyRecordsForClass(QxNamespaceRelationId, oldform->oid,
									AuthIdRelationId, DEPENDENCY_NORMAL);
	{
		ObjectAddress myself;
		ObjectAddress referenced;

		ObjectAddressSet(myself, QxNamespaceRelationId, oldform->oid);
		ObjectAddressSet(referenced, NamespaceRelationId, oldform->qxnamespaceid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		ObjectAddressSet(referenced, AuthIdRelationId,
						 stmt->set_auth_role ? authrole :
						 oldform->qxnamespaceauthrole);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
	InvokeObjectPostAlterHook(QxNamespaceRelationId, oldform->oid, 0);
	ReleaseSysCache(oldtup);
}

void
CreateProviderCommand(CreateProviderStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_qx_provider];
	bool		nulls[Natts_pg_qx_provider];
	HeapTuple	tup;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			provideroid;
	AclResult	aclresult;
	ObjectAddress myself;
	const char *receipt_alg;

	namespaceoid = RangeVarGetCreationNamespace(stmt->provider_name);
	ownerid = GetUserId();
	receipt_alg = (stmt->receipt_alg != NULL && stmt->receipt_alg[0] != '\0') ?
		stmt->receipt_alg : "hmac-sha256";

	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, ownerid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, get_namespace_name(namespaceoid));

	qx_validate_provider_kind(stmt->provider_kind, "CREATE PROVIDER");
	qx_validate_provider_endpoint(stmt->provider_kind, stmt->endpoint_name,
								  "CREATE PROVIDER");
	qx_validate_provider_receipt_alg(receipt_alg, "CREATE PROVIDER");
	qx_validate_provider_receipt_key(receipt_alg, stmt->receipt_key,
									 "CREATE PROVIDER");
	qx_validate_provider_isolation_contract(stmt->provider_kind,
											 receipt_alg,
											 stmt->attestation_required,
											 "CREATE PROVIDER");

	if (SearchSysCacheExists2(QXPROVIDERNAMENSP,
							  CStringGetDatum(stmt->provider_name->relname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("provider \"%s\" already exists in schema \"%s\"",
						stmt->provider_name->relname,
						get_namespace_name(namespaceoid))));

	rel = table_open(QxProviderRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	provideroid = GetNewOidWithIndex(rel, QxProviderOidIndexId,
									 Anum_pg_qx_provider_oid);
	values[Anum_pg_qx_provider_oid - 1] = ObjectIdGetDatum(provideroid);
	values[Anum_pg_qx_provider_qxprovidername - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->provider_name->relname));
	values[Anum_pg_qx_provider_qxprovidernamespace - 1] =
		ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_qx_provider_qxproviderowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_provider_qxproviderenabled - 1] =
		BoolGetDatum(true);
	values[Anum_pg_qx_provider_qxproviderattestationrequired - 1] =
		BoolGetDatum(stmt->attestation_required);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_provider_qxproviderkind,
					  stmt->provider_kind);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_provider_qxproviderendpoint,
					  stmt->endpoint_name);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_provider_qxproviderreceiptalg,
					  receipt_alg);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_provider_qxproviderreceiptkey,
					  stmt->receipt_key);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	qx_record_provider_dependencies(provideroid, ownerid, namespaceoid);
	ObjectAddressSet(myself, QxProviderRelationId, provideroid);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxProviderRelationId, provideroid, 0);
}

void
AlterProviderCommand(AlterProviderStmt *stmt)
{
	HeapTuple	oldtup;
	Form_pg_qx_provider oldform;
	Relation	rel;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_provider];
	bool		nulls[Natts_pg_qx_provider];
	bool		replaces[Natts_pg_qx_provider];
	Oid			namespaceoid = InvalidOid;
	Oid			ownerid;
	char	   *effective_kind = NULL;
	char	   *effective_endpoint = NULL;
	char	   *effective_receipt_alg = NULL;
	char	   *effective_receipt_key = NULL;
	bool		effective_attestation_required;

	if (!stmt->set_kind &&
		!stmt->set_endpoint &&
		!stmt->set_receipt_alg &&
		!stmt->set_receipt_key &&
		!stmt->set_attestation_required &&
		!stmt->set_enabled)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ALTER PROVIDER requires at least one change")));

	ownerid = GetUserId();
	oldtup = qx_lookup_provider_tuple(stmt->provider_name, &namespaceoid);
	if (!HeapTupleIsValid(oldtup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("provider \"%s\" does not exist",
						stmt->provider_name->relname)));

	oldform = (Form_pg_qx_provider) GETSTRUCT(oldtup);
	if (!has_privs_of_role(ownerid, oldform->qxproviderowner))
	{
		ReleaseSysCache(oldtup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter provider \"%s\"",
						stmt->provider_name->relname),
				 errdetail("Only the provider owner or a member of that role may alter it.")));
	}

	if (stmt->set_kind)
		qx_validate_provider_kind(stmt->provider_kind, "ALTER PROVIDER");
	if (stmt->set_receipt_alg)
		qx_validate_provider_receipt_alg(stmt->receipt_alg, "ALTER PROVIDER");
	effective_kind = stmt->set_kind ? pstrdup(stmt->provider_kind) :
		qxpolicy_text_attr(oldtup,
						   Anum_pg_qx_provider_qxproviderkind,
						   QXPROVIDEROID);
	effective_endpoint = stmt->set_endpoint ? pstrdup(stmt->endpoint_name) :
		qxpolicy_text_attr(oldtup,
						   Anum_pg_qx_provider_qxproviderendpoint,
						   QXPROVIDEROID);
	if (stmt->set_kind || stmt->set_endpoint)
		qx_validate_provider_endpoint(effective_kind,
									  effective_endpoint,
									  "ALTER PROVIDER");
	effective_receipt_alg = stmt->set_receipt_alg ? pstrdup(stmt->receipt_alg) :
		qxpolicy_text_attr(oldtup,
						   Anum_pg_qx_provider_qxproviderreceiptalg,
						   QXPROVIDEROID);
	if (effective_receipt_alg == NULL || effective_receipt_alg[0] == '\0')
	{
		if (effective_receipt_alg != NULL)
			pfree(effective_receipt_alg);
		effective_receipt_alg = pstrdup("hmac-sha256");
	}
	effective_receipt_key = stmt->set_receipt_key ? pstrdup(stmt->receipt_key) :
		qxpolicy_text_attr(oldtup,
						   Anum_pg_qx_provider_qxproviderreceiptkey,
						   QXPROVIDEROID);
	if (stmt->set_receipt_alg || stmt->set_receipt_key)
		qx_validate_provider_receipt_key(effective_receipt_alg,
										 effective_receipt_key,
										 "ALTER PROVIDER");
	effective_attestation_required = stmt->set_attestation_required ?
		stmt->attestation_required :
		oldform->qxproviderattestationrequired;
	qx_validate_provider_isolation_contract(effective_kind,
											 effective_receipt_alg,
											 effective_attestation_required,
											 "ALTER PROVIDER");
	qx_validate_provider_bound_principals(oldform->oid,
										  effective_kind,
										  effective_receipt_alg,
										  "ALTER PROVIDER");

	rel = table_open(QxProviderRelationId, RowExclusiveLock);
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (stmt->set_kind)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_provider_qxproviderkind,
						  stmt->provider_kind);
		replaces[Anum_pg_qx_provider_qxproviderkind - 1] = true;
	}
	if (stmt->set_endpoint)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_provider_qxproviderendpoint,
						  stmt->endpoint_name);
		replaces[Anum_pg_qx_provider_qxproviderendpoint - 1] = true;
	}
	if (stmt->set_receipt_alg)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_provider_qxproviderreceiptalg,
						  stmt->receipt_alg);
		replaces[Anum_pg_qx_provider_qxproviderreceiptalg - 1] = true;
	}
	if (stmt->set_receipt_key)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_provider_qxproviderreceiptkey,
						  stmt->receipt_key);
		replaces[Anum_pg_qx_provider_qxproviderreceiptkey - 1] = true;
	}
	if (stmt->set_attestation_required)
	{
		values[Anum_pg_qx_provider_qxproviderattestationrequired - 1] =
			BoolGetDatum(stmt->attestation_required);
		replaces[Anum_pg_qx_provider_qxproviderattestationrequired - 1] = true;
	}
	if (stmt->set_enabled)
	{
		values[Anum_pg_qx_provider_qxproviderenabled - 1] =
			BoolGetDatum(stmt->enabled);
		replaces[Anum_pg_qx_provider_qxproviderenabled - 1] = true;
	}

	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   values, nulls, replaces);
	CatalogTupleUpdate(rel, &oldtup->t_self, newtup);
	heap_freetuple(newtup);
	table_close(rel, RowExclusiveLock);
	if (effective_kind != NULL)
		pfree(effective_kind);
	if (effective_endpoint != NULL)
		pfree(effective_endpoint);
	if (effective_receipt_alg != NULL)
		pfree(effective_receipt_alg);
	if (effective_receipt_key != NULL)
		pfree(effective_receipt_key);

	InvokeObjectPostAlterHook(QxProviderRelationId, oldform->oid, 0);
	ReleaseSysCache(oldtup);
}

void
CreatePrincipalCommand(CreatePrincipalStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_qx_principal];
	bool		nulls[Natts_pg_qx_principal];
	HeapTuple	tup;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			provideroid;
	Oid			principaloid;
	AclResult	aclresult;
	ObjectAddress myself;
	char	   *effective_runtime_class;

	namespaceoid = RangeVarGetCreationNamespace(stmt->principal_name);
	ownerid = GetUserId();

	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, ownerid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, get_namespace_name(namespaceoid));

	qx_validate_tool_sandbox(stmt->sandbox_name);
	qx_validate_principal_program(stmt->program_name, "CREATE PRINCIPAL");
	provideroid = qx_validate_principal_provider_binding(namespaceoid, ownerid,
														 stmt->provider_name);
	if (stmt->runtime_class != NULL && stmt->runtime_class[0] != '\0')
		effective_runtime_class = pstrdup(stmt->runtime_class);
	else
	{
		char	   *provider_kind;

		provider_kind = qx_provider_kind_by_oid(provideroid);
		effective_runtime_class =
			pstrdup(qx_default_runtime_for_provider_kind(provider_kind));
		pfree(provider_kind);
	}
	qx_validate_principal_runtime_binding(provideroid,
										  effective_runtime_class,
										  stmt->sandbox_name,
										  stmt->receipt_signer,
										  "CREATE PRINCIPAL");

	if (SearchSysCacheExists2(QXPRINCIPALNAMENSP,
							  CStringGetDatum(stmt->principal_name->relname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("principal \"%s\" already exists in schema \"%s\"",
						stmt->principal_name->relname,
						get_namespace_name(namespaceoid))));

	rel = table_open(QxPrincipalRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	principaloid = GetNewOidWithIndex(rel, QxPrincipalOidIndexId,
									  Anum_pg_qx_principal_oid);
	values[Anum_pg_qx_principal_oid - 1] = ObjectIdGetDatum(principaloid);
	values[Anum_pg_qx_principal_qxprincipalname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->principal_name->relname));
	values[Anum_pg_qx_principal_qxprincipalnamespace - 1] =
		ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_qx_principal_qxprincipalowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_principal_qxprincipalproviderid - 1] =
		ObjectIdGetDatum(provideroid);
	values[Anum_pg_qx_principal_qxprincipalenabled - 1] =
		BoolGetDatum(stmt->enabled);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_principal_qxprincipalsandbox,
					  stmt->sandbox_name);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_principal_qxprincipalprogram,
					  stmt->program_name);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_principal_qxprincipalprovider,
					  stmt->provider_name->relname);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_principal_qxprincipalruntimeclass,
					  effective_runtime_class);
	qxpolicy_set_text(values, nulls,
					  Anum_pg_qx_principal_qxprincipalreceiptsigner,
					  stmt->receipt_signer);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	qx_record_principal_dependencies(principaloid, ownerid, namespaceoid,
									 provideroid);
	pfree(effective_runtime_class);
	ObjectAddressSet(myself, QxPrincipalRelationId, principaloid);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxPrincipalRelationId, principaloid, 0);
}

void
AlterPrincipalCommand(AlterPrincipalStmt *stmt)
{
	HeapTuple	oldtup;
	Form_pg_qx_principal oldform;
	Relation	rel;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_principal];
	bool		nulls[Natts_pg_qx_principal];
	bool		replaces[Natts_pg_qx_principal];
	Oid			namespaceoid = InvalidOid;
	Oid			ownerid;
	Oid			provideroid = InvalidOid;
	Oid			effective_provideroid = InvalidOid;
	char	   *effective_runtime_class = NULL;
	char	   *effective_sandbox_name = NULL;
	char	   *effective_receipt_signer = NULL;

	if (!stmt->set_provider &&
		!stmt->set_program &&
		!stmt->set_sandbox &&
		!stmt->set_runtime &&
		!stmt->set_receipt_signer &&
		!stmt->set_enabled)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ALTER PRINCIPAL requires at least one change")));

	ownerid = GetUserId();
	oldtup = qx_lookup_principal_tuple(stmt->principal_name, &namespaceoid);
	if (!HeapTupleIsValid(oldtup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("principal \"%s\" does not exist",
						stmt->principal_name->relname)));

	oldform = (Form_pg_qx_principal) GETSTRUCT(oldtup);
	if (!has_privs_of_role(ownerid, oldform->qxprincipalowner))
	{
		ReleaseSysCache(oldtup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter principal \"%s\"",
						stmt->principal_name->relname),
				 errdetail("Only the principal owner or a member of that role may alter it.")));
	}

	if (stmt->set_provider)
		provideroid = qx_validate_principal_provider_binding(namespaceoid, ownerid,
															 stmt->provider_name);
	if (stmt->set_program)
		qx_validate_principal_program(stmt->program_name, "ALTER PRINCIPAL");
	if (stmt->set_sandbox)
		qx_validate_tool_sandbox(stmt->sandbox_name);
	effective_provideroid = stmt->set_provider ? provideroid :
		oldform->qxprincipalproviderid;
	if (stmt->set_runtime)
		effective_runtime_class = pstrdup(stmt->runtime_class);
	else
		effective_runtime_class = qxpolicy_text_attr(oldtup,
													 Anum_pg_qx_principal_qxprincipalruntimeclass,
													 QXPRINCIPALOID);
	if (effective_runtime_class == NULL || effective_runtime_class[0] == '\0')
	{
		char	   *provider_kind;

		if (effective_runtime_class != NULL)
			pfree(effective_runtime_class);
		provider_kind = qx_provider_kind_by_oid(effective_provideroid);
		effective_runtime_class =
			pstrdup(qx_default_runtime_for_provider_kind(provider_kind));
		pfree(provider_kind);
	}
	effective_sandbox_name = stmt->set_sandbox ? pstrdup(stmt->sandbox_name) :
		qxpolicy_text_attr(oldtup,
						   Anum_pg_qx_principal_qxprincipalsandbox,
						   QXPRINCIPALOID);
	effective_receipt_signer = stmt->set_receipt_signer ? stmt->receipt_signer :
		qxpolicy_text_attr(oldtup,
						   Anum_pg_qx_principal_qxprincipalreceiptsigner,
						   QXPRINCIPALOID);
	qx_validate_principal_runtime_binding(effective_provideroid,
										  effective_runtime_class,
										  effective_sandbox_name,
										  effective_receipt_signer,
										  "ALTER PRINCIPAL");

	rel = table_open(QxPrincipalRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (stmt->set_provider)
	{
		values[Anum_pg_qx_principal_qxprincipalproviderid - 1] =
			ObjectIdGetDatum(provideroid);
		replaces[Anum_pg_qx_principal_qxprincipalproviderid - 1] = true;
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_principal_qxprincipalprovider,
						  stmt->provider_name->relname);
		replaces[Anum_pg_qx_principal_qxprincipalprovider - 1] = true;
	}

	if (stmt->set_program)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_principal_qxprincipalprogram,
						  stmt->program_name);
		replaces[Anum_pg_qx_principal_qxprincipalprogram - 1] = true;
	}

	if (stmt->set_sandbox)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_principal_qxprincipalsandbox,
						  stmt->sandbox_name);
		replaces[Anum_pg_qx_principal_qxprincipalsandbox - 1] = true;
	}
	if (stmt->set_runtime)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_principal_qxprincipalruntimeclass,
						  stmt->runtime_class);
		replaces[Anum_pg_qx_principal_qxprincipalruntimeclass - 1] = true;
	}
	if (stmt->set_receipt_signer)
	{
		qxpolicy_set_text(values, nulls,
						  Anum_pg_qx_principal_qxprincipalreceiptsigner,
						  stmt->receipt_signer);
		replaces[Anum_pg_qx_principal_qxprincipalreceiptsigner - 1] = true;
	}

	if (stmt->set_enabled)
	{
		values[Anum_pg_qx_principal_qxprincipalenabled - 1] =
			BoolGetDatum(stmt->enabled);
		replaces[Anum_pg_qx_principal_qxprincipalenabled - 1] = true;
	}

	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   values, nulls, replaces);
	CatalogTupleUpdate(rel, &oldtup->t_self, newtup);
	heap_freetuple(newtup);
	table_close(rel, RowExclusiveLock);
	if (effective_runtime_class != NULL)
		pfree(effective_runtime_class);
	if (effective_sandbox_name != NULL)
		pfree(effective_sandbox_name);
	if (!stmt->set_receipt_signer && effective_receipt_signer != NULL)
		pfree(effective_receipt_signer);

	deleteDependencyRecordsForClass(QxPrincipalRelationId, oldform->oid,
									QxProviderRelationId, DEPENDENCY_NORMAL);
	if (OidIsValid(stmt->set_provider ? provideroid :
				   oldform->qxprincipalproviderid))
	{
		ObjectAddress myself;
		ObjectAddress referenced;

		ObjectAddressSet(myself, QxPrincipalRelationId, oldform->oid);
		ObjectAddressSet(referenced, QxProviderRelationId,
						 stmt->set_provider ? provideroid :
						 oldform->qxprincipalproviderid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	InvokeObjectPostAlterHook(QxPrincipalRelationId, oldform->oid, 0);
	ReleaseSysCache(oldtup);
}

void
CreateToolCommand(CreateToolStmt *stmt)
{
	Relation	rel;
	Datum		values[Natts_pg_qx_tool];
	bool		nulls[Natts_pg_qx_tool];
	HeapTuple	tup;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			tooloid;
	Oid			principaloid;
	AclResult	aclresult;
	ObjectAddress myself;

	namespaceoid = RangeVarGetCreationNamespace(stmt->tool_name);
	ownerid = GetUserId();

	aclresult = object_aclcheck(NamespaceRelationId, namespaceoid, ownerid, ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA, get_namespace_name(namespaceoid));

	qx_validate_tool_costs(stmt->token_cost, stmt->cost_units, "CREATE TOOL");
	qx_validate_tool_sandbox(stmt->sandbox_name);
	principaloid = qx_validate_tool_principal_binding(namespaceoid, ownerid,
													  stmt->principal_name,
													  stmt->sandbox_name);

	if (stmt->policy_name != NULL)
		(void) QxLookupNamespacePolicy(namespaceoid, stmt->policy_name, false);

	if (SearchSysCacheExists2(QXTOOLNAMENSP,
							  CStringGetDatum(stmt->tool_name->relname),
							  ObjectIdGetDatum(namespaceoid)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("tool \"%s\" already exists in schema \"%s\"",
						stmt->tool_name->relname, get_namespace_name(namespaceoid))));

	rel = table_open(QxToolRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	tooloid = GetNewOidWithIndex(rel, QxToolOidIndexId,
								 Anum_pg_qx_tool_oid);
	values[Anum_pg_qx_tool_oid - 1] = ObjectIdGetDatum(tooloid);
	values[Anum_pg_qx_tool_qxtoolname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->tool_name->relname));
	values[Anum_pg_qx_tool_qxtoolnamespace - 1] =
		ObjectIdGetDatum(namespaceoid);
	values[Anum_pg_qx_tool_qxtoolowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_tool_qxtoolprincipalid - 1] =
		ObjectIdGetDatum(principaloid);
	values[Anum_pg_qx_tool_qxtoolenabled - 1] = BoolGetDatum(stmt->enabled);
	values[Anum_pg_qx_tool_qxtooltokencost - 1] = Int32GetDatum(stmt->token_cost);
	values[Anum_pg_qx_tool_qxtoolcostunits - 1] = Int32GetDatum(stmt->cost_units);
	qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolhandler,
					  stmt->handler_name);
	qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolsandbox,
					  stmt->sandbox_name);
	qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolprincipal,
					  stmt->principal_name);
	qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolpolicy,
					  stmt->policy_name);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);

	qx_record_tool_dependencies(tooloid, ownerid, namespaceoid, principaloid);
	ObjectAddressSet(myself, QxToolRelationId, tooloid);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxToolRelationId, tooloid, 0);
}

void
AlterToolCommand(AlterToolStmt *stmt)
{
	HeapTuple	oldtup;
	Form_pg_qx_tool oldform;
	Relation	rel;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_tool];
	bool		nulls[Natts_pg_qx_tool];
	bool		replaces[Natts_pg_qx_tool];
	Oid			namespaceoid = InvalidOid;
	Oid			ownerid;
	Oid			principaloid = InvalidOid;
	char	   *effective_principal_name = NULL;
	char	   *effective_sandbox_name = NULL;

	if (!stmt->set_handler &&
		!stmt->set_sandbox &&
		!stmt->set_principal &&
		!stmt->set_policy &&
		!stmt->set_token_cost &&
		!stmt->set_cost_units &&
		!stmt->set_enabled)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("ALTER TOOL requires at least one change")));

	ownerid = GetUserId();
	oldtup = qx_lookup_tool_tuple(stmt->tool_name, &namespaceoid);
	if (!HeapTupleIsValid(oldtup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("tool \"%s\" does not exist", stmt->tool_name->relname)));

	oldform = (Form_pg_qx_tool) GETSTRUCT(oldtup);
	if (!has_privs_of_role(ownerid, oldform->qxtoolowner))
	{
		ReleaseSysCache(oldtup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to alter tool \"%s\"",
						stmt->tool_name->relname),
				 errdetail("Only the tool owner or a member of that role may alter it.")));
	}

	if (stmt->set_sandbox)
		qx_validate_tool_sandbox(stmt->sandbox_name);
	if (stmt->set_token_cost || stmt->set_cost_units)
		qx_validate_tool_costs(stmt->set_token_cost ? stmt->token_cost : oldform->qxtooltokencost,
							   stmt->set_cost_units ? stmt->cost_units : oldform->qxtoolcostunits,
							   "ALTER TOOL");
	if (stmt->set_policy && stmt->policy_name != NULL)
		(void) QxLookupNamespacePolicy(namespaceoid, stmt->policy_name, false);

	effective_principal_name = stmt->set_principal ? stmt->principal_name :
		qxpolicy_text_attr(oldtup, Anum_pg_qx_tool_qxtoolprincipal, QXTOOLOID);
	effective_sandbox_name = stmt->set_sandbox ? stmt->sandbox_name :
		qxpolicy_text_attr(oldtup, Anum_pg_qx_tool_qxtoolsandbox, QXTOOLOID);
	principaloid = qx_validate_tool_principal_binding(namespaceoid,
													  ownerid,
													  effective_principal_name,
													  effective_sandbox_name);

	rel = table_open(QxToolRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	if (stmt->set_handler)
	{
		qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolhandler,
						  stmt->handler_name);
		replaces[Anum_pg_qx_tool_qxtoolhandler - 1] = true;
	}

	if (stmt->set_sandbox)
	{
		qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolsandbox,
						  stmt->sandbox_name);
		replaces[Anum_pg_qx_tool_qxtoolsandbox - 1] = true;
	}

	if (stmt->set_principal)
	{
		values[Anum_pg_qx_tool_qxtoolprincipalid - 1] =
			ObjectIdGetDatum(principaloid);
		replaces[Anum_pg_qx_tool_qxtoolprincipalid - 1] = true;
		qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolprincipal,
						  stmt->principal_name);
		replaces[Anum_pg_qx_tool_qxtoolprincipal - 1] = true;
	}

	if (stmt->set_policy)
	{
		qxpolicy_set_text(values, nulls, Anum_pg_qx_tool_qxtoolpolicy,
						  stmt->policy_name);
		replaces[Anum_pg_qx_tool_qxtoolpolicy - 1] = true;
	}

	if (stmt->set_token_cost)
	{
		values[Anum_pg_qx_tool_qxtooltokencost - 1] =
			Int32GetDatum(stmt->token_cost);
		replaces[Anum_pg_qx_tool_qxtooltokencost - 1] = true;
	}

	if (stmt->set_cost_units)
	{
		values[Anum_pg_qx_tool_qxtoolcostunits - 1] =
			Int32GetDatum(stmt->cost_units);
		replaces[Anum_pg_qx_tool_qxtoolcostunits - 1] = true;
	}

	if (stmt->set_enabled)
	{
		values[Anum_pg_qx_tool_qxtoolenabled - 1] =
			BoolGetDatum(stmt->enabled);
		replaces[Anum_pg_qx_tool_qxtoolenabled - 1] = true;
	}

	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   values, nulls, replaces);
	CatalogTupleUpdate(rel, &oldtup->t_self, newtup);
	heap_freetuple(newtup);
	table_close(rel, RowExclusiveLock);

	if (!stmt->set_principal && effective_principal_name != NULL)
		pfree(effective_principal_name);
	if (!stmt->set_sandbox && effective_sandbox_name != NULL)
		pfree(effective_sandbox_name);

	if (stmt->set_principal)
	{
		deleteDependencyRecordsForClass(QxToolRelationId, oldform->oid,
										QxPrincipalRelationId, DEPENDENCY_NORMAL);
		qx_record_tool_principal_dependency(oldform->oid, principaloid);
	}

	InvokeObjectPostAlterHook(QxToolRelationId, oldform->oid, 0);
	ReleaseSysCache(oldtup);
}
