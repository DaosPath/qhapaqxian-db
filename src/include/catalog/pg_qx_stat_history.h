/*-------------------------------------------------------------------------
 *
 * pg_qx_stat_history.h
 *	  definition of the "QX stat history" system catalog (pg_qx_stat_history)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_stat_history.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_STAT_HISTORY_H
#define PG_QX_STAT_HISTORY_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_stat_history_d.h"
#include "utils/timestamp.h"

#define timestamptz TimestampTz

/* ----------------
 *		pg_qx_stat_history definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_stat_history
 * ----------------
 */
CATALOG(pg_qx_stat_history,9624,QxStatHistoryRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxstathistorydbid BKI_LOOKUP(pg_database);	/* database */
	timestamptz qxstathistorycaptured;	/* snapshot timestamp */
	Oid			qxstathistoryentityoid BKI_DEFAULT(0);	/* entity oid when known */
	int32		qxstathistorysubmitcount BKI_DEFAULT(0);
	int32		qxstathistoryresumecount BKI_DEFAULT(0);
	int32		qxstathistoryverifiedreceipts BKI_DEFAULT(0);
	int32		qxstathistoryrejectedreceipts BKI_DEFAULT(0);
	int32		qxstathistorycheckpointcount BKI_DEFAULT(0);
	int32		qxstathistoryrenewcount BKI_DEFAULT(0);
	int32		qxstathistoryreclaimcount BKI_DEFAULT(0);
	int32		qxstathistoryreleasecount BKI_DEFAULT(0);
	int32		qxstathistoryretrydispatchcount BKI_DEFAULT(0);
	int32		qxstathistorydeadlettercount BKI_DEFAULT(0);
	int32		qxstathistorystartupscancount BKI_DEFAULT(0);
	int32		qxstathistoryfailoverrebuildcount BKI_DEFAULT(0);
	int32		qxstathistorytasksrequeued BKI_DEFAULT(0);
	int32		qxstathistoryattemptsfenced BKI_DEFAULT(0);

#ifdef CATALOG_VARLEN
	text		qxstathistoryscope BKI_FORCE_NULL;	/* provider, principal, ... */
	text		qxstathistoryentity BKI_FORCE_NULL; /* entity label when known */
#endif
} FormData_pg_qx_stat_history;

typedef FormData_pg_qx_stat_history *Form_pg_qx_stat_history;

DECLARE_TOAST(pg_qx_stat_history, 9627, 9628);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_stat_history_oid_index, 9625, QxStatHistoryOidIndexId, pg_qx_stat_history, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_stat_history_captured_index, 9626, QxStatHistoryCapturedIndexId, pg_qx_stat_history, btree(qxstathistorydbid oid_ops, qxstathistorycaptured timestamptz_ops));

#undef timestamptz

#endif							/* PG_QX_STAT_HISTORY_H */