/*-------------------------------------------------------------------------
 *
 * qx_scheduler.h
 *	  scheduler backend scaffolding for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/scheduler/qx_scheduler.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_BACKEND_SCHEDULER_H
#define QX_BACKEND_SCHEDULER_H

#include "qx/qx_scheduler.h"

typedef struct QxSchedulerWorkerProfile
{
	Oid			workerid;
	char	   *worker_name;
	char	   *launch_mode;
	int32		max_leases;
	int32		active_leases;
	TimestampTz	last_heartbeat_at;
	bool		healthy;
} QxSchedulerWorkerProfile;

typedef struct QxSchedulerLedger
{
	List	   *queue_snapshots;	/* list of QxSchedulerQueueSnapshot */
	List	   *lease_snapshots;	/* list of QxSchedulerLeaseSnapshot */
	List	   *heartbeat_snapshots;	/* list of QxSchedulerHeartbeatSnapshot */
} QxSchedulerLedger;

extern void QxSchedulerLedgerInit(QxSchedulerLedger *ledger);
extern void QxSchedulerLedgerAddQueue(QxSchedulerLedger *ledger,
									  QxSchedulerQueueSnapshot *snapshot);
extern void QxSchedulerLedgerAddLease(QxSchedulerLedger *ledger,
									  QxSchedulerLeaseSnapshot *snapshot);
extern void QxSchedulerLedgerAddHeartbeat(QxSchedulerLedger *ledger,
										  QxSchedulerHeartbeatSnapshot *snapshot);
extern bool QxSchedulerQueueRunnable(const QxSchedulerQueueSnapshot *snapshot);
extern bool QxSchedulerLeaseNeedsReclaim(const QxSchedulerLeaseSnapshot *lease,
										 TimestampTz now);
extern bool QxSchedulerHeartbeatNeedsAttention(const QxSchedulerHeartbeatSnapshot *heartbeat,
											   TimestampTz now);
extern char *QxSchedulerLedgerDescribe(const QxSchedulerLedger *ledger);

#endif							/* QX_BACKEND_SCHEDULER_H */
