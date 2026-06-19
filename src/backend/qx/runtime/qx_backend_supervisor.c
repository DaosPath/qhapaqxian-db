/*-------------------------------------------------------------------------
 *
 * qx_backend_supervisor.c
 *	  shared-memory registry of active backend execution leases
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#include "qx/qx_stat.h"
#include "qx_backend_supervisor.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

const char *
QxBackendSupervisorLeaseKindName(QxBackendLeaseKind kind)
{
	switch (kind)
	{
		case QX_BACKEND_LEASE_CONTAINER:
			return "container";
		case QX_BACKEND_LEASE_MICROVM:
			return "microvm";
		case QX_BACKEND_LEASE_HOST:
			return "host";
		default:
			return "unknown";
	}
}

QxBackendLeaseKind
QxBackendSupervisorLeaseKindFromName(const char *name)
{
	if (name == NULL)
		return QX_BACKEND_LEASE_HOST;

	if (pg_strcasecmp(name, "container") == 0)
		return QX_BACKEND_LEASE_CONTAINER;
	if (pg_strcasecmp(name, "microvm") == 0)
		return QX_BACKEND_LEASE_MICROVM;
	if (pg_strcasecmp(name, "host") == 0)
		return QX_BACKEND_LEASE_HOST;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported backend lease kind \"%s\"", name)));
	return QX_BACKEND_LEASE_HOST;
}

void
qx_backend_supervisor_register(Oid taskoid, const char *instance_id,
							   QxBackendLeaseKind kind)
{
	if (!OidIsValid(taskoid) || instance_id == NULL || instance_id[0] == '\0')
		return;

	if (QxStatBackendLeaseRegister(MyDatabaseId, taskoid, instance_id, (int) kind))
		QxStatReportBackendSupervisorEvent(MyDatabaseId, "register");
}

void
qx_backend_supervisor_release(Oid taskoid, const char *instance_id)
{
	if (QxStatBackendLeaseRelease(MyDatabaseId, taskoid, instance_id))
		QxStatReportBackendSupervisorEvent(MyDatabaseId, "release");
}

int
qx_backend_supervisor_count_for_task(Oid taskoid)
{
	return QxStatBackendLeaseCount(MyDatabaseId, taskoid);
}

int
qx_backend_supervisor_active_count(void)
{
	return QxStatBackendLeaseCount(MyDatabaseId, InvalidOid);
}

void
qx_backend_supervisor_fence_stale(Oid taskoid)
{
	int			removed;

	removed = QxStatBackendLeaseFence(MyDatabaseId, taskoid);

	if (removed > 0)
		QxStatReportBackendSupervisorEvent(MyDatabaseId, "fence");
}

List *
QxBackendSupervisorBuildLeaseSnapshots(void)
{
	List	   *snapshots = NIL;
	QxStatBackendLeaseSnapshot *shared_snapshots;
	int			count;
	int			i;

	shared_snapshots = palloc(sizeof(QxStatBackendLeaseSnapshot) *
							   QX_STAT_BACKEND_LEASE_SLOT_COUNT);
	count = QxStatBackendLeaseGetSnapshots(MyDatabaseId, shared_snapshots,
										QX_STAT_BACKEND_LEASE_SLOT_COUNT);
	for (i = 0; i < count; i++)
	{
		QxBackendSupervisorLeaseSnapshot *snapshot;
		QxStatBackendLeaseSnapshot *shared = &shared_snapshots[i];

		snapshot = palloc(sizeof(QxBackendSupervisorLeaseSnapshot));
		snapshot->taskoid = shared->taskoid;
		snapshot->instance_id = pstrdup(shared->instance_id);
		snapshot->kind = (QxBackendLeaseKind) shared->kind;
		snapshots = lappend(snapshots, snapshot);
	}
	pfree(shared_snapshots);

	return snapshots;
}

Datum
pg_qx_backend_supervisor_list(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	QxStatBackendLeaseSnapshot *snapshots;
	int			count;
	int			i;

	InitMaterializedSRF(fcinfo, MAT_SRF_BLESS);

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	snapshots = palloc(sizeof(QxStatBackendLeaseSnapshot) *
					  QX_STAT_BACKEND_LEASE_SLOT_COUNT);
	count = QxStatBackendLeaseGetSnapshots(MyDatabaseId, snapshots,
										QX_STAT_BACKEND_LEASE_SLOT_COUNT);
	for (i = 0; i < count; i++)
	{
		QxStatBackendLeaseSnapshot *entry = &snapshots[i];
		Datum		values[3];
		bool		nulls[3];

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = ObjectIdGetDatum(entry->taskoid);
		if (entry->instance_id[0] != '\0')
			values[1] = CStringGetTextDatum(entry->instance_id);
		else
			nulls[1] = true;
		values[2] = CStringGetTextDatum(
			QxBackendSupervisorLeaseKindName((QxBackendLeaseKind) entry->kind));
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	pfree(snapshots);

	MemoryContextSwitchTo(oldcontext);
	return (Datum) 0;
}

void
QxBackendSupervisorFreeLeaseSnapshots(List *leases)
{
	ListCell   *lc;

	if (leases == NIL)
		return;

	foreach(lc, leases)
	{
		QxBackendSupervisorLeaseSnapshot *snapshot = lfirst(lc);

		if (snapshot->instance_id != NULL)
			pfree(snapshot->instance_id);
		pfree(snapshot);
	}

	list_free(leases);
}
