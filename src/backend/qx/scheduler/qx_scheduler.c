/*-------------------------------------------------------------------------
 *
 * qx_scheduler.c
 *	  scheduler scaffolding for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/scheduler/qx_scheduler.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "qx_scheduler.h"

#ifndef USECS_PER_MSEC
#define USECS_PER_MSEC 1000
#endif

static const char *qx_scheduler_safe_text(const char *value);
static QxSchedulerQueueKind qx_scheduler_queue_kind_for_priority(const char *priority);
static int32 qx_scheduler_priority_weight_internal(const char *priority);
static int32 qx_scheduler_default_heartbeat_interval_ms_internal(
	const QxSchedulerTaskEnvelope *envelope);
static int32 qx_scheduler_default_lease_ttl_ms_internal(
	const QxSchedulerTaskEnvelope *envelope);
static TimestampTz qx_scheduler_heartbeat_deadline(const QxSchedulerLeaseSnapshot *lease);

void
QxSchedulerInitTaskEnvelope(QxSchedulerTaskEnvelope *envelope)
{
	if (envelope == NULL)
		return;

	MemSet(envelope, 0, sizeof(QxSchedulerTaskEnvelope));
}

QxSchedulerTaskEnvelope *
QxSchedulerCopyTaskEnvelope(const QxSchedulerTaskEnvelope *envelope)
{
	QxSchedulerTaskEnvelope *copy;

	if (envelope == NULL)
		return NULL;

	copy = palloc0(sizeof(QxSchedulerTaskEnvelope));
	*copy = *envelope;
	copy->queue_name = pstrdup(qx_scheduler_safe_text(envelope->queue_name));
	copy->task_name = pstrdup(qx_scheduler_safe_text(envelope->task_name));
	copy->agent_name = pstrdup(qx_scheduler_safe_text(envelope->agent_name));
	copy->identity_name = pstrdup(qx_scheduler_safe_text(envelope->identity_name));
	copy->namespace_policy_name = pstrdup(qx_scheduler_safe_text(envelope->namespace_policy_name));
	copy->principal_name = pstrdup(qx_scheduler_safe_text(envelope->principal_name));
	copy->provider_name = pstrdup(qx_scheduler_safe_text(envelope->provider_name));
	copy->provider_kind = pstrdup(qx_scheduler_safe_text(envelope->provider_kind));
	copy->principal_runtime = pstrdup(qx_scheduler_safe_text(envelope->principal_runtime));
	copy->priority = pstrdup(qx_scheduler_safe_text(envelope->priority));
	copy->checkpoint_label = pstrdup(qx_scheduler_safe_text(envelope->checkpoint_label));
	copy->resume_label = pstrdup(qx_scheduler_safe_text(envelope->resume_label));
	return copy;
}

QxSchedulerTaskEnvelope *
QxSchedulerEnvelopeFromAgentPlan(const QxAgentPlan *plan)
{
	QxSchedulerTaskEnvelope *envelope;

	if (plan == NULL)
		return NULL;

	envelope = palloc0(sizeof(QxSchedulerTaskEnvelope));
	envelope->ownerid = plan->ownerid;
	envelope->sessionoid = plan->sessionoid;
	envelope->agentoid = plan->agentoid;
	envelope->identityoid = plan->identityoid;
	envelope->namespace_policy_oid = plan->namespace_policy_oid;
	envelope->taskoid = plan->taskoid;
	envelope->attemptoid = 0;
	envelope->queue_name = QxSchedulerBuildQueueKey(plan->namespace_policy_name,
													plan->priority,
													plan->identity_name);
	envelope->task_name = pstrdup(qx_scheduler_safe_text(plan->task_name));
	envelope->agent_name = pstrdup(qx_scheduler_safe_text(plan->identity_name));
	envelope->identity_name = pstrdup(qx_scheduler_safe_text(plan->identity_name));
	envelope->namespace_policy_name = pstrdup(qx_scheduler_safe_text(plan->namespace_policy_name));
	envelope->principal_name = NULL;
	envelope->provider_name = NULL;
	envelope->provider_kind = NULL;
	envelope->principal_runtime = NULL;
	envelope->priority = pstrdup(qx_scheduler_safe_text(plan->priority));
	envelope->checkpoint_label = pstrdup(qx_scheduler_safe_text(plan->checkpoint_label));
	envelope->resume_label = NULL;
	envelope->resumable = plan->resumable;
	envelope->checkpointable = plan->checkpointable;
	envelope->urgent = (qx_scheduler_priority_weight_internal(plan->priority) >= 80);
	envelope->requires_heartbeat = true;
	envelope->is_retry = false;
	envelope->budget_tokens = plan->budget_tokens;
	envelope->budget_cost = plan->budget_cost;
	envelope->estimated_tokens = plan->estimated_tokens;
	envelope->estimated_cost = plan->estimated_total_cost;
	envelope->retry_count = 0;
	envelope->max_retries = 3;
	envelope->heartbeat_interval_ms = qx_scheduler_default_heartbeat_interval_ms_internal(envelope);
	envelope->lease_ttl_ms = qx_scheduler_default_lease_ttl_ms_internal(envelope);
	return envelope;
}

int32
QxSchedulerPriorityWeight(const char *priority)
{
	return qx_scheduler_priority_weight_internal(priority);
}

int32
QxSchedulerDefaultHeartbeatIntervalMs(const QxSchedulerTaskEnvelope *envelope)
{
	return qx_scheduler_default_heartbeat_interval_ms_internal(envelope);
}

int32
QxSchedulerDefaultLeaseTtlMs(const QxSchedulerTaskEnvelope *envelope)
{
	return qx_scheduler_default_lease_ttl_ms_internal(envelope);
}

bool
QxSchedulerRuntimeContractIsValid(const QxSchedulerTaskEnvelope *envelope)
{
	if (envelope == NULL)
		return false;

	if (envelope->sessionoid == InvalidOid ||
		envelope->agentoid == InvalidOid ||
		envelope->identityoid == InvalidOid ||
		envelope->namespace_policy_oid == InvalidOid)
		return false;

	if (envelope->task_name == NULL || envelope->priority == NULL)
		return false;

	if (envelope->budget_tokens < 0 || envelope->budget_cost < 0)
		return false;

	if (envelope->estimated_tokens < 0 || envelope->estimated_cost < 0)
		return false;

	return true;
}

bool
QxSchedulerEnvelopeNeedsRetry(const QxSchedulerTaskEnvelope *envelope)
{
	if (envelope == NULL)
		return false;

	if (!envelope->resumable)
		return false;

	if (envelope->retry_count >= envelope->max_retries)
		return false;

	return true;
}

QxSchedulerQueueSnapshot *
QxSchedulerQueueSnapshotFromEnvelope(const QxSchedulerTaskEnvelope *envelope)
{
	QxSchedulerQueueSnapshot *snapshot;

	if (envelope == NULL)
		return NULL;

	snapshot = palloc0(sizeof(QxSchedulerQueueSnapshot));
	snapshot->queueoid = InvalidOid;
	snapshot->ownerid = envelope->ownerid;
	snapshot->namespace_policy_oid = envelope->namespace_policy_oid;
	snapshot->agentoid = envelope->agentoid;
	snapshot->sessionoid = envelope->sessionoid;
	snapshot->taskoid = envelope->taskoid;
	snapshot->attemptoid = envelope->attemptoid;
	snapshot->queue_kind = qx_scheduler_queue_kind_for_priority(envelope->priority);
	snapshot->queue_name = pstrdup(qx_scheduler_safe_text(envelope->queue_name));
	snapshot->agent_name = pstrdup(qx_scheduler_safe_text(envelope->agent_name));
	snapshot->identity_name = pstrdup(qx_scheduler_safe_text(envelope->identity_name));
	snapshot->namespace_policy_name = pstrdup(qx_scheduler_safe_text(envelope->namespace_policy_name));
	snapshot->priority = pstrdup(qx_scheduler_safe_text(envelope->priority));
	snapshot->principal_name = pstrdup(qx_scheduler_safe_text(envelope->principal_name));
	snapshot->provider_name = pstrdup(qx_scheduler_safe_text(envelope->provider_name));
	snapshot->provider_kind = pstrdup(qx_scheduler_safe_text(envelope->provider_kind));
	snapshot->principal_runtime = pstrdup(qx_scheduler_safe_text(envelope->principal_runtime));
	snapshot->runnable_count = 1;
	snapshot->leased_count = 0;
	snapshot->blocked_count = 0;
	snapshot->retry_count = envelope->retry_count;
	snapshot->enqueued_at = GetCurrentTimestamp();
	snapshot->eligible_at = snapshot->enqueued_at;
	snapshot->updated_at = snapshot->enqueued_at;
	return snapshot;
}

QxSchedulerLeaseSnapshot *
QxSchedulerLeaseSnapshotFromEnvelope(const QxSchedulerTaskEnvelope *envelope,
									 Oid workeroid,
									 const char *worker_name)
{
	QxSchedulerLeaseSnapshot *lease;
	TimestampTz now;

	if (envelope == NULL)
		return NULL;

	now = GetCurrentTimestamp();
	lease = palloc0(sizeof(QxSchedulerLeaseSnapshot));
	lease->leaseoid = InvalidOid;
	lease->queueoid = InvalidOid;
	lease->ownerid = envelope->ownerid;
	lease->workeroid = workeroid;
	lease->sessionoid = envelope->sessionoid;
	lease->taskoid = envelope->taskoid;
	lease->attemptoid = envelope->attemptoid;
	lease->state = QX_SCHEDULER_LEASE_HELD;
	lease->queue_name = pstrdup(qx_scheduler_safe_text(envelope->queue_name));
	lease->worker_name = pstrdup(qx_scheduler_safe_text(worker_name));
	lease->lease_token = psprintf("lease:%u:%u:%u",
								  envelope->sessionoid,
								  envelope->taskoid,
								  envelope->attemptoid);
	lease->principal_name = pstrdup(qx_scheduler_safe_text(envelope->principal_name));
	lease->provider_name = pstrdup(qx_scheduler_safe_text(envelope->provider_name));
	lease->provider_kind = pstrdup(qx_scheduler_safe_text(envelope->provider_kind));
	lease->principal_runtime = pstrdup(qx_scheduler_safe_text(envelope->principal_runtime));
	lease->acquired_at = now;
	lease->renewed_at = now;
	lease->expires_at = now + ((TimestampTz) envelope->lease_ttl_ms * USECS_PER_MSEC);
	lease->last_heartbeat_at = now;
	lease->renewal_count = 0;
	lease->needs_recovery = false;
	return lease;
}

QxSchedulerLeaseSnapshot *
QxSchedulerRenewLeaseSnapshot(const QxSchedulerLeaseSnapshot *lease,
							  int32 lease_ttl_ms)
{
	QxSchedulerLeaseSnapshot *renewed;
	TimestampTz now;

	if (lease == NULL)
		return NULL;

	now = GetCurrentTimestamp();
	renewed = palloc0(sizeof(QxSchedulerLeaseSnapshot));
	*renewed = *lease;
	renewed->leaseoid = InvalidOid;
	renewed->queue_name = pstrdup(qx_scheduler_safe_text(lease->queue_name));
	renewed->worker_name = pstrdup(qx_scheduler_safe_text(lease->worker_name));
	renewed->lease_token = pstrdup(qx_scheduler_safe_text(lease->lease_token));
	renewed->principal_name = pstrdup(qx_scheduler_safe_text(lease->principal_name));
	renewed->provider_name = pstrdup(qx_scheduler_safe_text(lease->provider_name));
	renewed->provider_kind = pstrdup(qx_scheduler_safe_text(lease->provider_kind));
	renewed->principal_runtime = pstrdup(qx_scheduler_safe_text(lease->principal_runtime));
	renewed->state = QX_SCHEDULER_LEASE_HELD;
	renewed->renewed_at = now;
	renewed->expires_at = now + ((TimestampTz) lease_ttl_ms * USECS_PER_MSEC);
	renewed->last_heartbeat_at = now;
	renewed->renewal_count = lease->renewal_count + 1;
	renewed->needs_recovery = false;
	return renewed;
}

QxSchedulerLeaseSnapshot *
QxSchedulerReleaseLeaseSnapshot(const QxSchedulerLeaseSnapshot *lease,
								bool needs_recovery)
{
	QxSchedulerLeaseSnapshot *released;
	TimestampTz now;

	if (lease == NULL)
		return NULL;

	now = GetCurrentTimestamp();
	released = palloc0(sizeof(QxSchedulerLeaseSnapshot));
	*released = *lease;
	released->leaseoid = InvalidOid;
	released->queue_name = pstrdup(qx_scheduler_safe_text(lease->queue_name));
	released->worker_name = pstrdup(qx_scheduler_safe_text(lease->worker_name));
	released->lease_token = pstrdup(qx_scheduler_safe_text(lease->lease_token));
	released->principal_name = pstrdup(qx_scheduler_safe_text(lease->principal_name));
	released->provider_name = pstrdup(qx_scheduler_safe_text(lease->provider_name));
	released->provider_kind = pstrdup(qx_scheduler_safe_text(lease->provider_kind));
	released->principal_runtime = pstrdup(qx_scheduler_safe_text(lease->principal_runtime));
	released->state = needs_recovery ? QX_SCHEDULER_LEASE_RECLAIMED :
		QX_SCHEDULER_LEASE_RELEASED;
	released->renewed_at = now;
	released->expires_at = now;
	released->last_heartbeat_at = now;
	released->needs_recovery = needs_recovery;
	return released;
}

QxSchedulerHeartbeatSnapshot *
QxSchedulerHeartbeatSnapshotFromLease(const QxSchedulerLeaseSnapshot *lease)
{
	QxSchedulerHeartbeatSnapshot *heartbeat;
	TimestampTz now;

	if (lease == NULL)
		return NULL;

	now = GetCurrentTimestamp();
	heartbeat = palloc0(sizeof(QxSchedulerHeartbeatSnapshot));
	heartbeat->heartbeatoid = InvalidOid;
	heartbeat->leaseoid = lease->leaseoid;
	heartbeat->ownerid = lease->ownerid;
	heartbeat->workeroid = lease->workeroid;
	heartbeat->sessionoid = lease->sessionoid;
	heartbeat->taskoid = lease->taskoid;
	heartbeat->attemptoid = lease->attemptoid;
	heartbeat->state = QX_SCHEDULER_HEARTBEAT_HEALTHY;
	heartbeat->worker_name = pstrdup(qx_scheduler_safe_text(lease->worker_name));
	heartbeat->queue_name = pstrdup(qx_scheduler_safe_text(lease->queue_name));
	heartbeat->principal_runtime = pstrdup(qx_scheduler_safe_text(lease->principal_runtime));
	heartbeat->provider_kind = pstrdup(qx_scheduler_safe_text(lease->provider_kind));
	heartbeat->receipt_mode = pstrdup("scheduler-ledger");
	heartbeat->lag_ms = 0;
	heartbeat->observed_at = now;
	heartbeat->expected_before = qx_scheduler_heartbeat_deadline(lease);
	heartbeat->stale = false;
	return heartbeat;
}

QxSchedulerHeartbeatSnapshot *
QxSchedulerFinalizeHeartbeatSnapshot(const QxSchedulerLeaseSnapshot *lease,
									 const char *receipt_mode)
{
	QxSchedulerHeartbeatSnapshot *heartbeat;
	TimestampTz now;

	if (lease == NULL)
		return NULL;

	now = GetCurrentTimestamp();
	heartbeat = QxSchedulerHeartbeatSnapshotFromLease(lease);
	heartbeat->leaseoid = lease->leaseoid;
	heartbeat->observed_at = now;
	heartbeat->expected_before = now;
	heartbeat->stale = false;
	if (heartbeat->receipt_mode != NULL)
		pfree(heartbeat->receipt_mode);
	heartbeat->receipt_mode = pstrdup(qx_scheduler_safe_text(receipt_mode));
	return heartbeat;
}

bool
QxSchedulerLeaseExpired(const QxSchedulerLeaseSnapshot *lease, TimestampTz now)
{
	if (lease == NULL)
		return false;

	return (lease->state == QX_SCHEDULER_LEASE_EXPIRED ||
			now > lease->expires_at);
}

bool
QxSchedulerHeartbeatStale(const QxSchedulerHeartbeatSnapshot *heartbeat,
						  TimestampTz now)
{
	if (heartbeat == NULL)
		return false;

	if (heartbeat->state == QX_SCHEDULER_HEARTBEAT_STALE ||
		heartbeat->state == QX_SCHEDULER_HEARTBEAT_MISSED)
		return true;

	return (now > heartbeat->expected_before);
}

char *
QxSchedulerBuildQueueKey(const char *namespace_policy_name,
						 const char *priority,
						 const char *agent_name)
{
	return psprintf("%s:%s:%s",
					qx_scheduler_safe_text(namespace_policy_name),
					qx_scheduler_safe_text(priority),
					qx_scheduler_safe_text(agent_name));
}

char *
QxSchedulerDescribeTaskEnvelope(const QxSchedulerTaskEnvelope *envelope)
{
	StringInfoData buf;

	if (envelope == NULL)
		return pstrdup("SchedulerEnvelope <null>");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "SchedulerEnvelope task=%s session=%u agent=%u identity=%u policy=%u queue=%s priority=%s runtime=%s provider=%s/%s budget=%d:%d estimated=%d:%d retry=%d/%d heartbeat_ms=%d lease_ttl_ms=%d%s%s%s%s",
					 qx_scheduler_safe_text(envelope->task_name),
					 (unsigned int) envelope->sessionoid,
					 (unsigned int) envelope->agentoid,
					 (unsigned int) envelope->identityoid,
					 (unsigned int) envelope->namespace_policy_oid,
					 qx_scheduler_safe_text(envelope->queue_name),
					 qx_scheduler_safe_text(envelope->priority),
					 qx_scheduler_safe_text(envelope->principal_runtime),
					 qx_scheduler_safe_text(envelope->provider_name),
					 qx_scheduler_safe_text(envelope->provider_kind),
					 envelope->budget_tokens,
					 envelope->budget_cost,
					 envelope->estimated_tokens,
					 envelope->estimated_cost,
					 envelope->retry_count,
					 envelope->max_retries,
					 envelope->heartbeat_interval_ms,
					 envelope->lease_ttl_ms,
					 envelope->resumable ? " resumable" : "",
					 envelope->checkpointable ? " checkpointable" : "",
					 envelope->urgent ? " urgent" : "",
					 envelope->requires_heartbeat ? " heartbeat" : "");
	return buf.data;
}

char *
QxSchedulerDescribeQueueSnapshot(const QxSchedulerQueueSnapshot *snapshot)
{
	StringInfoData buf;

	if (snapshot == NULL)
		return pstrdup("QueueSnapshot <null>");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "QueueSnapshot queue=%s kind=%d task=%u session=%u runnable=%d leased=%d blocked=%d retry=%d runtime=%s provider=%s/%s eligible_at=" INT64_FORMAT " updated_at=" INT64_FORMAT,
					 qx_scheduler_safe_text(snapshot->queue_name),
					 (int) snapshot->queue_kind,
					 (unsigned int) snapshot->taskoid,
					 (unsigned int) snapshot->sessionoid,
					 snapshot->runnable_count,
					 snapshot->leased_count,
					 snapshot->blocked_count,
					 snapshot->retry_count,
					 qx_scheduler_safe_text(snapshot->principal_runtime),
					 qx_scheduler_safe_text(snapshot->provider_name),
					 qx_scheduler_safe_text(snapshot->provider_kind),
					 (int64) snapshot->eligible_at,
					 (int64) snapshot->updated_at);
	return buf.data;
}

char *
QxSchedulerDescribeLeaseSnapshot(const QxSchedulerLeaseSnapshot *snapshot)
{
	StringInfoData buf;

	if (snapshot == NULL)
		return pstrdup("LeaseSnapshot <null>");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "LeaseSnapshot lease=%u queue=%s worker=%s task=%u state=%d runtime=%s provider=%s/%s renewals=%d recovery=%s expires_at=" INT64_FORMAT,
					 (unsigned int) snapshot->leaseoid,
					 qx_scheduler_safe_text(snapshot->queue_name),
					 qx_scheduler_safe_text(snapshot->worker_name),
					 (unsigned int) snapshot->taskoid,
					 (int) snapshot->state,
					 qx_scheduler_safe_text(snapshot->principal_runtime),
					 qx_scheduler_safe_text(snapshot->provider_name),
					 qx_scheduler_safe_text(snapshot->provider_kind),
					 snapshot->renewal_count,
					 snapshot->needs_recovery ? "true" : "false",
					 (int64) snapshot->expires_at);
	return buf.data;
}

char *
QxSchedulerDescribeHeartbeatSnapshot(const QxSchedulerHeartbeatSnapshot *snapshot)
{
	StringInfoData buf;

	if (snapshot == NULL)
		return pstrdup("HeartbeatSnapshot <null>");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "HeartbeatSnapshot heartbeat=%u lease=%u worker=%s task=%u state=%d lag_ms=%d runtime=%s provider_kind=%s receipt_mode=%s stale=%s observed_at=" INT64_FORMAT,
					 (unsigned int) snapshot->heartbeatoid,
					 (unsigned int) snapshot->leaseoid,
					 qx_scheduler_safe_text(snapshot->worker_name),
					 (unsigned int) snapshot->taskoid,
					 (int) snapshot->state,
					 snapshot->lag_ms,
					 qx_scheduler_safe_text(snapshot->principal_runtime),
					 qx_scheduler_safe_text(snapshot->provider_kind),
					 qx_scheduler_safe_text(snapshot->receipt_mode),
					 snapshot->stale ? "true" : "false",
					 (int64) snapshot->observed_at);
	return buf.data;
}

void
QxSchedulerLedgerInit(QxSchedulerLedger *ledger)
{
	if (ledger == NULL)
		return;

	ledger->queue_snapshots = NIL;
	ledger->lease_snapshots = NIL;
	ledger->heartbeat_snapshots = NIL;
}

void
QxSchedulerLedgerAddQueue(QxSchedulerLedger *ledger,
						  QxSchedulerQueueSnapshot *snapshot)
{
	if (ledger == NULL || snapshot == NULL)
		return;

	ledger->queue_snapshots = lappend(ledger->queue_snapshots, snapshot);
}

void
QxSchedulerLedgerAddLease(QxSchedulerLedger *ledger,
						  QxSchedulerLeaseSnapshot *snapshot)
{
	if (ledger == NULL || snapshot == NULL)
		return;

	ledger->lease_snapshots = lappend(ledger->lease_snapshots, snapshot);
}

void
QxSchedulerLedgerAddHeartbeat(QxSchedulerLedger *ledger,
							  QxSchedulerHeartbeatSnapshot *snapshot)
{
	if (ledger == NULL || snapshot == NULL)
		return;

	ledger->heartbeat_snapshots = lappend(ledger->heartbeat_snapshots, snapshot);
}

bool
QxSchedulerQueueRunnable(const QxSchedulerQueueSnapshot *snapshot)
{
	if (snapshot == NULL)
		return false;

	return (snapshot->runnable_count > 0 &&
			snapshot->blocked_count == 0 &&
			snapshot->queue_kind != QX_SCHEDULER_QUEUE_MAINTENANCE);
}

bool
QxSchedulerLeaseNeedsReclaim(const QxSchedulerLeaseSnapshot *lease,
							 TimestampTz now)
{
	if (lease == NULL)
		return false;

	if (lease->state == QX_SCHEDULER_LEASE_RELEASED)
		return false;

	return QxSchedulerLeaseExpired(lease, now) ||
		lease->needs_recovery;
}

bool
QxSchedulerHeartbeatNeedsAttention(const QxSchedulerHeartbeatSnapshot *heartbeat,
								   TimestampTz now)
{
	return QxSchedulerHeartbeatStale(heartbeat, now);
}

char *
QxSchedulerLedgerDescribe(const QxSchedulerLedger *ledger)
{
	StringInfoData buf;

	if (ledger == NULL)
		return pstrdup("SchedulerLedger <null>");

	initStringInfo(&buf);
	appendStringInfo(&buf,
					 "SchedulerLedger queues=%d leases=%d heartbeats=%d",
					 list_length(ledger->queue_snapshots),
					 list_length(ledger->lease_snapshots),
					 list_length(ledger->heartbeat_snapshots));
	if (ledger->queue_snapshots != NIL)
		appendStringInfo(&buf, " latest_queue=(%s)",
						 qx_scheduler_safe_text(
							 ((QxSchedulerQueueSnapshot *) llast(ledger->queue_snapshots))->queue_name));
	if (ledger->lease_snapshots != NIL)
		appendStringInfo(&buf, " latest_lease=(%s)",
						 qx_scheduler_safe_text(
							 ((QxSchedulerLeaseSnapshot *) llast(ledger->lease_snapshots))->worker_name));
	if (ledger->heartbeat_snapshots != NIL)
		appendStringInfo(&buf, " latest_heartbeat=(%s)",
						 qx_scheduler_safe_text(
							 ((QxSchedulerHeartbeatSnapshot *) llast(ledger->heartbeat_snapshots))->worker_name));
	return buf.data;
}

static const char *
qx_scheduler_safe_text(const char *value)
{
	return (value == NULL || value[0] == '\0') ? "<unset>" : value;
}

static QxSchedulerQueueKind
qx_scheduler_queue_kind_for_priority(const char *priority)
{
	int32		weight;

	weight = qx_scheduler_priority_weight_internal(priority);
	if (weight >= 80)
		return QX_SCHEDULER_QUEUE_PRIMARY;
	if (weight >= 50)
		return QX_SCHEDULER_QUEUE_RETRY;
	if (weight >= 25)
		return QX_SCHEDULER_QUEUE_RECOVERY;
	return QX_SCHEDULER_QUEUE_MAINTENANCE;
}

static int32
qx_scheduler_priority_weight_internal(const char *priority)
{
	if (priority == NULL || priority[0] == '\0')
		return 40;

	if (pg_strcasecmp(priority, "urgent") == 0 ||
		pg_strcasecmp(priority, "critical") == 0)
		return 100;
	if (pg_strcasecmp(priority, "high") == 0)
		return 80;
	if (pg_strcasecmp(priority, "normal") == 0)
		return 60;
	if (pg_strcasecmp(priority, "low") == 0)
		return 35;
	if (pg_strcasecmp(priority, "idle") == 0)
		return 10;

	return 50;
}

static int32
qx_scheduler_default_heartbeat_interval_ms_internal(
	const QxSchedulerTaskEnvelope *envelope)
{
	int32		weight;

	if (envelope == NULL)
		return 5000;

	weight = qx_scheduler_priority_weight_internal(envelope->priority);
	if (weight >= 80)
		return 1500;
	if (weight >= 50)
		return 3000;
	if (weight >= 25)
		return 5000;
	return 8000;
}

static int32
qx_scheduler_default_lease_ttl_ms_internal(const QxSchedulerTaskEnvelope *envelope)
{
	int32		heartbeat_ms;

	heartbeat_ms = qx_scheduler_default_heartbeat_interval_ms_internal(envelope);
	return heartbeat_ms * 3;
}

static TimestampTz
qx_scheduler_heartbeat_deadline(const QxSchedulerLeaseSnapshot *lease)
{
	if (lease == NULL)
		return 0;

	return lease->renewed_at + ((TimestampTz) 3000 * USECS_PER_MSEC);
}
