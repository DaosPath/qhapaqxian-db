/*-------------------------------------------------------------------------
 *
 * pg_qx_attempt.h
 *	  definition of the "agent attempt" system catalog (pg_qx_attempt)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_attempt.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_ATTEMPT_H
#define PG_QX_ATTEMPT_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_attempt_d.h"

#define QX_ATTEMPT_STATE_RUNNING 'r'
#define QX_ATTEMPT_STATE_CHECKPOINTED 'k'
#define QX_ATTEMPT_STATE_COMPLETED 'c'
#define QX_ATTEMPT_STATE_FAILED 'f'

/* ----------------
 *		pg_qx_attempt definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_attempt
 * ----------------
 */
CATALOG(pg_qx_attempt,9562,QxAttemptRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxattemptdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxattemptsessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxattempttaskid BKI_LOOKUP(pg_qx_task);	/* parent task */
	Oid			qxattemptowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	Oid			qxattemptresumecheckpointid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_checkpoint);	/* checkpoint we resume from */
	int16		qxattemptseqno;	/* attempt ordinal per task */
	char		qxattemptstate;	/* see QX_ATTEMPT_STATE_* */

#ifdef CATALOG_VARLEN
	text		qxattemptstrategy BKI_FORCE_NULL;	/* initial or resume */
#endif
} FormData_pg_qx_attempt;

typedef FormData_pg_qx_attempt *Form_pg_qx_attempt;

DECLARE_TOAST(pg_qx_attempt, 9564, 9565);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_attempt_oid_index, 9566, QxAttemptOidIndexId, pg_qx_attempt, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_attempt_taskid_seqno_index, 9567, QxAttemptTaskIdSeqnoIndexId, pg_qx_attempt, btree(qxattempttaskid oid_ops, qxattemptseqno int2_ops));

MAKE_SYSCACHE(QXATTEMPTOID, pg_qx_attempt_oid_index, 8);

#endif							/* PG_QX_ATTEMPT_H */
