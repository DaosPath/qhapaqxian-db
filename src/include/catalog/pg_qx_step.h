/*-------------------------------------------------------------------------
 *
 * pg_qx_step.h
 *	  definition of the "agent step" system catalog (pg_qx_step)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_step.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_STEP_H
#define PG_QX_STEP_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_step_d.h"

#define QX_STEP_STATE_PENDING 'p'
#define QX_STEP_STATE_COMPLETED 'c'

/* ----------------
 *		pg_qx_step definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_step
 * ----------------
 */
CATALOG(pg_qx_step,9538,QxStepRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxstepdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxstepsessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxsteptaskid BKI_LOOKUP(pg_qx_task);	/* parent task */
	int16		qxstepseqno;	/* stable sequence inside task */
	char		qxstepstate;	/* see QX_STEP_STATE_* */

#ifdef CATALOG_VARLEN
	text		qxstepname BKI_FORCE_NULL;	/* symbolic step name */
	text		qxstepdetail BKI_FORCE_NULL;	/* operator-readable detail */
#endif
} FormData_pg_qx_step;

typedef FormData_pg_qx_step *Form_pg_qx_step;

DECLARE_TOAST(pg_qx_step, 9540, 9541);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_step_oid_index, 9542, QxStepOidIndexId, pg_qx_step, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_step_taskid_seqno_index, 9543, QxStepTaskIdSeqnoIndexId, pg_qx_step, btree(qxsteptaskid oid_ops, qxstepseqno int2_ops));

#endif							/* PG_QX_STEP_H */
