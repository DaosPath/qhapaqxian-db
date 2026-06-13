/*-------------------------------------------------------------------------
 *
 * qx_backend_supervisor.c
 *	  in-memory registry of active backend execution leases
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/pg_list.h"
#include "qx_backend_supervisor.h"
#include "utils/memutils.h"

typedef struct QxBackendLeaseEntry
{
	Oid			taskoid;
	char	   *instance_id;
	QxBackendLeaseKind kind;
} QxBackendLeaseEntry;

static List *qx_backend_supervisor_leases = NIL;

static QxBackendLeaseEntry *
qx_backend_supervisor_find(Oid taskoid, const char *instance_id)
{
	ListCell   *lc;

	foreach(lc, qx_backend_supervisor_leases)
	{
		QxBackendLeaseEntry *entry = lfirst(lc);

		if (entry->taskoid != taskoid)
			continue;
		if (instance_id == NULL || instance_id[0] == '\0')
			return entry;
		if (entry->instance_id != NULL &&
			strcmp(entry->instance_id, instance_id) == 0)
			return entry;
	}

	return NULL;
}

void
qx_backend_supervisor_register(Oid taskoid, const char *instance_id,
							   QxBackendLeaseKind kind)
{
	QxBackendLeaseEntry *entry;

	if (!OidIsValid(taskoid) || instance_id == NULL || instance_id[0] == '\0')
		return;

	entry = qx_backend_supervisor_find(taskoid, instance_id);
	if (entry != NULL)
	{
		entry->kind = kind;
		return;
	}

	entry = MemoryContextAlloc(TopMemoryContext, sizeof(QxBackendLeaseEntry));
	entry->taskoid = taskoid;
	entry->instance_id = MemoryContextStrdup(TopMemoryContext, instance_id);
	entry->kind = kind;
	qx_backend_supervisor_leases = lappend(qx_backend_supervisor_leases, entry);
}

void
qx_backend_supervisor_release(Oid taskoid, const char *instance_id)
{
	ListCell   *lc;

	foreach(lc, qx_backend_supervisor_leases)
	{
		QxBackendLeaseEntry *entry = lfirst(lc);

		if (entry->taskoid != taskoid)
			continue;
		if (instance_id != NULL && instance_id[0] != '\0' &&
			(entry->instance_id == NULL ||
			 strcmp(entry->instance_id, instance_id) != 0))
			continue;

		if (entry->instance_id != NULL)
			pfree(entry->instance_id);
		qx_backend_supervisor_leases = list_delete_cell(qx_backend_supervisor_leases,
														lc);
		return;
	}
}

int
qx_backend_supervisor_count_for_task(Oid taskoid)
{
	ListCell   *lc;
	int			count = 0;

	foreach(lc, qx_backend_supervisor_leases)
	{
		QxBackendLeaseEntry *entry = lfirst(lc);

		if (entry->taskoid == taskoid)
			count++;
	}

	return count;
}

void
qx_backend_supervisor_fence_stale(Oid taskoid)
{
	ListCell   *lc;
	ListCell   *next;

	for (lc = list_head(qx_backend_supervisor_leases); lc != NULL; lc = next)
	{
		QxBackendLeaseEntry *entry = lfirst(lc);

		next = lnext(qx_backend_supervisor_leases, lc);
		if (entry->taskoid != taskoid)
			continue;

		if (entry->instance_id != NULL)
			pfree(entry->instance_id);
		qx_backend_supervisor_leases = list_delete_cell(qx_backend_supervisor_leases,
														lc);
	}
}