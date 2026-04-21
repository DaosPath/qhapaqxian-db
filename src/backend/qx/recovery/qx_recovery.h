/*-------------------------------------------------------------------------
 *
 * qx_recovery.h
 *	  internal recovery helpers for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/recovery/qx_recovery.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_BACKEND_RECOVERY_H
#define QX_BACKEND_RECOVERY_H

#include "qx/qx_recovery.h"

extern List *QxRecoveryScanTaskSummaries(const QxRecoveryStartupRequest *request);
extern List *QxRecoveryScanAttemptSummaries(const QxRecoveryStartupRequest *request);
extern List *QxRecoveryScanCheckpointSummaries(const QxRecoveryStartupRequest *request);

extern void QxRecoveryPopulateReport(QxRecoveryReport *report,
									 const QxRecoveryStartupRequest *startup_request,
									 const QxRecoveryFailoverRequest *failover_request,
									 List *task_summaries,
									 List *attempt_summaries,
									 List *checkpoint_summaries);

#endif							/* QX_BACKEND_RECOVERY_H */
