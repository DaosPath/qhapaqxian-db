/*-------------------------------------------------------------------------
 *
 * pg_qx_tool.h
 *	  definition of the "agent tool registry" system catalog
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_TOOL_H
#define PG_QX_TOOL_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_tool_d.h"

CATALOG(pg_qx_tool,9585,QxToolRelationId)
{
	Oid			oid;
	NameData	qxtoolname;
	Oid			qxtoolnamespace BKI_LOOKUP(pg_namespace);
	Oid			qxtoolowner BKI_LOOKUP(pg_authid);
	bool		qxtoolenabled BKI_DEFAULT(t);
	int32		qxtooltokencost BKI_DEFAULT(8);
	int32		qxtoolcostunits BKI_DEFAULT(4);

#ifdef CATALOG_VARLEN
	text		qxtoolhandler BKI_FORCE_NULL;
	text		qxtoolpolicy BKI_FORCE_NULL;
#endif
} FormData_pg_qx_tool;

typedef FormData_pg_qx_tool *Form_pg_qx_tool;

DECLARE_TOAST(pg_qx_tool, 9586, 9587);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_tool_oid_index, 9588, QxToolOidIndexId, pg_qx_tool, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_tool_name_nsp_index, 9589, QxToolNameNspIndexId, pg_qx_tool, btree(qxtoolname name_ops, qxtoolnamespace oid_ops));

MAKE_SYSCACHE(QXTOOLOID, pg_qx_tool_oid_index, 8);
MAKE_SYSCACHE(QXTOOLNAMENSP, pg_qx_tool_name_nsp_index, 8);

#endif							/* PG_QX_TOOL_H */
