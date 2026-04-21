/*-------------------------------------------------------------------------
 *
 * pg_qx_provider.h
 *	  definition of the "agent runtime provider registry" system catalog
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_PROVIDER_H
#define PG_QX_PROVIDER_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_provider_d.h"

CATALOG(pg_qx_provider,9595,QxProviderRelationId)
{
	Oid			oid;
	NameData	qxprovidername;
	Oid			qxprovidernamespace BKI_LOOKUP(pg_namespace);
	Oid			qxproviderowner BKI_LOOKUP(pg_authid);
	bool		qxproviderenabled BKI_DEFAULT(t);
	bool		qxproviderattestationrequired BKI_DEFAULT(f);

#ifdef CATALOG_VARLEN
	text		qxproviderkind BKI_FORCE_NULL;
	text		qxproviderendpoint BKI_FORCE_NULL;
	text		qxproviderreceiptalg BKI_FORCE_NULL;
	text		qxproviderreceiptkey BKI_FORCE_NULL;
	text		qxproviderattestationprofile BKI_FORCE_NULL;
	text		qxproviderattestationversion BKI_FORCE_NULL;
	text		qxproviderattestationpolicy BKI_FORCE_NULL;
#endif
} FormData_pg_qx_provider;

typedef FormData_pg_qx_provider *Form_pg_qx_provider;

DECLARE_TOAST(pg_qx_provider, 9596, 9597);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_provider_oid_index, 9598, QxProviderOidIndexId, pg_qx_provider, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_qx_provider_name_nsp_index, 9599, QxProviderNameNspIndexId, pg_qx_provider, btree(qxprovidername name_ops, qxprovidernamespace oid_ops));

MAKE_SYSCACHE(QXPROVIDEROID, pg_qx_provider_oid_index, 8);
MAKE_SYSCACHE(QXPROVIDERNAMENSP, pg_qx_provider_name_nsp_index, 8);

#endif							/* PG_QX_PROVIDER_H */
