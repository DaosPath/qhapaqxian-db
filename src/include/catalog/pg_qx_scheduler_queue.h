/*-------------------------------------------------------------------------
 *
 * pg_qx_scheduler_queue.h
 *	  definition of the "scheduler queue ledger" system catalog
 *	  (pg_qx_scheduler_queue)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_scheduler_queue.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_SCHEDULER_QUEUE_H
#define PG_QX_SCHEDULER_QUEUE_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_scheduler_queue_d.h"
#include "utils/timestamp.h"

#define timestamptz TimestampTz

/* ----------------
 *		pg_qx_scheduler_queue definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_scheduler_queue
 * ----------------
 */
CATALOG(pg_qx_scheduler_queue,9600,QxSchedulerQueueRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxqueueledgerdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxqueueledgerowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	Oid			qxqueueledgernamespaceid BKI_LOOKUP(pg_qx_namespace);	/* namespace policy */
	Oid			qxqueueledgeragentid BKI_LOOKUP(pg_qx_agent);	/* related agent */
	Oid			qxqueueledgersessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxqueueledgertaskid BKI_LOOKUP(pg_qx_task);	/* related task */
	Oid			qxqueueledgerattemptid BKI_LOOKUP(pg_qx_attempt);	/* related attempt */
	int16		qxqueueledgerkind;	/* see QxSchedulerQueueKind */
	int32		qxqueueledgerrunnablecount;
	int32		qxqueueledgerleasedcount;
	int32		qxqueueledgerblockedcount;
	int32		qxqueueledgerretrycount;
	timestamptz qxqueueledgerenqueuedat;
	timestamptz qxqueueledgereligibleat;
	timestamptz qxqueueledgerupdatedat;

#ifdef CATALOG_VARLEN
	text		qxqueueledgername BKI_FORCE_NULL;
	text		qxqueueledgeragentname BKI_FORCE_NULL;
	text		qxqueueledgeridentityname BKI_FORCE_NULL;
	text		qxqueueledgernamespacepolicyname BKI_FORCE_NULL;
	text		qxqueueledgerpriority BKI_FORCE_NULL;
	text		qxqueueledgerprincipalname BKI_FORCE_NULL;
	text		qxqueueledgerprovidername BKI_FORCE_NULL;
	text		qxqueueledgerproviderkind BKI_FORCE_NULL;
	text		qxqueueledgerprincipalruntime BKI_FORCE_NULL;
#endif
} FormData_pg_qx_scheduler_queue;

typedef FormData_pg_qx_scheduler_queue *Form_pg_qx_scheduler_queue;

DECLARE_TOAST(pg_qx_scheduler_queue, 9602, 9603);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_scheduler_queue_oid_index, 9604, QxSchedulerQueueOidIndexId, pg_qx_scheduler_queue, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_scheduler_queue_taskid_index, 9605, QxSchedulerQueueTaskIdIndexId, pg_qx_scheduler_queue, btree(qxqueueledgertaskid oid_ops, qxqueueledgerattemptid oid_ops, oid oid_ops));

#undef timestamptz

#endif							/* PG_QX_SCHEDULER_QUEUE_H */
