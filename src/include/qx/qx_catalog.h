/*-------------------------------------------------------------------------
 *
 * qx_catalog.h
 *	  public catalog helper snapshots for QhapaqXian Engine
 *
 * The goal of this layer is to centralize catalog access for fork-owned
 * runtime, security, planner, and recovery code. Callers should prefer these
 * helpers over open-coding syscache access when they only need a stable
 * snapshot of catalog state.
 *
 *-------------------------------------------------------------------------*/
#ifndef QX_CATALOG_H
#define QX_CATALOG_H

#include "access/xlogdefs.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "qx/qx_scheduler.h"

typedef struct QxCatalogAgentInfo
{
	Oid			oid;
	char	   *name;
	Oid			namespaceoid;
	Oid			namespacepolicyoid;
	Oid			identityoid;
	Oid			ownerid;
	char	   *identity;
	char	   *model_uri;
	char	   *memory_profile;
	char	   *policy;
	char	   *tools;
	char	   *budget;
} QxCatalogAgentInfo;

typedef struct QxCatalogIdentityInfo
{
	Oid			oid;
	char	   *name;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			authrole;
	char	   *policy;
	char	   *budget;
} QxCatalogIdentityInfo;

typedef struct QxCatalogProviderInfo
{
	Oid			oid;
	char	   *name;
	Oid			namespaceoid;
	Oid			ownerid;
	bool		enabled;
	bool		attestation_required;
	char	   *kind;
	char	   *endpoint;
	char	   *receipt_alg;
	char	   *receipt_key;
	char	   *attestation_profile;
	char	   *attestation_version;
	char	   *attestation_policy;
} QxCatalogProviderInfo;

typedef struct QxCatalogPrincipalInfo
{
	Oid			oid;
	char	   *name;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			provideroid;
	bool		enabled;
	char	   *sandbox;
	char	   *program;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *provider_receipt_alg;
	bool		provider_enabled;
	bool		provider_attestation_required;
	char	   *runtime_class;
	char	   *receipt_signer;
	char	   *attestation_profile;
	char	   *attestation_version;
	char	   *attestation_policy;
} QxCatalogPrincipalInfo;

typedef struct QxCatalogNamespacePolicyInfo
{
	Oid			oid;
	char	   *name;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			authrole;
	bool		require_known_tools;
	bool		enforce_budgets;
	char	   *policy;
	char	   *allowed_tools;
} QxCatalogNamespacePolicyInfo;

typedef struct QxCatalogToolInfo
{
	Oid			oid;
	char	   *name;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			principaloid;
	Oid			provideroid;
	bool		enabled;
	int32		token_cost;
	int32		cost_units;
	bool		principal_enabled;
	bool		provider_enabled;
	char	   *handler;
	char	   *sandbox;
	char	   *runtime_class;
	char	   *sandbox_ceiling;
	char	   *capability_tags;
	char	   *principal_name;
	char	   *principal_sandbox;
	char	   *principal_runtime_class;
	char	   *principal_program;
	char	   *principal_receipt_signer;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *provider_receipt_alg;
	bool		provider_attestation_required;
	char	   *policy;
} QxCatalogToolInfo;

typedef struct QxCatalogSessionInfo
{
	Oid			oid;
	Oid			dbid;
	Oid			agentoid;
	Oid			namespacepolicyoid;
	Oid			identityoid;
	Oid			ownerid;
	char		status;
	char	   *context;
	char	   *agent_name;
	char	   *identity_name;
	char	   *policy_name;
} QxCatalogSessionInfo;

typedef struct QxCatalogTaskInfo
{
	Oid			oid;
	Oid			dbid;
	Oid			sessionoid;
	Oid			agentoid;
	Oid			namespacepolicyoid;
	Oid			identityoid;
	Oid			ownerid;
	Oid			lastattemptid;
	Oid			lastcheckpointid;
	int32		budget_tokens;
	int32		budget_cost;
	int32		authorized_tool_tokens;
	int32		authorized_tool_cost;
	int32		estimated_tokens;
	int32		estimated_cost;
	int32		consumed_tokens;
	int32		consumed_cost;
	char		state;
	char	   *name;
	char	   *goal;
	char	   *input;
	char	   *priority;
	char	   *authorized_tools;
	char	   *submit_contract;
	char	   *resume_contract;
	char	   *agent_name;
	char	   *identity_name;
	char	   *policy_name;
} QxCatalogTaskInfo;

typedef struct QxCatalogAttemptInfo
{
	Oid			oid;
	Oid			dbid;
	Oid			sessionoid;
	Oid			taskoid;
	Oid			ownerid;
	Oid			resumecheckpointid;
	int16		seqno;
	char		state;
	char	   *strategy;
} QxCatalogAttemptInfo;

typedef struct QxCatalogCheckpointInfo
{
	Oid			oid;
	Oid			dbid;
	Oid			sessionoid;
	Oid			taskoid;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			ownerid;
	char		state;
	char		task_state;
	int16		next_step_seqno;
	XLogRecPtr	lsn;
	char	   *label;
	char	   *data;
} QxCatalogCheckpointInfo;

extern void QxCatalogFreeAgentInfo(QxCatalogAgentInfo *info);
extern void QxCatalogFreeIdentityInfo(QxCatalogIdentityInfo *info);
extern void QxCatalogFreeProviderInfo(QxCatalogProviderInfo *info);
extern void QxCatalogFreePrincipalInfo(QxCatalogPrincipalInfo *info);
extern void QxCatalogFreeNamespacePolicyInfo(QxCatalogNamespacePolicyInfo *info);
extern void QxCatalogFreeToolInfo(QxCatalogToolInfo *info);
extern void QxCatalogFreeSessionInfo(QxCatalogSessionInfo *info);
extern void QxCatalogFreeTaskInfo(QxCatalogTaskInfo *info);
extern void QxCatalogFreeAttemptInfo(QxCatalogAttemptInfo *info);
extern void QxCatalogFreeCheckpointInfo(QxCatalogCheckpointInfo *info);
extern void QxCatalogFreeTaskInfoList(List *tasks);
extern void QxCatalogFreeAttemptInfoList(List *attempts);
extern void QxCatalogFreeCheckpointInfoList(List *checkpoints);

extern bool QxCatalogLookupAgentByOid(Oid agentoid, QxCatalogAgentInfo *info);
extern bool QxCatalogLookupAgentByName(Oid namespaceoid, const char *name,
									   QxCatalogAgentInfo *info);
extern bool QxCatalogLookupIdentityByOid(Oid identityoid,
										 QxCatalogIdentityInfo *info);
extern bool QxCatalogLookupIdentityByName(Oid namespaceoid, const char *name,
										  QxCatalogIdentityInfo *info);
extern bool QxCatalogLookupProviderByOid(Oid provideroid,
										 QxCatalogProviderInfo *info);
extern bool QxCatalogLookupProviderByName(Oid namespaceoid, const char *name,
										  QxCatalogProviderInfo *info);
extern bool QxCatalogLookupPrincipalByOid(Oid principaloid,
										  QxCatalogPrincipalInfo *info);
extern bool QxCatalogLookupPrincipalByName(Oid namespaceoid, const char *name,
										   QxCatalogPrincipalInfo *info);
extern bool QxCatalogLookupNamespacePolicyByOid(Oid policyoid,
												QxCatalogNamespacePolicyInfo *info);
extern bool QxCatalogLookupNamespacePolicyByName(Oid namespaceoid,
												 const char *name,
												 QxCatalogNamespacePolicyInfo *info);
extern bool QxCatalogLookupToolByOid(Oid tooloid, QxCatalogToolInfo *info);
extern bool QxCatalogLookupToolByName(Oid namespaceoid, const char *name,
									  QxCatalogToolInfo *info);
extern bool QxCatalogLookupSessionByOid(Oid sessionoid,
										QxCatalogSessionInfo *info);
extern bool QxCatalogLookupTaskByOid(Oid taskoid, QxCatalogTaskInfo *info);
extern bool QxCatalogLookupAttemptByOid(Oid attemptoid,
										QxCatalogAttemptInfo *info);
extern bool QxCatalogLookupCheckpointByOid(Oid checkpointoid,
										   QxCatalogCheckpointInfo *info);
extern List *QxCatalogBuildTaskInfoList(Oid databaseoid, Oid ownerid);
extern List *QxCatalogBuildAttemptInfoList(Oid databaseoid, Oid ownerid);
extern List *QxCatalogBuildCheckpointInfoList(Oid databaseoid, Oid ownerid);

extern bool QxCatalogLookupLatestSchedulerQueue(Oid dboid, Oid taskoid,
												Oid attemptoid,
												QxSchedulerQueueSnapshot *snapshot);
extern bool QxCatalogLookupLatestSchedulerLease(Oid dboid, Oid taskoid,
												Oid attemptoid,
												QxSchedulerLeaseSnapshot *snapshot,
												Oid *queueoid);
extern int16 QxCatalogMaxStepSeqnoForTask(Oid dboid, Oid taskoid);
extern void QxCatalogFreeSchedulerQueueSnapshot(QxSchedulerQueueSnapshot *snapshot);
extern void QxCatalogFreeSchedulerLeaseSnapshot(QxSchedulerLeaseSnapshot *snapshot);

#endif							/* QX_CATALOG_H */
