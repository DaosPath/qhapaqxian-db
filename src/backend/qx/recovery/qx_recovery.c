/*-------------------------------------------------------------------------
 *
 * qx_recovery.c
 *	  Stage 27 recovery scanner scaffold for QhapaqXian Engine
 *
 * The implementation deliberately reconstructs task/attempt/checkpoint
 * state from the durable pg_qx_* catalogs first. Runtime startup/failover
 * hooks can now consume the summaries to write scheduler evidence, while
 * bgworker supervision and replication replay remain separate integration
 * boundaries.
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/recovery/qx_recovery.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_qx_attempt.h"
#include "catalog/pg_qx_checkpoint.h"
#include "catalog/pg_qx_scheduler_lease.h"
#include "catalog/pg_qx_scheduler_queue.h"
#include "catalog/pg_qx_task.h"
#include "miscadmin.h"
#include "qx/qx_catalog.h"
#include "qx/qx_recovery.h"
#include "qx/qx_scheduler.h"

static void qx_recovery_init_report(QxRecoveryReport *report, Oid databaseoid,
									bool startup_scan, bool failover_rebuild);
static void QxRecoveryPopulateReport(QxRecoveryReport *report,
									 const QxRecoveryStartupRequest *startup_request,
									 const QxRecoveryFailoverRequest *failover_request,
									 List *task_summaries,
									 List *attempt_summaries,
									 List *checkpoint_summaries);
static char *qx_recovery_copy_string(const char *value);


static Oid
qx_recovery_effective_database_oid(const QxRecoveryStartupRequest *request)
{
	if (request != NULL && OidIsValid(request->databaseoid))
		return request->databaseoid;

	return MyDatabaseId;
}

static Oid
qx_recovery_effective_owner_oid(const QxRecoveryStartupRequest *request)
{
	if (request != NULL && OidIsValid(request->ownerid))
		return request->ownerid;

	return GetUserId();
}

static char *
qx_recovery_copy_string(const char *value)
{
	if (value == NULL)
		return NULL;

	return pstrdup(value);
}

static bool
qx_recovery_task_has_scheduler_recovery_queue(Oid databaseoid, Oid ownerid,
											  Oid taskoid, Oid attemptoid)
{
	QxSchedulerQueueSnapshot queue_snapshot;

	if (!OidIsValid(taskoid) || !OidIsValid(attemptoid))
		return false;

	if (!QxCatalogLookupLatestSchedulerQueue(databaseoid, ownerid, taskoid,
											 attemptoid, &queue_snapshot))
		return false;

	return queue_snapshot.queue_kind == QX_SCHEDULER_QUEUE_RECOVERY;
}

static bool
qx_recovery_attempt_has_scheduler_reclaim(Oid databaseoid, Oid ownerid,
										  Oid taskoid, Oid attemptoid)
{
	QxSchedulerLeaseSnapshot lease_snapshot;

	if (!OidIsValid(taskoid) || !OidIsValid(attemptoid))
		return false;

	if (!QxCatalogLookupLatestSchedulerLease(databaseoid, ownerid, taskoid,
											 attemptoid, &lease_snapshot,
											 NULL))
		return false;

	return lease_snapshot.state == QX_SCHEDULER_LEASE_RECLAIMED ||
		lease_snapshot.needs_recovery;
}

static QxRecoveryTaskSummary *
qx_recovery_task_summary_from_catalog(const QxCatalogTaskInfo *info)
{
	QxRecoveryTaskSummary *summary;

	Assert(info != NULL);

	summary = palloc0(sizeof(QxRecoveryTaskSummary));
	summary->taskoid = info->oid;
	summary->sessionoid = info->sessionoid;
	summary->agentoid = info->agentoid;
	summary->ownerid = info->ownerid;
	summary->namespace_policy_oid = info->namespacepolicyoid;
	summary->identityoid = info->identityoid;
	summary->lastattemptoid = info->lastattemptid;
	summary->lastcheckpointoid = info->lastcheckpointid;
	summary->attempt_count = 0;
	summary->checkpoint_count = 0;
	summary->nextstepseqno = 0;
	summary->taskstate = info->state;
	summary->needs_requeue = false;
	summary->needs_fence = false;
	summary->scheduler_recovery_queued = false;
	summary->scheduler_reclaimed = false;
	summary->has_semantic_lsn = false;
	summary->semantic_lsn = InvalidXLogRecPtr;
	summary->task_name = qx_recovery_copy_string(info->name);
	summary->goal = qx_recovery_copy_string(info->goal);
	summary->priority = qx_recovery_copy_string(info->priority);
	summary->recovery_reason = pstrdup("startup scan");

	return summary;
}

static QxRecoveryAttemptSummary *
qx_recovery_attempt_summary_from_catalog(const QxCatalogAttemptInfo *info)
{
	QxRecoveryAttemptSummary *summary;

	Assert(info != NULL);

	summary = palloc0(sizeof(QxRecoveryAttemptSummary));
	summary->attemptoid = info->oid;
	summary->taskoid = info->taskoid;
	summary->sessionoid = info->sessionoid;
	summary->ownerid = info->ownerid;
	summary->resume_checkpoint_oid = info->resumecheckpointid;
	summary->seqno = info->seqno;
	summary->state = info->state;
	summary->resumable = (info->state != QX_ATTEMPT_STATE_COMPLETED &&
						  info->state != QX_ATTEMPT_STATE_FAILED);
	summary->scheduler_reclaimed = false;
	summary->strategy = qx_recovery_copy_string(info->strategy);
	summary->recovery_reason = pstrdup("attempt scan");

	return summary;
}

static QxRecoveryCheckpointSummary *
qx_recovery_checkpoint_summary_from_catalog(const QxCatalogCheckpointInfo *info)
{
	QxRecoveryCheckpointSummary *summary;

	Assert(info != NULL);

	summary = palloc0(sizeof(QxRecoveryCheckpointSummary));
	summary->checkpointoid = info->oid;
	summary->taskoid = info->taskoid;
	summary->attemptoid = info->attemptoid;
	summary->stepoid = info->stepoid;
	summary->ownerid = info->ownerid;
	summary->state = info->state;
	summary->task_state = info->task_state;
	summary->nextstepseqno = info->next_step_seqno;
	summary->has_semantic_lsn = (info->lsn != InvalidXLogRecPtr);
	summary->semantic_lsn = info->lsn;
	summary->label = qx_recovery_copy_string(info->label);
	summary->data = qx_recovery_copy_string(info->data);

	return summary;
}

static void
qx_recovery_update_task_with_attempts(QxRecoveryTaskSummary *task,
									  List *attempts)
{
	ListCell   *lc;

	Assert(task != NULL);

	foreach(lc, attempts)
	{
		QxCatalogAttemptInfo *attempt = lfirst(lc);

		if (attempt->taskoid != task->taskoid)
			continue;

		task->attempt_count++;
		if (!OidIsValid(task->lastattemptoid) ||
			attempt->seqno >= task->nextstepseqno)
		{
			task->lastattemptoid = attempt->oid;
			task->nextstepseqno = attempt->seqno;
			task->needs_fence = (attempt->state == QX_ATTEMPT_STATE_RUNNING);
		}
		if (attempt->state == QX_ATTEMPT_STATE_CHECKPOINTED)
			task->needs_requeue = true;
	}
}

static void
qx_recovery_update_task_with_checkpoints(QxRecoveryTaskSummary *task,
										 List *checkpoints)
{
	ListCell   *lc;

	Assert(task != NULL);

	foreach(lc, checkpoints)
	{
		QxCatalogCheckpointInfo *checkpoint = lfirst(lc);

		if (checkpoint->taskoid != task->taskoid)
			continue;

		task->checkpoint_count++;
		task->lastcheckpointoid = checkpoint->oid;
		task->nextstepseqno = checkpoint->next_step_seqno;
		task->has_semantic_lsn = (checkpoint->lsn != InvalidXLogRecPtr);
		task->semantic_lsn = checkpoint->lsn;
		if (checkpoint->task_state == QX_TASK_STATE_CHECKPOINTED)
			task->needs_requeue = true;
	}
}

List *
QxRecoveryBuildTaskSummaries(const QxRecoveryStartupRequest *request)
{
	List	   *tasks;
	List	   *attempts;
	List	   *checkpoints;
	List	   *summaries = NIL;
	ListCell   *lc;
	Oid			databaseoid;
	Oid			ownerid;

	databaseoid = qx_recovery_effective_database_oid(request);
	ownerid = qx_recovery_effective_owner_oid(request);
	tasks = QxCatalogBuildTaskInfoList(databaseoid, ownerid);
	attempts = QxCatalogBuildAttemptInfoList(databaseoid, ownerid);
	checkpoints = QxCatalogBuildCheckpointInfoList(databaseoid, ownerid);

	foreach(lc, tasks)
	{
		QxCatalogTaskInfo *task = lfirst(lc);
		QxRecoveryTaskSummary *summary;

		summary = qx_recovery_task_summary_from_catalog(task);
		qx_recovery_update_task_with_attempts(summary, attempts);
		qx_recovery_update_task_with_checkpoints(summary, checkpoints);
		summary->scheduler_recovery_queued =
			qx_recovery_task_has_scheduler_recovery_queue(databaseoid,
														  ownerid,
														  summary->taskoid,
														  summary->lastattemptoid);
		summary->scheduler_reclaimed =
			qx_recovery_attempt_has_scheduler_reclaim(databaseoid,
													  ownerid,
													  summary->taskoid,
													  summary->lastattemptoid);
		if (summary->scheduler_reclaimed)
			summary->needs_fence = false;
		summaries = lappend(summaries, summary);
	}

	QxCatalogFreeTaskInfoList(tasks);
	QxCatalogFreeAttemptInfoList(attempts);
	QxCatalogFreeCheckpointInfoList(checkpoints);

	return summaries;
}

List *
QxRecoveryBuildAttemptSummaries(const QxRecoveryStartupRequest *request)
{
	List	   *attempts;
	List	   *summaries = NIL;
	ListCell   *lc;
	Oid			databaseoid;
	Oid			ownerid;

	databaseoid = qx_recovery_effective_database_oid(request);
	ownerid = qx_recovery_effective_owner_oid(request);
	attempts = QxCatalogBuildAttemptInfoList(databaseoid, ownerid);

	foreach(lc, attempts)
	{
		QxCatalogAttemptInfo *attempt = lfirst(lc);
		QxRecoveryAttemptSummary *summary;

		summary = qx_recovery_attempt_summary_from_catalog(attempt);
		summary->scheduler_reclaimed =
			qx_recovery_attempt_has_scheduler_reclaim(databaseoid,
													  ownerid,
													  summary->taskoid,
													  summary->attemptoid);
		summaries = lappend(summaries, summary);
	}

	QxCatalogFreeAttemptInfoList(attempts);

	return summaries;
}

List *
QxRecoveryBuildCheckpointSummaries(const QxRecoveryStartupRequest *request)
{
	List	   *checkpoints;
	List	   *summaries = NIL;
	ListCell   *lc;
	Oid			databaseoid;
	Oid			ownerid;

	databaseoid = qx_recovery_effective_database_oid(request);
	ownerid = qx_recovery_effective_owner_oid(request);
	checkpoints = QxCatalogBuildCheckpointInfoList(databaseoid, ownerid);

	foreach(lc, checkpoints)
	{
		QxCatalogCheckpointInfo *checkpoint = lfirst(lc);

		summaries = lappend(summaries,
							qx_recovery_checkpoint_summary_from_catalog(checkpoint));
	}

	QxCatalogFreeCheckpointInfoList(checkpoints);

	return summaries;
}

bool
QxRecoveryTaskNeedsRequeue(const QxRecoveryTaskSummary *summary)
{
	if (summary == NULL)
		return false;

	if (summary->scheduler_recovery_queued)
		return false;

	if (summary->taskstate == QX_TASK_STATE_COMPLETED)
		return false;

	return summary->needs_requeue ||
		summary->taskstate == QX_TASK_STATE_CHECKPOINTED ||
		summary->taskstate == QX_TASK_STATE_RUNNING;
}

bool
QxRecoveryAttemptNeedsFence(const QxRecoveryAttemptSummary *summary)
{
	if (summary == NULL)
		return false;

	if (summary->scheduler_reclaimed)
		return false;

	return summary->state == QX_ATTEMPT_STATE_RUNNING;
}

bool
QxRecoveryCheckpointNeedsReplay(const QxRecoveryCheckpointSummary *summary)
{
	if (summary == NULL)
		return false;

	return summary->has_semantic_lsn &&
		summary->state == QX_CHECKPOINT_STATE_DURABLE;
}

static void
qx_recovery_init_report(QxRecoveryReport *report, Oid databaseoid,
						bool startup_scan, bool failover_rebuild)
{
	memset(report, 0, sizeof(QxRecoveryReport));
	report->databaseoid = databaseoid;
	report->startup_scan = startup_scan;
	report->failover_rebuild = failover_rebuild;
}

static void
QxRecoveryPopulateReport(QxRecoveryReport *report,
						 const QxRecoveryStartupRequest *startup_request,
						 const QxRecoveryFailoverRequest *failover_request,
						 List *task_summaries,
						 List *attempt_summaries,
						 List *checkpoint_summaries)
{
	ListCell   *lc;

	Assert(report != NULL);
	Assert((startup_request == NULL) != (failover_request == NULL));

	if (startup_request != NULL)
		qx_recovery_init_report(report,
								qx_recovery_effective_database_oid(startup_request),
								true,
								false);
	else
		qx_recovery_init_report(report,
								(OidIsValid(failover_request->databaseoid) ?
								 failover_request->databaseoid :
								 MyDatabaseId),
								false,
								true);

	foreach(lc, task_summaries)
	{
		QxRecoveryTaskSummary *task = lfirst(lc);

		report->tasks_scanned++;
		if (QxRecoveryTaskNeedsRequeue(task))
			report->tasks_requeued++;
		if (task->needs_fence)
			report->orphan_attempts++;
	}

	foreach(lc, attempt_summaries)
	{
		QxRecoveryAttemptSummary *attempt = lfirst(lc);

		report->attempts_scanned++;
		if (QxRecoveryAttemptNeedsFence(attempt))
			report->attempts_fenced++;
	}

	foreach(lc, checkpoint_summaries)
	{
		QxRecoveryCheckpointSummary *checkpoint = lfirst(lc);

		report->checkpoints_scanned++;
		if (QxRecoveryCheckpointNeedsReplay(checkpoint))
			report->checkpoints_replayed++;
		if (checkpoint->state == QX_CHECKPOINT_STATE_DURABLE)
			report->semantic_replay_candidates++;
	}
}

QxRecoveryReport *
QxRecoveryRunStartupScan(const QxRecoveryStartupRequest *request,
						 const QxRecoveryHooks *hooks)
{
	QxRecoveryReport *report;
	List	   *tasks;
	List	   *attempts = NIL;
	List	   *checkpoints = NIL;
	ListCell   *lc;

	if (request == NULL)
		elog(ERROR, "startup recovery request cannot be null");

	report = palloc0(sizeof(QxRecoveryReport));
	tasks = QxRecoveryBuildTaskSummaries(request);
	if (request->include_attempts)
		attempts = QxRecoveryBuildAttemptSummaries(request);
	if (request->include_checkpoints)
		checkpoints = QxRecoveryBuildCheckpointSummaries(request);

	QxRecoveryPopulateReport(report, request, NULL, tasks, attempts, checkpoints);

	if (hooks != NULL && hooks->begin != NULL)
		hooks->begin(report, hooks->userdata);

	foreach(lc, tasks)
	{
		QxRecoveryTaskSummary *task = lfirst(lc);

		if (hooks != NULL && hooks->task != NULL)
			hooks->task(task, hooks->userdata);
		if (request->requeue_checkpointed_tasks && QxRecoveryTaskNeedsRequeue(task) &&
			hooks != NULL && hooks->requeue_task != NULL)
			hooks->requeue_task(task, hooks->userdata);
	}

	foreach(lc, attempts)
	{
		QxRecoveryAttemptSummary *attempt = lfirst(lc);

		if (hooks != NULL && hooks->attempt != NULL)
			hooks->attempt(attempt, hooks->userdata);
		if (request->fence_stale_attempts && QxRecoveryAttemptNeedsFence(attempt) &&
			hooks != NULL && hooks->fence_attempt != NULL)
			hooks->fence_attempt(attempt, hooks->userdata);
	}

	foreach(lc, checkpoints)
	{
		QxRecoveryCheckpointSummary *checkpoint = lfirst(lc);

		if (hooks != NULL && hooks->checkpoint != NULL)
			hooks->checkpoint(checkpoint, hooks->userdata);
	}

	if (hooks != NULL && hooks->finish != NULL)
		hooks->finish(report, hooks->userdata);

	QxRecoveryFreeTaskSummaries(tasks);
	QxRecoveryFreeAttemptSummaries(attempts);
	QxRecoveryFreeCheckpointSummaries(checkpoints);

	return report;
}

QxRecoveryReport *
QxRecoveryRunFailoverRebuild(const QxRecoveryFailoverRequest *request,
							 const QxRecoveryHooks *hooks)
{
	QxRecoveryStartupRequest startup_request;
	QxRecoveryReport *report;
	List	   *tasks;
	List	   *attempts = NIL;
	List	   *checkpoints = NIL;
	ListCell   *lc;

	if (request == NULL)
		elog(ERROR, "failover recovery request cannot be null");

	startup_request.databaseoid = request->databaseoid;
	startup_request.ownerid = request->ownerid;
	startup_request.include_attempts = request->rebuild_attempt_graph;
	startup_request.include_checkpoints = request->rebuild_checkpoint_graph;
	startup_request.fence_stale_attempts = request->fence_orphaned_attempts;
	startup_request.requeue_checkpointed_tasks = request->requeue_checkpointed_tasks;
	startup_request.rebuild_from_semantic_log = request->replay_semantic_log;

	report = palloc0(sizeof(QxRecoveryReport));
	tasks = QxRecoveryBuildTaskSummaries(&startup_request);
	if (startup_request.include_attempts)
		attempts = QxRecoveryBuildAttemptSummaries(&startup_request);
	if (startup_request.include_checkpoints)
		checkpoints = QxRecoveryBuildCheckpointSummaries(&startup_request);

	QxRecoveryPopulateReport(report, NULL, request, tasks, attempts, checkpoints);

	if (hooks != NULL && hooks->begin != NULL)
		hooks->begin(report, hooks->userdata);

	foreach(lc, tasks)
	{
		QxRecoveryTaskSummary *task = lfirst(lc);

		if (hooks != NULL && hooks->task != NULL)
			hooks->task(task, hooks->userdata);
		if (request->requeue_checkpointed_tasks && QxRecoveryTaskNeedsRequeue(task) &&
			hooks != NULL && hooks->requeue_task != NULL)
			hooks->requeue_task(task, hooks->userdata);
	}

	foreach(lc, attempts)
	{
		QxRecoveryAttemptSummary *attempt = lfirst(lc);

		if (hooks != NULL && hooks->attempt != NULL)
			hooks->attempt(attempt, hooks->userdata);
		if (request->fence_orphaned_attempts && QxRecoveryAttemptNeedsFence(attempt) &&
			hooks != NULL && hooks->fence_attempt != NULL)
			hooks->fence_attempt(attempt, hooks->userdata);
	}

	foreach(lc, checkpoints)
	{
		QxRecoveryCheckpointSummary *checkpoint = lfirst(lc);

		if (hooks != NULL && hooks->checkpoint != NULL)
			hooks->checkpoint(checkpoint, hooks->userdata);
	}

	if (hooks != NULL && hooks->finish != NULL)
		hooks->finish(report, hooks->userdata);

	QxRecoveryFreeTaskSummaries(tasks);
	QxRecoveryFreeAttemptSummaries(attempts);
	QxRecoveryFreeCheckpointSummaries(checkpoints);

	return report;
}

void
QxRecoveryFreeTaskSummary(QxRecoveryTaskSummary *summary)
{
	if (summary == NULL)
		return;

	pfree(summary->task_name);
	pfree(summary->goal);
	pfree(summary->priority);
	pfree(summary->recovery_reason);
	pfree(summary);
}

void
QxRecoveryFreeAttemptSummary(QxRecoveryAttemptSummary *summary)
{
	if (summary == NULL)
		return;

	pfree(summary->strategy);
	pfree(summary->recovery_reason);
	pfree(summary);
}

void
QxRecoveryFreeCheckpointSummary(QxRecoveryCheckpointSummary *summary)
{
	if (summary == NULL)
		return;

	pfree(summary->label);
	pfree(summary->data);
	pfree(summary);
}

void
QxRecoveryFreeReport(QxRecoveryReport *report)
{
	if (report == NULL)
		return;

	pfree(report);
}

void
QxRecoveryFreeTaskSummaries(List *tasks)
{
	ListCell   *lc;

	foreach(lc, tasks)
		QxRecoveryFreeTaskSummary(lfirst(lc));

	list_free(tasks);
}

void
QxRecoveryFreeAttemptSummaries(List *attempts)
{
	ListCell   *lc;

	foreach(lc, attempts)
		QxRecoveryFreeAttemptSummary(lfirst(lc));

	list_free(attempts);
}

void
QxRecoveryFreeCheckpointSummaries(List *checkpoints)
{
	ListCell   *lc;

	foreach(lc, checkpoints)
		QxRecoveryFreeCheckpointSummary(lfirst(lc));

	list_free(checkpoints);
}
