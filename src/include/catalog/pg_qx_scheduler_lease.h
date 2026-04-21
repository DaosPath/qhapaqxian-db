/*-------------------------------------------------------------------------
 *
 * pg_qx_scheduler_lease.h
 *	  definition of the "scheduler lease ledger" system catalog
 *	  (pg_qx_scheduler_lease)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_scheduler_lease.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_SCHEDULER_LEASE_H
#define PG_QX_SCHEDULER_LEASE_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_scheduler_lease_d.h"
#include "utils/timestamp.h"

#define timestamptz TimestampTz

/* ----------------
 *		pg_qx_scheduler_lease definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_scheduler_lease
 * ----------------
 */
CATALOG(pg_qx_scheduler_lease,9608,QxSchedulerLeaseRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxleaseledgerdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxleaseledgerowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	Oid			qxleaseledgerqueueid BKI_LOOKUP(pg_qx_scheduler_queue);	/* parent queue snapshot */
	Oid			qxleaseledgerworkerid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_agent);	/* worker if known */
	Oid			qxleaseledgersessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxleaseledgertaskid BKI_LOOKUP(pg_qx_task);	/* related task */
	Oid			qxleaseledgerattemptid BKI_LOOKUP(pg_qx_attempt);	/* related attempt */
	int16		qxleaseledgerstate;	/* see QxSchedulerLeaseState */
	timestamptz qxleaseledgeracquiredat;
	timestamptz qxleaseledgerrenewedat;
	timestamptz qxleaseledgerexpiresat;
	timestamptz qxleaseledgerlastheartbeatat;
	int32		qxleaseledgerrenewalcount;
	bool		qxleaseledgerneedsrecovery;

#ifdef CATALOG_VARLEN
	text		qxleaseledgerqueuename BKI_FORCE_NULL;
	text		qxleaseledgerworkername BKI_FORCE_NULL;
	text		qxleaseledgerleasetoken BKI_FORCE_NULL;
	text		qxleaseledgerprincipalname BKI_FORCE_NULL;
	text		qxleaseledgerprovidername BKI_FORCE_NULL;
	text		qxleaseledgerproviderkind BKI_FORCE_NULL;
	text		qxleaseledgerprincipalruntime BKI_FORCE_NULL;
#endif
} FormData_pg_qx_scheduler_lease;

typedef FormData_pg_qx_scheduler_lease *Form_pg_qx_scheduler_lease;

DECLARE_TOAST(pg_qx_scheduler_lease, 9610, 9611);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_scheduler_lease_oid_index, 9612, QxSchedulerLeaseOidIndexId, pg_qx_scheduler_lease, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_scheduler_lease_taskid_index, 9613, QxSchedulerLeaseTaskIdIndexId, pg_qx_scheduler_lease, btree(qxleaseledgertaskid oid_ops, qxleaseledgerattemptid oid_ops, oid oid_ops));
DECLARE_INDEX(pg_qx_scheduler_lease_queueid_index, 9614, QxSchedulerLeaseQueueIdIndexId, pg_qx_scheduler_lease, btree(qxleaseledgerqueueid oid_ops, oid oid_ops));

#undef timestamptz

#endif							/* PG_QX_SCHEDULER_LEASE_H */
