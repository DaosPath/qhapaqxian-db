/*-------------------------------------------------------------------------
 *
 * pg_qx_scheduler_heartbeat.h
 *	  definition of the "scheduler heartbeat ledger" system catalog
 *	  (pg_qx_scheduler_heartbeat)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_scheduler_heartbeat.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_SCHEDULER_HEARTBEAT_H
#define PG_QX_SCHEDULER_HEARTBEAT_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_scheduler_heartbeat_d.h"
#include "utils/timestamp.h"

#define timestamptz TimestampTz

/* ----------------
 *		pg_qx_scheduler_heartbeat definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_scheduler_heartbeat
 * ----------------
 */
CATALOG(pg_qx_scheduler_heartbeat,9616,QxSchedulerHeartbeatRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxheartbeatledgerdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxheartbeatledgerowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	Oid			qxheartbeatledgerleaseid BKI_LOOKUP(pg_qx_scheduler_lease);	/* parent lease snapshot */
	Oid			qxheartbeatledgerworkerid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_agent);	/* worker if known */
	Oid			qxheartbeatledgersessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxheartbeatledgertaskid BKI_LOOKUP(pg_qx_task);	/* related task */
	Oid			qxheartbeatledgerattemptid BKI_LOOKUP(pg_qx_attempt);	/* related attempt */
	int16		qxheartbeatledgerstate;	/* see QxSchedulerHeartbeatState */
	int32		qxheartbeatledgerlagms;
	timestamptz qxheartbeatledgerobservedat;
	timestamptz qxheartbeatledgerexpectedbefore;
	bool		qxheartbeatledgerstale;

#ifdef CATALOG_VARLEN
	text		qxheartbeatledgerworkername BKI_FORCE_NULL;
	text		qxheartbeatledgerqueuename BKI_FORCE_NULL;
	text		qxheartbeatledgerprincipalruntime BKI_FORCE_NULL;
	text		qxheartbeatledgerproviderkind BKI_FORCE_NULL;
	text		qxheartbeatledgerreceiptmode BKI_FORCE_NULL;
#endif
} FormData_pg_qx_scheduler_heartbeat;

typedef FormData_pg_qx_scheduler_heartbeat *Form_pg_qx_scheduler_heartbeat;

DECLARE_TOAST(pg_qx_scheduler_heartbeat, 9618, 9619);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_scheduler_heartbeat_oid_index, 9620, QxSchedulerHeartbeatOidIndexId, pg_qx_scheduler_heartbeat, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_scheduler_heartbeat_taskid_index, 9621, QxSchedulerHeartbeatTaskIdIndexId, pg_qx_scheduler_heartbeat, btree(qxheartbeatledgertaskid oid_ops, qxheartbeatledgerattemptid oid_ops, oid oid_ops));
DECLARE_INDEX(pg_qx_scheduler_heartbeat_leaseid_index, 9622, QxSchedulerHeartbeatLeaseIdIndexId, pg_qx_scheduler_heartbeat, btree(qxheartbeatledgerleaseid oid_ops, oid oid_ops));

#undef timestamptz

#endif							/* PG_QX_SCHEDULER_HEARTBEAT_H */
