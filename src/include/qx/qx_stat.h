/*-------------------------------------------------------------------------
 *
 * qx_stat.h
 *	  shared-memory statistics collector for QhapaqXian Engine
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_STAT_H
#define QX_STAT_H

#include "nodes/nodes.h"
#include "utils/timestamp.h"

#include "qx/qx_recovery.h"

#define QX_STAT_RUNTIME_CLASS_LEN	64

typedef enum QxStatKind
{
	QX_STAT_PROVIDER,
	QX_STAT_PRINCIPAL,
	QX_STAT_RUNTIME_CLASS,
	QX_STAT_SCHEDULER_ACTIVITY,
	QX_STAT_RECOVERY
} QxStatKind;

typedef struct QxStatCounters
{
	int64		submit_count;
	int64		resume_count;
	int64		checkpoint_count;
	int64		verified_receipts;
	int64		rejected_receipts;
	int64		renew_count;
	int64		reclaim_count;
	int64		release_count;
	int64		retry_dispatch_count;
	int64		dead_letter_count;
	int64		recovery_startup_scans;
	int64		recovery_failover_rebuilds;
	int64		recovery_tasks_requeued;
	int64		recovery_attempts_fenced;
	int64		recovery_tasks_requeue_suppressed;
	int64		recovery_attempts_fence_suppressed;
} QxStatCounters;

extern bool qhapaqxian_track_stats;

extern Size QxStatShmemSize(void);
extern void QxStatShmemInit(void);
extern void QxStatReportProviderExecution(Oid dboid, Oid provideroid,
										  bool resume, bool receipt_verified);
extern void QxStatReportPrincipalExecution(Oid dboid, Oid principaloid,
										   bool resume, bool checkpointed,
										   bool receipt_verified);
extern void QxStatReportRuntimeClassExecution(Oid dboid,
											  const char *runtime_class,
											  bool resume, bool checkpointed,
											  bool receipt_verified);
extern void QxStatReportSchedulerEvent(Oid dboid, const char *event_name);
extern void QxStatReportRecoveryScan(Oid dboid, const QxRecoveryReport *report,
									 bool failover_rebuild);
extern void QxStatFlushPending(void);
extern void QxStatReset(QxStatKind kind);
extern void QxStatResetAll(void);
extern void QxStatReportTrace(const char *trace_name, const char *trace_detail);

#endif							/* QX_STAT_H */