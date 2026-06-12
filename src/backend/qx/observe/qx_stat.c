/*-------------------------------------------------------------------------
 *
 * qx_stat.c
 *	  shared-memory statistics collector for QhapaqXian Engine
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/table.h"

#include "catalog/pg_qx_principal.h"
#include "catalog/pg_qx_provider.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "qx/qx_catalog.h"
#include "qx/qx_observe.h"
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
}

void
QxStatReportPrincipalExecution(Oid dboid, Oid principaloid, bool resume,
							   bool checkpointed)
{
	QxStatPrincipalMutation mutation;

	if (!qhapaqxian_track_stats || !OidIsValid(principaloid))
		return;

	mutation.resume = resume;
	mutation.checkpointed = checkpointed;
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
}

void
QxStatReportRuntimeClassExecution(Oid dboid, const char *runtime_class,
								  bool resume, bool checkpointed)
{
	QxStatPrincipalMutation mutation;

	if (!qhapaqxian_track_stats)
		return;
	if (runtime_class == NULL || runtime_class[0] == '\0')
		return;

	mutation.resume = resume;
	mutation.checkpointed = checkpointed;
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
										   checkpointed);

		if (principal_runtime != NULL && principal_runtime[0] != '\0')
			QxStatReportRuntimeClassExecution(MyDatabaseId, principal_runtime,
											  resume, checkpointed);
	}
	else if (trace_name != NULL && checkpointed &&
			 principal_runtime != NULL && principal_runtime[0] != '\0')
	{
		QxStatReportRuntimeClassExecution(MyDatabaseId, principal_runtime,
										  false, true);
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

		{
			Relation	rel;
			TableScanDesc scan;
			HeapTuple	tup;

			rel = table_open(QxProviderRelationId, AccessShareLock);
			scan = table_beginscan_catalog(rel, 0, NULL);
			while ((tup = heap_getnext(scan, ForwardScanDirection)) != NULL)
			{
				Form_pg_qx_provider form = (Form_pg_qx_provider) GETSTRUCT(tup);
				QxCatalogProviderInfo *info = palloc(sizeof(QxCatalogProviderInfo));

				if (QxCatalogLookupProviderByOid(form->oid, info))
					state->providers = lappend(state->providers, info);
			}
			table_endscan(scan);
			table_close(rel, AccessShareLock);
		}

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