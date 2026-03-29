/*-------------------------------------------------------------------------
 *
 * pg_qx_session.h
 *	  definition of the "agent session" system catalog (pg_qx_session)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_session.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_SESSION_H
#define PG_QX_SESSION_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_session_d.h"

#define QX_SESSION_STATUS_ACTIVE 'a'

/* ----------------
 *		pg_qx_session definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_session
 * ----------------
 */
CATALOG(pg_qx_session,9526,QxSessionRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxsessiondbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxsessionagentid BKI_LOOKUP(pg_qx_agent);	/* owning agent */
	Oid			qxsessionowner BKI_LOOKUP(pg_authid);	/* starter */
	char		qxsessionstatus;	/* see QX_SESSION_STATUS_* */

#ifdef CATALOG_VARLEN
	pg_node_tree qxcontext BKI_FORCE_NULL;	/* serialized raw context expr */
#endif
} FormData_pg_qx_session;

typedef FormData_pg_qx_session *Form_pg_qx_session;

DECLARE_TOAST(pg_qx_session, 9528, 9529);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_session_oid_index, 9530, QxSessionOidIndexId, pg_qx_session, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_session_agentid_index, 9531, QxSessionAgentIdIndexId, pg_qx_session, btree(qxsessionagentid oid_ops));

MAKE_SYSCACHE(QXSESSIONOID, pg_qx_session_oid_index, 8);

#endif							/* PG_QX_SESSION_H */
