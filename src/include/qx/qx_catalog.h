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
#include "qx/qx_semantic_log.h"

struct RelationData;
typedef struct RelationData *Relation;

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

typedef struct QxCatalogTaskInsertParams
{
	Oid			sessionoid;
	Oid			agentoid;
	Oid			identityoid;
	Oid			namespace_policy_oid;
	Oid			ownerid;
	const char *task_name;
	const char *goal;
	Node	   *input;
	const char *priority;
	List	   *authorized_tools;
	const char *submit_contract;
	const char *resume_contract;
	int32		budget_tokens;
	int32		budget_cost;
	int32		authorized_tool_tokens;
	int32		authorized_tool_cost;
	int32		estimated_tokens;
	int32		estimated_cost;
} QxCatalogTaskInsertParams;

typedef struct QxCatalogIdentityInsertParams
{
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			authrole;
	const char *name;
	const char *policy_name;
	List	   *budget_options;
} QxCatalogIdentityInsertParams;

typedef struct QxCatalogAgentInsertParams
{
	const char *name;
	Oid			namespaceoid;
	Oid			namespacepolicyoid;
	Oid			identityoid;
	Oid			ownerid;
	const char *identity_name;
	const char *model_uri;
	const char *memory_profile;
	const char *policy_name;
	List	   *tools;
	List	   *budget_options;
} QxCatalogAgentInsertParams;

typedef struct QxCatalogSessionInsertParams
{
	Oid			agentoid;
	Oid			namespacepolicyoid;
	Oid			identityoid;
	Oid			ownerid;
	char		status;
	const char *context_serialized;
} QxCatalogSessionInsertParams;

typedef struct QxCatalogNamespacePolicyInsertParams
{
	const char *name;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			authrole;
	bool		require_known_tools;
	bool		enforce_budgets;
	const char *policy_contract;
	List	   *allowed_tools;
} QxCatalogNamespacePolicyInsertParams;

typedef struct QxCatalogProviderInsertParams
{
	const char *name;
	Oid			namespaceoid;
	Oid			ownerid;
	bool		enabled;
	bool		attestation_required;
	const char *kind;
	const char *endpoint;
	const char *receipt_alg;
	const char *receipt_key;
	const char *attestation_profile;
	const char *attestation_version;
	const char *attestation_policy;
} QxCatalogProviderInsertParams;

typedef struct QxCatalogPrincipalInsertParams
{
	const char *name;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			provideroid;
	bool		enabled;
	const char *sandbox_name;
	const char *program_name;
	const char *provider_name;
	const char *runtime_class;
	const char *receipt_signer;
	const char *attestation_profile;
	const char *attestation_version;
	const char *attestation_policy;
} QxCatalogPrincipalInsertParams;

typedef struct QxCatalogToolInsertParams
{
	const char *name;
	Oid			namespaceoid;
	Oid			ownerid;
	Oid			principaloid;
	bool		enabled;
	int32		token_cost;
	int32		cost_units;
	const char *handler_name;
	const char *sandbox_name;
	const char *principal_name;
	const char *policy_name;
} QxCatalogToolInsertParams;

typedef struct QxCatalogMemoryInsertParams
{
	Oid			agentoid;
	Oid			sessionoid;
	Oid			ownerid;
	char		scope;
	const char *memory_key;
	const char *serialized_value;
	const char *serialized_tags;
} QxCatalogMemoryInsertParams;

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
extern void QxCatalogFreeProviderInfoList(List *providers);
extern void QxCatalogFreePrincipalInfoList(List *principals);
extern void QxCatalogFreeStringList(List *strings);

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
extern List *QxCatalogBuildProviderInfoList(Oid namespaceoid, Oid ownerid);
extern List *QxCatalogBuildPrincipalInfoList(Oid namespaceoid, Oid ownerid);
extern List *QxCatalogBuildDistinctRuntimeClassList(void);

extern bool QxCatalogLookupLatestSchedulerQueue(Oid dboid, Oid ownerid,
												Oid taskoid, Oid attemptoid,
												QxSchedulerQueueSnapshot *snapshot);
extern bool QxCatalogLookupLatestSchedulerLease(Oid dboid, Oid ownerid,
												Oid taskoid, Oid attemptoid,
												QxSchedulerLeaseSnapshot *snapshot,
												Oid *queueoid);
extern int16 QxCatalogMaxStepSeqnoForTask(Oid dboid, Oid taskoid);
extern void QxCatalogFreeSchedulerQueueSnapshot(QxSchedulerQueueSnapshot *snapshot);
extern void QxCatalogFreeSchedulerLeaseSnapshot(QxSchedulerLeaseSnapshot *snapshot);

extern void QxCatalogUpdateTaskRuntime(Relation taskrel, Oid taskoid, char state,
									   Oid lastattemptid, bool replace_attempt,
									   Oid lastcheckpointid, bool replace_checkpoint);
extern void QxCatalogUpdateAttemptState(Relation attemptrel, Oid attemptoid,
										char state);
extern void QxCatalogChargeTaskBudget(Relation taskrel, Oid taskoid,
									  int32 token_delta, int32 cost_delta,
									  const char *charge_name);

extern Oid QxCatalogInsertAttempt(Relation rel, Oid sessionoid, Oid taskoid,
								  Oid ownerid, Oid resumecheckpointid, int16 seqno,
								  char state, const char *strategy);
extern Oid QxCatalogInsertStep(Relation rel, Oid sessionoid, Oid taskoid,
								 int16 seqno, const char *name, const char *detail);
extern Oid QxCatalogInsertEvent(Relation rel, Oid sessionoid, Oid taskoid,
								  Oid stepoid, Oid ownerid,
								  const QxSemanticExecutionMetadata *metadata,
								  const char *kind, const char *payload);
extern Oid QxCatalogInsertTrace(Relation rel, Oid sessionoid, Oid taskoid,
								 Oid stepoid, Oid ownerid,
								 const QxSemanticExecutionMetadata *metadata,
								 char trace_state,
								 const char *name, const char *detail);
extern Oid QxCatalogInsertCheckpoint(Relation rel, Oid sessionoid, Oid taskoid,
									   Oid attemptoid, Oid stepoid, Oid ownerid,
									   const QxSemanticExecutionMetadata *metadata,
									   char taskstate, int16 nextstepseqno,
									   const char *label, const char *data);
extern Oid QxCatalogInsertSchedulerQueue(Relation rel,
										 const QxSchedulerQueueSnapshot *snapshot);
extern Oid QxCatalogInsertSchedulerLease(Relation rel, Oid queueoid,
										 const QxSchedulerLeaseSnapshot *snapshot);
extern Oid QxCatalogInsertSchedulerHeartbeat(Relation rel, Oid leaseoid,
											 const QxSchedulerHeartbeatSnapshot *snapshot);
extern Oid QxCatalogInsertTask(Relation taskrel,
							   const QxCatalogTaskInsertParams *params);
extern Oid QxCatalogInsertIdentity(Relation rel,
								   const QxCatalogIdentityInsertParams *params);
extern Oid QxCatalogInsertAgent(Relation rel,
								const QxCatalogAgentInsertParams *params);
extern Oid QxCatalogInsertSession(Relation rel,
								  const QxCatalogSessionInsertParams *params);
extern Oid QxCatalogInsertMemory(Relation memoryrel,
								 const QxCatalogMemoryInsertParams *params);
extern Oid QxCatalogInsertNamespacePolicy(Relation rel,
										  const QxCatalogNamespacePolicyInsertParams *params);
extern Oid QxCatalogInsertProvider(Relation rel,
								   const QxCatalogProviderInsertParams *params);
extern Oid QxCatalogInsertPrincipal(Relation rel,
									const QxCatalogPrincipalInsertParams *params);
extern Oid QxCatalogInsertTool(Relation rel,
							   const QxCatalogToolInsertParams *params);

#endif							/* QX_CATALOG_H */
