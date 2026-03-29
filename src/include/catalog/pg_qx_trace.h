/*-------------------------------------------------------------------------
 *
 * pg_qx_trace.h
 *	  definition of the "agent trace" system catalog (pg_qx_trace)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_trace.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_TRACE_H
#define PG_QX_TRACE_H

#include "access/xlogdefs.h"
#include "catalog/genbki.h"
#include "catalog/pg_qx_trace_d.h"

#define QX_TRACE_STATE_OPEN 'o'
#define QX_TRACE_STATE_CLOSED 'c'

/* ----------------
 *		pg_qx_trace definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_trace
 * ----------------
 */
CATALOG(pg_qx_trace,9550,QxTraceRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxtracedbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxtracesessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxtracetaskid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_task);	/* parent task if any */
	Oid			qxtracestepid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_step);	/* related step if any */
	Oid			qxtraceowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	char		qxtracestate;	/* see QX_TRACE_STATE_* */
	XLogRecPtr	qxtracelsn;		/* semantic logical message LSN */

#ifdef CATALOG_VARLEN
	text		qxtracename BKI_FORCE_NULL;	/* symbolic trace name */
	text		qxtracedetail BKI_FORCE_NULL;	/* operator-readable detail */
#endif
} FormData_pg_qx_trace;

typedef FormData_pg_qx_trace *Form_pg_qx_trace;

DECLARE_TOAST(pg_qx_trace, 9552, 9553);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_trace_oid_index, 9554, QxTraceOidIndexId, pg_qx_trace, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_trace_taskid_index, 9555, QxTraceTaskIdIndexId, pg_qx_trace, btree(qxtracetaskid oid_ops));

#endif							/* PG_QX_TRACE_H */
