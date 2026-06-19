/*-------------------------------------------------------------------------
 *
 * qx_stat.c
 *	  shared-memory statistics collector for QhapaqXian Engine
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <limits.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"

#include "catalog/catalog.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_qx_principal.h"
#include "catalog/pg_qx_provider.h"
#include "catalog/pg_qx_stat_history.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "qx/qx_catalog.h"
#include "qx/qx_observe.h"
#include "qx/qx_recovery.h"
#include "qx/qx_stat.h"

#include "storage/ipc.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"

#define QX_STAT_SLOT_COUNT			512

typedef struct QxStatSlot
{
	bool		in_use;
	Oid			dboid;
	QxStatKind	kind;
	Oid			entity_oid;
	char		runtime_class[QX_STAT_RUNTIME_CLASS_LEN];
	QxStatCounters counters;
} QxStatSlot;

typedef struct QxStatSharedState
{
	slock_t		mutex;
	QxStatSlot	slots[QX_STAT_SLOT_COUNT];
	QxStatBackendLeaseSnapshot backend_leases[QX_STAT_BACKEND_LEASE_SLOT_COUNT];
} QxStatSharedState;

bool		qhapaqxian_track_stats = true;

static QxStatSharedState *QxStat = NULL;

static void qx_stat_ensure_attached(void);
static bool qx_stat_slot_matches(const QxStatSlot *slot, Oid dboid,
								 QxStatKind kind, Oid entity_oid,
								 const char *runtime_class);
static QxStatCounters *qx_stat_lookup_entry(Oid dboid, QxStatKind kind,
											Oid entity_oid,
											const char *runtime_class,
											bool create);
static void qx_stat_with_entry(Oid dboid, QxStatKind kind, Oid entity_oid,
							   const char *runtime_class, bool create,
							   void (*mutate)(QxStatCounters *counters,
											  void *ctx), void *ctx);
static void qx_stat_reset_kind_locked(QxStatKind kind);
static void qx_stat_report_from_trace(const char *trace_name,
									  const char *trace_detail);
static Oid qx_stat_lookup_provider_oid(const char *provider_name);
static Oid qx_stat_lookup_principal_oid(const char *principal_name);

Size
QxStatShmemSize(void)
{
	return MAXALIGN(sizeof(QxStatSharedState));
}

void
QxStatShmemInit(void)
{
	bool		found;

	QxStat = ShmemInitStruct("Qx Stat Collector",
							 sizeof(QxStatSharedState),
							 &found);
	if (!found)
	{
		MemSet(QxStat, 0, sizeof(QxStatSharedState));
		SpinLockInit(&QxStat->mutex);
	}
}

static void
qx_stat_ensure_attached(void)
{
	if (QxStat == NULL)
		QxStatShmemInit();
}

static bool
qx_stat_slot_matches(const QxStatSlot *slot, Oid dboid, QxStatKind kind,
					 Oid entity_oid, const char *runtime_class)
{
	const char *rc = runtime_class != NULL ? runtime_class : "";

	if (!slot->in_use)
		return false;
	if (slot->dboid != dboid || slot->kind != kind)
		return false;
	if (slot->entity_oid != entity_oid)
		return false;
	return strcmp(slot->runtime_class, rc) == 0;
}

static QxStatCounters *
qx_stat_lookup_entry(Oid dboid, QxStatKind kind, Oid entity_oid,
					 const char *runtime_class, bool create)
{
	int			i;
	QxStatSlot *free_slot = NULL;

	qx_stat_ensure_attached();
	if (QxStat == NULL)
		return NULL;

	SpinLockAcquire(&QxStat->mutex);
	for (i = 0; i < QX_STAT_SLOT_COUNT; i++)
	{
		QxStatSlot   *slot = &QxStat->slots[i];

		if (qx_stat_slot_matches(slot, dboid, kind, entity_oid, runtime_class))
		{
			SpinLockRelease(&QxStat->mutex);
			return &slot->counters;
		}

		if (create && !slot->in_use && free_slot == NULL)
			free_slot = slot;
	}

	if (!create || free_slot == NULL)
	{
		SpinLockRelease(&QxStat->mutex);
		if (create)
			ereport(WARNING,
					(errmsg("qx_stat slot table is full for kind %d entity %u",
							(int) kind, entity_oid)));
		return NULL;
	}

	MemSet(free_slot, 0, sizeof(QxStatSlot));
	free_slot->in_use = true;
	free_slot->dboid = dboid;
	free_slot->kind = kind;
	free_slot->entity_oid = entity_oid;
	if (runtime_class != NULL)
		strlcpy(free_slot->runtime_class, runtime_class,
				sizeof(free_slot->runtime_class));

	SpinLockRelease(&QxStat->mutex);
	return &free_slot->counters;
}

static void
qx_stat_with_entry(Oid dboid, QxStatKind kind, Oid entity_oid,
				   const char *runtime_class, bool create,
				   void (*mutate)(QxStatCounters *counters, void *ctx),
				   void *ctx)
{
	int			i;
	QxStatSlot *free_slot = NULL;
	QxStatSlot *target = NULL;

	qx_stat_ensure_attached();
	if (QxStat == NULL || mutate == NULL)
		return;

	SpinLockAcquire(&QxStat->mutex);
	for (i = 0; i < QX_STAT_SLOT_COUNT; i++)
	{
		QxStatSlot   *slot = &QxStat->slots[i];

		if (qx_stat_slot_matches(slot, dboid, kind, entity_oid, runtime_class))
		{
			target = slot;
			break;
		}

		if (create && !slot->in_use && free_slot == NULL)
			free_slot = slot;
	}

	if (target == NULL && create && free_slot != NULL)
	{
		MemSet(free_slot, 0, sizeof(QxStatSlot));
		free_slot->in_use = true;
		free_slot->dboid = dboid;
		free_slot->kind = kind;
		free_slot->entity_oid = entity_oid;
		if (runtime_class != NULL)
			strlcpy(free_slot->runtime_class, runtime_class,
					sizeof(free_slot->runtime_class));
		target = free_slot;
	}

	if (target != NULL)
		mutate(&target->counters, ctx);
	else if (create)
		ereport(WARNING,
				(errmsg("qx_stat slot table is full for kind %d entity %u",
						(int) kind, entity_oid)));

	SpinLockRelease(&QxStat->mutex);
}

typedef struct QxStatProviderMutation
{
	bool		resume;
	bool		receipt_verified;
} QxStatProviderMutation;

static void
qx_stat_mutate_provider(QxStatCounters *counters, void *ctx)
{
	QxStatProviderMutation *mutation = (QxStatProviderMutation *) ctx;

	if (mutation->resume)
		counters->resume_count++;
	else
		counters->submit_count++;
	if (mutation->receipt_verified)
		counters->verified_receipts++;
	else
		counters->rejected_receipts++;
}

void
QxStatReportProviderExecution(Oid dboid, Oid provideroid, bool resume,
							  bool receipt_verified)
{
	QxStatProviderMutation mutation;

	if (!qhapaqxian_track_stats || !OidIsValid(provideroid))
		return;

	mutation.resume = resume;
	mutation.receipt_verified = receipt_verified;
	qx_stat_with_entry(dboid, QX_STAT_PROVIDER, provideroid, NULL, true,
					   qx_stat_mutate_provider, &mutation);
}

typedef struct QxStatPrincipalMutation
{
	bool		resume;
	bool		checkpointed;
	bool		receipt_verified;
} QxStatPrincipalMutation;

static void
qx_stat_mutate_principal(QxStatCounters *counters, void *ctx)
{
	QxStatPrincipalMutation *mutation = (QxStatPrincipalMutation *) ctx;

	if (mutation->resume)
		counters->resume_count++;
	else
		counters->submit_count++;
	if (mutation->checkpointed)
		counters->checkpoint_count++;
	if (mutation->receipt_verified)
		counters->verified_receipts++;
}

void
QxStatReportPrincipalExecution(Oid dboid, Oid principaloid, bool resume,
							   bool checkpointed, bool receipt_verified)
{
	QxStatPrincipalMutation mutation;

	if (!qhapaqxian_track_stats || !OidIsValid(principaloid))
		return;

	mutation.resume = resume;
	mutation.checkpointed = checkpointed;
	mutation.receipt_verified = receipt_verified;
	qx_stat_with_entry(dboid, QX_STAT_PRINCIPAL, principaloid, NULL, true,
					   qx_stat_mutate_principal, &mutation);
}

static void
qx_stat_mutate_runtime_class(QxStatCounters *counters, void *ctx)
{
	QxStatPrincipalMutation *mutation = (QxStatPrincipalMutation *) ctx;

	if (mutation->resume)
		counters->resume_count++;
	else
		counters->submit_count++;
	if (mutation->checkpointed)
		counters->checkpoint_count++;
	if (mutation->receipt_verified)
		counters->verified_receipts++;
}

void
QxStatReportRuntimeClassExecution(Oid dboid, const char *runtime_class,
								  bool resume, bool checkpointed,
								  bool receipt_verified)
{
	QxStatPrincipalMutation mutation;

	if (!qhapaqxian_track_stats)
		return;
	if (runtime_class == NULL || runtime_class[0] == '\0')
		return;

	mutation.resume = resume;
	mutation.checkpointed = checkpointed;
	mutation.receipt_verified = receipt_verified;
	qx_stat_with_entry(dboid, QX_STAT_RUNTIME_CLASS, InvalidOid,
					   runtime_class, true, qx_stat_mutate_runtime_class,
					   &mutation);
}

static void
qx_stat_mutate_scheduler(QxStatCounters *counters, void *ctx)
{
	const char *event_name = (const char *) ctx;

	if (strcmp(event_name, "renew") == 0)
		counters->renew_count++;
	else if (strcmp(event_name, "reclaim") == 0)
		counters->reclaim_count++;
	else if (strcmp(event_name, "release") == 0)
		counters->release_count++;
	else if (strcmp(event_name, "retry_dispatch") == 0)
		counters->retry_dispatch_count++;
	else if (strcmp(event_name, "dead_letter") == 0)
		counters->dead_letter_count++;
}

void
QxStatReportSchedulerEvent(Oid dboid, const char *event_name)
{
	if (!qhapaqxian_track_stats)
		return;
	if (event_name == NULL || event_name[0] == '\0')
		return;

	qx_stat_with_entry(dboid, QX_STAT_SCHEDULER_ACTIVITY, InvalidOid,
					   event_name, true, qx_stat_mutate_scheduler,
					   (void *) event_name);
}

typedef struct QxStatRecoveryMutation
{
	bool		failover_rebuild;
	const QxRecoveryReport *report;
} QxStatRecoveryMutation;

static void
qx_stat_mutate_recovery(QxStatCounters *counters, void *ctx)
{
	QxStatRecoveryMutation *mutation = (QxStatRecoveryMutation *) ctx;

	if (mutation->failover_rebuild)
		counters->recovery_failover_rebuilds++;
	else
		counters->recovery_startup_scans++;

	if (mutation->report != NULL)
	{
		counters->recovery_tasks_requeued += mutation->report->tasks_requeued;
		counters->recovery_attempts_fenced += mutation->report->attempts_fenced;
		counters->recovery_tasks_requeue_suppressed +=
			mutation->report->tasks_requeue_suppressed;
		counters->recovery_attempts_fence_suppressed +=
			mutation->report->attempts_fence_suppressed;
	}
}

void
QxStatReportRecoveryScan(Oid dboid, const QxRecoveryReport *report,
						 bool failover_rebuild)
{
	QxStatRecoveryMutation mutation;

	if (!qhapaqxian_track_stats)
		return;

	mutation.failover_rebuild = failover_rebuild;
	mutation.report = report;
	qx_stat_with_entry(dboid, QX_STAT_RECOVERY, InvalidOid, NULL, true,
					   qx_stat_mutate_recovery, &mutation);
}

static void
qx_stat_mutate_backend_supervisor(QxStatCounters *counters, void *ctx)
{
	const char *event_name = (const char *) ctx;

	if (strcmp(event_name, "register") == 0)
		counters->backend_supervisor_register_count++;
	else if (strcmp(event_name, "release") == 0)
		counters->backend_supervisor_release_count++;
	else if (strcmp(event_name, "fence") == 0)
		counters->backend_supervisor_fence_count++;
}

void
QxStatReportBackendSupervisorEvent(Oid dboid, const char *event_name)
{
	if (!qhapaqxian_track_stats)
		return;
	if (event_name == NULL || event_name[0] == '\0')
		return;

	qx_stat_with_entry(dboid, QX_STAT_BACKEND_SUPERVISOR, InvalidOid, NULL, true,
					   qx_stat_mutate_backend_supervisor,
					   (void *) event_name);
}

bool
QxStatBackendLeaseRegister(Oid dboid, Oid taskoid, const char *instance_id,
						   int kind)
{
	QxStatBackendLeaseSnapshot *free_slot = NULL;
	int			i;
	bool		inserted = false;

	if (!OidIsValid(dboid) || !OidIsValid(taskoid) || instance_id == NULL ||
		instance_id[0] == '\0')
		return false;
	if (strlen(instance_id) >= QX_STAT_BACKEND_INSTANCE_ID_LEN)
		ereport(ERROR,
				(errcode(ERRCODE_NAME_TOO_LONG),
				 errmsg("backend instance identifier is too long"),
				 errdetail("Backend instance identifiers must be shorter than %d bytes.",
						   QX_STAT_BACKEND_INSTANCE_ID_LEN)));

	qx_stat_ensure_attached();
	SpinLockAcquire(&QxStat->mutex);
	for (i = 0; i < QX_STAT_BACKEND_LEASE_SLOT_COUNT; i++)
	{
		QxStatBackendLeaseSnapshot *slot = &QxStat->backend_leases[i];

		if (!OidIsValid(slot->dboid))
		{
			if (free_slot == NULL)
				free_slot = slot;
			continue;
		}
		if (slot->dboid == dboid && slot->taskoid == taskoid &&
			strcmp(slot->instance_id, instance_id) == 0)
		{
			slot->kind = kind;
			SpinLockRelease(&QxStat->mutex);
			return false;
		}
	}

	if (free_slot != NULL)
	{
		MemSet(free_slot, 0, sizeof(*free_slot));
		free_slot->dboid = dboid;
		free_slot->taskoid = taskoid;
		free_slot->kind = kind;
		strlcpy(free_slot->instance_id, instance_id,
				sizeof(free_slot->instance_id));
		inserted = true;
	}
	SpinLockRelease(&QxStat->mutex);

	if (!inserted)
		ereport(WARNING,
				(errmsg("QX backend supervisor lease table is full")));
	return inserted;
}

bool
QxStatBackendLeaseRelease(Oid dboid, Oid taskoid, const char *instance_id)
{
	int			i;

	if (!OidIsValid(dboid) || !OidIsValid(taskoid))
		return false;

	qx_stat_ensure_attached();
	SpinLockAcquire(&QxStat->mutex);
	for (i = 0; i < QX_STAT_BACKEND_LEASE_SLOT_COUNT; i++)
	{
		QxStatBackendLeaseSnapshot *slot = &QxStat->backend_leases[i];

		if (slot->dboid != dboid || slot->taskoid != taskoid)
			continue;
		if (instance_id != NULL && instance_id[0] != '\0' &&
			strcmp(slot->instance_id, instance_id) != 0)
			continue;
		MemSet(slot, 0, sizeof(*slot));
		SpinLockRelease(&QxStat->mutex);
		return true;
	}
	SpinLockRelease(&QxStat->mutex);
	return false;
}

int
QxStatBackendLeaseFence(Oid dboid, Oid taskoid)
{
	int			i;
	int			removed = 0;

	if (!OidIsValid(dboid) || !OidIsValid(taskoid))
		return 0;

	qx_stat_ensure_attached();
	SpinLockAcquire(&QxStat->mutex);
	for (i = 0; i < QX_STAT_BACKEND_LEASE_SLOT_COUNT; i++)
	{
		QxStatBackendLeaseSnapshot *slot = &QxStat->backend_leases[i];

		if (slot->dboid == dboid && slot->taskoid == taskoid)
		{
			MemSet(slot, 0, sizeof(*slot));
			removed++;
		}
	}
	SpinLockRelease(&QxStat->mutex);
	return removed;
}

int
QxStatBackendLeaseCount(Oid dboid, Oid taskoid)
{
	int			i;
	int			count = 0;

	qx_stat_ensure_attached();
	SpinLockAcquire(&QxStat->mutex);
	for (i = 0; i < QX_STAT_BACKEND_LEASE_SLOT_COUNT; i++)
	{
		QxStatBackendLeaseSnapshot *slot = &QxStat->backend_leases[i];

		if (slot->dboid == dboid &&
			(!OidIsValid(taskoid) || slot->taskoid == taskoid))
			count++;
	}
	SpinLockRelease(&QxStat->mutex);
	return count;
}

int
QxStatBackendLeaseGetSnapshots(Oid dboid,
							   QxStatBackendLeaseSnapshot *snapshots,
							   int max_snapshots)
{
	int			i;
	int			count = 0;

	if (snapshots == NULL || max_snapshots <= 0)
		return 0;

	qx_stat_ensure_attached();
	SpinLockAcquire(&QxStat->mutex);
	for (i = 0; i < QX_STAT_BACKEND_LEASE_SLOT_COUNT && count < max_snapshots; i++)
	{
		QxStatBackendLeaseSnapshot *slot = &QxStat->backend_leases[i];

		if (slot->dboid != dboid)
			continue;
		snapshots[count++] = *slot;
	}
	SpinLockRelease(&QxStat->mutex);
	return count;
}

static void
qx_stat_aggregate_scheduler_locked(Oid dboid, QxStatCounters *totals)
{
	int			i;

	MemSet(totals, 0, sizeof(QxStatCounters));
	for (i = 0; i < QX_STAT_SLOT_COUNT; i++)
	{
		QxStatSlot   *slot = &QxStat->slots[i];

		if (!slot->in_use || slot->kind != QX_STAT_SCHEDULER_ACTIVITY ||
			slot->dboid != dboid)
			continue;

		totals->renew_count += slot->counters.renew_count;
		totals->reclaim_count += slot->counters.reclaim_count;
		totals->release_count += slot->counters.release_count;
		totals->retry_dispatch_count += slot->counters.retry_dispatch_count;
		totals->dead_letter_count += slot->counters.dead_letter_count;
	}
}

void
QxStatFlushPending(void)
{
	/* v1 collector writes counters directly; nothing to flush. */
}

static void
qx_stat_reset_kind_locked(QxStatKind kind)
{
	int			i;

	for (i = 0; i < QX_STAT_SLOT_COUNT; i++)
	{
		QxStatSlot   *slot = &QxStat->slots[i];

		if (!slot->in_use || slot->kind != kind)
			continue;
		MemSet(&slot->counters, 0, sizeof(slot->counters));
	}

}

void
QxStatResetAll(void)
{
	if (QxStat == NULL)
		return;

	SpinLockAcquire(&QxStat->mutex);
	MemSet(QxStat->slots, 0, sizeof(QxStat->slots));
	SpinLockRelease(&QxStat->mutex);
}

void
QxStatReset(QxStatKind kind)
{
	if (QxStat == NULL)
		return;

	SpinLockAcquire(&QxStat->mutex);
	qx_stat_reset_kind_locked(kind);
	SpinLockRelease(&QxStat->mutex);
}

static Oid
qx_stat_lookup_provider_oid(const char *provider_name)
{
	QxCatalogProviderInfo info;
	Oid			namespaceoid;

	if (provider_name == NULL || provider_name[0] == '\0')
		return InvalidOid;

	namespaceoid = get_namespace_oid("public", false);
	if (QxCatalogLookupProviderByName(namespaceoid, provider_name, &info))
	{
		Oid			provideroid = info.oid;

		QxCatalogFreeProviderInfo(&info);
		return provideroid;
	}

	return InvalidOid;
}

static Oid
qx_stat_lookup_principal_oid(const char *principal_name)
{
	QxCatalogPrincipalInfo info;
	Oid			namespaceoid;

	if (principal_name == NULL || principal_name[0] == '\0')
		return InvalidOid;

	namespaceoid = get_namespace_oid("public", false);
	if (QxCatalogLookupPrincipalByName(namespaceoid, principal_name, &info))
	{
		Oid			principaloid = info.oid;

		QxCatalogFreePrincipalInfo(&info);
		return principaloid;
	}

	return InvalidOid;
}

static void
qx_stat_report_from_trace(const char *trace_name, const char *trace_detail)
{
	char	   *provider_name;
	char	   *principal_name;
	char	   *principal_runtime;
	char	   *receipt_sig;
	bool		resume;
	bool		checkpointed;
	bool		receipt_verified;
	Oid			provideroid;
	Oid			principaloid;

	if (!qhapaqxian_track_stats)
		return;

	provider_name = QxObserveTraceDetailValue(trace_detail, "provider");
	principal_name = QxObserveTraceDetailValue(trace_detail, "principal");
	principal_runtime = QxObserveTraceDetailValue(trace_detail, "principal_runtime");
	receipt_sig = QxObserveTraceDetailValue(trace_detail, "receipt_sig");
	resume = trace_name != NULL && strcmp(trace_name, "runtime.external_resume") == 0;
	checkpointed = trace_name != NULL &&
		(strcmp(trace_name, "runtime.checkpoint") == 0 ||
		 strcmp(trace_name, "runtime.final_checkpoint") == 0);
	receipt_verified = receipt_sig != NULL && strcmp(receipt_sig, "verified") == 0;

	if (trace_name != NULL &&
		(strcmp(trace_name, "runtime.external_submit") == 0 ||
		 strcmp(trace_name, "runtime.external_resume") == 0))
	{
		provideroid = qx_stat_lookup_provider_oid(provider_name);
		if (OidIsValid(provideroid))
			QxStatReportProviderExecution(MyDatabaseId, provideroid, resume,
										  receipt_verified);

		principaloid = qx_stat_lookup_principal_oid(principal_name);
		if (OidIsValid(principaloid))
			QxStatReportPrincipalExecution(MyDatabaseId, principaloid, resume,
										   checkpointed, receipt_verified);

		if (principal_runtime != NULL && principal_runtime[0] != '\0')
			QxStatReportRuntimeClassExecution(MyDatabaseId, principal_runtime,
											  resume, checkpointed,
											  receipt_verified);
	}
	else if (trace_name != NULL && checkpointed &&
			 principal_runtime != NULL && principal_runtime[0] != '\0')
	{
		QxStatReportRuntimeClassExecution(MyDatabaseId, principal_runtime,
										  false, true, false);
	}
	else if (trace_name != NULL && strcmp(trace_name, "runtime.retry_dispatch") == 0)
		QxStatReportSchedulerEvent(MyDatabaseId, "retry_dispatch");
	else if (trace_name != NULL && strcmp(trace_name, "runtime.dead_letter") == 0)
		QxStatReportSchedulerEvent(MyDatabaseId, "dead_letter");

	if (provider_name != NULL)
		pfree(provider_name);
	if (principal_name != NULL)
		pfree(principal_name);
	if (principal_runtime != NULL)
		pfree(principal_runtime);
	if (receipt_sig != NULL)
		pfree(receipt_sig);
}

void
QxStatReportTrace(const char *trace_name, const char *trace_detail)
{
	qx_stat_report_from_trace(trace_name, trace_detail);
}

typedef struct QxStatProviderSrfState
{
	int			index;
	List	   *providers;
} QxStatProviderSrfState;

Datum
pg_qx_stat_reset(PG_FUNCTION_ARGS)
{
	char	   *scope;

	if (PG_ARGISNULL(0))
		scope = pstrdup("all");
	else
		scope = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (pg_strcasecmp(scope, "all") == 0)
		QxStatResetAll();
	else if (pg_strcasecmp(scope, "provider") == 0)
		QxStatReset(QX_STAT_PROVIDER);
	else if (pg_strcasecmp(scope, "principal") == 0)
		QxStatReset(QX_STAT_PRINCIPAL);
	else if (pg_strcasecmp(scope, "runtime_class") == 0)
		QxStatReset(QX_STAT_RUNTIME_CLASS);
	else if (pg_strcasecmp(scope, "scheduler") == 0 ||
			 pg_strcasecmp(scope, "scheduler_activity") == 0)
		QxStatReset(QX_STAT_SCHEDULER_ACTIVITY);
	else if (pg_strcasecmp(scope, "recovery") == 0)
		QxStatReset(QX_STAT_RECOVERY);
	else if (pg_strcasecmp(scope, "backend_supervisor") == 0 ||
			 pg_strcasecmp(scope, "supervisor") == 0)
		QxStatReset(QX_STAT_BACKEND_SUPERVISOR);
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid QX stat reset scope \"%s\"", scope)));

	pfree(scope);
	PG_RETURN_VOID();
}

Datum
pg_qx_stat_get_provider_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	QxStatProviderSrfState *state;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc0(sizeof(QxStatProviderSrfState));
		state->index = 0;
		state->providers = NIL;

		state->providers = QxCatalogBuildProviderInfoList(InvalidOid, InvalidOid);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (QxStatProviderSrfState *) funcctx->user_fctx;

	while (state->index < list_length(state->providers))
	{
		QxCatalogProviderInfo *info = list_nth(state->providers, state->index++);
		QxStatCounters *counters;
		Datum		values[14];
		bool		nulls[14];
		HeapTuple	tuple;

		counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_PROVIDER,
										info->oid, NULL, false);

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = ObjectIdGetDatum(info->oid);
		values[1] = CStringGetTextDatum(info->name != NULL ? info->name : "");
		values[2] = CStringGetTextDatum(info->kind != NULL ? info->kind : "");
		values[3] = CStringGetTextDatum(info->endpoint != NULL ? info->endpoint : "");
		values[4] = BoolGetDatum(info->enabled);
		values[5] = BoolGetDatum(info->attestation_required);
		values[6] = Int64GetDatum(counters != NULL ? counters->submit_count : 0);
		values[7] = Int64GetDatum(counters != NULL ? counters->resume_count : 0);
		values[8] = Int64GetDatum(counters != NULL ? counters->verified_receipts : 0);
		values[9] = Int64GetDatum(counters != NULL ? counters->rejected_receipts : 0);
		values[10] = Int64GetDatum(counters != NULL ? counters->checkpoint_count : 0);
		values[11] = Int64GetDatum(counters != NULL ?
								   counters->submit_count + counters->resume_count : 0);
		values[12] = Int64GetDatum(0);
		values[13] = Int64GetDatum(0);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

typedef struct QxStatPrincipalSrfState
{
	int			index;
	List	   *principals;
} QxStatPrincipalSrfState;

Datum
pg_qx_stat_get_principal_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	QxStatPrincipalSrfState *state;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc0(sizeof(QxStatPrincipalSrfState));
		state->index = 0;
		state->principals = NIL;

		state->principals = QxCatalogBuildPrincipalInfoList(InvalidOid, InvalidOid);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (QxStatPrincipalSrfState *) funcctx->user_fctx;

	while (state->index < list_length(state->principals))
	{
		QxCatalogPrincipalInfo *info = list_nth(state->principals, state->index++);
		QxStatCounters *counters;
		Datum		values[15];
		bool		nulls[15];
		HeapTuple	tuple;

		counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_PRINCIPAL,
										info->oid, NULL, false);

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = ObjectIdGetDatum(info->oid);
		values[1] = CStringGetTextDatum(info->name != NULL ? info->name : "");
		values[2] = CStringGetTextDatum(info->provider_name != NULL ?
										info->provider_name : "");
		values[3] = CStringGetTextDatum(info->provider_kind != NULL ?
										info->provider_kind : "");
		values[4] = CStringGetTextDatum(info->runtime_class != NULL ?
										info->runtime_class : "");
		values[5] = CStringGetTextDatum(info->sandbox != NULL ? info->sandbox : "");
		values[6] = CStringGetTextDatum(info->program != NULL ? info->program : "");
		values[7] = BoolGetDatum(info->enabled);
		values[8] = BoolGetDatum(info->receipt_signer != NULL &&
								 info->receipt_signer[0] != '\0');
		values[9] = Int64GetDatum(counters != NULL ? counters->submit_count : 0);
		values[10] = Int64GetDatum(counters != NULL ? counters->resume_count : 0);
		values[11] = Int64GetDatum(counters != NULL ? counters->verified_receipts : 0);
		values[12] = Int64GetDatum(counters != NULL ? counters->rejected_receipts : 0);
		values[13] = Int64GetDatum(counters != NULL ? counters->checkpoint_count : 0);
		values[14] = Int64GetDatum(counters != NULL ?
								   counters->submit_count + counters->resume_count : 0);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

typedef struct QxStatRuntimeClassSrfState
{
	int			index;
	List	   *runtime_classes;
} QxStatRuntimeClassSrfState;

Datum
pg_qx_stat_get_runtime_class_stats(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	QxStatRuntimeClassSrfState *state;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc0(sizeof(QxStatRuntimeClassSrfState));
		state->index = 0;
		state->runtime_classes = NIL;

		state->runtime_classes = QxCatalogBuildDistinctRuntimeClassList();

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (QxStatRuntimeClassSrfState *) funcctx->user_fctx;

	while (state->index < list_length(state->runtime_classes))
	{
		const char *runtime_class = list_nth(state->runtime_classes, state->index++);
		QxStatCounters *counters;
		Datum		values[8];
		bool		nulls[8];
		HeapTuple	tuple;

		counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_RUNTIME_CLASS,
										InvalidOid, runtime_class, false);

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = CStringGetTextDatum(runtime_class);
		values[1] = Int64GetDatum(counters != NULL ? counters->submit_count : 0);
		values[2] = Int64GetDatum(counters != NULL ? counters->resume_count : 0);
		values[3] = Int64GetDatum(counters != NULL ? counters->verified_receipts : 0);
		values[4] = Int64GetDatum(counters != NULL ? counters->rejected_receipts : 0);
		values[5] = Int64GetDatum(counters != NULL ? counters->checkpoint_count : 0);
		values[6] = Int64GetDatum(counters != NULL ?
								   counters->submit_count + counters->resume_count : 0);
		values[7] = Int64GetDatum(0);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

Datum
pg_qx_stat_get_scheduler_activity_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5];
	QxStatCounters totals;
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	qx_stat_ensure_attached();
	SpinLockAcquire(&QxStat->mutex);
	qx_stat_aggregate_scheduler_locked(MyDatabaseId, &totals);
	SpinLockRelease(&QxStat->mutex);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));

	values[0] = Int64GetDatum(totals.renew_count);
	values[1] = Int64GetDatum(totals.reclaim_count);
	values[2] = Int64GetDatum(totals.release_count);
	values[3] = Int64GetDatum(totals.retry_dispatch_count);
	values[4] = Int64GetDatum(totals.dead_letter_count);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum
pg_qx_stat_get_recovery_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[6];
	bool		nulls[6];
	QxStatCounters *counters;
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_RECOVERY,
									InvalidOid, NULL, false);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));

	values[0] = Int64GetDatum(counters != NULL ?
							  counters->recovery_startup_scans : 0);
	values[1] = Int64GetDatum(counters != NULL ?
							  counters->recovery_failover_rebuilds : 0);
	values[2] = Int64GetDatum(counters != NULL ?
							  counters->recovery_tasks_requeued : 0);
	values[3] = Int64GetDatum(counters != NULL ?
							  counters->recovery_attempts_fenced : 0);
	values[4] = Int64GetDatum(counters != NULL ?
							  counters->recovery_tasks_requeue_suppressed : 0);
	values[5] = Int64GetDatum(counters != NULL ?
							  counters->recovery_attempts_fence_suppressed : 0);

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

Datum
pg_qx_stat_get_backend_supervisor_stats(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[4];
	bool		nulls[4];
	QxStatCounters *counters;
	HeapTuple	tuple;

	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("function returning record called in context "
						"that cannot accept type record")));

	counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_BACKEND_SUPERVISOR,
									InvalidOid, NULL, false);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));

	values[0] = Int64GetDatum(counters != NULL ?
							  counters->backend_supervisor_register_count : 0);
	values[1] = Int64GetDatum(counters != NULL ?
							  counters->backend_supervisor_release_count : 0);
	values[2] = Int64GetDatum(counters != NULL ?
							  counters->backend_supervisor_fence_count : 0);
	values[3] = Int32GetDatum(QxStatBackendLeaseCount(MyDatabaseId, InvalidOid));

	tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

static bool
qx_stat_scope_matches(const char *requested, const char *candidate)
{
	if (requested == NULL || candidate == NULL)
		return false;
	if (pg_strcasecmp(requested, "all") == 0)
		return true;
	return pg_strcasecmp(requested, candidate) == 0;
}

static void
qx_stat_insert_history_row(const char *scope, Oid entityoid, const char *entity,
						   const QxStatCounters *counters)
{
	Relation	rel;
	Datum		values[Natts_pg_qx_stat_history];
	bool		nulls[Natts_pg_qx_stat_history];
	HeapTuple	tup;
	Oid			rowoid;
	TimestampTz captured = GetCurrentTimestamp();

	if (scope == NULL || counters == NULL)
		return;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	rel = table_open(QxStatHistoryRelationId, RowExclusiveLock);

	rowoid = GetNewOidWithIndex(rel, QxStatHistoryOidIndexId,
								Anum_pg_qx_stat_history_oid);
	values[Anum_pg_qx_stat_history_oid - 1] = ObjectIdGetDatum(rowoid);
	values[Anum_pg_qx_stat_history_qxstathistorydbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_stat_history_qxstathistorycaptured - 1] =
		TimestampTzGetDatum(captured);
	values[Anum_pg_qx_stat_history_qxstathistoryentityoid - 1] =
		ObjectIdGetDatum(entityoid);
	values[Anum_pg_qx_stat_history_qxstathistorysubmitcount - 1] =
		Int32GetDatum((int32) Min(counters->submit_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryresumecount - 1] =
		Int32GetDatum((int32) Min(counters->resume_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryverifiedreceipts - 1] =
		Int32GetDatum((int32) Min(counters->verified_receipts, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryrejectedreceipts - 1] =
		Int32GetDatum((int32) Min(counters->rejected_receipts, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistorycheckpointcount - 1] =
		Int32GetDatum((int32) Min(counters->checkpoint_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryrenewcount - 1] =
		Int32GetDatum((int32) Min(counters->renew_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryreclaimcount - 1] =
		Int32GetDatum((int32) Min(counters->reclaim_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryreleasecount - 1] =
		Int32GetDatum((int32) Min(counters->release_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryretrydispatchcount - 1] =
		Int32GetDatum((int32) Min(counters->retry_dispatch_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistorydeadlettercount - 1] =
		Int32GetDatum((int32) Min(counters->dead_letter_count, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistorystartupscancount - 1] =
		Int32GetDatum((int32) Min(counters->recovery_startup_scans, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryfailoverrebuildcount - 1] =
		Int32GetDatum((int32) Min(counters->recovery_failover_rebuilds, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistorytasksrequeued - 1] =
		Int32GetDatum((int32) Min(counters->recovery_tasks_requeued, INT_MAX));
	values[Anum_pg_qx_stat_history_qxstathistoryscope - 1] =
		CStringGetTextDatum(scope);
	if (entity != NULL && entity[0] != '\0')
		values[Anum_pg_qx_stat_history_qxstathistoryentity - 1] =
			CStringGetTextDatum(entity);
	else
		nulls[Anum_pg_qx_stat_history_qxstathistoryentity - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);
	table_close(rel, RowExclusiveLock);
}

static int64
qx_stat_snapshot_scope(const char *scope)
{
	int64		written = 0;
	List	   *providers;
	List	   *principals;
	List	   *runtime_classes;
	ListCell   *lc;

	if (scope == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("QX stat snapshot scope must not be null")));

	if (qx_stat_scope_matches(scope, "provider"))
	{
		providers = QxCatalogBuildProviderInfoList(InvalidOid, InvalidOid);
		foreach(lc, providers)
		{
			QxCatalogProviderInfo *info = lfirst(lc);
			QxStatCounters *counters;
			QxStatCounters empty;

			counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_PROVIDER,
											info->oid, NULL, false);
			if (counters == NULL)
			{
				MemSet(&empty, 0, sizeof(empty));
				counters = &empty;
			}
			qx_stat_insert_history_row("provider", info->oid, info->name, counters);
			written++;
		}
		list_free_deep(providers);
	}

	if (qx_stat_scope_matches(scope, "principal"))
	{
		principals = QxCatalogBuildPrincipalInfoList(InvalidOid, InvalidOid);
		foreach(lc, principals)
		{
			QxCatalogPrincipalInfo *info = lfirst(lc);
			QxStatCounters *counters;
			QxStatCounters empty;

			counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_PRINCIPAL,
											info->oid, NULL, false);
			if (counters == NULL)
			{
				MemSet(&empty, 0, sizeof(empty));
				counters = &empty;
			}
			qx_stat_insert_history_row("principal", info->oid, info->name,
									   counters);
			written++;
		}
		list_free_deep(principals);
	}

	if (qx_stat_scope_matches(scope, "runtime_class"))
	{
		runtime_classes = QxCatalogBuildDistinctRuntimeClassList();
		foreach(lc, runtime_classes)
		{
			const char *runtime_class = lfirst(lc);
			QxStatCounters *counters;
			QxStatCounters empty;

			counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_RUNTIME_CLASS,
											InvalidOid, runtime_class, false);
			if (counters == NULL)
			{
				MemSet(&empty, 0, sizeof(empty));
				counters = &empty;
			}
			qx_stat_insert_history_row("runtime_class", InvalidOid,
									   runtime_class, counters);
			written++;
		}
		list_free_deep(runtime_classes);
	}

	if (qx_stat_scope_matches(scope, "scheduler") ||
		qx_stat_scope_matches(scope, "scheduler_activity"))
	{
		QxStatCounters totals;

		qx_stat_ensure_attached();
		SpinLockAcquire(&QxStat->mutex);
		qx_stat_aggregate_scheduler_locked(MyDatabaseId, &totals);
		SpinLockRelease(&QxStat->mutex);
		qx_stat_insert_history_row("scheduler", InvalidOid, "scheduler_activity",
								   &totals);
		written++;
	}

	if (qx_stat_scope_matches(scope, "recovery"))
	{
		QxStatCounters *counters;
		QxStatCounters empty;

		counters = qx_stat_lookup_entry(MyDatabaseId, QX_STAT_RECOVERY,
										InvalidOid, NULL, false);
		if (counters == NULL)
		{
			MemSet(&empty, 0, sizeof(empty));
			counters = &empty;
		}
		qx_stat_insert_history_row("recovery", InvalidOid, "recovery", counters);
		written++;
	}

	if (written == 0 &&
		pg_strcasecmp(scope, "all") != 0 &&
		pg_strcasecmp(scope, "provider") != 0 &&
		pg_strcasecmp(scope, "principal") != 0 &&
		pg_strcasecmp(scope, "runtime_class") != 0 &&
		pg_strcasecmp(scope, "scheduler") != 0 &&
		pg_strcasecmp(scope, "scheduler_activity") != 0 &&
		pg_strcasecmp(scope, "recovery") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid QX stat snapshot scope \"%s\"", scope)));

	return written;
}

Datum
pg_qx_stat_snapshot(PG_FUNCTION_ARGS)
{
	char	   *scope;
	int64		written;

	if (PG_ARGISNULL(0))
		scope = pstrdup("all");
	else
		scope = text_to_cstring(PG_GETARG_TEXT_PP(0));

	written = qx_stat_snapshot_scope(scope);
	pfree(scope);
	PG_RETURN_INT64(written);
}

typedef struct QxStatHistorySrfState
{
	Relation	rel;
	TableScanDesc scan;
	char	   *scope;
	TimestampTz since;
} QxStatHistorySrfState;

static char *
qx_stat_history_text_attr(TupleDesc tupdesc, HeapTuple tup, AttrNumber attnum)
{
	bool		isnull;
	Datum		datum;

	datum = heap_getattr(tup, attnum, tupdesc, &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static bool
qx_stat_history_scope_matches(const char *requested, const char *row_scope)
{
	if (requested == NULL || row_scope == NULL)
		return false;
	if (pg_strcasecmp(requested, "all") == 0)
		return true;
	return pg_strcasecmp(requested, row_scope) == 0;
}

Datum
pg_qx_stat_get_history(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	QxStatHistorySrfState *state;

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;
		MemoryContext oldcontext;
		char	   *scope;
		TimestampTz since;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		state = palloc0(sizeof(QxStatHistorySrfState));
		if (PG_ARGISNULL(0))
			scope = pstrdup("all");
		else
			scope = text_to_cstring(PG_GETARG_TEXT_PP(0));
		if (PG_ARGISNULL(1))
			since = DT_NOBEGIN;
		else
			since = PG_GETARG_TIMESTAMPTZ(1);

		state->scope = scope;
		state->since = since;
		state->rel = table_open(QxStatHistoryRelationId, AccessShareLock);
		state->scan = table_beginscan_catalog(state->rel, 0, NULL);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		funcctx->user_fctx = state;
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	state = (QxStatHistorySrfState *) funcctx->user_fctx;

	while (true)
	{
		HeapTuple	tup;

		tup = heap_getnext(state->scan, ForwardScanDirection);
		if (tup == NULL)
			break;

		Form_pg_qx_stat_history row = (Form_pg_qx_stat_history) GETSTRUCT(tup);
		TupleDesc	histdesc = RelationGetDescr(state->rel);
		Datum		values[18];
		bool		nulls[18];
		HeapTuple	outtuple;
		char	   *row_scope;
		char	   *row_entity;

		if (row->qxstathistorydbid != MyDatabaseId)
			continue;
		if (!TIMESTAMP_IS_NOBEGIN(state->since) &&
			row->qxstathistorycaptured < state->since)
			continue;

		row_scope = qx_stat_history_text_attr(histdesc, tup,
											Anum_pg_qx_stat_history_qxstathistoryscope);
		if (row_scope == NULL)
			continue;
		if (!qx_stat_history_scope_matches(state->scope, row_scope))
		{
			pfree(row_scope);
			continue;
		}

		row_entity = qx_stat_history_text_attr(histdesc, tup,
											   Anum_pg_qx_stat_history_qxstathistoryentity);

		MemSet(values, 0, sizeof(values));
		MemSet(nulls, false, sizeof(nulls));

		values[0] = ObjectIdGetDatum(row->oid);
		values[1] = TimestampTzGetDatum(row->qxstathistorycaptured);
		values[2] = CStringGetTextDatum(row_scope);
		if (row_entity != NULL)
			values[3] = CStringGetTextDatum(row_entity);
		else
			nulls[3] = true;
		pfree(row_scope);
		if (row_entity != NULL)
			pfree(row_entity);
		values[4] = ObjectIdGetDatum(row->qxstathistoryentityoid);
		values[5] = Int32GetDatum(row->qxstathistorysubmitcount);
		values[6] = Int32GetDatum(row->qxstathistoryresumecount);
		values[7] = Int32GetDatum(row->qxstathistoryverifiedreceipts);
		values[8] = Int32GetDatum(row->qxstathistoryrejectedreceipts);
		values[9] = Int32GetDatum(row->qxstathistorycheckpointcount);
		values[10] = Int32GetDatum(row->qxstathistoryrenewcount);
		values[11] = Int32GetDatum(row->qxstathistoryreclaimcount);
		values[12] = Int32GetDatum(row->qxstathistoryreleasecount);
		values[13] = Int32GetDatum(row->qxstathistoryretrydispatchcount);
		values[14] = Int32GetDatum(row->qxstathistorydeadlettercount);
		values[15] = Int32GetDatum(row->qxstathistorystartupscancount);
		values[16] = Int32GetDatum(row->qxstathistoryfailoverrebuildcount);
		values[17] = Int32GetDatum(row->qxstathistorytasksrequeued);

		outtuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);
		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(outtuple));
	}

	table_endscan(state->scan);
	table_close(state->rel, AccessShareLock);
	pfree(state->scope);
	pfree(state);
	SRF_RETURN_DONE(funcctx);
}
