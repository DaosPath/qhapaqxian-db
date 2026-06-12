/*-------------------------------------------------------------------------
 *
 * qx_backend_supervisor.h
 *	  in-memory registry of active backend execution leases
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_BACKEND_SUPERVISOR_H
#define QX_BACKEND_SUPERVISOR_H

#include "postgres.h"

typedef enum QxBackendLeaseKind
{
	QX_BACKEND_LEASE_CONTAINER,
	QX_BACKEND_LEASE_MICROVM,
	QX_BACKEND_LEASE_HOST
} QxBackendLeaseKind;

extern void qx_backend_supervisor_register(Oid taskoid, const char *instance_id,
										   QxBackendLeaseKind kind);
extern void qx_backend_supervisor_release(Oid taskoid, const char *instance_id);
extern void qx_backend_supervisor_fence_stale(Oid taskoid);

#endif							/* QX_BACKEND_SUPERVISOR_H */