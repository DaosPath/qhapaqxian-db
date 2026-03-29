/*-------------------------------------------------------------------------
 *
 * pg_qx_memory.h
 *	  definition of the "agent memory" system catalog (pg_qx_memory)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_memory.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_MEMORY_H
#define PG_QX_MEMORY_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_memory_d.h"

#define QX_MEMORY_SCOPE_WORKING 'w'
#define QX_MEMORY_SCOPE_EPISODIC 'e'
#define QX_MEMORY_SCOPE_SEMANTIC 's'

/* ----------------
 *		pg_qx_memory definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_memory
 * ----------------
 */
CATALOG(pg_qx_memory,9568,QxMemoryRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxmemorydbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxmemoryagentid BKI_LOOKUP(pg_qx_agent);	/* owning agent */
	Oid			qxmemorysessionid BKI_LOOKUP(pg_qx_session);	/* owning session */
	Oid			qxmemorytaskid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_task);	/* related task if any */
	Oid			qxmemoryowner BKI_LOOKUP(pg_authid);	/* issuer owner */
	char		qxmemoryscope;	/* see QX_MEMORY_SCOPE_* */

#ifdef CATALOG_VARLEN
	text		qxmemorykey BKI_FORCE_NULL;	/* stable retrieval key */
	text		qxmemoryvalue BKI_FORCE_NULL;	/* serialized payload */
	text		qxmemorytags BKI_FORCE_NULL;	/* comma-separated tags */
#endif
} FormData_pg_qx_memory;

typedef FormData_pg_qx_memory *Form_pg_qx_memory;

DECLARE_TOAST(pg_qx_memory, 9570, 9571);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_memory_oid_index, 9572, QxMemoryOidIndexId, pg_qx_memory, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_memory_agent_scope_index, 9573, QxMemoryAgentScopeIndexId, pg_qx_memory, btree(qxmemoryagentid oid_ops, qxmemoryscope char_ops, oid oid_ops));
DECLARE_INDEX(pg_qx_memory_sessionid_index, 9574, QxMemorySessionIdIndexId, pg_qx_memory, btree(qxmemorysessionid oid_ops));

#endif							/* PG_QX_MEMORY_H */
