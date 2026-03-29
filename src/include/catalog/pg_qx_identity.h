/*-------------------------------------------------------------------------
 *
 * pg_qx_identity.h
 *	  definition of the "agent identity" system catalog (pg_qx_identity)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_identity.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_IDENTITY_H
#define PG_QX_IDENTITY_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_identity_d.h"

/* ----------------
 *		pg_qx_identity definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_identity
 * ----------------
 */
CATALOG(pg_qx_identity,9575,QxIdentityRelationId)
{
	Oid			oid;			/* oid */
	NameData	qxidentityname;	/* identity name */
	Oid			qxidentitynamespace BKI_LOOKUP(pg_namespace);	/* schema */
	Oid			qxidentityowner BKI_LOOKUP(pg_authid);	/* owner */
	Oid			qxidentityauthrole BKI_LOOKUP(pg_authid);	/* runtime auth role */

#ifdef CATALOG_VARLEN
	text		qxidentitypolicy BKI_FORCE_NULL;	/* policy label */
	pg_node_tree qxidentitybudget BKI_FORCE_NULL;	/* serialized List<DefElem> */
#endif
} FormData_pg_qx_identity;

typedef FormData_pg_qx_identity *Form_pg_qx_identity;

DECLARE_TOAST(pg_qx_identity, 9576, 9577);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_identity_oid_index, 9578, QxIdentityOidIndexId, pg_qx_identity, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_identity_name_nsp_index, 9579, QxIdentityNameNspIndexId, pg_qx_identity, btree(qxidentityname name_ops, qxidentitynamespace oid_ops));

MAKE_SYSCACHE(QXIDENTITYOID, pg_qx_identity_oid_index, 8);
MAKE_SYSCACHE(QXIDENTITYNAMENSP, pg_qx_identity_name_nsp_index, 8);

#endif							/* PG_QX_IDENTITY_H */
