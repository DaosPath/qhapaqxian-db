/*-------------------------------------------------------------------------
 *
 * qx_security.h
 *	  identity, tool, and budget helpers for QhapaqXian Engine
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_SECURITY_H
#define QX_SECURITY_H

#include "nodes/nodes.h"
#include "nodes/pg_list.h"

typedef struct QxBudgetPolicy
{
	bool		has_token_limit;
	bool		has_cost_limit;
	int32		token_limit;
	int32		cost_limit;
} QxBudgetPolicy;

typedef struct QxToolAuthorization
{
	Oid			namespace_policy_oid;
	char	   *namespace_policy_name;
	bool		require_known_tools;
	bool		enforce_budgets;
	int32		tool_count;
	int32		tool_token_cost;
	int32		tool_cost_units;
	List	   *tool_oids;		/* list of Oid */
} QxToolAuthorization;

extern Oid QxEnsureNamespacePolicy(Oid namespaceoid, Oid ownerid,
								   Oid authrole, const char *policy_name,
								   List *allowed_tools);
extern Oid QxEnsureOperationalIdentity(Oid namespaceoid, Oid ownerid,
									   const char *identity_name,
									   Oid authrole,
									   const char *policy_name,
									   List *budget_options);
extern void QxEnsureToolCatalogEntries(Oid namespaceoid, Oid ownerid,
									   List *tools);
extern char *QxIdentityNameById(Oid identityoid);
extern char *QxNamespacePolicyNameById(Oid policyoid);
extern void QxBudgetPolicyFromDefList(List *budget_options,
									  QxBudgetPolicy *policy);
extern void QxBudgetPolicyFromSerialized(const char *serialized,
										 QxBudgetPolicy *policy);
extern List *QxDeserializeToolList(const char *serialized);
extern void QxValidateToolList(List *tools);
extern void QxAuthorizeToolsForNamespace(Oid namespacepolicyoid,
										 Oid namespaceoid,
										 Oid ownerid,
										 List *tools,
										 QxToolAuthorization *authz);

#endif							/* QX_SECURITY_H */
