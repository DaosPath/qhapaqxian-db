/*-------------------------------------------------------------------------
 *
 * pg_qx_principal.h
 *	  definition of the "tool principal registry" system catalog
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_PRINCIPAL_H
#define PG_QX_PRINCIPAL_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_principal_d.h"

CATALOG(pg_qx_principal,9590,QxPrincipalRelationId)
{
	Oid			oid;
	NameData	qxprincipalname;
	Oid			qxprincipalnamespace BKI_LOOKUP(pg_namespace);
	Oid			qxprincipalowner BKI_LOOKUP(pg_authid);
	Oid			qxprincipalproviderid BKI_LOOKUP(pg_qx_provider);
	bool		qxprincipalenabled BKI_DEFAULT(t);

#ifdef CATALOG_VARLEN
	text		qxprincipalsandbox BKI_FORCE_NULL;
	text		qxprincipalprogram BKI_FORCE_NULL;
	text		qxprincipalprovider BKI_FORCE_NULL;
	text		qxprincipalruntimeclass BKI_FORCE_NULL;
	text		qxprincipalreceiptsigner BKI_FORCE_NULL;
	text		qxprincipalattestationprofile BKI_FORCE_NULL;
	text		qxprincipalattestationversion BKI_FORCE_NULL;
	text		qxprincipalattestationpolicy BKI_FORCE_NULL;
#endif
} FormData_pg_qx_principal;

typedef FormData_pg_qx_principal *Form_pg_qx_principal;

DECLARE_TOAST(pg_qx_principal, 9591, 9592);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_principal_oid_index, 9593, QxPrincipalOidIndexId, pg_qx_principal, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_principal_name_nsp_index, 9594, QxPrincipalNameNspIndexId, pg_qx_principal, btree(qxprincipalname name_ops, qxprincipalnamespace oid_ops));

MAKE_SYSCACHE(QXPRINCIPALOID, pg_qx_principal_oid_index, 8);
MAKE_SYSCACHE(QXPRINCIPALNAMENSP, pg_qx_principal_name_nsp_index, 8);

#endif							/* PG_QX_PRINCIPAL_H */
