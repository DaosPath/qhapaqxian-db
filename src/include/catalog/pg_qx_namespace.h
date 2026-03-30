/*-------------------------------------------------------------------------
 *
 * pg_qx_namespace.h
 *	  definition of the "agent namespace policy" system catalog
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_NAMESPACE_H
#define PG_QX_NAMESPACE_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_namespace_d.h"

CATALOG(pg_qx_namespace,9580,QxNamespaceRelationId)
{
	Oid			oid;
	NameData	qxnamespacepolicyname;
	Oid			qxnamespaceid BKI_LOOKUP(pg_namespace);
	Oid			qxnamespaceowner BKI_LOOKUP(pg_authid);
	Oid			qxnamespaceauthrole BKI_LOOKUP(pg_authid);
	bool		qxrequireknowntools BKI_DEFAULT(t);
	bool		qxenforcebudgets BKI_DEFAULT(t);

#ifdef CATALOG_VARLEN
	text		qxnamespacepolicy BKI_FORCE_NULL;
	pg_node_tree qxallowedtools BKI_FORCE_NULL;
#endif
} FormData_pg_qx_namespace;

typedef FormData_pg_qx_namespace *Form_pg_qx_namespace;

DECLARE_TOAST(pg_qx_namespace, 9581, 9582);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_namespace_oid_index, 9583, QxNamespaceOidIndexId, pg_qx_namespace, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_namespace_name_nsp_index, 9584, QxNamespaceNameNspIndexId, pg_qx_namespace, btree(qxnamespacepolicyname name_ops, qxnamespaceid oid_ops));

MAKE_SYSCACHE(QXNAMESPACEOID, pg_qx_namespace_oid_index, 8);
MAKE_SYSCACHE(QXNAMESPACENAMENSP, pg_qx_namespace_name_nsp_index, 8);

#endif							/* PG_QX_NAMESPACE_H */
