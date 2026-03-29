/*-------------------------------------------------------------------------
 *
 * pg_qx_event.h
 *	  definition of the "agent event" system catalog (pg_qx_event)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_event.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_EVENT_H
#define PG_QX_EVENT_H

#include "access/xlogdefs.h"
#include "catalog/genbki.h"
#include "catalog/pg_qx_event_d.h"

/* ----------------
 *		pg_qx_event definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_event
 * ----------------
 */
CATALOG(pg_qx_event,9544,QxEventRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxeventdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxeventsessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxeventtaskid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_task);	/* parent task if any */
	Oid			qxeventstepid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_step);	/* related step if any */
	Oid			qxeventowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	XLogRecPtr	qxeventlsn;		/* semantic logical message LSN */

#ifdef CATALOG_VARLEN
	text		qxeventkind BKI_FORCE_NULL;	/* event type label */
	text		qxeventpayload BKI_FORCE_NULL;	/* human/debug payload */
#endif
} FormData_pg_qx_event;

typedef FormData_pg_qx_event *Form_pg_qx_event;

DECLARE_TOAST(pg_qx_event, 9546, 9547);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_event_oid_index, 9548, QxEventOidIndexId, pg_qx_event, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_event_taskid_index, 9549, QxEventTaskIdIndexId, pg_qx_event, btree(qxeventtaskid oid_ops));

#endif							/* PG_QX_EVENT_H */
