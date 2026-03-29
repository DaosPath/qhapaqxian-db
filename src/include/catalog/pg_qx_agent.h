/*-------------------------------------------------------------------------
 *
 * pg_qx_agent.h
 *	  definition of the "agent" system catalog (pg_qx_agent)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_agent.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_AGENT_H
#define PG_QX_AGENT_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_agent_d.h"

/* ----------------
 *		pg_qx_agent definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_agent
 * ----------------
 */
CATALOG(pg_qx_agent,9520,QxAgentRelationId)
{
	Oid			oid;			/* oid */
	NameData	qxagentname;	/* agent name */
	Oid			qxagentnamespace BKI_LOOKUP(pg_namespace);	/* schema */
	Oid			qxnamespacepolicyid BKI_LOOKUP(pg_qx_namespace);	/* namespace policy */
	Oid			qxidentityid BKI_LOOKUP(pg_qx_identity);	/* operational identity */
	Oid			qxagentowner BKI_LOOKUP(pg_authid);	/* owner */

#ifdef CATALOG_VARLEN
	text		qxidentity BKI_FORCE_NULL;	/* logical identity profile */
	text		qxmodeluri BKI_FORCE_NULL;	/* backing model URI */
	text		qxmemoryprofile BKI_FORCE_NULL;	/* memory mode label */
	text		qxpolicy BKI_FORCE_NULL;	/* policy label */
	pg_node_tree qxtools BKI_FORCE_NULL;	/* serialized List<String> */
	pg_node_tree qxbudget BKI_FORCE_NULL;	/* serialized List<DefElem> */
#endif
} FormData_pg_qx_agent;

typedef FormData_pg_qx_agent *Form_pg_qx_agent;

DECLARE_TOAST(pg_qx_agent, 9522, 9523);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_agent_oid_index, 9524, QxAgentOidIndexId, pg_qx_agent, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_agent_name_nsp_index, 9525, QxAgentNameNspIndexId, pg_qx_agent, btree(qxagentname name_ops, qxagentnamespace oid_ops));

MAKE_SYSCACHE(QXAGENTOID, pg_qx_agent_oid_index, 8);
MAKE_SYSCACHE(QXAGENTNAMENSP, pg_qx_agent_name_nsp_index, 8);

#endif							/* PG_QX_AGENT_H */
