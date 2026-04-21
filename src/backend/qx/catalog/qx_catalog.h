/*-------------------------------------------------------------------------
 *
 * qx_catalog.h
 *	  internal helpers for QhapaqXian catalog snapshots
 *
 *-------------------------------------------------------------------------*/
#ifndef QX_BACKEND_CATALOG_H
#define QX_BACKEND_CATALOG_H

#include "access/htup.h"

#include "qx/qx_catalog.h"

extern char *QxCatalogTextAttr(HeapTuple tup, AttrNumber attnum, int cacheid);
extern char *QxCatalogNameCopy(const NameData *name);
extern void QxCatalogFreeString(char **ptr);

#endif							/* QX_BACKEND_CATALOG_H */
