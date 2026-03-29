/*-------------------------------------------------------------------------
 *
 * pg_qx_checkpoint.h
 *	  definition of the "agent checkpoint" system catalog (pg_qx_checkpoint)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_checkpoint.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_CHECKPOINT_H
#define PG_QX_CHECKPOINT_H

#include "access/xlogdefs.h"
#include "catalog/genbki.h"
#include "catalog/pg_qx_checkpoint_d.h"

#define QX_CHECKPOINT_STATE_DURABLE 'd'

/* ----------------
 *		pg_qx_checkpoint definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_checkpoint
 * ----------------
 */
CATALOG(pg_qx_checkpoint,9556,QxCheckpointRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxcheckpointdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxcheckpointsessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxcheckpointtaskid BKI_LOOKUP(pg_qx_task);	/* parent task */
	Oid			qxcheckpointattemptid BKI_LOOKUP(pg_qx_attempt);	/* attempt that wrote it */
	Oid			qxcheckpointstepid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_step);	/* checkpointing step if any */
	Oid			qxcheckpointowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	char		qxcheckpointstate;	/* see QX_CHECKPOINT_STATE_* */
	char		qxcheckpointtaskstate;	/* task state captured in checkpoint */
	int16		qxcheckpointnextstepseqno;	/* next step when resuming */
	XLogRecPtr	qxcheckpointlsn;	/* semantic logical message LSN */

#ifdef CATALOG_VARLEN
	text		qxcheckpointlabel BKI_FORCE_NULL;	/* checkpoint label */
	text		qxcheckpointdata BKI_FORCE_NULL;	/* serialized resume state */
#endif
} FormData_pg_qx_checkpoint;

typedef FormData_pg_qx_checkpoint *Form_pg_qx_checkpoint;

DECLARE_TOAST(pg_qx_checkpoint, 9558, 9559);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_checkpoint_oid_index, 9560, QxCheckpointOidIndexId, pg_qx_checkpoint, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_checkpoint_taskid_index, 9561, QxCheckpointTaskIdIndexId, pg_qx_checkpoint, btree(qxcheckpointtaskid oid_ops));

MAKE_SYSCACHE(QXCHECKPOINTOID, pg_qx_checkpoint_oid_index, 8);

#endif							/* PG_QX_CHECKPOINT_H */
