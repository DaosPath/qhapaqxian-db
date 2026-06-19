/*-------------------------------------------------------------------------
 *
 * qx_backend_supervisor.h
 *	  shared-memory registry of active backend execution leases
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

typedef struct QxBackendSupervisorLeaseSnapshot
{
	Oid			taskoid;
	char	   *instance_id;
	QxBackendLeaseKind kind;
} QxBackendSupervisorLeaseSnapshot;

extern void qx_backend_supervisor_register(Oid taskoid, const char *instance_id,
										   QxBackendLeaseKind kind);
extern void qx_backend_supervisor_release(Oid taskoid, const char *instance_id);
extern void qx_backend_supervisor_fence_stale(Oid taskoid);
extern int	qx_backend_supervisor_count_for_task(Oid taskoid);
extern int	qx_backend_supervisor_active_count(void);
extern List *QxBackendSupervisorBuildLeaseSnapshots(void);
extern void QxBackendSupervisorFreeLeaseSnapshots(List *leases);
extern const char *QxBackendSupervisorLeaseKindName(QxBackendLeaseKind kind);
extern QxBackendLeaseKind QxBackendSupervisorLeaseKindFromName(const char *name);

#endif							/* QX_BACKEND_SUPERVISOR_H */
