/*-------------------------------------------------------------------------
 *
 * qx_recovery.h
 *	  public recovery API for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/include/qx/qx_recovery.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_RECOVERY_H
#define QX_RECOVERY_H

#include "access/xlogdefs.h"
#include "nodes/nodes.h"

typedef struct QxRecoveryStartupRequest
{
	Oid			databaseoid;
	Oid			ownerid;
	bool		include_attempts;
	bool		include_checkpoints;
	bool		fence_stale_attempts;
	bool		requeue_checkpointed_tasks;
	bool		rebuild_from_semantic_log;
} QxRecoveryStartupRequest;

typedef struct QxRecoveryFailoverRequest
{
	Oid			databaseoid;
	Oid			ownerid;
	bool		rebuild_task_graph;
	bool		rebuild_attempt_graph;
	bool		rebuild_checkpoint_graph;
	bool		replay_semantic_log;
	bool		fence_orphaned_attempts;
	bool		requeue_checkpointed_tasks;
} QxRecoveryFailoverRequest;

typedef struct QxRecoveryTaskSummary
{
	Oid			taskoid;
	Oid			sessionoid;
	Oid			agentoid;
	Oid			ownerid;
	Oid			namespace_policy_oid;
	Oid			identityoid;
	Oid			lastattemptoid;
	Oid			lastcheckpointoid;
	int32		attempt_count;
	int32		checkpoint_count;
	int16		nextstepseqno;
	char		taskstate;
	bool		needs_requeue;
	bool		needs_fence;
	bool		scheduler_recovery_queued;
	bool		scheduler_reclaimed;
	bool		has_semantic_lsn;
	XLogRecPtr	semantic_lsn;
	char	   *task_name;
	char	   *goal;
	char	   *priority;
	char	   *recovery_reason;
} QxRecoveryTaskSummary;

typedef struct QxRecoveryAttemptSummary
{
	Oid			attemptoid;
	Oid			taskoid;
	Oid			sessionoid;
	Oid			ownerid;
	Oid			resume_checkpoint_oid;
	int16		seqno;
	char		state;
	bool		resumable;
	bool		scheduler_reclaimed;
	char	   *strategy;
	char	   *recovery_reason;
} QxRecoveryAttemptSummary;

typedef struct QxRecoveryCheckpointSummary
{
	Oid			checkpointoid;
	Oid			taskoid;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			ownerid;
	char		state;
	char		task_state;
	int16		nextstepseqno;
	bool		has_semantic_lsn;
	XLogRecPtr	semantic_lsn;
	char	   *label;
	char	   *data;
} QxRecoveryCheckpointSummary;

typedef struct QxRecoveryReport
{
	Oid			databaseoid;
	bool		startup_scan;
	bool		failover_rebuild;
	int32		tasks_scanned;
	int32		attempts_scanned;
	int32		checkpoints_scanned;
	int32		tasks_requeued;
	int32		attempts_fenced;
	int32		checkpoints_replayed;
	int32		orphan_attempts;
	int32		semantic_replay_candidates;
	int32		tasks_requeue_suppressed;
	int32		attempts_fence_suppressed;
} QxRecoveryReport;

typedef struct QxRecoveryHooks
{
	void		(*begin)(const QxRecoveryReport *report, void *userdata);
	void		(*task)(const QxRecoveryTaskSummary *summary, void *userdata);
	void		(*attempt)(const QxRecoveryAttemptSummary *summary, void *userdata);
	void		(*checkpoint)(const QxRecoveryCheckpointSummary *summary, void *userdata);
	void		(*fence_attempt)(const QxRecoveryAttemptSummary *summary, void *userdata);
	void		(*requeue_task)(const QxRecoveryTaskSummary *summary, void *userdata);
	void		(*finish)(const QxRecoveryReport *report, void *userdata);
	void	   *userdata;
} QxRecoveryHooks;

extern QxRecoveryReport *QxRecoveryRunStartupScan(const QxRecoveryStartupRequest *request,
												  const QxRecoveryHooks *hooks);
extern QxRecoveryReport *QxRecoveryRunFailoverRebuild(const QxRecoveryFailoverRequest *request,
													  const QxRecoveryHooks *hooks);

extern List *QxRecoveryBuildTaskSummaries(const QxRecoveryStartupRequest *request);
extern List *QxRecoveryBuildAttemptSummaries(const QxRecoveryStartupRequest *request);
extern List *QxRecoveryBuildCheckpointSummaries(const QxRecoveryStartupRequest *request);

extern bool QxRecoveryTaskNeedsRequeue(const QxRecoveryTaskSummary *summary);
extern bool QxRecoveryAttemptNeedsFence(const QxRecoveryAttemptSummary *summary);
extern bool QxRecoveryCheckpointNeedsReplay(const QxRecoveryCheckpointSummary *summary);

extern void QxRecoveryFreeTaskSummary(QxRecoveryTaskSummary *summary);
extern void QxRecoveryFreeAttemptSummary(QxRecoveryAttemptSummary *summary);
extern void QxRecoveryFreeCheckpointSummary(QxRecoveryCheckpointSummary *summary);
extern void QxRecoveryFreeReport(QxRecoveryReport *report);
extern void QxRecoveryFreeTaskSummaries(List *tasks);
extern void QxRecoveryFreeAttemptSummaries(List *attempts);
extern void QxRecoveryFreeCheckpointSummaries(List *checkpoints);

#endif							/* QX_RECOVERY_H */
