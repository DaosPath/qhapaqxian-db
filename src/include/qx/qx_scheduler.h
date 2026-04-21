/*-------------------------------------------------------------------------
 *
 * qx_scheduler.h
 *	  scheduler data model and API boundaries for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_scheduler.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_SCHEDULER_H
#define QX_SCHEDULER_H

#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "qx/qx_agent_plan.h"
#include "utils/timestamp.h"

typedef enum QxSchedulerQueueKind
{
	QX_SCHEDULER_QUEUE_PRIMARY,
	QX_SCHEDULER_QUEUE_RETRY,
	QX_SCHEDULER_QUEUE_RECOVERY,
	QX_SCHEDULER_QUEUE_MAINTENANCE
} QxSchedulerQueueKind;

typedef enum QxSchedulerLeaseState
{
	QX_SCHEDULER_LEASE_AVAILABLE,
	QX_SCHEDULER_LEASE_HELD,
	QX_SCHEDULER_LEASE_EXPIRED,
	QX_SCHEDULER_LEASE_RELEASED,
	QX_SCHEDULER_LEASE_RECLAIMED
} QxSchedulerLeaseState;

typedef enum QxSchedulerHeartbeatState
{
	QX_SCHEDULER_HEARTBEAT_HEALTHY,
	QX_SCHEDULER_HEARTBEAT_SLOW,
	QX_SCHEDULER_HEARTBEAT_STALE,
	QX_SCHEDULER_HEARTBEAT_MISSED
} QxSchedulerHeartbeatState;

typedef struct QxSchedulerTaskEnvelope
{
	Oid			ownerid;
	Oid			sessionoid;
	Oid			agentoid;
	Oid			identityoid;
	Oid			namespace_policy_oid;
	Oid			taskoid;
	Oid			attemptoid;
	char	   *queue_name;
	char	   *task_name;
	char	   *agent_name;
	char	   *identity_name;
	char	   *namespace_policy_name;
	char	   *principal_name;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *principal_runtime;
	char	   *priority;
	char	   *checkpoint_label;
	char	   *resume_label;
	bool		resumable;
	bool		checkpointable;
	bool		urgent;
	bool		requires_heartbeat;
	bool		is_retry;
	int32		budget_tokens;
	int32		budget_cost;
	int32		estimated_tokens;
	int32		estimated_cost;
	int32		retry_count;
	int32		max_retries;
	int32		heartbeat_interval_ms;
	int32		lease_ttl_ms;
} QxSchedulerTaskEnvelope;

typedef struct QxSchedulerQueueSnapshot
{
	Oid			queueoid;
	Oid			ownerid;
	Oid			namespace_policy_oid;
	Oid			agentoid;
	Oid			sessionoid;
	Oid			taskoid;
	Oid			attemptoid;
	QxSchedulerQueueKind queue_kind;
	char	   *queue_name;
	char	   *agent_name;
	char	   *identity_name;
	char	   *namespace_policy_name;
	char	   *priority;
	char	   *principal_name;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *principal_runtime;
	int32		runnable_count;
	int32		leased_count;
	int32		blocked_count;
	int32		retry_count;
	TimestampTz	enqueued_at;
	TimestampTz	eligible_at;
	TimestampTz	updated_at;
} QxSchedulerQueueSnapshot;

typedef struct QxSchedulerLeaseSnapshot
{
	Oid			leaseoid;
	Oid			queueoid;
	Oid			ownerid;
	Oid			workeroid;
	Oid			sessionoid;
	Oid			taskoid;
	Oid			attemptoid;
	QxSchedulerLeaseState state;
	char	   *queue_name;
	char	   *worker_name;
	char	   *lease_token;
	char	   *principal_name;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *principal_runtime;
	TimestampTz	acquired_at;
	TimestampTz	renewed_at;
	TimestampTz	expires_at;
	TimestampTz	last_heartbeat_at;
	int32		renewal_count;
	bool		needs_recovery;
} QxSchedulerLeaseSnapshot;

typedef struct QxSchedulerHeartbeatSnapshot
{
	Oid			heartbeatoid;
	Oid			leaseoid;
	Oid			ownerid;
	Oid			workeroid;
	Oid			sessionoid;
	Oid			taskoid;
	Oid			attemptoid;
	QxSchedulerHeartbeatState state;
	char	   *worker_name;
	char	   *queue_name;
	char	   *principal_runtime;
	char	   *provider_kind;
	char	   *receipt_mode;
	int32		lag_ms;
	TimestampTz	observed_at;
	TimestampTz	expected_before;
	bool		stale;
} QxSchedulerHeartbeatSnapshot;

extern void QxSchedulerInitTaskEnvelope(QxSchedulerTaskEnvelope *envelope);
extern QxSchedulerTaskEnvelope *QxSchedulerEnvelopeFromAgentPlan(const QxAgentPlan *plan);
extern QxSchedulerTaskEnvelope *QxSchedulerCopyTaskEnvelope(const QxSchedulerTaskEnvelope *envelope);
extern int32 QxSchedulerPriorityWeight(const char *priority);
extern int32 QxSchedulerDefaultHeartbeatIntervalMs(const QxSchedulerTaskEnvelope *envelope);
extern int32 QxSchedulerDefaultLeaseTtlMs(const QxSchedulerTaskEnvelope *envelope);
extern bool QxSchedulerRuntimeContractIsValid(const QxSchedulerTaskEnvelope *envelope);
extern bool QxSchedulerEnvelopeNeedsRetry(const QxSchedulerTaskEnvelope *envelope);
extern QxSchedulerQueueSnapshot *QxSchedulerQueueSnapshotFromEnvelope(const QxSchedulerTaskEnvelope *envelope);
extern QxSchedulerLeaseSnapshot *QxSchedulerLeaseSnapshotFromEnvelope(const QxSchedulerTaskEnvelope *envelope,
																	 Oid workeroid,
																	 const char *worker_name);
extern QxSchedulerLeaseSnapshot *QxSchedulerRenewLeaseSnapshot(const QxSchedulerLeaseSnapshot *lease,
															   int32 lease_ttl_ms);
extern QxSchedulerLeaseSnapshot *QxSchedulerReleaseLeaseSnapshot(const QxSchedulerLeaseSnapshot *lease,
																 bool needs_recovery);
extern QxSchedulerHeartbeatSnapshot *QxSchedulerHeartbeatSnapshotFromLease(const QxSchedulerLeaseSnapshot *lease);
extern QxSchedulerHeartbeatSnapshot *QxSchedulerFinalizeHeartbeatSnapshot(const QxSchedulerLeaseSnapshot *lease,
																		  const char *receipt_mode);
extern bool QxSchedulerLeaseExpired(const QxSchedulerLeaseSnapshot *lease,
									TimestampTz now);
extern bool QxSchedulerHeartbeatStale(const QxSchedulerHeartbeatSnapshot *heartbeat,
									  TimestampTz now);
extern char *QxSchedulerBuildQueueKey(const char *namespace_policy_name,
									  const char *priority,
									  const char *agent_name);
extern char *QxSchedulerDescribeTaskEnvelope(const QxSchedulerTaskEnvelope *envelope);
extern char *QxSchedulerDescribeQueueSnapshot(const QxSchedulerQueueSnapshot *snapshot);
extern char *QxSchedulerDescribeLeaseSnapshot(const QxSchedulerLeaseSnapshot *snapshot);
extern char *QxSchedulerDescribeHeartbeatSnapshot(const QxSchedulerHeartbeatSnapshot *snapshot);

#endif							/* QX_SCHEDULER_H */
